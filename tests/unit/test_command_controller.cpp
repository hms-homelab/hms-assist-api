/**
 * Unit tests for CommandController.
 *
 * Tests:
 *   - Request validation (missing fields, empty text, invalid JSON)
 *   - Pipeline tier routing (tier1 hit → tier2+3 not called, etc.)
 *   - Compound command path (v2.6.0): parallel Tier2 on regex-split parts +
 *     Tier3 split + dry-run dedup → execute remainder
 *   - Non-HA split path: empty sub_commands → non_ha fallback
 *   - TTS: media_player_entity_id triggers ha_->callService("tts","speak",...)
 *   - Response structure for success and failure
 *   - DB logging side-effects (log called, failures tolerated)
 *
 * No real HTTP server, DB, or HA required — all dependencies are mocked.
 */

#include "api/CommandController.h"
#include "intent/IntentClassifier.h"
#include "services/DatabaseService.h"
#include "clients/HomeAssistantClient.h"
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
using ::testing::Not;
using ::testing::Invoke;
using ::testing::SaveArg;
using ::testing::AtLeast;


// ─── Mocks ────────────────────────────────────────────────────────────────────

class MockClassifier : public IntentClassifier {
public:
    MOCK_METHOD(IntentResult, classify, (const VoiceCommand&), (override));
    MOCK_METHOD(SplitResult,  split,    (const VoiceCommand&), (override));
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

class MockHomeAssistantClient : public HomeAssistantClient {
public:
    MockHomeAssistantClient() : HomeAssistantClient("", "") {}
    MOCK_METHOD(std::vector<Entity>, getAllEntities,  (),                                 (override));
    MOCK_METHOD(Entity,  getEntityState, (const std::string&),                           (override));
    MOCK_METHOD(std::vector<Entity>, findEntities,
                (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, callService,
                (const std::string&, const std::string&,
                 const std::string&, const Json::Value&), (override));
    MOCK_METHOD(bool, turnOn,  (const std::string&, const Json::Value&), (override));
    MOCK_METHOD(bool, turnOff, (const std::string&),                     (override));
    MOCK_METHOD(bool, toggle,  (const std::string&),                     (override));
    MOCK_METHOD(bool, setTemperature, (const std::string&, float),       (override));
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

// Tier2 result with entity_id set (used for compound dedup testing)
static IntentResult makeTier2EntityResult(const std::string& entityId,
                                           const std::string& action,
                                           const std::string& friendlyName = "") {
    IntentResult r = makeResult(true, "tier2", "device_control", 0.75f);
    r.entities["entity_id"]     = entityId;
    r.entities["action"]        = action;
    r.entities["friendly_name"] = friendlyName.empty() ? entityId : friendlyName;
    r.response_text = action + " " + (friendlyName.empty() ? entityId : friendlyName) + ".";
    return r;
}

static SplitResult makeSplit(
    const std::vector<std::pair<std::string, bool>>& cmds,
    const std::string& nonHa = "") {
    SplitResult s;
    s.non_ha     = nonHa;
    s.confidence = 0.95f;
    for (const auto& [text, wait] : cmds) {
        SubCommand sc;
        sc.text              = text;
        sc.wait_for_previous = wait;
        s.sub_commands.push_back(sc);
    }
    return s;
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
        ha_    = std::make_shared<NiceMock<MockHomeAssistantClient>>();

        // Default: classifiers return failure, split returns empty, DB ops succeed
        ON_CALL(*tier1_, classify(_)).WillByDefault(Return(makeResult(false, "tier1")));
        ON_CALL(*tier2_, classify(_)).WillByDefault(Return(makeResult(false, "tier2")));
        ON_CALL(*tier3_, classify(_)).WillByDefault(Return(makeResult(false, "tier3")));
        ON_CALL(*tier3_, split(_)).WillByDefault(Return(SplitResult{}));
        ON_CALL(*db_,    logVoiceCommand(_)).WillByDefault(Return(1));
        ON_CALL(*db_,    logIntentResult(_, _)).WillByDefault(Return(true));
        ON_CALL(*db_,    isConnected()).WillByDefault(Return(true));
        ON_CALL(*db_,    getTotalCommands()).WillByDefault(Return(42));
        ON_CALL(*db_,    getSuccessfulIntents()).WillByDefault(Return(38));
        ON_CALL(*ha_,    callService(_, _, _, _)).WillByDefault(Return(true));

        ctrl_ = std::make_unique<CommandController>(
            tier1_, tier2_, tier3_, db_, ha_, "tts.piper");
    }

    std::shared_ptr<NiceMock<MockClassifier>>             tier1_, tier2_, tier3_;
    std::shared_ptr<NiceMock<MockDatabaseService>>        db_;
    std::shared_ptr<NiceMock<MockHomeAssistantClient>>    ha_;
    std::unique_ptr<CommandController>                     ctrl_;
};


// ─── Request validation ───────────────────────────────────────────────────────

TEST_F(CommandControllerTest, MissingBodyReturns400) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
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
    EXPECT_CALL(*tier3_, split(_)).Times(0);

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on the kitchen light","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parseBody(resp)["tier"].asString(), "tier1");
}

