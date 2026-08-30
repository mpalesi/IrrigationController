cmake_minimum_required(VERSION 3.16)
project(irrigation_controller_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(unit_tests
    unit/test_main.c
    ../firmware/app/state/state_store.c
    ../firmware/app/clock.c
    ../firmware/app/commands/command_bus.c
    ../firmware/app/commands/zone_command_handler.c
    ../firmware/app/events/event_bus.c
    ../firmware/domain/zones/zone_service.c
    ../firmware/domain/zones/zone_start_precondition.c
    ../firmware/domain/master_valve/master_valve_driver.c
    ../firmware/domain/master_valve/master_valve_service.c
    ../firmware/domain/programs/program_service.c
    ../firmware/domain/scheduler/scheduler_service.c
    ../firmware/interfaces/http/dev_kit_zone_configuration.c
    ../firmware/infrastructure/logging/logger.c
    ../firmware/hal/outputs/output_driver.c
    ../firmware/hal/outputs/virtual_output_driver.c
)
target_include_directories(unit_tests PRIVATE ../firmware)
target_compile_options(unit_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME unit_tests COMMAND unit_tests)
