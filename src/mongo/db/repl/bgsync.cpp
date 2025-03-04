/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/bgsync.h"

#include <memory>

#include "mongo/base/counter.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_pool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rollback_source_impl.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;

namespace repl {

namespace {
const int kSleepToAllowBatchingMillis = 2;
const int kSmallBatchLimitBytes = 40000;
const Milliseconds kRollbackOplogSocketTimeout(10 * 60 * 1000);

// The number of times a node attempted to choose a node to sync from among the available sync
// source options. This occurs if we re-evaluate our sync source, receive an error from the source,
// or step down.
Counter64 numSyncSourceSelections;
ServerStatusMetricField<Counter64> displayNumSyncSourceSelections("repl.syncSource.numSelections",
                                                                  &numSyncSourceSelections);

// The number of times a node kept it's original sync source after re-evaluating if its current sync
// source was optimal.
Counter64 numTimesChoseSameSyncSource;
ServerStatusMetricField<Counter64> displayNumTimesChoseSameSyncSource(
    "repl.syncSource.numTimesChoseSame", &numTimesChoseSameSyncSource);

// The number of times a node chose a new sync source after re-evaluating if its current sync source
// was optimal.
Counter64 numTimesChoseDifferentSyncSource;
ServerStatusMetricField<Counter64> displayNumTimesChoseDifferentSyncSource(
    "repl.syncSource.numTimesChoseDifferent", &numTimesChoseDifferentSyncSource);

// The number of times a node could not find a sync source when choosing a node to sync from among
// the available options.
Counter64 numTimesCouldNotFindSyncSource;
ServerStatusMetricField<Counter64> displayNumTimesCouldNotFindSyncSource(
    "repl.syncSource.numTimesCouldNotFind", &numTimesCouldNotFindSyncSource);

/**
 * Extends DataReplicatorExternalStateImpl to be member state aware.
 */
class DataReplicatorExternalStateBackgroundSync : public DataReplicatorExternalStateImpl {
public:
    DataReplicatorExternalStateBackgroundSync(
        ReplicationCoordinator* replicationCoordinator,
        ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
        BackgroundSync* bgsync);
    bool shouldStopFetching(const HostAndPort& source,
                            const rpc::ReplSetMetadata& replMetadata,
                            boost::optional<rpc::OplogQueryMetadata> oqMetadata) override;

private:
    BackgroundSync* _bgsync;
};

DataReplicatorExternalStateBackgroundSync::DataReplicatorExternalStateBackgroundSync(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
    BackgroundSync* bgsync)
    : DataReplicatorExternalStateImpl(replicationCoordinator, replicationCoordinatorExternalState),
      _bgsync(bgsync) {}

bool DataReplicatorExternalStateBackgroundSync::shouldStopFetching(
    const HostAndPort& source,
    const rpc::ReplSetMetadata& replMetadata,
    boost::optional<rpc::OplogQueryMetadata> oqMetadata) {
    if (_bgsync->shouldStopFetching()) {
        return true;
    }

    return DataReplicatorExternalStateImpl::shouldStopFetching(source, replMetadata, oqMetadata);
}

size_t getSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}
}  // namespace

// Failpoint which causes rollback to hang before starting.
MONGO_FAIL_POINT_DEFINE(rollbackHangBeforeStart);

BackgroundSync::BackgroundSync(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
    ReplicationProcess* replicationProcess,
    OplogApplier* oplogApplier)
    : _oplogApplier(oplogApplier),
      _replCoord(replicationCoordinator),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState),
      _replicationProcess(replicationProcess) {}

void BackgroundSync::startup(OperationContext* opCtx) {
    invariant(!_producerThread);
    _producerThread.reset(new stdx::thread([this] { _run(); }));
}

void BackgroundSync::shutdown(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lock(_mutex);

    _state = ProducerState::Stopped;

    if (_syncSourceResolver) {
        _syncSourceResolver->shutdown();
    }

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }

    if (_rollback) {
        _rollback->shutdown();
    }

    _inShutdown = true;
}

