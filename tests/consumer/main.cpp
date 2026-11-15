#include <StarDataParadiseDLLler.hpp>
#include <windows.h>

#include <array>
#include <charconv>
#include <chrono>
#include <future>
#include <iostream>
#include <set>
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
        std::cerr << "Expected " << expected << ", got " << actual << " (" << error.sqlstate
            << ", line " << error.line << ", column " << error.column
            << ", " << (error.correlation_id[0] ? "server" : "local") << ")\n";
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

std::string take(Buffer& buffer)
{
    require(buffer.value.data != nullptr && buffer.value.size, "native response buffer was empty");
    return {reinterpret_cast<const char*>(buffer.value.data), buffer.value.size};
}

std::string json_id(std::string_view response, std::string_view field = "result_id")
{
    const auto key = "\"" + std::string(field) + "\":\"";
    const auto start = response.find(key);
    require(start != response.npos, "Expected opaque ID was missing");
    const auto first = start + key.size();
    const auto end = response.find('"', first);
    require(end != response.npos && first < end, "Opaque ID was empty");
    return std::string(response.substr(first, end - first));
}

constexpr auto row_match = "SELECT id, note, price, flags, stamp FROM app.data.items WHERE id = :id"sv;
constexpr auto declarations_id = R"({"id":{"type":"BIGINT"}})"sv;
constexpr auto values_id = R"({"id":{"t":"i64","v":"-9223372036854775808"}})"sv;
constexpr auto lost_token = "000000000000000000000000000000ff"sv;

class SqlClient
{
public:
    explicit SqlClient(sdp_connection handle) : handle_(handle) {}

    std::string prepare(std::string_view query, std::string_view types = {})
    {
        Buffer statement;
        if (types.empty())
            expect(sdp_prepare(handle_, text(query), &statement.value, &error_), SDP_OK, error_);
        else
            expect(sdp_prepare_typed(handle_, text(query), text(types), &statement.value, &error_), SDP_OK, error_);
        return take(statement);
    }

    void close(std::string_view statement)
    {
        expect(sdp_statement_close(handle_, text(statement), &error_), SDP_OK, error_);
    }

    std::string execute(std::string_view statement, std::string_view values = {},
        std::string_view token = {}, uint32_t limit = 1000)
    {
        Buffer result;
        expect(sdp_execute(handle_, text(statement), text(values), text(token), limit, &result.value, &error_),
            SDP_OK, error_);
        return take(result);
    }

    void write(std::string_view query, std::string_view token = {})
    {
        const auto statement = prepare(query);
        execute(statement, {}, token);
        close(statement);
    }

    void fetch(std::string_view id, uint32_t count, std::string_view label)
    {
        for (int i = 0; i != 128; ++i)
        {
            Buffer result;
            expect(sdp_fetch(handle_, text(id), count, &result.value, &error_), SDP_OK, error_);
            const auto json = take(result);
            std::cout << label << ' ' << json << '\n';
            if (json.find("\"end_of_results\":true") != json.npos) return;
        }
        throw std::runtime_error("native result did not complete within the fixture bound");
    }

    void selected(std::string_view label)
    {
        const auto select = prepare(row_match, declarations_id);
        const auto result = json_id(execute(select, values_id));
        fetch(result, 1, label);
        close(select);
    }

    void outcome(std::string_view token, std::string_view name)
    {
        Buffer result;
        expect(sdp_tx_outcome(handle_, text(token), &result.value, &error_), SDP_OK, error_);
        std::cout << name << ' ' << take(result) << '\n';
    }

    sdp_error error_{};

private:
    sdp_connection handle_;
};

void sql_roundtrip(sdp_connection handle)
{
    SqlClient client(handle);
    auto& error = client.error_;
    client.write("CREATE DATABASE app", "00000000000000000000000000000001");
    client.write("CREATE SCHEMA app.data", "00000000000000000000000000000002");
    client.write("CREATE TABLE app.data.items (id BIGINT NOT NULL, note TEXT, price DECIMAL(38,4), flags BLOB, stamp TIMESTAMP WITH TIME ZONE)",
        "00000000000000000000000000000003");
    Buffer catalog;
    expect(sdp_metadata(handle, &catalog.value, &error), SDP_OK, error);
    std::cout << "metadata " << take(catalog) << '\n';

    const auto insert = client.prepare("INSERT INTO app.data.items VALUES (:id, :message, :money, :bits, :ts)",
        R"({"id":{"type":"BIGINT"},"message":{"type":"TEXT"},"money":{"type":"DECIMAL","precision":38,"scale":4},"bits":{"type":"BLOB"},"ts":{"type":"TIMESTAMP WITH TIME ZONE"}})");
    constexpr auto values = R"({"id":{"t":"i64","v":"-9223372036854775808"},"message":{"t":"text","v":"İstanbul '); SELECT 9; --"},"money":{"t":"decimal","unscaled":"12345678901234567890123456789012345678","scale":4},"bits":{"t":"binary","hex":"00ff"},"ts":{"t":"timestamp","seconds":"0","nanos":123456789,"offset_minutes":0}})"sv;
    constexpr auto first_token = "00000000000000000000000000000004"sv;
    Buffer rejected;
    expect(sdp_execute(handle, text(insert), text(R"({"id":{"t":"text","v":"1"}})"), text(first_token),
        1000, &rejected.value, &error), SDP_INVALID_ARGUMENT, error);
    require(!rejected.value.data && !error.unknown_outcome, "rejected parameters marked uncertain");
    expect(sdp_execute(handle, text(insert), text(R"({"id":{"t":"null"},"id":{"t":"null"}})"), text(first_token),
        1000, &rejected.value, &error), SDP_INVALID_ARGUMENT, error);
    expect(sdp_execute(handle, text(insert), text(values), text("short-token"), 1000, &rejected.value, &error),
        SDP_INVALID_ARGUMENT, error);
    std::cout << "inserted " << client.execute(insert, values, first_token) << '\n';
    expect(sdp_execute(handle, text(insert), text(values), text(first_token), 1000, &rejected.value, &error),
        SDP_INVALID_STATE, error);
    client.close(insert);
    client.selected("typed");

    expect(sdp_tx_begin(handle, &error), SDP_OK, error);
    expect(sdp_tx_savepoint(handle, text("before_writes"), &error), SDP_OK, error);
    client.write("INSERT INTO app.data.items VALUES (7, NULL, NULL, NULL, NULL)");
    expect(sdp_tx_rollback_to(handle, text("before_writes"), &error), SDP_OK, error);
    expect(sdp_tx_release(handle, text("before_writes"), &error), SDP_OK, error);
    client.write("INSERT INTO app.data.items VALUES (8, NULL, NULL, NULL, NULL)");
    client.write("DELETE FROM app.data.items WHERE id = 8");
    const auto update = client.prepare("UPDATE app.data.items SET note = :note WHERE id = :id",
        R"({"note":{"type":"TEXT"},"id":{"type":"BIGINT"}})");
    std::cout << "updated " << client.execute(update,
        R"({"note":{"t":"null"},"id":{"t":"i64","v":"-9223372036854775808"}})") << '\n';
    client.close(update);
    Buffer committed;
    constexpr auto stable_token = "00000000000000000000000000000005"sv;
    expect(sdp_tx_commit(handle, text(stable_token), &committed.value, &error), SDP_OK, error);
    std::cout << "committed " << take(committed) << '\n';
    client.outcome(stable_token, "ledger");

    expect(sdp_tx_begin(handle, &error), SDP_OK, error);
    client.write("INSERT INTO app.data.items VALUES (9, NULL, NULL, NULL, NULL)");
    expect(sdp_tx_rollback(handle, &error), SDP_OK, error);
    client.selected("nullable");
    const auto rolled = client.prepare("SELECT id FROM app.data.items WHERE id >= 0");
    client.fetch(json_id(client.execute(rolled)), 8, "rolled_back");
    client.close(rolled);

    std::string bulk = "INSERT INTO app.data.items VALUES ";
    for (int number = 1; number <= 320; ++number)
    {
        if (number > 1) bulk += ',';
        bulk += '(' + std::to_string(number) + ", NULL, NULL, NULL, NULL)";
    }
    client.write(bulk, "00000000000000000000000000000006");

    const auto scan = client.prepare("SELECT id FROM app.data.items WHERE id > 0");
    auto canceled = json_id(client.execute(scan));
    expect(sdp_result_cancel(handle, text(canceled), &error), SDP_OK, error);
    expect(sdp_fetch(handle, text(canceled), 10, &rejected.value, &error), SDP_INVALID_STATE, error);
    client.fetch(json_id(client.execute(scan)), 29, "stream");
    client.close(scan);

    client.outcome("00000000000000000000000000000000", "unknown");
}

