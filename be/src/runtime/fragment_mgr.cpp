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

#include "runtime/fragment_mgr.h"

#include <bvar/latency_recorder.h>
#include <gen_cpp/HeartbeatService_types.h>
#include <gperftools/profiler.h>
#include <thrift/protocol/TDebugProtocol.h>

#include <memory>
#include <sstream>

#include "common/object_pool.h"
#include "gen_cpp/FrontendService.h"
#include "gen_cpp/PaloInternalService_types.h"
#include "gen_cpp/PlanNodes_types.h"
#include "gen_cpp/QueryPlanExtra_types.h"
#include "gen_cpp/Types_types.h"
#include "gutil/strings/substitute.h"
#include "io/fs/stream_load_pipe.h"
#include "opentelemetry/trace/scope.h"
#include "pipeline/pipeline_fragment_context.h"
#include "runtime/client_cache.h"
#include "runtime/datetime_value.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/plan_fragment_executor.h"
#include "runtime/runtime_filter_mgr.h"
#include "runtime/stream_load/new_load_stream_mgr.h"
#include "runtime/stream_load/stream_load_context.h"
#include "runtime/task_group/task_group_manager.h"
#include "runtime/thread_context.h"
#include "service/backend_options.h"
#include "util/doris_metrics.h"
#include "util/network_util.h"
#include "util/stopwatch.hpp"
#include "util/telemetry/telemetry.h"
#include "util/threadpool.h"
#include "util/thrift_util.h"
#include "util/uid_util.h"
#include "util/url_coding.h"

namespace doris {

DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(plan_fragment_count, MetricUnit::NOUNIT);
DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(timeout_canceled_fragment_count, MetricUnit::NOUNIT);
DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(fragment_thread_pool_queue_size, MetricUnit::NOUNIT);
bvar::LatencyRecorder g_fragmentmgr_prepare_latency("doris_FragmentMgr", "prepare");

std::string to_load_error_http_path(const std::string& file_name) {
    if (file_name.empty()) {
        return "";
    }
    std::stringstream url;
    url << "http://" << get_host_port(BackendOptions::get_localhost(), config::webserver_port)
        << "/api/_load_error_log?"
        << "file=" << file_name;
    return url.str();
}

using apache::thrift::TException;
using apache::thrift::transport::TTransportException;

class RuntimeProfile;
class FragmentExecState {
public:
    using report_status_callback_impl = std::function<void(const ReportStatusRequest)>;
    // Constructor by using QueryFragmentsCtx
    FragmentExecState(const TUniqueId& query_id, const TUniqueId& instance_id, int backend_num,
                      ExecEnv* exec_env, std::shared_ptr<QueryFragmentsCtx> fragments_ctx,
                      const report_status_callback_impl& report_status_cb_impl);

    Status prepare(const TExecPlanFragmentParams& params);

    Status execute();

    Status cancel(const PPlanFragmentCancelReason& reason, const std::string& msg = "");
    bool is_canceled() { return _cancelled; }

    TUniqueId fragment_instance_id() const { return _fragment_instance_id; }

    TUniqueId query_id() const { return _query_id; }

    PlanFragmentExecutor* executor() { return &_executor; }

    const vectorized::VecDateTimeValue& start_time() const { return _start_time; }

    void set_merge_controller_handler(
            std::shared_ptr<RuntimeFilterMergeControllerEntity>& handler) {
        _merge_controller_handler = handler;
    }

    // Update status of this fragment execute
    Status update_status(Status status) {
        std::lock_guard<std::mutex> l(_status_lock);
        if (!status.ok() && _exec_status.ok()) {
            _exec_status = status;
            LOG(WARNING) << "query_id=" << print_id(_query_id)
                         << ", instance_id=" << print_id(_fragment_instance_id)
                         << " meet error status " << status;
        }
        return _exec_status;
    }

    void set_group(const TResourceInfo& info) {
        _set_rsc_info = true;
        _user = info.user;
        _group = info.group;
    }

    bool is_timeout(const vectorized::VecDateTimeValue& now) const {
        if (_timeout_second <= 0) {
            return false;
        }
        if (now.second_diff(_start_time) > _timeout_second) {
            return true;
        }
        return false;
    }

    int get_timeout_second() const { return _timeout_second; }

    std::shared_ptr<QueryFragmentsCtx> get_fragments_ctx() { return _fragments_ctx; }

    void set_need_wait_execution_trigger() { _need_wait_execution_trigger = true; }

private:
    void coordinator_callback(const Status& status, RuntimeProfile* profile, bool done);

    // Id of this query
    TUniqueId _query_id;
    // Id of this instance
    TUniqueId _fragment_instance_id;
    // Used to report to coordinator which backend is over
    int _backend_num;
    TNetworkAddress _coord_addr;

    PlanFragmentExecutor _executor;
    vectorized::VecDateTimeValue _start_time;

    std::mutex _status_lock;
    Status _exec_status;

    bool _set_rsc_info = false;
    std::string _user;
    std::string _group;

    int _timeout_second;
    std::atomic<bool> _cancelled {false};

    // This context is shared by all fragments of this host in a query
    std::shared_ptr<QueryFragmentsCtx> _fragments_ctx;

    std::shared_ptr<RuntimeFilterMergeControllerEntity> _merge_controller_handler;

