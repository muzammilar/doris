// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/base_tablet.h"

#include <bthread/mutex.h>
#include <fmt/format.h>
#include <rapidjson/prettywriter.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <random>
#include <shared_mutex>

#include "cloud/cloud_tablet.h"
#include "cloud/config.h"
#include "common/cast_set.h"
#include "common/logging.h"
#include "common/status.h"
#include "olap/calc_delete_bitmap_executor.h"
#include "olap/cumulative_compaction_time_series_policy.h"
#include "olap/delete_bitmap_calculator.h"
#include "olap/iterators.h"
#include "olap/memtable.h"
#include "olap/partial_update_info.h"
#include "olap/primary_key_index.h"
#include "olap/rowid_conversion.h"
#include "olap/rowset/beta_rowset.h"
#include "olap/rowset/rowset.h"
#include "olap/rowset/rowset_fwd.h"
#include "olap/rowset/rowset_reader.h"
#include "olap/tablet_fwd.h"
#include "olap/txn_manager.h"
#include "service/point_query_executor.h"
#include "util/bvar_helper.h"
#include "util/crc32c.h"
#include "util/debug_points.h"
#include "util/doris_metrics.h"
#include "util/key_util.h"
#include "vec/common/assert_cast.h"
#include "vec/common/schema_util.h"
#include "vec/data_types/data_type_factory.hpp"
#include "vec/jsonb/serialize.h"