void sql_lost_response(sdp_connection handle)
{
    SqlClient client(handle);
    auto& error = client.error_;
    expect(sdp_tx_begin(handle, &error), SDP_OK, error);
    client.write("UPDATE app.data.items SET note = 'committed through lost reply' WHERE id < 0");
    gate("COMMIT_READY");
    Buffer response;
    expect(sdp_tx_commit(handle, text(lost_token), &response.value, &error), SDP_TIMEOUT, error);
    require(error.unknown_outcome && !error.retryable && !response.value.data, "lost commit reply did not mark outcome unknown");
}

void sql_restart(sdp_connection handle)
{
    SqlClient client(handle);
    client.outcome(lost_token, "ledger");
    client.selected("persisted");
}

std::string session_id(std::string_view sessions, std::string_view actor)
{
    const auto actor_key = "\"actor\":\"" + std::string(actor) + "\"";
    const auto found = sessions.find(actor_key);
    require(found != sessions.npos, "Authenticated actor missing from session list");
    const auto end = sessions.find('}', found);
    constexpr std::string_view field = "\"session_id\":\"";
    const auto key = sessions.find(field, found + actor_key.size());
    require(key != sessions.npos && key < end, "Session ID not found for actor");
    const auto start = key + field.size();
    const auto quote = sessions.find('"', start);
    require(quote != sessions.npos && quote - start == 32, "Session ID is malformed");
    return std::string(sessions.substr(start, 32));
}

void security_roundtrip(sdp_connection root_handle, sdp_open_options options)
{
    Password alice_password, new_password;
    require(static_cast<bool>(std::getline(std::cin, alice_password.value) &&
        std::getline(std::cin, new_password.value)), "Missing generated security fixture credentials");
    SqlClient root(root_handle);
    auto& error = root.error_;

    root.write("CREATE TABLE secured (id BIGINT PRIMARY KEY, note TEXT)", "00000000000000000000000000000010");
    root.write("INSERT INTO secured VALUES (1, 'root only')", "00000000000000000000000000000011");
    root.write("CREATE USER alice", "00000000000000000000000000000012");
    root.write("CREATE ROLE readers", "00000000000000000000000000000013");
    expect(sdp_password_set(root_handle, text("alice"), {}, text(alice_password.value),
        text("00000000000000000000000000000014"), &error), SDP_OK, error);

    options.username = text("alice");
    options.password = text(alice_password.value);
    Handle alice_handle;
    expect(sdp_open(&options, &alice_handle.value, &error), SDP_OK, error);
    SqlClient alice(alice_handle.value);
    {
        Buffer denied;
        expect(sdp_prepare(alice_handle.value, text("SELECT id FROM secured"), &denied.value, &error),
            SDP_PERMISSION_DENIED, error);
        expect(sdp_metadata(alice_handle.value, &denied.value, &error), SDP_PERMISSION_DENIED, error);
        expect(sdp_session_list(alice_handle.value, &denied.value, &error), SDP_PERMISSION_DENIED, error);
        expect(sdp_tx_outcome(alice_handle.value, text("00000000000000000000000000000011"), &denied.value, &error),
            SDP_OK, error);
        require(take(denied) == R"({"status":"unknown"})", "Foreign request token leaked its outcome");
    }

    root.write("GRANT readers TO USER alice", "00000000000000000000000000000015");
    root.write("GRANT SELECT ON TABLE secured TO ROLE readers", "00000000000000000000000000000016");
    root.write("GRANT INSERT ON TABLE secured TO ROLE readers", "00000000000000000000000000000017");

    const auto previous = alice.prepare("SELECT id, note FROM secured");
    expect(sdp_tx_begin(alice_handle.value, &error), SDP_OK, error);
    alice.write("INSERT INTO secured VALUES (2, 'rollback on reset')");
    const auto result = json_id(alice.execute(previous));
    expect(sdp_session_reset(alice_handle.value, &error), SDP_OK, error);
    {
        Buffer denied;
        expect(sdp_fetch(alice_handle.value, text(result), 1, &denied.value, &error),
            SDP_INVALID_STATE, error);
        expect(sdp_execute(alice_handle.value, text(previous), {}, {}, 10, &denied.value, &error),
            SDP_INVALID_STATE, error);
    }
    const auto committed_only = root.prepare("SELECT id FROM secured WHERE id = 2");
    const auto probe = json_id(root.execute(committed_only));
    {
        Buffer response;
        expect(sdp_fetch(root_handle, text(probe), 10, &response.value, &error), SDP_OK, error);
        require(take(response).find("\"rows\":[]") != std::string::npos, "Pool reset did not roll back changes");
    }
    root.close(committed_only);

    const auto retained = alice.prepare("SELECT id FROM secured");
    const auto visible = json_id(alice.execute(retained));
    {
        Buffer response;
        expect(sdp_fetch(alice_handle.value, text(visible), 10, &response.value, &error), SDP_OK, error);
        require(take(response).find(R"({"t":"i64","v":"1"})") != std::string::npos,
            "Grant did not authorize the existing Alice session");
    }
    root.write("REVOKE SELECT ON TABLE secured FROM ROLE readers", "00000000000000000000000000000018");
    {
        Buffer denied;
        expect(sdp_execute(alice_handle.value, text(retained), {}, {}, 10, &denied.value, &error),
            SDP_PERMISSION_DENIED, error);
        expect(sdp_prepare(alice_handle.value, text("SELECT id FROM secured"), &denied.value, &error),
            SDP_PERMISSION_DENIED, error);
    }
    alice.close(retained);
    expect(sdp_password_set(alice_handle.value, text("alice"), text(alice_password.value), text(new_password.value),
        text("00000000000000000000000000000019"), &error), SDP_OK, error);
    {
        Handle rejected;
        expect(sdp_open(&options, &rejected.value, &error), SDP_AUTH_FAILED, error);
    }
    alice_password.clear();
    // End the old control target before testing management of the newly authenticated session.
    expect(sdp_close(alice_handle.value, &error), SDP_OK, error);
    alice_handle.value = 0;
    options.password = text(new_password.value);
    Handle fresh;
    expect(sdp_open(&options, &fresh.value, &error), SDP_OK, error);
    new_password.clear();
    {
        Buffer sessions;
        expect(sdp_session_list(root_handle, &sessions.value, &error), SDP_OK, error);
        const auto id = session_id(take(sessions), "alice");
        expect(sdp_query_cancel(root_handle, text(id), 0, &error), SDP_INVALID_STATE, error);
        expect(sdp_session_terminate(root_handle, text(id), &error), SDP_OK, error);
        sdp_status status = SDP_OK;
        for (unsigned attempt = 0; status == SDP_OK && attempt < 10; ++attempt)
        {
            status = sdp_health(fresh.value, &error);
            if (status == SDP_OK) Sleep(50);
        }
        require(status == SDP_CONNECTION_LOST || status == SDP_CANCELLED,
            "Terminated native connection stayed live");
    }
}

void plsql_roundtrip(sdp_connection handle)
{
    SqlClient client(handle);
    auto& error = client.error_;
    client.write("CREATE TABLE items (id BIGINT PRIMARY KEY)", "00000000000000000000000000000100");
    client.write("INSERT INTO items VALUES (3),(7),(9)", "00000000000000000000000000000101");
    client.write("CREATE PROCEDURE list_items(p_key IN BIGINT,p_value OUT BIGINT) AS DECLARE c REFCURSOR; "
        "d REFCURSOR; BEGIN SELECT id INTO p_value FROM items WHERE id = :p_key; "
        "OPEN c FOR SELECT id FROM items WHERE id = :p_key; EMIT c AS found; "
        "OPEN d FOR SELECT id FROM items; EMIT d AS all_rows; END;",
        "00000000000000000000000000000102");
    client.write("CREATE PROCEDURE echo_data(p_rec IN OUT RECORD,p_values IN OUT COLLECTION) AS BEGIN "
        "p_rec := p_rec; p_values := p_values; END;", "00000000000000000000000000000103");

    expect(sdp_tx_begin(handle, &error), SDP_OK, error);
    const auto list = client.prepare("CALL list_items(:key,NULL)", R"({"key":{"type":"BIGINT"}})");
    const auto inputs = R"({"key":{"t":"i64","v":"7"}})"sv;
    client.fetch(json_id(client.execute(list, inputs)), 1, "pl_sets");
    const auto cancelled = json_id(client.execute(list, inputs));
    {
        Buffer first;
        expect(sdp_fetch(handle, text(cancelled), 1, &first.value, &error), SDP_OK, error);
        expect(sdp_result_cancel(handle, text(cancelled), &error), SDP_OK, error);
        sdp_buffer_free(&first.value);
        expect(sdp_fetch(handle, text(cancelled), 1, &first.value, &error), SDP_INVALID_STATE, error);
    }
    client.close(list);

    const auto echo = client.prepare("CALL echo_data(:p_record,:p_values)",
        R"({"p_record":{"type":"RECORD"},"p_values":{"type":"COLLECTION"}})");
    constexpr auto invalid =
        R"({"p_record":{"t":"record","fields":[{"name":"x","value":{"t":"i64","v":"1"}},{"name":"x","value":{"t":"i64","v":"2"}}]},"p_values":{"t":"collection","items":[]}})"sv;
    {
        Buffer denied;
        expect(sdp_execute(handle, text(echo), text(invalid), {}, 10, &denied.value, &error),
            SDP_INVALID_ARGUMENT, error);
        require(!denied.value.data && !error.unknown_outcome, "Malformed compound reached execution");
    }
    constexpr auto compound =
        R"({"p_record":{"t":"record","fields":[{"name":"önce","value":{"t":"i64","v":"-9223372036854775808"}},)"
        R"({"name":"nested","value":{"t":"collection","items":[{"t":"null"},{"t":"decimal","unscaled":"12345678901234567890123456789012345678","scale":4},)"
        R"({"t":"record","fields":[{"name":"a","value":{"t":"text","v":"İstanbul\u0000"}},{"name":"day","value":{"t":"date","v":"2024-02-29"}}]}]}}]},)"
        R"("p_values":{"t":"collection","items":[{"t":"binary","hex":"00ff"},{"t":"timestamp","seconds":"0","nanos":123456789,"offset_minutes":180}]}})"sv;
    client.fetch(json_id(client.execute(echo, compound)), 1, "pl_compound");
    client.close(echo);
    expect(sdp_tx_rollback(handle, &error), SDP_OK, error);
}