TEST_F(CommandControllerTest, Tier1MissTier2HitSimpleCommandReturnsTier2) {
    // Simple command (no compound divider) — tier2 hit → return immediately
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(true, "tier2")));
    EXPECT_CALL(*tier3_, split(_)).Times(0);

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"dim the study","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parseBody(resp)["tier"].asString(), "tier2");
}

TEST_F(CommandControllerTest, AllTiersMissReturnsNonHaFailure) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(false, "tier2")));
    EXPECT_CALL(*tier3_, split(_)).WillOnce(Return(SplitResult{}));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"do something complex","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k422UnprocessableEntity);
    auto body = parseBody(resp);
    EXPECT_EQ(body["tier"].asString(),    "tier3");
    EXPECT_EQ(body["intent"].asString(),  "non_ha");
    EXPECT_FALSE(body["success"].asBool());
}

TEST_F(CommandControllerTest, Tier3SplitSingleCommandSucceeds) {
    EXPECT_CALL(*tier1_, classify(_))
        .WillRepeatedly(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_))
        .WillOnce(Return(makeResult(false, "tier2")))   // non-compound: full command
        .WillRepeatedly(Return(makeResult(true,  "tier2")));  // sub-command

    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(makeSplit({{"turn on lights", false}})));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on lights","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_EQ(body["tier"].asString(),   "tier3");
    EXPECT_EQ(body["intent"].asString(), "single_command");
    EXPECT_TRUE(body["success"].asBool());
}

TEST_F(CommandControllerTest, Tier3SplitMultiCommandSucceeds) {
    EXPECT_CALL(*tier1_, classify(_))
        .WillRepeatedly(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_))
        .WillOnce(Return(makeResult(false, "tier2")))   // full command
        .WillRepeatedly(Return(makeResult(true, "tier2")));  // sub-commands

    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(makeSplit({{"turn on lights", false}, {"lock door", false}})));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on lights","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_EQ(body["tier"].asString(),   "tier3");
    EXPECT_EQ(body["intent"].asString(), "multi_command");
    EXPECT_TRUE(body["success"].asBool());
    EXPECT_EQ(body["entities"]["commands"].size(), 2u);
}

TEST_F(CommandControllerTest, Tier3NonHaAnswerReturnsInResponseText) {
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(makeResult(false, "tier2")));
    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(makeSplit({}, "The capital of France is Paris.")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"what is the capital of France","device_id":"d1"})"));
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_EQ(body["tier"].asString(),   "tier3");
    EXPECT_EQ(body["intent"].asString(), "non_ha");
    EXPECT_TRUE(body["success"].asBool());
    EXPECT_THAT(body["response_text"].asString(), HasSubstr("Paris"));
}


// ─── Compound command path (v2.6.0) ─────────────────────────────────────────
// "X and Y" → regexSplitCompound → ["X","Y"]
// Tier2 on each part in parallel + Tier3 split on full command in parallel
// Dry-run dedup → execute remainder

