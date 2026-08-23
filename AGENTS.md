# IrrigationController — Codex Project Instructions

## 1. Project authority and scope

This repository contains the firmware for the IrrigationController.

The primary implementation reference is:

- `docs/IrrigationController_Consolidated_Software_Hardware_Specification_v1.0.docx`

The consolidated specification supersedes the separate draft software-design documents for implementation decisions. The original documents may be retained for traceability, but do not silently reintroduce decisions that were superseded by the consolidated specification.

When a requirement is not specified, do not invent a project-specific behavior. Identify the missing decision and ask for clarification when it materially affects architecture, public interfaces, safety, persistence, or hardware behavior.

## 2. Core architectural principles

The firmware shall preserve these architectural boundaries:

1. Command Bus + Event Bus + State Store.
2. State Store is the single source of truth for current runtime state.
3. Commands represent requests to change state.
4. Events represent facts that have already happened.
5. Queries are read-only.
6. Command handlers validate and dispatch to domain/application services.
7. Domain/application services contain irrigation business logic.
8. REST and WebSocket layers contain transport/interface logic only.
9. Application logic must never access GPIO, MCP23017 registers, relay drivers, or ESP32-specific peripherals directly.
10. Hardware-specific behavior belongs in the HAL/driver layer.
11. The Output Driver is the application-facing abstraction for physical outputs.
12. Do not create a second, parallel architecture merely because it is convenient for one feature.

Canonical flow:

    Request
      -> Command
      -> Validation
      -> Handler
      -> Service
      -> State Store
      -> Event
      -> Subscribers

## 3. Final hardware platform

The finalized PCB is based on:

- ESP32-WROOM-32E
- CP2102N USB/UART interface
- MCP23017 I/O expander
- ULN2803A output driver
- Eight Omron G5LE-family 5 V relay channels K601-K608
- Eight field output positions
- Two external digital inputs
- RESET and BOOT pushbuttons
- 5 V / 3.3 V logic supplies
- 24 VAC field circuit

The exact MCP23017 channel mapping and exact logical-to-physical relay mapping must be verified against the final PCB/first assembled board. Never guess these mappings.

All actuator outputs must have a safe OFF state during boot, reset, watchdog recovery, OTA, and error handling unless an explicitly documented and verified exception exists.

## 4. DEV_KIT and production hardware targets

Firmware must support development without the final IrrigationController PCB.

There are two hardware targets:

- `DEV_KIT`: ordinary ESP32 development board(s) used during software development.
- `IRRIGATION_CONTROLLER`: finalized custom PCB.

The application/domain layer must be identical for both targets.

Only the HAL, board configuration, and physical driver implementation should differ.

For `DEV_KIT`:

- Use ordinary ESP32 GPIOs or a Virtual Output Driver.
- Physical relays are not required.
- Output actions should be observable through logging and/or development GPIO/LEDs.
- Missing MCP23017 hardware must not prevent testing application logic.
- External inputs may be simulated.
- Time-dependent behavior must be testable without waiting for real irrigation durations.

For `IRRIGATION_CONTROLLER`:

- Use the real ESP32 + MCP23017 + output-driver/relay hardware.
- Board-specific pin/channel mappings must be isolated in the board/HAL configuration.
- The Master Valve is a remote actuator controlled through a Shelly 1 Gen 3 over the network. It is not one of the eight local relay outputs of the IrrigationController PCB.
- Master Valve communication shall be isolated behind a dedicated Shelly client/driver interface so that the Master Valve Service does not depend directly on the HTTP/network implementation.
- The Master Valve Service shall handle Shelly communication failures, timeouts and unreachable states explicitly. Irrigation zones must not be started when the required master-valve operation has failed.

Never add `#ifdef` blocks for board-specific behavior throughout application code. Keep target-specific code at the HAL/board boundary.

## 5. Recommended repository structure

Prefer the following structure unless the build framework requires a justified variation:

    firmware/
      app/
        services/
        state/
        commands/
        events/
        scheduler/
      domain/
        zones/
        programs/
        master_valve/
      infrastructure/
        persistence/
        logging/
        network/
        ota/
      hal/
        esp32/
        gpio/
        mcp23017/
        outputs/
      interfaces/
        rest/
        websocket/
      main/

    tests/
      unit/
      integration/
      hardware/

    docs/
    AGENTS.md

Keep public interfaces and domain models independent of hardware implementation.