    // If set the true, this plan fragment will be executed only after FE send execution start rpc.
    bool _need_wait_execution_trigger = false;
    report_status_callback_impl _report_status_cb_impl;
};

FragmentExecState::FragmentExecState(const TUniqueId& query_id,
                                     const TUniqueId& fragment_instance_id, int backend_num,
                                     ExecEnv* exec_env,
                                     std::shared_ptr<QueryFragmentsCtx> fragments_ctx,
                                     const report_status_callback_impl& report_status_cb_impl)
        : _query_id(query_id),
          _fragment_instance_id(fragment_instance_id),
          _backend_num(backend_num),
          _executor(exec_env, std::bind<void>(std::mem_fn(&FragmentExecState::coordinator_callback),
                                              this, std::placeholders::_1, std::placeholders::_2,
                                              std::placeholders::_3)),
          _set_rsc_info(false),
          _timeout_second(-1),
          _fragments_ctx(std::move(fragments_ctx)),
          _report_status_cb_impl(report_status_cb_impl) {
    _start_time = vectorized::VecDateTimeValue::local_time();
    _coord_addr = _fragments_ctx->coord_addr;
}

Status FragmentExecState::prepare(const TExecPlanFragmentParams& params) {
    if (params.__isset.query_options) {
        _timeout_second = params.query_options.execution_timeout;
    }

    if (_fragments_ctx == nullptr) {
        if (params.__isset.resource_info) {
            set_group(params.resource_info);
        }
    }

    if (_fragments_ctx == nullptr) {
        return _executor.prepare(params);
    } else {
        return _executor.prepare(params, _fragments_ctx.get());
    }
}

Status FragmentExecState::execute() {
    if (_need_wait_execution_trigger) {
        // if _need_wait_execution_trigger is true, which means this instance
        // is prepared but need to wait for the signal to do the rest execution.
        if (!_fragments_ctx->wait_for_start()) {
            return cancel(PPlanFragmentCancelReason::INTERNAL_ERROR, "wait fragment start timeout");
        }
    }
#ifndef BE_TEST
    if (_executor.runtime_state()->is_cancelled()) {
        return Status::Cancelled("cancelled before execution");
    }
#endif
    int64_t duration_ns = 0;
    {
        SCOPED_RAW_TIMER(&duration_ns);
        opentelemetry::trace::Tracer::GetCurrentSpan()->AddEvent("start executing Fragment");
        Status st = _executor.open();
        WARN_IF_ERROR(st,
                      strings::Substitute("Got error while opening fragment $0, query id: $1",
                                          print_id(_fragment_instance_id), print_id(_query_id)));
        if (!st.ok()) {
            cancel(PPlanFragmentCancelReason::INTERNAL_ERROR, "PlanFragmentExecutor open failed");
        }
        _executor.close();
    }
    DorisMetrics::instance()->fragment_requests_total->increment(1);
    DorisMetrics::instance()->fragment_request_duration_us->increment(duration_ns / 1000);
    return Status::OK();
}

Status FragmentExecState::cancel(const PPlanFragmentCancelReason& reason, const std::string& msg) {
    if (!_cancelled) {
        std::lock_guard<std::mutex> l(_status_lock);
        if (reason == PPlanFragmentCancelReason::LIMIT_REACH) {
            _executor.set_is_report_on_cancel(false);
        }
        _executor.cancel(reason, msg);
#ifndef BE_TEST
        // Get pipe from new load stream manager and send cancel to it or the fragment may hang to wait read from pipe
        // For stream load the fragment's query_id == load id, it is set in FE.
        auto stream_load_ctx = _fragments_ctx->exec_env()->new_load_stream_mgr()->get(_query_id);
        if (stream_load_ctx != nullptr) {
            stream_load_ctx->pipe->cancel(PPlanFragmentCancelReason_Name(reason));
        }
#endif
        _cancelled = true;
    }
    return Status::OK();
}

// There can only be one of these callbacks in-flight at any moment, because
// it is only invoked from the executor's reporting thread.
// Also, the reported status will always reflect the most recent execution status,
// including the final status when execution finishes.
void FragmentExecState::coordinator_callback(const Status& status, RuntimeProfile* profile,
                                             bool done) {
    _report_status_cb_impl(
            {status, profile, done, _coord_addr, _query_id, -1, _fragment_instance_id, _backend_num,
             _executor.runtime_state(),
             std::bind(&FragmentExecState::update_status, this, std::placeholders::_1),
             std::bind(&PlanFragmentExecutor::cancel, &_executor, std::placeholders::_1,
                       std::placeholders::_2)});
    DCHECK(status.ok() || done); // if !status.ok() => done
}

FragmentMgr::FragmentMgr(ExecEnv* exec_env)
        : _exec_env(exec_env), _stop_background_threads_latch(1) {
    _entity = DorisMetrics::instance()->metric_registry()->register_entity("FragmentMgr");
    INT_UGAUGE_METRIC_REGISTER(_entity, timeout_canceled_fragment_count);
    REGISTER_HOOK_METRIC(plan_fragment_count, [this]() { return _fragment_map.size(); });

    auto s = Thread::create(
            "FragmentMgr", "cancel_timeout_plan_fragment", [this]() { this->cancel_worker(); },
            &_cancel_thread);
    CHECK(s.ok()) << s.to_string();

    // TODO(zc): we need a better thread-pool
    // now one user can use all the thread pool, others have no resource.
    s = ThreadPoolBuilder("FragmentMgrThreadPool")
                .set_min_threads(config::fragment_pool_thread_num_min)
                .set_max_threads(config::fragment_pool_thread_num_max)
                .set_max_queue_size(config::fragment_pool_queue_size)
                .build(&_thread_pool);

    REGISTER_HOOK_METRIC(fragment_thread_pool_queue_size,
                         [this]() { return _thread_pool->get_queue_size(); });
    CHECK(s.ok()) << s.to_string();
}

FragmentMgr::~FragmentMgr() {
    DEREGISTER_HOOK_METRIC(plan_fragment_count);
    DEREGISTER_HOOK_METRIC(fragment_thread_pool_queue_size);
    _stop_background_threads_latch.count_down();
    if (_cancel_thread) {
        _cancel_thread->join();
    }
    // Stop all the worker, should wait for a while?
    // _thread_pool->wait_for();
    _thread_pool->shutdown();

    // Only me can delete
    {
        std::lock_guard<std::mutex> lock(_lock);
        _fragment_map.clear();
        _fragments_ctx_map.clear();
    }
}

std::string FragmentMgr::to_http_path(const std::string& file_name) {
    std::stringstream url;
    url << "http://" << BackendOptions::get_localhost() << ":" << config::webserver_port
        << "/api/_download_load?"
        << "token=" << _exec_env->token() << "&file=" << file_name;
    return url.str();
}

// There can only be one of these callbacks in-flight at any moment, because
// it is only invoked from the executor's reporting thread.
// Also, the reported status will always reflect the most recent execution status,
// including the final status when execution finishes.
void FragmentMgr::coordinator_callback(const ReportStatusRequest& req) {
    DCHECK(req.status.ok() || req.done); // if !status.ok() => done
    Status exec_status = req.update_fn(req.status);
    Status coord_status;
    FrontendServiceConnection coord(_exec_env->frontend_client_cache(), req.coord_addr,
                                    &coord_status);
    if (!coord_status.ok()) {
        std::stringstream ss;
        UniqueId uid(req.query_id.hi, req.query_id.lo);
        ss << "couldn't get a client for " << req.coord_addr << ", reason: " << coord_status;
        LOG(WARNING) << "query_id: " << uid << ", " << ss.str();
        req.update_fn(Status::InternalError(ss.str()));
        return;
    }

    TReportExecStatusParams params;
    params.protocol_version = FrontendServiceVersion::V1;
    params.__set_query_id(req.query_id);
    params.__set_backend_num(req.backend_num);
    params.__set_fragment_instance_id(req.fragment_instance_id);
    params.__set_fragment_id(req.fragment_id);
    exec_status.set_t_status(&params);
    params.__set_done(req.done);

    DCHECK(req.runtime_state != nullptr);
    if (req.runtime_state->query_type() == TQueryType::LOAD && !req.done && req.status.ok()) {
        // this is a load plan, and load is not finished, just make a brief report
        params.__set_loaded_rows(req.runtime_state->num_rows_load_total());
        params.__set_loaded_bytes(req.runtime_state->num_bytes_load_total());
    } else {
        if (req.runtime_state->query_type() == TQueryType::LOAD) {
            params.__set_loaded_rows(req.runtime_state->num_rows_load_total());
            params.__set_loaded_bytes(req.runtime_state->num_bytes_load_total());
        }
        if (req.profile == nullptr) {
            params.__isset.profile = false;
        } else {
            req.profile->to_thrift(&params.profile);
            params.__isset.profile = true;
        }

        if (!req.runtime_state->output_files().empty()) {
            params.__isset.delta_urls = true;
            for (auto& it : req.runtime_state->output_files()) {
                params.delta_urls.push_back(to_http_path(it));
            }
        }
        if (req.runtime_state->num_rows_load_total() > 0 ||
            req.runtime_state->num_rows_load_filtered() > 0) {
            params.__isset.load_counters = true;

            static std::string s_dpp_normal_all = "dpp.norm.ALL";
            static std::string s_dpp_abnormal_all = "dpp.abnorm.ALL";
            static std::string s_unselected_rows = "unselected.rows";

            params.load_counters.emplace(
                    s_dpp_normal_all, std::to_string(req.runtime_state->num_rows_load_success()));
            params.load_counters.emplace(
                    s_dpp_abnormal_all,
                    std::to_string(req.runtime_state->num_rows_load_filtered()));
            params.load_counters.emplace(
                    s_unselected_rows,
                    std::to_string(req.runtime_state->num_rows_load_unselected()));
        }
        if (!req.runtime_state->get_error_log_file_path().empty()) {
            params.__set_tracking_url(
                    to_load_error_http_path(req.runtime_state->get_error_log_file_path()));
        }
        if (!req.runtime_state->export_output_files().empty()) {
            params.__isset.export_files = true;
            params.export_files = req.runtime_state->export_output_files();
        }
        if (!req.runtime_state->tablet_commit_infos().empty()) {
            params.__isset.commitInfos = true;
            params.commitInfos.reserve(req.runtime_state->tablet_commit_infos().size());
            for (auto& info : req.runtime_state->tablet_commit_infos()) {
                params.commitInfos.push_back(info);
            }
        }
        if (!req.runtime_state->error_tablet_infos().empty()) {
            params.__isset.errorTabletInfos = true;
            params.errorTabletInfos.reserve(req.runtime_state->error_tablet_infos().size());
            for (auto& info : req.runtime_state->error_tablet_infos()) {
                params.errorTabletInfos.push_back(info);
            }
        }

        // Send new errors to coordinator
        req.runtime_state->get_unreported_errors(&(params.error_log));
        params.__isset.error_log = (params.error_log.size() > 0);
    }

    if (_exec_env->master_info()->__isset.backend_id) {
        params.__set_backend_id(_exec_env->master_info()->backend_id);
    }

    TReportExecStatusResult res;
    Status rpc_status;

    VLOG_DEBUG << "reportExecStatus params is "
               << apache::thrift::ThriftDebugString(params).c_str();
    if (!exec_status.ok()) {
        LOG(WARNING) << "report error status: " << exec_status.to_string()
                     << " to coordinator: " << req.coord_addr
                     << ", query id: " << print_id(req.query_id)
                     << ", instance id: " << print_id(req.fragment_instance_id);
    }
    try {
        try {
            coord->reportExecStatus(res, params);
        } catch (TTransportException& e) {
            LOG(WARNING) << "Retrying ReportExecStatus. query id: " << print_id(req.query_id)
                         << ", instance id: " << print_id(req.fragment_instance_id) << " to "
                         << req.coord_addr << ", err: " << e.what();
            rpc_status = coord.reopen();

            if (!rpc_status.ok()) {
                // we need to cancel the execution of this fragment
                req.update_fn(rpc_status);
                req.cancel_fn(PPlanFragmentCancelReason::INTERNAL_ERROR, "report rpc fail");
                return;
            }
            coord->reportExecStatus(res, params);
        }

        rpc_status = Status(res.status);
    } catch (TException& e) {
        std::stringstream msg;
        msg << "ReportExecStatus() to " << req.coord_addr << " failed:\n" << e.what();
        LOG(WARNING) << msg.str();
        rpc_status = Status::InternalError(msg.str());
    }

    if (!rpc_status.ok()) {
        // we need to cancel the execution of this fragment
        req.update_fn(rpc_status);
        req.cancel_fn(PPlanFragmentCancelReason::INTERNAL_ERROR, "rpc fail 2");
    }
}

static void empty_function(RuntimeState*, Status*) {}

void FragmentMgr::_exec_actual(std::shared_ptr<FragmentExecState> exec_state,
                               const FinishCallback& cb) {
    std::string func_name {"PlanFragmentExecutor::_exec_actual"};
#ifndef BE_TEST
    auto span = exec_state->executor()->runtime_state()->get_tracer()->StartSpan(func_name);
    SCOPED_ATTACH_TASK(exec_state->executor()->runtime_state());
#else
    auto span = telemetry::get_noop_tracer()->StartSpan(func_name);
#endif
    auto scope = opentelemetry::trace::Scope {span};
    span->SetAttribute("query_id", print_id(exec_state->query_id()));
    span->SetAttribute("instance_id", print_id(exec_state->fragment_instance_id()));

    LOG_INFO(func_name)
            .tag("query_id", exec_state->query_id())
            .tag("instance_id", exec_state->fragment_instance_id())
            .tag("pthread_id", (uintptr_t)pthread_self());

    Status st = exec_state->execute();
    if (!st.ok()) {
        exec_state->cancel(PPlanFragmentCancelReason::INTERNAL_ERROR, "exec_state execute failed");
    }

    std::shared_ptr<QueryFragmentsCtx> fragments_ctx = exec_state->get_fragments_ctx();
    bool all_done = false;
    if (fragments_ctx != nullptr) {
        // decrease the number of unfinished fragments
        all_done = fragments_ctx->countdown();
    }

    // remove exec state after this fragment finished
    {
        std::lock_guard<std::mutex> lock(_lock);
        _fragment_map.erase(exec_state->fragment_instance_id());
        if (all_done && fragments_ctx) {
            _fragments_ctx_map.erase(fragments_ctx->query_id);
        }
    }

    // Callback after remove from this id
    auto status = exec_state->executor()->status();
    cb(exec_state->executor()->runtime_state(), &status);
}

Status FragmentMgr::exec_plan_fragment(const TExecPlanFragmentParams& params) {
    if (params.txn_conf.need_txn) {
        std::shared_ptr<StreamLoadContext> stream_load_ctx =
                std::make_shared<StreamLoadContext>(_exec_env);
        stream_load_ctx->db = params.txn_conf.db;
        stream_load_ctx->db_id = params.txn_conf.db_id;
        stream_load_ctx->table = params.txn_conf.tbl;
        stream_load_ctx->txn_id = params.txn_conf.txn_id;
        stream_load_ctx->id = UniqueId(params.params.query_id);
        stream_load_ctx->put_result.params = params;
        stream_load_ctx->use_streaming = true;
        stream_load_ctx->load_type = TLoadType::MANUL_LOAD;
        stream_load_ctx->load_src_type = TLoadSourceType::RAW;
        stream_load_ctx->label = params.import_label;
        stream_load_ctx->format = TFileFormatType::FORMAT_CSV_PLAIN;
        stream_load_ctx->timeout_second = 3600;
        stream_load_ctx->auth.token = params.txn_conf.token;
        stream_load_ctx->need_commit_self = true;
        stream_load_ctx->need_rollback = true;
        auto pipe = std::make_shared<io::StreamLoadPipe>(
                io::kMaxPipeBufferedBytes /* max_buffered_bytes */, 64 * 1024 /* min_chunk_size */,
                -1 /* total_length */, true /* use_proto */);
        stream_load_ctx->body_sink = pipe;
        stream_load_ctx->pipe = pipe;
        stream_load_ctx->max_filter_ratio = params.txn_conf.max_filter_ratio;

        RETURN_IF_ERROR(
                _exec_env->new_load_stream_mgr()->put(stream_load_ctx->id, stream_load_ctx));

        RETURN_IF_ERROR(_exec_env->stream_load_executor()->execute_plan_fragment(stream_load_ctx));
        return Status::OK();
    } else {
        return exec_plan_fragment(params, empty_function);
    }
}

Status FragmentMgr::exec_plan_fragment(const TPipelineFragmentParams& params) {
    // TODO
    return exec_plan_fragment(params, empty_function);
}

Status FragmentMgr::start_query_execution(const PExecPlanFragmentStartRequest* request) {
    std::lock_guard<std::mutex> lock(_lock);
    TUniqueId query_id;
    query_id.__set_hi(request->query_id().hi());
    query_id.__set_lo(request->query_id().lo());
    auto search = _fragments_ctx_map.find(query_id);
    if (search == _fragments_ctx_map.end()) {
        return Status::InternalError(
                "Failed to get query fragments context. Query may be "
                "timeout or be cancelled. host: {}",
                BackendOptions::get_localhost());
    }
    search->second->set_ready_to_execute(false);
    return Status::OK();
}

void FragmentMgr::remove_pipeline_context(
        std::shared_ptr<pipeline::PipelineFragmentContext> f_context) {
    std::lock_guard<std::mutex> lock(_lock);
    auto query_id = f_context->get_query_id();
    auto* q_context = f_context->get_query_context();
    bool all_done = q_context->countdown();
    _pipeline_map.erase(f_context->get_fragment_instance_id());
    if (all_done) {
        _fragments_ctx_map.erase(query_id);
    }
}

template <typename Params>
Status FragmentMgr::_get_query_ctx(const Params& params, TUniqueId query_id, bool pipeline,
                                   std::shared_ptr<QueryFragmentsCtx>& fragments_ctx) {
    if (params.is_simplified_param) {
        // Get common components from _fragments_ctx_map
        std::lock_guard<std::mutex> lock(_lock);
        auto search = _fragments_ctx_map.find(query_id);
        if (search == _fragments_ctx_map.end()) {
            return Status::InternalError(
                    "Failed to get query fragments context. Query may be "
                    "timeout or be cancelled. host: {}",
                    BackendOptions::get_localhost());
        }
        fragments_ctx = search->second;
    } else {
        // This may be a first fragment request of the query.
        // Create the query fragments context.
        fragments_ctx.reset(new QueryFragmentsCtx(params.fragment_num_on_host, _exec_env));
        fragments_ctx->query_id = query_id;
        RETURN_IF_ERROR(DescriptorTbl::create(&(fragments_ctx->obj_pool), params.desc_tbl,
                                              &(fragments_ctx->desc_tbl)));
        fragments_ctx->coord_addr = params.coord;
        LOG(INFO) << "query_id: "
                  << UniqueId(fragments_ctx->query_id.hi, fragments_ctx->query_id.lo)
                  << " coord_addr " << fragments_ctx->coord_addr
                  << " total fragment num on current host: " << params.fragment_num_on_host;
        fragments_ctx->query_globals = params.query_globals;

        if (params.__isset.resource_info) {
            fragments_ctx->user = params.resource_info.user;
            fragments_ctx->group = params.resource_info.group;
            fragments_ctx->set_rsc_info = true;
        }

        fragments_ctx->get_shared_hash_table_controller()->set_pipeline_engine_enabled(pipeline);
        fragments_ctx->timeout_second = params.query_options.execution_timeout;
        _set_scan_concurrency(params, fragments_ctx.get());

        bool has_query_mem_tracker =
                params.query_options.__isset.mem_limit && (params.query_options.mem_limit > 0);
        int64_t bytes_limit = has_query_mem_tracker ? params.query_options.mem_limit : -1;
        if (bytes_limit > MemInfo::mem_limit()) {
            VLOG_NOTICE << "Query memory limit " << PrettyPrinter::print(bytes_limit, TUnit::BYTES)
                        << " exceeds process memory limit of "
                        << PrettyPrinter::print(MemInfo::mem_limit(), TUnit::BYTES)
                        << ". Using process memory limit instead";
            bytes_limit = MemInfo::mem_limit();
        }
        if (params.query_options.query_type == TQueryType::SELECT) {
            fragments_ctx->query_mem_tracker = std::make_shared<MemTrackerLimiter>(
                    MemTrackerLimiter::Type::QUERY,
                    fmt::format("Query#Id={}", print_id(fragments_ctx->query_id)), bytes_limit);
        } else if (params.query_options.query_type == TQueryType::LOAD) {
            fragments_ctx->query_mem_tracker = std::make_shared<MemTrackerLimiter>(
                    MemTrackerLimiter::Type::LOAD,
                    fmt::format("Load#Id={}", print_id(fragments_ctx->query_id)), bytes_limit);
        } else { // EXTERNAL
            fragments_ctx->query_mem_tracker = std::make_shared<MemTrackerLimiter>(
                    MemTrackerLimiter::Type::LOAD,
                    fmt::format("External#Id={}", print_id(fragments_ctx->query_id)), bytes_limit);
        }
        if (params.query_options.__isset.is_report_success &&
            params.query_options.is_report_success) {
            fragments_ctx->query_mem_tracker->enable_print_log_usage();
        }

        if (pipeline) {
            int ts = fragments_ctx->timeout_second;
            taskgroup::TaskGroupPtr tg;
            auto ts_id = taskgroup::TaskGroupManager::DEFAULT_TG_ID;
            if (ts > 0 && ts <= config::pipeline_short_query_timeout_s) {
                ts_id = taskgroup::TaskGroupManager::SHORT_TG_ID;
            }
            tg = taskgroup::TaskGroupManager::instance()->get_task_group(ts_id);
            fragments_ctx->set_task_group(tg);
            LOG(INFO) << "Query/load id: " << print_id(fragments_ctx->query_id)
                      << "use task group: " << tg->debug_string();
        }

        {
            // Find _fragments_ctx_map again, in case some other request has already
            // create the query fragments context.
            std::lock_guard<std::mutex> lock(_lock);
            auto search = _fragments_ctx_map.find(query_id);
            if (search == _fragments_ctx_map.end()) {
                _fragments_ctx_map.insert(std::make_pair(fragments_ctx->query_id, fragments_ctx));
                LOG(INFO) << "Register query/load memory tracker, query/load id: "
                          << print_id(fragments_ctx->query_id)
                          << " limit: " << PrettyPrinter::print(bytes_limit, TUnit::BYTES);
            } else {
                // Already has a query fragments context, use it
                fragments_ctx = search->second;
            }
        }
    }
    return Status::OK();
}

Status FragmentMgr::exec_plan_fragment(const TExecPlanFragmentParams& params,
                                       const FinishCallback& cb) {
    auto tracer = telemetry::is_current_span_valid() ? telemetry::get_tracer("tracer")
                                                     : telemetry::get_noop_tracer();
    VLOG_ROW << "exec_plan_fragment params is "
             << apache::thrift::ThriftDebugString(params).c_str();
    // sometimes TExecPlanFragmentParams debug string is too long and glog
    // will truncate the log line, so print query options seperately for debuggin purpose
    VLOG_ROW << "query options is "
             << apache::thrift::ThriftDebugString(params.query_options).c_str();
    START_AND_SCOPE_SPAN(tracer, span, "FragmentMgr::exec_plan_fragment");
    const TUniqueId& fragment_instance_id = params.params.fragment_instance_id;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto iter = _fragment_map.find(fragment_instance_id);
        if (iter != _fragment_map.end()) {
            // Duplicated
            return Status::OK();
        }
    }

