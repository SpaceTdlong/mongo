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

#include "mongo/db/repl/oplog_applier_impl.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/insert_group.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/platform/basic.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "third_party/murmurhash3/MurmurHash3.h"

namespace mongo {
namespace repl {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationAfterWritingOplogEntries);
MONGO_FAIL_POINT_DEFINE(hangAfterRecordingOpApplicationStartTime);

// The oplog entries applied
Counter64 opsAppliedStats;
ServerStatusMetricField<Counter64> displayOpsApplied("repl.apply.ops", &opsAppliedStats);

// Tracks the oplog application batch size.
Counter64 oplogApplicationBatchSize;
ServerStatusMetricField<Counter64> displayOplogApplicationBatchSize("repl.apply.batchSize",
                                                                    &oplogApplicationBatchSize);
// Number and time of each ApplyOps worker pool round
TimerStats applyBatchStats;
ServerStatusMetricField<TimerStats> displayOpBatchesApplied("repl.apply.batches", &applyBatchStats);

NamespaceString parseUUIDOrNs(OperationContext* opCtx, const OplogEntry& oplogEntry) {
    auto optionalUuid = oplogEntry.getUuid();
    if (!optionalUuid) {
        return oplogEntry.getNss();
    }

    const auto& uuid = optionalUuid.get();
    auto& catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(uuid);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "No namespace with UUID " << uuid.toString(),
            nss);
    return *nss;
}

NamespaceStringOrUUID getNsOrUUID(const NamespaceString& nss, const OplogEntry& op) {
    if (auto ui = op.getUuid()) {
        return {nss.db().toString(), ui.get()};
    }
    return nss;
}

/**
 * Used for logging a report of ops that take longer than "slowMS" to apply. This is called
 * right before returning from applyOplogEntryBatch, and it returns the same status.
 */
Status finishAndLogApply(ClockSource* clockSource,
                         Status finalStatus,
                         Date_t applyStartTime,
                         const OplogEntryBatch& batch) {

    if (finalStatus.isOK()) {
        auto applyEndTime = clockSource->now();
        auto diffMS = durationCount<Milliseconds>(applyEndTime - applyStartTime);

        // This op was slow to apply, so we should log a report of it.
        if (diffMS > serverGlobalParams.slowMS) {

            StringBuilder s;
            s << "applied op: ";

            if (batch.getOp().getOpType() == OpTypeEnum::kCommand) {
                s << "command ";
            } else {
                s << "CRUD ";
            }

            s << redact(batch.toBSON());
            s << ", took " << diffMS << "ms";

            log() << s.str();
        }
    }
    return finalStatus;
}

LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode) {
    return nss.isSystemDotViews() ? MODE_X : mode;
}

/**
 * Caches per-collection properties which are relevant for oplog application, so that they don't
 * have to be retrieved repeatedly for each op.
 */
class CachedCollectionProperties {
public:
    struct CollectionProperties {
        bool isCapped = false;
        const CollatorInterface* collator = nullptr;
    };

    CollectionProperties getCollectionProperties(OperationContext* opCtx,
                                                 const StringMapHashedKey& ns) {
        auto it = _cache.find(ns);
        if (it != _cache.end()) {
            return it->second;
        }

        auto collProperties = getCollectionPropertiesImpl(opCtx, NamespaceString(ns.key()));
        _cache[ns] = collProperties;
        return collProperties;
    }

private:
    CollectionProperties getCollectionPropertiesImpl(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
        CollectionProperties collProperties;

        auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(nss);

        if (!collection) {
            return collProperties;
        }

        collProperties.isCapped = collection->isCapped();
        collProperties.collator = collection->getDefaultCollator();
        return collProperties;
    }

    StringMap<CollectionProperties> _cache;
};

/**
 * Updates a CRUD op's hash and isForCappedCollection field if necessary.
 */
void processCrudOp(OperationContext* opCtx,
                   OplogEntry* op,
                   uint32_t* hash,
                   StringMapHashedKey* hashedNs,
                   CachedCollectionProperties* collPropertiesCache) {
    const bool supportsDocLocking =
        opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking();
    auto collProperties = collPropertiesCache->getCollectionProperties(opCtx, *hashedNs);

    // For doc locking engines, include the _id of the document in the hash so we get
    // parallelism even if all writes are to a single collection.
    //
    // For capped collections, this is illegal, since capped collections must preserve
    // insertion order.
    if (supportsDocLocking && !collProperties.isCapped) {
        BSONElement id = op->getIdElement();
        BSONElementComparator elementHasher(BSONElementComparator::FieldNamesMode::kIgnore,
                                            collProperties.collator);
        const size_t idHash = elementHasher.hash(id);
        MurmurHash3_x86_32(&idHash, sizeof(idHash), *hash, hash);
    }

    if (op->getOpType() == OpTypeEnum::kInsert && collProperties.isCapped) {
        // Mark capped collection ops before storing them to ensure we do not attempt to
        // bulk insert them.
        op->isForCappedCollection = true;
    }
}

