#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager inst;
        return inst;
    }

    void load(const std::string& path);

    struct HAConfig {
        std::string url;
        std::string token;
    };

    struct DBConfig {
        std::string host;
        int port{5432};
        std::string name;
        std::string user;
        std::string password;
        std::string ha_db_name;  // HA recorder DB for direct entity queries
    };

    struct OllamaConfig {
        std::string url;
        std::string embed_model;
        std::string fast_model;
        std::string smart_model;
        float escalation_threshold{0.7f};
    };

    struct WyomingConfig {
        std::string piper_host;
        int piper_port{10200};
        std::string whisper_host;
        int whisper_port{10300};
    };

    struct ServiceConfig {
        int port{8894};
        float vector_similarity_threshold{0.82f};
        int vector_search_limit{5};
    };

    HAConfig ha;
    DBConfig db;
    OllamaConfig ollama;
    WyomingConfig wyoming;
    ServiceConfig service;

    std::string dbConnectionString() const;
    std::string haDbConnectionString() const;  // Connection string for HA recorder DB

private:
    ConfigManager() = default;
};

#endif // CONFIG_MANAGER_H