std::string other_session(std::string_view sessions, std::string_view ignored)
{
    constexpr std::string_view key = "\"session_id\":\"";
    for (size_t from = 0;;)
    {
        const auto at = sessions.find(key, from);
        if (at == sessions.npos) throw std::runtime_error("Separate debugger target session missing");
        const auto start = at + key.size();
        const auto end = sessions.find('"', start);
        require(end != sessions.npos && end - start == 32, "Malformed debugger target session ID");
        if (const auto id = sessions.substr(start, 32); id != ignored) return std::string(id);
        from = end + 1;
    }
}

std::string wait_for_pause(sdp_connection controller, std::string_view id, uint32_t line)
{
    const auto until = std::chrono::steady_clock::now() + 5s;
    const auto expected = "\"line\":" + std::to_string(line) + ",";
    while (std::chrono::steady_clock::now() < until)
    {
        Buffer state;
        sdp_error error{};
        expect(sdp_debug_status(controller, text(id), &state.value, &error), SDP_OK, error);
        auto snapshot = take(state);
        if (snapshot.find("\"state\":\"paused\"") != snapshot.npos && snapshot.find(expected) != snapshot.npos)
            return snapshot;
        require(snapshot.find("\"state\":\"finished\"") == snapshot.npos, "Routine completed before its breakpoint");
        Sleep(15);
    }
    throw std::runtime_error("Debugger did not pause on the expected source line");
}

void debugger_roundtrip(sdp_connection controller, sdp_open_options options, Password& admin_password)
{
    Password guest_password;
    require(static_cast<bool>(std::getline(std::cin, guest_password.value)), "Missing generated debugger fixture credential");
    SqlClient control(controller);
    auto& error = control.error_;
    control.write("CREATE TABLE debug_items (id BIGINT PRIMARY KEY)", "00000000000000000000000000000201");
    control.write("INSERT INTO debug_items VALUES (7)", "00000000000000000000000000000202");
    control.write("CREATE PROCEDURE debug_work(p_input IN BIGINT,p_out OUT BIGINT) AS DECLARE\n"
        "local_value BIGINT := p_input;\n"
        "BEGIN\n"
        "local_value := local_value + 2;\n"
        "p_out := (SELECT id FROM debug_items WHERE id = :local_value);\n"
        "END;", "00000000000000000000000000000203");
    control.write("CREATE PROCEDURE write_fixture() AS BEGIN "
        "INSERT INTO debug_items VALUES (99); END;", "00000000000000000000000000000204");
    control.write("CREATE USER debug_guest", "00000000000000000000000000000205");
    expect(sdp_password_set(controller, text("debug_guest"), {}, text(guest_password.value),
        text("00000000000000000000000000000206"), &error), SDP_OK, error);

    std::string controller_id, target_id;
    {
        Buffer list;
        expect(sdp_session_list(controller, &list.value, &error), SDP_OK, error);
        controller_id = session_id(take(list), "local-admin");
    }
    options.password = text(admin_password.value);
    Handle target_handle, verifier_handle;
    expect(sdp_open(&options, &target_handle.value, &error), SDP_OK, error);
    {
        Buffer list;
        expect(sdp_session_list(controller, &list.value, &error), SDP_OK, error);
        target_id = other_session(take(list), controller_id);
    }
    expect(sdp_open(&options, &verifier_handle.value, &error), SDP_OK, error);
    admin_password.clear();

    {
        auto guest = options;
        guest.username = text("debug_guest");
        guest.password = text(guest_password.value);
        Handle handle;
        expect(sdp_open(&guest, &handle.value, &error), SDP_OK, error);
        guest_password.clear();
        Buffer denied;
        expect(sdp_debug_open(handle.value, text(target_id), text("debug_work"), 1, &denied.value, &error),
            SDP_PERMISSION_DENIED, error);
    }

    SqlClient target(target_handle.value);
    const auto statement = target.prepare("CALL debug_work(:p_value,NULL)", R"({"p_value":{"type":"BIGINT"}})");
    constexpr auto input = R"({"p_value":{"t":"i64","v":"5"}})"sv;
    std::string debug_id;
    {
        Buffer open;
        expect(sdp_debug_open(controller, text(target_id), text("debug_work"), 1, &open.value, &error), SDP_OK, error);
        debug_id = take(open);
    }
    expect(sdp_debug_options(controller, text(debug_id), 0, &error), SDP_OK, error);
    expect(sdp_debug_breakpoint(controller, text(debug_id), 4, 2, 1, &error), SDP_INVALID_STATE, error);
    expect(sdp_debug_breakpoint(controller, text(debug_id), 4, 1, 1, &error), SDP_OK, error);
    auto first = std::async(std::launch::async, [&] { return target.execute(statement, input); });
    std::cout << "debug_before " << wait_for_pause(controller, debug_id, 4) << '\n';
    {
        Buffer watch;
        expect(sdp_debug_watch(controller, text(debug_id), text("local_value := 0"), &watch.value, &error),
            SDP_INVALID_ARGUMENT, error);
        expect(sdp_debug_watch(controller, text(debug_id), text("local_value"), &watch.value, &error), SDP_OK, error);
        std::cout << "watch_before " << take(watch) << '\n';
    }
    expect(sdp_debug_step(controller, text(debug_id), SDP_STEP_OVER, &error), SDP_OK, error);
    std::cout << "debug_after " << wait_for_pause(controller, debug_id, 5) << '\n';
    {
        Buffer watch;
        expect(sdp_debug_watch(controller, text(debug_id), text("local_value"), &watch.value, &error), SDP_OK, error);
        std::cout << "watch_after " << take(watch) << '\n';
    }
    try { expect(sdp_debug_continue(controller, text(debug_id), &error), SDP_OK, error); }
    catch (const std::exception& cause) { throw std::runtime_error(std::string("debug_continue: ") + cause.what()); }
    std::string resumed;
    try { resumed = first.get(); }
    catch (const std::exception& cause) { throw std::runtime_error(std::string("resumed execute: ") + cause.what()); }
    try { target.fetch(json_id(resumed), 2, "debug_output"); }
    catch (const std::exception& cause) { throw std::runtime_error(std::string("resumed fetch: ") + cause.what()); }
    {
        Buffer profile;
        expect(sdp_profile_last(target_handle.value, &profile.value, &error), SDP_OK, error);
        std::cout << "profile " << take(profile) << '\n';
    }
    expect(sdp_debug_close(controller, text(debug_id), &error), SDP_OK, error);

    const auto fixture = control.prepare("CALL write_fixture()");
    {
        Buffer outcome;
        expect(sdp_routine_test(controller, text(fixture), {}, text(R"({"sets":[]})"), &outcome.value, &error),
            SDP_OK, error);
        std::cout << "fixture " << take(outcome) << '\n';
    }
    control.close(fixture);
    {
        Buffer reopened;
        expect(sdp_debug_open(controller, text(target_id), text("debug_work"), 1, &reopened.value, &error), SDP_OK, error);
        debug_id = take(reopened);
    }
    expect(sdp_debug_breakpoint(controller, text(debug_id), 4, 1, 1, &error), SDP_OK, error);
    expect(sdp_tx_begin(target_handle.value, &error), SDP_OK, error);
    target.write("INSERT INTO debug_items VALUES (100)");
    auto interrupted = std::async(std::launch::async, [&] {
        Buffer response;
        sdp_error status{};
        return sdp_execute(target_handle.value, text(statement), text(input), {}, 20, &response.value, &status);
    });
    std::cout << "debug_disconnect " << wait_for_pause(controller, debug_id, 4) << '\n';
    expect(sdp_close(controller, &error), SDP_OK, error);
    const auto status = interrupted.get();
    require(status == SDP_CANCELLED || status == SDP_CONNECTION_LOST, "Controller disconnect did not stop debug target");

    SqlClient verifier(verifier_handle.value);
    const auto verify = verifier.prepare("SELECT id FROM debug_items WHERE id > 7");
    verifier.fetch(json_id(verifier.execute(verify)), 10, "rollback_probe");
    verifier.close(verify);
}

