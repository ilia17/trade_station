#ifndef GATE_HANDLER_H
#define GATE_HANDLER_H

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include "../scr/exchange.h"

using json = nlohmann::json;

typedef websocketpp::client<websocketpp::config::asio_tls_client> ws_client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> ssl_context_ptr;

class GateHandler : public Exchange {
private:
    ws_client client;
    websocketpp::connection_hdl connection;

    ssl_context_ptr on_tls_init();

    // Sends {"time":...,"channel":"spot.ping"} every 10s
    void schedule_ping();

    void on_open(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl,
                    ws_client::message_ptr msg);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);

    // Parse a full snapshot result into top-5 OrderBookUpdate
    OrderBookUpdate parse_snapshot(const json& result);

public:
    GateHandler(Disruptor<1024>& disruptor);

    void connect()    override;
    void subscribe()  override;
    void disconnect() override;
    void run()        override;
    void change_symbol(const std::string& base, const std::string& quote) override;

    void subscribe_trades();
    void parse_trade(const json& result);
};

#endif
