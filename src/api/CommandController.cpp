#include "api/CommandController.h"
#include <iostream>
#include <chrono>

CommandController::CommandController(std::shared_ptr<DeterministicClassifier> tier1,
                                     std::shared_ptr<EmbeddingClassifier> tier2,
                                     std::shared_ptr<LLMClassifier> tier3,
                                     std::shared_ptr<DatabaseService> db,
                                     std::shared_ptr<EntityIngestService> ingest)
    : tier1_(tier1), tier2_(tier2), tier3_(tier3), db_(db), ingest_(ingest) {}

void CommandController::handleCommand(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto bodyJson = req->getJsonObject();
    if (!bodyJson) {
        cb(errorResponse(400, "Request body must be valid JSON"));
        return;
    }

    if (!bodyJson->isMember("text") || !bodyJson->isMember("device_id")) {
        cb(errorResponse(400, "Required fields: text, device_id"));
        return;
    }

    VoiceCommand command;
    command.text      = (*bodyJson)["text"].asString();
    command.device_id = (*bodyJson)["device_id"].asString();
    command.confidence = (*bodyJson).get("confidence", 1.0f).asFloat();
    command.context   = (*bodyJson).get("context", Json::Value()).toStyledString();

    if (command.text.empty()) {
        cb(errorResponse(400, "text cannot be empty"));
        return;
    }

    // Log the incoming command
    int commandId = -1;
    try {
        commandId = db_->logVoiceCommand(command);
    } catch (const std::exception& e) {
        std::cerr << "[CommandController] DB log failed: " << e.what() << std::endl;
    }

    // Run the classification pipeline
    IntentResult result = runPipeline(command);

    // Log the result
    if (commandId >= 0) {
        try {
            db_->logIntentResult(commandId, result);
        } catch (const std::exception& e) {
            std::cerr << "[CommandController] DB result log failed: " << e.what() << std::endl;
        }
    }

    Json::Value response = buildResponse(result);
    auto httpResp = drogon::HttpResponse::newHttpJsonResponse(response);
    httpResp->setStatusCode(result.success ? drogon::k200OK : drogon::k422UnprocessableEntity);
    cb(httpResp);
}

void CommandController::handleReindex(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::cout << "[CommandController] Manual re-index triggered" << std::endl;

    int count = 0;
    try {
        count = ingest_->ingest();
    } catch (const std::exception& e) {
        cb(errorResponse(500, std::string("Reindex failed: ") + e.what()));
        return;
    }

    Json::Value resp;
    resp["success"]            = true;
    resp["entities_indexed"]   = count;
    cb(drogon::HttpResponse::newHttpJsonResponse(resp));
}

IntentResult CommandController::runPipeline(const VoiceCommand& command) {
    // --- Tier 1: Deterministic (regex) ---
    IntentResult result = tier1_->classify(command);
    if (result.success) {
        std::cout << "[Pipeline] Tier1 hit: " << result.intent
                  << " (" << result.processing_time_ms << "ms)" << std::endl;
        return result;
    }

    // --- Tier 2: Vector search (semantic) ---
    result = tier2_->classify(command);
    if (result.success) {
        std::cout << "[Pipeline] Tier2 hit: " << result.intent
                  << " similarity=" << result.confidence
                  << " (" << result.processing_time_ms << "ms)" << std::endl;
        return result;
    }

    // --- Tier 3: LLM (8b → 120b) ---
    result = tier3_->classify(command);
    std::cout << "[Pipeline] Tier3 " << result.tier << ": "
              << (result.success ? "ok" : "fail")
              << " (" << result.processing_time_ms << "ms)" << std::endl;
    return result;
}

drogon::HttpResponsePtr CommandController::errorResponse(int status, const std::string& message) {
    Json::Value body;
    body["success"] = false;
    body["error"]   = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return resp;
}

Json::Value CommandController::buildResponse(const IntentResult& result) {
    Json::Value r;
    r["success"]            = result.success;
    r["tier"]               = result.tier;
    r["intent"]             = result.intent;
    r["confidence"]         = result.confidence;
    r["response_text"]      = result.response_text;
    r["processing_time_ms"] = result.processing_time_ms;
    r["entities"]           = result.entities;
    return r;
}