/**
 * Adds a single oplog entry to the appropriate writer vector.
 */
void addToWriterVector(OplogEntry* op,
                       std::vector<MultiApplier::OperationPtrs>* writerVectors,
                       uint32_t hash) {
    const uint32_t numWriters = writerVectors->size();
    auto& writer = (*writerVectors)[hash % numWriters];
    if (writer.empty()) {
        writer.reserve(8);  // Skip a few growth rounds
    }
    writer.push_back(op);
}

/**
 * Adds a set of derivedOps to writerVectors.
 */
void addDerivedOps(OperationContext* opCtx,
                   MultiApplier::Operations* derivedOps,
                   std::vector<MultiApplier::OperationPtrs>* writerVectors,
                   CachedCollectionProperties* collPropertiesCache) {
    for (auto&& op : *derivedOps) {
        auto hashedNs = StringMapHasher().hashed_key(op.getNss().ns());
        uint32_t hash = static_cast<uint32_t>(hashedNs.hash());
        if (op.isCrudOpType()) {
            processCrudOp(opCtx, &op, &hash, &hashedNs, collPropertiesCache);
        }
        addToWriterVector(&op, writerVectors, hash);
    }
}

void stableSortByNamespace(MultiApplier::OperationPtrs* oplogEntryPointers) {
    auto nssComparator = [](const OplogEntry* l, const OplogEntry* r) {
        return l->getNss() < r->getNss();
    };
    std::stable_sort(oplogEntryPointers->begin(), oplogEntryPointers->end(), nssComparator);
}

}  // namespace


namespace {

class ApplyBatchFinalizer {
public:
    ApplyBatchFinalizer(ReplicationCoordinator* replCoord) : _replCoord(replCoord) {}
    virtual ~ApplyBatchFinalizer(){};

    virtual void record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        _recordApplied(newOpTimeAndWallTime, consistency);
    };

protected:
    void _recordApplied(const OpTimeAndWallTime& newOpTimeAndWallTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        // We have to use setMyLastAppliedOpTimeAndWallTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastAppliedOpTimeAndWallTimeForward(newOpTimeAndWallTime, consistency);
    }

    void _recordDurable(const OpTimeAndWallTime& newOpTimeAndWallTime) {
        // We have to use setMyLastDurableOpTimeAndWallTimeFoward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastDurableOpTimeAndWallTimeForward(newOpTimeAndWallTime);
    }

private:
    // Used to update the replication system's progress.
    ReplicationCoordinator* _replCoord;
};

class ApplyBatchFinalizerForJournal : public ApplyBatchFinalizer {
public:
    ApplyBatchFinalizerForJournal(ReplicationCoordinator* replCoord)
        : ApplyBatchFinalizer(replCoord),
          _waiterThread{&ApplyBatchFinalizerForJournal::_run, this} {};
    ~ApplyBatchFinalizerForJournal();

    void record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                ReplicationCoordinator::DataConsistency consistency) override;

private:
    /**
     * Loops continuously, waiting for writes to be flushed to disk and then calls
     * ReplicationCoordinator::setMyLastOptime with _latestOpTime.
     * Terminates once _shutdownSignaled is set true.
     */
    void _run();

    // Protects _cond, _shutdownSignaled, and _latestOpTime.
    Mutex _mutex = MONGO_MAKE_LATCH("OplogApplierImpl::_mutex");
    // Used to alert our thread of a new OpTime.
    stdx::condition_variable _cond;
    // The next OpTime to set as the ReplicationCoordinator's lastOpTime after flushing.
    OpTimeAndWallTime _latestOpTimeAndWallTime;
    // Once this is set to true the _run method will terminate.
    bool _shutdownSignaled = false;
    // Thread that will _run(). Must be initialized last as it depends on the other variables.
    stdx::thread _waiterThread;
};

ApplyBatchFinalizerForJournal::~ApplyBatchFinalizerForJournal() {
    stdx::unique_lock<Latch> lock(_mutex);
    _shutdownSignaled = true;
    _cond.notify_all();
    lock.unlock();

    _waiterThread.join();
}

void ApplyBatchFinalizerForJournal::record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                                           ReplicationCoordinator::DataConsistency consistency) {
    _recordApplied(newOpTimeAndWallTime, consistency);

    stdx::unique_lock<Latch> lock(_mutex);
    _latestOpTimeAndWallTime = newOpTimeAndWallTime;
    _cond.notify_all();
}

