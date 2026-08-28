#pragma once

#include "app/commands/command.h"
#include "app/result.h"
#include "domain/zones/zone_service.h"

IrrigationResult zone_command_handler_dispatch(const Command *command, void *context);
