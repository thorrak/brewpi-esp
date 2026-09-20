# Glycol Heating Control

Glycol beer control combines the existing predictive cooling algorithm with
beer-temperature PID heating. Cooling is described in
[GLYCOL_COOLING_ALGORITHM.md](GLYCOL_COOLING_ALGORITHM.md). The implementation is in
`GlycolMode.cpp`; ordinary chamber control remains in `ChamberMode.cpp`.

## Activation and configuration

Enable glycol mode in the web interface's settings, select beer constant or beer
profile mode, and assign a beer sensor and heater. A configured light can act as
the heater when `lightAsHeater` is enabled. A fridge sensor is optional for glycol
beer control. While glycol mode is enabled, a request for fridge constant mode
is redirected to beer constant mode. Ordinary chamber control requires a fridge
sensor.

Heating uses the existing control constants:

| API key | Field | Meaning |
|---|---|---|
| `KpHeat` | `cc.Kp_heat` | Proportional gain |
| `KiHeat` | `cc.Ki_heat` | Integral gain |
| `KdHeat` | `cc.Kd_heat` | Derivative gain; normally negative to oppose warming |
| `pidMaxHeat` | `cc.pidMax_heat` | Maximum heating output; zero disables heating |

`GET /api/cc/` reads these values and `PUT /api/cc/` updates and persists them.
The main web interface enables glycol mode but has no form for these four gains.
PID gains do not change with the display unit; `pidMaxHeat` is expressed as a temperature difference
in the selected display unit. Gains accept the signed fixed-point range of
approximately -63.998 to +63.998. `pidMaxHeat` must be nonnegative and fit the same
internal range after conversion from Fahrenheit when applicable. Invalid numeric
values or types are rejected before changing live settings.

The timing values are persisted in `minTimes`: `GLYCOL_WINDOW_PERIOD` defaults to
1000 seconds and `GLYCOL_MIN_ON_TIME` to 10 seconds. `MIN_HEAT_OFF_TIME` and
`MIN_SWITCH_TIME` protect output transitions. The web API returns the glycol
window settings through `GET /api/extended/`, but does not currently expose an
update handler or UI fields for them.

## Demand and timing

The firmware updates temperatures, peak detection, PID, state, and outputs once
per second. Glycol mode skips chamber peak detection and calculates heating demand
from the beer sensor:

```text
error = setpoint - slow_filtered_beer_temperature
output = clamp(KpHeat * error + KiHeat * integral + KdHeat * beer_slope,
               0, pidMaxHeat)
```

The integral updates approximately once per minute, is suppressed during cooling
and coasting, and is constrained to avoid accumulating demand beyond maximum
output. Heating requires an assigned heater, a valid beer sensor and setpoint,
a positive output, and a positive `pidMaxHeat`.

Heating starts when fast-filtered beer temperature is at or below
`setpoint - trigger_margin`. It ends when temperature reaches the setpoint or
demand disappears. Heating takes priority over a predictive cooling request while
below this heating threshold. Cooling cannot start until `MIN_SWITCH_TIME` has
elapsed after the last heating output.

A heating window converts output to a relay ON duration:

```text
on_time_seconds = floor(output / pidMaxHeat * GLYCOL_WINDOW_PERIOD)
```

A positive duration is raised to `GLYCOL_MIN_ON_TIME` when necessary, then capped
at the window period. A duration that rounds to zero produces no pulse. Duty is
latched at the start of each window so changing PID output cannot create several
ON pulses inside one window. Reaching the setpoint or losing demand still turns
the heater off immediately; the minimum slice is not a reason to keep heating
after demand ends.

Before each OFF-to-ON transition, both `MIN_HEAT_OFF_TIME` since heating and
`MIN_SWITCH_TIME` since cooling must have elapsed. These delays do not interrupt
an already active ON slice. A blocked start resets the window, preserving a full
ON slice once the delay expires.

## States and output rules

`GLYCOL_HEATING` is an internal mode. The existing public states describe the
current relay command:

| Public state | Heater command | Meaning |
|---|---|---|
| `HEATING_MIN_TIME` | ON | Early part of the ON slice |
| `HEATING` | ON | Remaining ON slice |
| `WAITING_TO_HEAT` | OFF | Protection delay or normal window OFF slice |
| `IDLE` | OFF | No active heating demand |

An internal wait reason distinguishes heater-off delay, direction-switch delay,
and window OFF time. Cooling and heating outputs are mutually exclusive.
When the light is the glycol heater, door and camera requests cannot turn it on
outside an active heating state.

Turning control off, losing a required sensor, or changing control mode clears
heating demand, the integral and the duty window. Output-off timestamps remain
available so restarting control still observes the protection delays.

## Diagnostics and validation status

With `ENABLE_GLYCOL_LOGGING`, heating transitions are included in
`/glycol_log.csv`. Rotation retains the previous file at
`/glycol_log.archived.csv`; each file rotates after approximately 30 KB. Clearing
the glycol log clears both files. Archive failures leave the active log intact.

The integration has not been validated with physical sensors, heater relays, or a
glycol pump. No automated regression tests were added, as requested. Compilation
and inspection cannot establish thermal performance or suitable heater tuning
for a particular fermenter.
