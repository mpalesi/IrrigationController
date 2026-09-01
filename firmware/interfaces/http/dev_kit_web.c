#include "interfaces/http/dev_kit_web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"

#define QUERY_SIZE 512U
#define PROGRAM_RESPONSE_SIZE 4096U
#define SCHEDULE_RESPONSE_SIZE 2048U
#define MAX_STEP_SECONDS 3600U
#define URI_HANDLER_COUNT 16U

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Irrigation DEV_KIT</title>"
    "<style>body{font:16px sans-serif;margin:20px}button,input,select{margin:3px;padding:7px}.program{border:1px solid #bbb;padding:10px;margin:8px 0}.step{display:flex;align-items:center;flex-wrap:wrap}</style>"
    "<h1>Irrigation DEV_KIT</h1><pre id=status>Loading...</pre><h2>Zones</h2><div id=zones></div><h2>Zone names</h2><div id=zone-names></div><h2>Master valve</h2>"
    "<button onclick=\"go('/master/open')\">Open</button><button onclick=\"go('/master/close')\">Close</button>"
    "<h2>Programs <button onclick=addProgram()>New program</button></h2><div id=programs></div><button onclick=\"go('/program/abort')\">Abort active program</button>"
    "<h2>Schedule <button onclick=addSchedule()>New Schedule</button></h2><div id=schedule></div>"
    "<script>let config;const programs=document.querySelector('#programs'),zoneNames=document.querySelector('#zone-names'),scheduleEditor=document.querySelector('#schedule'),statusOutput=document.getElementById('status');"
    "async function go(u){let r=await fetch(u,{method:'POST'}),text=await r.text();alert(text);load();if(text.includes('\"result\":\"OK\"')&&(u.startsWith('/program/save')||u.startsWith('/program/delete')||u.startsWith('/zone/name')))loadPrograms();if(text.includes('\"result\":\"OK\"')&&(u.startsWith('/schedule/save')||u.startsWith('/schedule/delete')||u.startsWith('/schedule/toggle')))loadSchedule()}"
    "function duration(ms){let s=Math.floor(ms/1000),m=Math.floor(s/60);return m?m+' min '+s%60+' s':s+' s'}"
    "function total(c){let n=0;c.querySelectorAll('.seconds').forEach(x=>n+=(+x.value||0)*1000);c.querySelector('.total').textContent='Total duration: '+duration(n)}"
    "function step(c,s){let r=document.createElement('div');r.className='step';let z=document.createElement('select');z.className='zone';config.zones.forEach(x=>{let o=new Option(x.name,x.id,x.id==s.zone_id,x.id==s.zone_id);z.add(o)});let d=document.createElement('input');d.className='seconds';d.type='number';d.min=1;d.max=3600;d.value=Math.floor(s.duration_ms/1000)||1;d.oninput=()=>total(c);let b=document.createElement('button');b.textContent='Remove Zone';b.onclick=()=>{r.remove();total(c)};r.append(z,d,' seconds',b);c.querySelector('.steps').append(r)}"
    "function card(p){let c=document.createElement('div');c.className='program';c.dataset.index=p.index;let n=document.createElement('input');n.className='name';n.maxLength=31;n.placeholder='Program name';n.value=p.name;let t=document.createElement('div');t.className='total';let ss=document.createElement('div');ss.className='steps';let a=document.createElement('button');a.textContent='Add Zone';a.onclick=()=>{step(c,{zone_id:config.zones[0].id,duration_ms:30000});total(c)};let v=document.createElement('button');v.textContent='Save';v.onclick=()=>save(c);let x=document.createElement('button');x.textContent='Delete';x.onclick=()=>go('/program/delete?index='+c.dataset.index);let q=document.createElement('button');q.textContent='Start';q.onclick=()=>go('/program/start?index='+c.dataset.index);c.append(n,t,ss,a,v,x,q);programs.append(c);p.steps.forEach(s=>step(c,s));total(c)}"
    "function addProgram(){card({index:'new',name:'',steps:[]})}"
    "function save(c){let s=[...c.querySelectorAll('.step')].map(r=>[r.querySelector('.zone').value,r.querySelector('.seconds').value]);let q=new URLSearchParams({index:c.dataset.index,name:c.querySelector('.name').value,zones:s.map(x=>x[0]),durations:s.map(x=>x[1])});go('/program/save?'+q)}"
    "function saveZoneName(c){go('/zone/name?id='+encodeURIComponent(c.dataset.id)+'&name='+encodeURIComponent(c.querySelector('input').value))}"
    "function showZoneNames(){zoneNames.innerHTML='';config.zones.forEach(z=>{let c=document.createElement('div'),label=document.createElement('span'),name=document.createElement('input'),save=document.createElement('button');c.dataset.id=z.id;label.textContent=z.id+' ';name.maxLength=31;name.value=z.name;save.textContent='Save';save.onclick=()=>saveZoneName(c);c.append(label,name,save);zoneNames.append(c)})}"
    "const weekdayNames=['Monday','Tuesday','Wednesday','Thursday','Friday','Saturday','Sunday'];"
    "function saveSchedule(c){let mask=0;c.querySelectorAll('.weekday').forEach((d,index)=>{if(d.checked)mask|=1<<index});let q=new URLSearchParams({index:c.dataset.index,weekday_mask:mask,hour:c.querySelector('.hour').value,minute:c.querySelector('.minute').value,program_index:c.querySelector('.schedule-program').value});go('/schedule/save?'+q)}"
    "function scheduleCard(s){let c=document.createElement('div');c.className='program';c.dataset.index=s.index;let days=document.createElement('span');weekdayNames.forEach((name,index)=>{let label=document.createElement('label'),day=document.createElement('input');day.className='weekday';day.type='checkbox';day.checked=(s.weekday_mask&(1<<index))!==0;label.append(day,name);days.append(label)});let hour=document.createElement('input');hour.className='hour';hour.type='number';hour.min=0;hour.max=23;hour.value=s.hour;let minute=document.createElement('input');minute.className='minute';minute.type='number';minute.min=0;minute.max=59;minute.value=s.minute;let program=document.createElement('select');program.className='schedule-program';config.programs.forEach(p=>program.add(new Option(p.name,p.index,p.index===s.program_index,p.index===s.program_index)));let enabled=document.createElement('span');enabled.textContent='Enabled: '+(s.enabled?'Yes':'No');let save=document.createElement('button');save.textContent='Save';save.onclick=()=>saveSchedule(c);c.append('Days: ',days,' Time: ',hour,':',minute,' Program: ',program,enabled,save);if(s.index!=='new'){let toggle=document.createElement('button');toggle.textContent=s.enabled?'Disable':'Enable';toggle.onclick=()=>go('/schedule/toggle?index='+c.dataset.index);let remove=document.createElement('button');remove.textContent='Delete';remove.onclick=()=>go('/schedule/delete?index='+c.dataset.index);c.append(toggle,remove)}scheduleEditor.append(c)}"
    "function addSchedule(){scheduleCard({index:'new',weekday_mask:1,hour:6,minute:0,program_index:config.programs.length?config.programs[0].index:'',enabled:false})}"
    "function showSchedules(data){scheduleEditor.innerHTML='';data.schedules.forEach(scheduleCard)}"
    "const systemStates=['BOOT','READY','RUNNING','MANUAL','OTA','MAINTENANCE','ERROR'],masterStates=['CLOSED','OPENING','OPEN','CLOSING','FAULT'],programStates=['IDLE','WAITING_MASTER','RUNNING_ZONE','WAITING_POST_PROGRAM_DELAY','COMPLETED','ABORTED','FAULT'],zoneStates=['DISABLED','IDLE','OPENING','OPEN','CLOSING','FAULT'];"
    "function stateName(states,value){return states[value]||String(value)}"
    "async function load(){let j=await (await fetch('/status')).json();statusOutput.textContent='System: '+stateName(systemStates,j.system)+'\\nMaster Valve: '+stateName(masterStates,j.master)+'\\nProgram: '+stateName(programStates,j.program)+'\\nClock synchronized: '+(j.clock_valid?'Yes':'No')+'\\nDay: '+j.day+'\\nTime: '+j.time+'\\n'+j.zones.map(z=>z.name+': '+stateName(zoneStates,z.state)).join('\\n');zones.innerHTML=j.zones.map(z=>`${z.name} <button onclick=\"go('/zone/open?id=${z.id}')\">Open</button><button onclick=\"go('/zone/close?id=${z.id}')\">Close</button><br>`).join('')}"
    "function refreshScheduleProgramOptions(){scheduleEditor.querySelectorAll('.schedule-program').forEach(select=>{let selected=select.value;select.innerHTML='';config.programs.forEach(p=>select.add(new Option(p.name,p.index)));if([...select.options].some(option=>option.value===selected))select.value=selected;else select.selectedIndex=-1})}"
    "async function loadPrograms(){config=await (await fetch('/programs')).json();programs.innerHTML='';config.programs.forEach(card);showZoneNames();refreshScheduleProgramOptions()}"
    "async function loadSchedule(){showSchedules(await (await fetch('/schedule')).json())}"
    "async function initialize(){await load();await loadPrograms();await loadSchedule()}initialize();setInterval(load,2000)</script>";

