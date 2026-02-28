/**
 * Unit tests for CommandController.
 *
 * Tests:
 *   - Request validation (missing fields, empty text, invalid JSON)
 *   - Pipeline tier routing (tier1 hit → tier2+3 not called, etc.)
 *   - Response structure for success and failure
 *   - DB logging side-effects (log called, failures tolerated)
 *
 * No real HTTP server, DB, or HA required — all dependencies are mocked.
 */

#include "api/CommandController.h"
#include "intent/IntentClassifier.h"
#include "services/DatabaseService.h"
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <json/json.h>
#include <map>
#include <memory>
#include <sstream>

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::HasSubstr;


// ─── Mocks ────────────────────────────────────────────────────────────────────

class MockClassifier : public IntentClassifier {
public:
    MOCK_METHOD(IntentResult, classify, (const VoiceCommand&), (override));
};

class MockDatabaseService : public DatabaseService {
public:
    MockDatabaseService() : DatabaseService("") {}
    MOCK_METHOD(bool, connect,      (),                                  (override));
    MOCK_METHOD(bool, isConnected,  (),                                  (const, override));
    MOCK_METHOD(int,  logVoiceCommand,  (const VoiceCommand&),           (override));
    MOCK_METHOD(bool, logIntentResult,  (int, const IntentResult&),      (override));
    MOCK_METHOD(int,  getTotalCommands,  (),                             (override));
    MOCK_METHOD(int,  getSuccessfulIntents, (),                          (override));
};


// ─── Helpers ─────────────────────────────────────────────────────────────────

static IntentResult makeResult(bool success,
                                const std::string& tier = "tier1",
                                const std::string& intent = "light_control",
                                float confidence = 0.95f) {
    IntentResult r;
    r.success    = success;
    r.tier       = tier;
    r.intent     = intent;
    r.confidence = confidence;
    r.response_text = success ? "Done." : "Could not classify.";
    r.processing_time_ms = 3;
    return r;
}

static drogon::HttpRequestPtr makeJsonRequest(const std::string& body) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    return req;
}

// Call handleCommand synchronously and capture the response
static drogon::HttpResponsePtr dispatch(CommandController& ctrl,
                                         const drogon::HttpRequestPtr& req) {
    drogon::HttpResponsePtr captured;
    ctrl.handleCommand(req, [&captured](const drogon::HttpResponsePtr& r) {
        captured = r;
    });
    return captured;
}

static Json::Value parseBody(const drogon::HttpResponsePtr& resp) {
    Json::CharReaderBuilder builder;
    Json::Value val;
    std::string errs;
    std::istringstream ss(std::string(resp->getBody()));
    Json::parseFromStream(builder, ss, &val, &errs);
    return val;
}


// ─── Test fixture ─────────────────────────────────────────────────────────────

class CommandControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tier1_ = std::make_shared<NiceMock<MockClassifier>>();
        tier2_ = std::make_shared<NiceMock<MockClassifier>>();
        tier3_ = std::make_shared<NiceMock<MockClassifier>>();
        db_    = std::make_shared<NiceMock<MockDatabaseService>>();

        // Default: tiers return failure, DB ops succeed
        ON_CALL(*tier1_, classify(_)).WillByDefault(Return(makeResult(false, "tier1")));
        ON_CALL(*tier2_, classify(_)).WillByDefault(Return(makeResult(false, "tier2")));
        ON_CALL(*tier3_, classify(_)).WillByDefault(Return(makeResult(false, "tier3a")));
        ON_CALL(*db_,    logVoiceCommand(_)).WillByDefault(Return(1));
        ON_CALL(*db_,    logIntentResult(_, _)).WillByDefault(Return(true));
        ON_CALL(*db_,    isConnected()).WillByDefault(Return(true));
        ON_CALL(*db_,    getTotalCommands()).WillByDefault(Return(42));
        ON_CALL(*db_,    getSuccessfulIntents()).WillByDefault(Return(38));

        ctrl_ = std::make_unique<CommandController>(tier1_, tier2_, tier3_, db_);
    }

    std::shared_ptr<NiceMock<MockClassifier>>       tier1_, tier2_, tier3_;
    std::shared_ptr<NiceMock<MockDatabaseService>>  db_;
    std::unique_ptr<CommandController>               ctrl_;
};


// ─── Request validation ───────────────────────────────────────────────────────

