#include "utils/ConfigManager.h"
#include <iostream>

void ConfigManager::load(const std::string& path) {
    YAML::Node cfg;
    try {
        cfg = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to load config from " + path + ": " + e.what());
    }

    // Home Assistant
    ha.url   = cfg["homeassistant"]["url"].as<std::string>();
    ha.token = cfg["homeassistant"]["token"].as<std::string>();

    // Database
    db.host       = cfg["database"]["host"].as<std::string>();
    db.port       = cfg["database"]["port"].as<int>(5432);
    db.name       = cfg["database"]["name"].as<std::string>();
    db.user       = cfg["database"]["user"].as<std::string>();
    db.password   = cfg["database"]["password"].as<std::string>();
    db.ha_db_name = cfg["database"]["ha_db_name"].as<std::string>("");

    // Ollama
    ollama.url                  = cfg["ollama"]["url"].as<std::string>();
    ollama.embed_model          = cfg["ollama"]["embed_model"].as<std::string>("nomic-embed-text");
    ollama.fast_model           = cfg["ollama"]["fast_model"].as<std::string>();
    ollama.smart_model          = cfg["ollama"]["smart_model"].as<std::string>();
    ollama.escalation_threshold = cfg["ollama"]["escalation_threshold"].as<float>(0.7f);

    // Wyoming
    wyoming.piper_host   = cfg["wyoming"]["piper_host"].as<std::string>();
    wyoming.piper_port   = cfg["wyoming"]["piper_port"].as<int>(10200);
    wyoming.whisper_host = cfg["wyoming"]["whisper_host"].as<std::string>();
    wyoming.whisper_port = cfg["wyoming"]["whisper_port"].as<int>(10300);

    // Service
    service.port                       = cfg["service"]["port"].as<int>(8894);
    service.vector_similarity_threshold = cfg["service"]["vector_similarity_threshold"].as<float>(0.82f);
    service.vector_search_limit        = cfg["service"]["vector_search_limit"].as<int>(5);
    service.tts_entity                 = cfg["service"]["tts_entity"].as<std::string>("tts.piper");

    std::cout << "[Config] Loaded from " << path << std::endl;
    std::cout << "[Config]   HA:     " << ha.url << std::endl;
    std::cout << "[Config]   DB:     " << db.host << ":" << db.port << "/" << db.name << std::endl;
    std::cout << "[Config]   Ollama: " << ollama.url << " (embed=" << ollama.embed_model << ")" << std::endl;
    std::cout << "[Config]   Port:   " << service.port << std::endl;
}

std::string ConfigManager::dbConnectionString() const {
    return "host=" + db.host +
           " port=" + std::to_string(db.port) +
           " dbname=" + db.name +
           " user=" + db.user +
           " password=" + db.password;
}

std::string ConfigManager::haDbConnectionString() const {
    if (db.ha_db_name.empty()) return "";
    return "host=" + db.host +
           " port=" + std::to_string(db.port) +
           " dbname=" + db.ha_db_name +
           " user=" + db.user +
           " password=" + db.password;
}