static httpd_handle_t server;
static const char *TAG = "dev_kit_web";
static char status_response[1536];
static char programs_response[PROGRAM_RESPONSE_SIZE];
static char schedule_response[SCHEDULE_RESPONSE_SIZE];
static const char *const WEEKDAY_NAMES[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

typedef struct {
    char query[QUERY_SIZE];
    char program_index[8];
    char program_name[DEV_KIT_WEB_PROGRAM_NAME_SIZE];
    char program_zones[QUERY_SIZE];
    char program_durations[QUERY_SIZE];
    char zone_id[DEV_KIT_WEB_ZONE_ID_SIZE];
    char zone_name[IRRIGATION_CONFIGURATION_NAME_SIZE];
    char parse_zones[QUERY_SIZE];
    char parse_durations[QUERY_SIZE];
    IrrigationProgramStepConfiguration steps[DEV_KIT_WEB_MAX_PROGRAM_STEPS];
    char schedule_index[8];
    char schedule_weekday_mask[4];
    char schedule_hour[3];
    char schedule_minute[3];
    char schedule_program_index[2];
} DevKitWebRequestScratch;

/* ESP-IDF dispatches these URI handlers serially from the single httpd task. */
static DevKitWebRequestScratch request_scratch;

static const char *result_name(IrrigationResult result) { switch (result) { case IRRIGATION_RESULT_OK: return "OK"; case IRRIGATION_RESULT_REJECTED: return "REJECTED"; case IRRIGATION_RESULT_INVALID_STATE: return "INVALID_STATE"; case IRRIGATION_RESULT_NOT_FOUND: return "NOT_FOUND"; case IRRIGATION_RESULT_REFERENCED: return "PROGRAM_REFERENCED"; default: return "ERROR"; } }
static esp_err_t send_result(httpd_req_t *r, IrrigationResult result) { char response[64]; snprintf(response, sizeof(response), "{\"result\":\"%s\"}", result_name(result)); httpd_resp_set_type(r, "application/json"); return httpd_resp_sendstr(r, response); }

static void url_decode(char *value) { char *read = value, *write = value; while (*read != '\0') { if (*read == '+') { *write++ = ' '; ++read; } else if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) { char hex[3] = {read[1], read[2], '\0'}; *write++ = (char)strtoul(hex, NULL, 16); read += 3; } else { *write++ = *read++; } } *write = '\0'; }
static bool query_value(httpd_req_t *r, const char *key, char *value, size_t size) { if (httpd_req_get_url_query_len(r) >= sizeof(request_scratch.query) || httpd_req_get_url_query_str(r, request_scratch.query, sizeof(request_scratch.query)) != ESP_OK || httpd_query_key_value(request_scratch.query, key, value, size) != ESP_OK) return false; url_decode(value); return true; }
static bool slot_valid(const DevKitWebContext *c, unsigned long index) { return c->configuration_manager != NULL && index < configuration_manager_get(c->configuration_manager)->program_count; }
static bool parse_value(const char *text, unsigned long maximum, unsigned long *value) { char *end = NULL; if (*text == '\0') return false; *value = strtoul(text, &end, 10); return *end == '\0' && *value <= maximum; }
static int zone_index_for(const DevKitWebContext *c, const char *id) { for (size_t i = 0U; i < c->zone_count; ++i) if (strcmp(c->zones[i].id, id) == 0) return (int)i; return -1; }
static const char *zone_name_for(const DevKitWebContext *c, const char *id) { const char *name = configuration_manager_zone_name(c->configuration_manager, id); return name != NULL ? name : id; }
static bool zone_exists(const DevKitWebContext *c, const char *id) { return zone_index_for(c, id) >= 0; }