    std::shared_ptr<FragmentExecState> exec_state;
    std::shared_ptr<QueryFragmentsCtx> fragments_ctx;
    bool pipeline_engine_enabled = params.query_options.__isset.enable_pipeline_engine &&
                                   params.query_options.enable_pipeline_engine;
    RETURN_IF_ERROR(
            _get_query_ctx(params, params.params.query_id, pipeline_engine_enabled, fragments_ctx));
    fragments_ctx->fragment_ids.push_back(fragment_instance_id);

    exec_state.reset(
            new FragmentExecState(fragments_ctx->query_id, params.params.fragment_instance_id,
                                  params.backend_num, _exec_env, fragments_ctx,
                                  std::bind<void>(std::mem_fn(&FragmentMgr::coordinator_callback),
                                                  this, std::placeholders::_1)));
    if (params.__isset.need_wait_execution_trigger && params.need_wait_execution_trigger) {
        // set need_wait_execution_trigger means this instance will not actually being executed
        // until the execPlanFragmentStart RPC trigger to start it.
        exec_state->set_need_wait_execution_trigger();
    }

    int64_t duration_ns = 0;
    if (!pipeline_engine_enabled) {
        {
            SCOPED_RAW_TIMER(&duration_ns);
            RETURN_IF_ERROR(exec_state->prepare(params));
        }
        g_fragmentmgr_prepare_latency << (duration_ns / 1000);
        std::shared_ptr<RuntimeFilterMergeControllerEntity> handler;
        _runtimefilter_controller.add_entity(params, &handler,
                                             exec_state->executor()->runtime_state());
        exec_state->set_merge_controller_handler(handler);
        {
            std::lock_guard<std::mutex> lock(_lock);
            _fragment_map.insert(std::make_pair(params.params.fragment_instance_id, exec_state));
            _cv.notify_all();
        }
        auto st = _thread_pool->submit_func(
                [this, exec_state, cb,
                 parent_span = opentelemetry::trace::Tracer::GetCurrentSpan()] {
                    OpentelemetryScope scope {parent_span};
                    _exec_actual(exec_state, cb);
                });
        if (!st.ok()) {
            {
                // Remove the exec state added
                std::lock_guard<std::mutex> lock(_lock);
                _fragment_map.erase(params.params.fragment_instance_id);
            }
            exec_state->cancel(PPlanFragmentCancelReason::INTERNAL_ERROR,
                               "push plan fragment to thread pool failed");
            return Status::InternalError(strings::Substitute(
                    "push plan fragment $0 to thread pool failed. err = $1, BE: $2",
                    print_id(params.params.fragment_instance_id), st.to_string(),
                    BackendOptions::get_localhost()));
        }
    } else {
        if (!params.__isset.need_wait_execution_trigger || !params.need_wait_execution_trigger) {
            fragments_ctx->set_ready_to_execute_only();
        }
        _setup_shared_hashtable_for_broadcast_join(params, exec_state->executor()->runtime_state(),
                                                   fragments_ctx.get());
        std::shared_ptr<pipeline::PipelineFragmentContext> context =
                std::make_shared<pipeline::PipelineFragmentContext>(
                        fragments_ctx->query_id, fragment_instance_id, -1, params.backend_num,
                        fragments_ctx, _exec_env, cb,
                        std::bind<void>(std::mem_fn(&FragmentMgr::coordinator_callback), this,
                                        std::placeholders::_1));
        {
            SCOPED_RAW_TIMER(&duration_ns);
            auto prepare_st = context->prepare(params);
            if (!prepare_st.ok()) {
                context->close_if_prepare_failed();
                return prepare_st;
            }
        }
        g_fragmentmgr_prepare_latency << (duration_ns / 1000);

        std::shared_ptr<RuntimeFilterMergeControllerEntity> handler;
        _runtimefilter_controller.add_entity(params, &handler, context->get_runtime_state());
        context->set_merge_controller_handler(handler);

        {
            std::lock_guard<std::mutex> lock(_lock);
            _pipeline_map.insert(std::make_pair(fragment_instance_id, context));
            _cv.notify_all();
        }
        return context->submit();
    }

