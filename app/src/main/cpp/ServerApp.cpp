#include "ServerApp.hpp"
#include "controller/FileController.hpp"
#include "controller/WebUIController.hpp"
#include "oatpp/core/base/Environment.hpp"
#include <android/log.h>

#define TAG "AndroServerNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {
class Authentication : public oatpp::web::server::interceptor::RequestInterceptor {
public:
    Authentication(std::string origin, std::string authorization)
        : m_origin(std::move(origin)), m_host(m_origin.substr(m_origin.find("://") + 3)),
          m_authorization(std::move(authorization)) {}

    std::shared_ptr<OutgoingResponse> intercept(const std::shared_ptr<IncomingRequest>& request) override {
        using oatpp::web::protocol::http::Status;
        const auto host = request->getHeader("Host");
        const auto origin = request->getHeader("Origin");
        const auto fetchSite = request->getHeader("Sec-Fetch-Site");
        if (!host || *host != m_host ||
            (origin && *origin != m_origin) ||
            (fetchSite && *fetchSite == "cross-site")) {
            auto response = OutgoingResponse::createShared(Status::CODE_403, nullptr);
            response->putHeader("Connection", "close");
            return response;
        }

        const auto authorization = request->getHeader("Authorization");
        if (authorization && authorization->size() == m_authorization.size()) {
            unsigned char difference = 0;
            for (size_t i = 0; i < m_authorization.size(); ++i) {
                difference |= (*authorization)[i] ^ m_authorization[i];
            }
            if (difference == 0) return nullptr;
        }

        auto response = OutgoingResponse::createShared(Status::CODE_401, nullptr);
        response->putHeader("WWW-Authenticate", "Basic realm=\"AndroServer\", charset=\"UTF-8\"");
        response->putHeader("Cache-Control", "no-store");
        response->putHeader("Connection", "close");
        return response;
    }

private:
    std::string m_origin;
    std::string m_host;
    std::string m_authorization;
};
}

bool ServerApp::start(int port, const std::vector<std::string>& rootDirs, const std::string& host,
                      const std::string& authorization, const std::string& publicOrigin) {
    std::lock_guard<std::mutex> lock(m_lifecycle);
    if (m_isRunning.load()) return true;
    if (port < 0 || port > 65535 || host.empty() || authorization.empty() ||
        (port == 0 && (host != "127.0.0.1" || publicOrigin.rfind("https://", 0) != 0))) return false;
    stopLocked();

    FileController::Roots roots;
    for (const auto& directory : rootDirs) {
        std::error_code ec;
        const auto root = fs::canonical(directory, ec);
        if (ec || !fs::is_directory(root, ec) || access(root.c_str(), R_OK | W_OK | X_OK) != 0) {
            return false;
        }
        const auto name = root.filename().string();
        if (name.empty()) return false;
        auto alias = name;
        for (size_t suffix = 2; roots.count(alias); ++suffix) {
            alias = name + " (" + std::to_string(suffix) + ")";
        }
        roots.emplace(std::move(alias), root.string());
    }
    if (roots.empty()) return false;

    oatpp::base::Environment::init();
    try {
        m_component = std::make_shared<AppComponent>(host, static_cast<v_uint16>(port));
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
        OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, handler);
        OATPP_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, provider);
        router->addController(FileController::createShared(std::move(roots)));
        router->addController(WebUIController::createShared());
        const auto boundPort = provider->getProperty(oatpp::network::ConnectionProvider::PROPERTY_PORT).toString();
        const int actualPort = std::stoi(*boundPort);
        const auto origin = publicOrigin.empty()
            ? "http://" + (port == 80 ? host : host + ":" + std::to_string(port))
            : publicOrigin;
        std::static_pointer_cast<oatpp::web::server::HttpConnectionHandler>(handler)
            ->addRequestInterceptor(std::make_shared<Authentication>(origin, authorization));

        auto server = std::make_shared<oatpp::network::Server>(provider, handler);
        m_provider = provider;
        m_handler = handler;
        m_port.store(actualPort);
        m_isRunning.store(true);
        m_serverThread = std::thread([this, server] {
            try {
                server->run([this] { return m_isRunning.load(); });
            } catch (const std::exception& e) {
                LOGE("Server error: %s", e.what());
            }
            m_isRunning.store(false);
        });
        return true;
    } catch (const std::exception& e) {
        LOGE("Server startup failed: %s", e.what());
    }
    if (m_component) {
        stopLocked();
    } else {
        oatpp::base::Environment::destroy();
    }
    return false;
}

void ServerApp::stop() {
    std::lock_guard<std::mutex> lock(m_lifecycle);
    stopLocked();
}

void ServerApp::stopLocked() {
    m_isRunning.store(false);
    // The provider polls accept with a timeout. Join before closing its socket.
    if (m_serverThread.joinable()) m_serverThread.join();
    if (m_provider) m_provider->stop();
    if (m_handler) m_handler->stop();
    m_provider.reset();
    m_handler.reset();
    if (m_component) {
        m_component.reset();
        oatpp::base::Environment::destroy();
    }
}
