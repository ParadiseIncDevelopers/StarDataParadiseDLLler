#include "transport.hpp"

#include <openssl/crypto.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <fstream>

namespace sdp::native
{
namespace
{
using wire::Code;
using wire::Error;
using wire::Json;
using namespace std::chrono_literals;

struct SecretBytes
{
    std::string bytes;
    ~SecretBytes() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

struct ClearWireRequest
{
    Json& message;
    bool auth;
    ~ClearWireRequest() { clear(); }

    void clear()
    {
        if (auth)
        {
            auto& password = message["body"]["password"].get_ref<std::string&>();
            OPENSSL_cleanse(password.data(), password.size());
            auth = false;
        }
    }
};

template<size_t N>
void copy_text(char (&destination)[N], std::string_view value)
{
    const auto count = (std::min)(value.size(), N - 1);
    std::copy_n(value.begin(), count, destination);
    destination[count] = '\0';
}

uint32_t source_position(const Json& value)
{
    if (value.is_null()) return 0;
    if (!value.is_number_unsigned() || value == 0 || value > UINT32_MAX)
        throw Error(Code::malformed);
    return value.get<uint32_t>();
}

bool hex_id(std::string_view id)
{
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == id.npos;
}

RemoteError decode_remote_error(const Json& error, std::string_view session, uint32_t id)
{
    sdp_error result{};
    const auto& code = error.at("code");
    if (!code.is_number_unsigned() || code < SDP_MALFORMED_MESSAGE || code > SDP_OUT_OF_MEMORY)
        throw Error(Code::malformed);

    result.code = code.get<int32_t>();
    const auto description = wire::describe(static_cast<Code>(result.code));
    if (wire::string_field(error, "sqlstate") != description.state)
        throw Error(Code::malformed);

    const auto& retryable = error.at("retryable");
    const auto& unknown = error.at("unknown_outcome");
    if (!retryable.is_boolean() || !unknown.is_boolean()) throw Error(Code::malformed);
    result.retryable = retryable.get<bool>();
    result.unknown_outcome = unknown.get<bool>();
    result.line = source_position(error.at("line"));
    result.column = source_position(error.at("column"));
    copy_text(result.sqlstate, description.state);
    copy_text(result.message, description.message);
    const auto& correlation = error.at("correlation_id");
    if (correlation.is_string() && !session.empty() &&
        correlation == std::string(session) + "-" + std::to_string(id))
        copy_text(result.correlation_id, correlation.get_ref<const std::string&>());
    return {result};
}

void configure_tls(asio::ssl::context& context, SSL* stream, const Endpoint& endpoint)
{
    try
    {
        if (SSL_set_min_proto_version(stream, TLS1_2_VERSION) != 1) throw Error(Code::tls_failed);
        SSL_set_options(stream, SSL_OP_NO_COMPRESSION);

        std::ifstream file(endpoint.ca_file, std::ios::binary);
        std::string certificate(wire::max_payload + 1, '\0');
        if (!file || (!file.read(certificate.data(), static_cast<std::streamsize>(certificate.size()))
            && (!file.eof() || file.bad()))) throw Error(Code::tls_failed);
        const auto size = static_cast<size_t>(file.gcount());
        if (size == 0 || size > wire::max_payload) throw Error(Code::tls_failed);
        certificate.resize(size);
        context.add_certificate_authority(asio::buffer(certificate));
        SSL_set_verify(stream, SSL_VERIFY_PEER, nullptr);
        SSL_set_hostflags(stream, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

        asio::error_code address_error;
        asio::ip::make_address(endpoint.expected_name, address_error);
        if (!address_error)
        {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(stream), endpoint.expected_name.c_str()) != 1)
                throw Error(Code::tls_failed);
        }
        else
        {
            if (endpoint.expected_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")
                != std::string::npos) throw Error(Code::invalid_argument);
            if (SSL_set_tlsext_host_name(stream, endpoint.expected_name.c_str()) != 1 ||
                SSL_set1_host(stream, endpoint.expected_name.c_str()) != 1)
                throw Error(Code::tls_failed);
        }
    }
    catch (const asio::system_error&) { throw Error(Code::tls_failed); }
}
}

Transport::Transport(std::chrono::milliseconds timeout) : timeout_(timeout) {}

void Transport::begin(uint64_t operation) noexcept
{
    cancelled_.store(false);
    active_.store(operation);
}

void Transport::cancel(uint64_t operation)
{
    cancelled_.store(true);
    asio::post(io_, [this, operation] {
        if (active_.load() != operation) return;
        asio::error_code ignored;
        stream_.lowest_layer().cancel(ignored);
    });
}

void Transport::check(const asio::error_code& error, bool tls) const
{
    if (cancelled_.load()) throw Error(Code::cancelled);
    if (!error) return;
    if (error == asio::error::operation_aborted) throw Error(Code::timeout);
    if (tls) throw Error(Code::tls_failed);
    throw Error(Code::connection_lost);
}

std::chrono::steady_clock::duration Transport::remaining(Deadline deadline) const
{
    check({});
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw Error(Code::timeout);
    return deadline - now;
}

std::string Transport::open(const Endpoint& endpoint, std::string_view username, std::string_view password)
{
    return execute(connect(endpoint, username, password, std::chrono::steady_clock::now() + timeout_));
}

asio::awaitable<std::string> Transport::connect(const Endpoint& endpoint, std::string_view username,
    std::string_view password, Deadline deadline)
{
    configure_tls(tls_, stream_.native_handle(), endpoint);
    asio::error_code error;
    co_await stream_.next_layer().async_connect(endpoint.address, token(deadline, error));
    check(error);
    co_await stream_.async_handshake(asio::ssl::stream_base::client, token(deadline, error));
    check(error, true);

    const auto hello = co_await exchange(wire::request(next_id_++, "hello",
        Json{{"client", "StarDataParadiseDLLler/0.1.0"}}), deadline);
    session_ = wire::string_field(hello, "session_id");
    const auto protocol = hello.find("protocol_version");
    if (!hex_id(session_) || protocol == hello.end() || !protocol->is_number_integer() || *protocol != wire::version)
        throw Error(Code::incompatible);
    wire::string_field(hello, "product_version");

    Json credentials = Json::object();
    credentials["username"] = username;
    credentials["password"] = password;
    const auto auth_packet_id = next_id_++;
    auto auth_request = wire::request(auth_packet_id, "auth", std::move(credentials));
    Json auth = co_await exchange(std::move(auth_request), deadline, true);
    const auto authenticated = auth.find("authenticated");
    if (authenticated == auth.end() || !authenticated->is_boolean()) throw Error(Code::malformed);
    if (!authenticated->get<bool>()) throw Error(Code::auth_failed);

    const auto body = co_await exchange(wire::request(next_id_++, "capabilities"), deadline);
    const auto& features = wire::object_field(body, "features");
    if (!std::all_of(features.begin(), features.end(), [](const Json& value){ return value.is_boolean(); }))
        throw Error(Code::malformed);
    for (const auto key : {"tls", "auth", "health"})
        if (features.value(key, false) != true) throw Error(Code::incompatible);

    connected_.store(true);
    co_return wire::serialize(features);
}

asio::awaitable<Json> Transport::exchange(Json packet, Deadline deadline, bool auth)
{
    ClearWireRequest scrub{packet, auth};
    try
    {
        const auto id = wire::request_id(packet);
        SecretBytes payload{wire::serialize(packet)};
        const auto header = wire::header_for(payload.bytes.size());
        const std::array<asio::const_buffer, 2> buffers{asio::buffer(header), asio::buffer(payload.bytes)};
        asio::error_code error;
        co_await asio::async_write(stream_, buffers, token(deadline, error));
        OPENSSL_cleanse(payload.bytes.data(), payload.bytes.size());
        check(error);
        scrub.clear();

        wire::FrameHeader response_header{};
        co_await asio::async_read(stream_, asio::buffer(response_header), token(deadline, error));
        check(error);
        std::string bytes(wire::payload_size(response_header), '\0');
        co_await asio::async_read(stream_, asio::buffer(bytes), token(deadline, error));
        check(error);
        auto response = wire::parse_payload(bytes);
        wire::check_version(response);
        if (wire::request_id(response) != id || !response.at("ok").is_boolean())
            throw Error(Code::malformed);
        if (!response.at("ok").get<bool>())
            throw decode_remote_error(wire::object_field(response, "error"), session_, id);

        wire::object_field(response, "body");
        co_return std::move(response.at("body"));
    }
    catch (const Json::exception&) { throw Error(Code::malformed); }
}

asio::awaitable<void> Transport::health_request()
{
    if (!connected()) throw Error(Code::connection_lost);
    if (!next_id_) throw Error(Code::invalid_state);
    const auto result = co_await exchange(wire::request(next_id_++, "health"),
        std::chrono::steady_clock::now() + timeout_);
    wire::string_field(result, "product_version");
    if (wire::string_field(result, "status").empty()) throw Error(Code::malformed);
}

asio::awaitable<std::string> Transport::prepare_request(std::string_view sql)
{
    if (!connected()) throw Error(Code::connection_lost);
    if (!next_id_) throw Error(Code::invalid_state);
    Json body{{"sql", sql}};
    const auto result = co_await exchange(wire::request(next_id_++, "query.prepare", std::move(body)),
        std::chrono::steady_clock::now() + timeout_);
    co_return wire::string_field(result, "statement_id");
}

void Transport::health() { execute(health_request()); }

std::string Transport::prepare(std::string_view sql) { return execute(prepare_request(sql)); }

asio::awaitable<void> Transport::shutdown()
{
    asio::error_code error;
    co_await stream_.async_shutdown(asio::cancel_after(500ms, asio::redirect_error(asio::use_awaitable, error)));
}

void Transport::disconnect() noexcept
{
    connected_.store(false);
    asio::error_code ignored;
    stream_.lowest_layer().close(ignored);
}

void Transport::close() noexcept
{
    active_.store(0);
    if (connected())
    {
        try { execute(shutdown()); }
        catch (...) {}
    }
    disconnect();
}
}
