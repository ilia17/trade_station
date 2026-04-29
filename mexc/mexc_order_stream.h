#ifndef MEXC_ORDER_STREAM_H
#define MEXC_ORDER_STREAM_H

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <atomic>

#include "../trading/order.h"
#include "../proto/PushDataV3ApiWrapper.pb.h"

using json = nlohmann::json;

typedef websocketpp::client<websocketpp::config::asio_tls_client>      order_ws_client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> order_ssl_ptr;

// Subscribes to wss://wbs-api.mexc.com/ws?listenKey=...
// and publishes real-time order status changes into SharedOrderEvents.
//
// Protocol (per MEXC docs):
//   1. POST /api/v3/userDataStream  → { "listenKey": "..." }
//   2. Connect WS with that listenKey
//   3. Subscribe: {"method":"SUBSCRIPTION","params":["spot@private.orders.v3.api.pb"]}
//   4. Frames are binary PushDataV3ApiWrapper protobuf with privateorders() payload
//   5. Renew listenKey every 30 min via PUT /api/v3/userDataStream?listenKey=...
class MexcOrderStream {
public:
    // api_key    : MEXC API key
    // api_secret : MEXC API secret (needed to sign the listenKey request)
    // events     : shared queue that render.cpp drains each frame
    MexcOrderStream(const std::string& api_key, const std::string& api_secret,
                    SharedOrderEvents* events);
    ~MexcOrderStream();

    void start();   // non-blocking: spawns background threads
    void stop();
    bool is_running() const { return running_.load(); }

private:
    std::string       api_key_;
    std::string       api_secret_;
    SharedOrderEvents* events_;
    std::string       listen_key_;

    order_ws_client                client_;
    websocketpp::connection_hdl    connection_;
    std::atomic<bool>              running_{false};
    std::atomic<bool>              connected_{false};

    std::thread ws_thread_;
    std::thread renew_thread_;

    // listenKey lifecycle
    bool get_listen_key();
    bool renew_listen_key();

    // WebSocket callbacks
    order_ssl_ptr on_tls_init();
    void on_open(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, order_ws_client::message_ptr msg);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);

    void subscribe_orders();
    void schedule_ping();  // sends {"msg":"ping"} every 15s to keep connection alive
    void handle_order_proto(const PushDataV3ApiWrapper& wrapper);
};

#endif
