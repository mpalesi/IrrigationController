#include "app/commands/zone_command_handler.h"

IrrigationResult zone_command_handler_dispatch(const Command *command, void *context)
{
    ZoneService *service = context;
    if (command == NULL || service == NULL || command->zone_id == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    switch (command->type) {
    case COMMAND_TYPE_OPEN_ZONE:
        return zone_service_open(service, command->zone_id, command->duration_ms);
    case COMMAND_TYPE_CLOSE_ZONE:
        return zone_service_close(service, command->zone_id);
    default:
        return IRRIGATION_RESULT_NOT_SUPPORTED;
    }
}
