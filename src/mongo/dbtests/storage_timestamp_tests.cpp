/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <cstdint>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

class StorageTimestampTest {
public:
    ServiceContext::UniqueOperationContext _opCtxRaii = cc().makeOperationContext();
    OperationContext* _opCtx = _opCtxRaii.get();
    LogicalClock* _clock = LogicalClock::get(_opCtx);

    // Set up Timestamps in the past, present, and future.
    const LogicalTime pastLt = _clock->reserveTicks(1);
    const Timestamp pastTs = pastLt.asTimestamp();
    const LogicalTime presentLt = _clock->reserveTicks(1);
    const Timestamp presentTs = presentLt.asTimestamp();
    const LogicalTime futureLt = presentLt.addTicks(1);
    const Timestamp futureTs = futureLt.asTimestamp();
    const Timestamp nullTs = Timestamp();
    const int presentTerm = 1;

    StorageTimestampTest() {
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
        auto coordinatorMock =
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings);
        coordinatorMock->alwaysAllowWrites(true);
        setGlobalReplicationCoordinator(coordinatorMock);
        repl::StorageInterface::set(_opCtx->getServiceContext(),
                                    stdx::make_unique<repl::StorageInterfaceImpl>());

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx->getClient()).clearLastOp_forTest();

        auto registry = stdx::make_unique<OpObserverRegistry>();
        registry->addObserver(stdx::make_unique<UUIDCatalogObserver>());
        registry->addObserver(stdx::make_unique<OpObserverImpl>());
        _opCtx->getServiceContext()->setOpObserver(std::move(registry));

        repl::setOplogCollectionName(getGlobalServiceContext());
        repl::createOplog(_opCtx);

        ASSERT_OK(_clock->advanceClusterTime(LogicalTime(Timestamp(1, 0))));

        ASSERT_EQUALS(presentTs, pastLt.addTicks(1).asTimestamp());
        setReplCoordAppliedOpTime(repl::OpTime(presentTs, presentTerm));
    }

    ~StorageTimestampTest() {
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        try {
            reset(NamespaceString("local.oplog.rs"));
        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }

    /**
     * Walking on ice: resetting the ReplicationCoordinator destroys the underlying
     * `DropPendingCollectionReaper`. Use a truncate/dropAllIndexes to clean out a collection
     * without actually dropping it.
     */
    void reset(NamespaceString nss) const {
        ::mongo::writeConflictRetry(_opCtx, "deleteAll", nss.ns(), [&] {
            invariant(_opCtx->recoveryUnit()->selectSnapshot(Timestamp::min()).isOK());
            AutoGetCollection collRaii(_opCtx, nss, LockMode::MODE_X);

            if (collRaii.getCollection()) {
                WriteUnitOfWork wunit(_opCtx);
                invariant(collRaii.getCollection()->truncate(_opCtx).isOK());
                collRaii.getCollection()->getIndexCatalog()->dropAllIndexes(_opCtx, false);
                wunit.commit();
                return;
            }

            AutoGetOrCreateDb dbRaii(_opCtx, nss.db(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            invariant(dbRaii.getDb()->createCollection(_opCtx, nss.ns()));
            wunit.commit();
        });
    }

    void insertDocument(Collection* coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = false;
        const bool fromMigrate = false;
        ASSERT_OK(coll->insertDocument(_opCtx, stmt, nullOpDebug, enforceQuota, fromMigrate));
    }

    std::int32_t itCount(Collection* coll) {
        std::uint64_t ret = 0;
        auto cursor = coll->getRecordStore()->getCursor(_opCtx);
        while (cursor->next() != boost::none) {
            ++ret;
        }

        return ret;
    }

    BSONObj findOne(Collection* coll) {
        auto optRecord = coll->getRecordStore()->getCursor(_opCtx)->next();
        if (optRecord == boost::none) {
            // Print a stack trace to help disambiguate which `findOne` failed.
            printStackTrace();
            FAIL("Did not find any documents.");
        }
        return optRecord.get().data.toBson();
    }

    StatusWith<BSONObj> doAtomicApplyOps(const std::string& dbName,
                                         const std::list<BSONObj>& applyOpsList) {
        BSONObjBuilder result;
        Status status = applyOps(_opCtx,
                                 dbName,
                                 BSON("applyOps" << applyOpsList),
                                 repl::OplogApplication::Mode::kApplyOpsCmd,
                                 &result);
        if (!status.isOK()) {
            return status;
        }

        return {result.obj()};
    }

    // Creates a dummy command operation to persuade `applyOps` to be non-atomic.
    StatusWith<BSONObj> doNonAtomicApplyOps(const std::string& dbName,
                                            const std::list<BSONObj>& applyOpsList,
                                            Timestamp dummyTs) {
        BSONArrayBuilder builder;
        builder.append(applyOpsList);
        builder << BSON("ts" << dummyTs << "t" << 1LL << "h" << 1 << "op"
                             << "c"
                             << "ns"
                             << "test.$cmd"
                             << "o"
                             << BSON("applyOps" << BSONArrayBuilder().obj()));
        BSONObjBuilder result;
        Status status = applyOps(_opCtx,
                                 dbName,
                                 BSON("applyOps" << builder.arr()),
                                 repl::OplogApplication::Mode::kApplyOpsCmd,
                                 &result);
        if (!status.isOK()) {
            return status;
        }

        return {result.obj()};
    }

    void assertMinValidDocumentAtTimestamp(Collection* coll,
                                           const Timestamp& ts,
                                           const repl::MinValidDocument& expectedDoc) {
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(ts));
        auto doc =
            repl::MinValidDocument::parse(IDLParserErrorContext("MinValidDocument"), findOne(coll));
        ASSERT_EQ(expectedDoc.getMinValidTimestamp(), doc.getMinValidTimestamp())
            << "minValid timestamps weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getMinValidTerm(), doc.getMinValidTerm())
            << "minValid terms weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getAppliedThrough(), doc.getAppliedThrough())
            << "appliedThrough OpTimes weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getOldOplogDeleteFromPoint(), doc.getOldOplogDeleteFromPoint())
            << "Old oplogDeleteFromPoint timestamps weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getInitialSyncFlag(), doc.getInitialSyncFlag())
            << "Initial sync flags weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
    }

    void assertDocumentAtTimestamp(Collection* coll,
                                   const Timestamp& ts,
                                   const BSONObj& expectedDoc) {

        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(ts));
        if (expectedDoc.isEmpty()) {
            ASSERT_EQ(0, itCount(coll)) << "Should not find any documents in " << coll->ns()
                                        << " at ts: " << ts;
        } else {
            ASSERT_EQ(1, itCount(coll)) << "Should find one document in " << coll->ns()
                                        << " at ts: " << ts;
            auto doc = findOne(coll);
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, expectedDoc))
                << "Doc: " << doc.toString() << " Expected: " << expectedDoc.toString();
        }
    }

    void setReplCoordAppliedOpTime(const repl::OpTime& opTime) {
        repl::getGlobalReplicationCoordinator()->setMyLastAppliedOpTime(opTime);
    }

    /**
     * Asserts that the given collection is in (or not in) the KVCatalog's list of idents at the
     * provided timestamp.
     */
    void assertNamespaceInIdents(OperationContext* opCtx,
                                 NamespaceString nss,
                                 Timestamp ts,
                                 bool shouldExpect) {
        KVCatalog* kvCatalog =
            static_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getGlobalStorageEngine())
                ->getCatalog();

        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(ts));

        // getCollectionIdent() returns the ident for the given namespace in the KVCatalog.
        // getAllIdents() actually looks in the RecordStore for a list of all idents, and is thus
        // versioned by timestamp. These tests do not do any renames, so we can expect the
        // namespace to have a consistent ident across timestamps, if it exists.
        auto expectedIdent = kvCatalog->getCollectionIdent(nss.ns());
        auto idents = kvCatalog->getAllIdents(opCtx);
        auto found = std::find(idents.begin(), idents.end(), expectedIdent);

        if (shouldExpect) {
            ASSERT(found != idents.end()) << nss.ns() << " was not found at " << ts.toString();
        } else {
            ASSERT(found == idents.end()) << nss.ns() << " was found at " << ts.toString()
                                          << " when it should not have been.";
        }
    }

    std::tuple<std::string, std::string> getNewCollectionIndexIdent(
        KVCatalog* kvCatalog, std::vector<std::string>& origIdents) {
        // Find the collection and index ident by performing a set difference on the original
        // idents and the current idents.
        std::vector<std::string> identsWithColl = kvCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> collAndIdxIdents;
        std::set_difference(identsWithColl.begin(),
                            identsWithColl.end(),
                            origIdents.begin(),
                            origIdents.end(),
                            std::back_inserter(collAndIdxIdents));

        ASSERT(collAndIdxIdents.size() == 1 || collAndIdxIdents.size() == 2);
        if (collAndIdxIdents.size() == 1) {
            // `system.profile` collections do not have an `_id` index.
            return std::tie(collAndIdxIdents[0], "");
        }
        if (collAndIdxIdents.size() == 2) {
            // The idents are sorted, so the `collection-...` comes before `index-...`
            return std::tie(collAndIdxIdents[0], collAndIdxIdents[1]);
        }

        MONGO_UNREACHABLE;
    }

    void assertIdentsExistAtTimestamp(KVCatalog* kvCatalog,
                                      const std::string& collIdent,
                                      const std::string& indexIdent,
                                      Timestamp timestamp) {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(_opCtx->recoveryUnit()->selectSnapshot(timestamp));
        auto allIdents = kvCatalog->getAllIdents(_opCtx);
        ASSERT(std::find(allIdents.begin(), allIdents.end(), collIdent) != allIdents.end());
        if (indexIdent.size() > 0) {
            // `system.profile` does not have an `_id` index.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), indexIdent) != allIdents.end());
        }
    }

    void assertIdentsMissingAtTimestamp(KVCatalog* kvCatalog,
                                        const std::string& collIdent,
                                        const std::string& indexIdent,
                                        Timestamp timestamp) {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(_opCtx->recoveryUnit()->selectSnapshot(timestamp));
        auto allIdents = kvCatalog->getAllIdents(_opCtx);
        ASSERT(std::find(allIdents.begin(), allIdents.end(), collIdent) == allIdents.end());
        ASSERT(std::find(allIdents.begin(), allIdents.end(), indexIdent) == allIdents.end());
    }
};

class SecondaryInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            BSONObjBuilder result;
            ASSERT_OK(applyOps(
                _opCtx,
                nss.db().toString(),
                BSON("applyOps" << BSON_ARRAY(
                         BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t" << 1LL
                                   << "h"
                                   << 0xBEEFBEEFLL
                                   << "v"
                                   << 2
                                   << "op"
                                   << "i"
                                   << "ns"
                                   << nss.ns()
                                   << "ui"
                                   << autoColl.getCollection()->uuid().get()
                                   << "o"
                                   << BSON("_id" << idx))
                         << BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t" << 1LL
                                      << "h"
                                      << 1
                                      << "op"
                                      << "c"
                                      << "ns"
                                      << "test.$cmd"
                                      << "o"
                                      << BSON("applyOps" << BSONArrayBuilder().obj())))),
                repl::OplogApplication::Mode::kApplyOpsCmd,
                &result));
        }

        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(firstInsertTime.addTicks(idx).asTimestamp()));
            BSONObj result;
            ASSERT(Helpers::getLast(_opCtx, nss.ns().c_str(), result)) << " idx is " << idx;
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
                << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
        }
    }
};

class SecondaryArrayInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        BSONObjBuilder fullCommand;
        BSONArrayBuilder applyOpsB(fullCommand.subarrayStart("applyOps"));

        BSONObjBuilder applyOpsElem1Builder;

        // Populate the "ts" field with an array of all the grouped inserts' timestamps.
        BSONArrayBuilder tsArrayBuilder(applyOpsElem1Builder.subarrayStart("ts"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tsArrayBuilder.append(firstInsertTime.addTicks(idx).asTimestamp());
        }
        tsArrayBuilder.done();

        // Populate the "t" (term) field with an array of all the grouped inserts' terms.
        BSONArrayBuilder tArrayBuilder(applyOpsElem1Builder.subarrayStart("t"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tArrayBuilder.append(1LL);
        }
        tArrayBuilder.done();

        // Populate the "o" field with an array of all the grouped inserts.
        BSONArrayBuilder oArrayBuilder(applyOpsElem1Builder.subarrayStart("o"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            oArrayBuilder.append(BSON("_id" << idx));
        }
        oArrayBuilder.done();

        applyOpsElem1Builder << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                             << "i"
                             << "ns" << nss.ns() << "ui" << autoColl.getCollection()->uuid().get();

        applyOpsB.append(applyOpsElem1Builder.done());

        BSONObjBuilder applyOpsElem2Builder;
        applyOpsElem2Builder << "ts" << firstInsertTime.addTicks(docsToInsert).asTimestamp() << "t"
                             << 1LL << "h" << 1 << "op"
                             << "c"
                             << "ns"
                             << "test.$cmd"
                             << "o" << BSON("applyOps" << BSONArrayBuilder().obj());

        applyOpsB.append(applyOpsElem2Builder.done());
        applyOpsB.done();
        // Apply the group of inserts.
        BSONObjBuilder result;
        ASSERT_OK(applyOps(_opCtx,
                           nss.db().toString(),
                           fullCommand.done(),
                           repl::OplogApplication::Mode::kApplyOpsCmd,
                           &result));


        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(firstInsertTime.addTicks(idx).asTimestamp()));
            BSONObj result;
            ASSERT(Helpers::getLast(_opCtx, nss.ns().c_str(), result)) << " idx is " << idx;
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
                << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
        }
    }
};

class SecondaryDeleteTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedDeletes");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Insert some documents.
        const std::int32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        const LogicalTime lastInsertTime = firstInsertTime.addTicks(docsToInsert - 1);
        WriteUnitOfWork wunit(_opCtx);
        for (std::int32_t num = 0; num < docsToInsert; ++num) {
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << num << "a" << num),
                                           firstInsertTime.addTicks(num).asTimestamp(),
                                           0LL));
        }
        wunit.commit();
        ASSERT_EQ(docsToInsert, itCount(autoColl.getCollection()));

        // Delete all documents one at a time.
        const LogicalTime startDeleteTime = _clock->reserveTicks(docsToInsert);
        for (std::int32_t num = 0; num < docsToInsert; ++num) {
            ASSERT_OK(
                doNonAtomicApplyOps(
                    nss.db().toString(),
                    {BSON("ts" << startDeleteTime.addTicks(num).asTimestamp() << "t" << 0LL << "h"
                               << 0xBEEFBEEFLL
                               << "v"
                               << 2
                               << "op"
                               << "d"
                               << "ns"
                               << nss.ns()
                               << "ui"
                               << autoColl.getCollection()->uuid().get()
                               << "o"
                               << BSON("_id" << num))},
                    startDeleteTime.addTicks(num).asTimestamp())
                    .getStatus());
        }

        for (std::int32_t num = 0; num <= docsToInsert; ++num) {
            // The first loop queries at `lastInsertTime` and should count all documents. Querying
            // at each successive tick counts one less document.
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(lastInsertTime.addTicks(num).asTimestamp()));
            ASSERT_EQ(docsToInsert - num, itCount(autoColl.getCollection()));
        }
    }
};

class SecondaryUpdateTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Insert one document that will go through a series of updates.
        const LogicalTime insertTime = _clock->reserveTicks(1);
        WriteUnitOfWork wunit(_opCtx);
        insertDocument(autoColl.getCollection(),
                       InsertStatement(BSON("_id" << 0), insertTime.asTimestamp(), 0LL));
        wunit.commit();
        ASSERT_EQ(1, itCount(autoColl.getCollection()));

        // Each pair in the vector represents the update to perform at the next tick of the
        // clock. `pair.first` is the update to perform and `pair.second` is the full value of the
        // document after the transformation.
        const std::vector<std::pair<BSONObj, BSONObj>> updates = {
            {BSON("$set" << BSON("val" << 1)), BSON("_id" << 0 << "val" << 1)},
            {BSON("$unset" << BSON("val" << 1)), BSON("_id" << 0)},
            {BSON("$addToSet" << BSON("theSet" << 1)),
             BSON("_id" << 0 << "theSet" << BSON_ARRAY(1))},
            {BSON("$addToSet" << BSON("theSet" << 2)),
             BSON("_id" << 0 << "theSet" << BSON_ARRAY(1 << 2))},
            {BSON("$pull" << BSON("theSet" << 1)), BSON("_id" << 0 << "theSet" << BSON_ARRAY(2))},
            {BSON("$pull" << BSON("theSet" << 2)), BSON("_id" << 0 << "theSet" << BSONArray())},
            {BSON("$set" << BSON("theMap.val" << 1)),
             BSON("_id" << 0 << "theSet" << BSONArray() << "theMap" << BSON("val" << 1))},
            {BSON("$rename" << BSON("theSet"
                                    << "theOtherSet")),
             BSON("_id" << 0 << "theMap" << BSON("val" << 1) << "theOtherSet" << BSONArray())}};

        const LogicalTime firstUpdateTime = _clock->reserveTicks(updates.size());
        for (std::size_t idx = 0; idx < updates.size(); ++idx) {
            ASSERT_OK(
                doNonAtomicApplyOps(
                    nss.db().toString(),
                    {BSON("ts" << firstUpdateTime.addTicks(idx).asTimestamp() << "t" << 0LL << "h"
                               << 0xBEEFBEEFLL
                               << "v"
                               << 2
                               << "op"
                               << "u"
                               << "ns"
                               << nss.ns()
                               << "ui"
                               << autoColl.getCollection()->uuid().get()
                               << "o2"
                               << BSON("_id" << 0)
                               << "o"
                               << updates[idx].first)},
                    firstUpdateTime.addTicks(idx).asTimestamp())
                    .getStatus());
        }

        for (std::size_t idx = 0; idx < updates.size(); ++idx) {
            // Querying at each successive ticks after `insertTime` sees the document transform in
            // the series.
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(insertTime.addTicks(idx + 1).asTimestamp()));

            auto doc = findOne(autoColl.getCollection());
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, updates[idx].second))
                << "Doc: " << doc.toString() << " Expected: " << updates[idx].second.toString();
        }
    }
};

class SecondaryInsertToUpsert : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const LogicalTime insertTime = _clock->reserveTicks(2);

        // This applyOps runs into an insert of `{_id: 0, field: 0}` followed by a second insert
        // on the same collection with `{_id: 0}`. It's expected for this second insert to be
        // turned into an upsert. The goal document does not contain `field: 0`.
        BSONObjBuilder resultBuilder;
        auto swResult = doNonAtomicApplyOps(
            nss.db().toString(),
            {BSON("ts" << insertTime.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                       << "op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "ui"
                       << autoColl.getCollection()->uuid().get()
                       << "o"
                       << BSON("_id" << 0 << "field" << 0)),
             BSON("ts" << insertTime.addTicks(1).asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL
                       << "v"
                       << 2
                       << "op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "ui"
                       << autoColl.getCollection()->uuid().get()
                       << "o"
                       << BSON("_id" << 0))},
            insertTime.addTicks(1).asTimestamp());
        ASSERT_OK(swResult);

        BSONObj& result = swResult.getValue();
        ASSERT_EQ(3, result.getIntField("applied"));
        ASSERT(result["results"].Array()[0].Bool());
        ASSERT(result["results"].Array()[1].Bool());
        ASSERT(result["results"].Array()[2].Bool());

        // Reading at `insertTime` should show the original document, `{_id: 0, field: 0}`.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(insertTime.asTimestamp()));
        auto doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0,
                  SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0 << "field" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0, field: 0}";

        // Reading at `insertTime + 1` should show the second insert that got converted to an
        // upsert, `{_id: 0}`.
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(insertTime.addTicks(1).asTimestamp()));
        doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0}";
    }
};

class SecondaryAtomicApplyOps : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // Create a new collection.
        NamespaceString nss("unittests.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Reserve a timestamp before the inserts should happen.
        const LogicalTime preInsertTimestamp = _clock->reserveTicks(1);
        auto swResult = doAtomicApplyOps(nss.db().toString(),
                                         {BSON("v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << nss.ns()
                                                   << "ui"
                                                   << autoColl.getCollection()->uuid().get()
                                                   << "o"
                                                   << BSON("_id" << 0)),
                                          BSON("v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << nss.ns()
                                                   << "ui"
                                                   << autoColl.getCollection()->uuid().get()
                                                   << "o"
                                                   << BSON("_id" << 1))});
        ASSERT_OK(swResult);

        ASSERT_EQ(2, swResult.getValue().getIntField("applied"));
        ASSERT(swResult.getValue()["results"].Array()[0].Bool());
        ASSERT(swResult.getValue()["results"].Array()[1].Bool());

        // Reading at `preInsertTimestamp` should not find anything.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.asTimestamp()));
        ASSERT_EQ(0, itCount(autoColl.getCollection()))
            << "Should not observe a write at `preInsertTimestamp`. TS: "
            << preInsertTimestamp.asTimestamp();

        // Reading at `preInsertTimestamp + 1` should observe both inserts.
        recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.addTicks(1).asTimestamp()));
        ASSERT_EQ(2, itCount(autoColl.getCollection()))
            << "Should observe both writes at `preInsertTimestamp + 1`. TS: "
            << preInsertTimestamp.addTicks(1).asTimestamp();
    }
};


