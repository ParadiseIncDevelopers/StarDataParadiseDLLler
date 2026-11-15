#ifndef STARDATAPARADISEDLLLER_H
#define STARDATAPARADISEDLLLER_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(STARDATAPARADISEDLLLER_EXPORTS)
#    define SDP_API __declspec(dllexport)
#  else
#    define SDP_API __declspec(dllimport)
#  endif
#  define SDP_CALL __cdecl
#else
#  define SDP_API
#  define SDP_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SDP_ABI_VERSION 1u

typedef int32_t sdp_status;
typedef uint64_t sdp_connection;

enum
{
    SDP_OK = 0,
    SDP_MALFORMED_MESSAGE = 1001,
    SDP_MESSAGE_TOO_LARGE = 1002,
    SDP_INCOMPATIBLE_PROTOCOL = 1003,
    SDP_FEATURE_UNSUPPORTED = 1004,
    SDP_AUTH_FAILED = 1005,
    SDP_PERMISSION_DENIED = 1006,
    SDP_MODULE_UNAVAILABLE = 1007,
    SDP_TIMEOUT = 1008,
    SDP_CANCELLED = 1009,
    SDP_CONNECTION_LOST = 1010,
    SDP_TLS_FAILED = 1011,
    SDP_INVALID_ARGUMENT = 1012,
    SDP_INVALID_STATE = 1013,
    SDP_RESOURCE_LIMIT = 1014,
    SDP_INTERNAL_ERROR = 1015,
    SDP_OUT_OF_MEMORY = 1016
};

typedef struct sdp_text
{
    const char* data;
    uint32_t size;
} sdp_text;

typedef struct sdp_buffer
{
    uint8_t* data;
    uint32_t size;
} sdp_buffer;

typedef struct sdp_error
{
    sdp_status code;
    uint32_t retryable;
    uint32_t unknown_outcome;
    uint32_t line;
    uint32_t column;
    char sqlstate[6];
    char message[96];
    char correlation_id[80];
} sdp_error;

typedef struct sdp_open_options
{
    uint32_t struct_size;
    uint32_t abi_version;
    sdp_text address;
    sdp_text server_name;
    sdp_text ca_file;
    sdp_text username;
    sdp_text password;
    uint16_t port;
    uint16_t reserved;
    uint32_t timeout_ms;
} sdp_open_options;

typedef void (SDP_CALL *sdp_completion)(void* context, sdp_status status, const sdp_error* error);

// Product version is encoded as 0x00MMmmpp (major, minor, patch).
SDP_API uint32_t SDP_CALL sdp_library_version(void);
SDP_API uint32_t SDP_CALL sdp_abi_version(void);