std::string history(SqlClient& actor, sdp_connection connection, sdp_history_operation operation,
    std::string_view input = {})
{
    Buffer output;
    expect(sdp_history_command(connection, operation, text(input), &output.value, &actor.error_),
        SDP_OK, actor.error_);
    return take(output);
}

void history_roundtrip(sdp_connection handle, sdp_open_options options)
{
    Password guest_password;
    require(static_cast<bool>(std::getline(std::cin, guest_password.value)), "Missing generated History fixture credential");
    SqlClient client(handle);
    auto& error = client.error_;
    client.write("CREATE TABLE audit_items (id BIGINT PRIMARY KEY, note TEXT, payload BLOB)",
        "00000000000000000000000000000301");
    const auto prepared = client.prepare("INSERT INTO audit_items VALUES (11,:note,:payload)",
        R"({"note":{"type":"TEXT"},"payload":{"type":"BLOB"}})");
    const auto inserted = json_id(client.execute(prepared,
        R"({"note":{"t":"text","v":"ilk"},"payload":{"t":"binary","hex":"00ff10"}})",
        "00000000000000000000000000000302"), "lsn");
    client.close(prepared);

    const auto next = client.prepare("UPDATE audit_items SET id = 22, note = 'değişti' WHERE id = 11");
    const auto changed = json_id(client.execute(next, {}, "00000000000000000000000000000303"), "lsn");
    client.close(next);
    expect(sdp_tx_begin(handle, &error), SDP_OK, error);
    client.write("UPDATE audit_items SET note = 'rollback-only' WHERE id = 22");
    expect(sdp_tx_rollback(handle, &error), SDP_OK, error);
    const auto remove = client.prepare("DELETE FROM audit_items WHERE id = 22");
    const auto deleted = json_id(client.execute(remove, {}, "00000000000000000000000000000304"), "lsn");
    client.close(remove);
    std::cout << "history_revisions {\"inserted\":\"" << inserted << "\",\"changed\":\"" << changed
              << "\",\"deleted\":\"" << deleted << "\"}\n";

    constexpr auto target = R"({"target":"audit_items")"sv;
    const auto read = std::string(target) + ",\"revision\":\"" + inserted + "\"}";
    const auto difference = std::string(target) + ",\"from\":\"" + inserted + "\",\"to\":\"" + changed + "\"}";
    std::cout << "history_log " << history(client, handle, SDP_HISTORY_LOG, R"({"target":"audit_items","limit":100})") << '\n';
    std::cout << "history_read " << history(client, handle, SDP_HISTORY_READ, read) << '\n';
    std::cout << "history_diff " << history(client, handle, SDP_HISTORY_DIFF, difference) << '\n';
    std::cout << "history_verify " << history(client, handle, SDP_HISTORY_VERIFY) << '\n';
    std::cout << "history_status " << history(client, handle, SDP_HISTORY_STATUS) << '\n';

    client.write("CREATE USER history_guest", "00000000000000000000000000000305");
    client.write("GRANT SELECT ON TABLE audit_items TO USER history_guest", "00000000000000000000000000000306");
    expect(sdp_password_set(handle, text("history_guest"), {}, text(guest_password.value),
        text("00000000000000000000000000000307"), &error), SDP_OK, error);
    options.username = text("history_guest");
    options.password = text(guest_password.value);
    {
        Handle guest;
        expect(sdp_open(&options, &guest.value, &error), SDP_OK, error);
        guest_password.clear();
        SqlClient read_only(guest.value);
        const auto live_select = read_only.prepare("SELECT id FROM audit_items");
        read_only.close(live_select);
        Buffer denied;
        expect(sdp_history_command(guest.value, SDP_HISTORY_LOG, text(R"({"target":"audit_items"})"),
            &denied.value, &error), SDP_PERMISSION_DENIED, error);
        require(!denied.value.data, "Ordinary SELECT access leaked history");
    }

    const auto pin = "{\"pin_kind\":\"snapshot\",\"pin_name\":\"retained_fixture\",\"revision\":\"" + inserted + "\"}";
    std::cout << "history_pinned " << history(client, handle, SDP_HISTORY_PIN, pin) << '\n';
    const auto floor = "{\"revision\":\"" + deleted + "\"}";
    {
        Buffer blocked;
        const auto status = sdp_history_command(handle, SDP_HISTORY_RETAIN, text(floor), &blocked.value, &error);
        require((status == SDP_INVALID_STATE || status == SDP_RESOURCE_LIMIT) && !blocked.value.data &&
            std::string_view(error.message).find("retention_pinned") != std::string_view::npos,
            "Pin did not preserve its revision");
    }
    history(client, handle, SDP_HISTORY_UNPIN, R"({"pin_kind":"snapshot","pin_name":"retained_fixture"})");
    std::cout << "history_retained " << history(client, handle, SDP_HISTORY_RETAIN, floor) << '\n';
    {
        Buffer expired;
        expect(sdp_history_command(handle, SDP_HISTORY_READ, text(read), &expired.value, &error),
            SDP_INVALID_STATE, error);
        require(!expired.value.data &&
            std::string_view(error.message).find("revision_not_recoverable") != std::string_view::npos,
            "Expired revision lacked the recovery diagnostic");
    }
    std::cout << "history_purged " << history(client, handle, SDP_HISTORY_PURGE, floor) << '\n';
}

std::string write_revision(SqlClient& client, std::string_view query, std::string_view token)
{
    const auto statement = client.prepare(query);
    const auto reply = client.execute(statement, {}, token);
    client.close(statement);
    return json_id(reply, "lsn");
}

std::string restore_request(std::string_view revision, std::string_view row, std::string_view field = {})
{
    auto body = "{\"target\":\"restore_items\",\"revision\":\"" + std::string(revision) +
        "\",\"row_id\":\"" + std::string(row) + "\"";
    if (!field.empty()) body += ",\"field\":\"" + std::string(field) + "\"";
    return body + '}';
}

std::string apply_request(std::string_view plan, std::string_view token)
{
    return "{\"plan_id\":\"" + std::string(plan) + "\",\"request_token\":\"" + std::string(token) + "\"}";
}

void restore_roundtrip(sdp_connection handle, sdp_open_options options, Password& admin_password)
{
    SqlClient client(handle);
    auto& error = client.error_;
    {
        Buffer capabilities;
        expect(sdp_get_capabilities(handle, &capabilities.value, &error), SDP_OK, error);
        require(take(capabilities).find("\"history_restore_v1\":true") != std::string::npos,
            "Initialized server did not advertise restore capability");
    }
    client.write("CREATE TABLE restore_items (id BIGINT PRIMARY KEY,note TEXT,context TEXT)",
        "00000000000000000000000000000401");
    const auto original = write_revision(client, "INSERT INTO restore_items VALUES (11,'original','first')",
        "00000000000000000000000000000402");
    const auto original_read = "{\"target\":\"restore_items\",\"revision\":\"" + original + "\"}";
    const auto old = history(client, handle, SDP_HISTORY_READ, original_read);
    const auto row = json_id(old, "row_id");
    client.write("INSERT INTO restore_items VALUES (22,'second','before')", "00000000000000000000000000000403");
    client.write("DELETE FROM restore_items WHERE id = 11", "00000000000000000000000000000404");

    const auto request = restore_request(original, row);
    const auto stale_plan = history(client, handle, SDP_HISTORY_RESTORE, request);
    client.write("INSERT INTO restore_items VALUES (33,'independent','untouched')", "00000000000000000000000000000405");
    {
        Buffer stale;
        const auto stale_body = apply_request(json_id(stale_plan, "plan_id"), "00000000000000000000000000000406");
        expect(sdp_history_command(handle, SDP_HISTORY_RESTORE, text(stale_body), &stale.value, &error),
            SDP_INVALID_STATE, error);
        require(!stale.value.data && !error.unknown_outcome &&
            std::string_view(error.message).find("stale_preview") != std::string_view::npos,
            "Concurrent commit did not invalidate preview");
    }
    const auto preview = history(client, handle, SDP_HISTORY_RESTORE, request);
    constexpr auto restore_token = "00000000000000000000000000000407"sv;
    const auto apply = apply_request(json_id(preview, "plan_id"), restore_token);
    const auto restored = history(client, handle, SDP_HISTORY_RESTORE, apply);
    std::cout << "restore_preview " << preview << '\n';
    std::cout << "restore_apply " << restored << '\n';
    std::cout << "restore_original " << old << '\n';

    options.password = text(admin_password.value);
    Handle reconnect;
    expect(sdp_open(&options, &reconnect.value, &error), SDP_OK, error);
    admin_password.clear();
    SqlClient fresh(reconnect.value);
    std::cout << "restore_replay " << history(fresh, reconnect.value, SDP_HISTORY_RESTORE, apply) << '\n';
    fresh.outcome(restore_token, "restore_ledger");
    const auto restored_read = "{\"target\":\"restore_items\",\"revision\":\"" + json_id(restored, "lsn") +
        "\",\"row_id\":\"" + row + "\"}";
    std::cout << "restore_read " << history(fresh, reconnect.value, SDP_HISTORY_READ, restored_read) << '\n';

    const auto wrong = write_revision(client, "UPDATE restore_items SET note = 'wrong' WHERE id = 22",
        "00000000000000000000000000000408");
    const auto filter = "{\"target\":\"restore_items\",\"from\":\"" + wrong + "\",\"to\":\"" + wrong + "\"}";
    const auto updated_row = json_id(history(client, handle, SDP_HISTORY_LOG, filter), "row_id");
    client.write("UPDATE restore_items SET context = 'preserve' WHERE id = 22",
        "00000000000000000000000000000409");
    const auto reverse = history(client, handle, SDP_HISTORY_REVERT, restore_request(wrong, updated_row, "note"));
    std::cout << "revert_preview " << reverse << '\n';
    const auto inverse = apply_request(json_id(reverse, "plan_id"), "00000000000000000000000000000410");
    std::cout << "revert_apply " << history(client, handle, SDP_HISTORY_REVERT, inverse) << '\n';

    const auto selected = fresh.prepare("SELECT id,note,context FROM restore_items ORDER BY id");
    fresh.fetch(json_id(fresh.execute(selected)), 2, "restore_rows");
    fresh.close(selected);
}

