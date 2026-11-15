#include "transport.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

namespace
{
using sdp::native::Endpoint;
using sdp::native::PasswordChange;
using sdp::native::RemoteError;
using sdp::native::Transport;
using sdp::wire::Code;
using sdp::wire::Error;
using sdp::wire::Json;

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

const Json& response_field(const Json& object, const char* key, Json::value_t type)
{
    const auto found = object.find(key);
    if (found == object.end() || found->type() != type) throw Error(Code::malformed);
    return *found;
}

void exact_fields(const Json& object, std::initializer_list<const char*> fields)
{
    if (object.size() != fields.size() || std::ranges::any_of(fields, [&](const auto key) { return !object.contains(key); }))
        throw Error(Code::malformed);
}

std::string_view numeric_text(const Json& item, const char* key, size_t limit)
{
    const auto& text = sdp::wire::string_field(item, key);
    const std::string_view value(text);
    const auto digits = value.starts_with('-') ? value.substr(1) : value;
    if (digits.empty() || digits.size() > limit || digits.find_first_not_of("0123456789") != digits.npos ||
        (digits.front() == '0' && value != "0"))
        throw Error(Code::malformed);
    return value;
}

void signed_i64(const Json& item, const char* key)
{
    const auto number = numeric_text(item, key, 19);
    int64_t parsed{};
    const auto end = number.data() + number.size();
    const auto result = std::from_chars(number.data(), end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) throw Error(Code::malformed);
}

void validate_cell(const Json& cell, size_t depth, size_t& nodes)
{
    if (++nodes > 2048) throw Error(Code::resource_limit);
    const auto& tag = sdp::wire::string_field(cell, "t");
    if (tag == "record" || tag == "collection")
    {
        if (depth >= 8) throw Error(Code::resource_limit);
        const auto children = tag == "record" ? "fields" : "items";
        exact_fields(cell, {"t", children});
        const auto& values = response_field(cell, children, Json::value_t::array);
        if (values.size() > 128) throw Error(Code::resource_limit);
        if (tag == "collection")
        {
            for (const auto& value : values) validate_cell(value, depth + 1, nodes);
        }
        else
        {
            std::set<std::string_view> names;
            for (const auto& item : values)
            {
                exact_fields(item, {"name", "value"});
                const auto& name = sdp::wire::string_field(item, "name");
                if (name.empty() || name.size() > 128 || name.find('\0') != name.npos || !names.insert(name).second)
                    throw Error(Code::malformed);
                validate_cell(item["value"], depth + 1, nodes);
            }
        }
        if (depth == 0 && cell.dump().size() > 64 * 1024) throw Error(Code::resource_limit);
        return;
    }
    if (tag == "null") exact_fields(cell, {"t"});
    else if (tag == "decimal")
    {
        exact_fields(cell, {"t", "unscaled", "scale"});
        numeric_text(cell, "unscaled", 38);
        if (response_field(cell, "scale", Json::value_t::number_unsigned) > 38) throw Error(Code::malformed);
    }
    else if (tag == "timestamp")
    {
        if (cell.contains("zone"))
        {
            exact_fields(cell, {"t", "seconds", "nanos", "offset_minutes", "zone"});
            sdp::wire::string_field(cell, "zone");
        }
        else exact_fields(cell, {"t", "seconds", "nanos", "offset_minutes"});
        signed_i64(cell, "seconds");
        if (response_field(cell, "nanos", Json::value_t::number_unsigned) > 999999999 ||
            !cell["offset_minutes"].is_number_integer() || cell["offset_minutes"] < -840 || cell["offset_minutes"] > 840)
            throw Error(Code::malformed);
    }
    else if (tag == "binary")
    {
        exact_fields(cell, {"t", "hex"});
        const auto& hex = sdp::wire::string_field(cell, "hex");
        if (hex.size() % 2 || hex.find_first_not_of("0123456789abcdef") != hex.npos) throw Error(Code::malformed);
    }
    else
    {
        exact_fields(cell, {"t", "v"});
        if (tag == "i64") signed_i64(cell, "v");
        else if (tag == "bool") response_field(cell, "v", Json::value_t::boolean);
        else if (tag == "text" || tag == "date" || tag == "time") sdp::wire::string_field(cell, "v");
        else throw Error(Code::malformed);
    }
}

void validate_cell(const Json& cell)
{
    size_t nodes{};
    validate_cell(cell, 0, nodes);
}

bool validate_debug_local(const Json& local)
{
    sdp::wire::string_field(local, "name");
    sdp::wire::string_field(local, "type");
    const auto available = response_field(local, "available", Json::value_t::boolean).get<bool>();
    if (available) validate_cell(sdp::wire::object_field(local, "value"));
    return available;
}

void validate_debug_status(const Json& status)
{
    const auto& state = sdp::wire::string_field(status, "state");
    if (state != "armed" && state != "running" && state != "paused" &&
        state != "finished" && state != "cancelled" && state != "timed_out")
        throw Error(Code::malformed);
    response_field(status, "revision", Json::value_t::number_unsigned);
    const auto& stack = response_field(status, "stack", Json::value_t::array);
    if (stack.size() > 32) throw Error(Code::resource_limit);
    for (const auto& frame : stack)
    {
        response_field(frame, "routine", Json::value_t::array);
        response_field(frame, "revision", Json::value_t::number_unsigned);
        response_field(frame, "line", Json::value_t::number_unsigned);
        response_field(frame, "column", Json::value_t::number_unsigned);
        const auto& locals = response_field(frame, "locals", Json::value_t::array);
        if (locals.size() > 128) throw Error(Code::resource_limit);
        for (const auto& local : locals) validate_debug_local(local);
    }
}

void validate_fixture_sets(const Json& sets)
{
    if (sets.size() > 32) throw Error(Code::resource_limit);
    size_t cells{};
    for (const auto& set : sets)
    {
        sdp::wire::string_field(set, "name");
        const auto& rows = response_field(set, "rows", Json::value_t::array);
        if (rows.size() > 4096) throw Error(Code::resource_limit);
        for (const auto& row : rows)
        {
            if (!row.is_array() || row.size() > 128) throw Error(Code::malformed);
            if (row.size() > 32768 - cells) throw Error(Code::resource_limit);
            cells += row.size();
            for (const auto& cell : row) validate_cell(cell);
        }
    }
}

std::string_view history_operation_name(sdp_history_operation operation)
{
    switch (operation)
    {
    case SDP_HISTORY_LOG: return "history_log";
    case SDP_HISTORY_DIFF: return "history_diff";
    case SDP_HISTORY_READ: return "history_read";
    case SDP_HISTORY_VERIFY: return "history_verify";
    case SDP_HISTORY_STATUS: return "history_status";
    case SDP_HISTORY_RETAIN: return "history_retain";
    case SDP_HISTORY_PURGE: return "history_purge";
    case SDP_HISTORY_PIN: return "history_pin";
    case SDP_HISTORY_UNPIN: return "history_unpin";
    case SDP_HISTORY_RESTORE: return "history_restore";
    case SDP_HISTORY_REVERT: return "history_revert";
    default: throw Error(Code::invalid_argument);
    }
}

void positive_history_id(const Json& item, const char* key)
{
    const auto value = numeric_text(item, key, 20);
    uint64_t parsed{};
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, parsed);
    if (!parsed || result.ec != std::errc{} || result.ptr != end) throw Error(Code::malformed);
}

