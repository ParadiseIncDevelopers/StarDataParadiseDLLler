#ifndef STARDATAPARADISEDLLLER_HPP
#define STARDATAPARADISEDLLLER_HPP

#include "StarDataParadiseDLLler.h"

#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sdp::client
{
inline sdp_text text(std::string_view value)
{
    if (value.size() > (std::numeric_limits<uint32_t>::max)())
        throw std::length_error("Native SDK input exceeds the C ABI length limit");
    return {value.data(), static_cast<uint32_t>(value.size())};
}

class Error : public std::runtime_error
{
public:
    Error(sdp_status status, const sdp_error& details)
        : std::runtime_error(details.message[0] ? details.message : "Native SDK operation failed"),
          status_(status), details_(details) {}

    sdp_status status() const noexcept { return status_; }
    const sdp_error& details() const noexcept { return details_; }
    bool unknown_outcome() const noexcept { return details_.unknown_outcome != 0; }

private:
    sdp_status status_;
    sdp_error details_;
};

inline void check(sdp_status status, const sdp_error& details)
{
    if (status != SDP_OK) throw Error(status, details);
}

class Buffer
{
public:
    Buffer() noexcept = default;
    ~Buffer() { reset(); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept : value_(std::exchange(other.value_, {})) {}
    Buffer& operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            value_ = std::exchange(other.value_, {});
        }
        return *this;
    }

    void reset() noexcept { sdp_buffer_free(&value_); }

    std::string_view view() const noexcept
    {
        if (!value_.data) return {};
        return {reinterpret_cast<const char*>(value_.data), value_.size};
    }

    sdp_buffer* put() noexcept
    {
        reset();
        return &value_;
    }

private:
    sdp_buffer value_{};
};

class Connection
{
public:
    explicit Connection(const sdp_open_options& options)
    {
        sdp_error error{};
        check(sdp_open(&options, &handle_, &error), error);
    }

    ~Connection() { if (handle_) sdp_close(handle_, nullptr); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Connection& operator=(Connection&& other)
    {
        if (this != &other)
        {
            close();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }

    sdp_connection native_handle() const noexcept { return handle_; }

    void close()
    {
        if (!handle_) return;
        sdp_error error{};
        check(sdp_close(handle_, &error), error);
        handle_ = 0;
    }

    void health() const
    {
        sdp_error error{};
        check(sdp_health(handle_, &error), error);
    }

    Buffer session_info() const { return read(sdp_get_session_info); }
    Buffer capabilities() const { return read(sdp_get_capabilities); }
    Buffer metadata() const { return read(sdp_metadata); }

    Buffer outcome(std::string_view token) const
    {
        Buffer answer;
        sdp_error error{};
        check(sdp_tx_outcome(handle_, text(token), answer.put(), &error), error);
        return answer;
    }

private:
    using Read = sdp_status (SDP_CALL*)(sdp_connection, sdp_buffer*, sdp_error*);

    Buffer read(Read operation) const
    {
        Buffer answer;
        sdp_error error{};
        check(operation(handle_, answer.put(), &error), error);
        return answer;
    }

    sdp_connection handle_{};
};

// A Statement must be destroyed or closed before the authenticated Connection is closed.
class Statement
{
public:
    Statement(const Connection& connection, std::string_view sql, std::string_view types = {})
        : connection_(connection.native_handle())
    {
        sdp_error error{};
        check(sdp_prepare_typed(connection_, text(sql), text(types), id_.put(), &error), error);
    }

    ~Statement()
    {
        if (!id_.view().empty()) sdp_statement_close(connection_, id_text(), nullptr);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) noexcept = default;
    Statement& operator=(Statement&& other)
    {
        if (this != &other)
        {
            close();
            connection_ = other.connection_;
            id_ = std::move(other.id_);
        }
        return *this;
    }

    std::string_view id() const noexcept { return id_.view(); }

    void close()
    {
        if (id().empty()) return;
        sdp_error error{};
        check(sdp_statement_close(connection_, id_text(), &error), error);
        id_.reset();
    }

    Buffer execute(std::string_view parameters = {}, std::string_view request_token = {},
        uint32_t row_limit = 1'000'000) const
    {
        Buffer answer;
        sdp_error error{};
        check(sdp_execute(connection_, id_text(), text(parameters), text(request_token), row_limit, answer.put(), &error),
            error);
        return answer;
    }

private:
    sdp_text id_text() const noexcept
    {
        const auto value = id();
        return {value.data(), static_cast<uint32_t>(value.size())};
    }

    sdp_connection connection_;
    Buffer id_;
};

// A Transaction owns rollback on scope exit while its original session is still open.
class Transaction
{
public:
    explicit Transaction(const Connection& connection) : connection_(connection.native_handle())
    {
        sdp_error error{};
        check(sdp_tx_begin(connection_, &error), error);
    }

    ~Transaction() { if (connection_) sdp_tx_rollback(connection_, nullptr); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept : connection_(std::exchange(other.connection_, 0)) {}
    Transaction& operator=(Transaction&& other)
    {
        if (this != &other)
        {
            rollback();
            connection_ = std::exchange(other.connection_, 0);
        }
        return *this;
    }

    bool active() const noexcept { return connection_ != 0; }

    Buffer commit(std::string_view token)
    {
        Buffer answer;
        sdp_error error{};
        const auto status = sdp_tx_commit(connection_, text(token), answer.put(), &error);
        if (status == SDP_OK || error.unknown_outcome) connection_ = 0;
        check(status, error);
        return answer;
    }

    void rollback()
    {
        if (!connection_) return;
        sdp_error error{};
        check(sdp_tx_rollback(connection_, &error), error);
        connection_ = 0;
    }

private:
    sdp_connection connection_;
};
}

#endif
