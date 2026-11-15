#pragma once

#include "StarDataParadiseDLLler.h"

#include <sdp/wire.hpp>
#include <asio.hpp>
#include <asio/ssl.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>

namespace sdp::native
{
struct RemoteError
{
    sdp_error details;
};

struct Endpoint
{
    asio::ip::tcp::endpoint address;
    std::string expected_name;
    std::filesystem::path ca_file;
};

struct PasswordChange
{
    std::string_view username;
    std::string_view old_password;
    std::string_view new_password;
    std::string_view token;
};

class Transport
{
public:
    explicit Transport(std::chrono::milliseconds timeout);
    std::string open(const Endpoint& endpoint, std::string_view username, std::string_view password);
    void health();
    wire::Json request(std::string_view op, wire::Json body, bool& dispatched);
    void password_set(const PasswordChange& change, bool& dispatched);
    void begin(uint64_t operation) noexcept;
    void cancel(uint64_t operation);
    void close() noexcept;
    bool connected() const noexcept { return connected_.load(); }

private:
    using Deadline = std::chrono::steady_clock::time_point;
    std::chrono::steady_clock::duration remaining(Deadline deadline) const;
    auto token(Deadline deadline, asio::error_code& error) const
    {
        return asio::cancel_after(remaining(deadline), asio::redirect_error(asio::use_awaitable, error));
    }
    void check(const asio::error_code& error, bool tls = false) const;
    void disconnect() noexcept;
    asio::awaitable<std::string> connect(const Endpoint& endpoint, std::string_view username,
        std::string_view password, Deadline deadline);
    asio::awaitable<wire::Json> exchange(wire::Json packet, Deadline deadline, bool* dispatched = nullptr);
    asio::awaitable<wire::Json> exchange_packet(wire::Json& packet, Deadline deadline, bool* dispatched,
        bool contains_credentials = false);
    asio::awaitable<void> health_request();
    asio::awaitable<wire::Json> request_operation(std::string_view op, wire::Json body, bool& dispatched);
    asio::awaitable<void> password_request(const PasswordChange& change, bool& dispatched);
    asio::awaitable<void> shutdown();

    template<class Result>
    Result execute(asio::awaitable<Result> task)
    {
        try
        {
            io_.restart();
            auto result = asio::co_spawn(io_, std::move(task), asio::use_future);
            io_.run();
            return result.get();
        }
        catch (const RemoteError&) { throw; }
        catch (...)
        {
            disconnect();
            throw;
        }
    }

    std::chrono::milliseconds timeout_;
    asio::io_context io_{1};
    asio::ssl::context tls_{asio::ssl::context::tls_client};
    asio::ssl::stream<asio::ip::tcp::socket> stream_{io_, tls_};
    std::atomic<uint64_t> active_{0};
    std::atomic_bool cancelled_{false};
    std::atomic_bool connected_{false};
    uint32_t next_id_{1};
    std::string session_;
};
}
