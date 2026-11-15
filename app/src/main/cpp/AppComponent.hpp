#ifndef ANDROSERVER_APP_COMPONENT_HPP
#define ANDROSERVER_APP_COMPONENT_HPP

#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/component.hpp"

/**
 * oatpp の DI (Dependency Injection) コンポーネント
 */
class AppComponent {
private:
    oatpp::String m_host;
    v_uint16 m_port;

public:
    AppComponent(const std::string& host, v_uint16 port) : m_host(host.c_str()), m_port(port) {}

    /**
     * HTTPルーター
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, httpRouter)([] {
        return oatpp::web::server::HttpRouter::createShared();
    }());

    /**
     * JSON Object Mapper
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper)([] {
        auto serializerConfig = oatpp::parser::json::mapping::Serializer::Config::createShared();
        serializerConfig->useBeautifier = false; // 高速化のためインデント省略
        auto deserializerConfig = oatpp::parser::json::mapping::Deserializer::Config::createShared();
        deserializerConfig->allowUnknownFields = true;
        return std::make_shared<oatpp::parser::json::mapping::ObjectMapper>(serializerConfig, deserializerConfig);
    }());

    /**
     * TCP サーバーコネクションプロバイダー (IPv4)
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, serverConnectionProvider)([this] {
        return oatpp::network::tcp::server::ConnectionProvider::createShared(
            {m_host, m_port, oatpp::network::Address::IP_4}
        );
    }());

    /**
     * HTTP コネクションハンドラー (マルチスレッドプール)
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, serverConnectionHandler)([] {
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
        return oatpp::web::server::HttpConnectionHandler::createShared(router);
    }());
};

#endif // ANDROSERVER_APP_COMPONENT_HPP

