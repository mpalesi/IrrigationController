set(IRRIGATION_SOURCES
    "system_ready.c"
    "../app/state/state_store.c"
    "../app/clock.c"
    "../app/commands/command_bus.c"
    "../app/commands/zone_command_handler.c"
    "../app/events/event_bus.c"
    "../domain/zones/zone_service.c"
    "../domain/zones/zone_start_precondition.c"
    "../domain/master_valve/master_valve_driver.c"
    "../domain/master_valve/master_valve_service.c"
    "../domain/programs/program_service.c"
    "../domain/scheduler/scheduler_service.c"
    "../infrastructure/logging/logger.c"
    "../infrastructure/logging/esp_idf_logger.c"
    "../hal/outputs/output_driver.c"
)

if(CONFIG_IRRIGATION_TARGET_DEV_KIT)
    list(APPEND IRRIGATION_SOURCES
        "../hal/boards/dev_kit_board.c"
        "../hal/outputs/virtual_output_driver.c"
    )
else()
    list(APPEND IRRIGATION_SOURCES
        "../hal/boards/irrigation_controller_board.c"
        "../hal/outputs/unconfigured_output_driver.c"
    )
endif()

idf_component_register(
    SRCS ${IRRIGATION_SOURCES}
    INCLUDE_DIRS
        "."
        ".."
)
