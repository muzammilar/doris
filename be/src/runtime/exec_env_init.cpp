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

// IWYU pragma: no_include <bthread/errno.h>
#include <common/multi_version.h>
#include <gen_cpp/HeartbeatService_types.h>
#include <gen_cpp/Metrics_types.h>
#include <simdjson.h>
#include <sys/resource.h>

#include <cerrno> // IWYU pragma: keep
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "cloud/cloud_storage_engine.h"
#include "cloud/cloud_stream_load_executor.h"
#include "cloud/cloud_tablet_hotspot.h"
#include "cloud/cloud_warm_up_manager.h"
#include "cloud/config.h"
#include "common/cast_set.h"
#include "common/config.h"
#include "common/kerberos/kerberos_ticket_mgr.h"
#include "common/logging.h"
#include "common/status.h"
#include "io/cache/block_file_cache.h"
#include "io/cache/block_file_cache_downloader.h"
#include "io/cache/block_file_cache_factory.h"
#include "io/cache/fs_file_cache_storage.h"
#include "io/fs/file_meta_cache.h"
#include "io/fs/local_file_reader.h"
#include "olap/id_manager.h"
#include "olap/memtable_memory_limiter.h"
#include "olap/olap_define.h"
#include "olap/options.h"
#include "olap/page_cache.h"
#include "olap/rowset/segment_v2/inverted_index_cache.h"
#include "olap/schema_cache.h"
#include "olap/segment_loader.h"
#include "olap/storage_engine.h"
#include "olap/tablet_column_object_pool.h"
#include "olap/tablet_meta.h"
#include "olap/tablet_schema_cache.h"
#include "olap/wal/wal_manager.h"
#include "pipeline/pipeline_tracing.h"
#include "pipeline/query_cache/query_cache.h"
#include "pipeline/task_queue.h"
#include "pipeline/task_scheduler.h"
#include "runtime/broker_mgr.h"
#include "runtime/cache/result_cache.h"
#include "runtime/client_cache.h"
#include "runtime/exec_env.h"
#include "runtime/external_scan_context_mgr.h"
#include "runtime/fragment_mgr.h"
#include "runtime/group_commit_mgr.h"
#include "runtime/heartbeat_flags.h"
#include "runtime/index_policy/index_policy_mgr.h"
#include "runtime/load_channel_mgr.h"
#include "runtime/load_path_mgr.h"
#include "runtime/load_stream_mgr.h"
#include "runtime/memory/cache_manager.h"
#include "runtime/memory/heap_profiler.h"
#include "runtime/memory/mem_tracker.h"
#include "runtime/memory/mem_tracker_limiter.h"
#include "runtime/memory/thread_mem_tracker_mgr.h"
#include "runtime/process_profile.h"
#include "runtime/result_buffer_mgr.h"
#include "runtime/result_queue_mgr.h"
#include "runtime/routine_load/routine_load_task_executor.h"
#include "runtime/runtime_query_statistics_mgr.h"
#include "runtime/small_file_mgr.h"
#include "runtime/stream_load/new_load_stream_mgr.h"
#include "runtime/stream_load/stream_load_executor.h"
#include "runtime/thread_context.h"
#include "runtime/user_function_cache.h"
#include "runtime/workload_group/workload_group_manager.h"
#include "runtime/workload_management/workload_sched_policy_mgr.h"
#include "service/backend_options.h"
#include "service/backend_service.h"
#include "service/point_query_executor.h"
#include "util/bfd_parser.h"
#include "util/bit_util.h"
#include "util/brpc_client_cache.h"
#include "util/cpu_info.h"
#include "util/disk_info.h"
#include "util/dns_cache.h"
#include "util/doris_metrics.h"
#include "util/mem_info.h"
#include "util/metrics.h"
#include "util/parse_util.h"
#include "util/pretty_printer.h"
#include "util/threadpool.h"
#include "util/thrift_rpc_helper.h"
#include "util/timezone_utils.h"
#include "vec/exec/format/orc/orc_memory_pool.h"
#include "vec/exec/format/parquet/arrow_memory_pool.h"
#include "vec/exec/scan/scanner_scheduler.h"
#include "vec/functions/dictionary_factory.h"
#include "vec/runtime/vdata_stream_mgr.h"
#include "vec/sink/delta_writer_v2_pool.h"
#include "vec/sink/load_stream_map_pool.h"
#include "vec/spill/spill_stream_manager.h"
// clang-format off
// this must after util/brpc_client_cache.h
// /doris/thirdparty/installed/include/brpc/errno.pb.h:69:3: error: expected identifier
//  EINTERNAL = 2001,
//   ^
//  /doris/thirdparty/installed/include/hadoop_hdfs/hdfs.h:61:19: note: expanded from macro 'EINTERNAL'
//  #define EINTERNAL 255
#include "io/fs/hdfs/hdfs_mgr.h"
// clang-format on