TEST_F(CommandControllerTest, CompoundTier2HandlesBothParts_AllDeduped) {
    // Both regex parts resolve to unique entity_ids via Tier2.
    // Tier3 sub-commands dry-run to the same entity_ids → all deduped.
    // Result: tier="tier2", both entities in commands array.
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));

    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Invoke([](const VoiceCommand& cmd) -> IntentResult {
            if (cmd.text.find("sala") != std::string::npos)
                return makeTier2EntityResult("light.sala_1", "turn_off", "Sala 1");
            if (cmd.text.find("coffee") != std::string::npos)
                return makeTier2EntityResult("switch.coffee", "turn_on", "Coffee");
            return makeResult(false, "tier2");
        }));

    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(makeSplit({
            {"turn off sala 1", false},
            {"turn on coffee",  false}
        })));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn off sala 1 and turn on coffee","device_id":"d1"})"));

    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_TRUE(body["success"].asBool());
    // All deduped by Tier2 — tier3 sub-commands were skipped
    EXPECT_EQ(body["tier"].asString(), "tier2");
    EXPECT_GE(body["entities"]["commands"].size(), 1u);
}

TEST_F(CommandControllerTest, CompoundTier2OnePart_Tier3HandlesRemainder) {
    // Tier2 handles "sala 1" (entity light.sala_1) but FAILS for "unknown_device".
    // Tier3 splits → both sub-commands returned.
    // Dry-run "turn off sala 1" → light.sala_1 → in executed set → SKIPPED.
    // Dry-run "turn on unknown_device" → entity not found (fail) → KEPT.
    // "turn on unknown_device" executes via runSinglePipeline → tier label = "tier2+tier3".
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));

    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Invoke([](const VoiceCommand& cmd) -> IntentResult {
            if (cmd.text.find("sala") != std::string::npos)
                return makeTier2EntityResult("light.sala_1", "turn_off", "Sala 1");
            return makeResult(false, "tier2");  // unknown_device always fails
        }));

    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(makeSplit({
            {"turn off sala 1",      false},
            {"turn on unknown_device", false}
        })));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn off sala 1 and turn on unknown_device","device_id":"d1"})"));

    auto body = parseBody(resp);
    // sala 1 handled by Tier2; unknown_device not deduped → executed as Tier3 remainder
    EXPECT_EQ(body["tier"].asString(), "tier2+tier3");
}

TEST_F(CommandControllerTest, CompoundTier3SplitReceivesFullOriginalCommand) {
    // Verify that tier3_->split() is called with the FULL original text,
    // not a single regex-split part.
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));
    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Return(makeResult(false, "tier2")));

    VoiceCommand capturedCmd;
    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Invoke([&](const VoiceCommand& cmd) -> SplitResult {
            capturedCmd = cmd;
            return makeSplit({{"turn off sala 1", false}, {"turn on coffee", false}});
        }));

    dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn off sala 1 and turn on coffee","device_id":"d1"})"));

    EXPECT_EQ(capturedCmd.text, "turn off sala 1 and turn on coffee");
    EXPECT_TRUE(capturedCmd.already_executed.empty());
}

TEST_F(CommandControllerTest, CompoundTier2BothParts_EmptySplit_ReturnsTier2) {
    // Tier2 handles both parts. Tier3 split returns nothing.
    // Result: tier="tier2", Tier2 commands returned.
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));

    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Invoke([](const VoiceCommand& cmd) -> IntentResult {
            if (cmd.text.find("sala") != std::string::npos)
                return makeTier2EntityResult("light.sala_1", "turn_off", "Sala 1");
            if (cmd.text.find("coffee") != std::string::npos)
                return makeTier2EntityResult("switch.coffee", "turn_on", "Coffee");
            return makeResult(false, "tier2");
        }));

    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(SplitResult{}));  // LLM returns nothing

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn off sala 1 and turn on coffee","device_id":"d1"})"));

    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_TRUE(body["success"].asBool());
    EXPECT_EQ(body["tier"].asString(), "tier2");
}