// This should have the same result as `SecondaryInsertToUpsert` except it gets there a different
// way. Doing an atomic `applyOps` should result in a WriteConflictException because the same
// transaction is trying to write modify the same document twice. The `applyOps` command should
// catch that failure and retry in non-atomic mode, preserving the timestamps supplied by the
// user.
class SecondaryAtomicApplyOpsWCEToNonAtomic : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // Create a new collectiont.
        NamespaceString nss("unitteTsts.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const LogicalTime preInsertTimestamp = _clock->reserveTicks(1);
        auto swResult = doAtomicApplyOps(nss.db().toString(),
                                         {BSON("v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << nss.ns()
                                                   << "ui"
                                                   << autoColl.getCollection()->uuid().get()
                                                   << "o"
                                                   << BSON("_id" << 0 << "field" << 0)),
                                          BSON("v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << nss.ns()
                                                   << "ui"
                                                   << autoColl.getCollection()->uuid().get()
                                                   << "o"
                                                   << BSON("_id" << 0))});
        ASSERT_OK(swResult);

        ASSERT_EQ(2, swResult.getValue().getIntField("applied"));
        ASSERT(swResult.getValue()["results"].Array()[0].Bool());
        ASSERT(swResult.getValue()["results"].Array()[1].Bool());

        // Reading at `insertTime` should not see any documents.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.asTimestamp()));
        ASSERT_EQ(0, itCount(autoColl.getCollection()))
            << "Should not find any documents at `preInsertTimestamp`. TS: "
            << preInsertTimestamp.asTimestamp();

        // Reading at `preInsertTimestamp + 1` should show the final state of the document.
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.addTicks(1).asTimestamp()));
        auto doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0}";
    }
};

class SecondaryCreateCollection : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        NamespaceString nss("unittests.secondaryCreateCollection");
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss));

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObjBuilder resultBuilder;
        auto swResult = doNonAtomicApplyOps(
            nss.db().toString(),
            {
                BSON("ts" << presentTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                          << "c"
                          << "ui"
                          << UUID::gen()
                          << "ns"
                          << nss.getCommandNS().ns()
                          << "o"
                          << BSON("create" << nss.coll())),
            },
            presentTs);
        ASSERT_OK(swResult);

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        assertNamespaceInIdents(_opCtx, nss, pastTs, false);
        assertNamespaceInIdents(_opCtx, nss, presentTs, true);
        assertNamespaceInIdents(_opCtx, nss, futureTs, true);
        assertNamespaceInIdents(_opCtx, nss, nullTs, true);
    }
};

class SecondaryCreateTwoCollections : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        std::string dbName = "unittest";
        NamespaceString nss1(dbName, "secondaryCreateTwoCollections1");
        NamespaceString nss2(dbName, "secondaryCreateTwoCollections2");
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss1));
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss2));

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss1).getCollection()); }
        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss2).getCollection()); }

        const LogicalTime dummyLt = futureLt.addTicks(1);
        const Timestamp dummyTs = dummyLt.asTimestamp();

        BSONObjBuilder resultBuilder;
        auto swResult = doNonAtomicApplyOps(
            dbName,
            {
                BSON("ts" << presentTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                          << "c"
                          << "ui"
                          << UUID::gen()
                          << "ns"
                          << nss1.getCommandNS().ns()
                          << "o"
                          << BSON("create" << nss1.coll())),
                BSON("ts" << futureTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                          << "c"
                          << "ui"
                          << UUID::gen()
                          << "ns"
                          << nss2.getCommandNS().ns()
                          << "o"
                          << BSON("create" << nss2.coll())),
            },
            dummyTs);
        ASSERT_OK(swResult);

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss1).getCollection()); }
        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss2).getCollection()); }

        assertNamespaceInIdents(_opCtx, nss1, pastTs, false);
        assertNamespaceInIdents(_opCtx, nss1, presentTs, true);
        assertNamespaceInIdents(_opCtx, nss1, futureTs, true);
        assertNamespaceInIdents(_opCtx, nss1, dummyTs, true);
        assertNamespaceInIdents(_opCtx, nss1, nullTs, true);

        assertNamespaceInIdents(_opCtx, nss2, pastTs, false);
        assertNamespaceInIdents(_opCtx, nss2, presentTs, false);
        assertNamespaceInIdents(_opCtx, nss2, futureTs, true);
        assertNamespaceInIdents(_opCtx, nss2, dummyTs, true);
        assertNamespaceInIdents(_opCtx, nss2, nullTs, true);
    }
};

