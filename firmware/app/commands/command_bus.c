#include "app/commands/command_bus.h"

void command_bus_init(CommandBus *bus, CommandHandler handler, void *context)
{
    if (bus == NULL) {
        return;
    }
    bus->handler = handler;
    bus->context = context;
}

IrrigationResult command_bus_dispatch(const CommandBus *bus, const Command *command)
{
    if (bus == NULL || command == NULL || bus->handler == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    return bus->handler(command, bus->context);
}