## 6. Coding rules

- Prefer small, cohesive modules.
- Prefer explicit interfaces over hidden dependencies.
- Avoid global mutable state except where explicitly required by the architecture.
- Use dependency injection or explicit construction for services and drivers where practical.
- Do not place business logic in HTTP handlers, WebSocket handlers, GPIO callbacks, or hardware drivers.
- Do not use blocking `delay()` for scheduler/control timing.
- Use non-blocking timing mechanisms such as FreeRTOS timers, tasks, event groups, or monotonic time.
- Avoid dynamic allocation in high-frequency or timing-critical paths when a bounded/static alternative is practical.
- Validate all externally supplied data.
- Handle errors explicitly; do not silently ignore failures.
- Use structured Result/error types consistent with the specification.
- Keep logs useful and structured; never log passwords, tokens, or other secrets.
- Comments should explain intent, constraints, or non-obvious decisions rather than restating code.
- Do not add dependencies without a clear reason and confirmation when the dependency materially affects the architecture or build.

## 7. Command/Event/State rules

### Commands

Commands are immutable requests.

A Command should contain, as applicable:

- command ID
- timestamp
- origin/user identity
- command type/version
- payload

Commands must be validated before execution.

A Command handler must not manipulate hardware directly.

### Events

Events are immutable facts.

An Event should contain, as applicable:

- event ID
- timestamp
- source
- event type/version
- payload

Events do not modify state themselves.

### State Store

The State Store owns current runtime state.

Do not maintain competing copies of authoritative state inside unrelated services.

Persisted configuration and runtime state are separate concepts.

## 8. Irrigation domain rules

The following are core domain rules:

- A maximum of one irrigation zone may be active at a time.
- Zones are logical entities.
- Physical relay channels are represented by Output mappings.
- Program execution is ordered by steps.
- The Scheduler triggers programs; it does not directly control outputs.
- The Program Service controls program sequencing.
- The Zone Service controls zone lifecycle.
- The Master Valve Service controls master-valve sequencing.
- Output Driver performs physical actuation.
- The Master Valve is a remote actuator controlled through a Shelly 1 Gen 3. It is not mapped to a local PCB relay output.
- Configured open/close delays and timeouts must be respected.
- Faults must produce appropriate Events and move affected state machines to safe fault states.
- Completion must leave the system in a safe state.

Typical execution:

    Scheduler
      -> StartProgramCommand
      -> Program Service
      -> Master Valve
      -> Zone 1
      -> Zone 2
      -> ...
      -> Master Valve close
      -> ProgramCompletedEvent

## 9. State machines

Do not bypass the state machines by changing state arbitrarily.

Global states:

- BOOT
- READY
- RUNNING
- MANUAL
- OTA
- MAINTENANCE
- ERROR

Program states include:

- IDLE
- STARTING
- RUNNING
- PAUSED
- STOPPING
- COMPLETED
- ERROR

Zone states include:

- DISABLED
- IDLE
- OPENING
- OPEN
- CLOSING
- FAULT

Master valve states include:

- CLOSED
- OPENING
- OPEN
- CLOSING
- FAULT

When implementing a transition:

1. Validate that the transition is allowed.
2. Perform the associated domain/service operation.
3. Update the State Store.
4. Emit the appropriate Event.
5. Ensure failure results in a defined safe state.

Master Valve state transitions are driven by commands to the remote Shelly 1 Gen 3 and by the resulting communication outcome. A communication timeout or unreachable Shelly condition shall result in the appropriate FAULT state.


## 10. REST and WebSocket rules

REST base path:

    /api/v1

REST is a transport/interface layer.

Expected query endpoints include:

- `/api/v1/state`
- `/api/v1/zones`
- `/api/v1/zones/{id}`
- `/api/v1/programs`
- `/api/v1/programs/{id}`
- `/api/v1/scheduler`
- `/api/v1/network`
- `/api/v1/master-valve`
- `/api/v1/system`
- `/api/v1/logs`

Commands are transported through:

    POST /api/v1/commands

REST handlers must not implement irrigation sequencing.

WebSocket is an event/state notification channel and must not implement domain logic or directly manipulate outputs.

Use the canonical data model for REST/WebSocket payloads.

## 11. Persistence, networking and security

