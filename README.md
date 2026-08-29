# IrrigationController Firmware

Milestone 1 is an ESP-IDF 5.5.5 foundation for the Freenove FNK0090 `DEV_KIT`.
The application core uses only logical output IDs; board selection happens in the
HAL board-composition layer.

For the DEV_KIT web test, local zones 1-8 use GPIO 16, 17, 18, 19, 21, 22, 23,
and 25 respectively. The development-only Master Valve driver uses GPIO 26.

## Host tests

```sh
cmake -S tests -B build/host-tests
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

## DEV_KIT build, flash, and monitor

After activating ESP-IDF 5.5.5:

```sh
idf.py -C firmware set-target esp32
idf.py -C firmware build
idf.py -C firmware -p <serial-port> flash monitor
```

`IRRIGATION_CONTROLLER` is intentionally an unconfigured safe placeholder.
It contains no GPIO, MCP23017, relay-channel, or relay-polarity assumptions.
