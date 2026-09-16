#include "hal/master_valve/shelly_master_valve_driver.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool has_error;
    bool has_output;
    bool output;
} ShellyResponse;

static const char *skip_whitespace(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) ++cursor;
    return cursor;
}

static bool parse_string(const char **cursor, const char **content, size_t *length)
{
    const char *value = *cursor;
    if (*value++ != '"') return false;
    const char *start = value;
    size_t count = 0U;
    while (*value != '\0' && *value != '"') {
        if ((unsigned char)*value < 0x20U) return false;
        if (*value++ == '\\') {
            if (*value == 'u') {
                for (size_t index = 0U; index < 4U; ++index) {
                    if (!isxdigit((unsigned char)*++value)) return false;
                }
                ++value;
            } else if (*value == '"' || *value == '\\' || *value == '/' || *value == 'b' ||
                       *value == 'f' || *value == 'n' || *value == 'r' || *value == 't') {
                ++value;
            } else {
                return false;
            }
        }
        ++count;
    }
    if (*value != '"') return false;
    *content = start;
    *length = count;
    *cursor = value + 1;
    return true;
}

static bool skip_json_value(const char **cursor);

static bool skip_json_array(const char **cursor)
{
    const char *value = skip_whitespace(*cursor);
    if (*value++ != '[') return false;
    value = skip_whitespace(value);
    if (*value == ']') {
        *cursor = value + 1;
        return true;
    }
    for (;;) {
        if (!skip_json_value(&value)) return false;
        value = skip_whitespace(value);
        if (*value == ']') {
            *cursor = value + 1;
            return true;
        }
        if (*value++ != ',') return false;
        value = skip_whitespace(value);
    }
}

static bool skip_json_object(const char **cursor)
{
    const char *value = skip_whitespace(*cursor);
    if (*value++ != '{') return false;
    value = skip_whitespace(value);
    if (*value == '}') {
        *cursor = value + 1;
        return true;
    }
    for (;;) {
        const char *content;
        size_t length;
        if (!parse_string(&value, &content, &length)) return false;
        (void)content;
        (void)length;
        value = skip_whitespace(value);
        if (*value++ != ':') return false;
        if (!skip_json_value(&value)) return false;
        value = skip_whitespace(value);
        if (*value == '}') {
            *cursor = value + 1;
            return true;
        }
        if (*value++ != ',') return false;
        value = skip_whitespace(value);
    }
}

static bool skip_json_number(const char **cursor)
{
    const char *value = *cursor;
    if (*value == '-') ++value;
    if (*value == '0') {
        ++value;
    } else if (isdigit((unsigned char)*value)) {
        do { ++value; } while (isdigit((unsigned char)*value));
    } else {
        return false;
    }
    if (*value == '.') {
        if (!isdigit((unsigned char)*++value)) return false;
        do { ++value; } while (isdigit((unsigned char)*value));
    }
    if (*value == 'e' || *value == 'E') {
        ++value;
        if (*value == '+' || *value == '-') ++value;
        if (!isdigit((unsigned char)*value)) return false;
        do { ++value; } while (isdigit((unsigned char)*value));
    }
    *cursor = value;
    return true;
}

static bool skip_json_value(const char **cursor)
{
    const char *value = skip_whitespace(*cursor);
    if (*value == '"') {
        const char *content;
        size_t length;
        if (!parse_string(&value, &content, &length)) return false;
        (void)content;
        (void)length;
    } else if (*value == '{') {
        if (!skip_json_object(&value)) return false;
    } else if (*value == '[') {
        if (!skip_json_array(&value)) return false;
    } else if (strncmp(value, "true", 4U) == 0) {
        value += 4;
    } else if (strncmp(value, "false", 5U) == 0) {
        value += 5;
    } else if (strncmp(value, "null", 4U) == 0) {
        value += 4;
    } else if (!skip_json_number(&value)) {
        return false;
    }
    *cursor = value;
    return true;
}

static bool key_equals(const char *content, size_t length, const char *expected)
{
    const size_t expected_length = strlen(expected);
    return length == expected_length && memcmp(content, expected, length) == 0;
}

