#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <cstdlib>

class ConfigManager {
public:
    // MQTT Configuration
    static std::string getMqttBroker() {
        return getEnv("MQTT_BROKER", "192.168.2.15");
    }

    static int getMqttPort() {
        return std::stoi(getEnv("MQTT_PORT", "1883"));
    }

    static std::string getMqttUser() {
        return getEnv("MQTT_USER", "aamat");
    }

    static std::string getMqttPassword() {
        return getEnv("MQTT_PASS", "exploracion");
    }

    // PostgreSQL Configuration
    static std::string getDbHost() {
        return getEnv("DB_HOST", "localhost");
    }

    static int getDbPort() {
        return std::stoi(getEnv("DB_PORT", "5432"));
    }

    static std::string getDbName() {
        return getEnv("DB_NAME", "hms_assist");
    }

    static std::string getDbUser() {
        return getEnv("DB_USER", "maestro");
    }

    static std::string getDbPassword() {
        return getEnv("DB_PASS", "maestro_postgres_2026_secure");
    }

    // Home Assistant Configuration
    static std::string getHaUrl() {
        return getEnv("HA_URL", "http://192.168.2.15:8123");
    }

    static std::string getHaToken() {
        return getEnv("HA_TOKEN", "");
    }

    // Health Check Configuration
    static int getHealthCheckPort() {
        return std::stoi(getEnv("HEALTH_CHECK_PORT", "8894"));
    }

private:
    static std::string getEnv(const char* name, const std::string& defaultValue) {
        const char* value = std::getenv(name);
        return value ? std::string(value) : defaultValue;
    }
};

#endif // CONFIG_MANAGER_H