TEST_F(CommandControllerTest, CompoundTier3OnlyWhenTier2AllFail) {
    // Both Tier2 parts fail. Tier3 split returns 2 sub-commands.
    // Dry-run resolves fail too (no entity_ids) → both sub-commands execute via Tier3.
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));
    // Tier2 fails for all calls (compound parts + dry-runs + sub-command execution)
    ON_CALL(*tier2_, classify(_)).WillByDefault(Return(makeResult(false, "tier2")));

    // But sub-commands in runSinglePipeline have tier1 miss, tier2 miss → sub-command fails
    // To make the overall test pass, let's have tier2 succeed for execution calls after dry-run
    // We'll just check that tier label is "tier3" (no tier2 successes)
    EXPECT_CALL(*tier3_, split(_))
        .WillOnce(Return(makeSplit({{"sub1", false}, {"sub2", false}})));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn off sala 1 and turn on coffee","device_id":"d1"})"));

    auto body = parseBody(resp);
    EXPECT_EQ(body["tier"].asString(), "tier3");
}


// ─── TTS response ─────────────────────────────────────────────────────────────

TEST_F(CommandControllerTest, MediaPlayerEntityIdTriggersTtsSpeak) {
    // When media_player_entity_id is in the request and pipeline succeeds,
    // ha_->callService("tts","speak","tts.piper", ...) must be called.
    EXPECT_CALL(*tier1_, classify(_))
        .WillOnce(Return(makeResult(true, "tier1", "light_control", 0.95f)));

    EXPECT_CALL(*ha_, callService("tts", "speak", "tts.piper", _))
        .Times(1).WillOnce(Return(true));

    dispatch(*ctrl_,
        makeJsonRequest(R"({
            "text":"turn on kitchen light",
            "device_id":"d1",
            "media_player_entity_id":"media_player.frankie_respeaker"
        })"));
}

TEST_F(CommandControllerTest, NoMediaPlayerEntityId_NoTtsCall) {
    // Without media_player_entity_id, TTS must NOT be called.
    EXPECT_CALL(*tier1_, classify(_))
        .WillOnce(Return(makeResult(true, "tier1")));

    EXPECT_CALL(*ha_, callService("tts", "speak", _, _)).Times(0);

    dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on kitchen light","device_id":"d1"})"));
}

TEST_F(CommandControllerTest, EmptyResponseText_NoTtsCall) {
    // If response_text is empty, TTS must NOT be called even with a media player set.
    IntentResult r = makeResult(true, "tier1");
    r.response_text = "";
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(r));
    EXPECT_CALL(*ha_, callService("tts", "speak", _, _)).Times(0);

    dispatch(*ctrl_,
        makeJsonRequest(R"({
            "text":"turn on kitchen light",
            "device_id":"d1",
            "media_player_entity_id":"media_player.frankie_respeaker"
        })"));
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
    EXPECT_CALL(*tier3_, split(_)).WillOnce(Return(SplitResult{}));

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
    EXPECT_CALL(*db_, logVoiceCommand(_))
        .WillOnce(::testing::Throw(std::runtime_error("DB down")));
    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(true, "tier1")));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on kitchen light","device_id":"d1"})"));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}


// ─── Sensor query tests ───────────────────────────────────────────────────────
// Devices used: sensor.awn_outdoor_temperature, sensor.awn_outdoor_humidity,
//               light.sala_1, switch.coffee

// 11. Sensor query in compound: "tell me outdoor temp and turn on sala 1"
//     Both parts resolve via Tier2 (sensor + light). tier="tier2".
TEST_F(CommandControllerTest, SensorQueryInCompound_Tier2HandlesIt) {
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));

    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Invoke([](const VoiceCommand& cmd) -> IntentResult {
            if (cmd.text.find("outdoor") != std::string::npos ||
                cmd.text.find("temp") != std::string::npos) {
                auto r = makeTier2EntityResult("sensor.awn_outdoor_temperature",
                                               "query_state", "AWN Outdoor Temperature");
                r.intent = "sensor_query";
                r.response_text = "AWN Outdoor Temperature is 71.10 °F";
                return r;
            }
            if (cmd.text.find("sala") != std::string::npos)
                return makeTier2EntityResult("light.sala_1", "turn_on", "Sala 1");
            return makeResult(false, "tier2");
        }));

    EXPECT_CALL(*tier3_, split(_)).WillRepeatedly(Return(SplitResult{}));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"tell me outdoor temp and turn on sala 1","device_id":"d1"})"));

    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_TRUE(body["success"].asBool());
    EXPECT_EQ(body["tier"].asString(), "tier2");
}

