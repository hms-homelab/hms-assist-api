#include "utils/ConfigManager.h"
#include "clients/HomeAssistantClient.h"
#include "clients/OllamaClient.h"
#include "services/DatabaseService.h"
#include "services/VectorSearchService.h"
#include "services/EntityIngestService.h"
#include "intent/DeterministicClassifier.h"
#include "intent/EmbeddingClassifier.h"
#include "intent/LLMClassifier.h"
#include "api/CommandController.h"
#include <drogon/drogon.h>
#include <iostream>
#include <csignal>
#include <memory>
#include <cstdlib>

static std::shared_ptr<EntityIngestService> g_ingest;

void signalHandler(int signal) {
    std::cout << "\n[Main] Signal " << signal << " received, shutting down..." << std::endl;
    if (g_ingest) g_ingest->stop();
    drogon::app().quit();
}

int main(int argc, char* argv[]) {
    std::cout << "==========================================" << std::endl;
    std::cout << " HMS-Assist Voice Assistant API v2.0"       << std::endl;
    std::cout << "==========================================" << std::endl;

    // Config path: env var > default
    const char* configEnv = std::getenv("HMS_ASSIST_CONFIG");
    std::string configPath = configEnv ? configEnv : "/etc/hms-assist/config.yaml";

    try {
        ConfigManager::instance().load(configPath);
    } catch (const std::exception& e) {
        std::cerr << "[Main] FATAL: " << e.what() << std::endl;
        return 1;
    }

    auto& cfg = ConfigManager::instance();

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // --- Clients ---
    auto haClient = std::make_shared<HomeAssistantClient>(cfg.ha.url, cfg.ha.token);
    auto ollamaClient = std::make_shared<OllamaClient>(cfg.ollama.url);

    // --- Database ---
    auto dbService = std::make_shared<DatabaseService>(cfg.dbConnectionString());
    if (!dbService->connect()) {
        std::cerr << "[Main] FATAL: Cannot connect to database" << std::endl;
        return 1;
    }

    // --- Vector search ---
    auto vectorSearch = std::make_shared<VectorSearchService>(cfg.dbConnectionString());

    // --- Entity ingest (startup sync + hourly background) ---
    g_ingest = std::make_shared<EntityIngestService>(
        haClient, ollamaClient, vectorSearch,
        cfg.ollama.embed_model,
        cfg.haDbConnectionString()  // Direct HA DB query, falls back to REST API
    );
    g_ingest->startBackgroundSync();

    // --- Classifiers ---
    auto tier1 = std::make_shared<DeterministicClassifier>(haClient);

    auto tier2 = std::make_shared<EmbeddingClassifier>(
        ollamaClient, haClient, vectorSearch,
        cfg.ollama.embed_model,
        cfg.service.vector_similarity_threshold,
        cfg.service.vector_search_limit
    );

    auto tier3 = std::make_shared<LLMClassifier>(
        ollamaClient, haClient,
        cfg.ollama.fast_model,
        cfg.ollama.smart_model,
        cfg.ollama.escalation_threshold
    );

    // --- Controller ---
    auto controller = std::make_shared<CommandController>(
        tier1, tier2, tier3, dbService, g_ingest
    );

    // --- HTTP routes ---
    drogon::app().registerHandler(
        "/api/v1/command",
        [controller](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            controller->handleCommand(req, std::move(cb));
        },
        {drogon::Post}
    );

    drogon::app().registerHandler(
        "/admin/reindex",
        [controller](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            controller->handleReindex(req, std::move(cb));
        },
        {drogon::Post}
    );

    drogon::app().registerHandler(
        "/health",
        [&dbService, &vectorSearch](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {

            Json::Value response;
            response["status"]  = "healthy";
            response["service"] = "hms-assist";
            response["version"] = "2.0.0";

            Json::Value components;
            components["database"]      = dbService->isConnected() ? "connected" : "disconnected";
            components["vector_db"]     = "connected";
            components["entity_count"]  = vectorSearch->entityCount();
            response["components"] = components;

            Json::Value stats;
            stats["total_commands"]      = dbService->getTotalCommands();
            stats["successful_intents"]  = dbService->getSuccessfulIntents();
            response["statistics"] = stats;

            cb(drogon::HttpResponse::newHttpJsonResponse(response));
        },
        {drogon::Get}
    );

    std::cout << "[Main] Starting HTTP server on port " << cfg.service.port << std::endl;

    drogon::app()
        .setLogPath("./")
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("0.0.0.0", cfg.service.port)
        .setThreadNum(4)
        .run();

    std::cout << "[Main] HMS-Assist stopped." << std::endl;
    return 0;
}