void ApplyBatchFinalizerForJournal::_run() {
    Client::initThread("ApplyBatchFinalizerForJournal");

    while (true) {
        OpTimeAndWallTime latestOpTimeAndWallTime = {OpTime(), Date_t()};

        {
            stdx::unique_lock<Latch> lock(_mutex);
            while (_latestOpTimeAndWallTime.opTime.isNull() && !_shutdownSignaled) {
                _cond.wait(lock);
            }

            if (_shutdownSignaled) {
                return;
            }

            latestOpTimeAndWallTime = _latestOpTimeAndWallTime;
            _latestOpTimeAndWallTime = {OpTime(), Date_t()};
        }

        auto opCtx = cc().makeOperationContext();
        opCtx->recoveryUnit()->waitUntilDurable(opCtx.get());
        _recordDurable(latestOpTimeAndWallTime);
    }
}

}  // namespace

OplogApplierImpl::OplogApplierImpl(executor::TaskExecutor* executor,
                                   OplogBuffer* oplogBuffer,
                                   Observer* observer,
                                   ReplicationCoordinator* replCoord,
                                   ReplicationConsistencyMarkers* consistencyMarkers,
                                   StorageInterface* storageInterface,
                                   const OplogApplier::Options& options,
                                   ThreadPool* writerPool)
    : OplogApplier(executor, oplogBuffer, observer, options),
      _replCoord(replCoord),
      _writerPool(writerPool),
      _storageInterface(storageInterface),
      _consistencyMarkers(consistencyMarkers),
      _beginApplyingOpTime(options.beginApplyingOpTime) {}

void OplogApplierImpl::_run(OplogBuffer* oplogBuffer) {
    auto getNextApplierBatchFn = [this](OperationContext* opCtx, const BatchLimits& batchLimits) {
        return getNextApplierBatch(opCtx, batchLimits);
    };

    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!_replCoord->getMemberState().arbiter());

    OpQueueBatcher batcher(this, _storageInterface, oplogBuffer, getNextApplierBatchFn);

    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getStorageEngine()->isDurable()
            ? new ApplyBatchFinalizerForJournal(_replCoord)
            : new ApplyBatchFinalizer(_replCoord)};

    while (true) {  // Exits on message from OpQueueBatcher.
        // Use a new operation context each iteration, as otherwise we may appear to use a single
        // collection name to refer to collections with different UUIDs.
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        // This code path gets used during elections, so it should not be subject to Flow Control.
        // It is safe to exclude this operation context from Flow Control here because this code
        // path only gets used on secondaries or on a node transitioning to primary.
        opCtx.setShouldParticipateInFlowControl(false);

        // For pausing replication in tests.
        if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
            log() << "Oplog Applier - rsSyncApplyStop fail point enabled. Blocking until fail "
                     "point is disabled.";
            rsSyncApplyStop.pauseWhileSet(&opCtx);
        }

        // Transition to SECONDARY state, if possible.
        _replCoord->finishRecoveryIfEligible(&opCtx);

        // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
        // ready in time, we'll loop again so we can do the above checks periodically.
        OpQueue ops = batcher.getNextBatch(Seconds(1));
        if (ops.empty()) {
            if (ops.mustShutdown()) {
                // Shut down and exit oplog application loop.
                return;
            }
            if (MONGO_unlikely(rsSyncApplyStop.shouldFail())) {
                continue;
            }
            if (ops.termWhenExhausted()) {
                // Signal drain complete if we're in Draining state and the buffer is empty.
                // Since we check the states of batcher and oplog buffer without synchronization,
                // they can be stale. We make sure the applier is still draining in the given term
                // before and after the check, so that if the oplog buffer was exhausted, then
                // it still will be.
                _replCoord->signalDrainComplete(&opCtx, *ops.termWhenExhausted());
            }
            continue;  // Try again.
        }

        // Extract some info from ops that we'll need after releasing the batch below.
        const auto firstOpTimeInBatch = ops.front().getOpTime();
        const auto lastOpInBatch = ops.back();
        const auto lastOpTimeInBatch = lastOpInBatch.getOpTime();
        const auto lastWallTimeInBatch = lastOpInBatch.getWallClockTime();
        const auto lastAppliedOpTimeAtStartOfBatch = _replCoord->getMyLastAppliedOpTime();

        // Make sure the oplog doesn't go back in time or repeat an entry.
        if (firstOpTimeInBatch <= lastAppliedOpTimeAtStartOfBatch) {
            fassert(34361,
                    Status(ErrorCodes::OplogOutOfOrder,
                           str::stream() << "Attempted to apply an oplog entry ("
                                         << firstOpTimeInBatch.toString()
                                         << ") which is not greater than our last applied OpTime ("
                                         << lastAppliedOpTimeAtStartOfBatch.toString() << ")."));
        }

        // Don't allow the fsync+lock thread to see intermediate states of batch application.
        stdx::lock_guard<SimpleMutex> fsynclk(filesLockedFsync);

        // Apply the operations in this batch. '_multiApply' returns the optime of the last op that
        // was applied, which should be the last optime in the batch.
        auto lastOpTimeAppliedInBatch =
            fassertNoTrace(34437, _multiApply(&opCtx, ops.releaseBatch()));
        invariant(lastOpTimeAppliedInBatch == lastOpTimeInBatch);

        // Update various things that care about our last applied optime. Tests rely on 1 happening
        // before 2 even though it isn't strictly necessary.

        // 1. Persist our "applied through" optime to disk.
        _consistencyMarkers->setAppliedThrough(&opCtx, lastOpTimeInBatch);

        // 2. Ensure that the last applied op time hasn't changed since the start of this batch.
        const auto lastAppliedOpTimeAtEndOfBatch = _replCoord->getMyLastAppliedOpTime();
        invariant(lastAppliedOpTimeAtStartOfBatch == lastAppliedOpTimeAtEndOfBatch,
                  str::stream() << "the last known applied OpTime has changed from "
                                << lastAppliedOpTimeAtStartOfBatch.toString() << " to "
                                << lastAppliedOpTimeAtEndOfBatch.toString()
                                << " in the middle of batch application");

        // 3. Update oplog visibility by notifying the storage engine of the new oplog entries.
        const bool orderedCommit = true;
        _storageInterface->oplogDiskLocRegister(
            &opCtx, lastOpTimeInBatch.getTimestamp(), orderedCommit);

        // 4. Finalize this batch. We are at a consistent optime if our current optime is >= the
        // current 'minValid' optime. Note that recording the lastOpTime in the finalizer includes
        // advancing the global timestamp to at least its timestamp.
        const auto minValid = _consistencyMarkers->getMinValid(&opCtx);
        auto consistency = (lastOpTimeInBatch >= minValid)
            ? ReplicationCoordinator::DataConsistency::Consistent
            : ReplicationCoordinator::DataConsistency::Inconsistent;

        // The finalizer advances the global timestamp to lastOpTimeInBatch.
        finalizer->record({lastOpTimeInBatch, lastWallTimeInBatch}, consistency);
    }
}