    return Status::OK();
}

Status FragmentMgr::exec_plan_fragment(const TPipelineFragmentParams& params,
                                       const FinishCallback& cb) {
    auto tracer = telemetry::is_current_span_valid() ? telemetry::get_tracer("tracer")
                                                     : telemetry::get_noop_tracer();
    VLOG_ROW << "exec_plan_fragment params is "
             << apache::thrift::ThriftDebugString(params).c_str();
    // sometimes TExecPlanFragmentParams debug string is too long and glog
    // will truncate the log line, so print query options seperately for debuggin purpose
    VLOG_ROW << "query options is "
             << apache::thrift::ThriftDebugString(params.query_options).c_str();
    START_AND_SCOPE_SPAN(tracer, span, "FragmentMgr::exec_plan_fragment");

    std::shared_ptr<FragmentExecState> exec_state;
    std::shared_ptr<QueryFragmentsCtx> fragments_ctx;
    RETURN_IF_ERROR(_get_query_ctx(params, params.query_id, true, fragments_ctx));

    for (size_t i = 0; i < params.local_params.size(); i++) {
        const auto& local_params = params.local_params[i];

        const TUniqueId& fragment_instance_id = local_params.fragment_instance_id;
        {
            std::lock_guard<std::mutex> lock(_lock);
            auto iter = _pipeline_map.find(fragment_instance_id);
            if (iter != _pipeline_map.end()) {
                // Duplicated
                continue;
            }
        }

        fragments_ctx->fragment_ids.push_back(fragment_instance_id);

        exec_state.reset(new FragmentExecState(
                fragments_ctx->query_id, fragment_instance_id, local_params.backend_num, _exec_env,
                fragments_ctx,
                std::bind<void>(std::mem_fn(&FragmentMgr::coordinator_callback), this,
                                std::placeholders::_1)));
        if (params.__isset.need_wait_execution_trigger && params.need_wait_execution_trigger) {
            // set need_wait_execution_trigger means this instance will not actually being executed
            // until the execPlanFragmentStart RPC trigger to start it.
            exec_state->set_need_wait_execution_trigger();
        }

        int64_t duration_ns = 0;
        if (!params.__isset.need_wait_execution_trigger || !params.need_wait_execution_trigger) {
            fragments_ctx->set_ready_to_execute_only();
        }
        _setup_shared_hashtable_for_broadcast_join(
                params, local_params, exec_state->executor()->runtime_state(), fragments_ctx.get());
        std::shared_ptr<pipeline::PipelineFragmentContext> context =
                std::make_shared<pipeline::PipelineFragmentContext>(
                        fragments_ctx->query_id, fragment_instance_id, params.fragment_id,
                        local_params.backend_num, fragments_ctx, _exec_env, cb,
                        std::bind<void>(std::mem_fn(&FragmentMgr::coordinator_callback), this,
                                        std::placeholders::_1));
        {
            SCOPED_RAW_TIMER(&duration_ns);
            auto prepare_st = context->prepare(params, i);
            if (!prepare_st.ok()) {
                context->close_if_prepare_failed();
                return prepare_st;
            }
        }
        g_fragmentmgr_prepare_latency << (duration_ns / 1000);

        std::shared_ptr<RuntimeFilterMergeControllerEntity> handler;
        _runtimefilter_controller.add_entity(params, local_params, &handler,
                                             context->get_runtime_state());
        context->set_merge_controller_handler(handler);

        {
            std::lock_guard<std::mutex> lock(_lock);
            _pipeline_map.insert(std::make_pair(fragment_instance_id, context));
            _cv.notify_all();
        }
        RETURN_IF_ERROR(context->submit());
    }

    return Status::OK();
}