namespace doris {
#include "common/compile_check_begin.h"

using namespace ErrorCode;

namespace {

bvar::LatencyRecorder g_tablet_commit_phase_update_delete_bitmap_latency(
        "doris_pk", "commit_phase_update_delete_bitmap");
bvar::LatencyRecorder g_tablet_lookup_rowkey_latency("doris_pk", "tablet_lookup_rowkey");
bvar::Adder<uint64_t> g_tablet_pk_not_found("doris_pk", "lookup_not_found");
bvar::PerSecond<bvar::Adder<uint64_t>> g_tablet_pk_not_found_per_second(
        "doris_pk", "lookup_not_found_per_second", &g_tablet_pk_not_found, 60);
bvar::LatencyRecorder g_tablet_update_delete_bitmap_latency("doris_pk", "update_delete_bitmap");

static bvar::Adder<size_t> g_total_tablet_num("doris_total_tablet_num");

Status _get_segment_column_iterator(const BetaRowsetSharedPtr& rowset, uint32_t segid,
                                    const TabletColumn& target_column,
                                    SegmentCacheHandle* segment_cache_handle,
                                    std::unique_ptr<segment_v2::ColumnIterator>* column_iterator,
                                    OlapReaderStatistics* stats) {
    RETURN_IF_ERROR(SegmentLoader::instance()->load_segments(rowset, segment_cache_handle, true));
    // find segment
    auto it = std::find_if(
            segment_cache_handle->get_segments().begin(),
            segment_cache_handle->get_segments().end(),
            [&segid](const segment_v2::SegmentSharedPtr& seg) { return seg->id() == segid; });
    if (it == segment_cache_handle->get_segments().end()) {
        return Status::NotFound(fmt::format("rowset {} 's segemnt not found, seg_id {}",
                                            rowset->rowset_id().to_string(), segid));
    }
    segment_v2::SegmentSharedPtr segment = *it;
    StorageReadOptions opts;
    opts.stats = stats;
    RETURN_IF_ERROR(segment->new_column_iterator(target_column, column_iterator, &opts));
    segment_v2::ColumnIteratorOptions opt {
            .use_page_cache = !config::disable_storage_page_cache,
            .file_reader = segment->file_reader().get(),
            .stats = stats,
            .io_ctx = io::IOContext {.reader_type = ReaderType::READER_QUERY,
                                     .file_cache_stats = &stats->file_cache_stats},
    };
    RETURN_IF_ERROR((*column_iterator)->init(opt));
    return Status::OK();
}

} // namespace

extern MetricPrototype METRIC_query_scan_bytes;
extern MetricPrototype METRIC_query_scan_rows;
extern MetricPrototype METRIC_query_scan_count;
DEFINE_COUNTER_METRIC_PROTOTYPE_2ARG(flush_bytes, MetricUnit::BYTES);
DEFINE_COUNTER_METRIC_PROTOTYPE_2ARG(flush_finish_count, MetricUnit::OPERATIONS);

BaseTablet::BaseTablet(TabletMetaSharedPtr tablet_meta) : _tablet_meta(std::move(tablet_meta)) {
    _metric_entity = DorisMetrics::instance()->metric_registry()->register_entity(
            fmt::format("Tablet.{}", tablet_id()), {{"tablet_id", std::to_string(tablet_id())}},
            MetricEntityType::kTablet);
    INT_COUNTER_METRIC_REGISTER(_metric_entity, query_scan_bytes);
    INT_COUNTER_METRIC_REGISTER(_metric_entity, query_scan_rows);
    INT_COUNTER_METRIC_REGISTER(_metric_entity, query_scan_count);
    INT_COUNTER_METRIC_REGISTER(_metric_entity, flush_bytes);
    INT_COUNTER_METRIC_REGISTER(_metric_entity, flush_finish_count);

    // construct _timestamped_versioned_tracker from rs and stale rs meta
    _timestamped_version_tracker.construct_versioned_tracker(_tablet_meta->all_rs_metas(),
                                                             _tablet_meta->all_stale_rs_metas());

    // if !_tablet_meta->all_rs_metas()[0]->tablet_schema(),
    // that mean the tablet_meta is still no upgrade to doris 1.2 versions.
    // Before doris 1.2 version, rowset metas don't have tablet schema.
    // And when upgrade to doris 1.2 version,
    // all rowset metas will be set the tablet schmea from tablet meta.
    if (_tablet_meta->all_rs_metas().empty() || !_tablet_meta->all_rs_metas()[0]->tablet_schema()) {
        _max_version_schema = _tablet_meta->tablet_schema();
    } else {
        _max_version_schema =
                tablet_schema_with_merged_max_schema_version(_tablet_meta->all_rs_metas());
    }
    DCHECK(_max_version_schema);
    g_total_tablet_num << 1;
}

BaseTablet::~BaseTablet() {
    DorisMetrics::instance()->metric_registry()->deregister_entity(_metric_entity);
    g_total_tablet_num << -1;
}

TabletSchemaSPtr BaseTablet::tablet_schema_with_merged_max_schema_version(
        const std::vector<RowsetMetaSharedPtr>& rowset_metas) {
    RowsetMetaSharedPtr max_schema_version_rs = *std::max_element(
            rowset_metas.begin(), rowset_metas.end(),
            [](const RowsetMetaSharedPtr& a, const RowsetMetaSharedPtr& b) {
                return !a->tablet_schema()
                               ? true
                               : (!b->tablet_schema()
                                          ? false
                                          : a->tablet_schema()->schema_version() <
                                                    b->tablet_schema()->schema_version());
            });
    TabletSchemaSPtr target_schema = max_schema_version_rs->tablet_schema();
    if (target_schema->num_variant_columns() > 0) {
        // For variant columns tablet schema need to be the merged wide tablet schema
        std::vector<TabletSchemaSPtr> schemas;
        std::transform(rowset_metas.begin(), rowset_metas.end(), std::back_inserter(schemas),
                       [](const RowsetMetaSharedPtr& rs_meta) { return rs_meta->tablet_schema(); });
        static_cast<void>(
                vectorized::schema_util::get_least_common_schema(schemas, nullptr, target_schema));
        VLOG_DEBUG << "dump schema: " << target_schema->dump_full_schema();
    }
    return target_schema;
}

Status BaseTablet::set_tablet_state(TabletState state) {
    if (_tablet_meta->tablet_state() == TABLET_SHUTDOWN && state != TABLET_SHUTDOWN) {
        return Status::Error<META_INVALID_ARGUMENT>(
                "could not change tablet state from shutdown to {}", state);
    }
    _tablet_meta->set_tablet_state(state);
    return Status::OK();
}

void BaseTablet::update_max_version_schema(const TabletSchemaSPtr& tablet_schema) {
    std::lock_guard wrlock(_meta_lock);
    // Double Check for concurrent update
    if (!_max_version_schema ||
        tablet_schema->schema_version() > _max_version_schema->schema_version()) {
        _max_version_schema = tablet_schema;
    }
}

Status BaseTablet::update_by_least_common_schema(const TabletSchemaSPtr& update_schema) {
    std::lock_guard wrlock(_meta_lock);
    CHECK(_max_version_schema->schema_version() >= update_schema->schema_version());
    TabletSchemaSPtr final_schema;
    bool check_column_size = true;
    VLOG_DEBUG << "dump _max_version_schema: " << _max_version_schema->dump_full_schema();
    VLOG_DEBUG << "dump update_schema: " << update_schema->dump_full_schema();
    RETURN_IF_ERROR(vectorized::schema_util::get_least_common_schema(
            {_max_version_schema, update_schema}, _max_version_schema, final_schema,
            check_column_size));
    _max_version_schema = final_schema;
    VLOG_DEBUG << "dump updated tablet schema: " << final_schema->dump_full_schema();
    return Status::OK();
}

uint32_t BaseTablet::get_real_compaction_score() const {
    std::shared_lock l(_meta_lock);
    const auto& rs_metas = _tablet_meta->all_rs_metas();
    return std::accumulate(rs_metas.begin(), rs_metas.end(), 0,
                           [](uint32_t score, const RowsetMetaSharedPtr& rs_meta) {
                               return score + rs_meta->get_compaction_score();
                           });
}

Status BaseTablet::capture_rs_readers_unlocked(const Versions& version_path,
                                               std::vector<RowSetSplits>* rs_splits) const {
    DCHECK(rs_splits != nullptr && rs_splits->empty());
    for (auto version : version_path) {
        auto it = _rs_version_map.find(version);
        if (it == _rs_version_map.end()) {
            VLOG_NOTICE << "fail to find Rowset in rs_version for version. tablet=" << tablet_id()
                        << ", version='" << version.first << "-" << version.second;

            it = _stale_rs_version_map.find(version);
            if (it == _stale_rs_version_map.end()) {
                return Status::Error<CAPTURE_ROWSET_READER_ERROR>(
                        "fail to find Rowset in stale_rs_version for version. tablet={}, "
                        "version={}-{}",
                        tablet_id(), version.first, version.second);
            }
        }
        RowsetReaderSharedPtr rs_reader;
        auto res = it->second->create_reader(&rs_reader);
        if (!res.ok()) {
            return Status::Error<CAPTURE_ROWSET_READER_ERROR>(
                    "failed to create reader for rowset:{}", it->second->rowset_id().to_string());
        }
        rs_splits->emplace_back(std::move(rs_reader));
    }
    return Status::OK();
}

// snapshot manager may call this api to check if version exists, so that
// the version maybe not exist
RowsetSharedPtr BaseTablet::get_rowset_by_version(const Version& version,
                                                  bool find_in_stale) const {
    auto iter = _rs_version_map.find(version);
    if (iter == _rs_version_map.end()) {
        if (find_in_stale) {
            return get_stale_rowset_by_version(version);
        }
        return nullptr;
    }
    return iter->second;
}

RowsetSharedPtr BaseTablet::get_stale_rowset_by_version(const Version& version) const {
    auto iter = _stale_rs_version_map.find(version);
    if (iter == _stale_rs_version_map.end()) {
        VLOG_NOTICE << "no rowset for version:" << version << ", tablet: " << tablet_id();
        return nullptr;
    }
    return iter->second;
}

// Already under _meta_lock
RowsetSharedPtr BaseTablet::get_rowset_with_max_version() const {
    Version max_version = _tablet_meta->max_version();
    if (max_version.first == -1) {
        return nullptr;
    }

    auto iter = _rs_version_map.find(max_version);
    if (iter == _rs_version_map.end()) {
        DCHECK(false) << "invalid version:" << max_version;
        return nullptr;
    }
    return iter->second;
}

Status BaseTablet::get_all_rs_id(int64_t max_version, RowsetIdUnorderedSet* rowset_ids) const {
    std::shared_lock rlock(_meta_lock);
    return get_all_rs_id_unlocked(max_version, rowset_ids);
}

Status BaseTablet::get_all_rs_id_unlocked(int64_t max_version,
                                          RowsetIdUnorderedSet* rowset_ids) const {
    //  Ensure that the obtained versions of rowsets are continuous
    Version spec_version(0, max_version);
    Versions version_path;
    auto st = _timestamped_version_tracker.capture_consistent_versions(spec_version, &version_path);
    if (!st.ok()) [[unlikely]] {
        return st;
    }

    for (auto& ver : version_path) {
        if (ver.second == 1) {
            // [0-1] rowset is empty for each tablet, skip it
            continue;
        }
        auto it = _rs_version_map.find(ver);
        if (it == _rs_version_map.end()) {
            return Status::Error<CAPTURE_ROWSET_ERROR, false>(
                    "fail to find Rowset for version. tablet={}, version={}", tablet_id(),
                    ver.to_string());
        }
        rowset_ids->emplace(it->second->rowset_id());
    }
    return Status::OK();
}

Versions BaseTablet::get_missed_versions(int64_t spec_version) const {
    DCHECK(spec_version > 0) << "invalid spec_version: " << spec_version;

    Versions existing_versions;
    {
        std::shared_lock rdlock(_meta_lock);
        for (const auto& rs : _tablet_meta->all_rs_metas()) {
            existing_versions.emplace_back(rs->version());
        }
    }
    return calc_missed_versions(spec_version, std::move(existing_versions));
}

Versions BaseTablet::get_missed_versions_unlocked(int64_t spec_version) const {
    DCHECK(spec_version > 0) << "invalid spec_version: " << spec_version;

    Versions existing_versions;
    for (const auto& rs : _tablet_meta->all_rs_metas()) {
        existing_versions.emplace_back(rs->version());
    }
    return calc_missed_versions(spec_version, std::move(existing_versions));
}

void BaseTablet::_print_missed_versions(const Versions& missed_versions) const {
    std::stringstream ss;
    ss << tablet_id() << " has " << missed_versions.size() << " missed version:";
    // print at most 10 version
    for (int i = 0; i < 10 && i < missed_versions.size(); ++i) {
        ss << missed_versions[i] << ",";
    }
    LOG(WARNING) << ss.str();
}

bool BaseTablet::_reconstruct_version_tracker_if_necessary() {
    double orphan_vertex_ratio = _timestamped_version_tracker.get_orphan_vertex_ratio();
    if (orphan_vertex_ratio >= config::tablet_version_graph_orphan_vertex_ratio) {
        _timestamped_version_tracker.construct_versioned_tracker(
                _tablet_meta->all_rs_metas(), _tablet_meta->all_stale_rs_metas());
        return true;
    }
    return false;
}

// should use this method to get a copy of current tablet meta
// there are some rowset meta in local meta store and in in-memory tablet meta
// but not in tablet meta in local meta store
void BaseTablet::generate_tablet_meta_copy(TabletMeta& new_tablet_meta) const {
    TabletMetaPB tablet_meta_pb;
    {
        std::shared_lock rdlock(_meta_lock);
        _tablet_meta->to_meta_pb(&tablet_meta_pb);
    }
    generate_tablet_meta_copy_unlocked(new_tablet_meta);
}

// this is a unlocked version of generate_tablet_meta_copy()
// some method already hold the _meta_lock before calling this,
// such as EngineCloneTask::_finish_clone -> tablet->revise_tablet_meta
void BaseTablet::generate_tablet_meta_copy_unlocked(TabletMeta& new_tablet_meta) const {
    TabletMetaPB tablet_meta_pb;
    _tablet_meta->to_meta_pb(&tablet_meta_pb);
    new_tablet_meta.init_from_pb(tablet_meta_pb);
}

Status BaseTablet::calc_delete_bitmap_between_segments(
        RowsetId rowset_id, const std::vector<segment_v2::SegmentSharedPtr>& segments,
        DeleteBitmapPtr delete_bitmap) {
    size_t const num_segments = segments.size();
    if (num_segments < 2) {
        return Status::OK();
    }

    OlapStopWatch watch;
    size_t seq_col_length = 0;
    if (_tablet_meta->tablet_schema()->has_sequence_col()) {
        auto seq_col_idx = _tablet_meta->tablet_schema()->sequence_col_idx();
        seq_col_length = _tablet_meta->tablet_schema()->column(seq_col_idx).length() + 1;
    }
    size_t rowid_length = 0;
    if (!_tablet_meta->tablet_schema()->cluster_key_uids().empty()) {
        rowid_length = PrimaryKeyIndexReader::ROW_ID_LENGTH;
    }

    MergeIndexDeleteBitmapCalculator calculator;
    RETURN_IF_ERROR(calculator.init(rowset_id, segments, seq_col_length, rowid_length));

    RETURN_IF_ERROR(calculator.calculate_all(delete_bitmap));

    delete_bitmap->add(
            {rowset_id, DeleteBitmap::INVALID_SEGMENT_ID, DeleteBitmap::TEMP_VERSION_COMMON},
            DeleteBitmap::ROWSET_SENTINEL_MARK);
    LOG(INFO) << fmt::format(
            "construct delete bitmap between segments, "
            "tablet: {}, rowset: {}, number of segments: {}, bitmap count: {}, bitmap cardinality: "
            "{}, cost {} (us)",
            tablet_id(), rowset_id.to_string(), num_segments,
            delete_bitmap->get_delete_bitmap_count(), delete_bitmap->cardinality(),
            watch.get_elapse_time_us());
    return Status::OK();
}

std::vector<RowsetSharedPtr> BaseTablet::get_rowset_by_ids(
        const RowsetIdUnorderedSet* specified_rowset_ids) {
    std::vector<RowsetSharedPtr> rowsets;
    for (auto& rs : _rs_version_map) {
        if (!specified_rowset_ids ||
            specified_rowset_ids->find(rs.second->rowset_id()) != specified_rowset_ids->end()) {
            rowsets.push_back(rs.second);
        }
    }

    std::sort(rowsets.begin(), rowsets.end(), [](RowsetSharedPtr& lhs, RowsetSharedPtr& rhs) {
        return lhs->end_version() > rhs->end_version();
    });
    return rowsets;
}

Status BaseTablet::lookup_row_data(const Slice& encoded_key, const RowLocation& row_location,
                                   RowsetSharedPtr input_rowset, OlapReaderStatistics& stats,
                                   std::string& values, bool write_to_cache) {
    MonotonicStopWatch watch;
    size_t row_size = 1;
    watch.start();
    Defer _defer([&]() {
        LOG_EVERY_N(INFO, 500) << "get a single_row, cost(us):" << watch.elapsed_time() / 1000
                               << ", row_size:" << row_size;
    });

    BetaRowsetSharedPtr rowset = std::static_pointer_cast<BetaRowset>(input_rowset);
    CHECK(rowset);
    const TabletSchemaSPtr tablet_schema = rowset->tablet_schema();
    SegmentCacheHandle segment_cache_handle;
    std::unique_ptr<segment_v2::ColumnIterator> column_iterator;
    const auto& column = *DORIS_TRY(tablet_schema->column(BeConsts::ROW_STORE_COL));
    RETURN_IF_ERROR(_get_segment_column_iterator(rowset, row_location.segment_id, column,
                                                 &segment_cache_handle, &column_iterator, &stats));
    // get and parse tuple row
    vectorized::MutableColumnPtr column_ptr = vectorized::ColumnString::create();
    std::vector<segment_v2::rowid_t> rowids {static_cast<segment_v2::rowid_t>(row_location.row_id)};
    RETURN_IF_ERROR(column_iterator->read_by_rowids(rowids.data(), 1, column_ptr));
    assert(column_ptr->size() == 1);
    auto* string_column = static_cast<vectorized::ColumnString*>(column_ptr.get());
    StringRef value = string_column->get_data_at(0);
    values = value.to_string();
    if (write_to_cache) {
        RowCache::instance()->insert({tablet_id(), encoded_key}, Slice {value.data, value.size});
    }
    return Status::OK();
}

Status BaseTablet::lookup_row_key(const Slice& encoded_key, TabletSchema* latest_schema,
                                  bool with_seq_col,
                                  const std::vector<RowsetSharedPtr>& specified_rowsets,
                                  RowLocation* row_location, int64_t version,
                                  std::vector<std::unique_ptr<SegmentCacheHandle>>& segment_caches,
                                  RowsetSharedPtr* rowset, bool with_rowid,
                                  std::string* encoded_seq_value, OlapReaderStatistics* stats,
                                  DeleteBitmapPtr delete_bitmap) {
    SCOPED_BVAR_LATENCY(g_tablet_lookup_rowkey_latency);
    size_t seq_col_length = 0;
    // use the latest tablet schema to decide if the tablet has sequence column currently
    const TabletSchema* schema =
            (latest_schema == nullptr ? _tablet_meta->tablet_schema().get() : latest_schema);
    if (schema->has_sequence_col() && with_seq_col) {
        seq_col_length = schema->column(schema->sequence_col_idx()).length() + 1;
    }
    size_t rowid_length = 0;
    if (with_rowid && !schema->cluster_key_uids().empty()) {
        rowid_length = PrimaryKeyIndexReader::ROW_ID_LENGTH;
    }
    Slice key_without_seq =
            Slice(encoded_key.get_data(), encoded_key.get_size() - seq_col_length - rowid_length);
    RowLocation loc;

    auto tablet_delete_bitmap =
            delete_bitmap == nullptr ? _tablet_meta->delete_bitmap_ptr() : delete_bitmap;
    for (size_t i = 0; i < specified_rowsets.size(); i++) {
        const auto& rs = specified_rowsets[i];
        std::vector<KeyBoundsPB> segments_key_bounds;
        rs->rowset_meta()->get_segments_key_bounds(&segments_key_bounds);
        int num_segments = cast_set<int>(rs->num_segments());
        DCHECK_EQ(segments_key_bounds.size(), num_segments);
        std::vector<uint32_t> picked_segments;
        for (int j = num_segments - 1; j >= 0; j--) {
            if (key_is_not_in_segment(key_without_seq, segments_key_bounds[j],
                                      rs->rowset_meta()->is_segments_key_bounds_truncated())) {
                continue;
            }
            picked_segments.emplace_back(j);
        }
        if (picked_segments.empty()) {
            continue;
        }

        if (UNLIKELY(segment_caches[i] == nullptr)) {
            segment_caches[i] = std::make_unique<SegmentCacheHandle>();
            RETURN_IF_ERROR(SegmentLoader::instance()->load_segments(
                    std::static_pointer_cast<BetaRowset>(rs), segment_caches[i].get(), true, true));
        }
        auto& segments = segment_caches[i]->get_segments();
        DCHECK_EQ(segments.size(), num_segments);

        for (auto id : picked_segments) {
            Status s = segments[id]->lookup_row_key(encoded_key, schema, with_seq_col, with_rowid,
                                                    &loc, stats, encoded_seq_value);
            if (s.is<KEY_NOT_FOUND>()) {
                continue;
            }
            if (!s.ok() && !s.is<KEY_ALREADY_EXISTS>()) {
                return s;
            }
            if (s.ok() && tablet_delete_bitmap->contains_agg_without_cache(
                                  {loc.rowset_id, loc.segment_id, version}, loc.row_id)) {
                // if has sequence col, we continue to compare the sequence_id of
                // all rowsets, util we find an existing key.
                if (schema->has_sequence_col()) {
                    continue;
                }
                // The key is deleted, we don't need to search for it any more.
                break;
            }
            // `st` is either OK or KEY_ALREADY_EXISTS now.
            // for partial update, even if the key is already exists, we still need to
            // read it's original values to keep all columns align.
            *row_location = loc;
            if (rowset) {
                // return it's rowset
                *rowset = rs;
            }
            // find it and return
            return s;
        }
    }
    g_tablet_pk_not_found << 1;
    return Status::Error<ErrorCode::KEY_NOT_FOUND>("can't find key in all rowsets");
}

// if user pass a token, then all calculation works will submit to a threadpool,
// user can get all delete bitmaps from that token.
// if `token` is nullptr, the calculation will run in local, and user can get the result
// delete bitmap from `delete_bitmap` directly.
Status BaseTablet::calc_delete_bitmap(
        const BaseTabletSPtr& tablet, RowsetSharedPtr rowset,
        const std::vector<segment_v2::SegmentSharedPtr>& segments,
        const std::vector<RowsetSharedPtr>& specified_rowsets, DeleteBitmapPtr delete_bitmap,
        int64_t end_version, CalcDeleteBitmapToken* token, RowsetWriter* rowset_writer,
        DeleteBitmapPtr tablet_delete_bitmap,
        std::function<void(segment_v2::SegmentSharedPtr, Status)> callback) {
    if (specified_rowsets.empty() || segments.empty()) {
        return Status::OK();
    }

    OlapStopWatch watch;
    for (const auto& segment : segments) {
        const auto& seg = segment;
        if (token != nullptr) {
            RETURN_IF_ERROR(token->submit(tablet, rowset, seg, specified_rowsets, end_version,
                                          delete_bitmap, rowset_writer, tablet_delete_bitmap,
                                          callback));
        } else {
            RETURN_IF_ERROR(tablet->calc_segment_delete_bitmap(
                    rowset, segment, specified_rowsets, delete_bitmap, end_version, rowset_writer,
                    tablet_delete_bitmap));
        }
    }

    return Status::OK();
}

Status BaseTablet::calc_segment_delete_bitmap(RowsetSharedPtr rowset,
                                              const segment_v2::SegmentSharedPtr& seg,
                                              const std::vector<RowsetSharedPtr>& specified_rowsets,
                                              DeleteBitmapPtr delete_bitmap, int64_t end_version,
                                              RowsetWriter* rowset_writer,
                                              DeleteBitmapPtr tablet_delete_bitmap) {
    OlapStopWatch watch;
    auto rowset_id = rowset->rowset_id();
    Version dummy_version(end_version + 1, end_version + 1);
    auto rowset_schema = rowset->tablet_schema();

    PartialUpdateInfo* partial_update_info {nullptr};
    bool is_partial_update = rowset_writer && rowset_writer->is_partial_update();
    // `have_input_seq_column` is for fixed partial update only. For flexible partial update, we should use
    // the skip bitmap to determine wheather a row has specified the sequence column
    bool have_input_seq_column = false;
    // `rids_be_overwritten` is for flexible partial update only, it records row ids that is overwritten by
    // another row with higher seqeucne value
    std::set<uint32_t> rids_be_overwritten;
    if (is_partial_update) {
        partial_update_info = rowset_writer->get_partial_update_info().get();
        if (partial_update_info->is_fixed_partial_update() && rowset_schema->has_sequence_col()) {
            std::vector<uint32_t> including_cids =
                    rowset_writer->get_partial_update_info()->update_cids;
            have_input_seq_column =
                    rowset_schema->has_sequence_col() &&
                    (std::find(including_cids.cbegin(), including_cids.cend(),
                               rowset_schema->sequence_col_idx()) != including_cids.cend());
        }
    }

    DBUG_EXECUTE_IF("BaseTablet::calc_segment_delete_bitmap.sleep", {
        auto target_tablet_id = dp->param<int64_t>("tablet_id", -1);
        auto sleep = dp->param<int64_t>("sleep", 10);
        if (target_tablet_id == tablet_id()) {
            std::this_thread::sleep_for(std::chrono::seconds(sleep));
        }
    });

    if (rowset_schema->num_variant_columns() > 0) {
        // During partial updates, the extracted columns of a variant should not be included in the rowset schema.
        // This is because the partial update for a variant needs to ignore the extracted columns.
        // Otherwise, the schema types in different rowsets might be inconsistent. When performing a partial update,
        // the complete variant is constructed by reading all the sub-columns of the variant.
        rowset_schema = rowset_schema->copy_without_variant_extracted_columns();
    }
    // use for partial update
    FixedReadPlan read_plan_ori;
    FixedReadPlan read_plan_update;
    int64_t conflict_rows = 0;
    int64_t new_generated_rows = 0;

    std::map<RowsetId, RowsetSharedPtr> rsid_to_rowset;
    rsid_to_rowset[rowset_id] = rowset;
    vectorized::Block block = rowset_schema->create_block();
    vectorized::Block ordered_block = block.clone_empty();
    uint32_t pos = 0;

    RETURN_IF_ERROR(seg->load_pk_index_and_bf(nullptr)); // We need index blocks to iterate
    const auto* pk_idx = seg->get_primary_key_index();
    int64_t total = pk_idx->num_rows();
    uint32_t row_id = 0;
    int64_t remaining = total;
    bool exact_match = false;
    std::string last_key;
    int batch_size = 1024;
    // The data for each segment may be lookup multiple times. Creating a SegmentCacheHandle
    // will update the lru cache, and there will be obvious lock competition in multithreading
    // scenarios, so using a segment_caches to cache SegmentCacheHandle.
    std::vector<std::unique_ptr<SegmentCacheHandle>> segment_caches(specified_rowsets.size());
    while (remaining > 0) {
        std::unique_ptr<segment_v2::IndexedColumnIterator> iter;
        RETURN_IF_ERROR(pk_idx->new_iterator(&iter, nullptr));

        size_t num_to_read = std::min<int64_t>(batch_size, remaining);
        auto index_type = vectorized::DataTypeFactory::instance().create_data_type(
                pk_idx->type_info()->type(), 1, 0);
        auto index_column = index_type->create_column();
        Slice last_key_slice(last_key);
        RETURN_IF_ERROR(iter->seek_at_or_after(&last_key_slice, &exact_match));
        auto current_ordinal = iter->get_current_ordinal();
        DCHECK(total == remaining + current_ordinal)
                << "total: " << total << ", remaining: " << remaining
                << ", current_ordinal: " << current_ordinal;

        size_t num_read = num_to_read;
        RETURN_IF_ERROR(iter->next_batch(&num_read, index_column));
        DCHECK(num_to_read == num_read)
                << "num_to_read: " << num_to_read << ", num_read: " << num_read;
        last_key = index_column->get_data_at(num_read - 1).to_string();

        // exclude last_key, last_key will be read in next batch.
        if (num_read == batch_size && num_read != remaining) {
            num_read -= 1;
        }
        for (size_t i = 0; i < num_read; i++, row_id++) {
            Slice key = Slice(index_column->get_data_at(i).data, index_column->get_data_at(i).size);
            RowLocation loc;
            // calculate row id
            if (!_tablet_meta->tablet_schema()->cluster_key_uids().empty()) {
                size_t seq_col_length = 0;
                if (_tablet_meta->tablet_schema()->has_sequence_col()) {
                    seq_col_length =
                            _tablet_meta->tablet_schema()
                                    ->column(_tablet_meta->tablet_schema()->sequence_col_idx())
                                    .length() +
                            1;
                }
                size_t rowid_length = PrimaryKeyIndexReader::ROW_ID_LENGTH;
                Slice key_without_seq =
                        Slice(key.get_data(), key.get_size() - seq_col_length - rowid_length);
                Slice rowid_slice =
                        Slice(key.get_data() + key_without_seq.get_size() + seq_col_length + 1,
                              rowid_length - 1);
                const auto* type_info =
                        get_scalar_type_info<FieldType::OLAP_FIELD_TYPE_UNSIGNED_INT>();
                const auto* rowid_coder = get_key_coder(type_info->type());
                RETURN_IF_ERROR(rowid_coder->decode_ascending(&rowid_slice, rowid_length,
                                                              (uint8_t*)&row_id));
            }
            // same row in segments should be filtered
            if (delete_bitmap->contains({rowset_id, seg->id(), DeleteBitmap::TEMP_VERSION_COMMON},
                                        row_id)) {
                continue;
            }

            DBUG_EXECUTE_IF("BaseTablet::calc_segment_delete_bitmap.inject_err", {
                auto p = dp->param("percent", 0.01);
                std::mt19937 gen {std::random_device {}()};
                std::bernoulli_distribution inject_fault {p};
                if (inject_fault(gen)) {
                    return Status::InternalError(
                            "injection error in calc_segment_delete_bitmap, "
                            "tablet_id={}, rowset_id={}",
                            tablet_id(), rowset_id.to_string());
                }
            });

            RowsetSharedPtr rowset_find;
            Status st = Status::OK();
            if (tablet_delete_bitmap == nullptr) {
                st = lookup_row_key(key, rowset_schema.get(), true, specified_rowsets, &loc,
                                    dummy_version.first - 1, segment_caches, &rowset_find);
            } else {
                st = lookup_row_key(key, rowset_schema.get(), true, specified_rowsets, &loc,
                                    dummy_version.first - 1, segment_caches, &rowset_find, true,
                                    nullptr, nullptr, tablet_delete_bitmap);
            }
            bool expected_st = st.ok() || st.is<KEY_NOT_FOUND>() || st.is<KEY_ALREADY_EXISTS>();
            // It's a defensive DCHECK, we need to exclude some common errors to avoid core-dump
            // while stress test
            DCHECK(expected_st || st.is<MEM_LIMIT_EXCEEDED>())
                    << "unexpected error status while lookup_row_key:" << st;
            if (!expected_st) {
                return st;
            }
            if (st.is<KEY_NOT_FOUND>()) {
                continue;
            }

            ++conflict_rows;
            if (st.is<KEY_ALREADY_EXISTS>() &&
                (!is_partial_update ||
                 (partial_update_info->is_fixed_partial_update() && have_input_seq_column))) {
                // `st.is<KEY_ALREADY_EXISTS>()` means that there exists a row with the same key and larger value
                // in seqeunce column.
                // - If the current load is not a partial update, we just delete current row.
                // - Otherwise, it means that we are doing the alignment process in publish phase due to conflicts
                // during concurrent partial updates. And there exists another load which introduces a row with
                // the same keys and larger sequence column value published successfully after the commit phase
                // of the current load.
                //     - If the columns we update include sequence column, we should delete the current row becase the
                //       partial update on the current row has been `overwritten` by the previous one with larger sequence
                //       column value.
                //     - Otherwise, we should combine the values of the missing columns in the previous row and the values
                //       of the including columns in the current row into a new row.
                delete_bitmap->add({rowset_id, seg->id(), DeleteBitmap::TEMP_VERSION_COMMON},
                                   row_id);
                continue;
                // NOTE: for partial update which doesn't specify the sequence column, we can't use the sequence column value filled in flush phase
                // as its final value. Otherwise it may cause inconsistency between replicas.
            }
            if (is_partial_update && rowset_writer != nullptr) {
                // In publish version, record rows to be deleted for concurrent update
                // For example, if version 5 and 6 update a row, but version 6 only see
                // version 4 when write, and when publish version, version 5's value will
                // be marked as deleted and it's update is losed.
                // So here we should read version 5's columns and build a new row, which is
                // consists of version 6's update columns and version 5's origin columns
                // here we build 2 read plan for ori values and update values

                // - for fixed partial update, we should read update columns from current load's rowset
                // and read missing columns from previous rowsets to create the final block
                // - for flexible partial update, we should read all columns from current load's rowset
                // and read non sort key columns from previous rowsets to create the final block
                // So we only need to record rows to read for both mode partial update
                read_plan_ori.prepare_to_read(loc, pos);
                read_plan_update.prepare_to_read(RowLocation {rowset_id, seg->id(), row_id}, pos);

                // For flexible partial update, we should use skip bitmap to determine wheather
                // a row has specified the sequence column. But skip bitmap should be read from the segment.
                // So we record these row ids and process and filter them in `generate_new_block_for_flexible_partial_update()`
                if (st.is<KEY_ALREADY_EXISTS>() &&
                    partial_update_info->is_flexible_partial_update()) {
                    rids_be_overwritten.insert(pos);
                }

                rsid_to_rowset[rowset_find->rowset_id()] = rowset_find;
                ++pos;

                // delete bitmap will be calculate when memtable flush and
                // publish. The two stages may see different versions.
                // When there is sequence column, the currently imported data
                // of rowset may be marked for deletion at memtablet flush or
                // publish because the seq column is smaller than the previous
                // rowset.
                // just set 0 as a unified temporary version number, and update to
                // the real version number later.
                delete_bitmap->add(
                        {loc.rowset_id, loc.segment_id, DeleteBitmap::TEMP_VERSION_COMMON},
                        loc.row_id);
                delete_bitmap->add({rowset_id, seg->id(), DeleteBitmap::TEMP_VERSION_COMMON},
                                   row_id);
                ++new_generated_rows;
                continue;
            }
            // when st = ok
            delete_bitmap->add({loc.rowset_id, loc.segment_id, DeleteBitmap::TEMP_VERSION_COMMON},
                               loc.row_id);
        }
        remaining -= num_read;
    }
    // DCHECK_EQ(total, row_id) << "segment total rows: " << total << " row_id:" << row_id;

    if (config::enable_merge_on_write_correctness_check) {
        RowsetIdUnorderedSet rowsetids;
        for (const auto& specified_rowset : specified_rowsets) {
            rowsetids.emplace(specified_rowset->rowset_id());
            VLOG_NOTICE << "[tabletID:" << tablet_id() << "]"
                        << "[add_sentinel_mark_to_delete_bitmap][end_version:" << end_version << "]"
                        << "add:" << specified_rowset->rowset_id();
        }
        add_sentinel_mark_to_delete_bitmap(delete_bitmap.get(), rowsetids);
    }

    if (pos > 0) {
        DCHECK(partial_update_info);
        if (partial_update_info->is_fixed_partial_update()) {
            RETURN_IF_ERROR(generate_new_block_for_partial_update(
                    rowset_schema, partial_update_info, read_plan_ori, read_plan_update,
                    rsid_to_rowset, &block));
        } else {
            RETURN_IF_ERROR(generate_new_block_for_flexible_partial_update(
                    rowset_schema, partial_update_info, rids_be_overwritten, read_plan_ori,
                    read_plan_update, rsid_to_rowset, &block));
        }
        RETURN_IF_ERROR(sort_block(block, ordered_block));
        RETURN_IF_ERROR(rowset_writer->flush_single_block(&ordered_block));
        auto cost_us = watch.get_elapse_time_us();
        if (config::enable_mow_verbose_log || cost_us > 10 * 1000) {
            LOG(INFO) << "calc segment delete bitmap for "
                      << partial_update_info->partial_update_mode_str()
                      << ", tablet: " << tablet_id() << " rowset: " << rowset_id
                      << " seg_id: " << seg->id() << " dummy_version: " << end_version + 1
                      << " rows: " << seg->num_rows() << " conflict rows: " << conflict_rows
                      << " new generated rows: " << new_generated_rows
                      << " bitmap num: " << delete_bitmap->get_delete_bitmap_count()
                      << " bitmap cardinality: " << delete_bitmap->cardinality()
                      << " cost: " << cost_us << "(us)";
        }
        return Status::OK();
    }
    auto cost_us = watch.get_elapse_time_us();
    if (config::enable_mow_verbose_log || cost_us > 10 * 1000) {
        LOG(INFO) << "calc segment delete bitmap, tablet: " << tablet_id()
                  << " rowset: " << rowset_id << " seg_id: " << seg->id()
                  << " dummy_version: " << end_version + 1 << " rows: " << seg->num_rows()
                  << " conflict rows: " << conflict_rows
                  << " bitmap num: " << delete_bitmap->get_delete_bitmap_count()
                  << " bitmap cardinality: " << delete_bitmap->cardinality() << " cost: " << cost_us
                  << "(us)";
    }
    return Status::OK();
}

Status BaseTablet::sort_block(vectorized::Block& in_block, vectorized::Block& output_block) {
    vectorized::MutableBlock mutable_input_block =
            vectorized::MutableBlock::build_mutable_block(&in_block);
    vectorized::MutableBlock mutable_output_block =
            vectorized::MutableBlock::build_mutable_block(&output_block);

    std::shared_ptr<RowInBlockComparator> vec_row_comparator =
            std::make_shared<RowInBlockComparator>(_tablet_meta->tablet_schema());
    vec_row_comparator->set_block(&mutable_input_block);

    std::vector<std::unique_ptr<RowInBlock>> row_in_blocks;
    DCHECK(in_block.rows() <= std::numeric_limits<int>::max());
    row_in_blocks.reserve(in_block.rows());
    for (size_t i = 0; i < in_block.rows(); ++i) {
        row_in_blocks.emplace_back(std::make_unique<RowInBlock>(i));
    }
    std::sort(row_in_blocks.begin(), row_in_blocks.end(),
              [&](const std::unique_ptr<RowInBlock>& l,
                  const std::unique_ptr<RowInBlock>& r) -> bool {
                  auto value = (*vec_row_comparator)(l.get(), r.get());
                  DCHECK(value != 0) << "value equel when sort block, l_pos: " << l->_row_pos
                                     << " r_pos: " << r->_row_pos;
                  return value < 0;
              });
    std::vector<uint32_t> row_pos_vec;
    row_pos_vec.reserve(in_block.rows());
    for (auto& block : row_in_blocks) {
        row_pos_vec.emplace_back(block->_row_pos);
    }
    return mutable_output_block.add_rows(&in_block, row_pos_vec.data(),
                                         row_pos_vec.data() + in_block.rows());
}

// fetch value by row column
Status BaseTablet::fetch_value_through_row_column(RowsetSharedPtr input_rowset,
                                                  const TabletSchema& tablet_schema, uint32_t segid,
                                                  const std::vector<uint32_t>& rowids,
                                                  const std::vector<uint32_t>& cids,
                                                  vectorized::Block& block) {
    MonotonicStopWatch watch;
    watch.start();
    Defer _defer([&]() {
        LOG_EVERY_N(INFO, 500) << "fetch_value_by_rowids, cost(us):" << watch.elapsed_time() / 1000
                               << ", row_batch_size:" << rowids.size();
    });

    BetaRowsetSharedPtr rowset = std::static_pointer_cast<BetaRowset>(input_rowset);
    CHECK(rowset);
    CHECK(tablet_schema.has_row_store_for_all_columns());
    SegmentCacheHandle segment_cache_handle;
    std::unique_ptr<segment_v2::ColumnIterator> column_iterator;
    OlapReaderStatistics stats;
    const auto& column = *DORIS_TRY(tablet_schema.column(BeConsts::ROW_STORE_COL));
    RETURN_IF_ERROR(_get_segment_column_iterator(rowset, segid, column, &segment_cache_handle,
                                                 &column_iterator, &stats));
    // get and parse tuple row
    vectorized::MutableColumnPtr column_ptr = vectorized::ColumnString::create();
    RETURN_IF_ERROR(column_iterator->read_by_rowids(rowids.data(), rowids.size(), column_ptr));
    assert(column_ptr->size() == rowids.size());
    auto* string_column = static_cast<vectorized::ColumnString*>(column_ptr.get());
    vectorized::DataTypeSerDeSPtrs serdes;
    serdes.resize(cids.size());
    std::unordered_map<uint32_t, uint32_t> col_uid_to_idx;
    std::vector<std::string> default_values;
    default_values.resize(cids.size());
    for (int i = 0; i < cids.size(); ++i) {
        const TabletColumn& tablet_column = tablet_schema.column(cids[i]);
        vectorized::DataTypePtr type =
                vectorized::DataTypeFactory::instance().create_data_type(tablet_column);
        col_uid_to_idx[tablet_column.unique_id()] = i;
        default_values[i] = tablet_column.default_value();
        serdes[i] = type->get_serde();
    }
    RETURN_IF_ERROR(vectorized::JsonbSerializeUtil::jsonb_to_block(
            serdes, *string_column, col_uid_to_idx, block, default_values, {}));
    return Status::OK();
}

Status BaseTablet::fetch_value_by_rowids(RowsetSharedPtr input_rowset, uint32_t segid,
                                         const std::vector<uint32_t>& rowids,
                                         const TabletColumn& tablet_column,
                                         vectorized::MutableColumnPtr& dst) {
    MonotonicStopWatch watch;
    watch.start();
    Defer _defer([&]() {
        LOG_EVERY_N(INFO, 500) << "fetch_value_by_rowids, cost(us):" << watch.elapsed_time() / 1000
                               << ", row_batch_size:" << rowids.size();
    });

    // read row data
    BetaRowsetSharedPtr rowset = std::static_pointer_cast<BetaRowset>(input_rowset);
    CHECK(rowset);
    SegmentCacheHandle segment_cache_handle;
    std::unique_ptr<segment_v2::ColumnIterator> column_iterator;
    OlapReaderStatistics stats;
    RETURN_IF_ERROR(_get_segment_column_iterator(rowset, segid, tablet_column,
                                                 &segment_cache_handle, &column_iterator, &stats));
    RETURN_IF_ERROR(column_iterator->read_by_rowids(rowids.data(), rowids.size(), dst));
    return Status::OK();
}

const signed char* BaseTablet::get_delete_sign_column_data(const vectorized::Block& block,
                                                           size_t rows_at_least) {
    if (const vectorized::ColumnWithTypeAndName* delete_sign_column =
                block.try_get_by_name(DELETE_SIGN);
        delete_sign_column != nullptr) {
        const auto& delete_sign_col =
                reinterpret_cast<const vectorized::ColumnInt8&>(*(delete_sign_column->column));
        if (delete_sign_col.size() >= rows_at_least) {
            return delete_sign_col.get_data().data();
        }
    }
    return nullptr;
};

Status BaseTablet::generate_default_value_block(const TabletSchema& schema,
                                                const std::vector<uint32_t>& cids,
                                                const std::vector<std::string>& default_values,
                                                const vectorized::Block& ref_block,
                                                vectorized::Block& default_value_block) {
    auto mutable_default_value_columns = default_value_block.mutate_columns();
    for (auto i = 0; i < cids.size(); ++i) {
        const auto& column = schema.column(cids[i]);
        if (column.has_default_value()) {
            const auto& default_value = default_values[i];
            vectorized::ReadBuffer rb(const_cast<char*>(default_value.c_str()),
                                      default_value.size());
            RETURN_IF_ERROR(ref_block.get_by_position(i).type->from_string(
                    rb, mutable_default_value_columns[i].get()));
        }
    }
    default_value_block.set_columns(std::move(mutable_default_value_columns));
    return Status::OK();
}

Status BaseTablet::generate_new_block_for_partial_update(
        TabletSchemaSPtr rowset_schema, const PartialUpdateInfo* partial_update_info,
        const FixedReadPlan& read_plan_ori, const FixedReadPlan& read_plan_update,
        const std::map<RowsetId, RowsetSharedPtr>& rsid_to_rowset,
        vectorized::Block* output_block) {
    // do partial update related works
    // 1. read columns by read plan
    // 2. generate new block
    // 3. write a new segment and modify rowset meta
    // 4. mark current keys deleted
    CHECK(output_block);
    auto full_mutable_columns = output_block->mutate_columns();
    const auto& missing_cids = partial_update_info->missing_cids;
    const auto& update_cids = partial_update_info->update_cids;
    auto old_block = rowset_schema->create_block_by_cids(missing_cids);
    auto update_block = rowset_schema->create_block_by_cids(update_cids);

    bool have_input_seq_column = false;
    if (rowset_schema->has_sequence_col()) {
        have_input_seq_column =
                (std::find(update_cids.cbegin(), update_cids.cend(),
                           rowset_schema->sequence_col_idx()) != update_cids.cend());
    }

    // rowid in the final block(start from 0, increase continuously) -> rowid to read in update_block
    std::map<uint32_t, uint32_t> read_index_update;

    // read current rowset first, if a row in the current rowset has delete sign mark
    // we don't need to read values from old block
    RETURN_IF_ERROR(read_plan_update.read_columns_by_plan(
            *rowset_schema, update_cids, rsid_to_rowset, update_block, &read_index_update, false));
    size_t update_rows = read_index_update.size();
    for (auto i = 0; i < update_cids.size(); ++i) {
        for (auto idx = 0; idx < update_rows; ++idx) {
            full_mutable_columns[update_cids[i]]->insert_from(
                    *update_block.get_by_position(i).column, read_index_update[idx]);
        }
    }

    // if there is sequence column in the table, we need to read the sequence column,
    // otherwise it may cause the merge-on-read based compaction policy to produce incorrect results
    const auto* __restrict new_block_delete_signs =
            rowset_schema->has_sequence_col()
                    ? nullptr
                    : get_delete_sign_column_data(update_block, update_rows);

    // rowid in the final block(start from 0, increase, may not continuous becasue we skip to read some rows) -> rowid to read in old_block
    std::map<uint32_t, uint32_t> read_index_old;
    RETURN_IF_ERROR(read_plan_ori.read_columns_by_plan(*rowset_schema, missing_cids, rsid_to_rowset,
                                                       old_block, &read_index_old, true,
                                                       new_block_delete_signs));
    size_t old_rows = read_index_old.size();
    const auto* __restrict old_block_delete_signs =
            get_delete_sign_column_data(old_block, old_rows);
    DCHECK(old_block_delete_signs != nullptr);
    // build default value block
    auto default_value_block = old_block.clone_empty();
    RETURN_IF_ERROR(BaseTablet::generate_default_value_block(*rowset_schema, missing_cids,
                                                             partial_update_info->default_values,
                                                             old_block, default_value_block));

    CHECK(update_rows >= old_rows);

    // build full block
    for (auto i = 0; i < missing_cids.size(); ++i) {
        const auto& rs_column = rowset_schema->column(missing_cids[i]);
        auto& mutable_column = full_mutable_columns[missing_cids[i]];
        for (auto idx = 0; idx < update_rows; ++idx) {
            // There are two cases we don't need to read values from old data:
            //     1. if the conflicting new row's delete sign is marked, which means the value columns
            //     of the row will not be read. So we don't need to read the missing values from the previous rows.
            //     2. if the conflicting old row's delete sign is marked, which means that the key is not exist now,
            //     we should not read old values from the deleted data, and should use default value instead.
            //     NOTE: since now we are in the publishing phase, all data is commited
            //         before, even the `strict_mode` is true (which requires partial update
            //         load job can't insert new keys), this "new" key MUST be written into
            //         the new generated segment file.
            bool new_row_delete_sign =
                    (new_block_delete_signs != nullptr && new_block_delete_signs[idx]);
            if (new_row_delete_sign) {
                mutable_column->insert_default();
            } else {
                bool use_default = false;
                bool old_row_delete_sign = (old_block_delete_signs != nullptr &&
                                            old_block_delete_signs[read_index_old.at(idx)] != 0);
                if (old_row_delete_sign) {
                    if (!rowset_schema->has_sequence_col()) {
                        use_default = true;
                    } else if (have_input_seq_column || !rs_column.is_seqeunce_col()) {
                        // to keep the sequence column value not decreasing, we should read values of seq column
                        // from old rows even if the old row is deleted when the input don't specify the sequence column, otherwise
                        // it may cause the merge-on-read based compaction to produce incorrect results
                        use_default = true;
                    }
                }

                if (use_default) {
                    if (rs_column.has_default_value()) {
                        mutable_column->insert_from(*default_value_block.get_by_position(i).column,
                                                    0);
                    } else if (rs_column.is_nullable()) {
                        assert_cast<vectorized::ColumnNullable*, TypeCheckOnRelease::DISABLE>(
                                mutable_column.get())
                                ->insert_default();
                    } else {
                        mutable_column->insert(rs_column.get_vec_type()->get_default());
                    }
                } else {
                    mutable_column->insert_from(*old_block.get_by_position(i).column,
                                                read_index_old[idx]);
                }
            }
        }
    }
    output_block->set_columns(std::move(full_mutable_columns));
    VLOG_DEBUG << "full block when publish: " << output_block->dump_data();
    return Status::OK();
}

Status BaseTablet::generate_new_block_for_flexible_partial_update(
        TabletSchemaSPtr rowset_schema, const PartialUpdateInfo* partial_update_info,
        std::set<uint32_t>& rids_be_overwritten, const FixedReadPlan& read_plan_ori,
        const FixedReadPlan& read_plan_update,
        const std::map<RowsetId, RowsetSharedPtr>& rsid_to_rowset,
        vectorized::Block* output_block) {
    CHECK(output_block);

    int32_t seq_col_unique_id = -1;
    if (rowset_schema->has_sequence_col()) {
        seq_col_unique_id = rowset_schema->column(rowset_schema->sequence_col_idx()).unique_id();
    }
    const auto& non_sort_key_cids = partial_update_info->missing_cids;
    std::vector<uint32_t> all_cids(rowset_schema->num_columns());
    std::iota(all_cids.begin(), all_cids.end(), 0);
    auto old_block = rowset_schema->create_block_by_cids(non_sort_key_cids);
    auto update_block = rowset_schema->create_block_by_cids(all_cids);

    // rowid in the final block(start from 0, increase continuously) -> rowid to read in update_block
    std::map<uint32_t, uint32_t> read_index_update;

    // 1. read the current rowset first, if a row in the current rowset has delete sign mark
    // we don't need to read values from old block for that row
    RETURN_IF_ERROR(read_plan_update.read_columns_by_plan(*rowset_schema, all_cids, rsid_to_rowset,
                                                          update_block, &read_index_update, true));
    size_t update_rows = read_index_update.size();

    // TODO(bobhan1): add the delete sign optimazation here
    // // if there is sequence column in the table, we need to read the sequence column,
    // // otherwise it may cause the merge-on-read based compaction policy to produce incorrect results
    // const auto* __restrict new_block_delete_signs =
    //         rowset_schema->has_sequence_col()
    //                 ? nullptr
    //                 : get_delete_sign_column_data(update_block, update_rows);

    // 2. read previous rowsets
    // rowid in the final block(start from 0, increase, may not continuous becasue we skip to read some rows) -> rowid to read in old_block
    std::map<uint32_t, uint32_t> read_index_old;
    RETURN_IF_ERROR(read_plan_ori.read_columns_by_plan(
            *rowset_schema, non_sort_key_cids, rsid_to_rowset, old_block, &read_index_old, true));
    size_t old_rows = read_index_old.size();
    DCHECK(update_rows == old_rows);
    const auto* __restrict old_block_delete_signs =
            get_delete_sign_column_data(old_block, old_rows);
    DCHECK(old_block_delete_signs != nullptr);

    // 3. build default value block
    auto default_value_block = old_block.clone_empty();
    RETURN_IF_ERROR(BaseTablet::generate_default_value_block(*rowset_schema, non_sort_key_cids,
                                                             partial_update_info->default_values,
                                                             old_block, default_value_block));

    // 4. build the final block
    auto full_mutable_columns = output_block->mutate_columns();
    DCHECK(rowset_schema->has_skip_bitmap_col());
    auto skip_bitmap_col_idx = rowset_schema->skip_bitmap_col_idx();
    const std::vector<BitmapValue>* skip_bitmaps =
            &(assert_cast<const vectorized::ColumnBitmap*, TypeCheckOnRelease::DISABLE>(
                      update_block.get_by_position(skip_bitmap_col_idx).column->get_ptr().get())
                      ->get_data());

    if (rowset_schema->has_sequence_col() && !rids_be_overwritten.empty()) {
        // If the row specifies the sequence column, we should delete the current row becase the
        // flexible partial update on the current row has been `overwritten` by the previous one with larger sequence
        // column value.
        for (auto it = rids_be_overwritten.begin(); it != rids_be_overwritten.end();) {
            auto rid = *it;
            if (!skip_bitmaps->at(rid).contains(seq_col_unique_id)) {
                ++it;
            } else {
                it = rids_be_overwritten.erase(it);
            }
        }
    }

    auto fill_one_cell = [&read_index_old, &read_index_update, &rowset_schema, partial_update_info](
                                 const TabletColumn& tablet_column, std::size_t idx,
                                 vectorized::MutableColumnPtr& new_col,
                                 const vectorized::IColumn& default_value_col,
                                 const vectorized::IColumn& old_value_col,
                                 const vectorized::IColumn& cur_col, bool skipped,
                                 bool row_has_sequence_col,
                                 const signed char* delete_sign_column_data) {
        if (skipped) {
            bool use_default = false;
            bool old_row_delete_sign =
                    (delete_sign_column_data != nullptr &&
                     delete_sign_column_data[read_index_old[cast_set<uint32_t>(idx)]] != 0);
            if (old_row_delete_sign) {
                if (!rowset_schema->has_sequence_col()) {
                    use_default = true;
                } else if (row_has_sequence_col ||
                           (!tablet_column.is_seqeunce_col() &&
                            (tablet_column.unique_id() !=
                             partial_update_info->sequence_map_col_uid()))) {
                    // to keep the sequence column value not decreasing, we should read values of seq column(and seq map column)
                    // from old rows even if the old row is deleted when the input don't specify the sequence column, otherwise
                    // it may cause the merge-on-read based compaction to produce incorrect results
                    use_default = true;
                }
            }
            if (use_default) {
                if (tablet_column.has_default_value()) {
                    new_col->insert_from(default_value_col, 0);
                } else if (tablet_column.is_nullable()) {
                    assert_cast<vectorized::ColumnNullable*, TypeCheckOnRelease::DISABLE>(
                            new_col.get())
                            ->insert_many_defaults(1);
                } else if (tablet_column.is_auto_increment()) {
                    // For auto-increment column, its default value(generated value) is filled in current block in flush phase
                    // when the load doesn't specify the auto-increment column
                    //     - if the previous conflicting row is deleted, we should use the value in current block as its final value
                    //     - if the previous conflicting row is an insert, we should use the value in old block as its final value to
                    //       keep consistency between replicas
                    new_col->insert_from(cur_col, read_index_update[cast_set<uint32_t>(idx)]);
                } else {
                    new_col->insert(tablet_column.get_vec_type()->get_default());
                }
            } else {
                new_col->insert_from(old_value_col, read_index_old[cast_set<uint32_t>(idx)]);
            }
        } else {
            new_col->insert_from(cur_col, read_index_update[cast_set<uint32_t>(idx)]);
        }
    };

    for (std::size_t cid {0}; cid < rowset_schema->num_columns(); cid++) {
        vectorized::MutableColumnPtr& new_col = full_mutable_columns[cid];
        const vectorized::IColumn& cur_col = *update_block.get_by_position(cid).column;
        const auto& rs_column = rowset_schema->column(cid);
        auto col_uid = rs_column.unique_id();
        for (auto idx = 0; idx < update_rows; ++idx) {
            if (cid < rowset_schema->num_key_columns()) {
                new_col->insert_from(cur_col, read_index_update[idx]);
            } else {
                const vectorized::IColumn& default_value_col =
                        *default_value_block.get_by_position(cid - rowset_schema->num_key_columns())
                                 .column;
                const vectorized::IColumn& old_value_col =
                        *old_block.get_by_position(cid - rowset_schema->num_key_columns()).column;
                if (rids_be_overwritten.contains(idx)) {
                    new_col->insert_from(old_value_col, read_index_old[idx]);
                } else {
                    fill_one_cell(rs_column, idx, new_col, default_value_col, old_value_col,
                                  cur_col, skip_bitmaps->at(idx).contains(col_uid),
                                  rowset_schema->has_sequence_col()
                                          ? !skip_bitmaps->at(idx).contains(seq_col_unique_id)
                                          : false,
                                  old_block_delete_signs);
                }
            }
        }
        DCHECK_EQ(full_mutable_columns[cid]->size(), update_rows);
    }

    output_block->set_columns(std::move(full_mutable_columns));
    VLOG_DEBUG << "full block when publish: " << output_block->dump_data();
    return Status::OK();
}

Status BaseTablet::commit_phase_update_delete_bitmap(
        const BaseTabletSPtr& tablet, const RowsetSharedPtr& rowset,
        RowsetIdUnorderedSet& pre_rowset_ids, DeleteBitmapPtr delete_bitmap,
        const std::vector<segment_v2::SegmentSharedPtr>& segments, int64_t txn_id,
        CalcDeleteBitmapToken* token, RowsetWriter* rowset_writer) {
    DBUG_EXECUTE_IF("BaseTablet::commit_phase_update_delete_bitmap.enable_spin_wait", {
        auto tok = dp->param<std::string>("token", "invalid_token");
        while (DebugPoints::instance()->is_enable(
                "BaseTablet::commit_phase_update_delete_bitmap.block")) {
            auto block_dp = DebugPoints::instance()->get_debug_point(
                    "BaseTablet::commit_phase_update_delete_bitmap.block");
            if (block_dp) {
                auto pass_token = block_dp->param<std::string>("pass_token", "");
                if (pass_token == tok) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    SCOPED_BVAR_LATENCY(g_tablet_commit_phase_update_delete_bitmap_latency);
    RowsetIdUnorderedSet cur_rowset_ids;
    RowsetIdUnorderedSet rowset_ids_to_add;
    RowsetIdUnorderedSet rowset_ids_to_del;
    int64_t cur_version;

    std::vector<RowsetSharedPtr> specified_rowsets;
    {
        // to prevent seeing intermediate state of a tablet
        std::unique_lock<bthread::Mutex> sync_lock;
        if (config::is_cloud_mode()) {
            sync_lock = std::unique_lock<bthread::Mutex>(
                    std::static_pointer_cast<CloudTablet>(tablet)->get_sync_meta_lock());
        }
        std::shared_lock meta_rlock(tablet->_meta_lock);
        if (tablet->tablet_state() == TABLET_NOTREADY) {
            // tablet is under alter process. The delete bitmap will be calculated after conversion.
            LOG(INFO) << "tablet is under alter process, delete bitmap will be calculated later, "
                         "tablet_id: "
                      << tablet->tablet_id() << " txn_id: " << txn_id;
            return Status::OK();
        }
        cur_version = tablet->max_version_unlocked();
        RETURN_IF_ERROR(tablet->get_all_rs_id_unlocked(cur_version, &cur_rowset_ids));
        _rowset_ids_difference(cur_rowset_ids, pre_rowset_ids, &rowset_ids_to_add,
                               &rowset_ids_to_del);
        specified_rowsets = tablet->get_rowset_by_ids(&rowset_ids_to_add);
    }
    for (const auto& to_del : rowset_ids_to_del) {
        delete_bitmap->remove({to_del, 0, 0}, {to_del, UINT32_MAX, INT64_MAX});
    }

    RETURN_IF_ERROR(calc_delete_bitmap(tablet, rowset, segments, specified_rowsets, delete_bitmap,
                                       cur_version, token, rowset_writer));
    size_t total_rows = std::accumulate(
            segments.begin(), segments.end(), 0,
            [](size_t sum, const segment_v2::SegmentSharedPtr& s) { return sum += s->num_rows(); });
    LOG(INFO) << "[Before Commit] construct delete bitmap tablet: " << tablet->tablet_id()
              << ", rowset_ids to add: " << rowset_ids_to_add.size()
              << ", rowset_ids to del: " << rowset_ids_to_del.size()
              << ", cur max_version: " << cur_version << ", transaction_id: " << txn_id
              << ", total rows: " << total_rows;
    pre_rowset_ids = cur_rowset_ids;
    return Status::OK();
}

void BaseTablet::add_sentinel_mark_to_delete_bitmap(DeleteBitmap* delete_bitmap,
                                                    const RowsetIdUnorderedSet& rowsetids) {
    for (const auto& rowsetid : rowsetids) {
        delete_bitmap->add(
                {rowsetid, DeleteBitmap::INVALID_SEGMENT_ID, DeleteBitmap::TEMP_VERSION_COMMON},
                DeleteBitmap::ROWSET_SENTINEL_MARK);
    }
}

void BaseTablet::_rowset_ids_difference(const RowsetIdUnorderedSet& cur,
                                        const RowsetIdUnorderedSet& pre,
                                        RowsetIdUnorderedSet* to_add,
                                        RowsetIdUnorderedSet* to_del) {
    for (const auto& id : cur) {
        if (pre.find(id) == pre.end()) {
            to_add->insert(id);
        }
    }
    for (const auto& id : pre) {
        if (cur.find(id) == cur.end()) {
            to_del->insert(id);
        }
    }
}

Status BaseTablet::_capture_consistent_rowsets_unlocked(
        const std::vector<Version>& version_path, std::vector<RowsetSharedPtr>* rowsets) const {
    DCHECK(rowsets != nullptr);
    rowsets->reserve(version_path.size());
    for (const auto& version : version_path) {
        bool is_find = false;
        do {
            auto it = _rs_version_map.find(version);
            if (it != _rs_version_map.end()) {
                is_find = true;
                rowsets->push_back(it->second);
                break;
            }

            auto it_expired = _stale_rs_version_map.find(version);
            if (it_expired != _stale_rs_version_map.end()) {
                is_find = true;
                rowsets->push_back(it_expired->second);
                break;
            }
        } while (false);

        if (!is_find) {
            return Status::Error<CAPTURE_ROWSET_ERROR>(
                    "fail to find Rowset for version. tablet={}, version={}", tablet_id(),
                    version.to_string());
        }
    }
    return Status::OK();
}

Status BaseTablet::check_delete_bitmap_correctness(DeleteBitmapPtr delete_bitmap,
                                                   int64_t max_version, int64_t txn_id,
                                                   const RowsetIdUnorderedSet& rowset_ids,
                                                   std::vector<RowsetSharedPtr>* rowsets) {
    RowsetIdUnorderedSet missing_ids;
    for (const auto& rowsetid : rowset_ids) {
        if (!delete_bitmap->delete_bitmap.contains({rowsetid, DeleteBitmap::INVALID_SEGMENT_ID,
                                                    DeleteBitmap::TEMP_VERSION_COMMON})) {
            missing_ids.insert(rowsetid);
        }
    }

    if (!missing_ids.empty()) {
        LOG(WARNING) << "[txn_id:" << txn_id << "][tablet_id:" << tablet_id()
                     << "][max_version: " << max_version
                     << "] check delete bitmap correctness failed!";
        rapidjson::Document root;
        root.SetObject();
        rapidjson::Document required_rowsets_arr;
        required_rowsets_arr.SetArray();
        rapidjson::Document missing_rowsets_arr;
        missing_rowsets_arr.SetArray();

        if (rowsets != nullptr) {
            for (const auto& rowset : *rowsets) {
                rapidjson::Value value;
                std::string version_str = rowset->get_rowset_info_str();
                value.SetString(version_str.c_str(),
                                cast_set<rapidjson::SizeType>(version_str.length()),
                                required_rowsets_arr.GetAllocator());
                required_rowsets_arr.PushBack(value, required_rowsets_arr.GetAllocator());
            }
        } else {
            std::vector<RowsetSharedPtr> tablet_rowsets;
            {
                std::shared_lock meta_rlock(_meta_lock);
                tablet_rowsets = get_rowset_by_ids(&rowset_ids);
            }
            for (const auto& rowset : tablet_rowsets) {
                rapidjson::Value value;
                std::string version_str = rowset->get_rowset_info_str();
                value.SetString(version_str.c_str(),
                                cast_set<rapidjson::SizeType>(version_str.length()),
                                required_rowsets_arr.GetAllocator());
                required_rowsets_arr.PushBack(value, required_rowsets_arr.GetAllocator());
            }
        }
        for (const auto& missing_rowset_id : missing_ids) {
            rapidjson::Value miss_value;
            std::string rowset_id_str = missing_rowset_id.to_string();
            miss_value.SetString(rowset_id_str.c_str(),
                                 cast_set<rapidjson::SizeType>(rowset_id_str.length()),
                                 missing_rowsets_arr.GetAllocator());
            missing_rowsets_arr.PushBack(miss_value, missing_rowsets_arr.GetAllocator());
        }

        root.AddMember("required_rowsets", required_rowsets_arr, root.GetAllocator());
        root.AddMember("missing_rowsets", missing_rowsets_arr, root.GetAllocator());
        rapidjson::StringBuffer strbuf;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
        root.Accept(writer);
        std::string rowset_status_string = std::string(strbuf.GetString());
        LOG_EVERY_SECOND(WARNING) << rowset_status_string;
        // let it crash if correctness check failed in Debug mode
        DCHECK(false) << "delete bitmap correctness check failed in publish phase!";
        return Status::InternalError("check delete bitmap failed!");
    }
    return Status::OK();
}

Status BaseTablet::update_delete_bitmap(const BaseTabletSPtr& self, TabletTxnInfo* txn_info,
                                        int64_t txn_id, int64_t txn_expiration,
                                        DeleteBitmapPtr tablet_delete_bitmap) {
    SCOPED_BVAR_LATENCY(g_tablet_update_delete_bitmap_latency);
    RowsetIdUnorderedSet cur_rowset_ids;
    RowsetIdUnorderedSet rowset_ids_to_add;
    RowsetIdUnorderedSet rowset_ids_to_del;
    RowsetSharedPtr rowset = txn_info->rowset;
    int64_t cur_version = rowset->start_version();

    std::unique_ptr<RowsetWriter> transient_rs_writer;
    DeleteBitmapPtr delete_bitmap = txn_info->delete_bitmap;
    bool is_partial_update =
            txn_info->partial_update_info && txn_info->partial_update_info->is_partial_update();
    if (is_partial_update) {
        transient_rs_writer = DORIS_TRY(self->create_transient_rowset_writer(
                *rowset, txn_info->partial_update_info, txn_expiration));
        DBUG_EXECUTE_IF("BaseTablet::update_delete_bitmap.after.create_transient_rs_writer",
                        DBUG_BLOCK);
        // Partial update might generate new segments when there is conflicts while publish, and mark
        // the same key in original segments as delete.
        // When the new segment flush fails or the rowset build fails, the deletion marker for the
        // duplicate key of the original segment should not remain in `txn_info->delete_bitmap`,
        // so we need to make a copy of `txn_info->delete_bitmap` and make changes on it.
        delete_bitmap = std::make_shared<DeleteBitmap>(*(txn_info->delete_bitmap));
    }

    OlapStopWatch watch;
    std::vector<segment_v2::SegmentSharedPtr> segments;
    RETURN_IF_ERROR(std::dynamic_pointer_cast<BetaRowset>(rowset)->load_segments(&segments));
    auto t1 = watch.get_elapse_time_us();

    int64_t next_visible_version = txn_info->is_txn_load ? txn_info->next_visible_version
                                                         : txn_info->rowset->start_version();
    {
        std::shared_lock meta_rlock(self->_meta_lock);
        // tablet is under alter process. The delete bitmap will be calculated after conversion.
        if (self->tablet_state() == TABLET_NOTREADY) {
            LOG(INFO) << "tablet is under alter process, update delete bitmap later, tablet_id="
                      << self->tablet_id();
            return Status::OK();
        }
        RETURN_IF_ERROR(self->get_all_rs_id_unlocked(next_visible_version - 1, &cur_rowset_ids));
    }
    auto t2 = watch.get_elapse_time_us();

    _rowset_ids_difference(cur_rowset_ids, txn_info->rowset_ids, &rowset_ids_to_add,
                           &rowset_ids_to_del);
    for (const auto& to_del : rowset_ids_to_del) {
        delete_bitmap->remove({to_del, 0, 0}, {to_del, UINT32_MAX, INT64_MAX});
    }

    std::vector<RowsetSharedPtr> specified_rowsets;
    {
        std::shared_lock meta_rlock(self->_meta_lock);
        specified_rowsets = self->get_rowset_by_ids(&rowset_ids_to_add);
    }
    if (txn_info->is_txn_load) {
        for (auto invisible_rowset : txn_info->invisible_rowsets) {
            specified_rowsets.emplace_back(invisible_rowset);
        }
        std::sort(specified_rowsets.begin(), specified_rowsets.end(),
                  [](RowsetSharedPtr& lhs, RowsetSharedPtr& rhs) {
                      return lhs->end_version() > rhs->end_version();
                  });
    }
    auto t3 = watch.get_elapse_time_us();

    // If a rowset is produced by compaction before the commit phase of the partial update load
    // and is not included in txn_info->rowset_ids, we can skip the alignment process of that rowset
    // because data remains the same before and after compaction. But we still need to calculate the
    // the delete bitmap for that rowset.
    std::vector<RowsetSharedPtr> rowsets_skip_alignment;
    if (is_partial_update) {
        int64_t max_version_in_flush_phase =
                txn_info->partial_update_info->max_version_in_flush_phase;
        DCHECK(max_version_in_flush_phase != -1);
        std::vector<RowsetSharedPtr> remained_rowsets;
        for (const auto& specified_rowset : specified_rowsets) {
            if (specified_rowset->end_version() <= max_version_in_flush_phase &&
                specified_rowset->produced_by_compaction()) {
                rowsets_skip_alignment.emplace_back(specified_rowset);
            } else {
                remained_rowsets.emplace_back(specified_rowset);
            }
        }
        if (!rowsets_skip_alignment.empty()) {
            specified_rowsets = std::move(remained_rowsets);
        }
    }

    DBUG_EXECUTE_IF("BaseTablet::update_delete_bitmap.enable_spin_wait", {
        auto token = dp->param<std::string>("token", "invalid_token");
        while (DebugPoints::instance()->is_enable("BaseTablet::update_delete_bitmap.block")) {
            auto block_dp = DebugPoints::instance()->get_debug_point(
                    "BaseTablet::update_delete_bitmap.block");
            if (block_dp) {
                auto wait_token = block_dp->param<std::string>("wait_token", "");
                if (wait_token != token) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    if (!rowsets_skip_alignment.empty()) {
        auto token = self->calc_delete_bitmap_executor()->create_token();
        // set rowset_writer to nullptr to skip the alignment process
        RETURN_IF_ERROR(calc_delete_bitmap(self, rowset, segments, rowsets_skip_alignment,
                                           delete_bitmap, cur_version - 1, token.get(), nullptr,
                                           tablet_delete_bitmap));
        RETURN_IF_ERROR(token->wait());
    }

    // When there is only one segment, it will be calculated in the current thread.
    // Otherwise, it will be submitted to the thread pool for calculation.
    if (segments.size() <= 1) {
        RETURN_IF_ERROR(calc_delete_bitmap(self, rowset, segments, specified_rowsets, delete_bitmap,
                                           cur_version - 1, nullptr, transient_rs_writer.get(),
                                           tablet_delete_bitmap));

    } else {
        auto token = self->calc_delete_bitmap_executor()->create_token();
        RETURN_IF_ERROR(calc_delete_bitmap(self, rowset, segments, specified_rowsets, delete_bitmap,
                                           cur_version - 1, token.get(), transient_rs_writer.get(),
                                           tablet_delete_bitmap));
        RETURN_IF_ERROR(token->wait());
    }

    std::stringstream ss;
    ss << "cost(us): (load segments: " << t1 << ", get all rsid: " << t2 - t1
       << ", get rowsets: " << t3 - t2
       << ", calc delete bitmap: " << watch.get_elapse_time_us() - t3 << ")";

    if (config::enable_merge_on_write_correctness_check && rowset->num_rows() != 0) {
        // only do correctness check if the rowset has at least one row written
        // check if all the rowset has ROWSET_SENTINEL_MARK
        auto st = self->check_delete_bitmap_correctness(delete_bitmap, cur_version - 1, -1,
                                                        cur_rowset_ids, &specified_rowsets);
        if (!st.ok()) {
            LOG(WARNING) << fmt::format("delete bitmap correctness check failed in publish phase!");
        }
    }

    if (transient_rs_writer) {
        auto t4 = watch.get_elapse_time_us();
        DBUG_EXECUTE_IF("Tablet.update_delete_bitmap.partial_update_write_rowset_fail", {
            if (rand() % 100 < (100 * dp->param("percent", 0.5))) {
                LOG_WARNING("Tablet.update_delete_bitmap.partial_update_write_rowset random failed")
                        .tag("txn_id", txn_id);
                return Status::InternalError(
                        "debug update_delete_bitmap partial update write rowset random failed");
            }
        });
        // build rowset writer and merge transient rowset
        RETURN_IF_ERROR(transient_rs_writer->flush());
        RowsetSharedPtr transient_rowset;
        RETURN_IF_ERROR(transient_rs_writer->build(transient_rowset));
        auto old_segments = rowset->num_segments();
        rowset->merge_rowset_meta(*transient_rowset->rowset_meta());
        auto new_segments = rowset->num_segments();
        ss << ", " << txn_info->partial_update_info->partial_update_mode_str()
           << " flush rowset (old segment num: " << old_segments
           << ", new segment num: " << new_segments << ")"
           << ", cost:" << watch.get_elapse_time_us() - t4 << "(us)";

        // update the shared_ptr to new bitmap, which is consistent with current rowset.
        txn_info->delete_bitmap = delete_bitmap;
        // erase segment cache cause we will add a segment to rowset
        SegmentLoader::instance()->erase_segments(rowset->rowset_id(), rowset->num_segments());
    }

    size_t total_rows = std::accumulate(
            segments.begin(), segments.end(), 0,
            [](size_t sum, const segment_v2::SegmentSharedPtr& s) { return sum += s->num_rows(); });
    auto t5 = watch.get_elapse_time_us();
    int64_t lock_id = txn_info->is_txn_load ? txn_info->lock_id : -1;
    RETURN_IF_ERROR(self->save_delete_bitmap(txn_info, txn_id, delete_bitmap,
                                             transient_rs_writer.get(), cur_rowset_ids, lock_id,
                                             next_visible_version));

    // defensive check, check that the delete bitmap cache we wrote is correct
    RETURN_IF_ERROR(self->check_delete_bitmap_cache(txn_id, delete_bitmap.get()));

    LOG(INFO) << "[Publish] construct delete bitmap tablet: " << self->tablet_id()
              << ", rowset_ids to add: "
              << (specified_rowsets.size() + rowsets_skip_alignment.size())
              << ", rowset_ids to del: " << rowset_ids_to_del.size()
              << ", cur version: " << cur_version << ", transaction_id: " << txn_id << ","
              << ss.str() << " , total rows: " << total_rows
              << ", update delete_bitmap cost: " << watch.get_elapse_time_us() - t5 << "(us)";
    return Status::OK();
}

void BaseTablet::calc_compaction_output_rowset_delete_bitmap(
        const std::vector<RowsetSharedPtr>& input_rowsets, const RowIdConversion& rowid_conversion,
        uint64_t start_version, uint64_t end_version, std::set<RowLocation>* missed_rows,
        std::map<RowsetSharedPtr, std::list<std::pair<RowLocation, RowLocation>>>* location_map,
        const DeleteBitmap& input_delete_bitmap, DeleteBitmap* output_rowset_delete_bitmap) {
    RowLocation src;
    RowLocation dst;
    for (auto& rowset : input_rowsets) {
        src.rowset_id = rowset->rowset_id();
        for (uint32_t seg_id = 0; seg_id < rowset->num_segments(); ++seg_id) {
            src.segment_id = seg_id;
            DeleteBitmap subset_map(tablet_id());
            input_delete_bitmap.subset({rowset->rowset_id(), seg_id, start_version},
                                       {rowset->rowset_id(), seg_id, end_version}, &subset_map);
            // traverse all versions and convert rowid
            for (auto iter = subset_map.delete_bitmap.begin();
                 iter != subset_map.delete_bitmap.end(); ++iter) {
                auto cur_version = std::get<2>(iter->first);
                for (auto index = iter->second.begin(); index != iter->second.end(); ++index) {
                    src.row_id = *index;
                    if (rowid_conversion.get(src, &dst) != 0) {
                        VLOG_CRITICAL << "Can't find rowid, may be deleted by the delete_handler, "
                                      << " src loaction: |" << src.rowset_id << "|"
                                      << src.segment_id << "|" << src.row_id
                                      << " version: " << cur_version;
                        if (missed_rows) {
                            missed_rows->insert(src);
                        }
                        continue;
                    }
                    VLOG_DEBUG << "calc_compaction_output_rowset_delete_bitmap dst location: |"
                               << dst.rowset_id << "|" << dst.segment_id << "|" << dst.row_id
                               << " src location: |" << src.rowset_id << "|" << src.segment_id
                               << "|" << src.row_id << " start version: " << start_version
                               << "end version" << end_version;
                    if (location_map) {
                        (*location_map)[rowset].emplace_back(src, dst);
                    }
                    output_rowset_delete_bitmap->add({dst.rowset_id, dst.segment_id, cur_version},
                                                     dst.row_id);
                }
            }
        }
    }
}

Status BaseTablet::check_rowid_conversion(
        RowsetSharedPtr dst_rowset,
        const std::map<RowsetSharedPtr, std::list<std::pair<RowLocation, RowLocation>>>&
                location_map) {
    if (location_map.empty()) {
        VLOG_DEBUG << "check_rowid_conversion, location_map is empty";
        return Status::OK();
    }
    std::vector<segment_v2::SegmentSharedPtr> dst_segments;

    RETURN_IF_ERROR(
            std::dynamic_pointer_cast<BetaRowset>(dst_rowset)->load_segments(&dst_segments));
    std::unordered_map<RowsetId, std::vector<segment_v2::SegmentSharedPtr>> input_rowsets_segment;

    VLOG_DEBUG << "check_rowid_conversion, dst_segments size: " << dst_segments.size();
    for (auto [src_rowset, locations] : location_map) {
        std::vector<segment_v2::SegmentSharedPtr>& segments =
                input_rowsets_segment[src_rowset->rowset_id()];
        if (segments.empty()) {
            RETURN_IF_ERROR(
                    std::dynamic_pointer_cast<BetaRowset>(src_rowset)->load_segments(&segments));
        }
        for (auto& [src, dst] : locations) {
            std::string src_key;
            std::string dst_key;
            Status s = segments[src.segment_id]->read_key_by_rowid(src.row_id, &src_key);
            if (UNLIKELY(s.is<NOT_IMPLEMENTED_ERROR>())) {
                LOG(INFO) << "primary key index of old version does not "
                             "support reading key by rowid";
                break;
            }
            if (UNLIKELY(!s)) {
                LOG(WARNING) << "failed to get src key: |" << src.rowset_id << "|" << src.segment_id
                             << "|" << src.row_id << " status: " << s;
                DCHECK(false);
                return s;
            }

            s = dst_segments[dst.segment_id]->read_key_by_rowid(dst.row_id, &dst_key);
            if (UNLIKELY(!s)) {
                LOG(WARNING) << "failed to get dst key: |" << dst.rowset_id << "|" << dst.segment_id
                             << "|" << dst.row_id << " status: " << s;
                DCHECK(false);
                return s;
            }

            VLOG_DEBUG << "check_rowid_conversion, src: |" << src.rowset_id << "|" << src.segment_id
                       << "|" << src.row_id << "|" << src_key << " dst: |" << dst.rowset_id << "|"
                       << dst.segment_id << "|" << dst.row_id << "|" << dst_key;
            if (UNLIKELY(src_key.compare(dst_key) != 0)) {
                LOG(WARNING) << "failed to check key, src key: |" << src.rowset_id << "|"
                             << src.segment_id << "|" << src.row_id << "|" << src_key
                             << " dst key: |" << dst.rowset_id << "|" << dst.segment_id << "|"
                             << dst.row_id << "|" << dst_key;
                DCHECK(false);
                return Status::InternalError("failed to check rowid conversion");
            }
        }
    }
    return Status::OK();
}

// The caller should hold _rowset_update_lock and _meta_lock lock.
Status BaseTablet::update_delete_bitmap_without_lock(
        const BaseTabletSPtr& self, const RowsetSharedPtr& rowset,
        const std::vector<RowsetSharedPtr>* specified_base_rowsets) {
    DBUG_EXECUTE_IF("BaseTablet.update_delete_bitmap_without_lock.random_failed", {
        auto rnd = rand() % 100;
        auto percent = dp->param("percent", 0.1);
        if (rnd < (100 * percent)) {
            LOG(WARNING) << "BaseTablet.update_delete_bitmap_without_lock.random_failed";
            return Status::InternalError(
                    "debug tablet update delete bitmap without lock random failed");
        } else {
            LOG(INFO) << "BaseTablet.update_delete_bitmap_without_lock.random_failed not "
                         "triggered"
                      << ", rnd:" << rnd << ", percent: " << percent;
        }
    });
    int64_t cur_version = rowset->start_version();
    std::vector<segment_v2::SegmentSharedPtr> segments;
    RETURN_IF_ERROR(std::dynamic_pointer_cast<BetaRowset>(rowset)->load_segments(&segments));

    // If this rowset does not have a segment, there is no need for an update.
    if (segments.empty()) {
        LOG(INFO) << "[Schema Change or Clone] skip to construct delete bitmap tablet: "
                  << self->tablet_id() << " cur max_version: " << cur_version;
        return Status::OK();
    }

    // calculate delete bitmap between segments if necessary.
    DeleteBitmapPtr delete_bitmap = std::make_shared<DeleteBitmap>(self->tablet_id());
    RETURN_IF_ERROR(self->calc_delete_bitmap_between_segments(rowset->rowset_id(), segments,
                                                              delete_bitmap));

    // get all base rowsets to calculate on
    std::vector<RowsetSharedPtr> specified_rowsets;
    RowsetIdUnorderedSet cur_rowset_ids;
    if (specified_base_rowsets == nullptr) {
        RETURN_IF_ERROR(self->get_all_rs_id_unlocked(cur_version - 1, &cur_rowset_ids));
        specified_rowsets = self->get_rowset_by_ids(&cur_rowset_ids);
    } else {
        specified_rowsets = *specified_base_rowsets;
    }

    OlapStopWatch watch;
    auto token = self->calc_delete_bitmap_executor()->create_token();
    RETURN_IF_ERROR(calc_delete_bitmap(self, rowset, segments, specified_rowsets, delete_bitmap,
                                       cur_version - 1, token.get()));
    RETURN_IF_ERROR(token->wait());
    size_t total_rows = std::accumulate(
            segments.begin(), segments.end(), 0,
            [](size_t sum, const segment_v2::SegmentSharedPtr& s) { return sum += s->num_rows(); });
    LOG(INFO) << "[Schema Change or Clone] construct delete bitmap tablet: " << self->tablet_id()
              << ", rowset_ids: " << cur_rowset_ids.size() << ", cur max_version: " << cur_version
              << ", transaction_id: " << -1 << ", cost: " << watch.get_elapse_time_us()
              << "(us), total rows: " << total_rows;
    if (config::enable_merge_on_write_correctness_check) {
        // check if all the rowset has ROWSET_SENTINEL_MARK
        auto st = self->check_delete_bitmap_correctness(delete_bitmap, cur_version - 1, -1,
                                                        cur_rowset_ids, &specified_rowsets);
        if (!st.ok()) {
            LOG(WARNING) << fmt::format("delete bitmap correctness check failed in publish phase!");
        }
        delete_bitmap->remove_sentinel_marks();
    }
    for (auto& iter : delete_bitmap->delete_bitmap) {
        self->_tablet_meta->delete_bitmap().merge(
                {std::get<0>(iter.first), std::get<1>(iter.first), cur_version}, iter.second);
    }

    return Status::OK();
}

void BaseTablet::agg_delete_bitmap_for_stale_rowsets(
        Version version, DeleteBitmapKeyRanges& remove_delete_bitmap_key_ranges) {
    if (!config::enable_agg_and_remove_pre_rowsets_delete_bitmap) {
        return;
    }
    if (!(keys_type() == UNIQUE_KEYS && enable_unique_key_merge_on_write())) {
        return;
    }
    int64_t start_version = version.first;
    int64_t end_version = version.second;
    if (start_version == end_version) {
        return;
    }
    DCHECK(start_version < end_version)
            << ". start_version: " << start_version << ", end_version: " << end_version;
    // get pre rowsets
    std::vector<RowsetSharedPtr> pre_rowsets {};
    {
        std::shared_lock rdlock(_meta_lock);
        for (const auto& it2 : _rs_version_map) {
            if (it2.first.second < start_version) {
                pre_rowsets.emplace_back(it2.second);
            }
        }
    }
    std::sort(pre_rowsets.begin(), pre_rowsets.end(), Rowset::comparator);
    // do agg for pre rowsets
    DeleteBitmapPtr new_delete_bitmap = std::make_shared<DeleteBitmap>(tablet_id());
    for (auto& rowset : pre_rowsets) {
        for (uint32_t seg_id = 0; seg_id < rowset->num_segments(); ++seg_id) {
            auto d = tablet_meta()->delete_bitmap().get_agg_without_cache(
                    {rowset->rowset_id(), seg_id, end_version}, start_version);
            if (d->isEmpty()) {
                continue;
            }
            VLOG_DEBUG << "agg delete bitmap for tablet_id=" << tablet_id()
                       << ", rowset_id=" << rowset->rowset_id() << ", seg_id=" << seg_id
                       << ", rowset_version=" << rowset->version().to_string()
                       << ". compaction start_version=" << start_version
                       << ", end_version=" << end_version << ", delete_bitmap=" << d->cardinality();
            DeleteBitmap::BitmapKey start_key {rowset->rowset_id(), seg_id, start_version};
            DeleteBitmap::BitmapKey end_key {rowset->rowset_id(), seg_id, end_version};
            new_delete_bitmap->set(end_key, *d);
            remove_delete_bitmap_key_ranges.emplace_back(start_key, end_key);
        }
    }
    DBUG_EXECUTE_IF("BaseTablet.agg_delete_bitmap_for_stale_rowsets.merge_delete_bitmap.block",
                    DBUG_BLOCK);
    tablet_meta()->delete_bitmap().merge(*new_delete_bitmap);
}

void BaseTablet::check_agg_delete_bitmap_for_stale_rowsets(int64_t& useless_rowset_count,
                                                           int64_t& useless_rowset_version_count) {
    std::set<RowsetId> rowset_ids;
    std::set<int64_t> end_versions;
    traverse_rowsets(
            [&rowset_ids, &end_versions](const RowsetSharedPtr& rs) {
                rowset_ids.emplace(rs->rowset_id());
                end_versions.emplace(rs->end_version());
            },
            true);

    std::set<RowsetId> useless_rowsets;
    std::map<RowsetId, std::vector<int64_t>> useless_rowset_versions;
    {
        _tablet_meta->delete_bitmap().traverse_rowset_and_version(
                // 0: rowset and rowset with version exists
                // -1: rowset does not exist
                // -2: rowset exist, rowset with version does not exist
                [&](const RowsetId& rowset_id, int64_t version) {
                    if (rowset_ids.find(rowset_id) == rowset_ids.end()) {
                        useless_rowsets.emplace(rowset_id);
                        return -1;
                    }
                    if (end_versions.find(version) == end_versions.end()) {
                        if (useless_rowset_versions.find(rowset_id) ==
                            useless_rowset_versions.end()) {
                            useless_rowset_versions[rowset_id] = {};
                        }
                        useless_rowset_versions[rowset_id].emplace_back(version);
                        return -2;
                    }
                    return 0;
                });
    }
    useless_rowset_count = useless_rowsets.size();
    useless_rowset_version_count = useless_rowset_versions.size();
    if (!useless_rowsets.empty() || !useless_rowset_versions.empty()) {
        std::stringstream ss;
        if (!useless_rowsets.empty()) {
            ss << "useless rowsets: {";
            for (auto it = useless_rowsets.begin(); it != useless_rowsets.end(); ++it) {
                if (it != useless_rowsets.begin()) {
                    ss << ", ";
                }
                ss << it->to_string();
            }
            ss << "}. ";
        }
        if (!useless_rowset_versions.empty()) {
            ss << "useless rowset versions: {";
            for (auto iter = useless_rowset_versions.begin(); iter != useless_rowset_versions.end();
                 ++iter) {
                if (iter != useless_rowset_versions.begin()) {
                    ss << ", ";
                }
                ss << iter->first.to_string() << ": [";
                // some versions are continuous, such as [8, 9, 10, 11, 13, 17, 18]
                // print as [8-11, 13, 17-18]
                int64_t last_start_version = -1;
                int64_t last_end_version = -1;
                for (int64_t version : iter->second) {
                    if (last_start_version == -1) {
                        last_start_version = version;
                        last_end_version = version;
                        continue;
                    }
                    if (last_end_version + 1 == version) {
                        last_end_version = version;
                    } else {
                        if (last_start_version == last_end_version) {
                            ss << last_start_version << ", ";
                        } else {
                            ss << last_start_version << "-" << last_end_version << ", ";
                        }
                        last_start_version = version;
                        last_end_version = version;
                    }
                }
                if (last_start_version == last_end_version) {
                    ss << last_start_version;
                } else {
                    ss << last_start_version << "-" << last_end_version;
                }

                ss << "]";
            }
            ss << "}.";
        }
        LOG(WARNING) << "failed check_agg_delete_bitmap_for_stale_rowsets for tablet_id="
                     << tablet_id() << ". " << ss.str();
    } else {
        LOG(INFO) << "succeed check_agg_delete_bitmap_for_stale_rowsets for tablet_id="
                  << tablet_id();
    }
}

RowsetSharedPtr BaseTablet::get_rowset(const RowsetId& rowset_id) {
    std::shared_lock rdlock(_meta_lock);
    for (auto& version_rowset : _rs_version_map) {
        if (version_rowset.second->rowset_id() == rowset_id) {
            return version_rowset.second;
        }
    }
    for (auto& stale_version_rowset : _stale_rs_version_map) {
        if (stale_version_rowset.second->rowset_id() == rowset_id) {
            return stale_version_rowset.second;
        }
    }
    return nullptr;
}

std::vector<RowsetSharedPtr> BaseTablet::get_snapshot_rowset(bool include_stale_rowset) const {
    std::shared_lock rdlock(_meta_lock);
    std::vector<RowsetSharedPtr> rowsets;
    std::transform(_rs_version_map.cbegin(), _rs_version_map.cend(), std::back_inserter(rowsets),
                   [](auto& kv) { return kv.second; });
    if (include_stale_rowset) {
        std::transform(_stale_rs_version_map.cbegin(), _stale_rs_version_map.cend(),
                       std::back_inserter(rowsets), [](auto& kv) { return kv.second; });
    }
    return rowsets;
}

void BaseTablet::calc_consecutive_empty_rowsets(
        std::vector<RowsetSharedPtr>* empty_rowsets,
        const std::vector<RowsetSharedPtr>& candidate_rowsets, int64_t limit) {
    int len = cast_set<int>(candidate_rowsets.size());
    for (int i = 0; i < len - 1; ++i) {
        auto rowset = candidate_rowsets[i];
        auto next_rowset = candidate_rowsets[i + 1];

        // identify two consecutive rowsets that are empty
        if (rowset->num_segments() == 0 && next_rowset->num_segments() == 0 &&
            !rowset->rowset_meta()->has_delete_predicate() &&
            !next_rowset->rowset_meta()->has_delete_predicate() &&
            rowset->end_version() == next_rowset->start_version() - 1) {
            empty_rowsets->emplace_back(rowset);
            empty_rowsets->emplace_back(next_rowset);
            rowset = next_rowset;
            int next_index = i + 2;

            // keep searching for consecutive empty rowsets
            while (next_index < len && candidate_rowsets[next_index]->num_segments() == 0 &&
                   !candidate_rowsets[next_index]->rowset_meta()->has_delete_predicate() &&
                   rowset->end_version() == candidate_rowsets[next_index]->start_version() - 1) {
                empty_rowsets->emplace_back(candidate_rowsets[next_index]);
                rowset = candidate_rowsets[next_index++];
            }
            // if the number of consecutive empty rowset reach the limit,
            // and there are still rowsets following them
            if (empty_rowsets->size() >= limit && next_index < len) {
                return;
            } else {
                // current rowset is not empty, start searching from that rowset in the next
                i = next_index - 1;
                empty_rowsets->clear();
            }
        }
    }
}

Status BaseTablet::calc_file_crc(uint32_t* crc_value, int64_t start_version, int64_t end_version,
                                 uint32_t* rowset_count, int64_t* file_count) {
    Version v(start_version, end_version);
    std::vector<RowsetSharedPtr> rowsets;
    traverse_rowsets([&rowsets, &v](const auto& rs) {
        // get all rowsets
        if (v.contains(rs->version())) {
            rowsets.emplace_back(rs);
        }
    });
    std::sort(rowsets.begin(), rowsets.end(), Rowset::comparator);
    *rowset_count = cast_set<uint32_t>(rowsets.size());

    *crc_value = 0;
    *file_count = 0;
    for (const auto& rs : rowsets) {
        uint32_t rs_crc_value = 0;
        int64_t rs_file_count = 0;
        auto rowset = std::static_pointer_cast<BetaRowset>(rs);
        auto st = rowset->calc_file_crc(&rs_crc_value, &rs_file_count);
        if (!st.ok()) {
            return st;
        }
        // crc_value is calculated based on the crc_value of each rowset.
        *crc_value = crc32c::Extend(*crc_value, reinterpret_cast<const char*>(&rs_crc_value),
                                    sizeof(rs_crc_value));
        *file_count += rs_file_count;
    }
    return Status::OK();
}

Status BaseTablet::show_nested_index_file(std::string* json_meta) {
    Version v(0, max_version_unlocked());
    std::vector<RowsetSharedPtr> rowsets;
    traverse_rowsets([&rowsets, &v](const auto& rs) {
        // get all rowsets
        if (v.contains(rs->version())) {
            rowsets.emplace_back(rs);
        }
    });
    std::sort(rowsets.begin(), rowsets.end(), Rowset::comparator);

    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
    rapidjson::Value tabletIdValue(tablet_id());
    doc.AddMember("tablet_id", tabletIdValue, allocator);

    rapidjson::Value rowsets_value(rapidjson::kArrayType);

    for (const auto& rs : rowsets) {
        rapidjson::Value rowset_value(rapidjson::kObjectType);

        auto rowset = std::static_pointer_cast<BetaRowset>(rs);
        RETURN_IF_ERROR(rowset->show_nested_index_file(&rowset_value, allocator));
        rowsets_value.PushBack(rowset_value, allocator);
    }
    doc.AddMember("rowsets", rowsets_value, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    *json_meta = std::string(buffer.GetString());

    return Status::OK();
}

void BaseTablet::get_base_rowset_delete_bitmap_count(
        uint64_t* max_base_rowset_delete_bitmap_score,
        int64_t* max_base_rowset_delete_bitmap_score_tablet_id) {
    std::vector<RowsetSharedPtr> rowsets_;
    std::string base_rowset_id_str;
    {
        std::shared_lock rowset_ldlock(this->get_header_lock());
        for (const auto& it : _rs_version_map) {
            rowsets_.emplace_back(it.second);
        }
    }
    std::sort(rowsets_.begin(), rowsets_.end(), Rowset::comparator);
    if (!rowsets_.empty()) {
        bool base_found = false;
        for (auto& rowset : rowsets_) {
            if (rowset->start_version() > 2) {
                break;
            }
            base_found = true;
            uint64_t base_rowset_delete_bitmap_count =
                    this->tablet_meta()->delete_bitmap().get_count_with_range(
                            {rowset->rowset_id(), 0, 0},
                            {rowset->rowset_id(), UINT32_MAX, UINT64_MAX});
            if (base_rowset_delete_bitmap_count > *max_base_rowset_delete_bitmap_score) {
                *max_base_rowset_delete_bitmap_score = base_rowset_delete_bitmap_count;
                *max_base_rowset_delete_bitmap_score_tablet_id = this->tablet_id();
            }
        }
        if (!base_found) {
            LOG(WARNING) << "can not found base rowset for tablet " << tablet_id();
        }
    }
}

int32_t BaseTablet::max_version_config() {
    int32_t max_version = tablet_meta()->compaction_policy() == CUMULATIVE_TIME_SERIES_POLICY
                                  ? std::max(config::time_series_max_tablet_version_num,
                                             config::max_tablet_version_num)
                                  : config::max_tablet_version_num;
    return max_version;
}

} // namespace doris