// Schedules the writes to the oplog for 'ops' into threadPool. The caller must guarantee that
// 'ops' stays valid until all scheduled work in the thread pool completes.
void scheduleWritesToOplog(OperationContext* opCtx,
                           StorageInterface* storageInterface,
                           ThreadPool* writerPool,
                           const MultiApplier::Operations& ops) {
    auto makeOplogWriterForRange = [storageInterface, &ops](size_t begin, size_t end) {
        // The returned function will be run in a separate thread after this returns. Therefore all
        // captures other than 'ops' must be by value since they will not be available. The caller
        // guarantees that 'ops' will stay in scope until the spawned threads complete.
        return [storageInterface, &ops, begin, end](auto status) {
            invariant(status);

            auto opCtx = cc().makeOperationContext();

            // This code path is only executed on secondaries and initial syncing nodes, so it is
            // safe to exclude any writes from Flow Control.
            opCtx->setShouldParticipateInFlowControl(false);

            UnreplicatedWritesBlock uwb(opCtx.get());
            ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                opCtx->lockState());

            std::vector<InsertStatement> docs;
            docs.reserve(end - begin);
            for (size_t i = begin; i < end; i++) {
                // Add as unowned BSON to avoid unnecessary ref-count bumps.
                // 'ops' will outlive 'docs' so the BSON lifetime will be guaranteed.
                docs.emplace_back(InsertStatement{ops[i].getRaw(),
                                                  ops[i].getOpTime().getTimestamp(),
                                                  ops[i].getOpTime().getTerm()});
            }

            fassert(40141,
                    storageInterface->insertDocuments(
                        opCtx.get(), NamespaceString::kRsOplogNamespace, docs));
        };
    };

    // We want to be able to take advantage of bulk inserts so we don't use multiple threads if it
    // would result too little work per thread. This also ensures that we can amortize the
    // setup/teardown overhead across many writes.
    const size_t kMinOplogEntriesPerThread = 16;
    const bool enoughToMultiThread =
        ops.size() >= kMinOplogEntriesPerThread * writerPool->getStats().numThreads;

    // Only doc-locking engines support parallel writes to the oplog because they are required to
    // ensure that oplog entries are ordered correctly, even if inserted out-of-order. Additionally,
    // there would be no way to take advantage of multiple threads if a storage engine doesn't
    // support document locking.
    if (!enoughToMultiThread ||
        !opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {

        writerPool->schedule(makeOplogWriterForRange(0, ops.size()));
        return;
    }


    const size_t numOplogThreads = writerPool->getStats().numThreads;
    const size_t numOpsPerThread = ops.size() / numOplogThreads;
    for (size_t thread = 0; thread < numOplogThreads; thread++) {
        size_t begin = thread * numOpsPerThread;
        size_t end = (thread == numOplogThreads - 1) ? ops.size() : begin + numOpsPerThread;
        writerPool->schedule(makeOplogWriterForRange(begin, end));
    }
}

