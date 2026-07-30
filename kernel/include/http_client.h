#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tcp_client.h"

#define HTTP_CLIENT_MAX_HEADER_BYTES 8192u
#define HTTP_CLIENT_MAX_TRAILER_BYTES 8192u
#define HTTP_CLIENT_MAX_CHUNK_LINE_BYTES 1024u
#define HTTP_CLIENT_MAX_CHUNK_FRAMING_BYTES 16384u
#define HTTP_CLIENT_MAX_INFORMATIONAL_RESPONSES 8u

enum http_client_error {
    HTTP_CLIENT_ERROR_NONE = 0,
    HTTP_CLIENT_ERROR_INVALID_ARGUMENT,
    HTTP_CLIENT_ERROR_INVALID_HOST,
    HTTP_CLIENT_ERROR_INVALID_PATH,
    HTTP_CLIENT_ERROR_REQUEST_TOO_LARGE,
    HTTP_CLIENT_ERROR_TCP,
    HTTP_CLIENT_ERROR_HEADER_TOO_LARGE,
    HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE,
    HTTP_CLIENT_ERROR_UNSUPPORTED_VERSION,
    HTTP_CLIENT_ERROR_MALFORMED_HEADER,
    HTTP_CLIENT_ERROR_INVALID_CONTENT_LENGTH,
    HTTP_CLIENT_ERROR_CONFLICTING_FRAMING,
    HTTP_CLIENT_ERROR_UNSUPPORTED_TRANSFER_ENCODING,
    HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY,
    HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE,
    HTTP_CLIENT_ERROR_TRUNCATED_BODY,
    HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA,
    HTTP_CLIENT_ERROR_REDIRECT_LOCATION_MISSING,
    HTTP_CLIENT_ERROR_INVALID_REDIRECT_LOCATION,
    HTTP_CLIENT_ERROR_REDIRECT_LOCATION_TOO_LARGE,
    HTTP_CLIENT_ERROR_INVALID_INFORMATIONAL_FRAMING,
    HTTP_CLIENT_ERROR_TOO_MANY_INFORMATIONAL_RESPONSES,
    HTTP_CLIENT_ERROR_INFORMATIONAL_RESPONSE_WITHOUT_FINAL,
    HTTP_CLIENT_ERROR_UNSUPPORTED_PROTOCOL_SWITCH
};

/*
 * Every pointer in this view references the caller-owned response byte buffer.
 * No memory is allocated; chunked bodies are compacted within that same
 * buffer. The view remains valid only while the buffer remains unchanged.
 */
struct http_response {
    unsigned int version_major;
    unsigned int version_minor;
    unsigned int status_code;

    const uint8_t* reason_phrase;
    size_t reason_phrase_length;
    const uint8_t* header_fields;
    size_t header_fields_length;
    const uint8_t* body;
    size_t body_length;

    bool has_content_length;
    size_t content_length;
    bool has_transfer_encoding;
    bool chunked_transfer;
    bool connection_close;

    bool has_location;
    const uint8_t* location;
    size_t location_length;
};

/*
 * Builds an injection-safe HTTP/1.1 request with Connection: close.
 * request_buffer is caller-owned,
 * and the returned length excludes an optional trailing NUL.
 */
enum http_client_error http_client_build_get(
    const char* host,
    const char* path,
    void* request_buffer,
    size_t request_capacity,
    size_t* request_length);

/*
 * Parses one complete response, including a bounded chain of informational
 * responses before the final response. Close-delimited and chunked bodies are
 * supported. Chunked data is validated first and then compacted in place, so
 * response_bytes must be writable even though the returned view is read-only.
 * Unsupported transfer codings and protocol switches are rejected instead of
 * being exposed as if encoded bytes were an ordinary response body.
 */
enum http_client_error http_client_parse_response(
    void* response_bytes,
    size_t response_length,
    struct http_response* response);

/*
 * Reports only body bytes that can be identified without mutating an
 * incomplete response. Informational headers are skipped and Content-Length
 * is honored. Chunked progress remains zero until the complete response is
 * decoded by http_client_parse_response(), because wire framing bytes are not
 * decoded body bytes.
 */
size_t http_client_response_body_progress(
    const void* response_bytes,
    size_t response_length,
    size_t body_capacity);

bool http_response_find_header(
    const struct http_response* response,
    const char* name,
    const uint8_t** value,
    size_t* value_length);

/*
 * These helpers do not follow or resolve redirects; the caller owns URL
 * resolution, supported-scheme policy, hop limits, and loop detection.
 * Locations are restricted to bounded ASCII URI-reference characters with
 * well-formed percent escapes.
 */
bool http_response_is_redirect(const struct http_response* response);
bool http_response_redirect_location(
    const struct http_response* response,
    const uint8_t** location,
    size_t* location_length);
enum http_client_error http_response_copy_redirect_location(
    const struct http_response* response,
    char* destination,
    size_t destination_capacity,
    size_t* location_length);

/*
 * Runs connect, GET, peer-close wait, local FIN, and parse. The TCP client and
 * its receive buffer, request scratch buffer, and response view are all owned
 * by the caller. request_buffer must not overlap the TCP receive buffer.
 */
enum http_client_error http_client_get(
    struct tcp_client* tcp,
    uint32_t remote_ip,
    uint16_t remote_port,
    uint32_t initial_sequence,
    const char* host,
    const char* path,
    void* request_buffer,
    size_t request_capacity,
    struct http_response* response);

const char* http_client_error_string(enum http_client_error error);

#endif /* HTTP_CLIENT_H */