void history_token(const Json& item, const char* key)
{
    const auto& value = sdp::wire::string_field(item, key);
    if (value.size() != 32 || value.find_first_not_of("0123456789abcdef") != value.npos)
        throw Error(Code::malformed);
}

void validate_history_plan_request(const Json& body)
{
    try
    {
        if (body.contains("plan_id") || body.contains("request_token"))
        {
            exact_fields(body, {"plan_id", "request_token"});
            history_token(body, "plan_id");
            history_token(body, "request_token");
        }
        else
        {
            if (!body.contains("target") || !body.contains("revision")) throw Error(Code::invalid_argument);
            for (auto it = body.cbegin(); it != body.cend(); ++it)
                if (it.key() != "target" && it.key() != "revision" && it.key() != "row_id" && it.key() != "field")
                    throw Error(Code::invalid_argument);
        }
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void validate_history_plan_response(const Json& response, sdp_history_operation operation, bool apply)
{
    if (apply)
    {
        if (sdp::wire::string_field(response, "status") != "committed") throw Error(Code::malformed);
        positive_history_id(response, "lsn");
        response_field(response, "replayed", Json::value_t::boolean);
        return;
    }
    history_token(response, "plan_id");
    positive_history_id(response, "current_revision");
    positive_history_id(response, "revision");
    positive_history_id(response, "object_id");
    response_field(response, "name", Json::value_t::array);
    response_field(response, "dependencies", Json::value_t::array);
    const auto& effects = sdp::wire::object_field(response, "effects");
    for (const auto key : {"inserted", "updated", "deleted", "catalog"})
        response_field(effects, key, Json::value_t::number_unsigned);
    const auto expected = operation == SDP_HISTORY_RESTORE ? "restore" : "revert";
    if (sdp::wire::string_field(response, "mode") != expected) throw Error(Code::malformed);
}

Json history_request(sdp_text input)
{
    if (!input.size) return Json::object();
    try
    {
        auto request = sdp::wire::parse_payload(checked_text(input, 64 * 1024));
        if (request.size() > 32) throw Error(Code::invalid_argument);
        for (const auto key : {"revision", "from", "to", "row_id", "quota_bytes"})
        {
            if (request.contains(key)) positive_history_id(request, key);
        }
        for (const auto key : {"target", "pin_kind", "pin_name", "field"})
        {
            if (!request.contains(key)) continue;
            const auto& value = sdp::wire::string_field(request, key);
            if (value.empty() || value.size() > 512 || value.find('\0') != value.npos)
                throw Error(Code::invalid_argument);
        }
        for (const auto key : {"offset", "limit"})
        {
            if (request.contains(key) && response_field(request, key, Json::value_t::number_unsigned) > UINT32_MAX)
                throw Error(Code::invalid_argument);
        }
        for (const auto key : {"since_utc", "until_utc"})
        {
            const auto when = request.find(key);
            if (when == request.end()) continue;
            if (!when->is_number_integer() || (when->is_number_unsigned() &&
                when->get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)))
                throw Error(Code::invalid_argument);
        }
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

Json named_parameters(sdp_text text, const char* discriminator)
{
    if (!text.size) return Json::object();
    try
    {
        auto object = sdp::wire::parse_payload(checked_text(text, sdp::wire::max_payload));
        if (object.size() > 64) throw Error(Code::invalid_argument);
        for (auto it = object.cbegin(); it != object.cend(); ++it)
        {
            if (it.key().empty() || it.key().size() > 128 || it.key().find('\0') != std::string::npos)
                throw Error(Code::invalid_argument);
            if (!it->is_object() || sdp::wire::string_field(*it, discriminator).empty())
                throw Error(Code::invalid_argument);
            if (std::string_view(discriminator) == "t") validate_cell(*it);
            else for (const auto& field : *it)
                if (field.is_structured()) throw Error(Code::invalid_argument);
        }
        return object;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

std::string_view checked_hex_id(sdp_text text)
{
    const auto value = checked_text(text, 32);
    if (value.size() != 32 || value.find_first_not_of("0123456789abcdef") != value.npos)
        throw Error(Code::invalid_argument);
    return value;
}

void check_output(const sdp_buffer* output)
{
    if (!output || output->data || output->size) throw Error(Code::invalid_argument);
}

void write_buffer(sdp_buffer& output, std::string_view bytes)
{
    auto data = std::make_unique<uint8_t[]>(bytes.size());
    std::memcpy(data.get(), bytes.data(), bytes.size());
    output = {data.release(), static_cast<uint32_t>(bytes.size())};
}

void write_json(sdp_buffer& output, const Json& value)
{
    write_buffer(output, sdp::wire::serialize(value));
}

const std::string& response_id(const Json& response, const char* key)
{
    const auto& id = sdp::wire::string_field(response, key);
    if (id.empty() || id.size() > 128 || id.find('\0') != id.npos) throw Error(Code::malformed);
    return id;
}

struct Reader
{
    std::string id;
    std::optional<Json> columns;
    std::string set_name;
    uint32_t set{};
    uint32_t next_batch{};
};

void validate_columns(const Json& columns)
{
    if (columns.size() > 128) throw Error(Code::malformed);
    for (const auto& column : columns)
    {
        const auto& name = sdp::wire::string_field(column, "name");
        const auto& type = sdp::wire::string_field(column, "type");
        if (name.empty() || type.empty()) throw Error(Code::malformed);
    }
}

bool check_batch(const Json& response, uint32_t requested, Reader& reader)
{
    const auto& batch = sdp::wire::object_field(response, "batch");
    const auto set = response_field(batch, "result_set", Json::value_t::number_unsigned).get<uint64_t>();
    const auto index = response_field(batch, "batch_index", Json::value_t::number_unsigned).get<uint64_t>();
    const auto& rows = response_field(batch, "rows", Json::value_t::array);
    const auto end_of_set = response_field(batch, "end_of_set", Json::value_t::boolean).get<bool>();
    const auto end_of_results = response_field(batch, "end_of_results", Json::value_t::boolean).get<bool>();

    if (set != reader.set || index != reader.next_batch || rows.size() > requested || (end_of_results && !end_of_set))
        throw Error(Code::malformed);
    if (!index || batch.contains("columns"))
    {
        const auto& columns = response_field(batch, "columns", Json::value_t::array);
        validate_columns(columns);
        if (reader.columns && columns != *reader.columns) throw Error(Code::malformed);
        if (!reader.columns) reader.columns = columns;
    }
    if (batch.contains("set_name"))
    {
        const auto& name = sdp::wire::string_field(batch, "set_name");
        if (name.empty() || name.size() > 128 || name.find('\0') != name.npos || (index && name != reader.set_name))
            throw Error(Code::malformed);
        if (!index) reader.set_name = name;
    }
    if (!reader.columns || (reader.set && reader.set_name.empty())) throw Error(Code::malformed);
    for (const auto& row : rows)
    {
        if (!row.is_array() || row.size() != reader.columns->size()) throw Error(Code::malformed);
        for (const auto& cell : row) validate_cell(cell);
    }

    if (end_of_results) return true;
    if (end_of_set)
    {
        if (reader.set_name.empty() || reader.set == 31) throw Error(Code::malformed);
        ++reader.set;
        reader.next_batch = 0;
        reader.columns.reset();
        reader.set_name.clear();
    }
    else
    {
        if (reader.next_batch == UINT32_MAX) throw Error(Code::malformed);
        ++reader.next_batch;
    }
    return false;
}

class Connection
{
    template<class Work>
    static auto report_unknown(bool may_mutate, Transport& transport, Work&& work)
    {
        bool dispatched = false;
        try { return work(dispatched); }
        catch (const Error& failure)
        {
            if (!may_mutate || !dispatched || transport.connected()) throw;
            sdp_error error{};
            set_error(&error, failure.code());
            error.unknown_outcome = 1;
            throw RemoteError{error};
        }
    }

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

    Json request(std::string_view op, Json body, bool may_mutate = false)
    {
        Json response;
        run([&] { response = exchange(op, std::move(body), may_mutate); });
        return response;
    }

    void execute_statement(Json body, bool may_mutate, sdp_buffer& output)
    {
        run([&] {
            if (reader_) throw Error(Code::invalid_state);
            auto response = exchange("execute", std::move(body), may_mutate);
            response_field(response, "affected_rows", Json::value_t::number_unsigned);
            if (response.contains("lsn")) response_id(response, "lsn");
            if (response.contains("result_id"))
            {
                Reader reader{};
                reader.id = response_id(response, "result_id");
                if (response.contains("columns"))
                {
                    const auto& columns = response_field(response, "columns", Json::value_t::array);
                    validate_columns(columns);
                    reader.columns = columns;
                }
                reader_ = std::move(reader);
            }
            write_json(output, response);
        });
    }

    void fetch(std::string_view id, uint32_t count, sdp_buffer& output)
    {
        run([&] {
            if (!reader_ || reader_->id != id) throw Error(Code::invalid_state);
            try
            {
                auto response = exchange("fetch", Json{{"result_id", id}, {"batch_rows", count}});
                if (check_batch(response, count, *reader_)) reader_.reset();
                write_json(output, response);
            }
            catch (...)
            {
                reader_.reset();
                throw;
            }
        });
    }

    void cancel_result(std::string_view id)
    {
        run([&] {
            exchange("result_cancel", Json{{"result_id", id}});
            reader_.reset();
        });
    }

    void rollback()
    {
        run([&] {
            exchange("tx_rollback", Json::object());
            reader_.reset();
        });
    }

    void reset_session()
    {
        run([&] {
            exchange("session_reset", Json::object());
            reader_.reset();
        });
    }

    void password_set(const PasswordChange& change)
    {
        run([&] {
            report_unknown(true, transport_, [&](bool& dispatched) { transport_.password_set(change, dispatched); });
        });
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
    Json exchange(std::string_view op, Json body, bool may_mutate = false)
    {
        return report_unknown(may_mutate, transport_, [&](bool& dispatched) {
            return transport_.request(op, std::move(body), dispatched);
        });
    }

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
    std::optional<Reader> reader_;
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

sdp_status empty_request(sdp_connection connection, const char* op, sdp_error* error)
{
    return boundary(error, [&] { require_connection(connection)->request(op, Json::object()); });
}

sdp_status id_request(sdp_connection connection, const char* op, const char* field, sdp_text id, sdp_error* error)
{
    return boundary(error, [&] {
        require_connection(connection)->request(op, Json{{field, checked_text(id, 128)}});
    });
}

sdp_status savepoint_request(sdp_connection connection, const char* op, sdp_text name, sdp_error* error)
{
    return boundary(error, [&] {
        const auto value = checked_text(name, 64);
        if (value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != value.npos)
            throw Error(Code::invalid_argument);
        require_connection(connection)->request(op, Json{{"name", value}});
    });
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
    return sdp_prepare_typed(connection, sql, {}, statement_id, error);
}

sdp_status SDP_CALL sdp_prepare_typed(sdp_connection connection, sdp_text sql, sdp_text parameter_types_json,
    sdp_buffer* statement_id, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(statement_id);
        Json body{{"sql", checked_text(sql, sdp::wire::max_payload)},
            {"parameter_types", named_parameters(parameter_types_json, "type")}};
        const auto response = require_connection(connection)->request("query.prepare", std::move(body));
        write_buffer(*statement_id, response_id(response, "statement_id"));
    });
}

sdp_status SDP_CALL sdp_execute(sdp_connection connection, sdp_text statement_id, sdp_text parameters_json,
    sdp_text token, uint32_t row_limit, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        if (!row_limit || row_limit > 1'000'000) throw Error(Code::invalid_argument);
        Json body{{"statement_id", checked_text(statement_id, 128)},
            {"parameters", named_parameters(parameters_json, "t")}, {"row_limit", row_limit}};
        if (token.size) body["request_token"] = checked_hex_id(token);
        require_connection(connection)->execute_statement(std::move(body), token.size != 0, *response_json);
    });
}

sdp_status SDP_CALL sdp_fetch(sdp_connection connection, sdp_text result_id, uint32_t batch_rows,
    sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        if (!batch_rows || batch_rows > 1024) throw Error(Code::invalid_argument);
        require_connection(connection)->fetch(checked_text(result_id, 128), batch_rows, *response_json);
    });
}

sdp_status SDP_CALL sdp_statement_close(sdp_connection connection, sdp_text statement_id, sdp_error* error)
{
    return id_request(connection, "statement_close", "statement_id", statement_id, error);
}

sdp_status SDP_CALL sdp_result_cancel(sdp_connection connection, sdp_text result_id, sdp_error* error)
{
    return boundary(error, [&] { require_connection(connection)->cancel_result(checked_text(result_id, 128)); });
}

sdp_status SDP_CALL sdp_metadata(sdp_connection connection, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        write_json(*response_json, require_connection(connection)->request("metadata", Json::object()));
    });
}