std::string decision(SqlClient& actor, sdp_connection connection, sdp_decision_operation operation,
    std::string_view input)
{
    Buffer output;
    expect(sdp_decision_command(connection, operation, text(input), &output.value, &actor.error_),
        SDP_OK, actor.error_);
    return take(output);
}

std::string decision_sql(std::string_view sql, std::string_view token = {})
{
    auto json = "{\"sql\":\"" + std::string(sql) + "\"";
    if (!token.empty()) json += ",\"request_token\":\"" + std::string(token) + "\"";
    return json + "}";
}

void decision_roundtrip(sdp_connection handle)
{
    SqlClient client(handle);
    constexpr auto rule_token = "00000000000000000000000000000501"sv;
    constexpr auto activate_token = "00000000000000000000000000000503"sv;
    constexpr auto first = "CREATE DECISION RULE minimum_stock ON stocked_items FOR INSERT "
        "PRIORITY 100 WHEN (NEW.stock >= 5) THEN ALLOW"sv;
    constexpr auto next = "CREATE DECISION RULE minimum_stock ON stocked_items FOR INSERT "
        "PRIORITY 100 WHEN (NEW.stock >= 2) THEN ALLOW"sv;
    constexpr auto scenario = R"({"target":"stocked_items","event":"insert","proposed":{"id":{"t":"i64","v":"2"},"stock":{"t":"i64","v":"3"}}})"sv;
    Buffer capabilities;
    expect(sdp_get_capabilities(handle, &capabilities.value, &client.error_), SDP_OK, client.error_);
    require(take(capabilities).find("\"decision_explain_v1\":true") != std::string::npos,
        "Decision ABI fixture requires the advertised explanation capability");
    {
        Buffer rejected;
        expect(sdp_decision_command(handle, 0, text("{}"), &rejected.value, &client.error_),
            SDP_INVALID_ARGUMENT, client.error_);
        expect(sdp_decision_command(handle, SDP_DECISION_WHATIF,
            text(R"({"target":"stocked_items","event":"truncate"})"), &rejected.value, &client.error_),
            SDP_INVALID_ARGUMENT, client.error_);
        require(!rejected.value.data && !client.error_.unknown_outcome,
            "invalid Decision payload marked an unknown outcome or returned a buffer");
    }

    client.write("CREATE TABLE stocked_items (id BIGINT PRIMARY KEY, stock BIGINT NOT NULL)",
        "00000000000000000000000000000500");
    const auto before_validation = history(client, handle, SDP_HISTORY_STATUS);
    std::cout << "decision_validate " << decision(client, handle, SDP_DECISION_VALIDATE, decision_sql(first)) << '\n';
    require(history(client, handle, SDP_HISTORY_STATUS) == before_validation,
        "rule validation changed the current revision");

    const auto created = decision(client, handle, SDP_DECISION_CREATE, decision_sql(first, rule_token));
    const auto revision = json_id(created, "lsn");
    std::cout << "decision_created " << created << '\n';
    client.outcome(rule_token, "decision_ledger");

    const auto before_simulation = history(client, handle, SDP_HISTORY_STATUS);
    const auto blocked = client.prepare("INSERT INTO stocked_items VALUES (2,3)");
    {
        Buffer rejected;
        expect(sdp_execute(handle, text(blocked), {}, text("00000000000000000000000000000505"), 10,
            &rejected.value, &client.error_), SDP_PERMISSION_DENIED, client.error_);
        require(!rejected.value.data && !client.error_.unknown_outcome &&
            std::string_view(client.error_.message).ends_with("(denied)"), "Native Decision denial lost its safe reason");
    }
    client.close(blocked);
    std::cout << "decision_why " << decision(client, handle, SDP_DECISION_WHYNOT, scenario) << '\n';
    std::cout << "decision_whatif " << decision(client, handle, SDP_DECISION_WHATIF, scenario) << '\n';
    std::cout << "decision_recheck " << decision(client, handle, SDP_DECISION_RECHECK, scenario) << '\n';
    require(history(client, handle, SDP_HISTORY_STATUS) == before_simulation,
        "Decision inspection changed the current revision");

    std::cout << "decision_appended " << decision(client, handle, SDP_DECISION_CREATE,
        decision_sql(next, "00000000000000000000000000000502")) << '\n';
    auto historical = std::string(scenario);
    historical.pop_back();
    historical += ",\"revision\":\"" + revision + "\"}";
    std::cout << "decision_historical " << decision(client, handle, SDP_DECISION_EXPLAIN, historical) << '\n';
    constexpr auto cases = R"({"target":"stocked_items","rule":"minimum_stock","from_version":1,"to_version":2,"cases":[{"event":"insert","proposed":{"id":{"t":"i64","v":"2"},"stock":{"t":"i64","v":"3"}}},{"event":"insert","proposed":{"id":{"t":"i64","v":"3"},"stock":{"t":"i64","v":"10"}}}]})"sv;
    const auto before_corpus = history(client, handle, SDP_HISTORY_STATUS);
    std::cout << "decision_test " << decision(client, handle, SDP_DECISION_TEST, cases) << '\n';
    require(history(client, handle, SDP_HISTORY_STATUS) == before_corpus,
        "rule comparison changed the current revision");

    const auto selected = client.prepare("SELECT id,stock FROM stocked_items ORDER BY id");
    client.fetch(json_id(client.execute(selected)), 10, "decision_before");
    std::cout << "decision_activated " << decision(client, handle, SDP_DECISION_ACTIVATE, decision_sql(
        "ALTER DECISION RULE minimum_stock ON stocked_items ACTIVATE VERSION 2", activate_token)) << '\n';
    std::cout << "decision_now " << decision(client, handle, SDP_DECISION_EXPLAIN, scenario) << '\n';
    client.write("INSERT INTO stocked_items VALUES (2,3)", "00000000000000000000000000000504");
    client.fetch(json_id(client.execute(selected)), 10, "decision_after");
    client.close(selected);
}

std::string job(SqlClient& actor, sdp_connection connection, sdp_job_operation operation, std::string_view input = "{}")
{
    Buffer output;
    expect(sdp_job_command(connection, operation, text(input), &output.value, &actor.error_),
        SDP_OK, actor.error_);
    return take(output);
}

std::string job_selector(std::string_view field, std::string_view id)
{
    return "{\"" + std::string(field) + "\":\"" + std::string(id) + "\"}";
}

std::string await_job(SqlClient& actor, sdp_connection connection, std::string_view selector,
    std::string_view final_state)
{
    const auto wanted = "\"state\":\"" + std::string(final_state) + "\"";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        auto status = job(actor, connection, SDP_JOB_WATCH, selector);
        if (status.find(wanted) != status.npos) return status;
        if (status.find("\"state\":\"failed\"") != status.npos ||
            status.find("\"state\":\"succeeded\"") != status.npos ||
            status.find("\"state\":\"cancelled\"") != status.npos)
            throw std::runtime_error("Job entered an unexpected terminal state");
        Sleep(30);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Job did not enter its final state within the fixture bound");
}