StatusWith<OpTime> OplogApplierImpl::_multiApply(OperationContext* opCtx,
                                                 MultiApplier::Operations ops) {
    invariant(!ops.empty());

    LOG(2) << "replication batch size is " << ops.size();

    // Stop all readers until we're done. This also prevents doc-locking engines from deleting old
    // entries from the oplog until we finish writing.
    Lock::ParallelBatchWriterMode pbwm(opCtx->lockState());

    // TODO (SERVER-42996): This is a temporary invariant to protect against segfaults. This will
    // be removed once ApplierState is moved from ReplicationCoordinator to OplogApplier.
    invariant(_replCoord);
    if (_replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Stopped) {
        severe() << "attempting to replicate ops while primary";
        return {ErrorCodes::CannotApplyOplogWhilePrimary,
                "attempting to replicate ops while primary"};
    }

    // Increment the batch size stat.
    oplogApplicationBatchSize.increment(ops.size());

    std::vector<WorkerMultikeyPathInfo> multikeyVector(_writerPool->getStats().numThreads);
    {
        // Each node records cumulative batch application stats for itself using this timer.
        TimerHolder timer(&applyBatchStats);

        // We must wait for the all work we've dispatched to complete before leaving this block
        // because the spawned threads refer to objects on the stack
        ON_BLOCK_EXIT([&] { _writerPool->waitForIdle(); });

        // Write batch of ops into oplog.
        if (!getOptions().skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, ops.front().getTimestamp());
            scheduleWritesToOplog(opCtx, _storageInterface, _writerPool, ops);
        }

        // Holds 'pseudo operations' generated by secondaries to aid in replication.
        // Keep in scope until all operations in 'ops' and 'derivedOps' have been applied.
        // Pseudo operations include:
        // - applyOps operations expanded to individual ops.
        // - ops to update config.transactions. Normal writes to config.transactions in the
        //   primary don't create an oplog entry, so extract info from writes with transactions
        //   and create a pseudo oplog.
        std::vector<MultiApplier::Operations> derivedOps;

        std::vector<MultiApplier::OperationPtrs> writerVectors(_writerPool->getStats().numThreads);
        fillWriterVectors(opCtx, &ops, &writerVectors, &derivedOps);

        // Wait for writes to finish before applying ops.
        _writerPool->waitForIdle();

        // Use this fail point to hold the PBWM lock after we have written the oplog entries but
        // before we have applied them.
        if (MONGO_unlikely(pauseBatchApplicationAfterWritingOplogEntries.shouldFail())) {
            log() << "pauseBatchApplicationAfterWritingOplogEntries fail point enabled. Blocking "
                     "until fail point is disabled.";
            pauseBatchApplicationAfterWritingOplogEntries.pauseWhileSet(opCtx);
        }

        // Reset consistency markers in case the node fails while applying ops.
        if (!getOptions().skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
            _consistencyMarkers->setMinValidToAtLeast(opCtx, ops.back().getOpTime());
        }

        {
            std::vector<Status> statusVector(_writerPool->getStats().numThreads, Status::OK());

            // Doles out all the work to the writer pool threads. writerVectors is not modified,
            // but  applyOplogGroup will modify the vectors that it contains.
            invariant(writerVectors.size() == statusVector.size());
            for (size_t i = 0; i < writerVectors.size(); i++) {
                if (writerVectors[i].empty())
                    continue;

                _writerPool->schedule(
                    [this,
                     &writer = writerVectors.at(i),
                     &status = statusVector.at(i),
                     &multikeyVector = multikeyVector.at(i)](auto scheduleStatus) {
                        invariant(scheduleStatus);

                        auto opCtx = cc().makeOperationContext();

                        // This code path is only executed on secondaries and initial syncing nodes,
                        // so it is safe to exclude any writes from Flow Control.
                        opCtx->setShouldParticipateInFlowControl(false);

                        status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
                            [&] { return applyOplogGroup(opCtx.get(), &writer, &multikeyVector); });
                    });
            }

            _writerPool->waitForIdle();

            // If any of the statuses is not ok, return error.
            for (auto it = statusVector.cbegin(); it != statusVector.cend(); ++it) {
                const auto& status = *it;
                if (!status.isOK()) {
                    severe()
                        << "Failed to apply batch of operations. Number of operations in batch: "
                        << ops.size() << ". First operation: " << redact(ops.front().toBSON())
                        << ". Last operation: " << redact(ops.back().toBSON())
                        << ". Oplog application failed in writer thread "
                        << std::distance(statusVector.cbegin(), it) << ": " << redact(status);
                    return status;
                }
            }
        }
    }

    // Notify the storage engine that a replication batch has completed. This means that all the
    // writes associated with the oplog entries in the batch are finished and no new writes with
    // timestamps associated with those oplog entries will show up in the future.
    const auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->replicationBatchIsComplete();

    // Use this fail point to hold the PBWM lock and prevent the batch from completing.
    if (MONGO_unlikely(pauseBatchApplicationBeforeCompletion.shouldFail())) {
        log() << "pauseBatchApplicationBeforeCompletion fail point enabled. Blocking until fail "
                 "point is disabled.";
        while (MONGO_unlikely(pauseBatchApplicationBeforeCompletion.shouldFail())) {
            if (inShutdown()) {
                severe() << "Turn off pauseBatchApplicationBeforeCompletion before attempting "
                            "clean shutdown";
                fassertFailedNoTrace(50798);
            }
            sleepmillis(100);
        }
    }

    Timestamp firstTimeInBatch = ops.front().getTimestamp();
    // Set any indexes to multikey that this batch ignored. This must be done while holding the
    // parallel batch writer mode lock.
    for (WorkerMultikeyPathInfo infoVector : multikeyVector) {
        for (MultikeyPathInfo info : infoVector) {
            // We timestamp every multikey write with the first timestamp in the batch. It is always
            // safe to set an index as multikey too early, just not too late. We conservatively pick
            // the first timestamp in the batch since we do not have enough information to find out
            // the timestamp of the first write that set the given multikey path.
            fassert(50686,
                    _storageInterface->setIndexIsMultikey(
                        opCtx, info.nss, info.indexName, info.multikeyPaths, firstTimeInBatch));
        }
    }

    // Increment the counter for the number of ops applied during catchup if the node is in catchup
    // mode.
    _replCoord->incrementNumCatchUpOpsIfCatchingUp(ops.size());

    // We have now written all database writes and updated the oplog to match.
    return ops.back().getOpTime();
}