void FragmentMgr::_set_scan_concurrency(const TExecPlanFragmentParams& params,
                                        QueryFragmentsCtx* fragments_ctx) {
#ifndef BE_TEST
    // If the token is set, the scan task will use limited_scan_pool in scanner scheduler.
    // Otherwise, the scan task will use local/remote scan pool in scanner scheduler
    if (params.query_options.__isset.resource_limit &&
        params.query_options.resource_limit.__isset.cpu_limit) {
        fragments_ctx->set_thread_token(params.query_options.resource_limit.cpu_limit, false);
    }
#endif
}

void FragmentMgr::_set_scan_concurrency(const TPipelineFragmentParams& params,
                                        QueryFragmentsCtx* fragments_ctx) {
#ifndef BE_TEST
    // set thread token
    // the thread token will be set if
    // 1. the cpu_limit is set, or
    // 2. the limit is very small ( < 1024)
    // If the token is set, the scan task will use limited_scan_pool in scanner scheduler.
    // Otherwise, the scan task will use local/remote scan pool in scanner scheduler
    int concurrency = 1;
    bool is_serial = false;
    bool need_token = false;
    if (params.query_options.__isset.resource_limit &&
        params.query_options.resource_limit.__isset.cpu_limit) {
        concurrency = params.query_options.resource_limit.cpu_limit;
        need_token = true;
    } else {
        concurrency = config::doris_scanner_thread_pool_thread_num;
    }
    if (params.__isset.fragment && params.fragment.__isset.plan &&
        params.fragment.plan.nodes.size() > 0) {
        for (auto& node : params.fragment.plan.nodes) {
            // Only for SCAN NODE
            if (!_is_scan_node(node.node_type)) {
                continue;
            }
            if (node.__isset.conjuncts && !node.conjuncts.empty()) {
                // If the scan node has where predicate, do not set concurrency
                continue;
            }
            if (node.limit > 0 && node.limit < 1024) {
                concurrency = 1;
                is_serial = true;
                need_token = true;
                break;
            }
        }
    }
    if (need_token) {
        fragments_ctx->set_thread_token(concurrency, is_serial);
    }
#endif
}