std::string dataset(sdp_connection connection, sdp_dataset_operation operation, std::string_view input)
{
    Buffer output;
    sdp_error error{};
    expect(sdp_dataset_command(connection, operation, text(input), &output.value, &error), SDP_OK, error);
    return take(output);
}

std::string model(sdp_connection connection, sdp_model_operation operation, std::string_view input)
{
    Buffer output;
    sdp_error error{};
    expect(sdp_model_command(connection, operation, text(input), &output.value, &error), SDP_OK, error);
    return take(output);
}

void ml_roundtrip(sdp_connection connection)
{
    SqlClient actor(connection);
    {
        Buffer rejected;
        sdp_error error{};
        expect(sdp_dataset_command(connection, 99, text("{}"), &rejected.value, &error),
            SDP_INVALID_ARGUMENT, error);
        expect(sdp_model_command(connection, 99, text("{}"), &rejected.value, &error),
            SDP_INVALID_ARGUMENT, error);
        expect(sdp_model_command(connection, SDP_MODEL_RESUME, text(
            R"({"name":"sdk_model","job_id":"11111111111111111111111111111111","checksum":"bad","request_token":"00000000000000000000000000000229"})"),
            &rejected.value, &error), SDP_INVALID_ARGUMENT, error);
        require(!rejected.value.data && !error.unknown_outcome && !error.correlation_id[0],
            "invalid ML request crossed the ABI boundary");
    }
    const auto devices = model(connection, SDP_MODEL_DEVICES, R"({"device_index":0,"memory_mib":128})");
    std::cout << "ml_devices " << devices << '\n';
    actor.write("CREATE TABLE sdk_ml (x BIGINT PRIMARY KEY, y BIGINT)", "00000000000000000000000000000201");
    actor.write(R"(INSERT INTO sdk_ml VALUES (0,4),(1,7),(2,10),(3,13),(4,16),
        (5,19),(6,22),(7,25),(8,28),(9,31))", "00000000000000000000000000000202");
    const auto created = dataset(connection, SDP_DATASET_CREATE, R"({
        "request_token":"00000000000000000000000000000203",
        "sql":"SELECT x,y FROM sdk_ml ORDER BY x",
        "options":{
            "features":[{"column":"x","transform":"numeric","impute":"reject"}],
            "target":"y","split":{"mode":"time","time":"x","train":60,"validation":20}
        }
    })");
    const auto dataset_id = json_id(created, "dataset_id");
    std::cout << "ml_dataset " << created << '\n';
    std::cout << "ml_materialized " << dataset(connection, SDP_DATASET_GET, job_selector("dataset_id", dataset_id)) << '\n';

    std::cout << "ml_model " << model(connection, SDP_MODEL_CREATE,
        R"({"name":"sdk_model","algorithm":"linear","request_token":"00000000000000000000000000000204"})") << '\n';
    const auto train = model(connection, SDP_MODEL_TRAIN,
        "{\"name\":\"sdk_model\",\"dataset_id\":\"" + dataset_id +
        "\",\"seed\":87,\"request_token\":\"00000000000000000000000000000205\"}");
    const auto train_id = json_id(train, "job_id");
    std::cout << "ml_train " << train << '\n';
    std::cout << "ml_trained " << await_job(actor, connection, job_selector("job_id", train_id), "succeeded") << '\n';
    {
        Buffer rejected;
        sdp_error error{};
        const auto checkpoint = "{\"name\":\"sdk_model\",\"job_id\":\"" + train_id + "\"}";
        expect(sdp_model_command(connection, SDP_MODEL_CHECKPOINT, text(checkpoint), &rejected.value, &error),
            SDP_INVALID_ARGUMENT, error);
        require(!rejected.value.data && !error.unknown_outcome && error.correlation_id[0],
            "completed linear job incorrectly exposed a checkpoint");
    }
    std::cout << "ml_status " << model(connection, SDP_MODEL_STATUS, R"({"name":"sdk_model"})") << '\n';
    std::cout << "ml_evaluate " << model(connection, SDP_MODEL_EVALUATE, R"({"name":"sdk_model","version":1})") << '\n';
    std::cout << "ml_activate " << model(connection, SDP_MODEL_ACTIVATE,
        R"({"name":"sdk_model","version":1,"request_token":"00000000000000000000000000000206"})") << '\n';
    std::cout << "ml_predict " << model(connection, SDP_MODEL_PREDICT, R"({"name":"sdk_model","features":{"x":12}})") << '\n';
    std::cout << "ml_deactivate " << model(connection, SDP_MODEL_DEACTIVATE,
        R"({"name":"sdk_model","request_token":"00000000000000000000000000000207"})") << '\n';
    {
        Buffer rejected;
        sdp_error error{};
        expect(sdp_model_command(connection, SDP_MODEL_PREDICT, text(R"({"name":"sdk_model","features":{"x":12}})"),
            &rejected.value, &error), SDP_INVALID_STATE, error);
        require(!rejected.value.data && !error.unknown_outcome && error.correlation_id[0],
            "predict without an active artifact lacked a definite server error");
    }
    const auto second = model(connection, SDP_MODEL_TRAIN,
        "{\"name\":\"sdk_model\",\"dataset_id\":\"" + dataset_id +
        "\",\"seed\":88,\"request_token\":\"00000000000000000000000000000211\"}");
    await_job(actor, connection, job_selector("job_id", json_id(second, "job_id")), "succeeded");
    std::cout << "ml_compare " << model(connection, SDP_MODEL_COMPARE,
        R"({"runs":[{"name":"sdk_model","version":1},{"name":"sdk_model","version":2}],"split":"test"})") << '\n';
    std::cout << "ml_activate_second " << model(connection, SDP_MODEL_ACTIVATE,
        R"({"name":"sdk_model","version":2,"request_token":"00000000000000000000000000000212"})") << '\n';
    std::cout << "ml_rollback " << model(connection, SDP_MODEL_ROLLBACK,
        R"({"name":"sdk_model","version":1,"request_token":"00000000000000000000000000000208"})") << '\n';
    std::cout << "ml_delete_second " << model(connection, SDP_MODEL_DELETE,
        R"({"name":"sdk_model","version":2,"request_token":"00000000000000000000000000000213"})") << '\n';
    std::cout << "ml_deactivate_again " << model(connection, SDP_MODEL_DEACTIVATE,
        R"({"name":"sdk_model","request_token":"00000000000000000000000000000209"})") << '\n';
    std::cout << "ml_delete " << model(connection, SDP_MODEL_DELETE,
        R"({"name":"sdk_model","version":1,"request_token":"00000000000000000000000000000210"})") << '\n';
    std::cout << "ml_status_after_delete " << model(connection, SDP_MODEL_STATUS, R"({"name":"sdk_model"})") << '\n';
}

std::string backup(sdp_connection connection, sdp_backup_operation operation, std::string_view input = "{}")
{
    Buffer output;
    sdp_error error{};
    expect(sdp_backup_command(connection, operation, text(input), &output.value, &error), SDP_OK, error);
    return take(output);
}

void backup_roundtrip(sdp_connection connection)
{
    SqlClient actor(connection);
    {
        constexpr auto invalid_restore = R"({"backup_id":"11111111111111111111111111111111","target_name":"../outside"})"sv;
        Buffer rejected;
        sdp_error error{};
        expect(sdp_backup_command(connection, 99, text("{}"), &rejected.value, &error),
            SDP_INVALID_ARGUMENT, error);
        expect(sdp_backup_command(connection, SDP_BACKUP_RESTORE, text(invalid_restore), &rejected.value, &error),
            SDP_INVALID_ARGUMENT, error);
        require(!rejected.value.data && !error.unknown_outcome && !error.correlation_id[0],
            "invalid backup request crossed the ABI boundary");
    }
    actor.write("CREATE TABLE sdk_backup (id BIGINT PRIMARY KEY,note TEXT)",
        "00000000000000000000000000000281");
    actor.write("INSERT INTO sdk_backup VALUES (1,'archive row')",
        "00000000000000000000000000000282");
    const auto queued = backup(connection, SDP_BACKUP_SUBMIT,
        R"({"request_token":"00000000000000000000000000000283","max_bytes":67108864})");
    const auto job_id = json_id(queued, "job_id");
    std::cout << "backup_queued " << queued << '\n';
    const auto complete = await_job(actor, connection, job_selector("job_id", job_id), "succeeded");
    std::cout << "backup_complete " << complete << '\n';
    std::cout << "backup_status " << backup(connection, SDP_BACKUP_STATUS, job_selector("job_id", job_id)) << '\n';

    const auto backup_id = json_id(complete, "backup_id");
    const auto archive_selector = job_selector("backup_id", backup_id);
    std::cout << "backup_catalog " << backup(connection, SDP_BACKUP_CATALOG) << '\n';
    std::cout << "backup_verified " << backup(connection, SDP_BACKUP_VERIFY, archive_selector) << '\n';
    std::cout << "backup_restored " << backup(connection, SDP_BACKUP_RESTORE,
        "{\"backup_id\":\"" + backup_id + "\",\"target_name\":\"sdk_copy\"}") << '\n';
    std::cout << "backup_deleted " << backup(connection, SDP_BACKUP_DELETE, archive_selector) << '\n';
    std::cout << "backup_final_catalog " << backup(connection, SDP_BACKUP_CATALOG) << '\n';

    const auto generic = job(actor, connection, SDP_JOB_SUBMIT,
        R"({"kind":"backup","max_bytes":67108864,"request_token":"00000000000000000000000000000285"})");
    const auto generic_id = job_selector("job_id", json_id(generic, "job_id"));
    std::cout << "backup_generic_result " << await_job(actor, connection, generic_id, "succeeded") << '\n';

    const auto scheduled = job(actor, connection, SDP_JOB_SCHEDULE_CREATE,
        R"({"kind":"backup","rule":{"every_seconds":2},"max_bytes":67108864,"request_token":"00000000000000000000000000000284"})");
    const auto schedule = job_selector("schedule_id", json_id(scheduled, "schedule_id"));
    std::cout << "backup_schedule " << scheduled << '\n';
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto status = job(actor, connection, SDP_JOB_SCHEDULE_STATUS, schedule);
        if (status.find("\"last_job_id\":\"") != status.npos)
        {
            const auto scheduled_job = job_selector("job_id", json_id(status, "last_job_id"));
            std::cout << "backup_schedule_cancelled " << job(actor, connection, SDP_JOB_SCHEDULE_CANCEL, schedule) << '\n';
            std::cout << "backup_scheduled_result " << await_job(actor, connection, scheduled_job, "succeeded") << '\n';
            return;
        }
        Sleep(30);
    }
    throw std::runtime_error("Native backup schedule did not dispatch a job");
}

