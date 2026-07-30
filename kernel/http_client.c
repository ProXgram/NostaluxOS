#include "http_client.h"

static void clear_response(struct http_response* response) {
    uint8_t* bytes = (uint8_t*)response;

    for (size_t i = 0; i < sizeof(*response); i++) {
        bytes[i] = 0;
    }
}

static uint8_t ascii_lower(uint8_t character) {
    if (character >= (uint8_t)'A' &&
        character <= (uint8_t)'Z') {
        return (uint8_t)(character + ('a' - 'A'));
    }
    return character;
}

static bool span_equals_text_ignore_case(
    const uint8_t* bytes,
    size_t length,
    const char* text) {
    size_t i = 0;

    while (i < length && text[i] != '\0') {
        if (ascii_lower(bytes[i]) !=
            ascii_lower((uint8_t)text[i])) {
            return false;
        }
        i++;
    }
    return i == length && text[i] == '\0';
}

static bool valid_header_name_byte(uint8_t character) {
    if ((character >= (uint8_t)'a' &&
         character <= (uint8_t)'z') ||
        (character >= (uint8_t)'A' &&
         character <= (uint8_t)'Z') ||
        (character >= (uint8_t)'0' &&
         character <= (uint8_t)'9')) {
        return true;
    }

    switch (character) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
    }
    return false;
}

static bool append_bytes(
    uint8_t* output,
    size_t capacity,
    size_t* length,
    const uint8_t* bytes,
    size_t byte_count) {
    if (*length > capacity ||
        byte_count > capacity - *length) {
        return false;
    }
    for (size_t i = 0; i < byte_count; i++) {
        output[*length + i] = bytes[i];
    }
    *length += byte_count;
    return true;
}

static bool append_text(
    uint8_t* output,
    size_t capacity,
    size_t* length,
    const char* text) {
    size_t text_length = 0;

    while (text[text_length] != '\0') {
        text_length++;
    }
    return append_bytes(
        output,
        capacity,
        length,
        (const uint8_t*)text,
        text_length);
}

static bool valid_host(const char* host) {
    size_t length = 0;

    if (host == NULL || host[0] == '\0') {
        return false;
    }
    while (host[length] != '\0') {
        uint8_t character = (uint8_t)host[length];

        if (character <= (uint8_t)' ' ||
            character >= 0x7Fu ||
            character == (uint8_t)'/') {
            return false;
        }
        length++;
    }
    return true;
}

static bool valid_path(const char* path) {
    size_t length = 0;

    if (path == NULL || path[0] != '/') {
        return false;
    }
    while (path[length] != '\0') {
        uint8_t character = (uint8_t)path[length];

        if (character <= (uint8_t)' ' ||
            character >= 0x7Fu) {
            return false;
        }
        length++;
    }
    return true;
}

enum http_client_error http_client_build_get(
    const char* host,
    const char* path,
    void* request_buffer,
    size_t request_capacity,
    size_t* request_length) {
    static const char request_prefix[] = "GET ";
    static const char host_prefix[] = " HTTP/1.1\r\nHost: ";
    static const char request_suffix[] =
        "\r\nConnection: close\r\n"
        "User-Agent: Nostalux/1.0\r\n"
        "Accept: */*\r\n\r\n";
    uint8_t* output = (uint8_t*)request_buffer;
    size_t length = 0;

    if (request_length != NULL) {
        *request_length = 0;
    }
    if (request_buffer == NULL || request_length == NULL) {
        return HTTP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
    if (!valid_host(host)) {
        return HTTP_CLIENT_ERROR_INVALID_HOST;
    }
    if (!valid_path(path)) {
        return HTTP_CLIENT_ERROR_INVALID_PATH;
    }

    if (!append_text(
            output,
            request_capacity,
            &length,
            request_prefix) ||
        !append_text(
            output,
            request_capacity,
            &length,
            path) ||
        !append_text(
            output,
            request_capacity,
            &length,
            host_prefix) ||
        !append_text(
            output,
            request_capacity,
            &length,
            host) ||
        !append_text(
            output,
            request_capacity,
            &length,
            request_suffix)) {
        return HTTP_CLIENT_ERROR_REQUEST_TOO_LARGE;
    }

    if (length < request_capacity) {
        output[length] = '\0';
    }
    *request_length = length;
    return HTTP_CLIENT_ERROR_NONE;
}

static bool find_crlf(
    const uint8_t* bytes,
    size_t start,
    size_t limit,
    size_t* position) {
    for (size_t i = start; i + 1 < limit; i++) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            *position = i;
            return true;
        }
    }
    return false;
}

static bool find_header_separator(
    const uint8_t* bytes,
    size_t start,
    size_t limit,
    size_t* position) {
    for (size_t i = start; i + 3 < limit; i++) {
        if (bytes[i] == '\r' &&
            bytes[i + 1] == '\n' &&
            bytes[i + 2] == '\r' &&
            bytes[i + 3] == '\n') {
            *position = i;
            return true;
        }
    }
    return false;
}

