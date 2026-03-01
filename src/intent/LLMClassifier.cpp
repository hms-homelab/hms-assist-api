#include "intent/LLMClassifier.h"
#include <chrono>
#include <future>
#include <iostream>

// Temperature 0 — deterministic. Extracts HA sub-commands only.
// Key fix: "Use EXACT original wording" prevents paraphrasing of entity names.
// No escalate field, no non_ha generation.
static const std::string kCommandPrompt = R"(Parse the voice command and extract ONLY smart home device commands.

Output ONLY a JSON object. No prose.

Fields:
- "sub_commands": array of HA device/automation commands (may be empty)
- "has_non_ha": true if the command contains non-HA content (jokes, questions, facts, weather)

Each sub-command:
- "text": Use EXACT original wording from the voice input — do NOT translate, paraphrase, or reword
- "wait_for_previous": true if this must execute after all currently running commands finish

wait_for_previous rules:
- Same device, second step: first cmd is false, second is true
- Explicit order ("first ... then ..."): second cmd is true
- Independent devices: always false
- First sub-command always has wait_for_previous:false

Rules:
- sub_commands: ONLY HA commands (turn on/off, lock/unlock, open/close, set temperature, play/pause)
- "tell me", "what is", "who", "why", "how", "joke", "story", "weather" → NEVER in sub_commands → has_non_ha:true

Examples:
"turn off coffee and turn on sala 1" → {"sub_commands":[{"text":"turn off coffee","wait_for_previous":false},{"text":"turn on sala 1","wait_for_previous":false}],"has_non_ha":false}
"turn on sala 1 and tell me a joke" → {"sub_commands":[{"text":"turn on sala 1","wait_for_previous":false}],"has_non_ha":true}
"tell me a joke about dogs" → {"sub_commands":[],"has_non_ha":true}
"turn off coffee and then turn it back on" → {"sub_commands":[{"text":"turn off coffee","wait_for_previous":false},{"text":"turn on coffee","wait_for_previous":true}],"has_non_ha":false}
"what is the outdoor temperature" → {"sub_commands":[],"has_non_ha":true})";

// Temperature 0.7 — creative. Generates non-HA answer only.
// No escalate field — escalation is confidence-only.
static const std::string kNonHaPrompt = R"(Answer the non-smart-home part of this voice command (jokes, facts, questions, stories).

Output ONLY a JSON object. No prose.

Fields:
- "non_ha": your complete answer. Empty string if the command has no non-HA content.
- "confidence": 0.9-1.0 when certain of your answer; 0.5-0.8 when uncertain

Rules:
- Only answer jokes, questions, facts — NOT device commands
- If you need live data (current weather, news, current time): respond naturally, e.g. "I don't have live weather data right now, but check your weather app!"
- Do NOT include an "escalate" field

Examples:
"tell me a joke about dogs" → {"non_ha":"Why do dogs make terrible DJs? Because they always paws the music.","confidence":0.95}
"turn off sala 1 and tell me a joke" → {"non_ha":"Why can't sala lights ever be on time? They're always a little dim!","confidence":0.9}
"what is the capital of France" → {"non_ha":"The capital of France is Paris.","confidence":0.99}
"tell me how the weather is" → {"non_ha":"I don't have live weather data right now, but check your weather app!","confidence":0.8}
"turn off sala 1 and turn on coffee" → {"non_ha":"","confidence":1.0})";

LLMClassifier::LLMClassifier(std::shared_ptr<OllamaClient> ollama,
                               const std::string& fastModel,
                               const std::string& smartModel,
                               float escalationThreshold)
    : ollama_(ollama), fastModel_(fastModel),
      smartModel_(smartModel), escalationThreshold_(escalationThreshold) {}

SplitResult LLMClassifier::split(const VoiceCommand& command) {
    SplitResult result;

    std::string userMsg = "Voice command: \"" + command.text + "\"";

    auto t0 = std::chrono::steady_clock::now();

    // Two parallel async calls: command extraction (temp 0) + non_ha generation (temp 0.7)
    auto cmdFuture = std::async(std::launch::async, [this, userMsg]() {
        return ollama_->chatJson(userMsg, fastModel_, kCommandPrompt, 0.0f);
    });
    auto nonHaFuture = std::async(std::launch::async, [this, userMsg]() {
        return ollama_->chatJson(userMsg, fastModel_, kNonHaPrompt, 0.7f);
    });

    // Collect command call result
    Json::Value cmdJson;
    try {
        cmdJson = cmdFuture.get();
    } catch (const std::exception& e) {
        std::cerr << "[LLMClassifier] command call failed: " << e.what() << std::endl;
        cmdJson = Json::Value(Json::objectValue);
    }

    // Collect non_ha call result
    Json::Value nonHaJson;
    try {
        nonHaJson = nonHaFuture.get();
    } catch (const std::exception& e) {
        std::cerr << "[LLMClassifier] non_ha call failed: " << e.what() << std::endl;
        nonHaJson = Json::Value(Json::objectValue);
    }

    // Parse sub_commands from command call
    const Json::Value& arr = cmdJson["sub_commands"];
    if (arr.isArray()) {
        for (const auto& v : arr) {
            if (v.isObject()) {
                SubCommand sc;
                sc.text              = v.get("text", "").asString();
                sc.wait_for_previous = v.get("wait_for_previous", false).asBool();
                if (!sc.text.empty()) result.sub_commands.push_back(sc);
            }
        }
    }

    // Parse non_ha from non_ha call
    result.non_ha     = nonHaJson.get("non_ha", "").asString();
    result.confidence = nonHaJson.get("confidence", 1.0f).asFloat();

    auto llmMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "[LLMClassifier] parallel split: " << llmMs << "ms"
              << " sub_commands=" << result.sub_commands.size()
              << " non_ha=" << (result.non_ha.empty() ? "no" : "yes")
              << " confidence=" << result.confidence << std::endl;

    // Escalation: confidence-only (no escalate field from model)
    bool needsEscalation = !result.non_ha.empty() && result.confidence < escalationThreshold_;
    if (needsEscalation) {
        std::cout << "[LLMClassifier] escalating non_ha to smart model"
                  << " (confidence=" << result.confidence << ")" << std::endl;
        auto t1 = std::chrono::steady_clock::now();
        try {
            Json::Value smartJson = ollama_->chatJson(userMsg, smartModel_, kNonHaPrompt, 0.7f);
            result.non_ha     = smartJson.get("non_ha", result.non_ha).asString();
            result.confidence = smartJson.get("confidence", result.confidence).asFloat();
        } catch (const std::exception& e) {
            std::cerr << "[LLMClassifier] smart model failed: " << e.what() << std::endl;
        }
        std::cout << "[LLMClassifier] smart model: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t1).count() << "ms" << std::endl;
    }

    return result;
}
