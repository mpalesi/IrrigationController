#pragma once

#include <stddef.h>

#include "app/commands/command.h"
#include "app/result.h"

typedef IrrigationResult (*CommandHandler)(const Command *command, void *context);

typedef struct {
    CommandHandler handler;
    void *context;
} CommandBus;

void command_bus_init(CommandBus *bus, CommandHandler handler, void *context);
IrrigationResult command_bus_dispatch(const CommandBus *bus, const Command *command);