static bool parse_steps(const DevKitWebContext *c, const char *zones_text, const char *durations_text, size_t *count) {
    if (*zones_text == '\0' && *durations_text == '\0') { *count = 0; return true; }
    char *zone_context = NULL, *duration_context = NULL;
    snprintf(request_scratch.parse_zones, sizeof(request_scratch.parse_zones), "%s", zones_text); snprintf(request_scratch.parse_durations, sizeof(request_scratch.parse_durations), "%s", durations_text);
    char *zone = strtok_r(request_scratch.parse_zones, ",", &zone_context), *duration = strtok_r(request_scratch.parse_durations, ",", &duration_context); size_t n = 0;
    while (zone && duration && n < DEV_KIT_WEB_MAX_PROGRAM_STEPS) { char *end = NULL; unsigned long seconds = strtoul(duration, &end, 10); if (!zone_exists(c, zone) || *duration == '\0' || *end != '\0' || seconds == 0 || seconds > MAX_STEP_SECONDS || strlen(zone) >= DEV_KIT_WEB_ZONE_ID_SIZE) return false; snprintf(request_scratch.steps[n].zone_id, sizeof(request_scratch.steps[n].zone_id), "%s", zone); request_scratch.steps[n++].duration_ms = (uint32_t)seconds * 1000U; zone = strtok_r(NULL, ",", &zone_context); duration = strtok_r(NULL, ",", &duration_context); }
    if (zone || duration) {
        return false;
    }
    *count = n;
    return true;
}

