#include "transport.hpp"

#include <algorithm>
#include <array>
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

std::string_view decision_operation_name(sdp_decision_operation operation)
{
    switch (operation)
    {
    case SDP_DECISION_CREATE: return "decision_create";
    case SDP_DECISION_VALIDATE: return "decision_validate";
    case SDP_DECISION_ACTIVATE: return "decision_activate";
    case SDP_DECISION_TEST: return "decision_test";
    case SDP_DECISION_EXPLAIN: return "decision_explain";
    case SDP_DECISION_WHYNOT: return "decision_whynot";
    case SDP_DECISION_WHATIF: return "decision_whatif";
    case SDP_DECISION_RECHECK: return "decision_recheck";
    default: throw Error(Code::invalid_argument);
    }
}

void decision_text(const Json& request, const char* key, size_t limit)
{
    const auto& value = sdp::wire::string_field(request, key);
    if (value.empty() || value.size() > limit || value.find('\0') != value.npos)
        throw Error(Code::invalid_argument);
}

void decision_scenario(const Json& request, bool standalone)
{
    if (!request.is_object()) throw Error(Code::invalid_argument);
    if (standalone)
    {
        decision_text(request, "target", 512);
        if (request.contains("revision")) positive_history_id(request, "revision");
    }
    for (auto field = request.cbegin(); field != request.cend(); ++field)
        if (field.key() != "event" && field.key() != "row_id" && field.key() != "proposed" &&
            !(standalone && (field.key() == "target" || field.key() == "revision")))
            throw Error(Code::invalid_argument);
    const auto& event = sdp::wire::string_field(request, "event");
    if (event != "insert" && event != "update" && event != "delete")
        throw Error(Code::invalid_argument);
    if (request.contains("row_id")) positive_history_id(request, "row_id");
    if (request.contains("proposed"))
    {
        const auto& proposed = sdp::wire::object_field(request, "proposed");
        if (proposed.size() > 128) throw Error(Code::invalid_argument);
        size_t nodes{};
        for (auto field = proposed.cbegin(); field != proposed.cend(); ++field)
        {
            if (field.key().empty() || field.key().size() > 128 || field.key().find('\0') != std::string::npos)
                throw Error(Code::invalid_argument);
            validate_cell(*field, 0, nodes);
        }
    }
}

