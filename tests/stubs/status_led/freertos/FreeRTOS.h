#pragma once
#include <stddef.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter_critical(portMUX_TYPE *);
void test_exit_critical(portMUX_TYPE *);
#define portENTER_CRITICAL(lock) test_enter_critical(lock)
#define portEXIT_CRITICAL(lock) test_exit_critical(lock)
