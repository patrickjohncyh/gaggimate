# Plan: HomeKit integration improvements

Branch: `feat/homekit-improvements` (from `master` @ `6dff0448`, the current nightly)

## Goals (current scope)

1. Fix the reported bug: "Hey Siri, turn on the Gaggia" puts the thermostat into **Auto**, the
   machine does not wake, and the accessory then shows **Not Responding** in the Home app
   (the machine itself keeps working). Includes the cross-task locking fix, since that race
   contributes to the hang.
2. Machine modes: **Brew**, **Steam**, and **Hot Water** switches, mutually exclusive.
3. Accessory information: Name, Manufacturer, Model, SerialNumber, FirmwareRevision.

Deferred: status sensors (Brewing/Ready/Low Water), per-device setup code and QR in the web UI.

## Root cause of the Siri bug (verified against the HomeSpan 1.9.1 source)

- Siri's "turn on" writes `TargetHeatingCoolingState = 3` (Auto). We call `setValidValues(2, 0, 1)`,
  but HomeSpan 1.9.1 does **not** enforce valid values on writes: `SpanCharacteristic::loadUpdate`
  accepts any integer that parses. So `3` reaches `HomekitAccessory::update()`.
- `update()` copies the new target into `CurrentHeatingCoolingState`
  (`state->setVal(targetState->getNewVal())`). That characteristic's range is 0–2, so we publish
  an out-of-range value. HomeSpan's own warning for this case reads: *"This may cause device to
  become non-responsive!"* This matches the "Not Responding" symptom.
- `HomekitPlugin::loop()` treats "on" as `targetState == 1`. Since `3 != 1`, the machine is either
  left in standby or actively put into standby, which is why "turn on" does nothing.
- Contributing cause: the `boiler:*` and `controller:mode:change` handlers call `setVal(..., true)`
  from the main loop task. With `autoPoll()`, HomeSpan runs on its own FreeRTOS task, and `setVal`
  pushes into `homeSpan.Notifications` (a `std::vector`) with no locking. That vector is read and
  cleared concurrently by the poll task, which is a data race that can corrupt HomeSpan state or
  hang the HAP server. The `actionRequired` flag is also a plain `bool` shared between the two
  tasks.

## Constraints

- Platform is `espressif32@6.12.0` (Arduino core 2.x). HomeSpan 2.x requires Arduino core 3.x,
  so **we stay on HomeSpan 1.9.1**. That version has no `homeSpanPAUSE`/locking API.
- Existing pairings must survive. Keep a single accessory (no bridge) with the same AID. Adding
  services changes the attribute database, and HomeSpan bumps the config number automatically, so
  iOS will pick up the new services without re-pairing.
- HomeSpan 1.9.1 has no clean teardown (`end()`), so turning HomeKit on or off at runtime still
  needs a reboot.

## Chosen approach

### Threading model: one owner per side

- Only the HomeSpan poll task touches characteristics. The machine-to-HomeKit direction goes
  through a small mutex-guarded "pending state" struct (mode, current temp, target temp, brewing,
  ready, low water). Plugin event handlers write it. `HomekitAccessory::loop()` (a `SpanService`
  loop, which runs inside the poll task) applies only the fields that changed, calling `setVal`.
- Only the main task touches `Controller`. The HomeKit-to-machine direction goes through a
  mutex-guarded "pending command" (requested power, requested mode, requested target temp; each
  optional). `update()` records it, and `HomekitPlugin::loop()` consumes it. Only the fields that
  actually changed are applied, so toggling power no longer re-sends a stale target temp.

### Thermostat (power and boiler temperature)

- Coerce any non-Off target state (Heat, Cool, Auto) to Heat (on). After the write, snap
  `TargetHeatingCoolingState` back to 1 so the Home tile shows Heat, not Auto.
- `CurrentHeatingCoolingState` is only ever set to 0 (Off) or 1 (Heat).
- The Thermostat keeps its role: Off means standby, and Heat means awake (brew mode).

### Machine modes

- Three `Switch` services, **Brew**, **Steam**, and **Hot Water**. At most one is on, and it
  always mirrors the machine's current mode (changes made on the machine show up in HomeKit).
- Turning a switch on wakes the machine if needed (same sequence as the display UI: wake into
  brew, then switch) and selects that mode.
