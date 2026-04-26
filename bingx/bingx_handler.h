#ifndef BINGX_HANDLER_H
#define BINGX_HANDLER_H

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include "../scr/exchange.h"

using json = nlohmann::json;

typedef websocketpp::client<websocketpp::config::asio_tls_client> ws_client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> ssl_context_ptr;

class BingXHandler : public Exchange {
private:
    ws_client client;
    websocketpp::connection_hdl connection;

    ssl_context_ptr on_tls_init();

    // Sends {"ping":<ts>} every 20s
    void schedule_ping();

    void on_open(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl,
                    ws_client::message_ptr msg);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);

    // BingX messages are gzip-compressed binary frames
    std::string decompress_gzip(const std::string& compressed);

    // Parse a full depth snapshot into top-5 OrderBookUpdate
    OrderBookUpdate parse_snapshot(const json& data);

public:
    BingXHandler(Disruptor<1024>& disruptor);

    void connect()    override;
    void subscribe()  override;
    void disconnect() override;
    void run()        override;
    void change_symbol(const std::string& base, const std::string& quote) override;

    void subscribe_trades();
    void parse_trade(const json& data);
};

#endif