- Persistent configuration is separate from runtime State Store.
- Configuration is represented as JSON according to the canonical model.
- Configuration changes use the Command/Service architecture.
- Secrets must never be logged.
- Passwords must not be stored in plaintext.
- Authentication and authorization are required for protected Commands.
- Roles are Administrator, Operator and Guest.
- NTP/network time is used when available.
- Canonical timestamps are UTC ISO-8601.
- OTA images must be verified before installation.
- OTA and reboot paths must leave actuator outputs in a safe state.

## 12. FreeRTOS/task model

Preserve the intended separation:

- Command Task
- Scheduler Task
- IO Task
- Network Task
- Logger Task
- OTA Task

Use queues, event groups and timers for coordination where appropriate.

Do not create unnecessary tasks merely to parallelize trivial work.

Avoid long blocking operations in the control path.

## 13. Testing requirements

Testing is a first-class part of implementation.

### Unit tests

Test without physical hardware:

- State Store
- Commands and validation
- Events
- Results/errors
- Zone state machine
- Program state machine
- Master valve state machine
- Scheduler
- Program sequencing
- Configuration validation
- Domain services
- The Shelly client must be testable without a physical Shelly device by using a mock/fake Shelly interface.

### Integration tests

Test:

- Command -> Handler -> Service -> State Store -> Event
- irrigation sequencing
- REST -> Command Bus
- Event Bus -> WebSocket
- persistence
- network behavior

### Hardware tests

On `DEV_KIT`:

- Verify build/flash/boot.
- Verify Wi-Fi.
- Verify web/API behavior.
- Verify virtual/logical outputs.
- Simulate inputs and timing.
- Exercise full irrigation sequences without physical valves.

On the final PCB:

- Verify exact GPIO/MCP23017 mapping.
- Verify relay polarity and output driver.
- Verify all eight relay channels.
- Verify 24 VAC field outputs.
- Verify INPUT1/INPUT2.
- Verify RESET/BOOT.
- Verify safe boot/reset/watchdog/OTA behavior.
- Verify communication with the physical Shelly 1 Gen 3, including successful ON/OFF commands, timeout handling and unreachable-device behavior.

## 14. Development workflow

For every meaningful feature:

1. Read the relevant section of the consolidated specification.
2. Identify the domain/service/interface/HAL boundaries involved.
3. Implement the smallest coherent change.
4. Add or update unit tests.
5. Build the `DEV_KIT` target.
6. Run relevant tests.
7. Check for architectural violations.
8. Only then proceed to the next feature.

Before modifying architecture, public APIs, canonical data structures, state machines, or hardware abstraction boundaries, stop and identify the proposed change and its impact.

Do not silently change the specification.

## 15. Codex behavior

Codex should act as an implementation agent, not as the project architect.

Before substantial changes:

- inspect the repository;
- read `AGENTS.md`;
- read the relevant consolidated-specification sections;
- inspect existing interfaces and tests;
- preserve established terminology.

When requirements conflict:

1. Prefer the consolidated specification.
2. Prefer explicit hardware facts over assumptions.
3. Do not invent missing hardware mappings.
4. Flag the conflict rather than silently resolving it.

When a task is ambiguous but low-risk, make the smallest reversible implementation and document the assumption. When ambiguity affects architecture, safety, persistent data, public APIs, or hardware behavior, ask before proceeding.

Do not rewrite large portions of the project unless the task requires it.

## 16. Definition of done

A feature is not complete merely because the code compiles.

A feature is complete when:

- implementation follows the architecture;
- domain behavior matches the consolidated specification;
- relevant tests exist and pass;
- `DEV_KIT` build works when applicable;
- errors and fault paths are handled;
- no unsafe actuator behavior is introduced;
- public API/data-model changes are intentional and documented;
- no secrets are exposed in logs or source;
- the change does not introduce unnecessary hardware coupling.

## 17. First implementation milestone

The first Codex milestone is intentionally small.

Create:

1. the repository/build skeleton;
2. `DEV_KIT` target configuration;
3. `IRRIGATION_CONTROLLER` target placeholder;
4. HAL interfaces;
5. Virtual Output Driver for `DEV_KIT`;
6. basic logging;
7. test framework;
8. minimal State Store;
9. minimal Command/Event infrastructure;
10. a build and flashable "system ready" firmware.

Do not implement the entire irrigation application in the first milestone.

The goal is to establish a clean, testable foundation that can run on an ordinary ESP32 development kit today and later use the finalized IrrigationController PCB without changing the application/domain architecture.