void BackgroundSync::join(OperationContext* opCtx) {
    _producerThread->join();
}

bool BackgroundSync::inShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown_inlock();
}

bool BackgroundSync::tooStale() const {
    return _tooStale.load();
}

bool BackgroundSync::_inShutdown_inlock() const {
    return _inShutdown;
}

void BackgroundSync::_run() {
    Client::initThread("rsBackgroundSync");
    AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

    while (!inShutdown()) {
        try {
            _runProducer();
        } catch (const DBException& e) {
            std::string msg(str::stream() << "sync producer problem: " << redact(e));
            error() << msg;
            _replCoord->setMyHeartbeatMessage(msg);
            sleepmillis(100);  // sleep a bit to keep from hammering this thread with temp. errors.
        } catch (const std::exception& e2) {
            // redact(std::exception&) doesn't work
            severe() << "sync producer exception: " << redact(e2.what());
            fassertFailed(28546);
        }
    }
    // No need to reset optimes here because we are shutting down.
    stop(false);
}

void BackgroundSync::_runProducer() {
    if (getState() == ProducerState::Stopped) {
        sleepsecs(1);
        return;
    }

    auto memberState = _replCoord->getMemberState();
    invariant(!memberState.rollback());
    invariant(!memberState.startup());

    // We need to wait until initial sync has started.
    if (_replCoord->getMyLastAppliedOpTime().isNull()) {
        sleepsecs(1);
        return;
    }
    // we want to start when we're no longer primary
    // start() also loads _lastOpTimeFetched, which we know is set from the "if"
    {
        auto opCtx = cc().makeOperationContext();
        if (getState() == ProducerState::Starting) {
            start(opCtx.get());
        }
    }
    _produce();
}