namespace doris {

#include "common/compile_check_begin.h"
class PBackendService_Stub;
class PFunctionService_Stub;

static void init_doris_metrics(const std::vector<StorePath>& store_paths) {
    bool init_system_metrics = config::enable_system_metrics;
    std::set<std::string> disk_devices;
    std::vector<std::string> network_interfaces;
    std::vector<std::string> paths;
    for (const auto& store_path : store_paths) {
        paths.emplace_back(store_path.path);
    }
    if (init_system_metrics) {
        auto st = DiskInfo::get_disk_devices(paths, &disk_devices);
        if (!st.ok()) {
            LOG(WARNING) << "get disk devices failed, status=" << st;
            return;
        }
        st = get_inet_interfaces(&network_interfaces, BackendOptions::is_bind_ipv6());
        if (!st.ok()) {
            LOG(WARNING) << "get inet interfaces failed, status=" << st;
            return;
        }
    }
    DorisMetrics::instance()->initialize(init_system_metrics, disk_devices, network_interfaces);
}

// Used to calculate the num of min thread and max thread based on the passed config
static std::pair<size_t, size_t> get_num_threads(size_t min_num, size_t max_num) {
    auto num_cores = doris::CpuInfo::num_cores();
    min_num = (min_num == 0) ? num_cores : min_num;
    max_num = (max_num == 0) ? num_cores : max_num;
    auto factor = max_num / min_num;
    min_num = std::min(num_cores * factor, min_num);
    max_num = std::min(min_num * factor, max_num);
    return {min_num, max_num};
}

ThreadPool* ExecEnv::non_block_close_thread_pool() {
    return _non_block_close_thread_pool.get();
}

ExecEnv::ExecEnv() = default;

ExecEnv::~ExecEnv() {
    destroy();
}

Status ExecEnv::init(ExecEnv* env, const std::vector<StorePath>& store_paths,
                     const std::vector<StorePath>& spill_store_paths,
                     const std::set<std::string>& broken_paths) {
    return env->_init(store_paths, spill_store_paths, broken_paths);
}

// pick simdjson implementation based on CPU capabilities
inline void init_simdjson_parser() {
    // haswell: AVX2 (2013 Intel Haswell or later, all AMD Zen processors)
    const auto* haswell_implementation = simdjson::get_available_implementations()["haswell"];
    if (!haswell_implementation || !haswell_implementation->supported_by_runtime_system()) {
        // pick available implementation
        for (const auto* implementation : simdjson::get_available_implementations()) {
            if (implementation->supported_by_runtime_system()) {
                LOG(INFO) << "Using SimdJSON implementation : " << implementation->name() << ": "
                          << implementation->description();
                simdjson::get_active_implementation() = implementation;
                return;
            }
        }
        LOG(WARNING) << "No available SimdJSON implementation found.";
    } else {
        LOG(INFO) << "Using SimdJSON Haswell implementation";
    }
}

Status ExecEnv::_init(const std::vector<StorePath>& store_paths,
                      const std::vector<StorePath>& spill_store_paths,
                      const std::set<std::string>& broken_paths) {
    //Only init once before be destroyed
    if (ready()) {
        return Status::OK();
    }
    std::unordered_map<std::string, std::unique_ptr<vectorized::SpillDataDir>> spill_store_map;
    for (const auto& spill_path : spill_store_paths) {
        spill_store_map.emplace(spill_path.path, std::make_unique<vectorized::SpillDataDir>(
                                                         spill_path.path, spill_path.capacity_bytes,
                                                         spill_path.storage_medium));
    }
    init_doris_metrics(store_paths);
    _store_paths = store_paths;
    _tmp_file_dirs = std::make_unique<segment_v2::TmpFileDirs>(_store_paths);
    RETURN_IF_ERROR(_tmp_file_dirs->init());
    _user_function_cache = new UserFunctionCache();
    static_cast<void>(_user_function_cache->init(doris::config::user_function_dir));
    _external_scan_context_mgr = new ExternalScanContextMgr(this);
    set_stream_mgr(new doris::vectorized::VDataStreamMgr());
    _result_mgr = new ResultBufferMgr();
    _result_queue_mgr = new ResultQueueMgr();
    _backend_client_cache = new BackendServiceClientCache(config::max_client_cache_size_per_host);
    _frontend_client_cache = new FrontendServiceClientCache(config::max_client_cache_size_per_host);
    _broker_client_cache = new BrokerServiceClientCache(config::max_client_cache_size_per_host);

    TimezoneUtils::load_timezones_to_cache();

    static_cast<void>(ThreadPoolBuilder("SendBatchThreadPool")
                              .set_min_threads(config::send_batch_thread_pool_thread_num)
                              .set_max_threads(config::send_batch_thread_pool_thread_num)
                              .set_max_queue_size(config::send_batch_thread_pool_queue_size)
                              .build(&_send_batch_thread_pool));

    auto [buffered_reader_min_threads, buffered_reader_max_threads] =
            get_num_threads(config::num_buffered_reader_prefetch_thread_pool_min_thread,
                            config::num_buffered_reader_prefetch_thread_pool_max_thread);
    static_cast<void>(ThreadPoolBuilder("BufferedReaderPrefetchThreadPool")
                              .set_min_threads(cast_set<int>(buffered_reader_min_threads))
                              .set_max_threads(cast_set<int>(buffered_reader_max_threads))
                              .build(&_buffered_reader_prefetch_thread_pool));

    static_cast<void>(ThreadPoolBuilder("SendTableStatsThreadPool")
                              .set_min_threads(8)
                              .set_max_threads(32)
                              .build(&_send_table_stats_thread_pool));

    auto [s3_file_upload_min_threads, s3_file_upload_max_threads] =
            get_num_threads(config::num_s3_file_upload_thread_pool_min_thread,
                            config::num_s3_file_upload_thread_pool_max_thread);
    static_cast<void>(ThreadPoolBuilder("S3FileUploadThreadPool")
                              .set_min_threads(cast_set<int>(s3_file_upload_min_threads))
                              .set_max_threads(cast_set<int>(s3_file_upload_max_threads))
                              .build(&_s3_file_upload_thread_pool));

    // min num equal to fragment pool's min num
    // max num is useless because it will start as many as requested in the past
    // queue size is useless because the max thread num is very large
    static_cast<void>(ThreadPoolBuilder("LazyReleaseMemoryThreadPool")
                              .set_min_threads(1)
                              .set_max_threads(1)
                              .set_max_queue_size(1000000)
                              .build(&_lazy_release_obj_pool));
    static_cast<void>(ThreadPoolBuilder("NonBlockCloseThreadPool")
                              .set_min_threads(cast_set<int>(config::min_nonblock_close_thread_num))
                              .set_max_threads(cast_set<int>(config::max_nonblock_close_thread_num))
                              .build(&_non_block_close_thread_pool));
    static_cast<void>(ThreadPoolBuilder("S3FileSystemThreadPool")
                              .set_min_threads(config::min_s3_file_system_thread_num)
                              .set_max_threads(config::max_s3_file_system_thread_num)
                              .build(&_s3_file_system_thread_pool));
    RETURN_IF_ERROR(_init_mem_env());

    // NOTE: runtime query statistics mgr could be visited by query and daemon thread
    // so it should be created before all query begin and deleted after all query and daemon thread stoppped
    _runtime_query_statistics_mgr = new RuntimeQueryStatisticsMgr();
    CgroupCpuCtl::init_doris_cgroup_path();
    _file_cache_open_fd_cache = std::make_unique<io::FDCache>();
    _file_cache_factory = new io::FileCacheFactory();
    std::vector<doris::CachePath> cache_paths;
    init_file_cache_factory(cache_paths);
    doris::io::BeConfDataDirReader::init_be_conf_data_dir(store_paths, spill_store_paths,
                                                          cache_paths);
    _pipeline_tracer_ctx = std::make_unique<pipeline::PipelineTracerContext>(); // before query
    _init_runtime_filter_timer_queue();

    _workload_group_manager = new WorkloadGroupMgr();
    _scanner_scheduler = new doris::vectorized::ScannerScheduler();

    _fragment_mgr = new FragmentMgr(this);
    _result_cache = new ResultCache(config::query_cache_max_size_mb,
                                    config::query_cache_elasticity_size_mb);
    _cluster_info = new ClusterInfo();
    _load_path_mgr = new LoadPathMgr(this);
    _bfd_parser = BfdParser::create();
    _broker_mgr = new BrokerMgr(this);
    _load_channel_mgr = new LoadChannelMgr();
    auto num_flush_threads = std::min(
            _store_paths.size() * config::flush_thread_num_per_store,
            static_cast<size_t>(CpuInfo::num_cores()) * config::max_flush_thread_num_per_cpu);
    _load_stream_mgr = std::make_unique<LoadStreamMgr>(num_flush_threads);
    _new_load_stream_mgr = NewLoadStreamMgr::create_unique();
    _internal_client_cache = new BrpcClientCache<PBackendService_Stub>();
    _streaming_client_cache =
            new BrpcClientCache<PBackendService_Stub>("baidu_std", "single", "streaming");
    _function_client_cache =
            new BrpcClientCache<PFunctionService_Stub>(config::function_service_protocol);
    if (config::is_cloud_mode()) {
        _stream_load_executor = CloudStreamLoadExecutor::create_unique(this);
    } else {
        _stream_load_executor = StreamLoadExecutor::create_unique(this);
    }
    _routine_load_task_executor = new RoutineLoadTaskExecutor(this);
    RETURN_IF_ERROR(_routine_load_task_executor->init(MemInfo::mem_limit()));
    _small_file_mgr = new SmallFileMgr(this, config::small_file_dir);
    _group_commit_mgr = new GroupCommitMgr(this);
    _memtable_memory_limiter = std::make_unique<MemTableMemoryLimiter>();
    _load_stream_map_pool = std::make_unique<LoadStreamMapPool>();
    _delta_writer_v2_pool = std::make_unique<vectorized::DeltaWriterV2Pool>();
    _wal_manager = WalManager::create_unique(this, config::group_commit_wal_path);
    _dns_cache = new DNSCache();
    _write_cooldown_meta_executors = std::make_unique<WriteCooldownMetaExecutors>();
    _spill_stream_mgr = new vectorized::SpillStreamManager(std::move(spill_store_map));
    _kerberos_ticket_mgr = new kerberos::KerberosTicketMgr(config::kerberos_ccache_path);
    _hdfs_mgr = new io::HdfsMgr();
    _backend_client_cache->init_metrics("backend");
    _frontend_client_cache->init_metrics("frontend");
    _broker_client_cache->init_metrics("broker");
    static_cast<void>(_result_mgr->init());
    Status status = _load_path_mgr->init();
    if (!status.ok()) {
        LOG(ERROR) << "Load path mgr init failed. " << status;
        return status;
    }
    _broker_mgr->init();
    static_cast<void>(_small_file_mgr->init());
    status = _scanner_scheduler->init(this);
    if (!status.ok()) {
        LOG(ERROR) << "Scanner scheduler init failed. " << status;
        return status;
    }

    RETURN_IF_ERROR(_memtable_memory_limiter->init(MemInfo::mem_limit()));
    RETURN_IF_ERROR(_load_channel_mgr->init(MemInfo::mem_limit()));
    RETURN_IF_ERROR(_wal_manager->init());
    _heartbeat_flags = new HeartbeatFlags();

    _tablet_schema_cache =
            TabletSchemaCache::create_global_schema_cache(config::tablet_schema_cache_capacity);

    _tablet_column_object_pool = TabletColumnObjectPool::create_global_column_cache(
            config::tablet_schema_cache_capacity);

    // Storage engine
    doris::EngineOptions options;
    options.store_paths = store_paths;
    options.broken_paths = broken_paths;
    options.backend_uid = doris::UniqueId::gen_uid();
    // Check if the startup mode has been modified
    RETURN_IF_ERROR(_check_deploy_mode());
    if (config::is_cloud_mode()) {
        std::cout << "start BE in cloud mode, cloud_unique_id: " << config::cloud_unique_id
                  << ", meta_service_endpoint: " << config::meta_service_endpoint << std::endl;
        _storage_engine = std::make_unique<CloudStorageEngine>(options);
    } else {
        std::cout << "start BE in local mode" << std::endl;
        _storage_engine = std::make_unique<StorageEngine>(options);
    }
    auto st = _storage_engine->open();
    if (!st.ok()) {
        LOG(ERROR) << "Fail to open StorageEngine, res=" << st;
        return st;
    }
    _storage_engine->set_heartbeat_flags(this->heartbeat_flags());
    if (st = _storage_engine->start_bg_threads(nullptr); !st.ok()) {
        LOG(ERROR) << "Failed to starge bg threads of storage engine, res=" << st;
        return st;
    }

    // create internal workload group should be after storage_engin->open()
    RETURN_IF_ERROR(_create_internal_workload_group());
    _workload_sched_mgr = new WorkloadSchedPolicyMgr();
    _workload_sched_mgr->start(this);

    _index_policy_mgr = new IndexPolicyMgr();

    RETURN_IF_ERROR(_spill_stream_mgr->init());
    RETURN_IF_ERROR(_runtime_query_statistics_mgr->start_report_thread());
    _dict_factory = new doris::vectorized::DictionaryFactory();
    _s_ready = true;

    init_simdjson_parser();

    // Make aws-sdk-cpp InitAPI and ShutdownAPI called in the same thread
    S3ClientFactory::instance();
    return Status::OK();
}

// when user not sepcify a workload group in FE, then query could
// use dummy workload group.
Status ExecEnv::_create_internal_workload_group() {
    LOG(INFO) << "begin create internal workload group.";

    RETURN_IF_ERROR(_workload_group_manager->create_internal_wg());
    return Status::OK();
}

void ExecEnv::_init_runtime_filter_timer_queue() {
    _runtime_filter_timer_queue = new doris::pipeline::RuntimeFilterTimerQueue();
    _runtime_filter_timer_queue->run();
}

void ExecEnv::init_file_cache_factory(std::vector<doris::CachePath>& cache_paths) {
    // Load file cache before starting up daemon threads to make sure StorageEngine is read.
    if (!config::enable_file_cache) {
        if (config::is_cloud_mode()) {
            LOG(FATAL) << "Cloud mode requires to enable file cache, plz set "
                          "config::enable_file_cache "
                          "= true";
            exit(-1);
        }
        return;
    }
    if (config::file_cache_each_block_size > config::s3_write_buffer_size ||
        config::s3_write_buffer_size % config::file_cache_each_block_size != 0) {
        LOG_FATAL(
                "The config file_cache_each_block_size {} must less than or equal to config "
                "s3_write_buffer_size {} and config::s3_write_buffer_size % "
                "config::file_cache_each_block_size must be zero",
                config::file_cache_each_block_size, config::s3_write_buffer_size);
        exit(-1);
    }
    std::unordered_set<std::string> cache_path_set;
    Status rest = doris::parse_conf_cache_paths(doris::config::file_cache_path, cache_paths);
    if (!rest) {
        throw Exception(
                Status::FatalError("parse config file cache path failed, path={}, reason={}",
                                   doris::config::file_cache_path, rest.msg()));
    }

    doris::Status cache_status;
    for (auto& cache_path : cache_paths) {
        if (cache_path_set.find(cache_path.path) != cache_path_set.end()) {
            LOG(WARNING) << fmt::format("cache path {} is duplicate", cache_path.path);
            continue;
        }

        cache_status = doris::io::FileCacheFactory::instance()->create_file_cache(
                cache_path.path, cache_path.init_settings());
        if (!cache_status.ok()) {
            if (!doris::config::ignore_broken_disk) {
                throw Exception(
                        Status::FatalError("failed to init file cache, err: {}", cache_status));
            }
            LOG(WARNING) << "failed to init file cache, err: " << cache_status;
        }
        cache_path_set.emplace(cache_path.path);
    }
}

Status ExecEnv::_init_mem_env() {
    bool is_percent = false;
    std::stringstream ss;
    // 1. init mem tracker
    _process_profile = ProcessProfile::create_global_instance();
    _heap_profiler = HeapProfiler::create_global_instance();
    init_mem_tracker();
    thread_context()->thread_mem_tracker_mgr->init();

    if (!BitUtil::IsPowerOf2(config::min_buffer_size)) {
        ss << "Config min_buffer_size must be a power-of-two: " << config::min_buffer_size;
        return Status::InternalError(ss.str());
    }

    _id_manager = new IdManager();
    _cache_manager = CacheManager::create_global_instance();

    int64_t storage_cache_limit =
            ParseUtil::parse_mem_spec(config::storage_page_cache_limit, MemInfo::mem_limit(),
                                      MemInfo::physical_mem(), &is_percent);
    while (!is_percent && storage_cache_limit > MemInfo::mem_limit() / 2) {
        storage_cache_limit = storage_cache_limit / 2;
    }
    int32_t index_percentage = config::index_page_cache_percentage;
    int32_t num_shards = config::storage_page_cache_shard_size;
    if ((num_shards & (num_shards - 1)) != 0) {
        int old_num_shards = num_shards;
        num_shards = cast_set<int>(BitUtil::RoundUpToPowerOfTwo(num_shards));
        LOG(WARNING) << "num_shards should be power of two, but got " << old_num_shards
                     << ". Rounded up to " << num_shards
                     << ". Please modify the 'storage_page_cache_shard_size' parameter in your "
                        "conf file to be a power of two for better performance.";
    }
    if (storage_cache_limit < num_shards * 2) {
        LOG(WARNING) << "storage_cache_limit(" << storage_cache_limit << ") less than num_shards("
                     << num_shards
                     << ") * 2, cache capacity will be 0, continuing to use "
                        "cache will only have negative effects, will be disabled.";
    }
    int64_t pk_storage_page_cache_limit =
            ParseUtil::parse_mem_spec(config::pk_storage_page_cache_limit, MemInfo::mem_limit(),
                                      MemInfo::physical_mem(), &is_percent);
    while (!is_percent && pk_storage_page_cache_limit > MemInfo::mem_limit() / 2) {
        pk_storage_page_cache_limit = storage_cache_limit / 2;
    }
    _storage_page_cache = StoragePageCache::create_global_cache(
            storage_cache_limit, index_percentage, pk_storage_page_cache_limit, num_shards);
    LOG(INFO) << "Storage page cache memory limit: "
              << PrettyPrinter::print(storage_cache_limit, TUnit::BYTES)
              << ", origin config value: " << config::storage_page_cache_limit;

    // Init row cache
    int64_t row_cache_mem_limit =
            ParseUtil::parse_mem_spec(config::row_cache_mem_limit, MemInfo::mem_limit(),
                                      MemInfo::physical_mem(), &is_percent);
    while (!is_percent && row_cache_mem_limit > MemInfo::mem_limit() / 2) {
        // Reason same as buffer_pool_limit
        row_cache_mem_limit = row_cache_mem_limit / 2;
    }
    _row_cache = RowCache::create_global_cache(row_cache_mem_limit);
    LOG(INFO) << "Row cache memory limit: "
              << PrettyPrinter::print(row_cache_mem_limit, TUnit::BYTES)
              << ", origin config value: " << config::row_cache_mem_limit;

    uint64_t fd_number = config::min_file_descriptor_number;
    struct rlimit l;
    int ret = getrlimit(RLIMIT_NOFILE, &l);
    if (ret != 0) {
        LOG(WARNING) << "call getrlimit() failed. errno=" << strerror(errno)
                     << ", use default configuration instead.";
    } else {
        fd_number = static_cast<uint64_t>(l.rlim_cur);
    }
    // SegmentLoader caches segments in rowset granularity. So the size of
    // opened files will greater than segment_cache_capacity.
    int64_t segment_cache_capacity = config::segment_cache_capacity;
    int64_t segment_cache_fd_limit = fd_number / 100 * config::segment_cache_fd_percentage;
    if (segment_cache_capacity < 0 || segment_cache_capacity > segment_cache_fd_limit) {
        segment_cache_capacity = segment_cache_fd_limit;
    }

    int64_t segment_cache_mem_limit =
            MemInfo::mem_limit() / 100 * config::segment_cache_memory_percentage;

    _segment_loader = new SegmentLoader(segment_cache_mem_limit, segment_cache_capacity);
    LOG(INFO) << "segment_cache_capacity <= fd_number * 1 / 5, fd_number: " << fd_number
              << " segment_cache_capacity: " << segment_cache_capacity
              << " min_segment_cache_mem_limit " << segment_cache_mem_limit;

    _schema_cache = new SchemaCache(config::schema_cache_capacity);

    size_t block_file_cache_fd_cache_size =
            std::min((uint64_t)config::file_cache_max_file_reader_cache_size, fd_number / 3);
    LOG(INFO) << "max file reader cache size is: " << block_file_cache_fd_cache_size
              << ", resource hard limit is: " << fd_number
              << ", config file_cache_max_file_reader_cache_size is: "
              << config::file_cache_max_file_reader_cache_size;
    config::file_cache_max_file_reader_cache_size = block_file_cache_fd_cache_size;

    _file_meta_cache = new FileMetaCache(config::max_external_file_meta_cache_num);

    _lookup_connection_cache =
            LookupConnectionCache::create_global_instance(config::lookup_connection_cache_capacity);

    // use memory limit
    int64_t inverted_index_cache_limit =
            ParseUtil::parse_mem_spec(config::inverted_index_searcher_cache_limit,
                                      MemInfo::mem_limit(), MemInfo::physical_mem(), &is_percent);
    while (!is_percent && inverted_index_cache_limit > MemInfo::mem_limit() / 2) {
        // Reason same as buffer_pool_limit
        inverted_index_cache_limit = inverted_index_cache_limit / 2;
    }
    _inverted_index_searcher_cache =
            InvertedIndexSearcherCache::create_global_instance(inverted_index_cache_limit, 256);
    LOG(INFO) << "Inverted index searcher cache memory limit: "
              << PrettyPrinter::print(inverted_index_cache_limit, TUnit::BYTES)
              << ", origin config value: " << config::inverted_index_searcher_cache_limit;

    // use memory limit
    int64_t inverted_index_query_cache_limit =
            ParseUtil::parse_mem_spec(config::inverted_index_query_cache_limit,
                                      MemInfo::mem_limit(), MemInfo::physical_mem(), &is_percent);
    while (!is_percent && inverted_index_query_cache_limit > MemInfo::mem_limit() / 2) {
        // Reason same as buffer_pool_limit
        inverted_index_query_cache_limit = inverted_index_query_cache_limit / 2;
    }
    _inverted_index_query_cache = InvertedIndexQueryCache::create_global_cache(
            inverted_index_query_cache_limit, config::inverted_index_query_cache_shards);
    LOG(INFO) << "Inverted index query match cache memory limit: "
              << PrettyPrinter::print(inverted_index_cache_limit, TUnit::BYTES)
              << ", origin config value: " << config::inverted_index_query_cache_limit;

    // init orc memory pool
    _orc_memory_pool = new doris::vectorized::ORCMemoryPool();
    _arrow_memory_pool = new doris::vectorized::ArrowMemoryPool();

    _query_cache = QueryCache::create_global_cache(config::query_cache_size * 1024L * 1024L);
    LOG(INFO) << "query cache memory limit: " << config::query_cache_size << "MB";

    // The default delete bitmap cache is set to 100MB,
    // which can be insufficient and cause performance issues when the amount of user data is large.
    // To mitigate the problem of an inadequate cache,
    // we will take the larger of 0.5% of the total memory and 100MB as the delete bitmap cache size.
    int64_t delete_bitmap_agg_cache_cache_limit =
            ParseUtil::parse_mem_spec(config::delete_bitmap_dynamic_agg_cache_limit,
                                      MemInfo::mem_limit(), MemInfo::physical_mem(), &is_percent);
    _delete_bitmap_agg_cache = DeleteBitmapAggCache::create_instance(std::max(
            delete_bitmap_agg_cache_cache_limit, config::delete_bitmap_agg_cache_capacity));

    return Status::OK();
}

void ExecEnv::init_mem_tracker() {
    mem_tracker_limiter_pool.resize(MEM_TRACKER_GROUP_NUM,
                                    TrackerLimiterGroup()); // before all mem tracker init.
    _s_tracking_memory = true;
    _orphan_mem_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "Orphan");
    _brpc_iobuf_block_memory_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "IOBufBlockMemory");
    _segcompaction_mem_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::COMPACTION, "SegCompaction");
    _tablets_no_cache_mem_tracker = MemTrackerLimiter::create_shared(
            MemTrackerLimiter::Type::METADATA, "Tablets(not in SchemaCache, TabletSchemaCache)");
    _segments_no_cache_mem_tracker = MemTrackerLimiter::create_shared(
            MemTrackerLimiter::Type::METADATA, "Segments(not in SegmentCache)");
    _rowsets_no_cache_mem_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::METADATA, "Rowsets");
    _point_query_executor_mem_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "PointQueryExecutor");
    _query_cache_mem_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::CACHE, "QueryCache");
    _block_compression_mem_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "BlockCompression");
    _rowid_storage_reader_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "RowIdStorageReader");
    _subcolumns_tree_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "SubcolumnsTree");
    _s3_file_buffer_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::GLOBAL, "S3FileBuffer");
    _stream_load_pipe_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::LOAD, "StreamLoadPipe");
    _parquet_meta_tracker =
            MemTrackerLimiter::create_shared(MemTrackerLimiter::Type::METADATA, "ParquetMeta");
}

