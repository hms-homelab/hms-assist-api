/**
 * Unit tests for LLMClassifier (Tier 3) two-call split.
 *
 * Tests:
 *   - split() makes exactly two chatJson calls per invocation
 *   - Command call uses temperature 0.0 (deterministic)
 *   - Non-HA call uses temperature 0.7 (creative)
 *   - Sub-command text preserves exact original wording ("sala 1" unchanged)
 *   - Sub-commands come from the command call
 *   - non_ha comes from the non-HA call
 *   - If command call fails, non_ha still returned
 *   - If non_ha call fails, sub_commands still returned
 *   - Escalation when non_ha confidence < threshold (smart model called)
 *   - No escalation when confidence >= threshold
 *
 * All mocked — no Ollama, no network required.
 */

#include "intent/LLMClassifier.h"
#include "clients/OllamaClient.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::AnyOf;
using ::testing::FloatEq;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;


// ─── Mock (anonymous namespace) ───────────────────────────────────────────────

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

} // namespace


// ─── Helpers ─────────────────────────────────────────────────────────────────

static Json::Value makeCmdJson(const std::vector<std::string>& texts) {
    Json::Value v(Json::objectValue);
    v["sub_commands"] = Json::Value(Json::arrayValue);
    for (const auto& t : texts) {
        Json::Value sc(Json::objectValue);
        sc["text"]              = t;
        sc["wait_for_previous"] = false;
        v["sub_commands"].append(sc);
    }
    v["has_non_ha"] = false;
    return v;
}

static Json::Value makeNonHaJson(const std::string& nonHa, float confidence) {
    Json::Value v(Json::objectValue);
    v["non_ha"]     = nonHa;
    v["confidence"] = confidence;
    return v;
}

static Json::Value emptyJson() {
    return Json::Value(Json::objectValue);
}


// ─── Fixture ─────────────────────────────────────────────────────────────────

class LLMClassifierTest : public ::testing::Test {
protected:
    static constexpr const char* kFastModel  = "fast:8b";
    static constexpr const char* kSmartModel = "smart:120b";
    static constexpr float kThreshold = 0.7f;

    void SetUp() override {
        ollama_ = std::make_shared<NiceMock<MockOllamaClient>>();

        // Safe default: both calls return empty-ish JSON
        ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.0f)))
            .WillByDefault(Return(makeCmdJson({})));
        ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.7f)))
            .WillByDefault(Return(makeNonHaJson("", 1.0f)));

        cls_ = std::make_unique<LLMClassifier>(
            ollama_, kFastModel, kSmartModel, kThreshold);
    }

    std::shared_ptr<NiceMock<MockOllamaClient>> ollama_;
    std::unique_ptr<LLMClassifier>               cls_;
};


// ─── Tests ───────────────────────────────────────────────────────────────────

// 1. split() calls chatJson exactly twice (command + non_ha in parallel).
TEST_F(LLMClassifierTest, Split_MakesTwoParallelChatJsonCalls) {
    EXPECT_CALL(*ollama_, chatJson(_, _, _, _)).Times(2);

    VoiceCommand cmd;
    cmd.text = "turn on sala 1";
    cls_->split(cmd);
}

// 2. The command extraction call uses temperature 0.0 (deterministic).
TEST_F(LLMClassifierTest, Split_CommandCallUsesTemp0) {
    std::atomic<bool> seenTemp0{false};

    ON_CALL(*ollama_, chatJson(_, _, _, _))
        .WillByDefault(Invoke([&seenTemp0](
                const std::string&, const std::string&,
                const std::string&, float temp) -> Json::Value {
            if (temp == 0.0f) seenTemp0.store(true);
            return (temp == 0.0f) ? makeCmdJson({}) : makeNonHaJson("", 1.0f);
        }));

    VoiceCommand cmd;
    cmd.text = "turn on sala 1";
    cls_->split(cmd);

    EXPECT_TRUE(seenTemp0.load());
}

// 3. The non-HA generation call uses temperature 0.7 (creative).
TEST_F(LLMClassifierTest, Split_NonHaCallUsesTempPoint7) {
    std::atomic<bool> seenTemp7{false};

    ON_CALL(*ollama_, chatJson(_, _, _, _))
        .WillByDefault(Invoke([&seenTemp7](
                const std::string&, const std::string&,
                const std::string&, float temp) -> Json::Value {
            if (temp == 0.7f) seenTemp7.store(true);
            return (temp == 0.0f) ? makeCmdJson({}) : makeNonHaJson("", 1.0f);
        }));

    VoiceCommand cmd;
    cmd.text = "tell me a joke";
    cls_->split(cmd);

    EXPECT_TRUE(seenTemp7.load());
}