class SecondaryCreateCollectionBetweenInserts : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        std::string dbName = "unittest";
        NamespaceString nss1(dbName, "secondaryCreateCollectionBetweenInserts1");
        NamespaceString nss2(dbName, "secondaryCreateCollectionBetweenInserts2");
        BSONObj doc1 = BSON("_id" << 1 << "field" << 1);
        BSONObj doc2 = BSON("_id" << 2 << "field" << 2);

        const UUID uuid2 = UUID::gen();

        const LogicalTime insert2Lt = futureLt.addTicks(1);
        const Timestamp insert2Ts = insert2Lt.asTimestamp();

        const LogicalTime dummyLt = insert2Lt.addTicks(1);
        const Timestamp dummyTs = dummyLt.asTimestamp();

        {
            reset(nss1);
            AutoGetCollection autoColl(_opCtx, nss1, LockMode::MODE_X, LockMode::MODE_IX);

            ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss2));
            { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss2).getCollection()); }

            BSONObjBuilder resultBuilder;
            auto swResult = doNonAtomicApplyOps(
                dbName,
                {
                    BSON("ts" << presentTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                              << "i"
                              << "ns"
                              << nss1.ns()
                              << "ui"
                              << autoColl.getCollection()->uuid().get()
                              << "o"
                              << doc1),
                    BSON("ts" << futureTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                              << "c"
                              << "ui"
                              << uuid2
                              << "ns"
                              << nss2.getCommandNS().ns()
                              << "o"
                              << BSON("create" << nss2.coll())),
                    BSON("ts" << insert2Ts << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                              << "i"
                              << "ns"
                              << nss2.ns()
                              << "ui"
                              << uuid2
                              << "o"
                              << doc2),
                },
                dummyTs);
            ASSERT_OK(swResult);
        }

        {
            AutoGetCollectionForReadCommand autoColl1(_opCtx, nss1);
            auto coll1 = autoColl1.getCollection();
            ASSERT(coll1);
            AutoGetCollectionForReadCommand autoColl2(_opCtx, nss2);
            auto coll2 = autoColl2.getCollection();
            ASSERT(coll2);

            assertDocumentAtTimestamp(coll1, pastTs, BSONObj());
            assertDocumentAtTimestamp(coll1, presentTs, doc1);
            assertDocumentAtTimestamp(coll1, futureTs, doc1);
            assertDocumentAtTimestamp(coll1, insert2Ts, doc1);
            assertDocumentAtTimestamp(coll1, dummyTs, doc1);
            assertDocumentAtTimestamp(coll1, nullTs, doc1);

            assertNamespaceInIdents(_opCtx, nss2, pastTs, false);
            assertNamespaceInIdents(_opCtx, nss2, presentTs, false);
            assertNamespaceInIdents(_opCtx, nss2, futureTs, true);
            assertNamespaceInIdents(_opCtx, nss2, insert2Ts, true);
            assertNamespaceInIdents(_opCtx, nss2, dummyTs, true);
            assertNamespaceInIdents(_opCtx, nss2, nullTs, true);

            assertDocumentAtTimestamp(coll2, pastTs, BSONObj());
            assertDocumentAtTimestamp(coll2, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll2, futureTs, BSONObj());
            assertDocumentAtTimestamp(coll2, insert2Ts, doc2);
            assertDocumentAtTimestamp(coll2, dummyTs, doc2);
            assertDocumentAtTimestamp(coll2, nullTs, doc2);
        }
    }
};

class PrimaryCreateCollectionInApplyOps : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        NamespaceString nss("unittests.primaryCreateCollectionInApplyOps");
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss));

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObjBuilder resultBuilder;
        // This 'applyOps' command will not actually be atomic, however we use the atomic helper
        // to avoid the extra 'applyOps' oplog entry that the non-atomic form creates on primaries.
        auto swResult = doAtomicApplyOps(
            nss.db().toString(),
            {
                BSON("ts" << presentTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                          << "c"
                          << "ui"
                          << UUID::gen()
                          << "ns"
                          << nss.getCommandNS().ns()
                          << "o"
                          << BSON("create" << nss.coll())),
            });
        ASSERT_OK(swResult);

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObj result;
        ASSERT(Helpers::getLast(
            _opCtx, NamespaceString::kRsOplogNamespace.toString().c_str(), result));
        repl::OplogEntry op(result);
        ASSERT(op.getOpType() == repl::OpTypeEnum::kCommand) << op.toBSON();
        // The next logOp() call will get 'futureTs', which will be the timestamp at which we do
        // the write. Thus we expect the write to appear at 'futureTs' and not before.
        ASSERT_EQ(op.getTimestamp(), futureTs) << op.toBSON();
        ASSERT_EQ(op.getNamespace().ns(), nss.getCommandNS().ns()) << op.toBSON();
        ASSERT_BSONOBJ_EQ(op.getObject(), BSON("create" << nss.coll()));

        assertNamespaceInIdents(_opCtx, nss, pastTs, false);
        assertNamespaceInIdents(_opCtx, nss, presentTs, false);
        assertNamespaceInIdents(_opCtx, nss, futureTs, true);
        assertNamespaceInIdents(_opCtx, nss, nullTs, true);
    }
};

class InitializeMinValid : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);

        repl::MinValidDocument expectedMinValid;
        expectedMinValid.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValid.setMinValidTimestamp(nullTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValid);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValid);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValid);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValid);
    }
};

class SetMinValidInitialSyncFlag : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);
        consistencyMarkers.setInitialSyncFlag(_opCtx);

        repl::MinValidDocument expectedMinValidWithSetFlag;
        expectedMinValidWithSetFlag.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidWithSetFlag.setMinValidTimestamp(nullTs);
        expectedMinValidWithSetFlag.setInitialSyncFlag(true);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidWithSetFlag);

        consistencyMarkers.clearInitialSyncFlag(_opCtx);

        repl::MinValidDocument expectedMinValidWithUnsetFlag;
        expectedMinValidWithUnsetFlag.setMinValidTerm(presentTerm);
        expectedMinValidWithUnsetFlag.setMinValidTimestamp(presentTs);
        expectedMinValidWithUnsetFlag.setAppliedThrough(repl::OpTime(presentTs, presentTerm));

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidWithUnsetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidWithUnsetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidWithUnsetFlag);
    }
};

class SetMinValidToAtLeast : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);

        // Setting minValid sets it at the provided OpTime.
        consistencyMarkers.setMinValidToAtLeast(_opCtx, repl::OpTime(presentTs, presentTerm));

        repl::MinValidDocument expectedMinValidInit;
        expectedMinValidInit.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidInit.setMinValidTimestamp(nullTs);

        repl::MinValidDocument expectedMinValidPresent;
        expectedMinValidPresent.setMinValidTerm(presentTerm);
        expectedMinValidPresent.setMinValidTimestamp(presentTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidPresent);

        consistencyMarkers.setMinValidToAtLeast(_opCtx, repl::OpTime(futureTs, presentTerm));

        repl::MinValidDocument expectedMinValidFuture;
        expectedMinValidFuture.setMinValidTerm(presentTerm);
        expectedMinValidFuture.setMinValidTimestamp(futureTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidFuture);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidFuture);

        // Setting the timestamp to the past should be a noop.
        consistencyMarkers.setMinValidToAtLeast(_opCtx, repl::OpTime(pastTs, presentTerm));

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidFuture);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidFuture);
    }
};

