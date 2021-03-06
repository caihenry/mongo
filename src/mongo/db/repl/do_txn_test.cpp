/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */
#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/repl/do_txn.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/logger/logger.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace repl {
namespace {

/**
 * Mock OpObserver that tracks doTxn events.  doTxn internally applies its arguments using applyOps.
 */
class OpObserverMock : public OpObserverNoop {
public:
    /**
     * Called by doTxn() when ops are applied atomically.
     */
    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& doTxnCmd) override;

    // If not empty, holds the command object passed to last invocation of onApplyOps().
    BSONObj onApplyOpsCmdObj;
};

void OpObserverMock::onApplyOps(OperationContext* opCtx,
                                const std::string& dbName,
                                const BSONObj& doTxnCmd) {
    ASSERT_FALSE(doTxnCmd.isEmpty());
    // Get owned copy because 'doTxnCmd' may be a temporary BSONObj created by doTxn().
    onApplyOpsCmdObj = doTxnCmd.getOwned();
}

/**
 * Test fixture for doTxn().
 */
class DoTxnTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    OpObserverMock* _opObserver = nullptr;
    std::unique_ptr<StorageInterface> _storage;
};

void DoTxnTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    auto opCtx = cc().makeOperationContext();

    // Set up ReplicationCoordinator and create oplog.
    ReplicationCoordinator::set(service, stdx::make_unique<ReplicationCoordinatorMock>(service));
    setOplogCollectionName(service);
    createOplog(opCtx.get());

    // Ensure that we are primary.
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for doTxn().
    auto opObserver = stdx::make_unique<OpObserverMock>();
    _opObserver = opObserver.get();
    service->setOpObserver(std::move(opObserver));

    // This test uses StorageInterface to create collections and inspect documents inside
    // collections.
    _storage = stdx::make_unique<StorageInterfaceImpl>();
}

void DoTxnTest::tearDown() {
    _storage = {};
    _opObserver = nullptr;

    // Reset default log level in case it was changed.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kReplication,
                                                        logger::LogSeverity::Debug(0));

    ServiceContextMongoDTest::tearDown();
}

/**
 * Fixes up result document returned by doTxn and converts to Status.
 */
Status getStatusFromDoTxnResult(const BSONObj& result) {
    if (result["ok"]) {
        return getStatusFromCommandResult(result);
    }

    BSONObjBuilder builder;
    builder.appendElements(result);
    auto code = result.getIntField("code");
    builder.appendIntOrLL("ok", code == 0);
    auto newResult = builder.obj();
    return getStatusFromCommandResult(newResult);
}

TEST_F(DoTxnTest, AtomicDoTxnWithNoOpsReturnsSuccess) {
    auto opCtx = cc().makeOperationContext();
    BSONObjBuilder resultBuilder;
    auto cmdObj = BSON("doTxn" << BSONArray());
    auto expectedCmdObj = BSON("applyOps" << BSONArray());
    ASSERT_OK(doTxn(opCtx.get(), "test", cmdObj, &resultBuilder));
    ASSERT_BSONOBJ_EQ(expectedCmdObj, _opObserver->onApplyOpsCmdObj);
}

BSONObj makeInsertOperation(const NamespaceString& nss,
                            const OptionalCollectionUUID& uuid,
                            const BSONObj& documentToInsert) {
    return uuid ? BSON("op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "o"
                       << documentToInsert
                       << "ui"
                       << *uuid)
                : BSON("op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "o"
                       << documentToInsert);
}

/**
 * Creates an doTxn command object with a single insert operation.
 */
BSONObj makeDoTxnWithInsertOperation(const NamespaceString& nss,
                                     const OptionalCollectionUUID& uuid,
                                     const BSONObj& documentToInsert) {
    auto insertOp = makeInsertOperation(nss, uuid, documentToInsert);
    return BSON("doTxn" << BSON_ARRAY(insertOp));
}

/**
 * Creates an applyOps command object with a single insert operation.
 */
BSONObj makeApplyOpsWithInsertOperation(const NamespaceString& nss,
                                        const OptionalCollectionUUID& uuid,
                                        const BSONObj& documentToInsert) {
    auto insertOp = makeInsertOperation(nss, uuid, documentToInsert);
    return BSON("applyOps" << BSON_ARRAY(insertOp));
}

TEST_F(DoTxnTest, AtomicDoTxnInsertIntoNonexistentCollectionReturnsNamespaceNotFoundInResult) {
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss("test.t");
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeDoTxnWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::UnknownError, doTxn(opCtx.get(), "test", cmdObj, &resultBuilder));
    auto result = resultBuilder.obj();
    auto status = getStatusFromDoTxnResult(result);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(DoTxnTest, AtomicDoTxnInsertIntoCollectionWithoutUuid) {
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss("test.t");

    // Collection has no uuid.
    CollectionOptions collectionOptions;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeDoTxnWithInsertOperation(nss, boost::none, documentToInsert);
    auto expectedCmdObj = makeApplyOpsWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_OK(doTxn(opCtx.get(), "test", cmdObj, &resultBuilder));
    ASSERT_BSONOBJ_EQ(expectedCmdObj, _opObserver->onApplyOpsCmdObj);
}

TEST_F(DoTxnTest, AtomicDoTxnInsertWithUuidIntoCollectionWithUuid) {
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss("test.t");

    auto uuid = UUID::gen();

    CollectionOptions collectionOptions;
    collectionOptions.uuid = uuid;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeDoTxnWithInsertOperation(nss, uuid, documentToInsert);
    auto expectedCmdObj = makeApplyOpsWithInsertOperation(nss, uuid, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_OK(doTxn(opCtx.get(), "test", cmdObj, &resultBuilder));
    ASSERT_BSONOBJ_EQ(expectedCmdObj, _opObserver->onApplyOpsCmdObj);
}

TEST_F(DoTxnTest, AtomicDoTxnInsertWithUuidIntoCollectionWithoutUuid) {
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss("test.t");

    auto uuid = UUID::gen();

    // Collection has no uuid.
    CollectionOptions collectionOptions;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    // The doTxn returns a NamespaceNotFound error because of the failed UUID lookup
    // even though a collection exists with the same namespace as the insert operation.
    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeDoTxnWithInsertOperation(nss, uuid, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_EQUALS(ErrorCodes::UnknownError, doTxn(opCtx.get(), "test", cmdObj, &resultBuilder));
    auto result = resultBuilder.obj();
    auto status = getStatusFromDoTxnResult(result);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
}

TEST_F(DoTxnTest, AtomicDoTxnInsertWithoutUuidIntoCollectionWithUuid) {
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss("test.t");

    auto uuid = UUID::gen();

    CollectionOptions collectionOptions;
    collectionOptions.uuid = uuid;
    ASSERT_OK(_storage->createCollection(opCtx.get(), nss, collectionOptions));

    auto documentToInsert = BSON("_id" << 0);
    auto cmdObj = makeDoTxnWithInsertOperation(nss, boost::none, documentToInsert);
    BSONObjBuilder resultBuilder;
    ASSERT_OK(doTxn(opCtx.get(), "test", cmdObj, &resultBuilder));

    // Insert operation provided by caller did not contain collection uuid but doTxn() should add
    // the uuid to the oplog entry.
    auto expectedCmdObj = makeApplyOpsWithInsertOperation(nss, uuid, documentToInsert);
    ASSERT_BSONOBJ_EQ(expectedCmdObj, _opObserver->onApplyOpsCmdObj);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
