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

void clear_credentials(Json& message) noexcept
{
    auto body = message.find("body");
    if (body == message.end() || !body->is_object()) return;
    for (const auto field : {"password", "old_password", "new_password"})
    {
        auto secret = body->find(field);
        if (secret == body->end()) continue;
        if (auto* bytes = secret->get_ptr<std::string*>())
            OPENSSL_cleanse(bytes->data(), bytes->size());
    }
}

struct ClearWireRequest
{
    Json& message;
    ~ClearWireRequest() { clear_credentials(message); }
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
    const auto history_reason = error.find("history_reason");
    if (history_reason != error.end() && (result.code == SDP_INVALID_STATE || result.code == SDP_RESOURCE_LIMIT))
    {
        if (*history_reason == "revision_not_recoverable")
            copy_text(result.message, "Historical revision is no longer recoverable (revision_not_recoverable)");
        else if (*history_reason == "retention_pinned")
            copy_text(result.message, "Historical retention is blocked by a pin (retention_pinned)");
        else if (*history_reason == "object_identity_changed")
            copy_text(result.message, "Historical object identity changed (object_identity_changed)");
        else if (*history_reason == "stale_preview")
            copy_text(result.message, "History preview is stale (stale_preview)");
        else if (*history_reason == "restore_conflict")
            copy_text(result.message, "Historical data conflicts with current data (restore_conflict)");
        else if (*history_reason == "schema_conflict")
            copy_text(result.message, "Historical schema conflicts with current schema (schema_conflict)");
        else if (*history_reason == "plan_unknown")
            copy_text(result.message, "History preview is unavailable (plan_unknown)");
        else if (*history_reason == "request_conflict")
            copy_text(result.message, "History request token conflicts with its prior use (request_conflict)");
    }
    if (result.code == SDP_INTERNAL_ERROR && history_reason != error.end() && *history_reason == "history_integrity")
        copy_text(result.message, "Historical integrity verification failed (history_integrity)");
    const auto decision_reason = error.find("decision_reason");
    if (decision_reason != error.end() && decision_reason->is_string())
    {
        static constexpr struct
        {
            sdp_status code;
            std::string_view reason;
            std::string_view message;
        } reasons[] = {
            {SDP_PERMISSION_DENIED, "denied", "Decision denied the proposed write (denied)"},
            {SDP_PERMISSION_DENIED, "unknown", "Decision condition was UNKNOWN (unknown)"},
            {SDP_MODULE_UNAVAILABLE, "module_unavailable", "Decision runtime is unavailable (module_unavailable)"},
            {SDP_INVALID_STATE, "policy_unavailable", "Decision policy is unavailable (policy_unavailable)"},
            {SDP_INVALID_STATE, "dependency_snapshot_unavailable", "Decision dependency snapshot is unavailable (dependency_snapshot_unavailable)"},
            {SDP_INVALID_STATE, "history_image_changed", "Decision would change the restored row image (history_image_changed)"},
        };
        for (const auto& entry : reasons)
            if (entry.code == result.code && decision_reason->get_ref<const std::string&>() == entry.reason)
            {
                copy_text(result.message, entry.message);
                break;
            }
    }
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
        Json{{"client", "StarDataParadiseDLLler/0.3.0"}}), deadline);
    session_ = wire::string_field(hello, "session_id");
    const auto protocol = hello.find("protocol_version");
    if (!hex_id(session_) || protocol == hello.end() || !protocol->is_number_integer() || *protocol != wire::version)
        throw Error(Code::incompatible);
    wire::string_field(hello, "product_version");

    auto auth_request = wire::request(next_id_++, "auth");
    const ClearWireRequest scrub{auth_request};
    auto& credentials = auth_request["body"];
    credentials["username"] = username;
    credentials["password"] = password;
    Json auth = co_await exchange_packet(auth_request, deadline, nullptr, true);
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

asio::awaitable<Json> Transport::exchange(Json packet, Deadline deadline, bool* dispatched)
{
    co_return co_await exchange_packet(packet, deadline, dispatched);
}

asio::awaitable<Json> Transport::exchange_packet(Json& packet, Deadline deadline, bool* dispatched,
    bool contains_credentials)
{
    try
    {
        const auto id = wire::request_id(packet);
        SecretBytes payload{wire::serialize(packet)};
        const auto header = wire::header_for(payload.bytes.size());
        const std::array<asio::const_buffer, 2> buffers{asio::buffer(header), asio::buffer(payload.bytes)};
        asio::error_code error;
        auto write_token = token(deadline, error);
        if (dispatched) *dispatched = true;
        co_await asio::async_write(stream_, buffers, std::move(write_token));
        OPENSSL_cleanse(payload.bytes.data(), payload.bytes.size());
        check(error);
        if (contains_credentials) clear_credentials(packet);

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

asio::awaitable<Json> Transport::request_operation(std::string_view op, Json body, bool& dispatched)
{
    if (!connected()) throw Error(Code::connection_lost);
    if (!next_id_) throw Error(Code::invalid_state);
    co_return co_await exchange(wire::request(next_id_++, op, std::move(body)),
        std::chrono::steady_clock::now() + timeout_, &dispatched);
}

asio::awaitable<void> Transport::password_request(const PasswordChange& change, bool& dispatched)
{
    if (!connected()) throw Error(Code::connection_lost);
    if (!next_id_) throw Error(Code::invalid_state);
    auto packet = wire::request(next_id_++, "password_set");
    const ClearWireRequest scrub{packet};
    auto& body = packet["body"];
    body["username"] = change.username;
    body["request_token"] = change.token;
    body["new_password"] = change.new_password;
    if (!change.old_password.empty()) body["old_password"] = change.old_password;
    (void)co_await exchange_packet(packet, std::chrono::steady_clock::now() + timeout_, &dispatched, true);
}

void Transport::password_set(const PasswordChange& change, bool& dispatched)
{
    execute(password_request(change, dispatched));
}

void Transport::health() { execute(health_request()); }

Json Transport::request(std::string_view op, Json body, bool& dispatched)
{
    return execute(request_operation(op, std::move(body), dispatched));
}

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