class SetMinValidAppliedThrough : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);

        consistencyMarkers.setAppliedThrough(_opCtx, repl::OpTime(presentTs, presentTerm));

        repl::MinValidDocument expectedMinValidInit;
        expectedMinValidInit.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidInit.setMinValidTimestamp(nullTs);

        repl::MinValidDocument expectedMinValidPresent;
        expectedMinValidPresent.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidPresent.setMinValidTimestamp(nullTs);
        expectedMinValidPresent.setAppliedThrough(repl::OpTime(presentTs, presentTerm));

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidPresent);

        // appliedThrough opTime can be unset.
        consistencyMarkers.clearAppliedThrough(_opCtx, futureTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidInit);
    }
};

class ReaperDropIsTimestamped : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        auto storageInterface = repl::StorageInterface::get(_opCtx);
        repl::DropPendingCollectionReaper::set(
            _opCtx->getServiceContext(),
            stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));
        auto reaper = repl::DropPendingCollectionReaper::get(_opCtx);

        auto kvStorageEngine =
            dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getGlobalStorageEngine());
        KVCatalog* kvCatalog = kvStorageEngine->getCatalog();

        // Save the pre-state idents so we can capture the specific idents related to collection
        // creation.
        std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);

        NamespaceString nss("unittests.reaperDropIsTimestamped");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        const LogicalTime insertTimestamp = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0), insertTimestamp.asTimestamp(), 0LL));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        // The KVCatalog only adheres to timestamp requests on `getAllIdents`. To know the right
        // collection/index that gets removed on a drop, we must capture the randomized "ident"
        // string for the target collection and index.
        std::string collIdent;
        std::string indexIdent;
        std::tie(collIdent, indexIdent) = getNewCollectionIndexIdent(kvCatalog, origIdents);

        // The first phase of a drop in a replica set is to perform a rename. This does not change
        // the ident values.
        {
            WriteUnitOfWork wuow(_opCtx);
            Database* db = autoColl.getDb();
            ASSERT_OK(db->dropCollection(_opCtx, nss.ns()));
            wuow.commit();
        }

        // Bump the clock two. The drop will get the second tick. The first tick will identify a
        // snapshot of the data with the collection renamed.
        const LogicalTime postRenameTimestamp = _clock->reserveTicks(2);

        // Actually drop the collection, propagating to the KVCatalog. This drop will be
        // timestamped at the logical clock value.
        reaper->dropCollectionsOlderThan(
            _opCtx, repl::OpTime(_clock->getClusterTime().asTimestamp(), presentTerm));
        const LogicalTime postDropTime = _clock->reserveTicks(1);

        // Querying the catalog at insert time shows the collection and index existing.
        assertIdentsExistAtTimestamp(
            kvCatalog, collIdent, indexIdent, insertTimestamp.asTimestamp());

        // Querying the catalog at rename time continues to show the collection and index exist.
        assertIdentsExistAtTimestamp(
            kvCatalog, collIdent, indexIdent, postRenameTimestamp.asTimestamp());

        // Querying the catalog after the drop shows the collection and index being deleted.
        assertIdentsMissingAtTimestamp(
            kvCatalog, collIdent, indexIdent, postDropTime.asTimestamp());
    }
};

/**
 * The first step of `mongo::dropDatabase` is to rename all replicated collections, generating a
 * "drop collection" oplog entry. Then when those entries become majority commited, calls
 * `StorageEngine::dropDatabase`. At this point, two separate code paths can perform the final
 * removal of the collections from the storage engine: the reaper, or
 * `[KV]StorageEngine::dropDatabase` when it is called from `mongo::dropDatabase`. This race
 * exists on both primaries and secondaries. This test asserts `[KV]StorageEngine::dropDatabase`
 * correctly timestamps the final drop.
 */
