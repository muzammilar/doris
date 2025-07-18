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

#pragma once

#include "common/config.h"

namespace doris::config {
#include "common/compile_check_begin.h"

DECLARE_String(deploy_mode);
// deprecated do not configure directly
DECLARE_mString(cloud_unique_id);

static inline bool is_cloud_mode() {
    return deploy_mode == "cloud" || !cloud_unique_id.empty();
}

// Set the endpoint of meta service.
//
// If meta services are deployed behind a load balancer, set this config to "host:port" of the load balancer.
// Here is a set of configs to configure the connection behaviors:
// - meta_service_connection_pooled: distribute the long connections to different RS of the VIP.
// - meta_service_connection_age_base_seconds: expire the connection after a random time during [base, 2*base],
//      so that the BE has a chance to connect to a new RS. (When you add a new RS, the BE will connect to it)
// - meta_service_idle_connection_timeout_ms: rebuild the idle connections after the timeout exceeds. Some LB
// vendors will reset the connection if it is idle for a long time.
//
// If you want to access a group of meta services directly, set the addresses of meta services to this config,
// separated by a comma, like "host:port,host:port,host:port", then BE will choose a server to connect in randomly.
// In this mode, The config meta_service_connection_pooled is still useful, but the other two configs will be ignored.
DECLARE_mString(meta_service_endpoint);
// Whether check config::meta_service_endpoint is identical to the ms endpoint from FE master heartbeat
// This may help in some cases that we intend to change the config only FE side or BE side
DECLARE_mBool(enable_meta_service_endpoint_consistency_check);
// Set the underlying connection type to pooled.
DECLARE_Bool(meta_service_connection_pooled);
DECLARE_mInt64(meta_service_connection_pool_size);
// A connection will expire after a random time during [base, 2*base], so that the BE
// has a chance to connect to a new RS. Set zero to disable it.
//
// Only works when meta_service_endpoint is set to a single host.
DECLARE_mInt32(meta_service_connection_age_base_seconds);
// Rebuild the idle connections after the timeout exceeds. Set zero to disable it.
//
// Only works when meta_service_endpoint is set to a single host.
DECLARE_mInt32(meta_service_idle_connection_timeout_ms);
DECLARE_mInt32(meta_service_rpc_timeout_ms);
DECLARE_mInt32(meta_service_rpc_retry_times);
// default brpc timeout
DECLARE_mInt32(meta_service_brpc_timeout_ms);
DECLARE_mInt32(meta_service_rpc_timeout_retry_times);

// CloudTabletMgr config
DECLARE_Int64(tablet_cache_capacity);
DECLARE_Int64(tablet_cache_shards);
DECLARE_mInt32(tablet_sync_interval_s);
// parallelism for scanner init where may issue RPCs to sync rowset meta from MS
DECLARE_mInt32(init_scanner_sync_rowsets_parallelism);
DECLARE_mInt32(sync_rowsets_slow_threshold_ms);

// Cloud compaction config
DECLARE_mInt64(min_compaction_failure_interval_ms);
DECLARE_mBool(enable_new_tablet_do_compaction);
// For cloud read/write separate mode
DECLARE_mInt64(base_compaction_freeze_interval_s);
DECLARE_mInt64(compaction_load_max_freeze_interval_s);
DECLARE_mInt64(cumu_compaction_interval_s);

DECLARE_mInt32(compaction_timeout_seconds);
DECLARE_mInt32(lease_compaction_interval_seconds);
DECLARE_mBool(enable_parallel_cumu_compaction);
DECLARE_mDouble(base_compaction_thread_num_factor);
DECLARE_mDouble(cumu_compaction_thread_num_factor);
DECLARE_mInt32(check_auto_compaction_interval_seconds);
DECLARE_mInt32(max_base_compaction_task_num_per_disk);
DECLARE_mBool(prioritize_query_perf_in_compaction);
DECLARE_mInt32(compaction_max_rowset_count);

// CloudStorageEngine config
DECLARE_mInt32(refresh_s3_info_interval_s);
DECLARE_mInt32(vacuum_stale_rowsets_interval_s);
DECLARE_mInt32(schedule_sync_tablets_interval_s);

// Cloud mow
DECLARE_mInt32(mow_stream_load_commit_retry_times);

DECLARE_mBool(save_load_error_log_to_s3);

// the theads which sync the datas which loaded in other clusters
DECLARE_mInt32(sync_load_for_tablets_thread);

DECLARE_mInt32(delete_bitmap_lock_expiration_seconds);

DECLARE_mInt32(get_delete_bitmap_lock_max_retry_times);

// enable large txn lazy commit in meta-service `commit_txn`
DECLARE_mBool(enable_cloud_txn_lazy_commit);

DECLARE_mInt32(remove_expired_tablet_txn_info_interval_seconds);

DECLARE_mInt32(tablet_txn_info_min_expired_seconds);

DECLARE_mBool(enable_use_cloud_unique_id_from_fe);

DECLARE_Bool(enable_cloud_tablet_report);

DECLARE_mInt32(delete_bitmap_rpc_retry_times);

DECLARE_mInt64(meta_service_rpc_reconnect_interval_ms);

DECLARE_mInt32(meta_service_conflict_error_retry_times);

DECLARE_Bool(enable_check_storage_vault);

#include "common/compile_check_end.h"
} // namespace doris::config
