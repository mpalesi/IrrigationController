if(CONFIG_IRRIGATION_TARGET_DEV_KIT)
    message(STATUS "IrrigationController firmware target: DEV_KIT")
    set(IRRIGATION_SOURCES
        "system_ready.c"
        "../interfaces/http/dev_kit_web.c"
        "../interfaces/http/dev_kit_zone_configuration.c"
        "../app/configuration/configuration_manager.c"
        "../app/configuration/irrigation_configuration.c"
        "../infrastructure/persistence/configuration_repository.c"
        "../infrastructure/persistence/nvs_configuration_repository.c"
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
        "../domain/programs/program_execution.c"
        "../domain/history/program_execution_history.c"
        "../domain/scheduler/scheduler_service.c"
        "../infrastructure/logging/logger.c"
        "../infrastructure/logging/esp_idf_logger.c"
        "../hal/outputs/output_driver.c"
        "../hal/boards/dev_kit_board.c"
        "../hal/outputs/dev_kit_output_driver.c"
        "../hal/master_valve/dev_kit_master_valve_driver.c"
    )
elseif(CONFIG_IRRIGATION_TARGET_IRRIGATION_CONTROLLER)
    message(STATUS "IrrigationController firmware target: IRRIGATION_CONTROLLER production output driver")
    set(IRRIGATION_SOURCES
        "production_system_ready.c"
        "../infrastructure/logging/logger.c"
        "../infrastructure/logging/esp_idf_logger.c"
        "../hal/outputs/output_driver.c"
        "../hal/outputs/irrigation_controller_output_driver.c"
        "../hal/mcp23017/mcp23017.c"
        "../hal/boards/irrigation_controller_board.c"
    )
else()
    # ESP-IDF evaluates component requirements before creating a fresh sdkconfig.
    # Kconfig has only DEV_KIT and IRRIGATION_CONTROLLER; use the production
    # source set during that first pass so the requested production defaults can
    # be resolved without inheriting DEV_KIT sources.
    message(STATUS "IrrigationController firmware target: IRRIGATION_CONTROLLER production output driver")
    set(IRRIGATION_SOURCES
        "production_system_ready.c"
        "../infrastructure/logging/logger.c"
        "../infrastructure/logging/esp_idf_logger.c"
        "../hal/outputs/output_driver.c"
        "../hal/outputs/irrigation_controller_output_driver.c"
        "../hal/mcp23017/mcp23017.c"
        "../hal/boards/irrigation_controller_board.c"
    )
endif()

idf_component_register(
    SRCS ${IRRIGATION_SOURCES}
    INCLUDE_DIRS
        "."
        ".."
)