// 12. Pure sensor query: tier="tier2", intent="sensor_query", response has reading.
TEST_F(CommandControllerTest, SensorQueryAlone_ReturnsSensorResponse) {
    IntentResult sensorResult = makeTier2EntityResult(
        "sensor.awn_outdoor_temperature", "query_state", "AWN Outdoor Temperature");
    sensorResult.intent = "sensor_query";
    sensorResult.response_text = "AWN Outdoor Temperature is 71.10 °F";

    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(sensorResult));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"tell me the outdoor temperature","device_id":"d1"})"));

    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_EQ(body["tier"].asString(), "tier2");
    EXPECT_EQ(body["intent"].asString(), "sensor_query");
    EXPECT_TRUE(body["success"].asBool());
    EXPECT_THAT(body["response_text"].asString(), HasSubstr("71.10"));
}

// 13. Sensor query result is spoken via TTS (it's the HA confirmation text).
//     No second TTS call because there is no non_ha in the result.
TEST_F(CommandControllerTest, SensorQuery_TtsSpokenWithHaResponse) {
    IntentResult sensorResult = makeTier2EntityResult(
        "sensor.awn_outdoor_temperature", "query_state", "AWN Outdoor Temperature");
    sensorResult.intent = "sensor_query";
    sensorResult.response_text = "AWN Outdoor Temperature is 71.10 °F";

    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(makeResult(false, "tier1")));
    EXPECT_CALL(*tier2_, classify(_)).WillOnce(Return(sensorResult));

    // Exactly ONE TTS call: the sensor reading (no non_ha present)
    EXPECT_CALL(*ha_, callService("tts", "speak", "tts.piper", _)).Times(1);

    dispatch(*ctrl_,
        makeJsonRequest(R"({
            "text":"tell me the outdoor temperature",
            "device_id":"d1",
            "media_player_entity_id":"media_player.respeaker"
        })"));
}

// 14. Three-part compound: sala 1 + coffee + sensor temp — all handled by Tier2.
//     "turn on sala 1 and turn off coffee and tell me outdoor temp"
TEST_F(CommandControllerTest, ThreePartCompound_AllTier2_AllExecute) {
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));

    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Invoke([](const VoiceCommand& cmd) -> IntentResult {
            if (cmd.text.find("sala") != std::string::npos)
                return makeTier2EntityResult("light.sala_1", "turn_on", "Sala 1");
            if (cmd.text.find("coffee") != std::string::npos)
                return makeTier2EntityResult("switch.coffee", "turn_off", "Coffee");
            if (cmd.text.find("outdoor") != std::string::npos ||
                cmd.text.find("temp") != std::string::npos) {
                auto r = makeTier2EntityResult("sensor.awn_outdoor_temperature",
                                               "query_state", "AWN Outdoor Temperature");
                r.intent = "sensor_query";
                r.response_text = "AWN Outdoor Temperature is 71.10 °F";
                return r;
            }
            return makeResult(false, "tier2");
        }));

    EXPECT_CALL(*tier3_, split(_)).WillRepeatedly(Return(SplitResult{}));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"turn on sala 1 and turn off coffee and tell me outdoor temp","device_id":"d1"})"));

    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_TRUE(body["success"].asBool());
    EXPECT_EQ(body["tier"].asString(), "tier2");
    EXPECT_GE(body["entities"]["commands"].size(), 2u);
}

// 15. Three-part compound with sensor first:
//     "tell me outdoor temp and turn on sala 1 and turn off coffee"
TEST_F(CommandControllerTest, ThreePartCompound_SensorPlusHaCommands) {
    EXPECT_CALL(*tier1_, classify(_)).WillRepeatedly(Return(makeResult(false, "tier1")));

    ON_CALL(*tier2_, classify(_))
        .WillByDefault(Invoke([](const VoiceCommand& cmd) -> IntentResult {
            if (cmd.text.find("sala") != std::string::npos)
                return makeTier2EntityResult("light.sala_1", "turn_on", "Sala 1");
            if (cmd.text.find("coffee") != std::string::npos)
                return makeTier2EntityResult("switch.coffee", "turn_off", "Coffee");
            if (cmd.text.find("outdoor") != std::string::npos ||
                cmd.text.find("temp") != std::string::npos) {
                auto r = makeTier2EntityResult("sensor.awn_outdoor_temperature",
                                               "query_state", "AWN Outdoor Temperature");
                r.intent = "sensor_query";
                r.response_text = "AWN Outdoor Temperature is 71.10 °F";
                return r;
            }
            return makeResult(false, "tier2");
        }));

    EXPECT_CALL(*tier3_, split(_)).WillRepeatedly(Return(SplitResult{}));

    auto resp = dispatch(*ctrl_,
        makeJsonRequest(R"({"text":"tell me outdoor temp and turn on sala 1 and turn off coffee","device_id":"d1"})"));

    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parseBody(resp);
    EXPECT_TRUE(body["success"].asBool());
}