// 4. Sub-command text is the exact wording returned by the command call — not paraphrased.
TEST_F(LLMClassifierTest, Split_PreservesExactEntityName) {
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.0f)))
        .WillByDefault(Return(makeCmdJson({"turn on sala 1"})));

    VoiceCommand cmd;
    cmd.text = "turn on sala 1";
    auto result = cls_->split(cmd);

    ASSERT_EQ(result.sub_commands.size(), 1u);
    EXPECT_EQ(result.sub_commands[0].text, "turn on sala 1");
}

// 5. sub_commands come from the command call response.
TEST_F(LLMClassifierTest, Split_ReturnsSubCommandsFromCommandCall) {
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.0f)))
        .WillByDefault(Return(makeCmdJson({"turn off sala 1", "turn on coffee"})));

    VoiceCommand cmd;
    cmd.text = "turn off sala 1 and turn on coffee";
    auto result = cls_->split(cmd);

    ASSERT_EQ(result.sub_commands.size(), 2u);
    EXPECT_EQ(result.sub_commands[0].text, "turn off sala 1");
    EXPECT_EQ(result.sub_commands[1].text, "turn on coffee");
}

// 6. non_ha comes from the non-HA call response.
TEST_F(LLMClassifierTest, Split_ReturnsNonHaFromNonHaCall) {
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.7f)))
        .WillByDefault(Return(makeNonHaJson("Why did the light bulb fail school?", 0.95f)));

    VoiceCommand cmd;
    cmd.text = "tell me a joke about lights";
    auto result = cls_->split(cmd);

    EXPECT_EQ(result.non_ha, "Why did the light bulb fail school?");
}

// 7. If command call throws, sub_commands is empty but non_ha is still returned.
TEST_F(LLMClassifierTest, Split_CommandCallFails_ReturnsEmptySubCommands_NonHaStillReturned) {
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.0f)))
        .WillByDefault(Invoke([](const std::string&, const std::string&,
                                 const std::string&, float) -> Json::Value {
            throw std::runtime_error("command model down");
        }));
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.7f)))
        .WillByDefault(Return(makeNonHaJson("A great joke!", 0.95f)));

    VoiceCommand cmd;
    cmd.text = "turn on sala 1 and tell me a joke";
    auto result = cls_->split(cmd);

    EXPECT_TRUE(result.sub_commands.empty());
    EXPECT_EQ(result.non_ha, "A great joke!");
}

// 8. If non_ha call throws, sub_commands are still returned.
TEST_F(LLMClassifierTest, Split_NonHaCallFails_SubCommandsStillReturned) {
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.0f)))
        .WillByDefault(Return(makeCmdJson({"turn on sala 1"})));
    ON_CALL(*ollama_, chatJson(_, _, _, FloatEq(0.7f)))
        .WillByDefault(Invoke([](const std::string&, const std::string&,
                                 const std::string&, float) -> Json::Value {
            throw std::runtime_error("non_ha model down");
        }));

    VoiceCommand cmd;
    cmd.text = "turn on sala 1";
    auto result = cls_->split(cmd);

    ASSERT_EQ(result.sub_commands.size(), 1u);
    EXPECT_EQ(result.sub_commands[0].text, "turn on sala 1");
}

// 9. When non_ha confidence < escalationThreshold, smartModel is called with kNonHaPrompt.
TEST_F(LLMClassifierTest, Split_EscalatesWhenConfidenceLow) {
    EXPECT_CALL(*ollama_, chatJson(_, kFastModel, _, FloatEq(0.0f)))
        .WillOnce(Return(makeCmdJson({})));
    EXPECT_CALL(*ollama_, chatJson(_, kFastModel, _, FloatEq(0.7f)))
        .WillOnce(Return(makeNonHaJson("Not sure...", 0.2f)));  // < 0.7 threshold

    // Smart model must be called exactly once
    EXPECT_CALL(*ollama_, chatJson(_, kSmartModel, _, _))
        .WillOnce(Return(makeNonHaJson("Better answer!", 0.95f)));

    VoiceCommand cmd;
    cmd.text = "turn on sala 1 and tell me something hard";
    auto result = cls_->split(cmd);

    EXPECT_EQ(result.non_ha, "Better answer!");
}

// 10. When non_ha confidence >= escalationThreshold, smartModel is NOT called.
TEST_F(LLMClassifierTest, Split_NoEscalateWhenConfidenceHigh) {
    EXPECT_CALL(*ollama_, chatJson(_, kFastModel, _, FloatEq(0.0f)))
        .WillOnce(Return(makeCmdJson({})));
    EXPECT_CALL(*ollama_, chatJson(_, kFastModel, _, FloatEq(0.7f)))
        .WillOnce(Return(makeNonHaJson("Great joke!", 0.95f)));  // confidence >= threshold
    EXPECT_CALL(*ollama_, chatJson(_, kSmartModel, _, _)).Times(0);  // smart model never called

    VoiceCommand cmd;
    cmd.text = "tell me a joke and turn on sala 1";
    cls_->split(cmd);
}
