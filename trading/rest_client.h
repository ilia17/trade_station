#ifndef REST_CLIENT_H
#define REST_CLIENT_H

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace net       = boost::asio;
namespace ssl       = boost::asio::ssl;
using     tcp       = net::ip::tcp;

// ── Crypto helpers ────────────────────────────────────────────────────────────

inline std::string to_hex(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    for (size_t i = 0; i < len; i++)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return ss.str();
}

inline std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    HMAC(EVP_sha256(),
         key.data(),  key.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         hash, &len);
    return to_hex(hash, len);
}

inline std::string hmac_sha512_hex(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    HMAC(EVP_sha512(),
         key.data(),  key.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         hash, &len);
    return to_hex(hash, len);
}

inline std::string sha512_hex(const std::string& data) {
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    return to_hex(hash, SHA512_DIGEST_LENGTH);
}

// ── URL encoder ───────────────────────────────────────────────────────────────

inline std::string url_encode(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << std::uppercase << std::hex << std::setw(2)
                << std::setfill('0') << (int)c;
    }
    return out.str();
}

// ── Synchronous HTTPS POST ────────────────────────────────────────────────────
// Returns the response body. Throws std::runtime_error on failure.

inline std::string https_post(
    const std::string& host,
    const std::string& target,
    const std::string& body,
    const std::vector<std::pair<std::string,std::string>>& headers)
{
    net::io_context ioc;
    ssl::context    ctx(ssl::context::tlsv12_client);
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_none);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        throw std::runtime_error("SNI failed");

    auto results = resolver.resolve(host, "443");
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host,         host);
    req.set(http::field::user_agent,   "TradeStation/1.0");
    for (auto& [k, v] : headers)
        req.set(k, v);
    req.body() = body;
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);

    beast::error_code ec;
    stream.shutdown(ec); // ignore graceful-close errors

    return res.body();
}

#endif
