#include "interfaces/http/dev_kit_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"

static const char PAGE[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Irrigation DEV_KIT</title><style>body{font:16px sans-serif;margin:20px}button{margin:3px;padding:9px}</style>"
    "</head><body><h1>Irrigation DEV_KIT</h1><pre id=status>Loading...</pre>"
    "<h2>Zones</h2><div id=zones></div><h2>Master valve</h2>"
    "<button onclick=\"go('/master/open')\">Open</button><button onclick=\"go('/master/close')\">Close</button>"
    "<h2>Program</h2><button onclick=\"go('/program/start?index=0')\">Start test program</button>"
    "<button onclick=\"go('/program/abort')\">Abort</button><h2>Schedule</h2>"
    "<button onclick=\"go('/schedule/toggle?index=0')\">Toggle test schedule</button>"
    "<script>async function go(u){let r=await fetch(u,{method:'POST'});alert(await r.text());load()}"
    "async function load(){let j=await (await fetch('/status')).json();document.getElementById('status').textContent=JSON.stringify(j,null,2);"
    "zones.innerHTML=j.zones.map(z=>`${z.id} <button onclick=\"go('/zone/open?id=${z.id}')\">Open</button>"
    "<button onclick=\"go('/zone/close?id=${z.id}')\">Close</button><br>`).join('')}load();setInterval(load,2000)</script>"
    "</body></html>";

static httpd_handle_t server;
static const char *TAG = "dev_kit_web";
static char status_response[1536];

#define DEV_KIT_WEB_URI_HANDLER_COUNT 9U

static const char *result_name(IrrigationResult result)
{
    switch (result) {
    case IRRIGATION_RESULT_OK: return "OK";
    case IRRIGATION_RESULT_REJECTED: return "REJECTED";
    case IRRIGATION_RESULT_INVALID_STATE: return "INVALID_STATE";
    case IRRIGATION_RESULT_NOT_FOUND: return "NOT_FOUND";
    default: return "ERROR";
    }
}

static esp_err_t send_result(httpd_req_t *request, IrrigationResult result)
{
    char response[64];
    snprintf(response, sizeof(response), "{\"result\":\"%s\"}", result_name(result));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

static bool query_value(httpd_req_t *request, const char *key, char *value, size_t value_size)
{
    char query[96];
    return httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, value, value_size) == ESP_OK;
}

static esp_err_t page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    DevKitWebContext *context = request->user_ctx;
    RuntimeState state = state_store_snapshot(context->state_store);
    size_t used = (size_t)snprintf(status_response, sizeof(status_response),
                                   "{\"system\":%d,\"master\":%d,\"program\":%d,\"scheduler_entries\":%u,\"zones\":[",
                                   state.system_state, state.master_valve.state, state.program.state,
                                   (unsigned)state.scheduler.entry_count);
    for (size_t index = 0U; index < state.zone_count && used < sizeof(status_response); ++index) {
        const ZoneRuntimeState *zone = &state.zones[index];
        used += (size_t)snprintf(status_response + used, sizeof(status_response) - used,
                                 "%s{\"id\":\"%s\",\"state\":%d}", index == 0U ? "" : ",",
                                 zone->zone_id, zone->state);
    }
    used += (size_t)snprintf(status_response + used, sizeof(status_response) - used, "],\"programs\":[");
    for (size_t index = 0U; index < context->program_count && used < sizeof(status_response); ++index) {
        used += (size_t)snprintf(status_response + used, sizeof(status_response) - used,
                                 "%s\"%s\"", index == 0U ? "" : ",", context->programs[index].id);
    }
    used += (size_t)snprintf(status_response + used, sizeof(status_response) - used, "],\"schedules\":[");
    for (size_t index = 0U; index < context->scheduler_service->entry_count && used < sizeof(status_response); ++index) {
        const SchedulerEntry *entry = &context->scheduler_service->entries[index];
        used += (size_t)snprintf(status_response + used, sizeof(status_response) - used,
                                 "%s{\"id\":\"%s\",\"enabled\":%s}", index == 0U ? "" : ",",
                                 entry->id, entry->enabled ? "true" : "false");
    }
    (void)snprintf(status_response + used, sizeof(status_response) - used, "]}");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, status_response);
}

static esp_err_t zone_handler(httpd_req_t *request)
{
    DevKitWebContext *context = request->user_ctx;
    char id[32];
    if (!query_value(request, "id", id, sizeof(id))) {
        return send_result(request, IRRIGATION_RESULT_REJECTED);
    }
    IrrigationResult result = strncmp(request->uri, "/zone/open", strlen("/zone/open")) == 0
        ? zone_service_open(context->zone_service, id, 0U)
        : zone_service_close(context->zone_service, id);
    return send_result(request, result);
}

static esp_err_t master_handler(httpd_req_t *request)
{
    DevKitWebContext *context = request->user_ctx;
    return send_result(request, strcmp(request->uri, "/master/open") == 0
                       ? master_valve_service_open(context->master_valve_service)
                       : master_valve_service_close(context->master_valve_service));
}

static esp_err_t program_handler(httpd_req_t *request)
{
    DevKitWebContext *context = request->user_ctx;
    if (strcmp(request->uri, "/program/abort") == 0) {
        return send_result(request, program_service_abort(context->program_service));
    }
    char index_text[8];
    unsigned long index = 0U;
    if (!query_value(request, "index", index_text, sizeof(index_text))) {
        return send_result(request, IRRIGATION_RESULT_REJECTED);
    }
    index = strtoul(index_text, NULL, 10);
    return send_result(request, index < context->program_count
                       ? program_service_start(context->program_service, &context->programs[index])
                       : IRRIGATION_RESULT_NOT_FOUND);
}

static esp_err_t schedule_handler(httpd_req_t *request)
{
    DevKitWebContext *context = request->user_ctx;
    char index_text[8];
    if (!query_value(request, "index", index_text, sizeof(index_text))) {
        return send_result(request, IRRIGATION_RESULT_REJECTED);
    }
    unsigned long index = strtoul(index_text, NULL, 10);
    if (index >= context->scheduler_service->entry_count) {
        return send_result(request, IRRIGATION_RESULT_NOT_FOUND);
    }
    return send_result(request, scheduler_service_set_enabled(context->scheduler_service, index,
                       !context->scheduler_service->entries[index].enabled));
}

void dev_kit_web_start(const DevKitWebContext *context)
{
    if (server != NULL || context == NULL) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = DEV_KIT_WEB_URI_HANDLER_COUNT;
    if (httpd_start(&server, &config) != ESP_OK) {
        return;
    }
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler, .user_ctx = (void *)context},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = (void *)context},
        {.uri = "/zone/open", .method = HTTP_POST, .handler = zone_handler, .user_ctx = (void *)context},
        {.uri = "/zone/close", .method = HTTP_POST, .handler = zone_handler, .user_ctx = (void *)context},
        {.uri = "/master/open", .method = HTTP_POST, .handler = master_handler, .user_ctx = (void *)context},
        {.uri = "/master/close", .method = HTTP_POST, .handler = master_handler, .user_ctx = (void *)context},
        {.uri = "/program/start", .method = HTTP_POST, .handler = program_handler, .user_ctx = (void *)context},
        {.uri = "/program/abort", .method = HTTP_POST, .handler = program_handler, .user_ctx = (void *)context},
        {.uri = "/schedule/toggle", .method = HTTP_POST, .handler = schedule_handler, .user_ctx = (void *)context},
    };
    for (size_t index = 0U; index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        if (httpd_register_uri_handler(server, &handlers[index]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register URI %s", handlers[index].uri);
        }
    }
}
