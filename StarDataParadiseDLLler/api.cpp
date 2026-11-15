#include "transport.hpp"

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
using sdp::native::Endpoint;
using sdp::native::RemoteError;
using sdp::native::Transport;
using sdp::wire::Code;
using sdp::wire::Error;

void set_error(sdp_error* error, Code code) noexcept
{
    if (!error) return;
    const auto description = sdp::wire::describe(code);
    error->code = static_cast<sdp_status>(code);
    std::memcpy(error->sqlstate, description.state, sizeof(error->sqlstate));
    const auto length = std::strlen(description.message);
    std::memcpy(error->message, description.message, length + 1);
}

template<class Work>
sdp_status boundary(sdp_error* error, Work&& work) noexcept
{
    if (error) *error = {};
    try
    {
        work();
        return SDP_OK;
    }
    catch (const RemoteError& result)
    {
        if (error) *error = result.details;
        return result.details.code;
    }
    catch (const Error& result)
    {
        set_error(error, result.code());
        return static_cast<sdp_status>(result.code());
    }
    catch (const std::bad_alloc&)
    {
        set_error(error, Code::out_of_memory);
        return SDP_OUT_OF_MEMORY;
    }
    catch (...)
    {
        set_error(error, Code::internal);
        return SDP_INTERNAL_ERROR;
    }
}

std::string_view checked_text(sdp_text text, uint32_t limit)
{
    if (!text.data || !text.size || text.size > limit) throw Error(Code::invalid_argument);
    const std::string_view value(text.data, text.size);
    if (value.find('\0') != value.npos) throw Error(Code::invalid_argument);
    return value;
}

std::filesystem::path utf8_path(std::string_view value)
{
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const auto c : value) utf8.push_back(static_cast<char8_t>(c));
    return std::filesystem::path(utf8);
}

void write_buffer(sdp_buffer& output, std::string_view bytes)
{
    auto data = std::make_unique<uint8_t[]>(bytes.size());
    std::memcpy(data.get(), bytes.data(), bytes.size());
    output = {data.release(), static_cast<uint32_t>(bytes.size())};
}

class Connection
{
public:
    explicit Connection(uint32_t timeout) : transport_(std::chrono::milliseconds(timeout)) {}

    void open(const Endpoint& endpoint, std::string_view user, std::string_view password)
    {
        capabilities_ = transport_.open(endpoint, user, password);
    }

    void capabilities(sdp_buffer& output)
    {
        std::lock_guard lock(mutex_);
        ensure_open();
        write_buffer(output, capabilities_);
    }

    void health()
    {
        run([this] { transport_.health(); });
    }

    void prepare(std::string_view sql, sdp_buffer& statement)
    {
        run([&] { write_buffer(statement, transport_.prepare(sql)); });
    }

    void health_async(sdp_completion completion, void* context)
    {
        std::lock_guard lock(mutex_);
        begin();
        try
        {
            worker_ = std::thread([this, completion, context] {
                sdp_error error{};
                const auto status = boundary(&error, [this] { transport_.health(); });
                {
                    std::lock_guard callback_lock(mutex_);
                    in_callback_ = true;
                }
                try { completion(context, status, &error); }
                catch (...) {}
                finish();
            });
        }
        catch (...)
        {
            busy_ = false;
            throw;
        }
    }

    void cancel()
    {
        std::lock_guard lock(mutex_);
        if (closed_ || !busy_ || in_callback_) throw Error(Code::invalid_state);
        transport_.cancel(operation_);
    }

    void close()
    {
        std::unique_lock lock(mutex_);
        if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id())
            throw Error(Code::invalid_state);
        closed_ = true;
        if (busy_ && !in_callback_)
        {
            try { transport_.cancel(operation_); }
            catch (...) {} // The request also has a deadline if posting cancellation fails.
        }
        idle_.wait(lock, [this] { return !busy_; });
        if (worker_.joinable()) worker_.join();
        transport_.close();
    }

private:
    void ensure_open() const
    {
        if (closed_) throw Error(Code::invalid_state);
        if (!transport_.connected()) throw Error(Code::connection_lost);
    }

    void begin()
    {
        ensure_open();
        if (busy_) throw Error(Code::invalid_state);
        if (worker_.joinable()) worker_.join();
        busy_ = true;
        transport_.begin(++operation_);
    }

    void finish()
    {
        std::lock_guard lock(mutex_);
        busy_ = false;
        in_callback_ = false;
        idle_.notify_all();
    }

    template<class Work>
    void run(Work&& work)
    {
        {
            std::lock_guard lock(mutex_);
            begin();
        }
        try { work(); }
        catch (...)
        {
            finish();
            throw;
        }
        finish();
    }

    std::mutex mutex_;
    std::condition_variable idle_;
    std::thread worker_;
    Transport transport_;
    std::string capabilities_;
    uint64_t operation_{};
    bool busy_{};
    bool closed_{};
    bool in_callback_{};
};

