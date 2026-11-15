#include <StarDataParadiseDLLler.h>
#include <windows.h>

#include <charconv>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
using namespace std::string_view_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

sdp_text text(std::string_view value)
{
    return {value.data(), static_cast<uint32_t>(value.size())};
}

void expect(sdp_status actual, sdp_status expected, const sdp_error& error)
{
    if (actual != expected || error.code != expected)
    {
        std::cerr << "Expected " << expected << ", got " << actual << " (" << error.sqlstate << ")\n";
        throw std::runtime_error("unexpected native status");
    }
}

class Handle
{
public:
    ~Handle() { sdp_close(value, nullptr); }
    sdp_connection value{};
};

struct Buffer
{
    sdp_buffer value{};
    ~Buffer() { sdp_buffer_free(&value); }
};

struct CompletionResult
{
    sdp_status status;
    sdp_status error;
    sdp_status close_in_callback;
};

struct Completion
{
    sdp_connection handle;
    std::promise<CompletionResult> done;
    std::promise<void> entered;
    std::shared_future<void> release;
};

void SDP_CALL complete(void* context, sdp_status status, const sdp_error* error)
{
    auto& state = *static_cast<Completion*>(context);
    const auto close_status = sdp_close(state.handle, nullptr);
    state.entered.set_value();
    if (state.release.valid()) state.release.wait();
    state.done.set_value({status, error->code, close_status});
}

struct Password
{
    std::string value;
    ~Password() { clear(); }

    void clear() noexcept
    {
        if (!value.empty()) SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
};

uint16_t port_from(std::string_view option)
{
    unsigned port{};
    const auto result = std::from_chars(option.data(), option.data() + option.size(), port);
    require(result.ec == std::errc{} && result.ptr == option.data() + option.size()
        && port && port <= 65535, "invalid argument port");
    return static_cast<uint16_t>(port);
}

void gate(std::string_view message)
{
    std::cout << message << std::endl;
    std::string command;
    require(static_cast<bool>(std::getline(std::cin, command)) && command == "go", "test gate closed");
}

struct CallbackGuard
{
    sdp_connection handle;
    std::promise<void>& release;
    bool released{};

    void unblock()
    {
        if (released) return;
        release.set_value();
        released = true;
    }