template <bool IsPrimary>
class KVDropDatabase : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        auto storageInterface = repl::StorageInterface::get(_opCtx);
        repl::DropPendingCollectionReaper::set(
            _opCtx->getServiceContext(),
            stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));

        auto kvStorageEngine =
            dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getGlobalStorageEngine());
        KVCatalog* kvCatalog = kvStorageEngine->getCatalog();

        // Declare the database to be in a "synced" state, i.e: in steady-state replication.
        Timestamp syncTime = _clock->reserveTicks(1).asTimestamp();
        invariant(!syncTime.isNull());
        kvStorageEngine->setInitialDataTimestamp(syncTime);

        // This test is dropping collections individually before following up with a
        // `dropDatabase` call. This is illegal in typical replication operation as `dropDatabase`
        // may find collections that haven't been renamed to a "drop-pending"
        // namespace. Workaround this by operating on a separate DB from the other tests.
        const NamespaceString nss("unittestsDropDB.kvDropDatabase");
        const NamespaceString sysProfile("unittestsDropDB.system.profile");

        std::string collIdent;
        std::string indexIdent;
        std::string sysProfileIdent;
        // `*.system.profile` does not have an `_id` index. Just create it to abide by the API. This
        // value will be the empty string. Helper methods accommodate this.
        std::string sysProfileIndexIdent;
        for (auto& tuple : {std::tie(nss, collIdent, indexIdent),
                            std::tie(sysProfile, sysProfileIdent, sysProfileIndexIdent)}) {
            // Save the pre-state idents so we can capture the specific idents related to collection
            // creation.
            std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);
            const auto& nss = std::get<0>(tuple);

            // Non-replicated namespaces are wrapped in an unreplicated writes block. This has the
            // side-effect of not timestamping the collection creation.
            repl::UnreplicatedWritesBlock notReplicated(_opCtx);
            if (nss.isReplicated()) {
                TimestampBlock tsBlock(_opCtx, _clock->reserveTicks(1).asTimestamp());
                reset(nss);
            } else {
                reset(nss);
            }

            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

            // Bind the local values to the variables in the parent scope.
            auto& collIdent = std::get<1>(tuple);
            auto& indexIdent = std::get<2>(tuple);
            std::tie(collIdent, indexIdent) = getNewCollectionIndexIdent(kvCatalog, origIdents);
        }

        const Timestamp postCreateTime = _clock->reserveTicks(1).asTimestamp();

        // Assert that `kvDropDatabase` came into creation between `syncTime` and `postCreateTime`.
        assertIdentsMissingAtTimestamp(kvCatalog, collIdent, indexIdent, syncTime);
        assertIdentsExistAtTimestamp(kvCatalog, collIdent, indexIdent, postCreateTime);

        // `system.profile` is never timestamped. This means the creation appears to have taken
        // place at the beginning of time.
        assertIdentsExistAtTimestamp(kvCatalog, sysProfileIdent, sysProfileIndexIdent, syncTime);
        assertIdentsExistAtTimestamp(
            kvCatalog, sysProfileIdent, sysProfileIndexIdent, postCreateTime);

        AutoGetCollection coll(_opCtx, nss, LockMode::MODE_X);
        {
            // Drop/rename `kvDropDatabase`. `system.profile` does not get dropped/renamed.
            WriteUnitOfWork wuow(_opCtx);
            Database* db = coll.getDb();
            ASSERT_OK(db->dropCollection(_opCtx, nss.ns()));
            wuow.commit();
        }

        // Reserve two ticks. The first represents after the rename in which the `kvDropDatabase`
        // idents still exist. The second will be used by the `dropDatabase`, as that only looks
        // at the clock; it does not advance it.
        const Timestamp postRenameTime = _clock->reserveTicks(2).asTimestamp();
        // The namespace has changed, but the ident still exists as-is after the rename.
        assertIdentsExistAtTimestamp(kvCatalog, collIdent, indexIdent, postRenameTime);

        // Primaries and secondaries call `dropDatabase` (and thus, `StorageEngine->dropDatabase`)
        // in different contexts. Both contexts must end up with correct results.
        if (IsPrimary) {
            // Primaries call `StorageEngine->dropDatabase` outside of the WUOW that logs the
            // `dropDatabase` oplog entry. It is not called in the context of a `TimestampBlock`.

            ASSERT_OK(dropDatabase(_opCtx, nss.db().toString()));
        } else {
            // Secondaries processing a `dropDatabase` oplog entry wrap the call in an
            // UnreplicatedWritesBlock and a TimestampBlock with the oplog entry's optime.

            repl::UnreplicatedWritesBlock norep(_opCtx);
            const Timestamp preDropTime = _clock->getClusterTime().asTimestamp();
            TimestampBlock dropTime(_opCtx, preDropTime);
            ASSERT_OK(dropDatabase(_opCtx, nss.db().toString()));
        }

        const Timestamp postDropTime = _clock->reserveTicks(1).asTimestamp();

        // First, assert that `system.profile` never seems to have existed.
        for (const auto& ts : {syncTime, postCreateTime, postDropTime}) {
            assertIdentsMissingAtTimestamp(kvCatalog, sysProfileIdent, sysProfileIndexIdent, ts);
        }

        // Now assert that `kvDropDatabase` still existed at `postCreateTime`, but was deleted at
        // `postDropTime`.
        assertIdentsExistAtTimestamp(kvCatalog, collIdent, indexIdent, postCreateTime);
        assertIdentsExistAtTimestamp(kvCatalog, collIdent, indexIdent, postRenameTime);
        assertIdentsMissingAtTimestamp(kvCatalog, collIdent, indexIdent, postDropTime);
    }
};


class AllStorageTimestampTests : public unittest::Suite {
public:
    AllStorageTimestampTests() : unittest::Suite("StorageTimestampTests") {}
    void setupTests() {
        add<SecondaryInsertTimes>();
        add<SecondaryArrayInsertTimes>();
        add<SecondaryDeleteTimes>();
        add<SecondaryUpdateTimes>();
        add<SecondaryInsertToUpsert>();
        add<SecondaryAtomicApplyOps>();
        add<SecondaryAtomicApplyOpsWCEToNonAtomic>();
        add<SecondaryCreateCollection>();
        add<SecondaryCreateTwoCollections>();
        add<SecondaryCreateCollectionBetweenInserts>();
        add<PrimaryCreateCollectionInApplyOps>();
        add<InitializeMinValid>();
        add<SetMinValidInitialSyncFlag>();
        add<SetMinValidToAtLeast>();
        add<SetMinValidAppliedThrough>();
        add<ReaperDropIsTimestamped>();
        // KVDropDatabase<IsPrimary>
        add<KVDropDatabase<false>>();
        add<KVDropDatabase<true>>();
    }
};

unittest::SuiteInstance<AllStorageTimestampTests> allStorageTimestampTests;
}  // namespace mongo
