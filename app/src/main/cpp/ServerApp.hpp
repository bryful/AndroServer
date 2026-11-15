#ifndef ANDROSERVER_SERVER_APP_HPP
#define ANDROSERVER_SERVER_APP_HPP

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include "oatpp/network/Server.hpp"
#include "AppComponent.hpp"

class ServerApp {
private:
    std::atomic<bool> m_isRunning{false};
    std::mutex m_lifecycle;
    std::thread m_serverThread;
    std::shared_ptr<oatpp::network::ServerConnectionProvider> m_provider;
    std::shared_ptr<oatpp::network::ConnectionHandler> m_handler;
    std::shared_ptr<AppComponent> m_component;
    std::atomic<int> m_port{8080};

    void stopLocked();

public:
    static ServerApp& getInstance() {
        static ServerApp instance;
        return instance;
    }

    bool start(int port, const std::vector<std::string>& rootDirs, const std::string& host,
               const std::string& authorization, const std::string& publicOrigin = {});
    void stop();
    bool isRunning() const { return m_isRunning.load(); }
    int getPort() const { return m_port; }
};

#endif // ANDROSERVER_SERVER_APP_HPP

