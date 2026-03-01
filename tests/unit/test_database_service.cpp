/**
 * Unit tests for DatabaseService.
 *
 * Tests:
 *   - Initial state: not connected
 *   - All operations guard on connected_ flag and return safe defaults
 *     when no real DB is present — no network required
 *   - disconnect() is idempotent (no crash when called on a never-connected instance)
 *   - Regression: disconnect() uses conn_.reset() not conn_->close() (pqxx 7.7+)
 *
 * No real PostgreSQL connection required for any of these tests.
 */

#include "services/DatabaseService.h"
#include "intent/IntentClassifier.h"
#include <gtest/gtest.h>


// ─── Initial state ────────────────────────────────────────────────────────────

TEST(DatabaseServiceTest, InitialState_NotConnected) {
    DatabaseService db("");
    EXPECT_FALSE(db.isConnected());
}

TEST(DatabaseServiceTest, InitialState_ConnectWithEmptyString_ReturnsFalse) {
    DatabaseService db("");
    // Empty connection string → pqxx throws, connect() returns false
    EXPECT_FALSE(db.connect());
    EXPECT_FALSE(db.isConnected());
}


// ─── Guarded operations when not connected ────────────────────────────────────

TEST(DatabaseServiceTest, LogVoiceCommand_WhenNotConnected_ReturnsMinusOne) {
    DatabaseService db("");
    VoiceCommand cmd;
    cmd.text       = "turn on the light";
    cmd.device_id  = "test_device";
    cmd.confidence = 1.0f;
    EXPECT_EQ(db.logVoiceCommand(cmd), -1);
}

TEST(DatabaseServiceTest, LogIntentResult_WhenNotConnected_ReturnsFalse) {
    DatabaseService db("");
    IntentResult result;
    result.intent     = "light_control";
    result.tier       = "tier1";
    result.confidence = 0.95f;
    result.success    = true;
    EXPECT_FALSE(db.logIntentResult(1, result));
}

TEST(DatabaseServiceTest, GetTotalCommands_WhenNotConnected_ReturnsZero) {
    DatabaseService db("");
    EXPECT_EQ(db.getTotalCommands(), 0);
}

TEST(DatabaseServiceTest, GetSuccessfulIntents_WhenNotConnected_ReturnsZero) {
    DatabaseService db("");
    EXPECT_EQ(db.getSuccessfulIntents(), 0);
}

TEST(DatabaseServiceTest, GetIntentDistribution_WhenNotConnected_ReturnsEmptyMap) {
    DatabaseService db("");
    auto dist = db.getIntentDistribution();
    EXPECT_TRUE(dist.empty());
}


// ─── disconnect() safety ──────────────────────────────────────────────────────

TEST(DatabaseServiceTest, Disconnect_WhenNeverConnected_IsIdempotent) {
    // Regression: used to call conn_->close() which is protected in pqxx 7.7+
    // Now uses conn_.reset() — must not throw or crash when conn_ is nullptr
    DatabaseService db("");
    EXPECT_NO_THROW(db.disconnect());
    EXPECT_NO_THROW(db.disconnect());  // second call also safe
}

TEST(DatabaseServiceTest, Disconnect_WhenNotConnected_LeavesNotConnected) {
    DatabaseService db("");
    db.disconnect();
    EXPECT_FALSE(db.isConnected());
}