void BackgroundSync::_produce() {
    if (MONGO_unlikely(stopReplProducer.shouldFail())) {
        // This log output is used in js tests so please leave it.
        log() << "bgsync - stopReplProducer fail point "
                 "enabled. Blocking until fail point is disabled.";

        // TODO(SERVER-27120): Remove the return statement and uncomment the while loop.
        // Currently we cannot block here or we prevent primaries from being fully elected since
        // we'll never call _signalNoNewDataForApplier.
        //        while (MONGO_unlikely(stopReplProducer.shouldFail()) && !inShutdown()) {
        //            mongo::sleepsecs(1);
        //        }
        mongo::sleepsecs(1);
        return;
    }

    // this oplog reader does not do a handshake because we don't want the server it's syncing
    // from to track how far it has synced
    HostAndPort oldSource;
    OpTime lastOpTimeFetched;
    HostAndPort source;
    SyncSourceResolverResponse syncSourceResp;
    {
        stdx::unique_lock<Latch> lock(_mutex);
        if (_lastOpTimeFetched.isNull()) {
            // then we're initial syncing and we're still waiting for this to be set
            lock.unlock();
            sleepsecs(1);
            // if there is no one to sync from
            return;
        }

        if (_state != ProducerState::Running) {
            return;
        }

        oldSource = _syncSourceHost;
    }

    // find a target to sync from the last optime fetched
    {
        OpTime minValidSaved;
        {
            auto opCtx = cc().makeOperationContext();
            minValidSaved = _replicationProcess->getConsistencyMarkers()->getMinValid(opCtx.get());
        }
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        const auto requiredOpTime = (minValidSaved > _lastOpTimeFetched) ? minValidSaved : OpTime();
        lastOpTimeFetched = _lastOpTimeFetched;
        if (!_syncSourceHost.empty()) {
            log() << "Clearing sync source " << _syncSourceHost << " to choose a new one.";
        }
        _syncSourceHost = HostAndPort();
        _syncSourceResolver = std::make_unique<SyncSourceResolver>(
            _replicationCoordinatorExternalState->getTaskExecutor(),
            _replCoord,
            lastOpTimeFetched,
            requiredOpTime,
            [&syncSourceResp](const SyncSourceResolverResponse& resp) { syncSourceResp = resp; });
    }
    // This may deadlock if called inside the mutex because SyncSourceResolver::startup() calls
    // ReplicationCoordinator::chooseNewSyncSource(). ReplicationCoordinatorImpl's mutex has to
    // acquired before BackgroundSync's.
    // It is safe to call startup() outside the mutex on this instance of SyncSourceResolver because
    // we do not destroy this instance outside of this function which is only called from a single
    // thread.
    auto status = _syncSourceResolver->startup();
    if (ErrorCodes::CallbackCanceled == status || ErrorCodes::isShutdownError(status.code())) {
        return;
    }
    fassert(40349, status);
    _syncSourceResolver->join();
    {
        stdx::lock_guard<Latch> lock(_mutex);
        _syncSourceResolver.reset();
    }

    numSyncSourceSelections.increment(1);

    if (syncSourceResp.syncSourceStatus == ErrorCodes::OplogStartMissing) {
        // All (accessible) sync sources are too far ahead of us.
        if (_replCoord->getMemberState().primary()) {
            warning() << "Too stale to catch up.";
            log() << "Our newest OpTime : " << lastOpTimeFetched;
            log() << "Earliest OpTime available is " << syncSourceResp.earliestOpTimeSeen
                  << " from " << syncSourceResp.getSyncSource();
            auto status = _replCoord->abortCatchupIfNeeded(
                ReplicationCoordinator::PrimaryCatchUpConclusionReason::kFailedWithError);
            if (!status.isOK()) {
                LOG(1) << "Aborting catch-up failed with status: " << status;
            }
            return;
        }

        if (_tooStale.swap(true)) {
            // We had already marked ourselves too stale.
            return;
        }

        // Need to take the RSTL in mode X to transition out of SECONDARY.
        auto opCtx = cc().makeOperationContext();
        ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

        error() << "too stale to catch up -- entering maintenance mode";
        log() << "Our newest OpTime : " << lastOpTimeFetched;
        log() << "Earliest OpTime available is " << syncSourceResp.earliestOpTimeSeen;
        log() << "See http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember";

        // Activate maintenance mode and transition to RECOVERING.
        auto status = _replCoord->setMaintenanceMode(true);
        if (!status.isOK()) {
            warning() << "Failed to transition into maintenance mode: " << status;
        }
        status = _replCoord->setFollowerMode(MemberState::RS_RECOVERING);
        if (!status.isOK()) {
            warning() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                      << ". Current state: " << _replCoord->getMemberState() << causedBy(status);
        }
        return;
    } else if (syncSourceResp.isOK() && !syncSourceResp.getSyncSource().empty()) {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            _syncSourceHost = syncSourceResp.getSyncSource();
            source = _syncSourceHost;
        }
        // If our sync source has not changed, it is likely caused by our heartbeat data map being
        // out of date. In that case we sleep for 1 second to reduce the amount we spin waiting
        // for our map to update.
        if (oldSource == source) {
            log() << "Chose same sync source candidate as last time, " << source
                  << ". Sleeping for 1 second to avoid immediately choosing a new sync source for "
                     "the same reason as last time.";
            numTimesChoseSameSyncSource.increment(1);
            sleepsecs(1);
        } else {
            log() << "Changed sync source from "
                  << (oldSource.empty() ? std::string("empty") : oldSource.toString()) << " to "
                  << source;
            numTimesChoseDifferentSyncSource.increment(1);
        }
    } else {
        if (!syncSourceResp.isOK()) {
            log() << "failed to find sync source, received error "
                  << syncSourceResp.syncSourceStatus.getStatus();
        }
        // No sync source found.
        LOG(1) << "Could not find a sync source. Sleeping for 1 second before trying again.";
        numTimesCouldNotFindSyncSource.increment(1);

        sleepsecs(1);
        return;
    }

    // If we find a good sync source after having gone too stale, disable maintenance mode so we can
    // transition to SECONDARY.
    if (_tooStale.swap(false)) {
        log() << "No longer too stale. Able to sync from " << source;
        auto status = _replCoord->setMaintenanceMode(false);
        if (!status.isOK()) {
            warning() << "Failed to leave maintenance mode: " << status;
        }
    }

    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        lastOpTimeFetched = _lastOpTimeFetched;
    }

    if (!_replCoord->getMemberState().primary()) {
        _replCoord->signalUpstreamUpdater();
    }

    // Set the applied point if unset. This is most likely the first time we've established a sync
    // source since stepping down or otherwise clearing the applied point. We need to set this here,
    // before the OplogWriter gets a chance to append to the oplog.
    {
        auto opCtx = cc().makeOperationContext();
        if (_replicationProcess->getConsistencyMarkers()->getAppliedThrough(opCtx.get()).isNull()) {
            _replicationProcess->getConsistencyMarkers()->setAppliedThrough(
                opCtx.get(), _replCoord->getMyLastAppliedOpTime());
        }
    }

    // "lastFetched" not used. Already set in _enqueueDocuments.
    Status fetcherReturnStatus = Status::OK();
    DataReplicatorExternalStateBackgroundSync dataReplicatorExternalState(
        _replCoord, _replicationCoordinatorExternalState, this);
    OplogFetcher* oplogFetcher;
    try {
        auto onOplogFetcherShutdownCallbackFn = [&fetcherReturnStatus](const Status& status) {
            fetcherReturnStatus = status;
        };
        // The construction of OplogFetcher has to be outside bgsync mutex, because it calls
        // replication coordinator.
        auto oplogFetcherPtr = std::make_unique<OplogFetcher>(
            _replicationCoordinatorExternalState->getTaskExecutor(),
            lastOpTimeFetched,
            source,
            NamespaceString::kRsOplogNamespace,
            _replCoord->getConfig(),
            _replicationCoordinatorExternalState->getOplogFetcherSteadyStateMaxFetcherRestarts(),
            syncSourceResp.rbid,
            true /* requireFresherSyncSource */,
            &dataReplicatorExternalState,
            [this](const auto& a1, const auto& a2, const auto& a3) {
                return this->_enqueueDocuments(a1, a2, a3);
            },
            onOplogFetcherShutdownCallbackFn,
            bgSyncOplogFetcherBatchSize);
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
        _oplogFetcher = std::move(oplogFetcherPtr);
        oplogFetcher = _oplogFetcher.get();
    } catch (const mongo::DBException&) {
        fassertFailedWithStatus(34440, exceptionToStatus());
    }

    const auto logLevel = getTestCommandsEnabled() ? 0 : 1;
    LOG(logLevel) << "scheduling fetcher to read remote oplog on " << source << " starting at "
                  << oplogFetcher->getFindQuery_forTest()["filter"];
    auto scheduleStatus = oplogFetcher->startup();
    if (!scheduleStatus.isOK()) {
        warning() << "unable to schedule fetcher to read remote oplog on " << source << ": "
                  << scheduleStatus;
        return;
    }

    oplogFetcher->join();
    LOG(1) << "fetcher stopped reading remote oplog on " << source;

    // If the background sync is stopped after the fetcher is started, we need to
    // re-evaluate our sync source and oplog common point.
    if (getState() != ProducerState::Running) {
        log() << "Replication producer stopped after oplog fetcher finished returning a batch from "
                 "our sync source.  Abandoning this batch of oplog entries and re-evaluating our "
                 "sync source.";
        return;
    }

    if (fetcherReturnStatus.code() == ErrorCodes::OplogOutOfOrder) {
        // This is bad because it means that our source
        // has not returned oplog entries in ascending ts order, and they need to be.

        warning() << redact(fetcherReturnStatus);
        // Do not blacklist the server here, it will be blacklisted when we try to reuse it,
        // if it can't return a matching oplog start from the last fetch oplog ts field.
        return;
    } else if (fetcherReturnStatus.code() == ErrorCodes::OplogStartMissing) {
        auto opCtx = cc().makeOperationContext();
        auto storageInterface = StorageInterface::get(opCtx.get());
        _runRollback(
            opCtx.get(), fetcherReturnStatus, source, syncSourceResp.rbid, storageInterface);
    } else if (fetcherReturnStatus == ErrorCodes::InvalidBSON) {
        Seconds blacklistDuration(60);
        warning() << "Fetcher got invalid BSON while querying oplog. Blacklisting sync source "
                  << source << " for " << blacklistDuration << ".";
        _replCoord->blacklistSyncSource(source, Date_t::now() + blacklistDuration);
    } else if (!fetcherReturnStatus.isOK()) {
        warning() << "Fetcher stopped querying remote oplog with error: "
                  << redact(fetcherReturnStatus);
    }
}