std::mutex registry_mutex;
std::unordered_map<sdp_connection, std::shared_ptr<Connection>> registry;
sdp_connection next_handle = 1;
constexpr size_t max_handles = 1024;

std::shared_ptr<Connection> find_connection(sdp_connection handle)
{
    std::lock_guard lock(registry_mutex);
    const auto match = registry.find(handle);
    return match == registry.end() ? nullptr : match->second;
}

std::shared_ptr<Connection> require_connection(sdp_connection handle)
{
    auto connection = find_connection(handle);
    if (!connection) throw Error(Code::invalid_state);
    return connection;
}

sdp_connection register_connection(std::shared_ptr<Connection> connection)
{
    std::lock_guard lock(registry_mutex);
    if (registry.size() >= max_handles || next_handle == 0) throw Error(Code::resource_limit);
    const auto handle = next_handle++;
    registry.emplace(handle, std::move(connection));
    return handle;
}
}

uint32_t SDP_CALL sdp_abi_version(void)
{
    return SDP_ABI_VERSION;
}

sdp_status SDP_CALL sdp_open(const sdp_open_options* options, sdp_connection* output, sdp_error* error)
{
    if (output) *output = 0;
    return boundary(error, [&] {
        if (!output || !options || options->struct_size != sizeof(*options) || options->abi_version != SDP_ABI_VERSION
            || options->reserved != 0 || options->port == 0 || options->timeout_ms < 100 || options->timeout_ms > 120000)
            throw Error(Code::invalid_argument);

        const auto address = checked_text(options->address, 64);
        const auto username = checked_text(options->username, 64);
        const auto password = checked_text(options->password, 1024);
        const auto name = checked_text(options->server_name, 253);
        const auto ca = checked_text(options->ca_file, 32768);
        asio::error_code address_error;
        const auto ip = asio::ip::make_address(address, address_error);
        if (address_error || ip.is_unspecified()) throw Error(Code::invalid_argument);
        const Endpoint endpoint{{ip, options->port}, std::string(name), utf8_path(ca)};

        auto connection = std::make_shared<Connection>(options->timeout_ms);
        connection->open(endpoint, username, password);
        *output = register_connection(std::move(connection));
    });
}

sdp_status SDP_CALL sdp_get_capabilities(sdp_connection connection, sdp_buffer* output, sdp_error* error)
{
    return boundary(error, [&] {
        if (!output || output->data || output->size) throw Error(Code::invalid_argument);
        require_connection(connection)->capabilities(*output);
    });
}

sdp_status SDP_CALL sdp_health(sdp_connection connection, sdp_error* error)
{
    return boundary(error, [&] { require_connection(connection)->health(); });
}

sdp_status SDP_CALL sdp_prepare(sdp_connection connection, sdp_text sql, sdp_buffer* statement_id, sdp_error* error)
{
    return boundary(error, [&] {
        if (!statement_id || statement_id->data || statement_id->size) throw Error(Code::invalid_argument);
        require_connection(connection)->prepare(checked_text(sql, sdp::wire::max_payload), *statement_id);
    });
}

sdp_status SDP_CALL sdp_health_async(sdp_connection connection, sdp_completion completion,
    void* context, sdp_error* error)
{
    return boundary(error, [&] {
        if (!completion) throw Error(Code::invalid_argument);
        require_connection(connection)->health_async(completion, context);
    });
}

sdp_status SDP_CALL sdp_cancel(sdp_connection connection, sdp_error* error)
{
    return boundary(error, [&] { require_connection(connection)->cancel(); });
}

sdp_status SDP_CALL sdp_close(sdp_connection connection, sdp_error* error)
{
    return boundary(error, [&] {
        const auto instance = find_connection(connection);
        if (!instance) return;
        instance->close();
        std::lock_guard lock(registry_mutex);
        registry.erase(connection);
    });
}

void SDP_CALL sdp_buffer_free(sdp_buffer* buffer)
{
    if (!buffer) return;
    delete[] buffer->data;
    *buffer = {};
}
