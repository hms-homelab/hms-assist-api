#include "services/DatabaseService.h"
#include <iostream>

DatabaseService::DatabaseService(const std::string& connectionString)
    : connectionString_(connectionString), connected_(false) {
}

DatabaseService::~DatabaseService() {
    disconnect();
}

bool DatabaseService::connect() {
    try {
        conn_ = std::make_unique<pqxx::connection>(connectionString_);

        if (conn_->is_open()) {
            std::cout << "[Database] Connected to PostgreSQL: " << conn_->dbname() << std::endl;
            connected_ = true;
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Database] Connection failed: " << e.what() << std::endl;
        connected_ = false;
    }

    return false;
}

void DatabaseService::disconnect() {
    if (conn_ && conn_->is_open()) {
        conn_->close();
        connected_ = false;
        std::cout << "[Database] Disconnected" << std::endl;
    }
}

int DatabaseService::logVoiceCommand(const VoiceCommand& command) {
    if (!connected_) {
        std::cerr << "[Database] Not connected" << std::endl;
        return -1;
    }

    try {
        pqxx::work txn(*conn_);

        std::string query = "INSERT INTO voice_commands (device_id, text, confidence) "
                           "VALUES (" + txn.quote(command.device_id) + ", " +
                           txn.quote(command.text) + ", " +
                           std::to_string(command.confidence) + ") RETURNING id";

        pqxx::result result = txn.exec(query);
        txn.commit();

        if (!result.empty()) {
            int command_id = result[0][0].as<int>();
            std::cout << "[Database] Logged voice command (ID: " << command_id << ")" << std::endl;
            return command_id;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to log voice command: " << e.what() << std::endl;
    }

    return -1;
}

bool DatabaseService::logIntentResult(int commandId, const IntentResult& result) {
    if (!connected_) {
        std::cerr << "[Database] Not connected" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);

        // Serialize entities as JSONB
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::string entities_json = Json::writeString(writer, result.entities);

        std::string query = "INSERT INTO intent_results "
                           "(command_id, intent, tier, confidence, response_text, processing_time_ms, success, entities) "
                           "VALUES (" +
                           std::to_string(commandId) + ", " +
                           txn.quote(result.intent) + ", " +
                           txn.quote(result.tier) + ", " +
                           std::to_string(result.confidence) + ", " +
                           txn.quote(result.response_text) + ", " +
                           std::to_string(result.processing_time_ms) + ", " +
                           std::string(result.success ? "true" : "false") + ", " +
                           txn.quote(entities_json) + "::jsonb)";

        txn.exec(query);
        txn.commit();

        std::cout << "[Database] Logged intent result for command " << commandId << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to log intent result: " << e.what() << std::endl;
    }

    return false;
}

int DatabaseService::getTotalCommands() {
    if (!connected_) return 0;

    try {
        pqxx::work txn(*conn_);
        pqxx::result result = txn.exec("SELECT COUNT(*) FROM voice_commands");
        return result[0][0].as<int>();
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to get total commands: " << e.what() << std::endl;
    }

    return 0;
}

int DatabaseService::getSuccessfulIntents() {
    if (!connected_) return 0;

    try {
        pqxx::work txn(*conn_);
        pqxx::result result = txn.exec("SELECT COUNT(*) FROM intent_results WHERE success = true");
        return result[0][0].as<int>();
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to get successful intents: " << e.what() << std::endl;
    }

    return 0;
}

std::map<std::string, int> DatabaseService::getIntentDistribution() {
    std::map<std::string, int> distribution;

    if (!connected_) return distribution;

    try {
        pqxx::work txn(*conn_);
        pqxx::result result = txn.exec("SELECT intent, COUNT(*) FROM intent_results GROUP BY intent");

        for (const auto& row : result) {
            std::string intent = row[0].as<std::string>();
            int count = row[1].as<int>();
            distribution[intent] = count;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to get intent distribution: " << e.what() << std::endl;
    }

    return distribution;
}
