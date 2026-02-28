#ifndef DATABASE_SERVICE_H
#define DATABASE_SERVICE_H

#include "intent/IntentClassifier.h"
#include <pqxx/pqxx>
#include <memory>
#include <string>

class DatabaseService {
public:
    DatabaseService(const std::string& connectionString);
    ~DatabaseService();

    bool connect();
    void disconnect();
    bool isConnected() const { return connected_; }

    // Log voice command
    int logVoiceCommand(const VoiceCommand& command);

    // Log intent result
    bool logIntentResult(int commandId, const IntentResult& result);

    // Get statistics
    int getTotalCommands();
    int getSuccessfulIntents();
    std::map<std::string, int> getIntentDistribution();

private:
    std::string connectionString_;
    std::unique_ptr<pqxx::connection> conn_;
    bool connected_;
};

#endif // DATABASE_SERVICE_H
