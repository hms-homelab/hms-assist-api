/**
 * Unit tests for EmbeddingClassifier (Tier 2).
 *
 * Tests:
 *   - inferAction(): sensor domain → "query_state"
 *   - classify() with sensor entity: calls getEntityState, NOT callService
 *   - Formatted response text ("AWN Outdoor Temperature is 71.10 °F")
 *   - dry_run: entity resolved, no HA calls
 *   - intent == "sensor_query", tier == "tier2", success == true
 *
 * Devices used: sensor.awn_outdoor_temperature, sensor.awn_outdoor_humidity,
 *               light.sala_1 (sanity check only)
 *
 * All dependencies mocked — no network, no DB, no HA required.
 */

#include "intent/EmbeddingClassifier.h"
#include "clients/HomeAssistantClient.h"
#include "clients/OllamaClient.h"
#include "services/VectorSearchService.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::Invoke;


// ─── Mocks (anonymous namespace — no link conflict with other test files) ─────

namespace {

class MockOllamaClient : public OllamaClient {
public:
    MockOllamaClient() : OllamaClient("") {}
    MOCK_METHOD(std::vector<float>, embed,
                (const std::string&, const std::string&), (override));
    MOCK_METHOD(Json::Value, chatJson,
                (const std::string&, const std::string&,
                 const std::string&, float), (override));
};

class MockHomeAssistantClient : public HomeAssistantClient {
public:
    MockHomeAssistantClient() : HomeAssistantClient("", "") {}
    MOCK_METHOD(std::vector<Entity>, getAllEntities, (), (override));
    MOCK_METHOD(Entity, getEntityState, (const std::string&), (override));
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

class MockVectorSearchService : public VectorSearchService {
public:
    MockVectorSearchService() : VectorSearchService("") {}
    MOCK_METHOD(std::vector<EntityMatch>, search,
                (const std::vector<float>&, float, int), (override));
};

} // namespace


// ─── Helpers ─────────────────────────────────────────────────────────────────

static EntityMatch makeEntityMatch(const std::string& entityId,
                                    const std::string& domain,
                                    const std::string& friendlyName,
                                    float similarity = 0.85f) {
    EntityMatch m;
    m.entity_id     = entityId;
    m.domain        = domain;
    m.friendly_name = friendlyName;
    m.state         = "";
    m.similarity    = similarity;
    return m;
}

static Entity makeEntity(const std::string& entityId,
                          const std::string& state,
                          const std::string& unit = "") {
    Entity e;
    e.entity_id = entityId;
    e.state     = state;
    if (!unit.empty()) {
        e.attributes["unit_of_measurement"] = unit;
    }
    return e;
}


// ─── Fixture ─────────────────────────────────────────────────────────────────

class EmbeddingClassifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        ollama_ = std::make_shared<NiceMock<MockOllamaClient>>();
        ha_     = std::make_shared<NiceMock<MockHomeAssistantClient>>();
        vector_ = std::make_shared<NiceMock<MockVectorSearchService>>();

        // Default: embed returns a dummy vector, search returns empty
        ON_CALL(*ollama_, embed(_, _))
            .WillByDefault(Return(std::vector<float>(384, 0.1f)));
        ON_CALL(*vector_, search(_, _, _))
            .WillByDefault(Return(std::vector<EntityMatch>{}));
        ON_CALL(*ha_, callService(_, _, _, _)).WillByDefault(Return(true));

        cls_ = std::make_unique<EmbeddingClassifier>(
            ollama_, ha_, vector_,
            "nomic-embed-text", /*threshold=*/0.65f, /*limit=*/5);
    }

    std::shared_ptr<NiceMock<MockOllamaClient>>          ollama_;
    std::shared_ptr<NiceMock<MockHomeAssistantClient>>   ha_;
    std::shared_ptr<NiceMock<MockVectorSearchService>>   vector_;
    std::unique_ptr<EmbeddingClassifier>                  cls_;
};


// ─── Tests ───────────────────────────────────────────────────────────────────

// 1. inferAction returns "query_state" for sensor domain.
// Tested indirectly via classify(): sensor entity → intent == "sensor_query".
TEST_F(EmbeddingClassifierTest, InferAction_SensorDomain_ReturnsQueryState) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_temperature", "sensor",
                                     "AWN Outdoor Temperature");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("sensor.awn_outdoor_temperature", "71.10", "°F");
    ON_CALL(*ha_, getEntityState("sensor.awn_outdoor_temperature"))
        .WillByDefault(Return(e));

    VoiceCommand cmd;
    cmd.text = "tell me the outdoor temperature";
    auto result = cls_->classify(cmd);

    EXPECT_EQ(result.intent, "sensor_query");
}