Status ExecEnv::_check_deploy_mode() {
    for (auto _path : _store_paths) {
        auto deploy_mode_path = fmt::format("{}/{}", _path.path, DEPLOY_MODE_PREFIX);
        std::string expected_mode = doris::config::is_cloud_mode() ? "cloud" : "local";
        bool exists = false;
        RETURN_IF_ERROR(io::global_local_filesystem()->exists(deploy_mode_path, &exists));
        if (exists) {
            // check if is ok
            io::FileReaderSPtr reader;
            RETURN_IF_ERROR(io::global_local_filesystem()->open_file(deploy_mode_path, &reader));
            size_t fsize = reader->size();
            if (fsize > 0) {
                std::string actual_mode;
                actual_mode.resize(fsize, '\0');
                size_t bytes_read = 0;
                RETURN_IF_ERROR(reader->read_at(0, {actual_mode.data(), fsize}, &bytes_read));
                DCHECK_EQ(fsize, bytes_read);
                if (expected_mode != actual_mode) {
                    return Status::InternalError(
                            "You can't switch deploy mode from {} to {}, "
                            "maybe you need to check be.conf\n",
                            actual_mode.c_str(), expected_mode.c_str());
                }
                LOG(INFO) << "The current deployment mode is " << expected_mode << ".";
            }
        } else {
            io::FileWriterPtr file_writer;
            RETURN_IF_ERROR(
                    io::global_local_filesystem()->create_file(deploy_mode_path, &file_writer));
            RETURN_IF_ERROR(file_writer->append(expected_mode));
            RETURN_IF_ERROR(file_writer->close());
            LOG(INFO) << "The file deploy_mode doesn't exist, create it.";
            auto cluster_id_path = fmt::format("{}/{}", _path.path, CLUSTER_ID_PREFIX);
            RETURN_IF_ERROR(io::global_local_filesystem()->exists(cluster_id_path, &exists));
            if (exists) {
                LOG(WARNING) << "This may be an upgrade from old version,"
                             << "or the deploy_mode file has been manually deleted";
            }
        }
    }
    return Status::OK();
}

