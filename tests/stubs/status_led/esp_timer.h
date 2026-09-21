#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct TestTimer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *);
    void *arg;
    const char *name;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t);
