
#ifndef MEXC_HANDLER_H
#define MEXC_HANDLER_H

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <map>
#include <functional>
#include "../scr/exchange.h"
#include "../proto/PushDataV3ApiWrapper.pb.h"
#include "../proto/PublicAggreDepthsV3Api.pb.h"

using json = nlohmann::json;

typedef websocketpp::client<websocketpp::config::asio_tls_client> ws_client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> ssl_context_ptr;

class MexcHandler : public Exchange {
private:
    ws_client client;
    websocketpp::connection_hdl connection;

    // In-memory order book: bids sorted high→low, asks sorted low→high
    std::map<double, double, std::greater<double>> bids_book;
    std::map<double, double>                       asks_book;

    // SSL context for TLS connection
    ssl_context_ptr on_tls_init();

    // Heartbeat — sends {"msg":"ping"} every 20s to keep connection alive
    void schedule_ping();

    // WebSocket callbacks
    void on_open(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl,
                    ws_client::message_ptr msg);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);

    // Apply incremental depth deltas and build a top-5 snapshot
    OrderBookUpdate apply_depth(const PublicAggreDepthsV3Api& depths);

public:
    MexcHandler(Disruptor<1024>& disruptor);

    void connect()    override;
    void subscribe()  override;
    void disconnect() override;
    void run()        override;
};

#endif