#ifdef BE_TEST
void ExecEnv::set_new_load_stream_mgr(std::unique_ptr<NewLoadStreamMgr>&& new_load_stream_mgr) {
    this->_new_load_stream_mgr = std::move(new_load_stream_mgr);
}

void ExecEnv::clear_new_load_stream_mgr() {
    this->_new_load_stream_mgr.reset();
}

void ExecEnv::set_stream_load_executor(std::unique_ptr<StreamLoadExecutor>&& stream_load_executor) {
    this->_stream_load_executor = std::move(stream_load_executor);
}

void ExecEnv::clear_stream_load_executor() {
    this->_stream_load_executor.reset();
}

void ExecEnv::set_wal_mgr(std::unique_ptr<WalManager>&& wm) {
    this->_wal_manager = std::move(wm);
}
void ExecEnv::clear_wal_mgr() {
    this->_wal_manager.reset();
}
#endif
// TODO(zhiqiang): Need refactor all thread pool. Each thread pool must have a Stop method.
// We need to stop all threads before releasing resource.
void ExecEnv::destroy() {
    //Only destroy once after init
    if (!ready()) {
        return;
    }
    // Memory barrier to prevent other threads from accessing destructed resources
    _s_ready = false;

    SAFE_STOP(_wal_manager);
    _wal_manager.reset();
    SAFE_STOP(_load_channel_mgr);
    SAFE_STOP(_scanner_scheduler);
    SAFE_STOP(_broker_mgr);
    SAFE_STOP(_load_path_mgr);
    SAFE_STOP(_result_mgr);
    SAFE_STOP(_group_commit_mgr);
    // _routine_load_task_executor should be stopped before _new_load_stream_mgr.
    SAFE_STOP(_routine_load_task_executor);
    // stop workload scheduler
    SAFE_STOP(_workload_sched_mgr);
    // stop pipline step 2, cgroup execution
    SAFE_STOP(_workload_group_manager);

    SAFE_STOP(_external_scan_context_mgr);
    SAFE_STOP(_fragment_mgr);
    SAFE_STOP(_runtime_filter_timer_queue);
    // NewLoadStreamMgr should be destoried before storage_engine & after fragment_mgr stopped.
    _load_stream_mgr.reset();
    _new_load_stream_mgr.reset();
    _stream_load_executor.reset();
    _memtable_memory_limiter.reset();
    _delta_writer_v2_pool.reset();
    _load_stream_map_pool.reset();
    SAFE_STOP(_write_cooldown_meta_executors);

    // _id_manager must be destoried before tablet schema cache
    SAFE_DELETE(_id_manager);

    // StorageEngine must be destoried before _cache_manager destory
    SAFE_STOP(_storage_engine);
    _storage_engine.reset();

    SAFE_STOP(_spill_stream_mgr);
    if (_runtime_query_statistics_mgr) {
        _runtime_query_statistics_mgr->stop_report_thread();
    }
    SAFE_SHUTDOWN(_buffered_reader_prefetch_thread_pool);
    SAFE_SHUTDOWN(_s3_file_upload_thread_pool);
    SAFE_SHUTDOWN(_lazy_release_obj_pool);
    SAFE_SHUTDOWN(_non_block_close_thread_pool);
    SAFE_SHUTDOWN(_s3_file_system_thread_pool);
    SAFE_SHUTDOWN(_send_batch_thread_pool);
    SAFE_SHUTDOWN(_send_table_stats_thread_pool);

    SAFE_DELETE(_load_channel_mgr);

    SAFE_DELETE(_inverted_index_query_cache);
    SAFE_DELETE(_inverted_index_searcher_cache);
    SAFE_DELETE(_lookup_connection_cache);
    SAFE_DELETE(_schema_cache);
    SAFE_DELETE(_segment_loader);
    SAFE_DELETE(_row_cache);
    SAFE_DELETE(_query_cache);
    SAFE_DELETE(_delete_bitmap_agg_cache);

    // Free resource after threads are stopped.
    // Some threads are still running, like threads created by _new_load_stream_mgr ...
    SAFE_DELETE(_tablet_schema_cache);
    SAFE_DELETE(_tablet_column_object_pool);

    // _scanner_scheduler must be desotried before _storage_page_cache
    SAFE_DELETE(_scanner_scheduler);
    // _storage_page_cache must be destoried before _cache_manager
    SAFE_DELETE(_storage_page_cache);

    SAFE_DELETE(_small_file_mgr);
    SAFE_DELETE(_broker_mgr);
    SAFE_DELETE(_load_path_mgr);
    SAFE_DELETE(_result_mgr);
    SAFE_DELETE(_file_meta_cache);
    SAFE_DELETE(_group_commit_mgr);
    SAFE_DELETE(_routine_load_task_executor);
    // _stream_load_executor
    SAFE_DELETE(_function_client_cache);
    SAFE_DELETE(_streaming_client_cache);
    SAFE_DELETE(_internal_client_cache);

    SAFE_DELETE(_bfd_parser);
    SAFE_DELETE(_result_cache);
    SAFE_DELETE(_vstream_mgr);
    // When _vstream_mgr is deconstructed, it will try call query context's dctor and will
    // access spill stream mgr, so spill stream mgr should be deconstructed after data stream manager
    SAFE_DELETE(_spill_stream_mgr);
    SAFE_DELETE(_fragment_mgr);
    SAFE_DELETE(_workload_sched_mgr);
    SAFE_DELETE(_workload_group_manager);
    SAFE_DELETE(_file_cache_factory);
    SAFE_DELETE(_runtime_filter_timer_queue);
    SAFE_DELETE(_dict_factory);
    // TODO(zhiqiang): Maybe we should call shutdown before release thread pool?
    _lazy_release_obj_pool.reset(nullptr);
    _non_block_close_thread_pool.reset(nullptr);
    _s3_file_system_thread_pool.reset(nullptr);
    _send_table_stats_thread_pool.reset(nullptr);
    _buffered_reader_prefetch_thread_pool.reset(nullptr);
    _s3_file_upload_thread_pool.reset(nullptr);
    _send_batch_thread_pool.reset(nullptr);
    _write_cooldown_meta_executors.reset(nullptr);

    SAFE_DELETE(_broker_client_cache);
    SAFE_DELETE(_frontend_client_cache);
    SAFE_DELETE(_backend_client_cache);
    SAFE_DELETE(_result_queue_mgr);

    SAFE_DELETE(_external_scan_context_mgr);
    SAFE_DELETE(_user_function_cache);

    // cache_manager must be destoried after all cache.
    // https://github.com/apache/doris/issues/24082#issuecomment-1712544039
    SAFE_DELETE(_cache_manager);
    _file_cache_open_fd_cache.reset(nullptr);

    // _heartbeat_flags must be destoried after staroge engine
    SAFE_DELETE(_heartbeat_flags);

    // Master Info is a thrift object, it could be the last one to deconstruct.
    // Master info should be deconstruct later than fragment manager, because fragment will
    // access cluster_info.backend_id to access some info. If there is a running query and master
    // info is deconstructed then BE process will core at coordinator back method in fragment mgr.
    SAFE_DELETE(_cluster_info);

    // NOTE: runtime query statistics mgr could be visited by query and daemon thread
    // so it should be created before all query begin and deleted after all query and daemon thread stoppped
    SAFE_DELETE(_runtime_query_statistics_mgr);

    SAFE_DELETE(_arrow_memory_pool);

    SAFE_DELETE(_orc_memory_pool);

    // dns cache is a global instance and need to be released at last
    SAFE_DELETE(_dns_cache);
    SAFE_DELETE(_kerberos_ticket_mgr);
    SAFE_DELETE(_hdfs_mgr);

    SAFE_DELETE(_process_profile);
    SAFE_DELETE(_heap_profiler);

    SAFE_DELETE(_index_policy_mgr);

    _s_tracking_memory = false;

    LOG(INFO) << "Doris exec envorinment is destoried.";
}

} // namespace doris