// 2. inferAction returns a non-query_state action for light domain (sanity check).
TEST_F(EmbeddingClassifierTest, InferAction_LightDomain_ReturnsTurnOn) {
    EntityMatch m = makeEntityMatch("light.sala_1", "light", "Sala 1");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("light.sala_1", "on");
    ON_CALL(*ha_, getEntityState("light.sala_1")).WillByDefault(Return(e));

    VoiceCommand cmd;
    cmd.text = "turn on sala 1";
    auto result = cls_->classify(cmd);

    EXPECT_NE(result.intent, "sensor_query");
    EXPECT_TRUE(result.success);
}

// 3. For sensor entity: getEntityState is called, callService is NOT called.
TEST_F(EmbeddingClassifierTest, Classify_SensorEntity_CallsGetEntityState_NotCallService) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_temperature", "sensor",
                                     "AWN Outdoor Temperature");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("sensor.awn_outdoor_temperature", "71.10", "°F");
    EXPECT_CALL(*ha_, getEntityState("sensor.awn_outdoor_temperature"))
        .WillOnce(Return(e));
    EXPECT_CALL(*ha_, callService(_, _, _, _)).Times(0);

    VoiceCommand cmd;
    cmd.text = "tell me the outdoor temperature";
    cls_->classify(cmd);
}

// 4. Response text is formatted as "<friendly_name> is <state> <unit>".
TEST_F(EmbeddingClassifierTest, Classify_SensorEntity_ReturnsFormattedResponse) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_temperature", "sensor",
                                     "AWN Outdoor Temperature");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("sensor.awn_outdoor_temperature", "71.10", "°F");
    ON_CALL(*ha_, getEntityState("sensor.awn_outdoor_temperature"))
        .WillByDefault(Return(e));

    VoiceCommand cmd;
    cmd.text = "tell me the outdoor temperature";
    auto result = cls_->classify(cmd);

    EXPECT_EQ(result.response_text, "AWN Outdoor Temperature is 71.10 °F");
}

// 5. dry_run: entity is resolved, no getEntityState or callService called.
TEST_F(EmbeddingClassifierTest, Classify_SensorEntity_DryRun_NoGetEntityState) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_temperature", "sensor",
                                     "AWN Outdoor Temperature");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    EXPECT_CALL(*ha_, getEntityState(_)).Times(0);
    EXPECT_CALL(*ha_, callService(_, _, _, _)).Times(0);

    VoiceCommand cmd;
    cmd.text    = "tell me the outdoor temperature";
    cmd.dry_run = true;
    auto result = cls_->classify(cmd);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entities.get("entity_id", "").asString(),
              "sensor.awn_outdoor_temperature");
}

// 6. success is true for a sensor query.
TEST_F(EmbeddingClassifierTest, Classify_SensorEntity_Success_True) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_humidity", "sensor",
                                     "AWN Outdoor Humidity");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("sensor.awn_outdoor_humidity", "95.00", "%");
    ON_CALL(*ha_, getEntityState("sensor.awn_outdoor_humidity"))
        .WillByDefault(Return(e));

    VoiceCommand cmd;
    cmd.text = "tell me the outdoor humidity";
    auto result = cls_->classify(cmd);

    EXPECT_TRUE(result.success);
}

// 7. intent is "sensor_query" for sensor entity.
TEST_F(EmbeddingClassifierTest, Classify_SensorEntity_IntentIsSensorQuery) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_humidity", "sensor",
                                     "AWN Outdoor Humidity");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("sensor.awn_outdoor_humidity", "95.00", "%");
    ON_CALL(*ha_, getEntityState("sensor.awn_outdoor_humidity"))
        .WillByDefault(Return(e));

    VoiceCommand cmd;
    cmd.text = "tell me the outdoor humidity";
    auto result = cls_->classify(cmd);

    EXPECT_EQ(result.intent, "sensor_query");
}

// 8. tier is "tier2" for sensor query (same as all other Tier 2 results).
TEST_F(EmbeddingClassifierTest, Classify_SensorEntity_TierIsTier2) {
    EntityMatch m = makeEntityMatch("sensor.awn_outdoor_temperature", "sensor",
                                     "AWN Outdoor Temperature");
    ON_CALL(*vector_, search(_, _, _))
        .WillByDefault(Return(std::vector<EntityMatch>{m}));

    Entity e = makeEntity("sensor.awn_outdoor_temperature", "71.10", "°F");
    ON_CALL(*ha_, getEntityState("sensor.awn_outdoor_temperature"))
        .WillByDefault(Return(e));

    VoiceCommand cmd;
    cmd.text = "what is the outdoor temperature";
    auto result = cls_->classify(cmd);

    EXPECT_EQ(result.tier, "tier2");
}