bool FragmentMgr::_is_scan_node(const TPlanNodeType::type& type) {
    return type == TPlanNodeType::OLAP_SCAN_NODE || type == TPlanNodeType::MYSQL_SCAN_NODE ||
           type == TPlanNodeType::SCHEMA_SCAN_NODE || type == TPlanNodeType::META_SCAN_NODE ||
           type == TPlanNodeType::ES_SCAN_NODE || type == TPlanNodeType::ES_HTTP_SCAN_NODE ||
           type == TPlanNodeType::ODBC_SCAN_NODE || type == TPlanNodeType::DATA_GEN_SCAN_NODE ||
           type == TPlanNodeType::FILE_SCAN_NODE || type == TPlanNodeType::JDBC_SCAN_NODE;
}

void FragmentMgr::cancel(const TUniqueId& fragment_id, const PPlanFragmentCancelReason& reason,
                         const std::string& msg) {
    std::shared_ptr<FragmentExecState> exec_state;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto iter = _fragment_map.find(fragment_id);
        if (iter != _fragment_map.end()) {
            exec_state = iter->second;
        }
    }
    if (exec_state) {
        exec_state->cancel(reason, msg);
        return;
    }

    std::shared_ptr<pipeline::PipelineFragmentContext> pipeline_fragment_ctx;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto iter = _pipeline_map.find(fragment_id);
        if (iter != _pipeline_map.end()) {
            pipeline_fragment_ctx = iter->second;
        }
    }
    if (pipeline_fragment_ctx) {
        pipeline_fragment_ctx->cancel(reason, msg);
    }
}

void FragmentMgr::cancel_query(const TUniqueId& query_id, const PPlanFragmentCancelReason& reason,
                               const std::string& msg) {
    std::vector<TUniqueId> cancel_fragment_ids;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto ctx = _fragments_ctx_map.find(query_id);
        if (ctx != _fragments_ctx_map.end()) {
            cancel_fragment_ids = ctx->second->fragment_ids;
        }
    }
    for (auto it : cancel_fragment_ids) {
        cancel(it, reason, msg);
    }
}

bool FragmentMgr::query_is_canceled(const TUniqueId& query_id) {
    std::lock_guard<std::mutex> lock(_lock);
    auto ctx = _fragments_ctx_map.find(query_id);
    if (ctx != _fragments_ctx_map.end()) {
        for (auto it : ctx->second->fragment_ids) {
            auto exec_state_iter = _fragment_map.find(it);
            if (exec_state_iter != _fragment_map.end() && exec_state_iter->second) {
                return exec_state_iter->second->is_canceled();
            }

            auto pipeline_ctx_iter = _pipeline_map.find(it);
            if (pipeline_ctx_iter != _pipeline_map.end() && pipeline_ctx_iter->second) {
                return pipeline_ctx_iter->second->is_canceled();
            }
        }
    }
    return true;
}