Status BackgroundSync::_enqueueDocuments(Fetcher::Documents::const_iterator begin,
                                         Fetcher::Documents::const_iterator end,
                                         const OplogFetcher::DocumentsInfo& info) {
    // If this is the first batch of operations returned from the query, "toApplyDocumentCount" will
    // be one fewer than "networkDocumentCount" because the first document (which was applied
    // previously) is skipped.
    if (info.toApplyDocumentCount == 0) {
        return Status::OK();  // Nothing to do.
    }

    auto opCtx = cc().makeOperationContext();

    // Wait for enough space.
    _oplogApplier->waitForSpace(opCtx.get(), info.toApplyDocumentBytes);

    {
        // Don't add more to the buffer if we are in shutdown. Continue holding the lock until we
        // are done to prevent going into shutdown.
        stdx::unique_lock<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return Status::OK();
        }

        // Buffer docs for later application.
        _oplogApplier->enqueue(opCtx.get(), begin, end);

        // Update last fetched info.
        _lastOpTimeFetched = info.lastDocument;
        LOG(3) << "batch resetting _lastOpTimeFetched: " << _lastOpTimeFetched;
    }

    // Check some things periodically (whenever we run out of items in the current cursor batch).
    if (info.networkDocumentBytes > 0 && info.networkDocumentBytes < kSmallBatchLimitBytes) {
        // On a very low latency network, if we don't wait a little, we'll be
        // getting ops to write almost one at a time.  This will both be expensive
        // for the upstream server as well as potentially defeating our parallel
        // application of batches on the secondary.
        //
        // The inference here is basically if the batch is really small, we are "caught up".
        sleepmillis(kSleepToAllowBatchingMillis);
    }

    return Status::OK();
}