/**
 * ops - This only modifies the isForCappedCollection field on each op. It does not alter the ops
 *      vector in any other way.
 * writerVectors - Set of operations for each worker thread to apply.
 * derivedOps - If provided, this function inserts a decomposition of applyOps operations
 *      and instructions for updating the transactions table.  Required if processing oplogs
 *      with transactions.
 * sessionUpdateTracker - if provided, keeps track of session info from ops.
 */
void OplogApplierImpl::_deriveOpsAndFillWriterVectors(
    OperationContext* opCtx,
    MultiApplier::Operations* ops,
    std::vector<MultiApplier::OperationPtrs>* writerVectors,
    std::vector<MultiApplier::Operations>* derivedOps,
    SessionUpdateTracker* sessionUpdateTracker) noexcept {

    LogicalSessionIdMap<std::vector<OplogEntry*>> partialTxnOps;
    CachedCollectionProperties collPropertiesCache;
    for (auto&& op : *ops) {
        // If the operation's optime is before or the same as the beginApplyingOpTime we don't want
        // to apply it, so don't include it in writerVectors.
        if (op.getOpTime() <= getOptions().beginApplyingOpTime) {
            continue;
        }

        auto hashedNs = StringMapHasher().hashed_key(op.getNss().ns());
        // Reduce the hash from 64bit down to 32bit, just to allow combinations with murmur3 later
        // on. Bit depth not important, we end up just doing integer modulo with this in the end.
        // The hash function should provide entropy in the lower bits as it's used in hash tables.
        uint32_t hash = static_cast<uint32_t>(hashedNs.hash());

        // We need to track all types of ops, including type 'n' (these are generated from chunk
        // migrations).
        if (sessionUpdateTracker) {
            if (auto newOplogWrites = sessionUpdateTracker->updateSession(op)) {
                derivedOps->emplace_back(std::move(*newOplogWrites));
                addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            }
        }


        // If this entry is part of a multi-oplog-entry transaction, ignore it until the commit.
        // We must save it here because we are not guaranteed it has been written to the oplog
        // yet.
        // We also do this for prepare during initial sync.
        if (op.isPartialTransaction() ||
            (op.shouldPrepare() && getOptions().mode == OplogApplication::Mode::kInitialSync)) {
            auto& partialTxnList = partialTxnOps[*op.getSessionId()];
            // If this operation belongs to an existing partial transaction, partialTxnList
            // must contain the previous operations of the transaction.
            invariant(partialTxnList.empty() ||
                      partialTxnList.front()->getTxnNumber() == op.getTxnNumber());
            partialTxnList.push_back(&op);
            continue;
        }

        if (op.getCommandType() == OplogEntry::CommandType::kAbortTransaction) {
            auto& partialTxnList = partialTxnOps[*op.getSessionId()];
            partialTxnList.clear();
        }

        if (op.isCrudOpType())
            processCrudOp(opCtx, &op, &hash, &hashedNs, &collPropertiesCache);
        // Extract applyOps operations and fill writers with extracted operations using this
        // function.
        if (op.isTerminalApplyOps()) {
            auto logicalSessionId = op.getSessionId();
            // applyOps entries generated by a transaction must have a sessionId and a
            // transaction number.
            if (logicalSessionId && op.getTxnNumber()) {
                // On commit of unprepared transactions, get transactional operations from the
                // oplog and fill writers with those operations.
                // Flush partialTxnList operations for current transaction.
                auto& partialTxnList = partialTxnOps[*logicalSessionId];

                derivedOps->emplace_back(
                    readTransactionOperationsFromOplogChain(opCtx, op, partialTxnList));
                partialTxnList.clear();

                // Transaction entries cannot have different session updates.
                addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            } else {
                // The applyOps entry was not generated as part of a transaction.
                invariant(!op.getPrevWriteOpTimeInTransaction());

                derivedOps->emplace_back(ApplyOps::extractOperations(op));

                // Nested entries cannot have different session updates.
                addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            }
            continue;
        }

        // If we see a commitTransaction command that is a part of a prepared transaction during
        // initial sync, find the prepare oplog entry, extract applyOps operations, and fill writers
        // with the extracted operations.
        if (op.isPreparedCommit() && (getOptions().mode == OplogApplication::Mode::kInitialSync)) {
            auto logicalSessionId = op.getSessionId();
            auto& partialTxnList = partialTxnOps[*logicalSessionId];

            derivedOps->emplace_back(
                readTransactionOperationsFromOplogChain(opCtx, op, partialTxnList));
            partialTxnList.clear();

            addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            continue;
        }

        addToWriterVector(&op, writerVectors, hash);
    }
}