// Open verifies the server certificate and completes hello, auth and capability discovery.
SDP_API sdp_status SDP_CALL sdp_open(const sdp_open_options* options, sdp_connection* connection, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_get_capabilities(sdp_connection connection, sdp_buffer* output, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_health(sdp_connection connection, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_prepare(sdp_connection connection, sdp_text sql, sdp_buffer* statement_id, sdp_error* error);

// Separate SQL text and JSON objects of named wire v1 parameter declarations and values.
// Empty parameter objects may be supplied as zero-length sdp_text. Statement IDs are opaque bytes.
SDP_API sdp_status SDP_CALL sdp_prepare_typed(sdp_connection connection, sdp_text sql,
    sdp_text parameter_types_json, sdp_buffer* statement_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_execute(sdp_connection connection, sdp_text statement_id, sdp_text parameters_json,
    sdp_text request_token, uint32_t row_limit, sdp_buffer* response_json, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_fetch(sdp_connection connection, sdp_text result_id, uint32_t batch_rows,
    sdp_buffer* response_json, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_statement_close(sdp_connection connection, sdp_text statement_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_result_cancel(sdp_connection connection, sdp_text result_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_metadata(sdp_connection connection, sdp_buffer* response_json, sdp_error* error);

// A generated request token identifies a write. On an unknown outcome reconnect and query the token.
SDP_API sdp_status SDP_CALL sdp_tx_begin(sdp_connection connection, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_tx_commit(sdp_connection connection, sdp_text request_token,
    sdp_buffer* response_json, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_tx_rollback(sdp_connection connection, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_tx_savepoint(sdp_connection connection, sdp_text name, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_tx_rollback_to(sdp_connection connection, sdp_text name, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_tx_release(sdp_connection connection, sdp_text name, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_tx_outcome(sdp_connection connection, sdp_text request_token,
    sdp_buffer* response_json, sdp_error* error);

// Reset preserves the authenticated identity and releases transaction, statement and result state.
SDP_API sdp_status SDP_CALL sdp_session_reset(sdp_connection connection, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_session_list(sdp_connection connection, sdp_buffer* response_json, sdp_error* error);
// Use a separate control connection to cancel an in-flight request. Request ID 0 selects the active request.
SDP_API sdp_status SDP_CALL sdp_query_cancel(sdp_connection connection, sdp_text session_id,
    uint32_t request_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_session_terminate(sdp_connection connection, sdp_text session_id, sdp_error* error);
// An empty old password requests a privileged reset. The caller owns the input bytes.
SDP_API sdp_status SDP_CALL sdp_password_set(sdp_connection connection, sdp_text username,
    sdp_text old_password, sdp_text new_password, sdp_text request_token, sdp_error* error);

// Debug controls use a connection distinct from the target routine's connection.
typedef uint32_t sdp_debug_step_mode;
enum
{
    SDP_STEP_INTO = 1,
    SDP_STEP_OVER = 2,
    SDP_STEP_OUT = 3
};

SDP_API sdp_status SDP_CALL sdp_debug_open(sdp_connection connection, sdp_text target_session, sdp_text routine,
    uint32_t revision, sdp_buffer* debug_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_breakpoint(sdp_connection connection, sdp_text debug_id, uint32_t line,
    uint32_t revision, uint32_t enabled, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_status(sdp_connection connection, sdp_text debug_id,
    sdp_buffer* response_json, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_continue(sdp_connection connection, sdp_text debug_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_step(sdp_connection connection, sdp_text debug_id,
    sdp_debug_step_mode mode, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_watch(sdp_connection connection, sdp_text debug_id, sdp_text name,
    sdp_buffer* response_json, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_options(sdp_connection connection, sdp_text debug_id,
    uint32_t exception_stop, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_debug_close(sdp_connection connection, sdp_text debug_id, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_profile_last(sdp_connection connection, sdp_buffer* response_json, sdp_error* error);

// Available only on isolated test instances. The Server rolls back the test invocation.
SDP_API sdp_status SDP_CALL sdp_routine_test(sdp_connection connection, sdp_text statement_id,
    sdp_text parameters_json, sdp_text expected_json, sdp_buffer* response_json, sdp_error* error);

// History request and response bodies are UTF-8 JSON objects. The Server applies current permissions.
typedef uint32_t sdp_history_operation;
enum
{
    SDP_HISTORY_LOG = 1,
    SDP_HISTORY_DIFF = 2,
    SDP_HISTORY_READ = 3,
    SDP_HISTORY_VERIFY = 4,
    SDP_HISTORY_STATUS = 5,
    SDP_HISTORY_RETAIN = 6,
    SDP_HISTORY_PURGE = 7,
    SDP_HISTORY_PIN = 8,
    SDP_HISTORY_UNPIN = 9,
    SDP_HISTORY_RESTORE = 10,
    SDP_HISTORY_REVERT = 11
};

SDP_API sdp_status SDP_CALL sdp_history_command(sdp_connection connection, sdp_history_operation operation,
    sdp_text request_json, sdp_buffer* response_json, sdp_error* error);

// A queued health operation invokes completion once. The error pointer is valid during completion.
SDP_API sdp_status SDP_CALL sdp_health_async(sdp_connection connection, sdp_completion completion,
    void* context, sdp_error* error);
SDP_API sdp_status SDP_CALL sdp_cancel(sdp_connection connection, sdp_error* error);

// Close waits for in-flight work and callbacks. Invoke it from a thread outside callbacks.
SDP_API sdp_status SDP_CALL sdp_close(sdp_connection connection, sdp_error* error);
SDP_API void SDP_CALL sdp_buffer_free(sdp_buffer* buffer);

#ifdef __cplusplus
}
#endif

#endif