TEST_F(CommandControllerTest, MissingBodyReturns400) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    // No content-type set, no body → getJsonObject returns null
    auto resp = dispatch(*ctrl_, req);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_FALSE(parseBody(resp)["success"].asBool());
}

TEST_F(CommandControllerTest, MissingTextFieldReturns400) {
    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"device_id":"dev1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    auto body = parseBody(resp);
    EXPECT_FALSE(body["success"].asBool());
    EXPECT_THAT(body["error"].asString(), HasSubstr("text"));
}

TEST_F(CommandControllerTest, MissingDeviceIdFieldReturns400) {
    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on the light"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(CommandControllerTest, EmptyTextReturns400) {
    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"","device_id":"dev1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_FALSE(parseBody(resp)["success"].asBool());
}

TEST_F(CommandControllerTest, ValidRequestReachesClassifier) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(true, "tier1")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on the kitchen light","device_id":"satellite_1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}


// ─── Pipeline routing ─────────────────────────────────────────────────────────

TEST_F(CommandControllerTest, Tier1HitSkipsTier2AndTier3) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(true, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).Times(0);
    EXPECT_CALL(*tier3_, classify(_)).Times(0);

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on the kitchen light","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parseBody(resp)["tier"].asString(), "tier1");
}

TEST_F(CommandControllerTest, Tier1MissTier2HitSkipsTier3) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(true, "tier2")));
    EXPECT_CALL(*tier3_, classify(_)).Times(0);

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"dim the study","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parseBody(resp)["tier"].asString(), "tier2");
}

TEST_F(CommandControllerTest, AllTiersMissReturnsTier3Result) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(false, "tier2")));
    EXPECT_CALL(*tier3_, classify(_)).WillOnce(Return(makeResult(false, "tier3a")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"do something complex","device_id":"d1"})"));
    // Failure → 422
    EXPECT_EQ(resp->getStatusCode(), drogon::k422UnprocessableEntity);
    EXPECT_EQ(parseBody(resp)["tier"].asString(), "tier3a");
}

TEST_F(CommandControllerTest, Tier3SuccessReturns200) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(false, "tier2")));
    EXPECT_CALL(*tier3_, classify(_)).WillOnce(Return(makeResult(true, "tier3b", "compound_command")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on lights and lock the door","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parseBody(resp)["tier"].asString(), "tier3b");
}


// ─── Response structure ───────────────────────────────────────────────────────

TEST_F(CommandControllerTest, SuccessResponseHasAllFields) {
    auto result = makeResult(true, "tier1", "light_control", 0.95f);
    result.response_text     = "Done, lights on.";
    result.processing_time_ms = 4;

    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(result));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on kitchen light","device_id":"d1"})"));
    auto body = parseBody(resp);

    EXPECT_TRUE(body["success"].asBool());
    EXPECT_EQ(body["tier"].asString(),    "tier1");
    EXPECT_EQ(body["intent"].asString(),  "light_control");
    EXPECT_FLOAT_EQ(body["confidence"].asFloat(), 0.95f);
    EXPECT_EQ(body["response_text"].asString(), "Done, lights on.");
    EXPECT_EQ(body["processing_time_ms"].asInt(), 4);
}

TEST_F(CommandControllerTest, FailureResponseHasSuccessFalse) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(false, "tier2")));
    EXPECT_CALL(*tier3_, classify(_)).WillOnce(Return(makeResult(false, "tier3a")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"blah blah blah","device_id":"d1"})"));
    EXPECT_FALSE(parseBody(resp)["success"].asBool());
}


// ─── DB logging ───────────────────────────────────────────────────────────────

TEST_F(CommandControllerTest, LogsVoiceCommandAndResult) {
    EXPECT_CALL(*db_, logVoiceCommand(_)).Times(1).WillOnce(Return(7));
    EXPECT_CALL(*db_, logIntentResult(7, _)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(true, "tier1")));

    dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on kitchen light","device_id":"d1"})"));
}

TEST_F(CommandControllerTest, DbLogFailureDoesNotAbortResponse) {
    // DB throws on logVoiceCommand — should still get a valid response
    EXPECT_CALL(*db_, logVoiceCommand(_))
        .WillOnce(::testing::Throw(std::runtime_error("DB down")));
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(true, "tier1")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on kitchen light","device_id":"d1"})"));
    // Despite DB failure, the command should still be processed
    EXPECT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}