Json decision_request(sdp_decision_operation operation, sdp_text input)
{
    try
    {
        auto request = sdp::wire::parse_payload(checked_text(input, 64 * 1024));
        if (operation == SDP_DECISION_CREATE || operation == SDP_DECISION_ACTIVATE || operation == SDP_DECISION_VALIDATE)
        {
            if (operation == SDP_DECISION_VALIDATE) exact_fields(request, {"sql"});
            else
            {
                exact_fields(request, {"sql", "request_token"});
                history_token(request, "request_token");
            }
            decision_text(request, "sql", 32 * 1024);
        }
        else if (operation == SDP_DECISION_TEST)
        {
            if (request.contains("revision"))
            {
                exact_fields(request, {"target", "rule", "from_version", "to_version", "cases", "revision"});
                positive_history_id(request, "revision");
            }
            else exact_fields(request, {"target", "rule", "from_version", "to_version", "cases"});
            decision_text(request, "target", 512);
            decision_text(request, "rule", 512);
            for (const auto key : {"from_version", "to_version"})
            {
                const auto& version = response_field(request, key, Json::value_t::number_unsigned);
                if (version == 0 || version > UINT32_MAX) throw Error(Code::invalid_argument);
            }
            const auto& cases = response_field(request, "cases", Json::value_t::array);
            if (cases.empty() || cases.size() > 16) throw Error(Code::invalid_argument);
            for (const auto& item : cases) decision_scenario(item, false);
        }
        else decision_scenario(request, true);
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void check_decision_result(const Json& response)
{
    positive_history_id(response, "revision");
    response_field(response, "allowed", Json::value_t::boolean);
    sdp::wire::string_field(response, "reason");
    response_field(response, "rules", Json::value_t::array);
    const auto& computed = sdp::wire::object_field(response, "computed");
    if (computed.size() > 128) throw Error(Code::malformed);
    size_t nodes{};
    for (const auto& value : computed) validate_cell(value, 0, nodes);
}

void check_decision_response(sdp_decision_operation operation, const Json& response)
{
    switch (operation)
    {
    case SDP_DECISION_CREATE:
    case SDP_DECISION_ACTIVATE:
        response_field(response, "affected_rows", Json::value_t::number_unsigned);
        positive_history_id(response, "lsn");
        break;
    case SDP_DECISION_VALIDATE:
        if (response_field(response, "valid", Json::value_t::boolean) != true) throw Error(Code::malformed);
        break;
    case SDP_DECISION_TEST:
    {
        positive_history_id(response, "revision");
        const auto& changed = response_field(response, "changed", Json::value_t::number_unsigned);
        const auto& cases = response_field(response, "cases", Json::value_t::array);
        if (cases.empty() || cases.size() > 16 || changed > cases.size()) throw Error(Code::malformed);
        for (const auto& item : cases)
        {
            check_decision_result(sdp::wire::object_field(item, "prior"));
            check_decision_result(sdp::wire::object_field(item, "candidate"));
        }
        break;
    }
    default:
        check_decision_result(response);
        break;
    }
}

std::string_view job_operation_name(sdp_job_operation operation)
{
    switch (operation)
    {
    case SDP_JOB_SUBMIT: return "job_submit";
    case SDP_JOB_STATUS: return "job_status";
    case SDP_JOB_WATCH: return "job_watch";
    case SDP_JOB_CANCEL: return "job_cancel";
    case SDP_JOB_SCHEDULE_CREATE: return "job_schedule_create";
    case SDP_JOB_SCHEDULE_STATUS: return "job_schedule_status";
    case SDP_JOB_SCHEDULE_CANCEL: return "job_schedule_cancel";
    case SDP_JOB_CAPABILITIES: return "job_capabilities";
    default: throw Error(Code::invalid_argument);
    }
}

void job_unsigned(const Json& object, const char* key, uint64_t minimum, uint64_t maximum = UINT64_MAX)
{
    const auto& value = response_field(object, key, Json::value_t::number_unsigned);
    if (value < minimum || value > maximum) throw Error(Code::invalid_argument);
}

void job_misfire(const Json& object)
{
    const auto& value = sdp::wire::string_field(object, "misfire");
    if (value != "skip" && value != "run_once") throw Error(Code::invalid_argument);
}

void schedule_rule(const Json& rule)
{
    const auto extra = rule.contains("misfire") ? 1u : 0u;
    if (extra) job_misfire(rule);
    if (rule.contains("every_seconds"))
    {
        if (rule.size() != 1 + extra) throw Error(Code::invalid_argument);
        job_unsigned(rule, "every_seconds", 1, 31'536'000);
    }
    else
    {
        if (rule.size() != 3 + extra) throw Error(Code::invalid_argument);
        decision_text(rule, "timezone", 128);
        job_unsigned(rule, "hour", 0, 23);
        job_unsigned(rule, "minute", 0, 59);
    }
}

void job_submission(sdp_job_operation operation, const Json& request)
{
    const bool schedule = operation == SDP_JOB_SCHEDULE_CREATE;
    history_token(request, "request_token");
    const auto& kind = sdp::wire::string_field(request, "kind");
    const bool backup = schedule && kind == "backup";
    if (backup)
    {
        if (request.contains("max_bytes")) job_unsigned(request, "max_bytes", 1, 536870912);
    }
    else
    {
        if (kind != "compute") throw Error(Code::invalid_argument);
        job_unsigned(request, "count", 1, 1'000'000'000);
    }
    if (schedule)
    {
        const auto& rule = sdp::wire::object_field(request, "rule");
        if (rule.contains("misfire") && request.contains("misfire")) throw Error(Code::invalid_argument);
        schedule_rule(rule);
    }
    for (auto field = request.cbegin(); field != request.cend(); ++field)
    {
        const auto& key = field.key();
        if (key == "request_token" || key == "kind" || (schedule && key == "rule") ||
            (backup ? key == "max_bytes" : key == "count"))
            continue;
        if (!backup && key == "timeout_ms") job_unsigned(request, "timeout_ms", 100, 60'000);
        else if (!backup && key == "memory_mib") job_unsigned(request, "memory_mib", 16, 512);
        else if (!backup && key == "output_bytes") job_unsigned(request, "output_bytes", 1, 65'536);
        else if (schedule && key == "retries") job_unsigned(request, "retries", 0, 3);
        else if (schedule && key == "backoff_seconds") job_unsigned(request, "backoff_seconds", 1, 3600);
        else if (schedule && key == "misfire") job_misfire(request);
        else throw Error(Code::invalid_argument);
    }
}

Json job_request(sdp_job_operation operation, sdp_text input)
{
    try
    {
        auto request = input.size ? sdp::wire::parse_payload(checked_text(input, 64 * 1024)) : Json::object();
        switch (operation)
        {
        case SDP_JOB_SUBMIT:
        case SDP_JOB_SCHEDULE_CREATE:
            job_submission(operation, request);
            break;
        case SDP_JOB_SCHEDULE_STATUS:
        case SDP_JOB_SCHEDULE_CANCEL:
            exact_fields(request, {"schedule_id"});
            history_token(request, "schedule_id");
            break;
        case SDP_JOB_STATUS:
        case SDP_JOB_WATCH:
        case SDP_JOB_CANCEL:
            exact_fields(request, {"job_id"});
            history_token(request, "job_id");
            break;
        case SDP_JOB_CAPABILITIES:
            exact_fields(request, {});
            break;
        }
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void check_job_response(sdp_job_operation operation, const Json& response)
{
    switch (operation)
    {
    case SDP_JOB_SUBMIT:
    case SDP_JOB_STATUS:
    case SDP_JOB_WATCH:
    case SDP_JOB_CANCEL:
    {
        history_token(response, "job_id");
        const auto& state = sdp::wire::string_field(response, "state");
        if (state.empty()) throw Error(Code::malformed);
        response_field(response, "attempt", Json::value_t::number_unsigned);
        response_field(response, "cancel_requested", Json::value_t::boolean);
        response_field(response, "events", Json::value_t::array);
        const auto progress = response.find("progress");
        const auto result = response.find("result");
        if (result == response.end() || (!result->is_null() && !result->is_object()) || progress == response.end())
            throw Error(Code::malformed);
        if (!progress->is_null())
        {
            if (!progress->is_object()) throw Error(Code::malformed);
            const auto& done = response_field(*progress, "done", Json::value_t::number_unsigned);
            const auto& total = response_field(*progress, "total", Json::value_t::number_unsigned);
            if (total == 0 || done > total) throw Error(Code::malformed);
        }
        if (response.contains("lsn")) positive_history_id(response, "lsn");
        break;
    }
    case SDP_JOB_SCHEDULE_CREATE:
    case SDP_JOB_SCHEDULE_STATUS:
    case SDP_JOB_SCHEDULE_CANCEL:
        history_token(response, "schedule_id");
        response_field(response, "enabled", Json::value_t::boolean);
        if (response.contains("lsn")) positive_history_id(response, "lsn");
        break;
    }
}

struct WireCommand
{
    std::string_view name;
    bool writes;
};

constexpr std::array dataset_commands{
    WireCommand{"dataset_create", true},
    WireCommand{"dataset_get", false},
    WireCommand{"dataset_delete", true},
    WireCommand{"dataset_plsql_submit", true}};
constexpr std::array model_commands{
    WireCommand{"model_create", true},
    WireCommand{"model_status", false},
    WireCommand{"model_train", true},
    WireCommand{"model_evaluate", false},
    WireCommand{"model_predict", false},
    WireCommand{"model_activate", true},
    WireCommand{"model_compare", false},
    WireCommand{"model_checkpoint", false},
    WireCommand{"model_resume", true},
    WireCommand{"model_deactivate", true},
    WireCommand{"model_rollback", true},
    WireCommand{"model_delete", true},
    WireCommand{"model_devices", false}};
static_assert(dataset_commands.size() == SDP_DATASET_PLSQL_SUBMIT);
static_assert(model_commands.size() == SDP_MODEL_DEVICES);

template<size_t Count>
const WireCommand& wire_command_spec(uint32_t operation, const std::array<WireCommand, Count>& commands)
{
    if (!operation || operation > commands.size()) throw Error(Code::invalid_argument);
    return commands[operation - 1];
}

void only_fields(const Json& request, std::initializer_list<std::string_view> allowed)
{
    for (const auto& item : request.items())
        if (std::ranges::find(allowed, item.key()) == allowed.end()) throw Error(Code::invalid_argument);
}

Json dataset_request(sdp_dataset_operation operation, sdp_text input)
{
    try
    {
        auto request = input.size ? sdp::wire::parse_payload(checked_text(input, 64 * 1024)) : Json::object();
        switch (operation)
        {
        case SDP_DATASET_CREATE:
            only_fields(request, {"request_token", "sql", "options", "parameter_types", "parameters"});
            history_token(request, "request_token");
            decision_text(request, "sql", 64 * 1024);
            (void)sdp::wire::object_field(request, "options");
            for (const auto key : {"parameter_types", "parameters"})
                if (request.contains(key)) (void)sdp::wire::object_field(request, key);
            break;
        case SDP_DATASET_GET:
        case SDP_DATASET_DELETE:
            exact_fields(request, {"dataset_id"});
            history_token(request, "dataset_id");
            break;
        case SDP_DATASET_PLSQL_SUBMIT:
        {
            exact_fields(request, {"request_token", "dataset_id", "routine", "indices", "arguments"});
            history_token(request, "request_token");
            history_token(request, "dataset_id");
            decision_text(request, "routine", 1024);
            const auto& indices = response_field(request, "indices", Json::value_t::array);
            if (indices.empty() || indices.size() > 64) throw Error(Code::invalid_argument);
            for (const auto& index : indices)
                if (!index.is_number_unsigned() || index.get<uint64_t>() >= 1024) throw Error(Code::invalid_argument);
            const auto& arguments = sdp::wire::object_field(request, "arguments");
            if (arguments.size() > 32) throw Error(Code::invalid_argument);
            for (const auto& item : arguments.items())
                if (!item.value().is_string()) throw Error(Code::invalid_argument);
            break;
        }
        }
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void model_comparison(const Json& body)
{
    exact_fields(body, {"runs", "split"});
    const auto& split = sdp::wire::string_field(body, "split");
    if (split != "train" && split != "validation" && split != "test") throw Error(Code::invalid_argument);
    const auto& runs = response_field(body, "runs", Json::value_t::array);
    if (runs.size() < 2 || runs.size() > 8) throw Error(Code::invalid_argument);
    for (const auto& run : runs)
    {
        exact_fields(run, {"name", "version"});
        decision_text(run, "name", 1024);
        job_unsigned(run, "version", 1, UINT32_MAX);
    }
}

void sha256_field(const Json& body, const char* key)
{
    const auto& checksum = sdp::wire::string_field(body, key);
    if (checksum.size() != 64 || checksum.find_first_not_of("0123456789abcdef") != checksum.npos)
        throw Error(Code::malformed);
}

void model_device_options(const Json& request)
{
    if (request.contains("device_index")) job_unsigned(request, "device_index", 0, 31);
    if (request.contains("memory_mib")) job_unsigned(request, "memory_mib", 1, 131072);
}

void model_training_device(const Json& request)
{
    const auto device = request.contains("device") ?
        std::string_view(sdp::wire::string_field(request, "device")) : "cpu";
    if (device == "cpu")
    {
        if (request.contains("device_index") || request.contains("memory_mib") || request.contains("allow_cpu_fallback"))
            throw Error(Code::invalid_argument);
        return;
    }
    if (device != "gpu") throw Error(Code::invalid_argument);
    model_device_options(request);
    if (request.contains("allow_cpu_fallback"))
        response_field(request, "allow_cpu_fallback", Json::value_t::boolean);
}

Json model_request(sdp_model_operation operation, sdp_text input)
{
    try
    {
        auto request = input.size ? sdp::wire::parse_payload(checked_text(input, 64 * 1024)) : Json::object();
        if (operation == SDP_MODEL_COMPARE)
        {
            model_comparison(request);
            return request;
        }
        if (operation == SDP_MODEL_DEVICES)
        {
            only_fields(request, {"device_index", "memory_mib"});
            model_device_options(request);
            return request;
        }
        decision_text(request, "name", 1024);
        switch (operation)
        {
        case SDP_MODEL_CREATE:
            exact_fields(request, {"name", "algorithm", "request_token"});
            decision_text(request, "algorithm", 32);
            history_token(request, "request_token");
            break;
        case SDP_MODEL_STATUS:
            exact_fields(request, {"name"});
            break;
        case SDP_MODEL_TRAIN:
            only_fields(request, {"name", "dataset_id", "request_token", "seed", "options",
                "pause_after_epoch", "device", "device_index", "memory_mib", "allow_cpu_fallback"});
            history_token(request, "dataset_id");
            history_token(request, "request_token");
            if (request.contains("seed")) job_unsigned(request, "seed", 0);
            if (request.contains("options")) (void)sdp::wire::object_field(request, "options");
            if (request.contains("pause_after_epoch")) job_unsigned(request, "pause_after_epoch", 1, 1000);
            model_training_device(request);
            break;
        case SDP_MODEL_EVALUATE:
            only_fields(request, {"name", "version"});
            if (request.contains("version")) job_unsigned(request, "version", 1, UINT32_MAX);
            break;
        case SDP_MODEL_PREDICT:
            exact_fields(request, {"name", "features"});
            (void)sdp::wire::object_field(request, "features");
            break;
        case SDP_MODEL_ACTIVATE:
        case SDP_MODEL_ROLLBACK:
        case SDP_MODEL_DELETE:
            exact_fields(request, {"name", "version", "request_token"});
            job_unsigned(request, "version", 1, UINT32_MAX);
            history_token(request, "request_token");
            break;
        case SDP_MODEL_CHECKPOINT:
            exact_fields(request, {"name", "job_id"});
            history_token(request, "job_id");
            break;
        case SDP_MODEL_RESUME:
            exact_fields(request, {"name", "job_id", "checksum", "request_token"});
            history_token(request, "job_id");
            sha256_field(request, "checksum");
            history_token(request, "request_token");
            break;
        case SDP_MODEL_DEACTIVATE:
            exact_fields(request, {"name", "request_token"});
            history_token(request, "request_token");
            break;
        }
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void check_dataset_response(sdp_dataset_operation operation, const Json& response)
{
    if (operation == SDP_DATASET_PLSQL_SUBMIT)
    {
        check_job_response(SDP_JOB_SUBMIT, response);
        return;
    }
    if (operation == SDP_DATASET_DELETE)
    {
        if (!response_field(response, "deleted", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        positive_history_id(response, "lsn");
        return;
    }
    history_token(response, "dataset_id");
    decision_text(response, "owner", 128);
    positive_history_id(response, "revision");
    response_field(response, "row_count", Json::value_t::number_unsigned);
    response_field(response, "byte_count", Json::value_t::number_unsigned);
    if (operation == SDP_DATASET_GET)
    {
        (void)sdp::wire::object_field(response, "source");
        (void)sdp::wire::object_field(response, "prepared");
    }
    if (response.contains("lsn")) positive_history_id(response, "lsn");
}

void check_model_response(sdp_model_operation operation, const Json& response)
{
    switch (operation)
    {
    case SDP_MODEL_CREATE:
    case SDP_MODEL_STATUS:
        decision_text(response, "name", 1024);
        positive_history_id(response, "object_id");
        decision_text(response, "algorithm", 32);
        response_field(response, "versions", Json::value_t::array);
        if (!response.contains("active_version") ||
            !(response["active_version"].is_null() || response["active_version"].is_number_unsigned()))
            throw Error(Code::malformed);
        break;
    case SDP_MODEL_TRAIN:
    case SDP_MODEL_RESUME:
        check_job_response(SDP_JOB_SUBMIT, response);
        break;
    case SDP_MODEL_EVALUATE:
        response_field(response, "version", Json::value_t::number_unsigned);
        (void)sdp::wire::object_field(response, "artifact");
        (void)sdp::wire::object_field(response, "metrics");
        break;
    case SDP_MODEL_PREDICT:
        response_field(response, "version", Json::value_t::number_unsigned);
        if (!response.contains("prediction") ||
            !(response["prediction"].is_number() || response["prediction"].is_array()))
            throw Error(Code::malformed);
        break;
    case SDP_MODEL_ACTIVATE:
    case SDP_MODEL_ROLLBACK:
        response_field(response, "version", Json::value_t::number_unsigned);
        response_field(response, "active_version", Json::value_t::number_unsigned);
        positive_history_id(response, "lsn");
        break;
    case SDP_MODEL_DEACTIVATE:
        response_field(response, "active_version", Json::value_t::null);
        positive_history_id(response, "lsn");
        break;
    case SDP_MODEL_DELETE:
        response_field(response, "version", Json::value_t::number_unsigned);
        if (!response_field(response, "deleted", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        positive_history_id(response, "lsn");
        break;
    case SDP_MODEL_CHECKPOINT:
        history_token(response, "job_id");
        history_token(response, "dataset_id");
        sha256_field(response, "checksum");
        response_field(response, "epoch", Json::value_t::number_unsigned);
        break;
    case SDP_MODEL_DEVICES:
        response_field(response, "cpu_available", Json::value_t::boolean);
        response_field(response, "gpu_available", Json::value_t::boolean);
        response_field(response, "gpu_verified", Json::value_t::boolean);
        response_field(response, "devices", Json::value_t::array);
        response_field(response, "device_index", Json::value_t::number_unsigned);
        response_field(response, "memory_mib", Json::value_t::number_unsigned);
        if (!response.contains("driver_version") ||
            !(response["driver_version"].is_null() || response["driver_version"].is_number_unsigned()))
            throw Error(Code::malformed);
        break;
    case SDP_MODEL_COMPARE:
        response_field(response, "comparable", Json::value_t::boolean);
        response_field(response, "runs", Json::value_t::array);
        break;
    }
    if (response.contains("lsn")) positive_history_id(response, "lsn");
}

constexpr std::array backup_commands{
    WireCommand{"backup_submit", true},
    WireCommand{"backup_status", false},
    WireCommand{"backup_cancel", true},
    WireCommand{"backup_catalog", false},
    WireCommand{"backup_verify", true},
    WireCommand{"backup_restore", true},
    WireCommand{"backup_delete", true}};
static_assert(backup_commands.size() == SDP_BACKUP_DELETE);

void instance_target_name(const Json& body)
{
    decision_text(body, "target_name", 32);
    const auto& name = sdp::wire::string_field(body, "target_name");
    constexpr std::string_view label_characters = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    if (name.find_first_not_of(label_characters) != name.npos) throw Error(Code::invalid_argument);
}

Json backup_request(sdp_backup_operation operation, sdp_text input)
{
    try
    {
        auto request = input.size ? sdp::wire::parse_payload(checked_text(input, 64 * 1024)) : Json::object();
        switch (operation)
        {
        case SDP_BACKUP_SUBMIT:
            only_fields(request, {"request_token", "max_bytes"});
            history_token(request, "request_token");
            if (request.contains("max_bytes")) job_unsigned(request, "max_bytes", 1, 536870912);
            break;
        case SDP_BACKUP_STATUS:
        case SDP_BACKUP_CANCEL:
            exact_fields(request, {"job_id"});
            history_token(request, "job_id");
            break;
        case SDP_BACKUP_CATALOG:
            exact_fields(request, {});
            break;
        case SDP_BACKUP_VERIFY:
        case SDP_BACKUP_DELETE:
            exact_fields(request, {"backup_id"});
            history_token(request, "backup_id");
            break;
        case SDP_BACKUP_RESTORE:
            exact_fields(request, {"backup_id", "target_name"});
            history_token(request, "backup_id");
            instance_target_name(request);
            break;
        }
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void check_backup_summary(const Json& response)
{
    history_token(response, "backup_id");
    response_field(response, "lsn", Json::value_t::number_unsigned);
    response_field(response, "verified", Json::value_t::boolean);
    sha256_field(response, "manifest_sha256");
}

void check_backup_response(sdp_backup_operation operation, const Json& response)
{
    switch (operation)
    {
    case SDP_BACKUP_SUBMIT:
    case SDP_BACKUP_STATUS:
    case SDP_BACKUP_CANCEL:
        check_job_response(SDP_JOB_STATUS, response);
        break;
    case SDP_BACKUP_CATALOG:
        response_field(response, "pitr", Json::value_t::boolean);
        for (const auto& entry : response_field(response, "backups", Json::value_t::array))
            check_backup_summary(entry);
        break;
    case SDP_BACKUP_VERIFY:
        check_backup_summary(response);
        if (!response_field(response, "verified", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        break;
    case SDP_BACKUP_RESTORE:
        check_backup_summary(response);
        decision_text(response, "target_name", 32);
        if (!response_field(response, "restored", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        break;
    case SDP_BACKUP_DELETE:
        history_token(response, "backup_id");
        if (!response_field(response, "deleted", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        break;
    }
}

constexpr std::array admin_commands{
    WireCommand{"admin_status", false},
    WireCommand{"admin_health", false},
    WireCommand{"admin_health_check", false},
    WireCommand{"admin_config_preview", false},
    WireCommand{"admin_config_apply", true},
    WireCommand{"admin_support_preview", false},
    WireCommand{"admin_support_create", true},
    WireCommand{"admin_maintenance_submit", true}};
static_assert(admin_commands.size() == SDP_ADMIN_MAINTENANCE_SUBMIT);

constexpr std::array<std::string_view, 5> health_checks{"storage", "history", "backup", "certificate", "jobs"};

Json admin_request(sdp_admin_operation operation, sdp_text input)
{
    try
    {
        auto request = input.size ? sdp::wire::parse_payload(checked_text(input, 64 * 1024)) : Json::object();
        switch (operation)
        {
        case SDP_ADMIN_STATUS:
        case SDP_ADMIN_HEALTH:
        case SDP_ADMIN_SUPPORT_PREVIEW:
            exact_fields(request, {});
            break;
        case SDP_ADMIN_HEALTH_CHECK:
        {
            exact_fields(request, {"check"});
            const auto& name = sdp::wire::string_field(request, "check");
            if (std::ranges::find(health_checks, name) == health_checks.end())
                throw Error(Code::invalid_argument);
            break;
        }
        case SDP_ADMIN_CONFIG_PREVIEW:
        case SDP_ADMIN_CONFIG_APPLY:
        {
            exact_fields(request, {"config"});
            const auto& config = sdp::wire::object_field(request, "config");
            if (config.empty()) throw Error(Code::invalid_argument);
            only_fields(config, {"timeout_ms", "port", "max_connections"});
            if (config.contains("timeout_ms")) job_unsigned(config, "timeout_ms", 100, 120000);
            if (config.contains("port")) job_unsigned(config, "port", 0, 65535);
            if (config.contains("max_connections")) job_unsigned(config, "max_connections", 1, 128);
            break;
        }
        case SDP_ADMIN_SUPPORT_CREATE:
            exact_fields(request, {"target_name"});
            instance_target_name(request);
            break;
        case SDP_ADMIN_MAINTENANCE_SUBMIT:
        {
            exact_fields(request, {"request_token", "action"});
            history_token(request, "request_token");
            const auto& action = sdp::wire::string_field(request, "action");
            if (action != "integrity" && action != "statistics" && action != "index")
                throw Error(Code::invalid_argument);
            break;
        }
        }
        return request;
    }
    catch (const Error&) { throw Error(Code::invalid_argument); }
}

void check_health_record(const Json& record)
{
    const auto& status = sdp::wire::string_field(record, "status");
    if (status != "UNKNOWN" && status != "OK" && status != "ERROR") throw Error(Code::malformed);
    const auto checked_at = record.find("checked_at");
    if (checked_at == record.end() || (!checked_at->is_null() && !checked_at->is_number_integer()) ||
        !record.contains("evidence") || !record.contains("remediation"))
        throw Error(Code::malformed);
}

void check_admin_response(sdp_admin_operation operation, const Json& response)
{
    switch (operation)
    {
    case SDP_ADMIN_STATUS:
        decision_text(response, "instance_name", 128);
        decision_text(response, "instance_id", 128);
        response_field(response, "port", Json::value_t::number_unsigned);
        response_field(response, "sessions", Json::value_t::array);
        (void)sdp::wire::object_field(response, "limits");
        break;
    case SDP_ADMIN_HEALTH:
    {
        decision_text(response, "instance_id", 128);
        const auto& checks = sdp::wire::object_field(response, "checks");
        for (const auto name : health_checks) check_health_record(sdp::wire::object_field(checks, name.data()));
        break;
    }
    case SDP_ADMIN_HEALTH_CHECK:
    {
        const auto& name = sdp::wire::string_field(response, "check");
        if (std::ranges::find(health_checks, name) == health_checks.end()) throw Error(Code::malformed);
        check_health_record(sdp::wire::object_field(response, "result"));
        break;
    }
    case SDP_ADMIN_CONFIG_PREVIEW:
    case SDP_ADMIN_CONFIG_APPLY:
        if (!response_field(response, "valid", Json::value_t::boolean).get<bool>() ||
            response_field(response, "applied", Json::value_t::boolean).get<bool>() != (operation == SDP_ADMIN_CONFIG_APPLY))
            throw Error(Code::malformed);
        (void)sdp::wire::object_field(response, "dynamic");
        (void)sdp::wire::object_field(response, "restart_required");
        break;
    case SDP_ADMIN_SUPPORT_PREVIEW:
    case SDP_ADMIN_SUPPORT_CREATE:
        response_field(response, "format", Json::value_t::number_unsigned);
        (void)sdp::wire::object_field(response, "config");
        (void)sdp::wire::object_field(response, "log_counts");
        response_field(response, "discarded_records", Json::value_t::number_unsigned);
        if (!response_field(response, "secret_scan_passed", Json::value_t::boolean).get<bool>())
            throw Error(Code::malformed);
        if (operation == SDP_ADMIN_SUPPORT_CREATE) instance_target_name(response);
        break;
    case SDP_ADMIN_MAINTENANCE_SUBMIT:
        check_job_response(SDP_JOB_SUBMIT, response);
        break;
    }
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

sdp_status SDP_CALL sdp_decision_command(sdp_connection connection, sdp_decision_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto name = decision_operation_name(operation);
        auto body = decision_request(operation, request_json);
        const bool writing = operation == SDP_DECISION_CREATE || operation == SDP_DECISION_ACTIVATE;
        const auto response = require_connection(connection)->request(name, std::move(body), writing);
        check_decision_response(operation, response);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_job_command(sdp_connection connection, sdp_job_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto name = job_operation_name(operation);
        auto body = job_request(operation, request_json);
        const bool writing = operation == SDP_JOB_SUBMIT || operation == SDP_JOB_CANCEL ||
            operation == SDP_JOB_SCHEDULE_CREATE || operation == SDP_JOB_SCHEDULE_CANCEL;
        const auto response = require_connection(connection)->request(name, std::move(body), writing);
        check_job_response(operation, response);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_dataset_command(sdp_connection connection, sdp_dataset_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto& operation_spec = wire_command_spec(operation, dataset_commands);
        auto body = dataset_request(operation, request_json);
        const auto response = require_connection(connection)->request(operation_spec.name, std::move(body), operation_spec.writes);
        check_dataset_response(operation, response);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_model_command(sdp_connection connection, sdp_model_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto& operation_spec = wire_command_spec(operation, model_commands);
        auto body = model_request(operation, request_json);
        const auto response = require_connection(connection)->request(operation_spec.name, std::move(body), operation_spec.writes);
        check_model_response(operation, response);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_backup_command(sdp_connection connection, sdp_backup_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto& operation_spec = wire_command_spec(operation, backup_commands);
        auto body = backup_request(operation, request_json);
        const auto response = require_connection(connection)->request(operation_spec.name, std::move(body), operation_spec.writes);
        check_backup_response(operation, response);
        write_json(*response_json, response);
    });
}

sdp_status SDP_CALL sdp_admin_command(sdp_connection connection, sdp_admin_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error)
{
    return boundary(error, [&] {
        check_output(response_json);
        const auto& operation_spec = wire_command_spec(operation, admin_commands);
        auto body = admin_request(operation, request_json);
        const auto response = require_connection(connection)->request(operation_spec.name, std::move(body), operation_spec.writes);
        check_admin_response(operation, response);
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