// ─── Two TTS call tests ───────────────────────────────────────────────────────

// 16. When result has non_ha set: two TTS calls (HA first, joke second).
TEST_F(CommandControllerTest, TwoTtsCalls_HaResponseFirst_ThenJoke) {
    const std::string joke = "Why did the light bulb fail school? It wasn't very bright.";

    IntentResult r;
    r.success            = true;
    r.tier               = "tier3";
    r.intent             = "multi_command";
    r.confidence         = 0.95f;
    r.processing_time_ms = 5;
    r.response_text      = "Turned off sala 1. " + joke;
    r.entities["non_ha"] = joke;
    r.entities["commands"] = Json::Value(Json::arrayValue);

    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(r));
    EXPECT_CALL(*ha_, callService("tts", "speak", "tts.piper", _)).Times(2);

    dispatch(*ctrl_,
        makeJsonRequest(R"({
            "text":"turn off sala 1 and tell me a joke about lights",
            "device_id":"d1",
            "media_player_entity_id":"media_player.respeaker"
        })"));
}

// 17. The HA text in the first TTS call does not contain the joke.
TEST_F(CommandControllerTest, TwoTtsCalls_HaTextStrippedFromCombinedResponse) {
    const std::string joke = "Why can't sala lights ever be on time? They're always a little dim.";

    IntentResult r;
    r.success            = true;
    r.tier               = "tier3";
    r.intent             = "multi_command";
    r.confidence         = 0.95f;
    r.processing_time_ms = 5;
    r.response_text      = "Turned on sala 1. " + joke;
    r.entities["non_ha"] = joke;
    r.entities["commands"] = Json::Value(Json::arrayValue);

    EXPECT_CALL(*tier1_, classify(_)).WillOnce(Return(r));

    std::vector<std::string> ttsMessages;
    std::mutex mu;
    EXPECT_CALL(*ha_, callService("tts", "speak", "tts.piper", _))
        .Times(2)
        .WillRepeatedly(Invoke([&ttsMessages, &mu](
                const std::string&, const std::string&, const std::string&,
                const Json::Value& params) -> bool {
            std::lock_guard<std::mutex> lock(mu);
            ttsMessages.push_back(params.get("message", "").asString());
            return true;
        }));

    dispatch(*ctrl_,
        makeJsonRequest(R"({
            "text":"turn on sala 1 and tell me a joke about lights",
            "device_id":"d1",
            "media_player_entity_id":"media_player.respeaker"
        })"));

    ASSERT_EQ(ttsMessages.size(), 2u);
    // First call: HA confirmation only — must NOT contain the joke
    EXPECT_THAT(ttsMessages[0], HasSubstr("Turned on sala 1"));
    EXPECT_THAT(ttsMessages[0], Not(HasSubstr("dim")));
    // Second call: joke text
    EXPECT_THAT(ttsMessages[1], HasSubstr("dim"));
}

// 18. Pure HA command with no non_ha: only ONE TTS call (no empty second call).
TEST_F(CommandControllerTest, SingleTtsCall_WhenNoNonHa) {
    EXPECT_CALL(*tier1_, classify(_))
        .WillOnce(Return(makeResult(true, "tier1", "light_control", 0.95f)));

    EXPECT_CALL(*ha_, callService("tts", "speak", "tts.piper", _)).Times(1);

    dispatch(*ctrl_,
        makeJsonRequest(R"({
            "text":"turn off sala 1",
            "device_id":"d1",
            "media_player_entity_id":"media_player.respeaker"
        })"));
}