void FragmentMgr::cancel_worker() {
    LOG(INFO) << "FragmentMgr cancel worker start working.";
    do {
        std::vector<TUniqueId> to_cancel;
        std::vector<TUniqueId> to_cancel_queries;
        vectorized::VecDateTimeValue now = vectorized::VecDateTimeValue::local_time();
        {
            std::lock_guard<std::mutex> lock(_lock);
            for (auto& it : _fragment_map) {
                if (it.second->is_timeout(now)) {
                    to_cancel.push_back(it.second->fragment_instance_id());
                }
            }
            for (auto it = _fragments_ctx_map.begin(); it != _fragments_ctx_map.end();) {
                if (it->second->is_timeout(now)) {
                    it = _fragments_ctx_map.erase(it);
                } else {
                    ++it;
                }
            }
        }
        timeout_canceled_fragment_count->increment(to_cancel.size());
        for (auto& id : to_cancel) {
            cancel(id, PPlanFragmentCancelReason::TIMEOUT);
            LOG(INFO) << "FragmentMgr cancel worker going to cancel timeout fragment "
                      << print_id(id);
        }
    } while (!_stop_background_threads_latch.wait_for(std::chrono::seconds(1)));
    LOG(INFO) << "FragmentMgr cancel worker is going to exit.";
}

void FragmentMgr::debug(std::stringstream& ss) {
    // Keep things simple
    std::lock_guard<std::mutex> lock(_lock);

    ss << "FragmentMgr have " << _fragment_map.size() << " jobs.\n";
    ss << "job_id\t\tstart_time\t\texecute_time(s)\n";
    vectorized::VecDateTimeValue now = vectorized::VecDateTimeValue::local_time();
    for (auto& it : _fragment_map) {
        ss << it.first << "\t" << it.second->start_time().debug_string() << "\t"
           << now.second_diff(it.second->start_time()) << "\n";
    }
}

/*
 * 1. resolve opaqued_query_plan to thrift structure
 * 2. build TExecPlanFragmentParams
 */
Status FragmentMgr::exec_external_plan_fragment(const TScanOpenParams& params,
                                                const TUniqueId& fragment_instance_id,
                                                std::vector<TScanColumnDesc>* selected_columns) {
    const std::string& opaqued_query_plan = params.opaqued_query_plan;
    std::string query_plan_info;
    // base64 decode query plan
    if (!base64_decode(opaqued_query_plan, &query_plan_info)) {
        LOG(WARNING) << "open context error: base64_decode decode opaqued_query_plan failure";
        std::stringstream msg;
        msg << "query_plan_info: " << query_plan_info
            << " validate error, should not be modified after returned Doris FE processed";
        return Status::InvalidArgument(msg.str());
    }
    TQueryPlanInfo t_query_plan_info;
    const uint8_t* buf = (const uint8_t*)query_plan_info.data();
    uint32_t len = query_plan_info.size();
    // deserialize TQueryPlanInfo
    auto st = deserialize_thrift_msg(buf, &len, false, &t_query_plan_info);
    if (!st.ok()) {
        LOG(WARNING) << "open context error: deserialize TQueryPlanInfo failure";
        std::stringstream msg;
        msg << "query_plan_info: " << query_plan_info
            << " deserialize error, should not be modified after returned Doris FE processed";
        return Status::InvalidArgument(msg.str());
    }

    // set up desc tbl
    DescriptorTbl* desc_tbl = nullptr;
    ObjectPool obj_pool;
    st = DescriptorTbl::create(&obj_pool, t_query_plan_info.desc_tbl, &desc_tbl);
    if (!st.ok()) {
        LOG(WARNING) << "open context error: extract DescriptorTbl failure";
        std::stringstream msg;
        msg << "query_plan_info: " << query_plan_info
            << " create DescriptorTbl error, should not be modified after returned Doris FE "
               "processed";
        return Status::InvalidArgument(msg.str());
    }
    TupleDescriptor* tuple_desc = desc_tbl->get_tuple_descriptor(0);
    if (tuple_desc == nullptr) {
        LOG(WARNING) << "open context error: extract TupleDescriptor failure";
        std::stringstream msg;
        msg << "query_plan_info: " << query_plan_info
            << " get  TupleDescriptor error, should not be modified after returned Doris FE "
               "processed";
        return Status::InvalidArgument(msg.str());
    }
    // process selected columns form slots
    for (const SlotDescriptor* slot : tuple_desc->slots()) {
        TScanColumnDesc col;
        col.__set_name(slot->col_name());
        col.__set_type(to_thrift(slot->type().type));
        selected_columns->emplace_back(std::move(col));
    }

    VLOG_QUERY << "BackendService execute open()  TQueryPlanInfo: "
               << apache::thrift::ThriftDebugString(t_query_plan_info);
    // assign the param used to execute PlanFragment
    TExecPlanFragmentParams exec_fragment_params;
    exec_fragment_params.protocol_version = (PaloInternalServiceVersion::type)0;
    exec_fragment_params.__set_is_simplified_param(false);
    exec_fragment_params.__set_fragment(t_query_plan_info.plan_fragment);
    exec_fragment_params.__set_desc_tbl(t_query_plan_info.desc_tbl);

    // assign the param used for executing of PlanFragment-self
    TPlanFragmentExecParams fragment_exec_params;
    fragment_exec_params.query_id = t_query_plan_info.query_id;
    fragment_exec_params.fragment_instance_id = fragment_instance_id;
    std::map<::doris::TPlanNodeId, std::vector<TScanRangeParams>> per_node_scan_ranges;
    std::vector<TScanRangeParams> scan_ranges;
    std::vector<int64_t> tablet_ids = params.tablet_ids;
    TNetworkAddress address;
    address.hostname = BackendOptions::get_localhost();
    address.port = doris::config::be_port;
    std::map<int64_t, TTabletVersionInfo> tablet_info = t_query_plan_info.tablet_info;
    for (auto tablet_id : params.tablet_ids) {
        TPaloScanRange scan_range;
        scan_range.db_name = params.database;
        scan_range.table_name = params.table;
        auto iter = tablet_info.find(tablet_id);
        if (iter != tablet_info.end()) {
            TTabletVersionInfo info = iter->second;
            scan_range.tablet_id = tablet_id;
            scan_range.version = std::to_string(info.version);
            // Useless but it is required field in TPaloScanRange
            scan_range.version_hash = "0";
            scan_range.schema_hash = std::to_string(info.schema_hash);
            scan_range.hosts.push_back(address);
        } else {
            std::stringstream msg;
            msg << "tablet_id: " << tablet_id << " not found";
            LOG(WARNING) << "tablet_id [ " << tablet_id << " ] not found";
            return Status::NotFound(msg.str());
        }
        TScanRange doris_scan_range;
        doris_scan_range.__set_palo_scan_range(scan_range);
        TScanRangeParams scan_range_params;
        scan_range_params.scan_range = doris_scan_range;
        scan_ranges.push_back(scan_range_params);
    }
    per_node_scan_ranges.insert(std::make_pair((::doris::TPlanNodeId)0, scan_ranges));
    fragment_exec_params.per_node_scan_ranges = per_node_scan_ranges;
    exec_fragment_params.__set_params(fragment_exec_params);
    TQueryOptions query_options;
    query_options.batch_size = params.batch_size;
    query_options.execution_timeout = params.execution_timeout;
    query_options.mem_limit = params.mem_limit;
    query_options.query_type = TQueryType::EXTERNAL;
    exec_fragment_params.__set_query_options(query_options);
    VLOG_ROW << "external exec_plan_fragment params is "
             << apache::thrift::ThriftDebugString(exec_fragment_params).c_str();
    return exec_plan_fragment(exec_fragment_params);
}