void BackgroundSync::_runRollback(OperationContext* opCtx,
                                  const Status& fetcherReturnStatus,
                                  const HostAndPort& source,
                                  int requiredRBID,
                                  StorageInterface* storageInterface) {
    if (_replCoord->getMemberState().primary()) {
        warning() << "Rollback situation detected in catch-up mode. Aborting catch-up mode.";
        auto status = _replCoord->abortCatchupIfNeeded(
            ReplicationCoordinator::PrimaryCatchUpConclusionReason::kFailedWithError);
        if (!status.isOK()) {
            LOG(1) << "Aborting catch-up failed with status: " << status;
        }
        return;
    }

    ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(opCtx->lockState());

    // Explicitly start future read transactions without a timestamp.
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);

    // Rollback is a synchronous operation that uses the task executor and may not be
    // executed inside the fetcher callback.

    OpTime lastOpTimeFetched;
    {
        stdx::lock_guard<Latch> lock(_mutex);
        lastOpTimeFetched = _lastOpTimeFetched;
    }

    log() << "Starting rollback due to " << redact(fetcherReturnStatus);
    log() << "Replication commit point: " << _replCoord->getLastCommittedOpTime();

    // TODO: change this to call into the Applier directly to block until the applier is
    // drained.
    //
    // Wait till all buffered oplog entries have drained and been applied.
    auto lastApplied = _replCoord->getMyLastAppliedOpTime();
    if (lastApplied != lastOpTimeFetched) {
        log() << "Waiting for all operations from " << lastApplied << " until " << lastOpTimeFetched
              << " to be applied before starting rollback.";
        while (lastOpTimeFetched > (lastApplied = _replCoord->getMyLastAppliedOpTime())) {
            sleepmillis(10);
            if (getState() != ProducerState::Running) {
                return;
            }
        }
    }

    if (MONGO_unlikely(rollbackHangBeforeStart.shouldFail())) {
        // This log output is used in js tests so please leave it.
        log() << "rollback - rollbackHangBeforeStart fail point "
                 "enabled. Blocking until fail point is disabled.";
        while (MONGO_unlikely(rollbackHangBeforeStart.shouldFail()) && !inShutdown()) {
            mongo::sleepsecs(1);
        }
    }

    OplogInterfaceLocal localOplog(opCtx);

    const int messagingPortTags = 0;
    ConnectionPool connectionPool(messagingPortTags);
    std::unique_ptr<ConnectionPool::ConnectionPtr> connection;
    auto getConnection = [&connection, &connectionPool, source]() -> DBClientBase* {
        if (!connection.get()) {
            connection.reset(new ConnectionPool::ConnectionPtr(
                &connectionPool, source, Date_t::now(), kRollbackOplogSocketTimeout));
        };
        return connection->get();
    };

    // Because oplog visibility is updated asynchronously, wait until all uncommitted oplog entries
    // are visible before potentially truncating the oplog.
    storageInterface->waitForAllEarlierOplogWritesToBeVisible(opCtx);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    if (!forceRollbackViaRefetch.load() && storageEngine->supportsRecoverToStableTimestamp()) {
        log() << "Rollback using 'recoverToStableTimestamp' method.";
        _runRollbackViaRecoverToCheckpoint(
            opCtx, source, &localOplog, storageInterface, getConnection);
    } else {
        log() << "Rollback using the 'rollbackViaRefetch' method.";
        _fallBackOnRollbackViaRefetch(opCtx, source, requiredRBID, &localOplog, getConnection);
    }

    // Reset the producer to clear the sync source and the last optime fetched.
    stop(true);
    startProducerIfStopped();
}