static esp_err_t page_handler(httpd_req_t *r) { httpd_resp_set_type(r, "text/html"); return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN); }
static esp_err_t status_handler(httpd_req_t *r)
{
    DevKitWebContext *c = r->user_ctx;
    RuntimeState s = state_store_snapshot(c->state_store);
    time_t now;
    struct tm local_time;
    char iso_week[3] = {0};
    char time_text[9] = "--:--:--";
    const char *day = "Unknown";
    time(&now);
    localtime_r(&now, &local_time);
    const bool clock_valid = now >= 1700000000 && strftime(iso_week, sizeof(iso_week), "%V", &local_time) != 0U;
    if (clock_valid) {
        day = WEEKDAY_NAMES[local_time.tm_wday];
        (void)strftime(time_text, sizeof(time_text), "%H:%M:%S", &local_time);
    }
    size_t used = (size_t)snprintf(status_response, sizeof(status_response),
                                   "{\"system\":%d,\"master\":%d,\"program\":%d,\"zones\":[",
                                   s.system_state, s.master_valve.state, s.program.state);
    for (size_t i = 0U; i < s.zone_count && used < sizeof(status_response); ++i) {
        used += (size_t)snprintf(status_response + used, sizeof(status_response) - used,
                                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"state\":%d}",
                                 i ? "," : "", s.zones[i].zone_id,
                                 zone_name_for(c, s.zones[i].zone_id), s.zones[i].state);
    }
    (void)snprintf(status_response + used, sizeof(status_response) - used,
                   "],\"clock_valid\":%s,\"day\":\"%s\",\"time\":\"%s\"}",
                   clock_valid ? "true" : "false", day, time_text);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, status_response);
}
static esp_err_t programs_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; const IrrigationConfiguration *configuration = configuration_manager_get(c->configuration_manager); size_t used = (size_t)snprintf(programs_response, sizeof(programs_response), "{\"zones\":["); for (size_t i = 0; i < c->zone_count; ++i) used += (size_t)snprintf(programs_response + used, sizeof(programs_response) - used, "%s{\"id\":\"%s\",\"name\":\"%s\"}", i ? "," : "", c->zones[i].id, zone_name_for(c, c->zones[i].id)); used += (size_t)snprintf(programs_response + used, sizeof(programs_response) - used, "],\"programs\":["); for (size_t i = 0U; i < configuration->program_count; ++i) { const IrrigationProgramConfiguration *p = &configuration->programs[i]; uint32_t total = 0U; for (size_t j = 0U; j < p->step_count; ++j) total += p->steps[j].duration_ms; used += (size_t)snprintf(programs_response + used, sizeof(programs_response) - used, "%s{\"index\":%u,\"name\":\"%s\",\"total_duration_ms\":%u,\"steps\":[", i ? "," : "", (unsigned)i, p->display_name, (unsigned)total); for (size_t j = 0U; j < p->step_count; ++j) used += (size_t)snprintf(programs_response + used, sizeof(programs_response) - used, "%s{\"zone_id\":\"%s\",\"duration_ms\":%u}", j ? "," : "", p->steps[j].zone_id, (unsigned)p->steps[j].duration_ms); used += (size_t)snprintf(programs_response + used, sizeof(programs_response) - used, "]}"); } snprintf(programs_response + used, sizeof(programs_response) - used, "]}"); httpd_resp_set_type(r, "application/json"); return httpd_resp_sendstr(r, programs_response); }
static esp_err_t schedule_get_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; const IrrigationConfiguration *configuration = configuration_manager_get(c->configuration_manager); size_t used = (size_t)snprintf(schedule_response, sizeof(schedule_response), "{\"schedules\":["); for (size_t i = 0U; i < configuration->schedule_count; ++i) { const IrrigationScheduleConfiguration *schedule = &configuration->schedules[i]; size_t program_index = 0U; while (program_index < configuration->program_count && strcmp(configuration->programs[program_index].program_id, schedule->program_id) != 0) ++program_index; if (program_index == configuration->program_count) return send_result(r, IRRIGATION_RESULT_NOT_FOUND); used += (size_t)snprintf(schedule_response + used, sizeof(schedule_response) - used, "%s{\"index\":%u,\"weekday_mask\":%u,\"hour\":%u,\"minute\":%u,\"program_index\":%u,\"enabled\":%s}", i ? "," : "", (unsigned)i, (unsigned)schedule->weekday_mask, (unsigned)schedule->hour, (unsigned)schedule->minute, (unsigned)program_index, schedule->enabled ? "true" : "false"); } (void)snprintf(schedule_response + used, sizeof(schedule_response) - used, "]}"); httpd_resp_set_type(r, "application/json"); return httpd_resp_sendstr(r, schedule_response); }

