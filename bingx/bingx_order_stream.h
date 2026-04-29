#ifndef BINGX_ORDER_STREAM_H
#define BINGX_ORDER_STREAM_H

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <atomic>

#include "../trading/order.h"

using bingx_order_ws_t  = websocketpp::client<websocketpp::config::asio_tls_client>;
using bingx_order_ssl_t = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;

// Streams real-time order events for the BingX spot account via WebSocket.
//
// Protocol:
//   1. POST /openApi/user/auth/userDataStream  (X-BX-APIKEY header)
//      → { "listenKey": "..." }
//   2. Connect: wss://open-api-ws.bingx.com/market?listenKey=<key>
//   3. Subscribe: {"id":"...","reqType":"sub","dataType":"spot.executionReport"}
//   4. Messages are gzip-compressed JSON.
//   5. Server sends "Ping" text frames; respond with "Pong".
//   6. Renew listenKey every 30 min via PUT /openApi/user/auth/userDataStream
//
// Order status field "X": NEW / PARTIALLY_FILLED / FILLED / CANCELED
class BingXOrderStream {
public:
    BingXOrderStream(const std::string& api_key, const std::string& api_secret,
                     SharedOrderEvents* events);
    ~BingXOrderStream();

    void start();
    void stop();
    bool is_running() const { return running_.load(); }

private:
    std::string        api_key_;
    std::string        api_secret_;
    SharedOrderEvents* events_;
    std::string        listen_key_;

    bingx_order_ws_t            client_;
    websocketpp::connection_hdl connection_;
    std::atomic<bool>           running_{false};
    std::atomic<bool>           connected_{false};

    std::thread ws_thread_;
    std::thread renew_thread_;

    bool get_listen_key();
    bool renew_listen_key();

    bingx_order_ssl_t on_tls_init();
    void on_open(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, bingx_order_ws_t::message_ptr msg);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);

    void subscribe_orders();
    void schedule_ping();
    void handle_order_event(const nlohmann::json& data);
};

#endif
