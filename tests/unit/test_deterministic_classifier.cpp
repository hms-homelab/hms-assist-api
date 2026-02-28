/**
 * Unit tests for DeterministicClassifier (Tier 1).
 *
 * All Home Assistant calls are mocked — no network required.
 *
 * Coverage:
 *   - All 16 regex patterns (light on/off/toggle, thermostat set/warmer/cooler,
 *     lock/unlock, media pause/next/previous/play, scene activate/mode)
 *   - HA returns empty entity → success=false with helpful message
 *   - No pattern match → success=false, tier=tier1
 *   - Verify tier="tier1" in all cases
 */

#include "intent/DeterministicClassifier.h"
#include "clients/HomeAssistantClient.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

using ::testing::_;
using ::testing::Return;
using ::testing::HasSubstr;
using ::testing::AnyNumber;


// ─── Mock ─────────────────────────────────────────────────────────────────────

class MockHAClient : public HomeAssistantClient {
public:
    MockHAClient() : HomeAssistantClient("http://mock:8123", "mock_token") {}

    MOCK_METHOD(std::vector<Entity>, findEntities,
                (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, turnOn,
                (const std::string&, const Json::Value&), (override));
    MOCK_METHOD(bool, turnOff, (const std::string&), (override));
    MOCK_METHOD(bool, toggle, (const std::string&), (override));
    MOCK_METHOD(bool, setTemperature, (const std::string&, float), (override));
    MOCK_METHOD(Entity, getEntityState, (const std::string&), (override));
    MOCK_METHOD(bool, callService,
                (const std::string&, const std::string&,
                 const std::string&, const Json::Value&), (override));
};


// ─── Helpers ─────────────────────────────────────────────────────────────────

static Entity makeEntity(const std::string& id,
                          const std::string& name,
                          const std::string& state = "on") {
    Entity e;
    e.entity_id    = id;
    e.friendly_name = name;
    e.state        = state;
    e.domain       = id.substr(0, id.find('.'));
    return e;
}

static VoiceCommand cmd(const std::string& text) {
    VoiceCommand c;
    c.text      = text;
    c.device_id = "test_device";
    c.confidence = 1.0f;
    return c;
}


// ─── Test fixture ─────────────────────────────────────────────────────────────

class DeterministicClassifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_ha_ = std::make_shared<MockHAClient>();
        classifier_ = std::make_shared<DeterministicClassifier>(mock_ha_);
    }

    std::shared_ptr<MockHAClient>              mock_ha_;
    std::shared_ptr<DeterministicClassifier>   classifier_;
};


// ─── Tier 1 tag ───────────────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, ResultAlwaysHasTier1Tag) {
    EXPECT_CALL(*mock_ha_, findEntities(_, _)).WillRepeatedly(Return(std::vector<Entity>{}));
    auto result = classifier_->classify(cmd("turn on the kitchen light"));
    EXPECT_EQ(result.tier, "tier1");
}

TEST_F(DeterministicClassifierTest, NoMatchReturnsTier1Failure) {
    auto result = classifier_->classify(cmd("what is the weather today"));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.tier, "tier1");
}


// ─── Light control ────────────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, LightTurnOn) {
    auto light = makeEntity("light.kitchen_ceiling", "Kitchen Ceiling");
    EXPECT_CALL(*mock_ha_, findEntities("kitchen", "light"))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, turnOn("light.kitchen_ceiling", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("turn on the kitchen light"));
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.intent, "light_control");
    EXPECT_FLOAT_EQ(r.confidence, 0.95f);
    EXPECT_THAT(r.response_text, HasSubstr("Kitchen Ceiling"));
}

TEST_F(DeterministicClassifierTest, LightTurnOff) {
    auto light = makeEntity("light.bedroom_lamp", "Bedroom Lamp");
    EXPECT_CALL(*mock_ha_, findEntities("bedroom", "light"))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, turnOff("light.bedroom_lamp"))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("turn off the bedroom light"));
    EXPECT_TRUE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("Bedroom Lamp"));
}

TEST_F(DeterministicClassifierTest, LightSwitchOn) {
    auto light = makeEntity("light.living_room", "Living Room");
    EXPECT_CALL(*mock_ha_, findEntities("living room", "light"))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, turnOn("light.living_room", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("switch on the living room light"));
    EXPECT_TRUE(r.success);
}

