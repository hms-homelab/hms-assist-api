#include "clients/MqttClient.h"
#include <iostream>
#include <chrono>
#include <thread>

MqttClient::MqttClient(const std::string& broker, int port, const std::string& clientId,
                       const std::string& username, const std::string& password)
    : broker_(broker), port_(port), clientId_(clientId), username_(username),
      password_(password), mqttClient_(nullptr), callback_(nullptr), connected_(false) {

    std::string serverUri = "tcp://" + broker_ + ":" + std::to_string(port_);
    mqttClient_ = new mqtt::async_client(serverUri, clientId_);
    callback_ = new Callback(this);
    mqttClient_->set_callback(*callback_);
}

MqttClient::~MqttClient() {
    disconnect();
    delete callback_;
    delete mqttClient_;
}

bool MqttClient::connect() {
    try {
        mqtt::connect_options connOpts;
        connOpts.set_keep_alive_interval(20);
        connOpts.set_clean_session(true);
        connOpts.set_user_name(username_);
        connOpts.set_password(password_);
        connOpts.set_automatic_reconnect(true);

        std::cout << "[MQTT] Connecting to broker: " << broker_ << ":" << port_ << std::endl;

        mqtt::token_ptr conntok = mqttClient_->connect(connOpts);
        conntok->wait();

        connected_ = true;
        std::cout << "[MQTT] Connected successfully" << std::endl;
        return true;
    } catch (const mqtt::exception& exc) {
        std::cerr << "[MQTT] Connection failed: " << exc.what() << std::endl;
        connected_ = false;
        return false;
    }
}

void MqttClient::disconnect() {
    if (connected_ && mqttClient_) {
        try {
            mqttClient_->disconnect()->wait();
            connected_ = false;
            std::cout << "[MQTT] Disconnected" << std::endl;
        } catch (const mqtt::exception& exc) {
            std::cerr << "[MQTT] Disconnect error: " << exc.what() << std::endl;
        }
    }
}

bool MqttClient::isConnected() const {
    return connected_ && mqttClient_ && mqttClient_->is_connected();
}

void MqttClient::subscribe(const std::string& topic, int qos) {
    if (!isConnected()) {
        std::cerr << "[MQTT] Cannot subscribe - not connected" << std::endl;
        return;
    }

    try {
        mqttClient_->subscribe(topic, qos)->wait();
        std::cout << "[MQTT] Subscribed to: " << topic << std::endl;
    } catch (const mqtt::exception& exc) {
        std::cerr << "[MQTT] Subscribe failed: " << exc.what() << std::endl;
    }
}

void MqttClient::publish(const std::string& topic, const std::string& payload, int qos, bool retained) {
    if (!isConnected()) {
        std::cerr << "[MQTT] Cannot publish - not connected" << std::endl;
        return;
    }

    try {
        auto msg = mqtt::make_message(topic, payload);
        msg->set_qos(qos);
        msg->set_retained(retained);
        mqttClient_->publish(msg)->wait();
        std::cout << "[MQTT] Published to " << topic << ": " << payload << std::endl;
    } catch (const mqtt::exception& exc) {
        std::cerr << "[MQTT] Publish failed: " << exc.what() << std::endl;
    }
}

void MqttClient::setMessageCallback(MessageCallback callback) {
    messageCallback_ = callback;
}

void MqttClient::Callback::connected(const std::string& cause) {
    std::cout << "[MQTT] Connected: " << cause << std::endl;
    client_->connected_ = true;
}

void MqttClient::Callback::connection_lost(const std::string& cause) {
    std::cout << "[MQTT] Connection lost: " << cause << std::endl;
    client_->connected_ = false;
}

void MqttClient::Callback::message_arrived(mqtt::const_message_ptr msg) {
    if (client_->messageCallback_) {
        client_->messageCallback_(msg->get_topic(), msg->to_string());
    }
}