static esp_err_t zone_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; char id[32]; if (!query_value(r, "id", id, sizeof(id))) return send_result(r, IRRIGATION_RESULT_REJECTED); return send_result(r, strncmp(r->uri, "/zone/open", strlen("/zone/open")) == 0 ? zone_service_open(c->zone_service, id, 0) : zone_service_close(c->zone_service, id)); }
static IrrigationResult configuration_mutation_result(IrrigationResult result)
{
    if (result == IRRIGATION_RESULT_INTERNAL_ERROR) {
        ESP_LOGE(TAG, "configuration mutation was not applied because persistence failed or is unavailable");
    }
    return result;
}

static esp_err_t zone_name_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; if (!query_value(r, "id", request_scratch.zone_id, sizeof(request_scratch.zone_id)) || !query_value(r, "name", request_scratch.zone_name, sizeof(request_scratch.zone_name))) return send_result(r, IRRIGATION_RESULT_REJECTED); return send_result(r, configuration_mutation_result(configuration_manager_rename_zone(c->configuration_manager, request_scratch.zone_id, request_scratch.zone_name))); }
static esp_err_t master_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; return send_result(r, strcmp(r->uri, "/master/open") == 0 ? master_valve_service_open(c->master_valve_service) : master_valve_service_close(c->master_valve_service)); }
static esp_err_t program_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; if (strcmp(r->uri, "/program/abort") == 0) return send_result(r, program_service_abort(c->program_service)); char index_text[8]; if (!query_value(r, "index", index_text, sizeof(index_text))) return send_result(r, IRRIGATION_RESULT_REJECTED); unsigned long index = strtoul(index_text, NULL, 10); const Program *program = slot_valid(c, index) ? configuration_manager_program_at(c->configuration_manager, index) : NULL; return send_result(r, program == NULL ? IRRIGATION_RESULT_NOT_FOUND : program_service_start(c->program_service, program)); }