- Turning the active switch off puts the machine into standby. Turning off an inactive switch
  does nothing.
- All switches off means standby, which matches the thermostat being Off.
- Grind mode shows the thermostat as on with no switch active.
- Siri's "turn on <accessory>" writes the thermostat and all three switches at once. When
  several switches are turned on together, brew wins over hot water, which wins over steam
  (the lowest boiler temperature is the safest pick). A power-off write wins over everything.
- Commands are ignored while a process (a shot, steaming, hot water) is running, and the
  characteristics snap back to the real state.

### Accessory information

- Name "GaggiMate", Manufacturer "GaggiMate", Model "GaggiMate". HomeKit talks to the
  GaggiMate board, not the Gaggia machine. The user's own name in the Home app is kept on the
  iPhone and is unaffected.
- SerialNumber is the Wi-Fi MAC address.
- FirmwareRevision is the numeric `x.y.z` from `BUILD_GIT_VERSION`, because HAP rejects
  suffixes like `-210-g6dff0448`.

### Ready notification

- **Espresso Ready** (`OccupancySensor`): on once the machine is awake and
  `WarningManager::isTemperatureStable()` holds (20 s within max(2 °C, 2 %) of the setpoint).
  It is latched through temperature dips (a shot) and resets on a mode or setpoint change, so
  switching to steam gives a second "ready" when steam is at temperature. iOS notifications are
  configured per sensor in the Home app.
- **Heat soak.** A stable boiler (about 3 min) isn't enough: the ≈0.45 kg brass portafilter is
  heated only through the group lugs, so it takes ≈15 min from cold. `HeatSoakEstimator` models
  it as a first-order lag of the boiler temperature, normalised between room temperature and the
  brew setpoint: `soak += (drive - soak) * (1 - e^(-dt/tau))`.
  - tau = warm-up time / 3, where the warm-up time setting (`hk_wu`) defaults to 15 min;
    0 means boiler only.
  - Ready also requires soak ≥ 0.95.
  - Room temperature is the first boiler reading if it's between 10 and 35 °C; otherwise the
    model falls back to 28 °C.
  - The model starts unsoaked at boot.
  - The reference is the last brew setpoint, so a return from steam doesn't read as cooling.
  - Estimates for the machine: boiler + water ≈1.1 kJ/K at 1360 W; portafilter ≈0.17 kJ/K fed
    through ≈0.4 W/K with ≈0.15 W/K lost to the room, giving tau ≈ 5 min. These are tuned to
    community warm-up reports (15–20 min) rather than measured; a thermocouple log from a cold
    start would let us fit tau.
- **Batching fix:** writes are staged in `update()` and committed at the start of the service
  loops. HomeSpan runs every request's `update()` calls before any `loop()`, so one Siri request
  always reaches the main task as a single command and the precedence rules hold.
- A backflush reminder was built and then dropped at the user's request. HomeKit's
  `FilterMaintenance` service only shows up linked to an Air Purifier, so a reminder would need
  a sensor plus a reset switch instead.

## Tests

- Pure logic lives in `src/display/plugins/homekit/HomekitMapping.{h,cpp}` (no HomeSpan or
  Arduino): the power request mapping, mode resolution and precedence, the characteristic state
  for each mode, temperature clamping and notify threshold, and firmware revision parsing.
- `test/test_homekit_mapping` runs under `pio test -e native` (17 cases).
- On-device checks (manual, because HomeSpan can't run natively):
  - "Hey Siri, turn on/off <name>"
  - Toggle each switch in the Home app, and change modes on the machine
  - Try to switch modes during a shot (it should be ignored and snap back)
  - Leave it running overnight and confirm there's no "Not Responding"
  - Confirm an existing pairing survives the upgrade and the new switches appear

## Out of scope and open questions

- Runtime enable/disable without a reboot (blocked on HomeSpan 1.9.1).
- Upgrading to HomeSpan 2.x (needs an Arduino core 3.x migration).

## CI

- `check.yml` runs `pio check` on the display with `--fail-on-defect=medium` (non-blocking) and
  on the controller (blocking), plus the native tests.
- Before pushing, run `pio test -e native`, `pio run -e display`, and `pio check -e display`
  locally.