sdp_status SDP_CALL sdp_tx_begin(sdp_connection connection, sdp_error* error)
{
    return empty_request(connection, "tx_begin", error);
}

sdp_status SDP_CALL sdp_tx_commit(sdp_connection connection, sdp_text token, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto response = require_connection(connection)->request("tx_commit",
            Json{{"request_token", checked_hex_id(token)}}, true);
        response_id(response, "lsn");
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_tx_rollback(sdp_connection connection, sdp_error* error)
{
    return boundary(error, [&] { require_connection(connection)->rollback(); });
}

sdp_status SDP_CALL sdp_tx_savepoint(sdp_connection connection, sdp_text name, sdp_error* error)
{
    return savepoint_request(connection, "tx_savepoint", name, error);
}

sdp_status SDP_CALL sdp_tx_rollback_to(sdp_connection connection, sdp_text name, sdp_error* error)
{
    return savepoint_request(connection, "tx_rollback_to", name, error);
}

sdp_status SDP_CALL sdp_tx_release(sdp_connection connection, sdp_text name, sdp_error* error)
{
    return savepoint_request(connection, "tx_release", name, error);
}

sdp_status SDP_CALL sdp_tx_outcome(sdp_connection connection, sdp_text token, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto response = require_connection(connection)->request("tx_outcome",
            Json{{"request_token", checked_hex_id(token)}});
        const auto& status = sdp::wire::string_field(response, "status");
        if (status == "committed") response_id(response, "lsn");
        else if (status != "unknown") throw Error(Code::malformed);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_session_reset(sdp_connection connection, sdp_error* error)
{
    return boundary(error, [&] { require_connection(connection)->reset_session(); });
}

sdp_status SDP_CALL sdp_session_list(sdp_connection connection, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto response = require_connection(connection)->request("session_list", Json::object());
        response_field(response, "sessions", Json::value_t::array);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_query_cancel(sdp_connection connection, sdp_text session, uint32_t request_id, sdp_error* error)
{
    return boundary(error, [&] {
        Json body{{"session_id", checked_hex_id(session)}};
        if (request_id) body["request_id"] = request_id;
        const auto response = require_connection(connection)->request("query_cancel", std::move(body));
        if (!response_field(response, "accepted", Json::value_t::boolean).get<bool>())
            throw Error(Code::invalid_state);
    });
}

sdp_status SDP_CALL sdp_session_terminate(sdp_connection connection, sdp_text session, sdp_error* error)
{
    return boundary(error, [&] {
        require_connection(connection)->request("session_terminate", Json{{"session_id", checked_hex_id(session)}});
    });
}

sdp_status SDP_CALL sdp_password_set(sdp_connection connection, sdp_text username, sdp_text old_password,
    sdp_text new_password, sdp_text token, sdp_error* error)
{
    return boundary(error, [&] {
        const auto user = checked_text(username, 64);
        if (user.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != user.npos)
            throw Error(Code::invalid_argument);
        const PasswordChange change{user,
            old_password.size ? checked_text(old_password, 1024) : std::string_view{},
            checked_text(new_password, 1024), checked_hex_id(token)};
        require_connection(connection)->password_set(change);
    });
}

sdp_status SDP_CALL sdp_debug_open(sdp_connection connection, sdp_text target_session, sdp_text routine,
    uint32_t revision, sdp_buffer* debug_id, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(debug_id);
        if (!revision) throw Error(Code::invalid_argument);
        const auto response = require_connection(connection)->request("debug_open",
            Json{{"session_id", checked_hex_id(target_session)}, {"routine", checked_text(routine, 512)},
                {"revision", revision}});
        write_buffer(*debug_id, response_id(response, "debug_id"));
    });
}

sdp_status SDP_CALL sdp_debug_breakpoint(sdp_connection connection, sdp_text debug_id, uint32_t line,
    uint32_t revision, uint32_t enabled, sdp_error* error)
{
    return boundary(error, [&] {
        if (!line || !revision || enabled > 1) throw Error(Code::invalid_argument);
        require_connection(connection)->request("debug_breakpoint",
            Json{{"debug_id", checked_text(debug_id, 128)}, {"line", line}, {"revision", revision},
                {"enabled", enabled != 0}});
    });
}

sdp_status SDP_CALL sdp_debug_status(sdp_connection connection, sdp_text debug_id,
    sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto response = require_connection(connection)->request("debug_status",
            Json{{"debug_id", checked_text(debug_id, 128)}});
        validate_debug_status(response);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_debug_continue(sdp_connection connection, sdp_text debug_id, sdp_error* error)
{
    return id_request(connection, "debug_continue", "debug_id", debug_id, error);
}

sdp_status SDP_CALL sdp_debug_step(sdp_connection connection, sdp_text debug_id,
    sdp_debug_step_mode mode, sdp_error* error)
{
    return boundary(error, [&] {
        const char* spelling;
        switch (mode)
        {
        case SDP_STEP_INTO: spelling = "into"; break;
        case SDP_STEP_OVER: spelling = "over"; break;
        case SDP_STEP_OUT: spelling = "out"; break;
        default: throw Error(Code::invalid_argument);
        }
        require_connection(connection)->request("debug_step",
            Json{{"debug_id", checked_text(debug_id, 128)}, {"mode", spelling}});
    });
}

sdp_status SDP_CALL sdp_debug_watch(sdp_connection connection, sdp_text debug_id, sdp_text name,
    sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto response = require_connection(connection)->request("debug_watch",
            Json{{"debug_id", checked_text(debug_id, 128)}, {"name", checked_text(name, 128)}});
        if (!validate_debug_local(response)) throw Error(Code::malformed);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_debug_options(sdp_connection connection, sdp_text debug_id,
    uint32_t exception_stop, sdp_error* error)
{
    return boundary(error, [&] {
        if (exception_stop > 1) throw Error(Code::invalid_argument);
        require_connection(connection)->request("debug_options",
            Json{{"debug_id", checked_text(debug_id, 128)}, {"exception_stop", exception_stop != 0}});
    });
}

sdp_status SDP_CALL sdp_debug_close(sdp_connection connection, sdp_text debug_id, sdp_error* error)
{
    return id_request(connection, "debug_close", "debug_id", debug_id, error);
}

sdp_status SDP_CALL sdp_profile_last(sdp_connection connection, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto response = require_connection(connection)->request("profile_last", Json::object());
        const auto& samples = response_field(response, "samples", Json::value_t::array);
        for (const auto& sample : samples)
        {
            response_field(sample, "routine", Json::value_t::array);
            for (const auto name : {"revision", "inclusive_us", "exclusive_us", "sql_us", "instructions"})
                response_field(sample, name, Json::value_t::number_unsigned);
        }
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_routine_test(sdp_connection connection, sdp_text statement_id, sdp_text parameters_json,
    sdp_text expected_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        Json expected;
        try
        {
            expected = sdp::wire::parse_payload(checked_text(expected_json, 256 * 1024));
            if (expected.contains("exception"))
            {
                exact_fields(expected, {"exception"});
                const auto& exception = sdp::wire::string_field(expected, "exception");
                if (exception.empty() || exception.size() > 128) throw Error(Code::invalid_argument);
            }
            else
            {
                exact_fields(expected, {"sets"});
                validate_fixture_sets(response_field(expected, "sets", Json::value_t::array));
            }
        }
        catch (const Error&) { throw Error(Code::invalid_argument); }
        Json body{{"statement_id", checked_text(statement_id, 128)},
            {"parameters", named_parameters(parameters_json, "t")}, {"expected", std::move(expected)}};
        const auto response = require_connection(connection)->request("routine_test", std::move(body));
        response_field(response, "passed", Json::value_t::boolean);
        if (!response_field(response, "rolled_back", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        validate_fixture_sets(response_field(response, "sets", Json::value_t::array));
        const auto exception = response.find("exception");
        if (exception == response.end() || (!exception->is_null() && !exception->is_string()))
            throw Error(Code::malformed);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_history_command(sdp_connection connection, sdp_history_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto name = history_operation_name(operation);
        auto body = history_request(request_json);
        const bool plan = operation == SDP_HISTORY_RESTORE || operation == SDP_HISTORY_REVERT;
        if (plan) validate_history_plan_request(body);
        const bool apply = plan && body.contains("plan_id");
        const bool changes_policy = operation == SDP_HISTORY_RETAIN || operation == SDP_HISTORY_PURGE ||
            operation == SDP_HISTORY_PIN || operation == SDP_HISTORY_UNPIN;
        const auto response = require_connection(connection)->request(name, std::move(body), changes_policy || apply);
        if (plan) validate_history_plan_response(response, operation, apply);
        write_json(*response_json, response);
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