static bool parse_content_length(
    const uint8_t* value,
    size_t value_length,
    size_t* parsed) {
    size_t result = 0;
    const size_t maximum = (size_t)-1;

    if (value_length == 0) {
        return false;
    }
    for (size_t i = 0; i < value_length; i++) {
        size_t digit;

        if (value[i] < (uint8_t)'0' ||
            value[i] > (uint8_t)'9') {
            return false;
        }
        digit = (size_t)(value[i] - (uint8_t)'0');
        if (result > (maximum - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }
    *parsed = result;
    return true;
}

static bool span_has_token_ignore_case(
    const uint8_t* value,
    size_t value_length,
    const char* token) {
    size_t cursor = 0;

    while (cursor < value_length) {
        size_t start;
        size_t end;

        while (cursor < value_length &&
               (value[cursor] == (uint8_t)' ' ||
                value[cursor] == (uint8_t)'\t' ||
                value[cursor] == (uint8_t)',')) {
            cursor++;
        }
        start = cursor;
        while (cursor < value_length &&
               value[cursor] != (uint8_t)',') {
            cursor++;
        }
        end = cursor;
        while (end > start &&
               (value[end - 1] == (uint8_t)' ' ||
                value[end - 1] == (uint8_t)'\t')) {
            end--;
        }
        if (span_equals_text_ignore_case(
                &value[start],
                end - start,
                token)) {
            return true;
        }
    }
    return false;
}

struct http_header_view {
    const uint8_t* name;
    size_t name_length;
    const uint8_t* value;
    size_t value_length;
};

struct http_parse_context {
    bool transfer_encoding_seen;
    bool chunked_seen;
    bool identity_seen;
    bool unsupported_transfer_coding;
};

static enum http_client_error parse_header_view(
    const uint8_t* line,
    size_t line_length,
    struct http_header_view* view) {
    size_t colon = 0;
    size_t value_start;
    size_t value_end;

    while (colon < line_length &&
           line[colon] != (uint8_t)':') {
        if (!valid_header_name_byte(line[colon])) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
        colon++;
    }
    if (colon == 0 || colon == line_length) {
        return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
    }

    value_start = colon + 1;
    while (value_start < line_length &&
           (line[value_start] == (uint8_t)' ' ||
            line[value_start] == (uint8_t)'\t')) {
        value_start++;
    }
    value_end = line_length;
    while (value_end > value_start &&
           (line[value_end - 1] == (uint8_t)' ' ||
            line[value_end - 1] == (uint8_t)'\t')) {
        value_end--;
    }
    for (size_t i = value_start; i < value_end; i++) {
        if ((line[i] < (uint8_t)' ' &&
             line[i] != (uint8_t)'\t') ||
            line[i] == 0x7Fu) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
    }

    view->name = line;
    view->name_length = colon;
    view->value = &line[value_start];
    view->value_length = value_end - value_start;
    return HTTP_CLIENT_ERROR_NONE;
}

static void skip_ows(
    const uint8_t* bytes,
    size_t length,
    size_t* cursor) {
    while (*cursor < length &&
           (bytes[*cursor] == (uint8_t)' ' ||
            bytes[*cursor] == (uint8_t)'\t')) {
        (*cursor)++;
    }
}

static bool parse_token(
    const uint8_t* bytes,
    size_t length,
    size_t* cursor,
    size_t* token_start,
    size_t* token_length) {
    size_t start = *cursor;

    while (*cursor < length &&
           valid_header_name_byte(bytes[*cursor])) {
        (*cursor)++;
    }
    if (*cursor == start) {
        return false;
    }
    if (token_start != NULL) {
        *token_start = start;
    }
    if (token_length != NULL) {
        *token_length = *cursor - start;
    }
    return true;
}

static bool parse_quoted_string(
    const uint8_t* bytes,
    size_t length,
    size_t* cursor) {
    if (*cursor >= length ||
        bytes[*cursor] != (uint8_t)'"') {
        return false;
    }
    (*cursor)++;
    while (*cursor < length) {
        uint8_t character = bytes[*cursor];

        if (character == (uint8_t)'"') {
            (*cursor)++;
            return true;
        }
        if (character == (uint8_t)'\\') {
            (*cursor)++;
            if (*cursor >= length) {
                return false;
            }
            character = bytes[*cursor];
            if (!((character >= 0x20u &&
                   character <= 0x7Eu) ||
                  character == (uint8_t)'\t' ||
                  character >= 0x80u)) {
                return false;
            }
            (*cursor)++;
            continue;
        }
        if (!((character == (uint8_t)'\t') ||
              character == (uint8_t)' ' ||
              character == (uint8_t)'!' ||
              (character >= 0x23u && character <= 0x5Bu) ||
              (character >= 0x5Du && character <= 0x7Eu) ||
              character >= 0x80u)) {
            return false;
        }
        (*cursor)++;
    }
    return false;
}

static bool parse_required_parameter(
    const uint8_t* bytes,
    size_t length,
    size_t* cursor) {
    skip_ows(bytes, length, cursor);
    if (!parse_token(
            bytes, length, cursor, NULL, NULL)) {
        return false;
    }
    skip_ows(bytes, length, cursor);
    if (*cursor >= length ||
        bytes[*cursor] != (uint8_t)'=') {
        return false;
    }
    (*cursor)++;
    skip_ows(bytes, length, cursor);
    if (*cursor < length &&
        bytes[*cursor] == (uint8_t)'"') {
        return parse_quoted_string(bytes, length, cursor);
    }
    return parse_token(
        bytes, length, cursor, NULL, NULL);
}

static enum http_client_error parse_transfer_encoding(
    const uint8_t* value,
    size_t value_length,
    struct http_parse_context* context) {
    size_t cursor = 0;

    if (value_length == 0) {
        return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
    }
    for (;;) {
        size_t coding_start;
        size_t coding_length;
        bool has_parameter = false;
        bool is_chunked;
        bool is_identity;

        skip_ows(value, value_length, &cursor);
        if (!parse_token(
                value,
                value_length,
                &cursor,
                &coding_start,
                &coding_length)) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
        if (context->chunked_seen ||
            context->identity_seen) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
        is_chunked = span_equals_text_ignore_case(
            &value[coding_start],
            coding_length,
            "chunked");
        is_identity = span_equals_text_ignore_case(
            &value[coding_start],
            coding_length,
            "identity");

        skip_ows(value, value_length, &cursor);
        while (cursor < value_length &&
               value[cursor] == (uint8_t)';') {
            has_parameter = true;
            cursor++;
            if (!parse_required_parameter(
                    value, value_length, &cursor)) {
                return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
            }
            skip_ows(value, value_length, &cursor);
        }
        if ((is_chunked || is_identity) &&
            has_parameter) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }

        context->transfer_encoding_seen = true;
        if (is_chunked) {
            context->chunked_seen = true;
        } else if (is_identity) {
            if (context->transfer_encoding_seen &&
                (context->unsupported_transfer_coding ||
                 context->chunked_seen)) {
                return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
            }
            context->identity_seen = true;
        } else {
            context->unsupported_transfer_coding = true;
        }

        skip_ows(value, value_length, &cursor);
        if (cursor == value_length) {
            return HTTP_CLIENT_ERROR_NONE;
        }
        if (value[cursor] != (uint8_t)',') {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
        cursor++;
        skip_ows(value, value_length, &cursor);
        if (cursor == value_length) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
    }
}

static enum http_client_error parse_header_line(
    const uint8_t* line,
    size_t line_length,
    struct http_response* response,
    struct http_parse_context* context) {
    struct http_header_view header;
    enum http_client_error error =
        parse_header_view(line, line_length, &header);

    if (error != HTTP_CLIENT_ERROR_NONE) {
        return error;
    }
    if (span_equals_text_ignore_case(
            header.name,
            header.name_length,
            "content-length")) {
        size_t content_length;

        if (!parse_content_length(
                header.value,
                header.value_length,
                &content_length)) {
            return HTTP_CLIENT_ERROR_INVALID_CONTENT_LENGTH;
        }
        if (response->has_content_length &&
            response->content_length != content_length) {
            return HTTP_CLIENT_ERROR_INVALID_CONTENT_LENGTH;
        }
        response->has_content_length = true;
        response->content_length = content_length;
    } else if (span_equals_text_ignore_case(
                   header.name,
                   header.name_length,
                   "transfer-encoding")) {
        error = parse_transfer_encoding(
            header.value,
            header.value_length,
            context);
        if (error != HTTP_CLIENT_ERROR_NONE) {
            return error;
        }
        response->has_transfer_encoding = true;
    } else if (span_equals_text_ignore_case(
                   header.name,
                   header.name_length,
                   "connection") &&
               span_has_token_ignore_case(
                   header.value,
                   header.value_length,
                   "close")) {
        response->connection_close = true;
    } else if (span_equals_text_ignore_case(
                   header.name,
                   header.name_length,
                   "location")) {
        if (response->has_location) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
        response->has_location = true;
        response->location = header.value;
        response->location_length = header.value_length;
    }

    return HTTP_CLIENT_ERROR_NONE;
}

static bool hex_digit_value(uint8_t character, uint8_t* value) {
    if (character >= (uint8_t)'0' &&
        character <= (uint8_t)'9') {
        *value = (uint8_t)(character - (uint8_t)'0');
        return true;
    }
    character = ascii_lower(character);
    if (character >= (uint8_t)'a' &&
        character <= (uint8_t)'f') {
        *value =
            (uint8_t)(character - (uint8_t)'a' + 10u);
        return true;
    }
    return false;
}

static bool validate_chunk_extensions(
    const uint8_t* line,
    size_t line_length,
    size_t cursor) {
    while (cursor < line_length) {
        skip_ows(line, line_length, &cursor);
        if (cursor == line_length) {
            return false;
        }
        if (line[cursor] != (uint8_t)';') {
            return false;
        }
        cursor++;
        skip_ows(line, line_length, &cursor);
        if (!parse_token(
                line,
                line_length,
                &cursor,
                NULL,
                NULL)) {
            return false;
        }
        skip_ows(line, line_length, &cursor);
        if (cursor < line_length &&
            line[cursor] == (uint8_t)'=') {
            cursor++;
            skip_ows(line, line_length, &cursor);
            if (cursor < line_length &&
                line[cursor] == (uint8_t)'"') {
                if (!parse_quoted_string(
                        line,
                        line_length,
                        &cursor)) {
                    return false;
                }
            } else if (!parse_token(
                           line,
                           line_length,
                           &cursor,
                           NULL,
                           NULL)) {
                return false;
            }
            skip_ows(line, line_length, &cursor);
        }
    }
    return true;
}

static enum http_client_error parse_chunk_size_line(
    const uint8_t* line,
    size_t line_length,
    size_t* chunk_size) {
    size_t cursor = 0;
    size_t value = 0;
    const size_t maximum = (size_t)-1;
    bool has_digit = false;

    while (cursor < line_length) {
        uint8_t digit;

        if (!hex_digit_value(line[cursor], &digit)) {
            break;
        }
        has_digit = true;
        if (value > (maximum - digit) / 16u) {
            return HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY;
        }
        value = value * 16u + digit;
        cursor++;
    }
    if (!has_digit ||
        !validate_chunk_extensions(
            line, line_length, cursor)) {
        return HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY;
    }
    *chunk_size = value;
    return HTTP_CLIENT_ERROR_NONE;
}

static enum http_client_error find_chunk_line_end(
    const uint8_t* bytes,
    size_t length,
    size_t start,
    size_t* line_end) {
    size_t remaining;
    size_t limit;

    if (start > length) {
        return HTTP_CLIENT_ERROR_TRUNCATED_BODY;
    }
    remaining = length - start;
    limit = remaining;
    if (limit > HTTP_CLIENT_MAX_CHUNK_LINE_BYTES + 2u) {
        limit = HTTP_CLIENT_MAX_CHUNK_LINE_BYTES + 2u;
    }
    if (find_crlf(
            bytes,
            start,
            start + limit,
            line_end)) {
        if (*line_end - start >
            HTTP_CLIENT_MAX_CHUNK_LINE_BYTES) {
            return HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
        }
        return HTTP_CLIENT_ERROR_NONE;
    }
    if (remaining > HTTP_CLIENT_MAX_CHUNK_LINE_BYTES) {
        return HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
    }
    return HTTP_CLIENT_ERROR_TRUNCATED_BODY;
}

static bool forbidden_trailer_name(
    const uint8_t* name,
    size_t name_length) {
    static const char* const forbidden[] = {
        "connection",
        "content-length",
        "host",
        "location",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
    };

    for (size_t i = 0;
         i < sizeof(forbidden) / sizeof(forbidden[0]);
         i++) {
        if (span_equals_text_ignore_case(
                name, name_length, forbidden[i])) {
            return true;
        }
    }
    return false;
}

static enum http_client_error validate_chunk_trailers(
    const uint8_t* bytes,
    size_t length,
    size_t start,
    size_t* message_end) {
    size_t cursor = start;

    for (;;) {
        size_t line_end;
        size_t search_limit;
        struct http_header_view header;
        enum http_client_error error;

        if (cursor > length ||
            cursor - start >
                HTTP_CLIENT_MAX_TRAILER_BYTES) {
            return
                HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
        }
        search_limit = length;
        if (length - start >
            HTTP_CLIENT_MAX_TRAILER_BYTES) {
            search_limit =
                start + HTTP_CLIENT_MAX_TRAILER_BYTES;
        }
        if (!find_crlf(
                bytes, cursor, search_limit, &line_end)) {
            if (length - start >
                HTTP_CLIENT_MAX_TRAILER_BYTES) {
                return
                    HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
            }
            return HTTP_CLIENT_ERROR_TRUNCATED_BODY;
        }
        if (line_end + 2u - start >
            HTTP_CLIENT_MAX_TRAILER_BYTES) {
            return
                HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
        }
        if (line_end == cursor) {
            *message_end = line_end + 2u;
            return HTTP_CLIENT_ERROR_NONE;
        }

        error = parse_header_view(
            &bytes[cursor],
            line_end - cursor,
            &header);
        if (error != HTTP_CLIENT_ERROR_NONE ||
            forbidden_trailer_name(
                header.name,
                header.name_length)) {
            return
                HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY;
        }
        cursor = line_end + 2u;
    }
}

static enum http_client_error walk_chunked_body(
    uint8_t* bytes,
    size_t encoded_length,
    bool compact,
    size_t* decoded_length) {
    size_t read_cursor = 0;
    size_t write_cursor = 0;
    size_t framing_bytes = 0;

    for (;;) {
        size_t line_end;
        size_t chunk_size;
        enum http_client_error error =
            find_chunk_line_end(
                bytes,
                encoded_length,
                read_cursor,
                &line_end);

        if (error != HTTP_CLIENT_ERROR_NONE) {
            return error;
        }
        const size_t size_line_bytes =
            line_end - read_cursor + 2u;
        if (size_line_bytes >
            HTTP_CLIENT_MAX_CHUNK_FRAMING_BYTES - framing_bytes) {
            return HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
        }
        framing_bytes += size_line_bytes;
        error = parse_chunk_size_line(
            &bytes[read_cursor],
            line_end - read_cursor,
            &chunk_size);
        if (error != HTTP_CLIENT_ERROR_NONE) {
            return error;
        }
        read_cursor = line_end + 2u;
        if (chunk_size == 0) {
            size_t message_end;

            error = validate_chunk_trailers(
                bytes,
                encoded_length,
                read_cursor,
                &message_end);
            if (error != HTTP_CLIENT_ERROR_NONE) {
                return error;
            }
            if (message_end != encoded_length) {
                return
                    HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA;
            }
            *decoded_length = write_cursor;
            return HTTP_CLIENT_ERROR_NONE;
        }
        if (read_cursor > encoded_length ||
            chunk_size > encoded_length - read_cursor) {
            return HTTP_CLIENT_ERROR_TRUNCATED_BODY;
        }
        if (encoded_length - read_cursor - chunk_size < 2u) {
            return HTTP_CLIENT_ERROR_TRUNCATED_BODY;
        }
        if (bytes[read_cursor + chunk_size] !=
                (uint8_t)'\r' ||
            bytes[read_cursor + chunk_size + 1u] !=
                (uint8_t)'\n') {
            return
                HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY;
        }
        if (framing_bytes >
            HTTP_CLIENT_MAX_CHUNK_FRAMING_BYTES - 2u) {
            return HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE;
        }
        framing_bytes += 2u;
        if (compact) {
            for (size_t i = 0; i < chunk_size; i++) {
                bytes[write_cursor + i] =
                    bytes[read_cursor + i];
            }
        }
        write_cursor += chunk_size;
        read_cursor += chunk_size + 2u;
    }
}

static enum http_client_error decode_chunked_body(
    uint8_t* bytes,
    size_t encoded_length,
    size_t* decoded_length) {
    size_t validated_length = 0;
    enum http_client_error error = walk_chunked_body(
        bytes,
        encoded_length,
        false,
        &validated_length);

    if (error != HTTP_CLIENT_ERROR_NONE) {
        return error;
    }
    error = walk_chunked_body(
        bytes,
        encoded_length,
        true,
        decoded_length);
    if (error != HTTP_CLIENT_ERROR_NONE ||
        *decoded_length != validated_length) {
        return HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY;
    }
    return HTTP_CLIENT_ERROR_NONE;
}

static bool redirect_status(unsigned int status_code) {
    return status_code == 300u ||
           status_code == 301u ||
           status_code == 302u ||
           status_code == 303u ||
           status_code == 307u ||
           status_code == 308u;
}

static bool uri_character_allowed(uint8_t character) {
    if ((character >= (uint8_t)'a' &&
         character <= (uint8_t)'z') ||
        (character >= (uint8_t)'A' &&
         character <= (uint8_t)'Z') ||
        (character >= (uint8_t)'0' &&
         character <= (uint8_t)'9')) {
        return true;
    }
    switch (character) {
        case '-':
        case '.':
        case '_':
        case '~':
        case ':':
        case '/':
        case '?':
        case '#':
        case '[':
        case ']':
        case '@':
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
            return true;
    }
    return false;
}

static bool valid_uri_reference(
    const uint8_t* location,
    size_t location_length) {
    if (location == NULL || location_length == 0) {
        return false;
    }
    for (size_t i = 0; i < location_length; i++) {
        uint8_t character = location[i];

        if (character == (uint8_t)'%') {
            uint8_t ignored;

            if (i + 2u >= location_length ||
                !hex_digit_value(location[i + 1u], &ignored) ||
                !hex_digit_value(location[i + 2u], &ignored)) {
                return false;
            }
            i += 2u;
        } else if (!uri_character_allowed(character)) {
            return false;
        }
    }
    return true;
}

static enum http_client_error parse_response_head(
    const uint8_t* bytes,
    size_t response_length,
    size_t message_start,
    size_t header_limit,
    struct http_response* response,
    size_t* out_body_start) {
    size_t status_end;
    size_t separator;
    size_t fields_start;
    size_t fields_end;
    size_t cursor;
    enum http_client_error error;
    struct http_parse_context context = {0};

    if (message_start >= response_length ||
        message_start >= header_limit) {
        return response_length >= HTTP_CLIENT_MAX_HEADER_BYTES
                   ? HTTP_CLIENT_ERROR_HEADER_TOO_LARGE
                   : HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE;
    }
    if (!find_crlf(
            bytes,
            message_start,
            header_limit,
            &status_end)) {
        if (response_length >= HTTP_CLIENT_MAX_HEADER_BYTES) {
            return HTTP_CLIENT_ERROR_HEADER_TOO_LARGE;
        }
        return HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE;
    }
    const size_t status_length = status_end - message_start;
    if (status_length < 12u ||
        bytes[message_start] != (uint8_t)'H' ||
        bytes[message_start + 1u] != (uint8_t)'T' ||
        bytes[message_start + 2u] != (uint8_t)'T' ||
        bytes[message_start + 3u] != (uint8_t)'P' ||
        bytes[message_start + 4u] != (uint8_t)'/' ||
        bytes[message_start + 5u] != (uint8_t)'1' ||
        bytes[message_start + 6u] != (uint8_t)'.' ||
        bytes[message_start + 8u] != (uint8_t)' ' ||
        bytes[message_start + 9u] < (uint8_t)'0' ||
        bytes[message_start + 9u] > (uint8_t)'9' ||
        bytes[message_start + 10u] < (uint8_t)'0' ||
        bytes[message_start + 10u] > (uint8_t)'9' ||
        bytes[message_start + 11u] < (uint8_t)'0' ||
        bytes[message_start + 11u] > (uint8_t)'9' ||
        (status_length > 12u &&
         bytes[message_start + 12u] != (uint8_t)' ')) {
        return HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE;
    }
    if (bytes[message_start + 7u] != (uint8_t)'0' &&
        bytes[message_start + 7u] != (uint8_t)'1') {
        return HTTP_CLIENT_ERROR_UNSUPPORTED_VERSION;
    }
    for (size_t i = message_start + 13u; i < status_end; i++) {
        if ((bytes[i] < (uint8_t)' ' &&
             bytes[i] != (uint8_t)'\t') ||
            bytes[i] == 0x7Fu) {
            return HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE;
        }
    }

    response->version_major = 1;
    response->version_minor =
        (unsigned int)(bytes[message_start + 7u] - (uint8_t)'0');
    response->status_code =
        (unsigned int)(bytes[message_start + 9u] - (uint8_t)'0') *
            100u +
        (unsigned int)(bytes[message_start + 10u] - (uint8_t)'0') *
            10u +
        (unsigned int)(bytes[message_start + 11u] - (uint8_t)'0');
    if (response->status_code < 100u ||
        response->status_code > 599u) {
        return HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE;
    }
    if (status_length > 12u) {
        response->reason_phrase = &bytes[message_start + 13u];
        response->reason_phrase_length = status_length - 13u;
    } else {
        response->reason_phrase = &bytes[status_end];
    }

    if (!find_header_separator(
            bytes,
            status_end,
            header_limit,
            &separator)) {
        if (response_length >= HTTP_CLIENT_MAX_HEADER_BYTES) {
            return HTTP_CLIENT_ERROR_HEADER_TOO_LARGE;
        }
        return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
    }
    *out_body_start = separator + 4u;
    if (*out_body_start > response_length) {
        return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
    }

    fields_start = status_end + 2u;
    fields_end = separator;
    if (fields_end < fields_start) {
        fields_end = fields_start;
    }
    response->header_fields = &bytes[fields_start];
    response->header_fields_length = fields_end - fields_start;

    cursor = fields_start;
    while (cursor < fields_end) {
        size_t line_end;

        if (!find_crlf(
                bytes,
                cursor,
                fields_end,
                &line_end)) {
            line_end = fields_end;
        }
        if (line_end == cursor) {
            return HTTP_CLIENT_ERROR_MALFORMED_HEADER;
        }
        error = parse_header_line(
            &bytes[cursor],
            line_end - cursor,
            response,
            &context);
        if (error != HTTP_CLIENT_ERROR_NONE) {
            return error;
        }
        if (line_end == fields_end) {
            cursor = fields_end;
        } else {
            cursor = line_end + 2u;
        }
    }

    if (response->has_transfer_encoding &&
        response->has_content_length) {
        return HTTP_CLIENT_ERROR_CONFLICTING_FRAMING;
    }
    if (context.unsupported_transfer_coding) {
        return
            HTTP_CLIENT_ERROR_UNSUPPORTED_TRANSFER_ENCODING;
    }
    response->chunked_transfer = context.chunked_seen;
    if (redirect_status(response->status_code) &&
        response->has_location &&
        !valid_uri_reference(
            response->location,
            response->location_length)) {
        return
            HTTP_CLIENT_ERROR_INVALID_REDIRECT_LOCATION;
    }

    return HTTP_CLIENT_ERROR_NONE;
}

size_t http_client_response_body_progress(
    const void* response_bytes,
    size_t response_length,
    size_t body_capacity) {
    const uint8_t* bytes = (const uint8_t*)response_bytes;
    size_t message_start = 0;
    size_t body_start = 0;
    size_t informational_count = 0;
    struct http_response response;

    if (bytes == NULL || response_length == 0 ||
        body_capacity == 0) {
        return 0;
    }
    const size_t header_limit =
        response_length < HTTP_CLIENT_MAX_HEADER_BYTES
            ? response_length : HTTP_CLIENT_MAX_HEADER_BYTES;

    for (;;) {
        clear_response(&response);
        if (parse_response_head(
                bytes, response_length, message_start,
                header_limit, &response, &body_start) !=
            HTTP_CLIENT_ERROR_NONE) {
            return 0;
        }
        if (response.status_code >= 200u) break;
        if (response.status_code == 101u ||
            response.has_content_length ||
            response.has_transfer_encoding ||
            ++informational_count >
                HTTP_CLIENT_MAX_INFORMATIONAL_RESPONSES) {
            return 0;
        }
        message_start = body_start;
        if (message_start >= response_length) return 0;
    }

    if (response.chunked_transfer ||
        response.status_code == 204u ||
        response.status_code == 304u ||
        body_start > response_length) {
        return 0;
    }
    size_t progress = response_length - body_start;
    if (response.has_content_length &&
        progress > response.content_length) {
        progress = response.content_length;
    }
    if (progress > body_capacity) progress = body_capacity;
    return progress;
}

enum http_client_error http_client_parse_response(
    void* response_bytes,
    size_t response_length,
    struct http_response* response) {
    uint8_t* bytes = (uint8_t*)response_bytes;
    size_t message_start = 0;
    size_t body_start = 0;
    size_t informational_count = 0;
    size_t available_body;
    size_t header_limit;
    enum http_client_error error;
    bool body_forbidden;

    if (response == NULL) {
        return HTTP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
    clear_response(response);
    if (response_bytes == NULL || response_length == 0) {
        return HTTP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
    header_limit = response_length < HTTP_CLIENT_MAX_HEADER_BYTES
                       ? response_length
                       : HTTP_CLIENT_MAX_HEADER_BYTES;

    for (;;) {
        clear_response(response);
        error = parse_response_head(
            bytes,
            response_length,
            message_start,
            header_limit,
            response,
            &body_start);
        if (error != HTTP_CLIENT_ERROR_NONE) {
            return error;
        }
        if (response->status_code >= 200u) {
            break;
        }
        if (response->status_code == 101u) {
            return HTTP_CLIENT_ERROR_UNSUPPORTED_PROTOCOL_SWITCH;
        }
        if (response->has_content_length ||
            response->has_transfer_encoding) {
            return HTTP_CLIENT_ERROR_INVALID_INFORMATIONAL_FRAMING;
        }
        informational_count++;
        if (informational_count >
            HTTP_CLIENT_MAX_INFORMATIONAL_RESPONSES) {
            return
                HTTP_CLIENT_ERROR_TOO_MANY_INFORMATIONAL_RESPONSES;
        }
        message_start = body_start;
        if (message_start >= response_length) {
            return
                HTTP_CLIENT_ERROR_INFORMATIONAL_RESPONSE_WITHOUT_FINAL;
        }
    }

    response->body = &bytes[body_start];
    available_body = response_length - body_start;
    body_forbidden =
        response->status_code == 204u ||
        response->status_code == 304u;
    if (body_forbidden) {
        if (available_body != 0) {
            return HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA;
        }
        response->body_length = 0;
    } else if (response->chunked_transfer) {
        error = decode_chunked_body(
            &bytes[body_start],
            available_body,
            &response->body_length);
        if (error != HTTP_CLIENT_ERROR_NONE) {
            return error;
        }
    } else if (response->has_content_length) {
        if (available_body < response->content_length) {
            return HTTP_CLIENT_ERROR_TRUNCATED_BODY;
        }
        if (available_body > response->content_length) {
            return HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA;
        }
        response->body_length = response->content_length;
    } else {
        response->body_length = available_body;
    }

    return HTTP_CLIENT_ERROR_NONE;
}

bool http_response_find_header(
    const struct http_response* response,
    const char* name,
    const uint8_t** value,
    size_t* value_length) {
    size_t name_length = 0;
    size_t cursor = 0;

    if (response == NULL ||
        name == NULL ||
        value == NULL ||
        value_length == NULL) {
        return false;
    }
    while (name[name_length] != '\0') {
        name_length++;
    }
    if (name_length == 0) {
        return false;
    }

    while (cursor < response->header_fields_length) {
        size_t line_end = cursor;
        size_t colon = cursor;
        size_t start;
        size_t end;

        while (line_end < response->header_fields_length &&
               !(line_end + 1 <
                     response->header_fields_length &&
                 response->header_fields[line_end] ==
                     (uint8_t)'\r' &&
                 response->header_fields[line_end + 1] ==
                     (uint8_t)'\n')) {
            line_end++;
        }
        while (colon < line_end &&
               response->header_fields[colon] != (uint8_t)':') {
            colon++;
        }
        if (colon - cursor == name_length &&
            span_equals_text_ignore_case(
                &response->header_fields[cursor],
                name_length,
                name)) {
            start = colon + 1u;
            while (start < line_end &&
                   (response->header_fields[start] ==
                        (uint8_t)' ' ||
                    response->header_fields[start] ==
                        (uint8_t)'\t')) {
                start++;
            }
            end = line_end;
            while (end > start &&
                   (response->header_fields[end - 1] ==
                        (uint8_t)' ' ||
                    response->header_fields[end - 1] ==
                        (uint8_t)'\t')) {
                end--;
            }
            *value = &response->header_fields[start];
            *value_length = end - start;
            return true;
        }

        cursor = line_end;
        if (cursor + 1 < response->header_fields_length) {
            cursor += 2u;
        }
    }
    return false;
}

bool http_response_is_redirect(
    const struct http_response* response) {
    return response != NULL &&
           redirect_status(response->status_code);
}

bool http_response_redirect_location(
    const struct http_response* response,
    const uint8_t** location,
    size_t* location_length) {
    if (location != NULL) {
        *location = NULL;
    }
    if (location_length != NULL) {
        *location_length = 0;
    }
    if (response == NULL ||
        location == NULL ||
        location_length == NULL ||
        !http_response_is_redirect(response) ||
        !response->has_location ||
        !valid_uri_reference(
            response->location,
            response->location_length)) {
        return false;
    }

    *location = response->location;
    *location_length = response->location_length;
    return true;
}

enum http_client_error http_response_copy_redirect_location(
    const struct http_response* response,
    char* destination,
    size_t destination_capacity,
    size_t* location_length) {
    const uint8_t* location;
    size_t length;

    if (location_length != NULL) {
        *location_length = 0;
    }
    if (destination != NULL && destination_capacity != 0) {
        destination[0] = '\0';
    }
    if (response == NULL ||
        destination == NULL ||
        location_length == NULL ||
        destination_capacity == 0) {
        return HTTP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
    if (!http_response_is_redirect(response) ||
        !response->has_location) {
        return
            HTTP_CLIENT_ERROR_REDIRECT_LOCATION_MISSING;
    }
    if (!valid_uri_reference(
            response->location,
            response->location_length)) {
        return
            HTTP_CLIENT_ERROR_INVALID_REDIRECT_LOCATION;
    }
    if (!http_response_redirect_location(
            response, &location, &length)) {
        return
            HTTP_CLIENT_ERROR_INVALID_REDIRECT_LOCATION;
    }
    if (length >= destination_capacity) {
        return
            HTTP_CLIENT_ERROR_REDIRECT_LOCATION_TOO_LARGE;
    }
    for (size_t i = 0; i < length; i++) {
        destination[i] = (char)location[i];
    }
    destination[length] = '\0';
    *location_length = length;
    return HTTP_CLIENT_ERROR_NONE;
}

enum http_client_error http_client_get(
    struct tcp_client* tcp,
    uint32_t remote_ip,
    uint16_t remote_port,
    uint32_t initial_sequence,
    const char* host,
    const char* path,
    void* request_buffer,
    size_t request_capacity,
    struct http_response* response) {
    size_t request_length;
    enum http_client_error error;

    if (tcp == NULL || response == NULL) {
        return HTTP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
    clear_response(response);
    error = http_client_build_get(
        host,
        path,
        request_buffer,
        request_capacity,
        &request_length);
    if (error != HTTP_CLIENT_ERROR_NONE) {
        return error;
    }

    if (!tcp_client_connect(
            tcp,
            remote_ip,
            remote_port,
            initial_sequence) ||
        !tcp_client_send(
            tcp,
            request_buffer,
            request_length) ||
        !tcp_client_wait_for_peer_close(tcp) ||
        !tcp_client_close(tcp)) {
        return HTTP_CLIENT_ERROR_TCP;
    }

    return http_client_parse_response(
        (void*)tcp_client_received_data(tcp),
        tcp_client_received_length(tcp),
        response);
}

const char* http_client_error_string(enum http_client_error error) {
    switch (error) {
        case HTTP_CLIENT_ERROR_NONE:
            return "no error";
        case HTTP_CLIENT_ERROR_INVALID_ARGUMENT:
            return "invalid HTTP client argument";
        case HTTP_CLIENT_ERROR_INVALID_HOST:
            return "invalid HTTP host";
        case HTTP_CLIENT_ERROR_INVALID_PATH:
            return "invalid HTTP path";
        case HTTP_CLIENT_ERROR_REQUEST_TOO_LARGE:
            return "HTTP request buffer is too small";
        case HTTP_CLIENT_ERROR_TCP:
            return "TCP operation failed";
        case HTTP_CLIENT_ERROR_HEADER_TOO_LARGE:
            return "HTTP response headers are too large";
        case HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE:
            return "malformed HTTP status line";
        case HTTP_CLIENT_ERROR_UNSUPPORTED_VERSION:
            return "unsupported HTTP version";
        case HTTP_CLIENT_ERROR_MALFORMED_HEADER:
            return "malformed HTTP header";
        case HTTP_CLIENT_ERROR_INVALID_CONTENT_LENGTH:
            return "invalid HTTP Content-Length";
        case HTTP_CLIENT_ERROR_CONFLICTING_FRAMING:
            return "conflicting HTTP message framing";
        case HTTP_CLIENT_ERROR_UNSUPPORTED_TRANSFER_ENCODING:
            return "unsupported HTTP Transfer-Encoding";
        case HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY:
            return "malformed HTTP chunked body";
        case HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE:
            return "HTTP chunk metadata is too large";
        case HTTP_CLIENT_ERROR_TRUNCATED_BODY:
            return "HTTP response body is truncated";
        case HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA:
            return "HTTP response contains unexpected extra data";
        case HTTP_CLIENT_ERROR_REDIRECT_LOCATION_MISSING:
            return "HTTP redirect has no Location";
        case HTTP_CLIENT_ERROR_INVALID_REDIRECT_LOCATION:
            return "HTTP redirect Location is invalid";
        case HTTP_CLIENT_ERROR_REDIRECT_LOCATION_TOO_LARGE:
            return "HTTP redirect Location is too large";
        case HTTP_CLIENT_ERROR_INVALID_INFORMATIONAL_FRAMING:
            return "HTTP informational response has invalid framing";
        case HTTP_CLIENT_ERROR_TOO_MANY_INFORMATIONAL_RESPONSES:
            return "too many HTTP informational responses";
        case HTTP_CLIENT_ERROR_INFORMATIONAL_RESPONSE_WITHOUT_FINAL:
            return "HTTP informational response has no final response";
        case HTTP_CLIENT_ERROR_UNSUPPORTED_PROTOCOL_SWITCH:
            return "HTTP protocol switching is unsupported";
    }
    return "unknown HTTP client error";
}