static bool parse_shelly_response(const char *text, ShellyResponse *response)
{
    if (text == NULL || response == NULL) return false;
    const char *value = skip_whitespace(text);
    if (*value++ != '{') return false;
    *response = (ShellyResponse){0};
    value = skip_whitespace(value);
    if (*value == '}') return *skip_whitespace(value + 1) == '\0';
    for (;;) {
        const char *key;
        size_t key_length;
        if (!parse_string(&value, &key, &key_length)) return false;
        value = skip_whitespace(value);
        if (*value++ != ':') return false;
        value = skip_whitespace(value);
        if (key_equals(key, key_length, "output")) {
            if (response->has_output) return false;
            if (strncmp(value, "true", 4U) == 0) {
                response->output = true;
                value += 4;
            } else if (strncmp(value, "false", 5U) == 0) {
                response->output = false;
                value += 5;
            } else {
                return false;
            }
            response->has_output = true;
        } else {
            if (key_equals(key, key_length, "error")) response->has_error = true;
            if (!skip_json_value(&value)) return false;
        }
        value = skip_whitespace(value);
        if (*value == '}') return *skip_whitespace(value + 1) == '\0';
        if (*value++ != ',') return false;
        value = skip_whitespace(value);
    }
}

static IrrigationResult request(ShellyMasterValveDriver *driver, const char *path,
                                ShellyResponse *response)
{
    size_t response_length = 0U;
    int http_status = 0;
    IrrigationResult result = shelly_http_transport_get(driver->transport, driver->host, driver->port,
                                                         path, driver->operation_timeout_ms,
                                                         driver->response, sizeof(driver->response),
                                                         &response_length, &http_status);
    if (result != IRRIGATION_RESULT_OK) return result;
    if (http_status < 200 || http_status >= 300 || response_length >= sizeof(driver->response)) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    driver->response[response_length] = '\0';
    return parse_shelly_response(driver->response, response) && !response->has_error
        ? IRRIGATION_RESULT_OK : IRRIGATION_RESULT_INTERNAL_ERROR;
}

static IrrigationResult set_and_confirm(MasterValveDriver *base, bool requested_output)
{
    ShellyMasterValveDriver *driver = (ShellyMasterValveDriver *)base;
    if (driver == NULL || !driver->configured || driver->transport == NULL) {
        printf("shelly_master_valve: driver is not configured\n");
        return IRRIGATION_RESULT_REJECTED;
    }
    char path[80];
    if (snprintf(path, sizeof(path), "/rpc/Switch.Set?id=%u&on=%s", (unsigned)driver->switch_id,
                 requested_output ? "true" : "false") >= (int)sizeof(path)) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    ShellyResponse response;
    IrrigationResult result = request(driver, path, &response);
    if (result != IRRIGATION_RESULT_OK) {
        printf("shelly_master_valve: Switch.Set(%s) failed result=%d\n",
               requested_output ? "open" : "close", result);
        return result;
    }
    if (snprintf(path, sizeof(path), "/rpc/Switch.GetStatus?id=%u", (unsigned)driver->switch_id) >=
        (int)sizeof(path)) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    result = request(driver, path, &response);
    if (result != IRRIGATION_RESULT_OK || !response.has_output || response.output != requested_output) {
        printf("shelly_master_valve: Switch.GetStatus did not confirm %s result=%d\n",
               requested_output ? "open" : "closed", result);
        return result == IRRIGATION_RESULT_OK ? IRRIGATION_RESULT_INTERNAL_ERROR : result;
    }
    printf("shelly_master_valve: %s confirmed\n", requested_output ? "open" : "closed");
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult shelly_open_and_confirm(MasterValveDriver *base)
{
    return set_and_confirm(base, true);
}

static IrrigationResult shelly_close_and_confirm(MasterValveDriver *base)
{
    return set_and_confirm(base, false);
}

static const MasterValveDriverVTable SHELLY_MASTER_VALVE_DRIVER_VTABLE = {
    .open_and_confirm = shelly_open_and_confirm,
    .close_and_confirm = shelly_close_and_confirm,
};

void shelly_master_valve_driver_init(ShellyMasterValveDriver *driver, ShellyHttpTransport *transport,
                                     ShellyMasterValveConfiguration configuration)
{
    if (driver == NULL) return;
    *driver = (ShellyMasterValveDriver){.base.vtable = &SHELLY_MASTER_VALVE_DRIVER_VTABLE,
                                       .transport = transport,
                                       .port = configuration.port,
                                       .switch_id = configuration.switch_id,
                                       .operation_timeout_ms = configuration.operation_timeout_ms};
    if (configuration.host != NULL && configuration.host[0] != '\0' &&
        strnlen(configuration.host, sizeof(driver->host)) < sizeof(driver->host) &&
        configuration.port != 0U && configuration.operation_timeout_ms != 0U) {
        (void)snprintf(driver->host, sizeof(driver->host), "%s", configuration.host);
        driver->configured = true;
    }
}