void BackgroundSync::_runRollbackViaRecoverToCheckpoint(
    OperationContext* opCtx,
    const HostAndPort& source,
    OplogInterface* localOplog,
    StorageInterface* storageInterface,
    OplogInterfaceRemote::GetConnectionFn getConnection) {

    OplogInterfaceRemote remoteOplog(source,
                                     getConnection,
                                     NamespaceString::kRsOplogNamespace.ns(),
                                     rollbackRemoteOplogQueryBatchSize.load());

    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != ProducerState::Running) {
            return;
        }
    }

    _rollback = std::make_unique<RollbackImpl>(
        localOplog, &remoteOplog, storageInterface, _replicationProcess, _replCoord);

    log() << "Scheduling rollback (sync source: " << source << ")";
    auto status = _rollback->runRollback(opCtx);
    if (status.isOK()) {
        log() << "Rollback successful.";
    } else if (status == ErrorCodes::UnrecoverableRollbackError) {
        severe() << "Rollback failed with unrecoverable error: " << status;
        fassertFailedWithStatusNoTrace(50666, status);
    } else {
        warning() << "Rollback failed with retryable error: " << status;
    }
}

void BackgroundSync::_fallBackOnRollbackViaRefetch(
    OperationContext* opCtx,
    const HostAndPort& source,
    int requiredRBID,
    OplogInterface* localOplog,
    OplogInterfaceRemote::GetConnectionFn getConnection) {

    RollbackSourceImpl rollbackSource(getConnection,
                                      source,
                                      NamespaceString::kRsOplogNamespace.ns(),
                                      rollbackRemoteOplogQueryBatchSize.load());

    rollback(opCtx, *localOplog, rollbackSource, requiredRBID, _replCoord, _replicationProcess);
}

HostAndPort BackgroundSync::getSyncTarget() const {
    stdx::unique_lock<Latch> lock(_mutex);
    return _syncSourceHost;
}