static IrrigationResult configure_scheduler(DevKitWebContext *context, IrrigationResult result) {
    result = configuration_mutation_result(result);
    if (result != IRRIGATION_RESULT_OK) return result;
    result = configuration_manager_configure_scheduler(context->configuration_manager, context->scheduler_service);
    if (result != IRRIGATION_RESULT_OK) ESP_LOGE(TAG, "persistent configuration was saved but runtime scheduler application failed");
    return result;
}
static esp_err_t program_save_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; if (program_service_is_active(c->program_service) || !query_value(r, "index", request_scratch.program_index, sizeof(request_scratch.program_index)) || !query_value(r, "name", request_scratch.program_name, sizeof(request_scratch.program_name)) || !query_value(r, "zones", request_scratch.program_zones, sizeof(request_scratch.program_zones)) || !query_value(r, "durations", request_scratch.program_durations, sizeof(request_scratch.program_durations)) || request_scratch.program_name[0] == '\0') return send_result(r, IRRIGATION_RESULT_REJECTED); size_t count; if (!parse_steps(c, request_scratch.program_zones, request_scratch.program_durations, &count)) return send_result(r, IRRIGATION_RESULT_REJECTED); IrrigationResult result; if (strcmp(request_scratch.program_index, "new") == 0) result = configuration_manager_create_program(c->configuration_manager, request_scratch.program_name, request_scratch.steps, count, NULL); else { unsigned long index; if (!parse_value(request_scratch.program_index, DEV_KIT_WEB_MAX_PROGRAMS - 1U, &index)) return send_result(r, IRRIGATION_RESULT_REJECTED); result = configuration_manager_update_program(c->configuration_manager, index, request_scratch.program_name, request_scratch.steps, count); } return send_result(r, configure_scheduler(c, result)); }
static esp_err_t program_delete_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; unsigned long index; if (program_service_is_active(c->program_service) || !query_value(r, "index", request_scratch.program_index, sizeof(request_scratch.program_index)) || !parse_value(request_scratch.program_index, DEV_KIT_WEB_MAX_PROGRAMS - 1U, &index)) return send_result(r, IRRIGATION_RESULT_REJECTED); return send_result(r, configure_scheduler(c, configuration_manager_delete_program(c->configuration_manager, index))); }
static esp_err_t schedule_save_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; unsigned long index, weekday_mask, hour, minute, program_index; if (!query_value(r, "index", request_scratch.schedule_index, sizeof(request_scratch.schedule_index))) return send_result(r, IRRIGATION_RESULT_REJECTED); const bool is_new = strcmp(request_scratch.schedule_index, "new") == 0; if ((!is_new && !parse_value(request_scratch.schedule_index, IRRIGATION_MAX_SCHEDULES - 1U, &index)) || !query_value(r, "weekday_mask", request_scratch.schedule_weekday_mask, sizeof(request_scratch.schedule_weekday_mask)) || !query_value(r, "hour", request_scratch.schedule_hour, sizeof(request_scratch.schedule_hour)) || !query_value(r, "minute", request_scratch.schedule_minute, sizeof(request_scratch.schedule_minute)) || !query_value(r, "program_index", request_scratch.schedule_program_index, sizeof(request_scratch.schedule_program_index)) || !parse_value(request_scratch.schedule_weekday_mask, (1U << 7U) - 1U, &weekday_mask) || weekday_mask == 0U || !parse_value(request_scratch.schedule_hour, 23U, &hour) || !parse_value(request_scratch.schedule_minute, 59U, &minute) || !parse_value(request_scratch.schedule_program_index, DEV_KIT_WEB_MAX_PROGRAMS - 1U, &program_index) || !slot_valid(c, program_index)) return send_result(r, IRRIGATION_RESULT_REJECTED); const IrrigationConfiguration *configuration = configuration_manager_get(c->configuration_manager); const char *program_id = configuration->programs[program_index].program_id; IrrigationResult result = is_new ? configuration_manager_create_schedule(c->configuration_manager, (uint8_t)weekday_mask, (uint8_t)hour, (uint8_t)minute, program_id, NULL) : configuration_manager_update_schedule(c->configuration_manager, index, (uint8_t)weekday_mask, (uint8_t)hour, (uint8_t)minute, program_id); return send_result(r, configure_scheduler(c, result)); }
static esp_err_t schedule_delete_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; unsigned long index; if (!query_value(r, "index", request_scratch.schedule_index, sizeof(request_scratch.schedule_index)) || !parse_value(request_scratch.schedule_index, IRRIGATION_MAX_SCHEDULES - 1U, &index)) return send_result(r, IRRIGATION_RESULT_REJECTED); return send_result(r, configure_scheduler(c, configuration_manager_delete_schedule(c->configuration_manager, index))); }
static esp_err_t schedule_handler(httpd_req_t *r) { DevKitWebContext *c = r->user_ctx; unsigned long index; if (!query_value(r, "index", request_scratch.schedule_index, sizeof(request_scratch.schedule_index)) || !parse_value(request_scratch.schedule_index, IRRIGATION_MAX_SCHEDULES - 1U, &index)) return send_result(r, IRRIGATION_RESULT_REJECTED); const IrrigationConfiguration *configuration = configuration_manager_get(c->configuration_manager); if (index >= configuration->schedule_count) return send_result(r, IRRIGATION_RESULT_NOT_FOUND); return send_result(r, configure_scheduler(c, configuration_manager_set_schedule_enabled(c->configuration_manager, index, !configuration->schedules[index].enabled))); }