std::string admin(sdp_connection connection, sdp_admin_operation operation, std::string_view input = "{}")
{
    Buffer output;
    sdp_error error{};
    expect(sdp_admin_command(connection, operation, text(input), &output.value, &error), SDP_OK, error);
    return take(output);
}

void admin_roundtrip(sdp_connection connection)
{
    SqlClient actor(connection);
    {
        Buffer rejected;
        sdp_error error{};
        expect(sdp_admin_command(connection, 99, text("{}"), &rejected.value, &error),
            SDP_INVALID_ARGUMENT, error);
        expect(sdp_admin_command(connection, SDP_ADMIN_CONFIG_APPLY, text(R"({"config":{"data_dir":"elsewhere"}})"),
            &rejected.value, &error), SDP_INVALID_ARGUMENT, error);
        expect(sdp_admin_command(connection, SDP_ADMIN_SUPPORT_CREATE, text(R"({"target_name":"../elsewhere"})"),
            &rejected.value, &error), SDP_INVALID_ARGUMENT, error);
        require(!rejected.value.data && !error.unknown_outcome && !error.correlation_id[0],
            "invalid admin request crossed the ABI boundary");
    }

    std::cout << "admin_status " << admin(connection, SDP_ADMIN_STATUS) << '\n';
    std::cout << "admin_health_before " << admin(connection, SDP_ADMIN_HEALTH) << '\n';
    std::cout << "admin_storage " << admin(connection, SDP_ADMIN_HEALTH_CHECK,
        R"({"check":"storage"})") << '\n';
    std::cout << "admin_health_after " << admin(connection, SDP_ADMIN_HEALTH) << '\n';
    std::cout << "admin_config_preview " << admin(connection, SDP_ADMIN_CONFIG_PREVIEW,
        R"({"config":{"timeout_ms":8500,"port":13393,"max_connections":12}})") << '\n';
    std::cout << "admin_config_applied " << admin(connection, SDP_ADMIN_CONFIG_APPLY,
        R"({"config":{"timeout_ms":8500}})") << '\n';

    actor.write("CREATE TABLE sdk_maintenance (id BIGINT PRIMARY KEY,note TEXT)",
        "00000000000000000000000000000301");
    actor.write("INSERT INTO sdk_maintenance VALUES (1,'maintenance row')",
        "00000000000000000000000000000302");
    std::cout << "admin_support_preview " << admin(connection, SDP_ADMIN_SUPPORT_PREVIEW) << '\n';
    std::cout << "admin_support_created " << admin(connection, SDP_ADMIN_SUPPORT_CREATE,
        R"({"target_name":"sdk_bundle"})") << '\n';
    const auto queued = admin(connection, SDP_ADMIN_MAINTENANCE_SUBMIT,
        R"({"action":"statistics","request_token":"00000000000000000000000000000303"})");
    std::cout << "admin_maintenance_queued " << queued << '\n';
    const auto job_id = job_selector("job_id", json_id(queued, "job_id"));
    std::cout << "admin_maintenance_done " << await_job(actor, connection, job_id, "succeeded") << '\n';

    const auto generic = job(actor, connection, SDP_JOB_SUBMIT,
        R"({"kind":"maintenance","action":"integrity","request_token":"00000000000000000000000000000304"})");
    const auto generic_id = job_selector("job_id", json_id(generic, "job_id"));
    std::cout << "admin_generic_maintenance " << await_job(actor, connection, generic_id, "succeeded") << '\n';

    const auto scheduled = job(actor, connection, SDP_JOB_SCHEDULE_CREATE,
        R"({"kind":"maintenance","action":"index","rule":{"every_seconds":1},"request_token":"00000000000000000000000000000305"})");
    const auto schedule = job_selector("schedule_id", json_id(scheduled, "schedule_id"));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto status = job(actor, connection, SDP_JOB_SCHEDULE_STATUS, schedule);
        if (status.find("\"last_job_id\":\"") != status.npos)
        {
            const auto scheduled_job = job_selector("job_id", json_id(status, "last_job_id"));
            (void)job(actor, connection, SDP_JOB_SCHEDULE_CANCEL, schedule);
            std::cout << "admin_scheduled_maintenance " << await_job(actor, connection, scheduled_job, "succeeded")
                << '\n';
            return;
        }
        Sleep(30);
    }
    throw std::runtime_error("Native maintenance schedule did not dispatch a job");
}

