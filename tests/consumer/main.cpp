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

int use_package(int argc, char** argv)
{
    require(argc == 4, "Usage: sdp_native_consumer mode port ca-file");
    const std::string_view mode(argv[1]);
    require(sdp_abi_version() == SDP_ABI_VERSION && sdp_library_version() == 0x00000200u,
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
    options.timeout_ms = mode == "handshake_timeout" ? 900 : mode == "debugger" ? 12000 : 3500;

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
    if (mode != "debugger" && mode != "restore") password.clear();
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
