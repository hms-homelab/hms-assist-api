#include "utils/ConfigManager.h"
#include "clients/MqttClient.h"
#include "clients/HomeAssistantClient.h"
#include "services/DatabaseService.h"
#include "services/VoiceService.h"
#include <drogon/drogon.h>
#include <iostream>
#include <csignal>
#include <memory>

static std::shared_ptr<VoiceService> voiceService;

void signalHandler(int signal) {
    std::cout << "\n[Main] Received signal " << signal << ", shutting down..." << std::endl;

    if (voiceService) {
        voiceService->stop();
    }

    drogon::app().quit();
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "HMS-Assist Voice Assistant Service" << std::endl;
    std::cout << "==========================================" << std::endl;

    // Register signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Load configuration
    std::string mqttBroker = ConfigManager::getMqttBroker();
    int mqttPort = ConfigManager::getMqttPort();
    std::string mqttUser = ConfigManager::getMqttUser();
    std::string mqttPassword = ConfigManager::getMqttPassword();

    std::string haUrl = ConfigManager::getHaUrl();
    std::string haToken = ConfigManager::getHaToken();

    std::string dbHost = ConfigManager::getDbHost();
    int dbPort = ConfigManager::getDbPort();
    std::string dbName = ConfigManager::getDbName();
    std::string dbUser = ConfigManager::getDbUser();
    std::string dbPassword = ConfigManager::getDbPassword();

    int healthCheckPort = ConfigManager::getHealthCheckPort();

    std::cout << "[Main] Configuration loaded:" << std::endl;
    std::cout << "  MQTT Broker: " << mqttBroker << ":" << mqttPort << std::endl;
    std::cout << "  Home Assistant: " << haUrl << std::endl;
    std::cout << "  Database: " << dbHost << ":" << dbPort << "/" << dbName << std::endl;
    std::cout << "  Health Check Port: " << healthCheckPort << std::endl;

    // Initialize Home Assistant client
    std::cout << "\n[Main] Initializing Home Assistant client..." << std::endl;
    auto haClient = std::make_shared<HomeAssistantClient>(haUrl, haToken);

    // Initialize database service
    std::cout << "[Main] Connecting to PostgreSQL database..." << std::endl;
    std::string dbConnectionString = "host=" + dbHost +
                                    " port=" + std::to_string(dbPort) +
                                    " dbname=" + dbName +
                                    " user=" + dbUser +
                                    " password=" + dbPassword;

    auto dbService = std::make_shared<DatabaseService>(dbConnectionString);

    if (!dbService->connect()) {
        std::cerr << "[Main] Failed to connect to database, exiting" << std::endl;
        return 1;
    }

    // Initialize MQTT client
    std::cout << "[Main] Connecting to MQTT broker..." << std::endl;
    auto mqttClient = std::make_shared<MqttClient>(
        mqttBroker, mqttPort, "hms_assist_service", mqttUser, mqttPassword
    );

    if (!mqttClient->connect()) {
        std::cerr << "[Main] Failed to connect to MQTT broker, exiting" << std::endl;
        return 1;
    }

    // Initialize voice service
    std::cout << "[Main] Initializing voice service..." << std::endl;
    voiceService = std::make_shared<VoiceService>(mqttClient, haClient, dbService);
    voiceService->start();

    // Set up Drogon HTTP server for health check
    std::cout << "[Main] Starting health check HTTP server on port " << healthCheckPort << "..." << std::endl;

    drogon::app().registerHandler(
        "/health",
        [&dbService, &mqttClient](const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value response;
            response["status"] = "healthy";
            response["service"] = "hms-assist";

            Json::Value components;
            components["mqtt"] = mqttClient->isConnected() ? "connected" : "disconnected";
            components["database"] = dbService->isConnected() ? "connected" : "disconnected";

            response["components"] = components;

            // Statistics
            Json::Value stats;
            stats["total_commands"] = dbService->getTotalCommands();
            stats["successful_intents"] = dbService->getSuccessfulIntents();

            response["statistics"] = stats;

            auto httpResponse = drogon::HttpResponse::newHttpJsonResponse(response);
            callback(httpResponse);
        },
        {drogon::Get}
    );

    drogon::app()
        .setLogPath("./")
        .setLogLevel(trantor::Logger::kInfo)
        .addListener("0.0.0.0", healthCheckPort)
        .setThreadNum(1)
        .run();

    std::cout << "\n[Main] HMS-Assist service stopped" << std::endl;
    return 0;
}