void job_roundtrip(Handle& submitter, sdp_open_options& options)
{
    Password guest_password;
    require(static_cast<bool>(std::getline(std::cin, guest_password.value)), "Missing generated Job guest credentials");
    SqlClient actor(submitter.value);
    Buffer features;
    expect(sdp_get_capabilities(submitter.value, &features.value, &actor.error_), SDP_OK, actor.error_);
    require(take(features).find("\"jobs_v1\":true") != std::string::npos, "Job capability was unavailable");
    std::cout << "job_capabilities " << job(actor, submitter.value, SDP_JOB_CAPABILITIES) << '\n';
    {
        Buffer rejected;
        expect(sdp_job_command(submitter.value, 99, text("{}"), &rejected.value, &actor.error_),
            SDP_INVALID_ARGUMENT, actor.error_);
        expect(sdp_job_command(submitter.value, SDP_JOB_STATUS, text(R"({"job_id":"invalid"})"),
            &rejected.value, &actor.error_), SDP_INVALID_ARGUMENT, actor.error_);
        require(!rejected.value.data && !actor.error_.unknown_outcome, "invalid Job payload marked uncertain");
    }
    constexpr auto submit = R"({"kind":"compute","count":500000,"request_token":"00000000000000000000000000000601"})"sv;
    const auto queued = job(actor, submitter.value, SDP_JOB_SUBMIT, submit);
    const auto queued_selector = job_selector("job_id", json_id(queued, "job_id"));
    std::cout << "job_queued " << queued << '\n';
    expect(sdp_close(submitter.value, &actor.error_), SDP_OK, actor.error_);
    submitter.value = 0;

    Handle resumed;
    expect(sdp_open(&options, &resumed.value, &actor.error_), SDP_OK, actor.error_);
    SqlClient observer(resumed.value);
    observer.outcome("00000000000000000000000000000601", "job_ledger");
    std::cout << "job_done " << await_job(observer, resumed.value, queued_selector, "succeeded") << '\n';
    std::cout << "job_status " << job(observer, resumed.value, SDP_JOB_STATUS, queued_selector) << '\n';
    std::cout << "job_replayed " << job(observer, resumed.value, SDP_JOB_SUBMIT, submit) << '\n';
    {
        Buffer conflict;
        constexpr auto changed = R"({"kind":"compute","count":2,"request_token":"00000000000000000000000000000601"})"sv;
        expect(sdp_job_command(resumed.value, SDP_JOB_SUBMIT, text(changed), &conflict.value, &observer.error_),
            SDP_INVALID_STATE, observer.error_);
        require(!conflict.value.data, "different input reused a committed Job token");
    }

    const auto durable_before = history(observer, resumed.value, SDP_HISTORY_STATUS);
    expect(sdp_tx_begin(resumed.value, &observer.error_), SDP_OK, observer.error_);
    constexpr auto rollback_job = R"({"kind":"compute","count":19,"request_token":"00000000000000000000000000000606"})"sv;
    const auto staged = job(observer, resumed.value, SDP_JOB_SUBMIT, rollback_job);
    require(staged.find("\"lsn\"") == staged.npos, "Uncommitted Job submit returned a durable revision");
    expect(sdp_tx_rollback(resumed.value, &observer.error_), SDP_OK, observer.error_);
    require(history(observer, resumed.value, SDP_HISTORY_STATUS) == durable_before,
        "Rolled back Job submit published a History revision");
    {
        Buffer withdrawn;
        const auto selector = job_selector("job_id", json_id(staged, "job_id"));
        expect(sdp_job_command(resumed.value, SDP_JOB_STATUS, text(selector), &withdrawn.value, &observer.error_),
            SDP_PERMISSION_DENIED, observer.error_);
    }
    observer.outcome("00000000000000000000000000000606", "job_withdrawn");

    observer.write("CREATE USER job_guest", "00000000000000000000000000000607");
    expect(sdp_password_set(resumed.value, text("job_guest"), {}, text(guest_password.value),
        text("00000000000000000000000000000608"), &observer.error_), SDP_OK, observer.error_);
    {
        auto guest_options = options;
        guest_options.username = text("job_guest");
        guest_options.password = text(guest_password.value);
        Handle guest;
        expect(sdp_open(&guest_options, &guest.value, &observer.error_), SDP_OK, observer.error_);
        guest_password.clear();
        for (const auto operation : {SDP_JOB_STATUS, SDP_JOB_WATCH, SDP_JOB_CANCEL})
        {
            Buffer denied;
            expect(sdp_job_command(guest.value, operation, text(queued_selector), &denied.value, &observer.error_),
                SDP_PERMISSION_DENIED, observer.error_);
            require(!denied.value.data, "unauthorized Job operation returned output");
        }
    }

    constexpr auto cancel_job = R"({"kind":"compute","count":500000000,"request_token":"00000000000000000000000000000602"})"sv;
    const auto waiting = job(observer, resumed.value, SDP_JOB_SUBMIT, cancel_job);
    const auto cancel_selector = job_selector("job_id", json_id(waiting, "job_id"));
    std::string progress;
    const auto progress_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do
    {
        progress = job(observer, resumed.value, SDP_JOB_WATCH, cancel_selector);
        if (progress.find("\"progress\":{") != progress.npos) break;
        Sleep(10);
    } while (std::chrono::steady_clock::now() < progress_deadline);
    require(progress.find("\"progress\":{") != progress.npos, "Job worker did not report progress");
    std::cout << "job_progress " << progress << '\n';
    std::cout << "job_cancel_request " << job(observer, resumed.value, SDP_JOB_CANCEL, cancel_selector) << '\n';
    std::cout << "job_cancelled " << await_job(observer, resumed.value, cancel_selector, "cancelled") << '\n';

    constexpr auto create_schedule = R"({"kind":"compute","count":300,"rule":{"every_seconds":1},"misfire":"run_once","retries":1,"backoff_seconds":1,"request_token":"00000000000000000000000000000603"})"sv;
    const auto schedule = job(observer, resumed.value, SDP_JOB_SCHEDULE_CREATE, create_schedule);
    const auto schedule_selector = job_selector("schedule_id", json_id(schedule, "schedule_id"));
    std::cout << "job_schedule " << schedule << '\n';
    auto observation = schedule;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (observation.find("\"last_job_id\":\"") == observation.npos && std::chrono::steady_clock::now() < deadline)
    {
        Sleep(50);
        observation = job(observer, resumed.value, SDP_JOB_SCHEDULE_STATUS, schedule_selector);
    }
    const auto scheduled = job_selector("job_id", json_id(observation, "last_job_id"));
    std::cout << "job_schedule_observed " << observation << '\n';
    std::cout << "job_schedule_done " << await_job(observer, resumed.value, scheduled, "succeeded") << '\n';
    std::cout << "job_schedule_cancelled " << job(observer, resumed.value, SDP_JOB_SCHEDULE_CANCEL, schedule_selector) << '\n';
}

void raii_roundtrip(const sdp_open_options& options)
{
    using sdp::client::Connection;
    using sdp::client::Statement;
    using sdp::client::Transaction;

    Connection initial(options);
    auto info = initial.session_info();
    const auto expected = json_id(info.view(), "session_id");
    auto moved = std::move(info);
    require(info.view().empty() && json_id(moved.view(), "session_id") == expected &&
        moved.view().find("\"protocol_version\":1") != moved.view().npos, "Moved metadata buffer lost ownership");

    Connection active(options);
    const auto closed = active.native_handle();
    active = std::move(initial);
    require(initial.native_handle() == 0 && json_id(active.session_info().view(), "session_id") == expected,
        "Moving a connection changed its authenticated identity");
    sdp::client::Buffer response;
    sdp_error error{};
    const auto stale = sdp_get_session_info(closed, response.put(), &error);
    expect(stale, SDP_INVALID_STATE, error);

    {
        Statement create(active, "CREATE TABLE raii_rows (id BIGINT PRIMARY KEY)");
        (void)create.execute({}, "00000000000000000000000000000311");
    }
    {
        Transaction transaction(active);
        Statement insert(active, "INSERT INTO raii_rows VALUES (:id)", R"({"id":{"type":"BIGINT"}})");
        Statement moved_insert(std::move(insert));
        require(insert.id().empty(), "Moved statement did not clear the original owner");
        (void)moved_insert.execute(R"({"id":{"t":"i64","v":"1"}})");
    }
    {
        Statement select(active, "SELECT id FROM raii_rows");
        const auto result = json_id(select.execute().view());
        const auto fetch = sdp_fetch(active.native_handle(), text(result), 4, response.put(), &error);
        expect(fetch, SDP_OK, error);
        require(response.view().find("\"rows\":[]") != response.view().npos, "Scope rollback failed");
        const std::string retired(select.id());
        select.close();
        const auto expired = sdp_statement_close(active.native_handle(), text(retired), &error);
        expect(expired, SDP_INVALID_STATE, error);
    }
    {
        Transaction transaction(active);
        Statement insert(active, "INSERT INTO raii_rows VALUES (2)");
        (void)insert.execute();
        constexpr auto token = "00000000000000000000000000000312"sv;
        const auto committed = transaction.commit(token);
        require(!transaction.active() && active.outcome(token).view() == committed.view(),
            "Transaction did not preserve the commit receipt");
    }

    std::array<std::future<std::string>, 4> connections;
    for (auto& connection : connections)
    {
        connection = std::async(std::launch::async, [&] {
            Connection worker(options);
            worker.health();
            return json_id(worker.session_info().view(), "session_id");
        });
    }
    std::set<std::string> ids{expected};
    for (auto& connection : connections) require(ids.insert(connection.get()).second, "Parallel sessions share an ID");
}

int use_package(int argc, char** argv)
{
    require(argc == 4, "Usage: sdp_native_consumer mode port ca-file");
    const std::string_view mode(argv[1]);
    require(sdp_abi_version() == SDP_ABI_VERSION && sdp_library_version() == 0x00000400u,
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
    options.timeout_ms = mode == "handshake_timeout" ? 900 : (mode == "debugger" || mode == "jobs" || mode == "backup" || mode == "admin") ? 12000 : 3500;

    if (mode == "raii")
    {
        raii_roundtrip(options);
        return 0;
    }

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
    if (mode != "debugger" && mode != "restore" && mode != "jobs") password.clear();
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

    if (mode == "admin")
    {
        admin_roundtrip(connection.value);
        return 0;
    }

    if (mode == "backup")
    {
        backup_roundtrip(connection.value);
        return 0;
    }

    if (mode == "ml")
    {
        ml_roundtrip(connection.value);
        return 0;
    }

    if (mode == "jobs")
    {
        job_roundtrip(connection, options);
        return 0;
    }

    if (mode == "decision")
    {
        decision_roundtrip(connection.value);
        return 0;
    }

    if (mode == "restore")
    {
        restore_roundtrip(connection.value, options, password);
        return 0;
    }

    if (mode == "history")
    {
        history_roundtrip(connection.value, options);
        return 0;
    }

    if (mode == "debugger")
    {
        debugger_roundtrip(connection.value, options, password);
        return 0;
    }

    if (mode == "plsql")
    {
        plsql_roundtrip(connection.value);
        return 0;
    }

    if (mode == "security")
    {
        security_roundtrip(connection.value, options);
        return 0;
    }

    if (mode == "sql" || mode == "sql_lost_commit" || mode == "sql_restart")
    {
        if (mode == "sql") sql_roundtrip(connection.value);
        else if (mode == "sql_lost_commit") sql_lost_response(connection.value);
        else sql_restart(connection.value);
        return 0;
    }

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