    ~CallbackGuard()
    {
        try { unblock(); }
        catch (...) {}
        sdp_close(handle, nullptr);
    }
};

void check_async(Handle& connection, std::string_view mode)
{
    Completion state{connection.value};
    auto done = state.done.get_future();
    auto entered = state.entered.get_future();

    std::promise<void> release;
    std::future<sdp_status> close_one;
    std::future<sdp_status> close_two;
    CallbackGuard guard{connection.value, release};
    if (mode == "callback_close") state.release = release.get_future().share();

    sdp_error error{};
    expect(sdp_health_async(connection.value, complete, &state, &error), SDP_OK, error);
    sdp_status required = SDP_OK;
    if (mode == "cancel")
    {
        gate("PENDING");
        expect(sdp_cancel(connection.value, &error), SDP_OK, error);
        required = SDP_CANCELLED;
    }
    if (mode == "operation_timeout") required = SDP_TIMEOUT;
    if (mode == "connection_loss") required = SDP_CONNECTION_LOST;

    if (mode == "callback_close")
    {
        if (entered.wait_for(5s) != std::future_status::ready)
        {
            guard.unblock();
            sdp_close(connection.value, nullptr);
            throw std::runtime_error("callback did not start");
        }
        close_one = std::async(std::launch::async, [&] { return sdp_close(connection.value, nullptr); });
        close_two = std::async(std::launch::async, [&] { return sdp_close(connection.value, nullptr); });
        const auto pending_one = close_one.wait_for(100ms);
        const auto pending_two = close_two.wait_for(100ms);
        guard.unblock();
        require(pending_one == std::future_status::timeout && pending_two == std::future_status::timeout,
            "concurrent close returned before callback");
        require(close_one.get() == SDP_OK && close_two.get() == SDP_OK, "concurrent close failed");
    }

    if (done.wait_for(10s) != std::future_status::ready)
    {
        sdp_close(connection.value, nullptr);
        throw std::runtime_error("async completion did not arrive");
    }
    const auto result = done.get();
    require(result.status == required && result.error == required, "async status mismatch");
    require(result.close_in_callback == SDP_INVALID_STATE, "close did not prevent self-join");
    if (required != SDP_OK)
        expect(sdp_health(connection.value, &error), SDP_CONNECTION_LOST, error);
    expect(sdp_close(connection.value, &error), SDP_OK, error);
    expect(sdp_close(connection.value, &error), SDP_OK, error);
    expect(sdp_health(connection.value, &error), SDP_INVALID_STATE, error);
}

int use_package(int argc, char** argv)
{
    require(argc == 4, "Usage: sdp_native_consumer mode port ca-file");
    const std::string_view mode(argv[1]);
    require(sdp_abi_version() == SDP_ABI_VERSION && sdp_library_version() == 0x00000100u,
        "library or ABI version mismatch");

    Password password;
    require(static_cast<bool>(std::getline(std::cin, password.value)), "missing password on stdin");
    sdp_open_options options{};
    options.struct_size = sizeof(options);
    options.abi_version = SDP_ABI_VERSION;
    options.address = text("127.0.0.1");
    auto server_name = "localhost"sv;
    if (mode == "tls_name") server_name = "other.invalid";
    if (mode == "tls_ip") server_name = "127.0.0.1";
    options.server_name = text(server_name);
    options.ca_file = text(argv[3]);
    options.username = text("local-admin");
    options.password = text(password.value);
    options.port = port_from(argv[2]);
    options.timeout_ms = mode == "handshake_timeout" || mode == "operation_timeout" ? 900 : 3500;

    Handle connection;
    sdp_error error{};
    if (mode == "invalid_abi")
    {
        ++options.abi_version;
        expect(sdp_open(&options, &connection.value, &error), SDP_INVALID_ARGUMENT, error);
        require(connection.value == 0, "rejected ABI created a handle");
        return 0;
    }

    const auto status = sdp_open(&options, &connection.value, &error);
    password.clear();
    if (mode == "tls_name" || mode == "tls_untrusted" || mode == "handshake_timeout" || mode == "auth_reject")
    {
        sdp_status expected = SDP_TLS_FAILED;
        if (mode == "handshake_timeout") expected = SDP_TIMEOUT;
        if (mode == "auth_reject") expected = SDP_AUTH_FAILED;
        expect(status, expected, error);
        require(connection.value == 0, "rejected connection created a handle");
        return 0;
    }
    expect(status, SDP_OK, error);

    if (mode == "real")
    {
        Buffer capabilities;
        expect(sdp_get_capabilities(connection.value, &capabilities.value, &error), SDP_OK, error);
        const std::string_view features(reinterpret_cast<const char*>(capabilities.value.data), capabilities.value.size);
        require(features.find("\"sql\":false") != features.npos && features.find("\"health\":true") != features.npos,
            "capability discovery did not match Server");
        sdp_buffer_free(&capabilities.value);
        require(!capabilities.value.data && !capabilities.value.size, "free did not clear the buffer");

        Buffer statement;
        expect(sdp_prepare(connection.value, text("SELECT 1"), &statement.value, &error), SDP_FEATURE_UNSUPPORTED, error);
        require(std::string_view(error.sqlstate) == "0A000" && error.correlation_id[0]
            && !error.retryable && !error.unknown_outcome && !statement.value.data,
            "SQL unavailable error did not match the wire contract");
        expect(sdp_health(connection.value, &error), SDP_OK, error);
    }

    if (mode == "cancel" || mode == "operation_timeout" || mode == "connection_loss") gate("CONNECTED");
    check_async(connection, mode);
    return 0;
}

int main(int argc, char** argv)
{
    try
    {
        const int exit_code = use_package(argc, argv);
        std::cout << argv[1] << " passed\n";
        return exit_code;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
