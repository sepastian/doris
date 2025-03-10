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

package org.apache.doris.qe;

import org.apache.doris.analysis.RedirectStatus;
import org.apache.doris.common.ClientPool;
import org.apache.doris.common.telemetry.Telemetry;
import org.apache.doris.thrift.FrontendService;
import org.apache.doris.thrift.TMasterOpRequest;
import org.apache.doris.thrift.TMasterOpResult;
import org.apache.doris.thrift.TNetworkAddress;
import org.apache.doris.thrift.TUniqueId;

import com.google.common.collect.ImmutableMap;
import io.opentelemetry.api.trace.Span;
import io.opentelemetry.context.Context;
import io.opentelemetry.context.Scope;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;
import org.apache.thrift.TException;
import org.apache.thrift.transport.TTransportException;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;

public class MasterOpExecutor {
    private static final Logger LOG = LogManager.getLogger(MasterOpExecutor.class);

    private static final float RPC_TIMEOUT_COEFFICIENT = 1.2f;

    private final OriginStatement originStmt;
    private final ConnectContext ctx;
    private TMasterOpResult result;

    private int waitTimeoutMs;
    // the total time of thrift connectTime add readTime and writeTime
    private int thriftTimeoutMs;

    private boolean shouldNotRetry;

    public MasterOpExecutor(OriginStatement originStmt, ConnectContext ctx, RedirectStatus status, boolean isQuery) {
        this.originStmt = originStmt;
        this.ctx = ctx;
        if (status.isNeedToWaitJournalSync()) {
            this.waitTimeoutMs = (int) (ctx.getExecTimeout() * 1000 * RPC_TIMEOUT_COEFFICIENT);
        } else {
            this.waitTimeoutMs = 0;
        }
        this.thriftTimeoutMs = (int) (ctx.getExecTimeout() * 1000 * RPC_TIMEOUT_COEFFICIENT);
        // if isQuery=false, we shouldn't retry twice when catch exception because of Idempotency
        this.shouldNotRetry = !isQuery;
    }

    public void execute() throws Exception {
        Span forwardSpan =
                ctx.getTracer().spanBuilder("forward").setParent(Context.current())
                        .startSpan();
        try (Scope ignored = forwardSpan.makeCurrent()) {
            forward();
        } catch (Exception e) {
            forwardSpan.recordException(e);
            throw e;
        } finally {
            forwardSpan.end();
        }
        LOG.info("forwarding to master get result max journal id: {}", result.maxJournalId);
        ctx.getEnv().getJournalObservable().waitOn(result.maxJournalId, waitTimeoutMs);
    }

    // Send request to Master
    private void forward() throws Exception {
        if (!ctx.getEnv().isReady()) {
            throw new Exception("Node catalog is not ready, please wait for a while.");
        }
        String masterHost = ctx.getEnv().getMasterIp();
        int masterRpcPort = ctx.getEnv().getMasterRpcPort();
        TNetworkAddress thriftAddress = new TNetworkAddress(masterHost, masterRpcPort);

        FrontendService.Client client;
        try {
            client = ClientPool.frontendPool.borrowObject(thriftAddress, thriftTimeoutMs);
        } catch (Exception e) {
            // may throw NullPointerException. add err msg
            throw new Exception("Failed to get master client.", e);
        }
        TMasterOpRequest params = new TMasterOpRequest();
        params.setCluster(ctx.getClusterName());
        params.setSql(originStmt.originStmt);
        params.setStmtIdx(originStmt.idx);
        params.setUser(ctx.getQualifiedUser());
        params.setDb(ctx.getDatabase());
        params.setResourceInfo(ctx.toResourceCtx());
        params.setUserIp(ctx.getRemoteIP());
        params.setStmtId(ctx.getStmtId());
        params.setCurrentUserIdent(ctx.getCurrentUserIdentity().toThrift());

        // query options
        params.setQueryOptions(ctx.getSessionVariable().getQueryOptionVariables());
        // session variables
        params.setSessionVariables(ctx.getSessionVariable().getForwardVariables());

        // create a trace carrier
        Map<String, String> traceCarrier = new HashMap<>();
        // Inject the request with the current context
        Telemetry.getOpenTelemetry().getPropagators().getTextMapPropagator()
                .inject(Context.current(), traceCarrier, (carrier, key, value) -> carrier.put(key, value));
        // carrier send tracing to master
        params.setTraceCarrier(traceCarrier);

        if (null != ctx.queryId()) {
            params.setQueryId(ctx.queryId());
        }

        LOG.info("Forward statement {} to Master {}", ctx.getStmtId(), thriftAddress);

        boolean isReturnToPool = false;
        try {
            result = client.forward(params);
            isReturnToPool = true;
        } catch (TTransportException e) {
            // wrap the raw exception.
            Exception exception = new ForwardToMasterException(
                    String.format("Forward statement %s to Master %s failed", ctx.getStmtId(),
                            thriftAddress), e);

            boolean ok = ClientPool.frontendPool.reopen(client, thriftTimeoutMs);
            if (!ok) {
                throw exception;
            }
            if (shouldNotRetry || e.getType() == TTransportException.TIMED_OUT) {
                throw exception;
            } else {
                LOG.warn("Forward statement " + ctx.getStmtId() + " to Master " + thriftAddress + " twice", e);
                try {
                    result = client.forward(params);
                    isReturnToPool = true;
                } catch (TException ex) {
                    throw exception;
                }
            }
        } finally {
            if (isReturnToPool) {
                ClientPool.frontendPool.returnObject(thriftAddress, client);
            } else {
                ClientPool.frontendPool.invalidateObject(thriftAddress, client);
            }
        }
    }

    public ByteBuffer getOutputPacket() {
        if (result == null) {
            return null;
        }
        return result.packet;
    }

    public TUniqueId getQueryId() {
        if (result != null && result.isSetQueryId()) {
            return result.getQueryId();
        } else {
            return null;
        }
    }

    public String getProxyStatus() {
        if (result == null) {
            return QueryState.MysqlStateType.UNKNOWN.name();
        }
        if (!result.isSetStatus()) {
            return QueryState.MysqlStateType.UNKNOWN.name();
        } else {
            return result.getStatus();
        }
    }

    public ShowResultSet getProxyResultSet() {
        if (result == null) {
            return null;
        }
        if (result.isSetResultSet()) {
            return new ShowResultSet(result.resultSet);
        } else {
            return null;
        }
    }

    public void setResult(TMasterOpResult result) {
        this.result = result;
    }

    public static class ForwardToMasterException extends RuntimeException {

        private static final Map<Integer, String> TYPE_MSG_MAP =
                ImmutableMap.<Integer, String>builder()
                        .put(TTransportException.UNKNOWN, "Unknown exception")
                        .put(TTransportException.NOT_OPEN, "Connection is not open")
                        .put(TTransportException.ALREADY_OPEN, "Connection has already opened up")
                        .put(TTransportException.TIMED_OUT, "Connection timeout")
                        .put(TTransportException.END_OF_FILE, "EOF")
                        .put(TTransportException.CORRUPTED_DATA, "Corrupted data")
                        .build();

        private final String msg;

        public ForwardToMasterException(String msg, TTransportException exception) {
            this.msg = msg + ", cause: " + TYPE_MSG_MAP.get(exception.getType());
        }

        @Override
        public String getMessage() {
            return msg;
        }
    }
}