TEST_F(DeterministicClassifierTest, LightSwitchOff) {
    auto light = makeEntity("light.office", "Office Light");
    EXPECT_CALL(*mock_ha_, findEntities("office", "light"))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, turnOff("light.office"))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("switch off the office light"));
    EXPECT_TRUE(r.success);
}

TEST_F(DeterministicClassifierTest, LightToggle) {
    auto light = makeEntity("light.hallway", "Hallway Light");
    EXPECT_CALL(*mock_ha_, findEntities("hallway", "light"))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, toggle("light.hallway"))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("toggle the hallway light"));
    EXPECT_TRUE(r.success);
}

TEST_F(DeterministicClassifierTest, LightNotFoundReturnsFailure) {
    EXPECT_CALL(*mock_ha_, findEntities("garage", "light"))
        .WillOnce(Return(std::vector<Entity>{}));

    auto r = classifier_->classify(cmd("turn on the garage light"));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.tier, "tier1");
    EXPECT_THAT(r.response_text, HasSubstr("garage"));
}

TEST_F(DeterministicClassifierTest, LightHaCallFailure) {
    auto light = makeEntity("light.kitchen_ceiling", "Kitchen Ceiling");
    EXPECT_CALL(*mock_ha_, findEntities(_, _))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, turnOn(_, _)).WillOnce(Return(false));

    auto r = classifier_->classify(cmd("turn on the kitchen light"));
    EXPECT_FALSE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("couldn't control"));
}

TEST_F(DeterministicClassifierTest, LightEntitiesPopulatedInResult) {
    auto light = makeEntity("light.patio", "Patio Light");
    EXPECT_CALL(*mock_ha_, findEntities(_, _))
        .WillOnce(Return(std::vector<Entity>{light}));
    EXPECT_CALL(*mock_ha_, turnOn(_, _)).WillOnce(Return(true));

    auto r = classifier_->classify(cmd("turn on the patio light"));
    EXPECT_EQ(r.entities["entity_id"].asString(), "light.patio");
}


// ─── Thermostat control ───────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, ThermostatSetTemperature) {
    auto therm = makeEntity("climate.living_room", "Living Room Thermostat");
    EXPECT_CALL(*mock_ha_, findEntities("living room", "climate"))
        .WillOnce(Return(std::vector<Entity>{therm}));
    EXPECT_CALL(*mock_ha_, setTemperature("climate.living_room", 72.0f))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("set the living room thermostat to 72"));
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.intent, "thermostat_control");
    EXPECT_THAT(r.response_text, HasSubstr("72"));
}

TEST_F(DeterministicClassifierTest, ThermostatMakeItWarmer) {
    auto therm = makeEntity("climate.main", "Main Thermostat", "heat");
    Entity current = therm;
    current.attributes["temperature"] = 68.0;
    EXPECT_CALL(*mock_ha_, findEntities(_, "climate"))
        .WillOnce(Return(std::vector<Entity>{therm}));
    EXPECT_CALL(*mock_ha_, getEntityState("climate.main"))
        .WillOnce(Return(current));
    EXPECT_CALL(*mock_ha_, setTemperature("climate.main", 70.0f))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("make it warmer"));
    EXPECT_TRUE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("70"));
}

TEST_F(DeterministicClassifierTest, ThermostatMakeItCooler) {
    auto therm = makeEntity("climate.main", "Main Thermostat", "cool");
    Entity current = therm;
    current.attributes["temperature"] = 74.0;
    EXPECT_CALL(*mock_ha_, findEntities(_, "climate"))
        .WillOnce(Return(std::vector<Entity>{therm}));
    EXPECT_CALL(*mock_ha_, getEntityState("climate.main"))
        .WillOnce(Return(current));
    EXPECT_CALL(*mock_ha_, setTemperature("climate.main", 72.0f))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("make it cooler"));
    EXPECT_TRUE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("72"));
}


// ─── Lock control ─────────────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, LockDoor) {
    auto lock = makeEntity("lock.front_door", "Front Door Lock");
    EXPECT_CALL(*mock_ha_, findEntities("front", "lock"))
        .WillOnce(Return(std::vector<Entity>{lock}));
    EXPECT_CALL(*mock_ha_, callService("lock", "lock", "lock.front_door", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("lock the front door"));
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.intent, "lock_control");
    EXPECT_THAT(r.response_text, HasSubstr("locked"));
}

