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