void dev_kit_web_start(const DevKitWebContext *context) { if (server != NULL || context == NULL) return; httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.max_uri_handlers = URI_HANDLER_COUNT; if (httpd_start(&server, &config) != ESP_OK) return; const httpd_uri_t handlers[] = {{.uri="/",.method=HTTP_GET,.handler=page_handler,.user_ctx=(void *)context},{.uri="/status",.method=HTTP_GET,.handler=status_handler,.user_ctx=(void *)context},{.uri="/programs",.method=HTTP_GET,.handler=programs_handler,.user_ctx=(void *)context},{.uri="/schedule",.method=HTTP_GET,.handler=schedule_get_handler,.user_ctx=(void *)context},{.uri="/zone/open",.method=HTTP_POST,.handler=zone_handler,.user_ctx=(void *)context},{.uri="/zone/close",.method=HTTP_POST,.handler=zone_handler,.user_ctx=(void *)context},{.uri="/zone/name",.method=HTTP_POST,.handler=zone_name_handler,.user_ctx=(void *)context},{.uri="/master/open",.method=HTTP_POST,.handler=master_handler,.user_ctx=(void *)context},{.uri="/master/close",.method=HTTP_POST,.handler=master_handler,.user_ctx=(void *)context},{.uri="/program/start",.method=HTTP_POST,.handler=program_handler,.user_ctx=(void *)context},{.uri="/program/abort",.method=HTTP_POST,.handler=program_handler,.user_ctx=(void *)context},{.uri="/program/save",.method=HTTP_POST,.handler=program_save_handler,.user_ctx=(void *)context},{.uri="/program/delete",.method=HTTP_POST,.handler=program_delete_handler,.user_ctx=(void *)context},{.uri="/schedule/save",.method=HTTP_POST,.handler=schedule_save_handler,.user_ctx=(void *)context},{.uri="/schedule/delete",.method=HTTP_POST,.handler=schedule_delete_handler,.user_ctx=(void *)context},{.uri="/schedule/toggle",.method=HTTP_POST,.handler=schedule_handler,.user_ctx=(void *)context}}; for (size_t i = 0; i < sizeof(handlers)/sizeof(handlers[0]); ++i) if (httpd_register_uri_handler(server, &handlers[i]) != ESP_OK) ESP_LOGE(TAG, "failed to register URI %s", handlers[i].uri); }