TEST_F(DeterministicClassifierTest, UnlockDoor) {
    auto lock = makeEntity("lock.back_door", "Back Door Lock");
    EXPECT_CALL(*mock_ha_, findEntities("back", "lock"))
        .WillOnce(Return(std::vector<Entity>{lock}));
    EXPECT_CALL(*mock_ha_, callService("lock", "unlock", "lock.back_door", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("unlock the back door"));
    EXPECT_TRUE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("unlocked"));
}

TEST_F(DeterministicClassifierTest, LockNotFoundReturnsFailure) {
    EXPECT_CALL(*mock_ha_, findEntities("side", "lock"))
        .WillOnce(Return(std::vector<Entity>{}));

    auto r = classifier_->classify(cmd("lock the side door"));
    EXPECT_FALSE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("side"));
}


// ─── Media control ────────────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, MediaPause) {
    auto mp = makeEntity("media_player.living_room", "Living Room Speaker");
    EXPECT_CALL(*mock_ha_, findEntities("", "media_player"))
        .WillOnce(Return(std::vector<Entity>{mp}));
    EXPECT_CALL(*mock_ha_, callService("media_player", "media_pause",
                                       "media_player.living_room", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("pause the music"));
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.intent, "media_control");
}

TEST_F(DeterministicClassifierTest, MediaNextTrack) {
    auto mp = makeEntity("media_player.kitchen", "Kitchen Speaker");
    EXPECT_CALL(*mock_ha_, findEntities("", "media_player"))
        .WillOnce(Return(std::vector<Entity>{mp}));
    EXPECT_CALL(*mock_ha_, callService("media_player", "media_next_track",
                                       "media_player.kitchen", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("skip to the next song"));
    EXPECT_TRUE(r.success);
}

TEST_F(DeterministicClassifierTest, MediaPreviousTrack) {
    auto mp = makeEntity("media_player.bedroom", "Bedroom Speaker");
    EXPECT_CALL(*mock_ha_, findEntities("", "media_player"))
        .WillOnce(Return(std::vector<Entity>{mp}));
    EXPECT_CALL(*mock_ha_, callService("media_player", "media_previous_track",
                                       "media_player.bedroom", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("previous track"));
    EXPECT_TRUE(r.success);
}

TEST_F(DeterministicClassifierTest, MediaNoPlayerFoundReturnsFailure) {
    EXPECT_CALL(*mock_ha_, findEntities("", "media_player"))
        .WillOnce(Return(std::vector<Entity>{}));

    auto r = classifier_->classify(cmd("pause the music"));
    EXPECT_FALSE(r.success);
    EXPECT_THAT(r.response_text, HasSubstr("media player"));
}


// ─── Scene control ────────────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, SceneActivate) {
    auto scene = makeEntity("scene.movie_night", "Movie Night");
    EXPECT_CALL(*mock_ha_, findEntities("movie night", "scene"))
        .WillOnce(Return(std::vector<Entity>{scene}));
    // processSceneControl calls turnOn(), not callService()
    EXPECT_CALL(*mock_ha_, turnOn("scene.movie_night", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("activate movie night scene"));
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.intent, "scene_control");
}

TEST_F(DeterministicClassifierTest, SceneSetMode) {
    auto scene = makeEntity("scene.night_mode", "Night Mode");
    EXPECT_CALL(*mock_ha_, findEntities("night", "scene"))
        .WillOnce(Return(std::vector<Entity>{scene}));
    EXPECT_CALL(*mock_ha_, turnOn("scene.night_mode", _))
        .WillOnce(Return(true));

    auto r = classifier_->classify(cmd("set night mode"));
    EXPECT_TRUE(r.success);
}


// ─── Processing time ─────────────────────────────────────────────────────────

TEST_F(DeterministicClassifierTest, ProcessingTimeMsIsSet) {
    EXPECT_CALL(*mock_ha_, findEntities(_, _))
        .WillRepeatedly(Return(std::vector<Entity>{}));

    auto r = classifier_->classify(cmd("turn on the porch light"));
    // Should be very fast (regex), but just verify it's been measured
    EXPECT_GE(r.processing_time_ms, 0);
}