Status FragmentMgr::apply_filter(const PPublishFilterRequest* request,
                                 butil::IOBufAsZeroCopyInputStream* attach_data) {
    bool is_pipeline = request->has_is_pipeline() && request->is_pipeline();

    UniqueId fragment_instance_id = request->fragment_id();
    TUniqueId tfragment_instance_id = fragment_instance_id.to_thrift();

    std::shared_ptr<FragmentExecState> fragment_state;
    std::shared_ptr<pipeline::PipelineFragmentContext> pip_context;

    RuntimeFilterMgr* runtime_filter_mgr = nullptr;
    if (is_pipeline) {
        std::unique_lock<std::mutex> lock(_lock);
        auto iter = _pipeline_map.find(tfragment_instance_id);
        if (iter == _pipeline_map.end()) {
            VLOG_CRITICAL << "unknown.... fragment-id:" << fragment_instance_id;
            return Status::InvalidArgument("fragment-id: {}", fragment_instance_id.to_string());
        }
        pip_context = iter->second;

        DCHECK(pip_context != nullptr);
        runtime_filter_mgr = pip_context->get_runtime_state()->runtime_filter_mgr();
    } else {
        std::unique_lock<std::mutex> lock(_lock);
        auto iter = _fragment_map.find(tfragment_instance_id);
        if (iter == _fragment_map.end()) {
            VLOG_CRITICAL << "unknown.... fragment-id:" << fragment_instance_id;
            return Status::InvalidArgument("fragment-id: {}", fragment_instance_id.to_string());
        }
        fragment_state = iter->second;

        DCHECK(fragment_state != nullptr);
        runtime_filter_mgr = fragment_state->executor()->runtime_state()->runtime_filter_mgr();
    }

    return runtime_filter_mgr->update_filter(request, attach_data);
}

Status FragmentMgr::merge_filter(const PMergeFilterRequest* request,
                                 butil::IOBufAsZeroCopyInputStream* attach_data) {
    UniqueId queryid = request->query_id();
    bool is_pipeline = request->has_is_pipeline() && request->is_pipeline();
    std::shared_ptr<RuntimeFilterMergeControllerEntity> filter_controller;
    RETURN_IF_ERROR(_runtimefilter_controller.acquire(queryid, &filter_controller));

    auto fragment_instance_id = filter_controller->instance_id();
    TUniqueId tfragment_instance_id = fragment_instance_id.to_thrift();
    std::shared_ptr<FragmentExecState> fragment_state;
    std::shared_ptr<pipeline::PipelineFragmentContext> pip_context;
    if (is_pipeline) {
        std::lock_guard<std::mutex> lock(_lock);
        auto iter = _pipeline_map.find(tfragment_instance_id);
        if (iter == _pipeline_map.end()) {
            VLOG_CRITICAL << "unknown fragment-id:" << fragment_instance_id;
            return Status::InvalidArgument("fragment-id: {}", fragment_instance_id.to_string());
        }

        // hold reference to pip_context, or else runtime_state can be destroyed
        // when filter_controller->merge is still in progress
        pip_context = iter->second;
    } else {
        std::unique_lock<std::mutex> lock(_lock);
        auto iter = _fragment_map.find(tfragment_instance_id);
        if (iter == _fragment_map.end()) {
            VLOG_CRITICAL << "unknown fragment-id:" << fragment_instance_id;
            return Status::InvalidArgument("fragment-id: {}", fragment_instance_id.to_string());
        }

        // hold reference to fragment_state, or else runtime_state can be destroyed
        // when filter_controller->merge is still in progress
        fragment_state = iter->second;
    }
    RETURN_IF_ERROR(filter_controller->merge(request, attach_data));
    return Status::OK();
}

void FragmentMgr::_setup_shared_hashtable_for_broadcast_join(const TExecPlanFragmentParams& params,
                                                             RuntimeState* state,
                                                             QueryFragmentsCtx* fragments_ctx) {
    if (!params.query_options.__isset.enable_share_hash_table_for_broadcast_join ||
        !params.query_options.enable_share_hash_table_for_broadcast_join) {
        return;
    }

    if (!params.__isset.fragment || !params.fragment.__isset.plan ||
        params.fragment.plan.nodes.empty()) {
        return;
    }
    for (auto& node : params.fragment.plan.nodes) {
        if (node.node_type != TPlanNodeType::HASH_JOIN_NODE ||
            !node.hash_join_node.__isset.is_broadcast_join ||
            !node.hash_join_node.is_broadcast_join) {
            continue;
        }

        if (params.build_hash_table_for_broadcast_join) {
            fragments_ctx->get_shared_hash_table_controller()->set_builder_and_consumers(
                    params.params.fragment_instance_id, params.instances_sharing_hash_table,
                    node.node_id);
        }
    }
}

void FragmentMgr::_setup_shared_hashtable_for_broadcast_join(
        const TPipelineFragmentParams& params, const TPipelineInstanceParams& local_params,
        RuntimeState* state, QueryFragmentsCtx* fragments_ctx) {
    if (!params.query_options.__isset.enable_share_hash_table_for_broadcast_join ||
        !params.query_options.enable_share_hash_table_for_broadcast_join) {
        return;
    }

    if (!params.__isset.fragment || !params.fragment.__isset.plan ||
        params.fragment.plan.nodes.empty()) {
        return;
    }
    for (auto& node : params.fragment.plan.nodes) {
        if (node.node_type != TPlanNodeType::HASH_JOIN_NODE ||
            !node.hash_join_node.__isset.is_broadcast_join ||
            !node.hash_join_node.is_broadcast_join) {
            continue;
        }

        if (local_params.build_hash_table_for_broadcast_join) {
            fragments_ctx->get_shared_hash_table_controller()->set_builder_and_consumers(
                    local_params.fragment_instance_id, params.instances_sharing_hash_table,
                    node.node_id);
        }
    }
}

} // namespace doris
