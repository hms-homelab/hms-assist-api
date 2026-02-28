/**
 * Unit tests for ConfigManager — YAML parsing + connection string builders.
 * No network or DB connections required.
 */

#include "utils/ConfigManager.h"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string writeTempConfig(const std::string& yaml) {
    std::string path = "/tmp/hms_assist_test_config.yaml";
    std::ofstream f(path);
    f << yaml;
    return path;
}

static const std::string VALID_YAML = R"(
homeassistant:
  url: http://192.168.2.7:8123
  token: test_token_abc123

database:
  host: 192.168.2.15
  port: 5432
  name: hms_assist
  user: maestro
  password: secret_pw
  ha_db_name: homeassistant

ollama:
  url: http://192.168.2.5:11434
  embed_model: nomic-embed-text
  fast_model: llama3.1:8b-instruct-q4_K_M
  smart_model: gpt-oss:120b-cloud
  escalation_threshold: 0.7

wyoming:
  piper_host: 192.168.2.5
  piper_port: 10200
  whisper_host: 192.168.2.5
  whisper_port: 10300

service:
  port: 8894
  vector_similarity_threshold: 0.82
  vector_search_limit: 5
)";


// ─── Tests ────────────────────────────────────────────────────────────────────

class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = writeTempConfig(VALID_YAML);
        ConfigManager::instance().load(path_);
    }
    void TearDown() override {
        std::remove(path_.c_str());
    }
    std::string path_;
};

// HA fields
TEST_F(ConfigManagerTest, ParsesHaUrl) {
    EXPECT_EQ(ConfigManager::instance().ha.url, "http://192.168.2.7:8123");
}
TEST_F(ConfigManagerTest, ParsesHaToken) {
    EXPECT_EQ(ConfigManager::instance().ha.token, "test_token_abc123");
}

// DB fields
TEST_F(ConfigManagerTest, ParsesDbHost) {
    EXPECT_EQ(ConfigManager::instance().db.host, "192.168.2.15");
}
TEST_F(ConfigManagerTest, ParsesDbPort) {
    EXPECT_EQ(ConfigManager::instance().db.port, 5432);
}
TEST_F(ConfigManagerTest, ParsesDbName) {
    EXPECT_EQ(ConfigManager::instance().db.name, "hms_assist");
}
TEST_F(ConfigManagerTest, ParsesDbUser) {
    EXPECT_EQ(ConfigManager::instance().db.user, "maestro");
}
TEST_F(ConfigManagerTest, ParsesDbPassword) {
    EXPECT_EQ(ConfigManager::instance().db.password, "secret_pw");
}
TEST_F(ConfigManagerTest, ParsesHaDbName) {
    EXPECT_EQ(ConfigManager::instance().db.ha_db_name, "homeassistant");
}

// Ollama fields
TEST_F(ConfigManagerTest, ParsesOllamaUrl) {
    EXPECT_EQ(ConfigManager::instance().ollama.url, "http://192.168.2.5:11434");
}
TEST_F(ConfigManagerTest, ParsesEmbedModel) {
    EXPECT_EQ(ConfigManager::instance().ollama.embed_model, "nomic-embed-text");
}
TEST_F(ConfigManagerTest, ParsesFastModel) {
    EXPECT_EQ(ConfigManager::instance().ollama.fast_model, "llama3.1:8b-instruct-q4_K_M");
}
TEST_F(ConfigManagerTest, ParsesSmartModel) {
    EXPECT_EQ(ConfigManager::instance().ollama.smart_model, "gpt-oss:120b-cloud");
}
TEST_F(ConfigManagerTest, ParsesEscalationThreshold) {
    EXPECT_FLOAT_EQ(ConfigManager::instance().ollama.escalation_threshold, 0.7f);
}

// Wyoming fields
TEST_F(ConfigManagerTest, ParsesWyomingPiperHost) {
    EXPECT_EQ(ConfigManager::instance().wyoming.piper_host, "192.168.2.5");
}
TEST_F(ConfigManagerTest, ParsesWyomingPiperPort) {
    EXPECT_EQ(ConfigManager::instance().wyoming.piper_port, 10200);
}
TEST_F(ConfigManagerTest, ParsesWyomingWhisperPort) {
    EXPECT_EQ(ConfigManager::instance().wyoming.whisper_port, 10300);
}

// Service fields
TEST_F(ConfigManagerTest, ParsesServicePort) {
    EXPECT_EQ(ConfigManager::instance().service.port, 8894);
}
TEST_F(ConfigManagerTest, ParsesVectorSimilarityThreshold) {
    EXPECT_FLOAT_EQ(ConfigManager::instance().service.vector_similarity_threshold, 0.82f);
}
TEST_F(ConfigManagerTest, ParsesVectorSearchLimit) {
    EXPECT_EQ(ConfigManager::instance().service.vector_search_limit, 5);
}

// Connection string builders
TEST_F(ConfigManagerTest, DbConnectionStringContainsAllParts) {
    std::string cs = ConfigManager::instance().dbConnectionString();
    EXPECT_NE(cs.find("host=192.168.2.15"), std::string::npos);
    EXPECT_NE(cs.find("port=5432"), std::string::npos);
    EXPECT_NE(cs.find("dbname=hms_assist"), std::string::npos);
    EXPECT_NE(cs.find("user=maestro"), std::string::npos);
    EXPECT_NE(cs.find("password=secret_pw"), std::string::npos);
}
TEST_F(ConfigManagerTest, HaDbConnectionStringUsesHaDbName) {
    std::string cs = ConfigManager::instance().haDbConnectionString();
    EXPECT_NE(cs.find("dbname=homeassistant"), std::string::npos);
    EXPECT_NE(cs.find("user=maestro"), std::string::npos);
}
TEST_F(ConfigManagerTest, HaDbConnectionStringEmptyWhenNotConfigured) {
    std::string minimal = R"(
homeassistant:
  url: http://ha:8123
  token: tok
database:
  host: db
  port: 5432
  name: mydb
  user: u
  password: p
ollama:
  url: http://ol:11434
  fast_model: fast
  smart_model: smart
wyoming:
  piper_host: w
  whisper_host: w
service:
  port: 8894
)";
    // Use a distinct temp path so we don't clobber path_
    std::string p = "/tmp/hms_assist_test_minimal.yaml";
    { std::ofstream f(p); f << minimal; }
    ConfigManager::instance().load(p);
    EXPECT_TRUE(ConfigManager::instance().haDbConnectionString().empty());
    std::remove(p.c_str());
    // Restore to known-good state for other tests
    ConfigManager::instance().load(path_);
}

// Error cases
TEST(ConfigManagerErrorTest, ThrowsOnMissingFile) {
    EXPECT_THROW(
        ConfigManager::instance().load("/nonexistent/path/config.yaml"),
        std::runtime_error
    );
}