void OplogApplierImpl::fillWriterVectors(
    OperationContext* opCtx,
    MultiApplier::Operations* ops,
    std::vector<MultiApplier::OperationPtrs>* writerVectors,
    std::vector<MultiApplier::Operations>* derivedOps) noexcept {

    SessionUpdateTracker sessionUpdateTracker;
    _deriveOpsAndFillWriterVectors(opCtx, ops, writerVectors, derivedOps, &sessionUpdateTracker);

    auto newOplogWrites = sessionUpdateTracker.flushAll();
    if (!newOplogWrites.empty()) {
        derivedOps->emplace_back(std::move(newOplogWrites));
        _deriveOpsAndFillWriterVectors(
            opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
    }
}

Status OplogApplierImpl::applyOplogGroup(OperationContext* opCtx,
                                         MultiApplier::OperationPtrs* ops,
                                         WorkerMultikeyPathInfo* workerMultikeyPathInfo) {

    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);
    // Since we swap the locker in stash / unstash transaction resources,
    // ShouldNotConflictWithSecondaryBatchApplicationBlock will touch the locker that has been
    // destroyed by unstash in its destructor. Thus we set the flag explicitly.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // Explicitly start future read transactions without a timestamp.
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);

    // When querying indexes, we return the record matching the key if it exists, or an adjacent
    // document. This means that it is possible for us to hit a prepare conflict if we query for an
    // incomplete key and an adjacent key is prepared.
    // We ignore prepare conflicts on secondaries because they may encounter prepare conflicts that
    // did not occur on the primary.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    stableSortByNamespace(ops);

    const auto oplogApplicationMode = getOptions().mode;

    InsertGroup insertGroup(ops, opCtx, oplogApplicationMode);

    {  // Ensure that the MultikeyPathTracker stops tracking paths.
        ON_BLOCK_EXIT([opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
        MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();

        for (auto it = ops->cbegin(); it != ops->cend(); ++it) {
            const OplogEntry& entry = **it;

            // If we are successful in grouping and applying inserts, advance the current iterator
            // past the end of the inserted group of entries.
            auto groupResult = insertGroup.groupAndApplyInserts(it);
            if (groupResult.isOK()) {
                it = groupResult.getValue();
                continue;
            }

            // If we didn't create a group, try to apply the op individually.
            try {
                const Status status = applyOplogEntryBatch(opCtx, &entry, oplogApplicationMode);

                if (!status.isOK()) {
                    // Tried to apply an update operation but the document is missing, there must be
                    // a delete operation for the document later in the oplog.
                    if (status == ErrorCodes::UpdateOperationFailed &&
                        oplogApplicationMode == OplogApplication::Mode::kInitialSync) {
                        continue;
                    }

                    severe() << "Error applying operation (" << redact(entry.toBSON())
                             << "): " << causedBy(redact(status));
                    return status;
                }
            } catch (const DBException& e) {
                // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
                // dropped before initial sync or recovery ends anyways and we should ignore it.
                if (e.code() == ErrorCodes::NamespaceNotFound && entry.isCrudOpType() &&
                    getOptions().allowNamespaceNotFoundErrorsOnCrudOps) {
                    continue;
                }

                severe() << "writer worker caught exception: " << redact(e)
                         << " on: " << redact(entry.toBSON());
                return e.toStatus();
            }
        }
    }

    invariant(!MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo());
    invariant(workerMultikeyPathInfo->empty());
    auto newPaths = MultikeyPathTracker::get(opCtx).getMultikeyPathInfo();
    if (!newPaths.empty()) {
        workerMultikeyPathInfo->swap(newPaths);
    }

    return Status::OK();
}

Status applyOplogEntryBatch(OperationContext* opCtx,
                            const OplogEntryBatch& batch,
                            OplogApplication::Mode oplogApplicationMode) {
    // Guarantees that applyOplogEntryBatch's context matches that of its calling function,
    // applyOplogGroup.
    invariant(!opCtx->writesAreReplicated());
    invariant(documentValidationDisabled(opCtx));

    auto op = batch.getOp();
    // Count each log op application as a separate operation, for reporting purposes
    CurOp individualOp(opCtx);

    const NamespaceString nss(op.getNss());

    auto incrementOpsAppliedStats = [] { opsAppliedStats.increment(1); };

    auto applyOp = [&](Database* db) {
        // We convert updates to upserts when not in initial sync because after rollback and during
        // startup we may replay an update after a delete and crash since we do not ignore
        // errors. In initial sync we simply ignore these update errors so there is no reason to
        // upsert.
        //
        // TODO (SERVER-21700): Never upsert during oplog application unless an external applyOps
        // wants to. We should ignore these errors intelligently while in RECOVERING and STARTUP
        // mode (similar to initial sync) instead so we do not accidentally ignore real errors.
        bool shouldAlwaysUpsert = (oplogApplicationMode != OplogApplication::Mode::kInitialSync);
        Status status = applyOperation_inlock(
            opCtx, db, batch, shouldAlwaysUpsert, oplogApplicationMode, incrementOpsAppliedStats);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        return status;
    };

    auto clockSource = opCtx->getServiceContext()->getFastClockSource();
    auto applyStartTime = clockSource->now();

    if (MONGO_unlikely(hangAfterRecordingOpApplicationStartTime.shouldFail())) {
        log() << "applyOplogEntryBatch - fail point hangAfterRecordingOpApplicationStartTime "
                 "enabled. "
              << "Blocking until fail point is disabled. ";
        hangAfterRecordingOpApplicationStartTime.pauseWhileSet();
    }

    auto opType = op.getOpType();

    auto finishApply = [&](Status status) {
        return finishAndLogApply(clockSource, status, applyStartTime, batch);
    };

    if (opType == OpTypeEnum::kNoop) {
        incrementOpsAppliedStats();
        return Status::OK();
    } else if (OplogEntry::isCrudOpType(opType)) {
        return finishApply(writeConflictRetry(opCtx, "applyOplogEntryBatch_CRUD", nss.ns(), [&] {
            // Need to throw instead of returning a status for it to be properly ignored.
            try {
                AutoGetCollection autoColl(
                    opCtx, getNsOrUUID(nss, op), fixLockModeForSystemDotViewsChanges(nss, MODE_IX));
                auto db = autoColl.getDb();
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream() << "missing database (" << nss.db() << ")",
                        db);
                OldClientContext ctx(opCtx, autoColl.getNss().ns(), db);
                return applyOp(ctx.db());
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                // Delete operations on non-existent namespaces can be treated as successful for
                // idempotency reasons.
                // During RECOVERING mode, we ignore NamespaceNotFound for all CRUD ops since
                // storage does not wait for drops to be checkpointed (SERVER-33161).
                if (opType == OpTypeEnum::kDelete ||
                    oplogApplicationMode == OplogApplication::Mode::kRecovering) {
                    return Status::OK();
                }

                ex.addContext(str::stream()
                              << "Failed to apply operation: " << redact(batch.toBSON()));
                throw;
            }
        }));
    } else if (opType == OpTypeEnum::kCommand) {
        return finishApply(writeConflictRetry(opCtx, "applyOplogEntryBatch_command", nss.ns(), [&] {
            // A special case apply for commands to avoid implicit database creation.
            Status status = applyCommand_inlock(opCtx, op, oplogApplicationMode);
            incrementOpsAppliedStats();
            return status;
        }));
    }

    MONGO_UNREACHABLE;
}


}  // namespace repl
}  // namespace mongo
