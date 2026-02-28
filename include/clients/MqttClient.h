#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <string>
#include <functional>
#include <mqtt/async_client.h>

class MqttClient {
public:
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    MqttClient(const std::string& broker, int port, const std::string& clientId,
               const std::string& username, const std::string& password);
    ~MqttClient();

    bool connect();
    void disconnect();
    bool isConnected() const;

    void subscribe(const std::string& topic, int qos = 1);
    void publish(const std::string& topic, const std::string& payload, int qos = 1, bool retained = false);

    void setMessageCallback(MessageCallback callback);

private:
    class Callback : public virtual mqtt::callback {
    public:
        explicit Callback(MqttClient* client) : client_(client) {}

        void connected(const std::string& cause) override;
        void connection_lost(const std::string& cause) override;
        void message_arrived(mqtt::const_message_ptr msg) override;

    private:
        MqttClient* client_;
    };

    std::string broker_;
    int port_;
    std::string clientId_;
    std::string username_;
    std::string password_;

    mqtt::async_client* mqttClient_;
    Callback* callback_;
    MessageCallback messageCallback_;
    bool connected_;
};

#endif // MQTT_CLIENT_H
