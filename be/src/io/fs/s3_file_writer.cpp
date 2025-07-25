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

#include "io/fs/s3_file_writer.h"

#include <aws/s3/model/CompletedPart.h>
#include <bvar/reducer.h>
#include <fmt/core.h>
#include <glog/logging.h>

#include <sstream>
#include <tuple>
#include <utility>

#include "common/config.h"
#include "common/status.h"
#include "cpp/sync_point.h"
#include "io/cache/block_file_cache.h"
#include "io/cache/block_file_cache_factory.h"
#include "io/cache/file_block.h"
#include "io/cache/file_cache_common.h"
#include "io/fs/file_writer.h"
#include "io/fs/path.h"
#include "io/fs/s3_file_bufferpool.h"
#include "io/fs/s3_file_system.h"
#include "io/fs/s3_obj_storage_client.h"
#include "runtime/exec_env.h"
#include "util/s3_util.h"

namespace doris::io {
#include "common/compile_check_begin.h"

bvar::Adder<uint64_t> s3_file_writer_total("s3_file_writer_total_num");
bvar::Adder<uint64_t> s3_bytes_written_total("s3_file_writer_bytes_written");
bvar::Adder<uint64_t> s3_file_created_total("s3_file_writer_file_created");
bvar::Adder<uint64_t> s3_file_being_written("s3_file_writer_file_being_written");
bvar::Adder<uint64_t> s3_file_writer_async_close_queuing("s3_file_writer_async_close_queuing");
bvar::Adder<uint64_t> s3_file_writer_async_close_processing(
        "s3_file_writer_async_close_processing");

S3FileWriter::S3FileWriter(std::shared_ptr<ObjClientHolder> client, std::string bucket,
                           std::string key, const FileWriterOptions* opts)
        : _obj_storage_path_opts({.path = fmt::format("s3://{}/{}", bucket, key),
                                  .bucket = std::move(bucket),
                                  .key = std::move(key)}),
          _used_by_s3_committer(opts ? opts->used_by_s3_committer : false),
          _obj_client(std::move(client)) {
    s3_file_writer_total << 1;
    s3_file_being_written << 1;
    Aws::Http::SetCompliantRfc3986Encoding(true);
    if (config::enable_file_cache && opts != nullptr && opts->write_file_cache) {
        _cache_builder = std::make_unique<FileCacheAllocatorBuilder>(FileCacheAllocatorBuilder {
                opts ? opts->is_cold_data : false, opts ? opts->file_cache_expiration : 0,
                BlockFileCache::hash(_obj_storage_path_opts.path.filename().native()),
                FileCacheFactory::instance()->get_by_path(
                        BlockFileCache::hash(_obj_storage_path_opts.path.filename().native()))});
    }
}

S3FileWriter::~S3FileWriter() {
    if (_async_close_pack != nullptr) {
        // For thread safety
        std::ignore = _async_close_pack->future.get();
        _async_close_pack = nullptr;
    } else {
        // Consider one situation where the file writer is destructed after it submit at least one async task
        // without calling close(), then there exists one occasion where the async task is executed right after
        // the correspoding S3 file writer is already destructed
        _wait_until_finish(fmt::format("wait s3 file {} upload to be finished",
                                       _obj_storage_path_opts.path.native()));
    }
    // We won't do S3 abort operation in BE, we let s3 service do it own.
    if (state() == State::OPENED && !_failed) {
        s3_bytes_written_total << _bytes_appended;
    }
    s3_file_being_written << -1;
}

Status S3FileWriter::_create_multi_upload_request() {
    LOG(INFO) << "create_multi_upload_request " << _obj_storage_path_opts.path.native();
    const auto& client = _obj_client->get();
    if (nullptr == client) {
        return Status::InternalError<false>("invalid obj storage client");
    }
    auto resp = client->create_multipart_upload(_obj_storage_path_opts);
    if (resp.resp.status.code == ErrorCode::OK) {
        _obj_storage_path_opts.upload_id = resp.upload_id;
    }
    return {resp.resp.status.code, std::move(resp.resp.status.msg)};
}

void S3FileWriter::_wait_until_finish(std::string_view task_name) {
    auto timeout_duration = config::s3_file_writer_log_interval_second;
    auto msg = fmt::format(
            "{} multipart upload already takes {} seconds, bucket={}, key={}, upload_id={}",
            task_name, timeout_duration, _obj_storage_path_opts.bucket,
            _obj_storage_path_opts.path.native(),
            _obj_storage_path_opts.upload_id.has_value() ? *_obj_storage_path_opts.upload_id : "");
    timespec current_time;
    // We don't need high accuracy here, so we use time(nullptr)
    // since it's the fastest way to get current time(second)
    auto current_time_second = time(nullptr);
    current_time.tv_sec = current_time_second + timeout_duration;
    current_time.tv_nsec = 0;
    // bthread::countdown_event::timed_wait() should use absolute time
    while (0 != _countdown_event.timed_wait(current_time)) {
        current_time.tv_sec += timeout_duration;
        LOG(WARNING) << msg;
    }
}

Status S3FileWriter::close(bool non_block) {
    if (state() == State::CLOSED) {
        return Status::InternalError("S3FileWriter already closed, file path {}, file key {}",
                                     _obj_storage_path_opts.path.native(),
                                     _obj_storage_path_opts.key);
    }
    if (state() == State::ASYNC_CLOSING) {
        if (non_block) {
            return Status::InternalError("Don't submit async close multi times");
        }
        CHECK(_async_close_pack != nullptr);
        _st = _async_close_pack->future.get();
        _async_close_pack = nullptr;
        // We should wait for all the pre async task to be finished
        _state = State::CLOSED;
        // The next time we call close() with no matter non_block true or false, it would always return the
        // '_st' value because this writer is already closed.
        return _st;
    }
    if (non_block) {
        _state = State::ASYNC_CLOSING;
        _async_close_pack = std::make_unique<AsyncCloseStatusPack>();
        _async_close_pack->future = _async_close_pack->promise.get_future();
        s3_file_writer_async_close_queuing << 1;
        return ExecEnv::GetInstance()->non_block_close_thread_pool()->submit_func([&]() {
            s3_file_writer_async_close_queuing << -1;
            s3_file_writer_async_close_processing << 1;
            _async_close_pack->promise.set_value(_close_impl());
            s3_file_writer_async_close_processing << -1;
        });
    }
    _st = _close_impl();
    _state = State::CLOSED;
    return _st;
}

bool S3FileWriter::_complete_part_task_callback(Status s) {
    bool ret = false;
    if (!s.ok()) [[unlikely]] {
        VLOG_NOTICE << "failed at key: " << _obj_storage_path_opts.key
                    << ", status: " << s.to_string();
        std::unique_lock<std::mutex> _lck {_completed_lock};
        _failed = true;
        ret = true;
        _st = std::move(s);
    }
    // After the signal, there is a scenario where the previous invocation of _wait_until_finish
    // returns to the caller, and subsequently, the S3 file writer is destructed.
    // This means that accessing _failed afterwards would result in a heap use after free vulnerability.
    _countdown_event.signal();
    return ret;
}

Status S3FileWriter::_build_upload_buffer() {
    auto builder = FileBufferBuilder();
    builder.set_type(BufferType::UPLOAD)
            .set_upload_callback([part_num = _cur_part_num, this](UploadFileBuffer& buf) {
                _upload_one_part(part_num, buf);
            })
            .set_file_offset(_bytes_appended)
            .set_sync_after_complete_task([this](auto&& PH1) {
                return _complete_part_task_callback(std::forward<decltype(PH1)>(PH1));
            })
            .set_is_cancelled([this]() { return _failed.load(); });
    if (_cache_builder != nullptr) {
        // We would load the data into file cache asynchronously which indicates
        // that this instance of S3FileWriter might have been destructed when we
        // try to do writing into file cache, so we make the lambda capture the variable
        // we need by value to extend their lifetime
        builder.set_allocate_file_blocks_holder(
                [builder = *_cache_builder, offset = _bytes_appended]() -> FileBlocksHolderPtr {
                    return builder.allocate_cache_holder(offset, config::s3_write_buffer_size);
                });
    }
    RETURN_IF_ERROR(builder.build(&_pending_buf));
    auto* buf = dynamic_cast<UploadFileBuffer*>(_pending_buf.get());
    DCHECK(buf != nullptr);
    return Status::OK();
}

Status S3FileWriter::_close_impl() {
    VLOG_DEBUG << "S3FileWriter::close, path: " << _obj_storage_path_opts.path.native();

    if (_cur_part_num == 1 && _pending_buf) { // data size is less than config::s3_write_buffer_size
        RETURN_IF_ERROR(_set_upload_to_remote_less_than_buffer_size());
    }

    if (_bytes_appended == 0) {
        DCHECK_EQ(_cur_part_num, 1);
        // No data written, but need to create an empty file
        RETURN_IF_ERROR(_build_upload_buffer());
        if (!_used_by_s3_committer) {
            auto* pending_buf = dynamic_cast<UploadFileBuffer*>(_pending_buf.get());
            pending_buf->set_upload_to_remote([this](UploadFileBuffer& buf) { _put_object(buf); });
        } else {
            RETURN_IF_ERROR(_create_multi_upload_request());
        }
    }

    if (_pending_buf != nullptr) { // there is remaining data in buffer need to be uploaded
        _countdown_event.add_count();
        RETURN_IF_ERROR(FileBuffer::submit(std::move(_pending_buf)));
        _pending_buf = nullptr;
    }

    RETURN_IF_ERROR(_complete());
    SYNC_POINT_RETURN_WITH_VALUE("s3_file_writer::close", Status());

    return Status::OK();
}

Status S3FileWriter::appendv(const Slice* data, size_t data_cnt) {
    if (state() != State::OPENED) [[unlikely]] {
        return Status::InternalError("append to closed file: {}",
                                     _obj_storage_path_opts.path.native());
    }

    size_t buffer_size = config::s3_write_buffer_size;
    TEST_SYNC_POINT_RETURN_WITH_VALUE("s3_file_writer::appenv", Status());
    for (size_t i = 0; i < data_cnt; i++) {
        size_t data_size = data[i].get_size();
        for (size_t pos = 0, data_size_to_append = 0; pos < data_size; pos += data_size_to_append) {
            if (_failed) {
                return _st;
            }
            if (!_pending_buf) {
                RETURN_IF_ERROR(_build_upload_buffer());
            }
            // we need to make sure all parts except the last one to be 5MB or more
            // and shouldn't be larger than buf
            data_size_to_append = std::min(data_size - pos, _pending_buf->get_file_offset() +
                                                                    buffer_size - _bytes_appended);

            // if the buffer has memory buf inside, the data would be written into memory first then S3 then file cache
            // it would be written to cache then S3 if the buffer doesn't have memory preserved
            RETURN_IF_ERROR(_pending_buf->append_data(
                    Slice {data[i].get_data() + pos, data_size_to_append}));
            TEST_SYNC_POINT_CALLBACK("s3_file_writer::appenv_1", &_pending_buf, _cur_part_num);

            // If this is the last part and the data size is less than s3_write_buffer_size,
            // the pending_buf will be handled by _close_impl() and _complete()
            // If this is the last part and the data size is equal to s3_write_buffer_size,
            // the pending_buf is handled here and submitted. it will be waited by _complete()
            if (_pending_buf->get_size() == buffer_size) {
                // only create multiple upload request when the data size is
                // larger or equal to s3_write_buffer_size than one memory buffer
                if (_cur_part_num == 1) {
                    RETURN_IF_ERROR(_create_multi_upload_request());
                }
                _cur_part_num++;
                _countdown_event.add_count();
                RETURN_IF_ERROR(FileBuffer::submit(std::move(_pending_buf)));
                _pending_buf = nullptr;
            }
            _bytes_appended += data_size_to_append;
        }
    }
    return Status::OK();
}

void S3FileWriter::_upload_one_part(int part_num, UploadFileBuffer& buf) {
    VLOG_DEBUG << "upload_one_part " << _obj_storage_path_opts.path.native()
               << " part=" << part_num;
    if (buf.is_cancelled()) {
        LOG_INFO("file {} skip part {} because previous failure {}",
                 _obj_storage_path_opts.path.native(), part_num, _st);
        return;
    }
    const auto& client = _obj_client->get();
    if (nullptr == client) {
        LOG_WARNING("failed to upload part, key={}, part_num={} bacause of null obj client",
                    _obj_storage_path_opts.key, part_num);
        buf.set_status(Status::InternalError<false>("invalid obj storage client"));
        return;
    }
    auto resp = client->upload_part(_obj_storage_path_opts, buf.get_string_view_data(), part_num);
    if (resp.resp.status.code != ErrorCode::OK) {
        LOG_WARNING("failed to upload part, key={}, part_num={}, status={}",
                    _obj_storage_path_opts.key, part_num, resp.resp.status.msg);
        buf.set_status(Status(resp.resp.status.code, std::move(resp.resp.status.msg)));
        return;
    }
    s3_bytes_written_total << buf.get_size();

    ObjectCompleteMultiPart completed_part {
            static_cast<int>(part_num), resp.etag.has_value() ? std::move(resp.etag.value()) : ""};

    std::unique_lock<std::mutex> lck {_completed_lock};
    _completed_parts.emplace_back(std::move(completed_part));
}

// if enabled check
// 1. issue a head object request for existence check
// 2. check the file size
Status check_after_upload(ObjStorageClient* client, const ObjectStorageResponse& upload_res,
                          const ObjectStoragePathOptions& path_opt, int64_t bytes_appended,
                          const std::string& put_or_comp) {
    if (!config::enable_s3_object_check_after_upload) return Status::OK();

    auto head_res = client->head_object(path_opt);

    // clang-format off
    auto err_msg = [&]() {
        std::stringstream ss;
        ss << "failed to check object after upload=" << put_or_comp
            << " file_path=" << path_opt.path.native()
            << fmt::format(" {}_err=", put_or_comp) << upload_res.status.msg
            << fmt::format(" {}_code=", put_or_comp) << upload_res.status.code
            << fmt::format(" {}_http_code=", put_or_comp) << upload_res.http_code
            << fmt::format(" {}_request_id=", put_or_comp) << upload_res.request_id
            << " head_err=" << head_res.resp.status.msg
            << " head_code=" << head_res.resp.status.code
            << " head_http_code=" << head_res.resp.http_code
            << " head_request_id=" << head_res.resp.request_id;
        return ss.str();
    };
    // clang-format on

    // TODO(gavin): make it fail by injection
    TEST_SYNC_POINT_CALLBACK("S3FileWriter::check_after_load", &head_res);
    if (head_res.resp.status.code != ErrorCode::OK && head_res.resp.http_code != 200) {
        LOG(WARNING) << "failed to issue head object after upload, " << err_msg();
        DCHECK(false) << "failed to issue head object after upload, " << err_msg();
        // FIXME(gavin): we should retry if this HEAD fails?
        return Status::IOError(
                "failed to issue head object after upload, status_code={}, http_code={}, err={}",
                head_res.resp.status.code, head_res.resp.http_code, head_res.resp.status.msg);
    }
    if (head_res.file_size != bytes_appended) {
        LOG(WARNING) << "failed to check size after upload, expected_size=" << bytes_appended
                     << " actual_size=" << head_res.file_size << err_msg();
        DCHECK_EQ(bytes_appended, head_res.file_size)
                << "failed to check size after upload," << err_msg();
        return Status::IOError(
                "failed to check object size after upload, expected_size={} actual_size={}",
                bytes_appended, head_res.file_size);
    }
    return Status::OK();
}

Status S3FileWriter::_complete() {
    const auto& client = _obj_client->get();
    if (nullptr == client) {
        return Status::InternalError<false>("invalid obj storage client");
    }
    if (_failed) {
        _wait_until_finish("early quit");
        return _st;
    }
    // When the part num is only one, it means the data is less than 5MB so we can just put it.
    if (_cur_part_num == 1) {
        _wait_until_finish("PutObject");
        return _st;
    }
    // Wait multipart load and finish.
    _wait_until_finish("Complete");
    TEST_SYNC_POINT_CALLBACK("S3FileWriter::_complete:1",
                             std::make_pair(&_failed, &_completed_parts));
    if (_used_by_s3_committer) {    // S3 committer will complete multipart upload file on FE side.
        s3_file_created_total << 1; // Assume that it will be created successfully
        return Status::OK();
    }

    // check number of parts
    int64_t expected_num_parts1 = (_bytes_appended / config::s3_write_buffer_size) +
                                  !!(_bytes_appended % config::s3_write_buffer_size);
    int64_t expected_num_parts2 =
            (_bytes_appended % config::s3_write_buffer_size) ? _cur_part_num : _cur_part_num - 1;
    DCHECK_EQ(expected_num_parts1, expected_num_parts2)
            << " bytes_appended=" << _bytes_appended << " cur_part_num=" << _cur_part_num
            << " s3_write_buffer_size=" << config::s3_write_buffer_size;
    if (_failed || _completed_parts.size() != static_cast<size_t>(expected_num_parts1) ||
        expected_num_parts1 != expected_num_parts2) {
        _st = Status::InternalError(
                "failed to complete multipart upload, error status={} failed={} #complete_parts={} "
                "#expected_parts={} "
                "completed_parts_list={} file_path={} file_size={} has left buffer not uploaded={}",
                _st, _failed, _completed_parts.size(), expected_num_parts1, _dump_completed_part(),
                _obj_storage_path_opts.path.native(), _bytes_appended, _pending_buf != nullptr);
        LOG(WARNING) << _st;
        return _st;
    }
    // make sure _completed_parts are ascending order
    std::sort(_completed_parts.begin(), _completed_parts.end(),
              [](auto& p1, auto& p2) { return p1.part_num < p2.part_num; });
    TEST_SYNC_POINT_CALLBACK("S3FileWriter::_complete:2", &_completed_parts);
    LOG(INFO) << "complete_multipart_upload " << _obj_storage_path_opts.path.native()
              << " size=" << _bytes_appended << " number_parts=" << _completed_parts.size()
              << " s3_write_buffer_size=" << config::s3_write_buffer_size;
    auto resp = client->complete_multipart_upload(_obj_storage_path_opts, _completed_parts);
    if (resp.status.code != ErrorCode::OK) {
        LOG_WARNING("failed to complete multipart upload, err={}, file_path={}", resp.status.msg,
                    _obj_storage_path_opts.path.native());
        return {resp.status.code, std::move(resp.status.msg)};
    }

    RETURN_IF_ERROR(check_after_upload(client.get(), resp, _obj_storage_path_opts, _bytes_appended,
                                       "complete_multipart"));

    s3_file_created_total << 1;
    return Status::OK();
}

Status S3FileWriter::_set_upload_to_remote_less_than_buffer_size() {
    auto* buf = dynamic_cast<UploadFileBuffer*>(_pending_buf.get());
    DCHECK(buf != nullptr);
    if (_used_by_s3_committer) {
        // If used_by_s3_committer, we always use multi-parts uploading.
        buf->set_upload_to_remote([part_num = _cur_part_num, this](UploadFileBuffer& buf) {
            _upload_one_part(part_num, buf);
        });
        DCHECK(_cur_part_num == 1);
        RETURN_IF_ERROR(_create_multi_upload_request());
    } else {
        // if we only need to upload one file less than 5MB, we can just
        // call PutObject to reduce the network IO
        buf->set_upload_to_remote([this](UploadFileBuffer& b) { _put_object(b); });
    }
    return Status::OK();
}

void S3FileWriter::_put_object(UploadFileBuffer& buf) {
    LOG(INFO) << "put_object " << _obj_storage_path_opts.path.native()
              << " size=" << _bytes_appended;
    if (state() == State::CLOSED) {
        DCHECK(state() != State::CLOSED)
                << "state=" << (int)state() << " path=" << _obj_storage_path_opts.path.native();
        LOG_WARNING("failed to put object because file closed, file path {}",
                    _obj_storage_path_opts.path.native());
        buf.set_status(Status::InternalError<false>("try to put closed file"));
        return;
    }
    const auto& client = _obj_client->get();
    if (nullptr == client) {
        buf.set_status(Status::InternalError<false>("invalid obj storage client"));
        return;
    }
    TEST_SYNC_POINT_RETURN_WITH_VOID("S3FileWriter::_put_object", this, &buf);
    auto resp = client->put_object(_obj_storage_path_opts, buf.get_string_view_data());
    if (resp.status.code != ErrorCode::OK) {
        LOG_WARNING("failed to put object, put object failed because {}, file path {}",
                    resp.status.msg, _obj_storage_path_opts.path.native());
        buf.set_status({resp.status.code, std::move(resp.status.msg)});
        return;
    }

    auto st = check_after_upload(client.get(), resp, _obj_storage_path_opts, _bytes_appended,
                                 "put_object");
    if (!st.ok()) {
        buf.set_status(st);
        return;
    }

    s3_file_created_total << 1;
}

std::string S3FileWriter::_dump_completed_part() const {
    std::stringstream ss;
    ss << "part_numbers:";
    for (const auto& part : _completed_parts) {
        ss << " " << part.part_num;
    }
    return ss.str();
}
#include "common/compile_check_end.h"

} // namespace doris::io