void BackgroundSync::clearSyncTarget() {
    stdx::unique_lock<Latch> lock(_mutex);
    log() << "Resetting sync source to empty, which was " << _syncSourceHost;
    _syncSourceHost = HostAndPort();
}

void BackgroundSync::stop(bool resetLastFetchedOptime) {
    stdx::lock_guard<Latch> lock(_mutex);

    _state = ProducerState::Stopped;
    log() << "Stopping replication producer";

    _syncSourceHost = HostAndPort();
    if (resetLastFetchedOptime) {
        invariant(_oplogApplier->getBuffer()->isEmpty());
        _lastOpTimeFetched = OpTime();
        log() << "Resetting last fetched optimes in bgsync";
    }

    if (_syncSourceResolver) {
        _syncSourceResolver->shutdown();
    }

    if (_oplogFetcher) {
        _oplogFetcher->shutdown();
    }
}

void BackgroundSync::start(OperationContext* opCtx) {
    OpTime lastAppliedOpTime;
    ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(opCtx->lockState());

    // Explicitly start future read transactions without a timestamp.
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);

    do {
        lastAppliedOpTime = _readLastAppliedOpTime(opCtx);
        stdx::lock_guard<Latch> lk(_mutex);
        // Double check the state after acquiring the mutex.
        if (_state != ProducerState::Starting) {
            return;
        }
        // If a node steps down during drain mode, then the buffer may not be empty at the beginning
        // of secondary state.
        if (!_oplogApplier->getBuffer()->isEmpty()) {
            log() << "going to start syncing, but buffer is not empty";
        }
        _state = ProducerState::Running;

        // When a node steps down during drain mode, the last fetched optime would be newer than
        // the last applied.
        if (_lastOpTimeFetched <= lastAppliedOpTime) {
            LOG(1) << "Setting bgsync _lastOpTimeFetched=" << lastAppliedOpTime
                   << ". Previous _lastOpTimeFetched: " << _lastOpTimeFetched;
            _lastOpTimeFetched = lastAppliedOpTime;
        }
        // Reload the last applied optime from disk if it has been changed.
    } while (lastAppliedOpTime != _replCoord->getMyLastAppliedOpTime());

    LOG(1) << "bgsync fetch queue set to: " << _lastOpTimeFetched;
}

OpTime BackgroundSync::_readLastAppliedOpTime(OperationContext* opCtx) {
    BSONObj oplogEntry;
    try {
        bool success = writeConflictRetry(
            opCtx, "readLastAppliedOpTime", NamespaceString::kRsOplogNamespace.ns(), [&] {
                Lock::DBLock lk(opCtx, "local", MODE_X);
                return Helpers::getLast(
                    opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), oplogEntry);
            });

        if (!success) {
            // This can happen when we are to do an initial sync.
            return OpTime();
        }
    } catch (const ExceptionForCat<ErrorCategory::ShutdownError>&) {
        throw;
    } catch (const DBException& ex) {
        severe() << "Problem reading " << NamespaceString::kRsOplogNamespace.ns() << ": "
                 << redact(ex);
        fassertFailed(18904);
    }

    OplogEntry parsedEntry(oplogEntry);
    LOG(1) << "Successfully read last entry of oplog while starting bgsync: " << redact(oplogEntry);
    return parsedEntry.getOpTime();
}

bool BackgroundSync::shouldStopFetching() const {
    // Check if we have been stopped.
    if (getState() != ProducerState::Running) {
        LOG(2) << "Stopping oplog fetcher due to stop request.";
        return true;
    }

    // Check current sync source.
    if (getSyncTarget().empty()) {
        LOG(1) << "Stopping oplog fetcher; canceling oplog query because we have no valid sync "
                  "source.";
        return true;
    }

    return false;
}

BackgroundSync::ProducerState BackgroundSync::getState() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _state;
}

void BackgroundSync::startProducerIfStopped() {
    stdx::lock_guard<Latch> lock(_mutex);
    // Let producer run if it's already running.
    if (_state == ProducerState::Stopped) {
        _state = ProducerState::Starting;
    }
}


}  // namespace repl
}  // namespace mongo
