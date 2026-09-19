# Glycol Heating Control Algorithm Design

## Purpose

This document describes the intended control model for the heating phase when `extendedSettings.glycol` is enabled.

Cooling and heating do not use the same strategy:

- Cooling remains predictive bang-bang control with adaptive learning.
- Heating is intentionally simpler: direct beer-temperature PID converted to time-proportional on/off control inside a fixed window.

This file is not a runtime data table. It is a design/specification document meant to keep the implementation coherent and to make state transitions explicit before changing code.

---

## Why A Separate Heating Design Exists

The predictive cooling model is needed because glycol cooling has:

- meaningful dead time
- significant post-stop coast
- pump and reservoir behavior that changes during the cycle

Heating usually behaves differently:

- the heater response is slower and more monotonic
- there is typically less "coast after stop" than with glycol cooling
- a beer-only PID mapped to duty cycle is easier to reason about and tune

So glycol mode should be treated as a hybrid controller:

- predictive cooling
- time-proportional heating

---

## Control Pipeline

Every second, the firmware loop runs:

1. `updateTemperatures()`
2. `detectPeaks()`
3. `updatePID()`
4. `updateState()`
5. `updateOutputs()`

For glycol heating, the intended meaning of each stage is:

1. Sense the current beer temperature and filtered derivative.
2. Ignore compressor-style peak detection.
3. Compute heating demand only.
4. Decide whether heating is allowed now and whether the current window slice is ON or OFF.
5. Drive the heater output from the final state.

The important rule is:

`updatePID()` computes demand. It must not decide actuator timing.

`updateState()` arbitrates protection delays, window timing, and public state.

`updateOutputs()` only maps state to hardware pins.

---

## Main Variables

### Persistent Inputs

- `cs.beerSetting`: beer temperature target
- `cc.Kp_heat`, `cc.Ki_heat`, `cc.Kd_heat`: heating PID gains
- `cc.pidMax_heat`: maximum heating authority
- `minTimes.MIN_HEAT_OFF_TIME`: minimum heater off time
- `minTimes.MIN_SWITCH_TIME`: minimum delay after cooling before heating
- `minTimes.MIN_HEAT_ON_TIME`: minimum legacy heater on time state
- `minTimes.GLYCOL_WINDOW_PERIOD`: duty-cycle window length in seconds
- `minTimes.GLYCOL_MIN_ON_TIME`: minimum ON slice length inside a window
- `glycolConfig.trigger_margin`: deadband used to begin heating

### Runtime Inputs

- `cv.beerDiff = beerSetting - beerTemp`
- `cv.beerSlope`
- `glycolRuntime.heating_output`: PID output clamped to `0..pidMax_heat`
- `lastHeatTime`
- `lastCoolTime`

### Public State

- `state = IDLE`
- `state = WAITING_TO_HEAT`
- `state = HEATING`
- `state = HEATING_MIN_TIME`

### Internal Glycol State

- `glycolRuntime.state = GLYCOL_IDLE`
- `glycolRuntime.state = GLYCOL_HEATING`

The public `state` is legacy/UI/output state.

The internal `glycolRuntime.state` is the real controller mode.

These are related, but they are not the same thing.

---

## Heating Philosophy

The heating branch answers 4 separate questions every second:

1. Is there heating demand?
2. Is the heater allowed to start right now?
3. If allowed, is the current duty-cycle slice ON or OFF?
4. Given the answers above, what public state should be exposed?

Most recent bugs came from mixing those 4 questions into one block.

---

## Demand Calculation

Heating demand is calculated in `updatePID()` from beer temperature only.

The intended behavior is:

```text
beer_error = setpoint - beer_temp
beer_slope = d(beer_temp)/dt

heating_output = clamp(P + I + D, 0, pidMax_heat)
```

Heating demand exists only when all of the following are true:

- glycol mode is enabled
- beer mode is active
- heating hardware exists
- `pidMax_heat > 0`
- `heating_output > 0`
- beer temperature is below the heating trigger threshold

Recommended threshold:

```text
start heating when beer_temp <= setpoint - trigger_margin
stop heating when beer_temp >= setpoint
```

This gives a small deadband and avoids short toggling around setpoint.

---

## Window Scheduler

Heating is time-proportional.

The duty-cycle window converts heating authority into ON time:

```text
on_time = heating_output / pidMax_heat * GLYCOL_WINDOW_PERIOD
```

Then it is clamped:

- if `on_time == 0`, do not heat
- if `0 < on_time < GLYCOL_MIN_ON_TIME`, force `on_time = GLYCOL_MIN_ON_TIME`
- if `on_time > GLYCOL_WINDOW_PERIOD`, force `on_time = GLYCOL_WINDOW_PERIOD`

This means the heater does not receive an analog value. It only receives ON or OFF, but with a duty cycle proportional to PID output.

---

## Protection Gating

Before an OFF-to-ON transition is allowed, both protections must be satisfied:

```text
timeSinceHeating() >= MIN_HEAT_OFF_TIME
timeSinceCooling() >= MIN_SWITCH_TIME
```

Important rule:

These protections apply only when starting a new ON segment.

They must not force a transition back to waiting while the heater is already ON.

Important rule:

If protections block the start of a new ON segment, the ON segment must not be consumed while blocked.

In practice this means the duty-cycle window should be delayed or restarted once the actuator is actually allowed to turn on.

---

## State Machine

The intended heating behavior is intentionally small:

```text
GLYCOL_IDLE
    -> GLYCOL_HEATING when heating demand becomes true

GLYCOL_HEATING
    -> GLYCOL_IDLE when setpoint is reached or demand disappears
```

Inside `GLYCOL_HEATING`, the public exposed state can still change each second:

- `WAITING_TO_HEAT` if protection delays block a new ON segment
- `WAITING_TO_HEAT` if current duty-cycle slice is OFF
- `HEATING_MIN_TIME` if current duty-cycle slice is ON and still inside minimum ON slice
- `HEATING` if current duty-cycle slice is ON and beyond minimum ON slice

So:

- `GLYCOL_HEATING` is the internal controller state
- `WAITING_TO_HEAT` is not always an error or protection state
- `WAITING_TO_HEAT` can also mean "PWM slice is currently OFF"

That ambiguity is acceptable for display compatibility, but it must be explicit in the design.

---

## Transition Table

This table is the main design artifact to reason about the implementation.

| Condition | Internal glycol state | Public state | Heater output | Timers updated | Notes |
|---|---|---|---|---|---|
| Glycol disabled, not in beer mode, invalid setpoint, or no heater hardware | `GLYCOL_IDLE` | `IDLE` | OFF | `lastIdleTime` | Heating branch inactive |
| Heating demand is false | `GLYCOL_IDLE` | `IDLE` | OFF | `lastIdleTime` | No need to heat |
| Heating demand becomes true | `GLYCOL_HEATING` | `WAITING_TO_HEAT` | OFF | `lastIdleTime` | Enter heating controller |
| In `GLYCOL_HEATING`, setpoint reached or demand disappears | `GLYCOL_IDLE` | `IDLE` | OFF | `lastIdleTime` | Exit heating controller |
| In `GLYCOL_HEATING`, ON segment requested, but `MIN_HEAT_OFF_TIME` not satisfied | `GLYCOL_HEATING` | `WAITING_TO_HEAT` | OFF | `waitTime`, `lastIdleTime` | Protection wait; do not consume ON slice |
| In `GLYCOL_HEATING`, ON segment requested, but `MIN_SWITCH_TIME` after cooling not satisfied | `GLYCOL_HEATING` | `WAITING_TO_HEAT` | OFF | `waitTime`, `lastIdleTime` | Protection wait; do not consume ON slice |
| In `GLYCOL_HEATING`, protections satisfied and window slice is ON | `GLYCOL_HEATING` | `HEATING_MIN_TIME` or `HEATING` | ON | `lastHeatTime` | Public heating state depends on elapsed ON slice |
| In `GLYCOL_HEATING`, protections satisfied and window slice is OFF | `GLYCOL_HEATING` | `WAITING_TO_HEAT` | OFF | `waitTime`, `lastIdleTime` | This is normal PWM OFF time |
| Heating output collapses to zero inside current cycle | `GLYCOL_IDLE` | `IDLE` | OFF | `lastIdleTime` | No point keeping heating active |

---

## Interpretation Of `WAITING_TO_HEAT`

`WAITING_TO_HEAT` currently merges two different meanings:

1. protection delay still active
2. duty-cycle window is currently in its OFF portion

The firmware can keep exposing the same legacy state for UI compatibility, but the implementation should internally distinguish the reason.

Recommended internal distinction:

```text
wait_reason = NONE
wait_reason = HEAT_OFF_DELAY
wait_reason = SWITCH_DELAY
wait_reason = PWM_OFF_SLICE
```

This does not need to be user-visible immediately, but it should exist in the design.

---

## Required Invariants

These invariants should hold at all times:

- Cooling and heating must never be ON at the same time.
- Protection delays apply only to OFF-to-ON transitions.
- A blocked ON segment must not be consumed while blocked.
- `stateIsHeating()` should mean "heater output is currently ON", not "controller is in heating mode".
- `lastHeatTime` should move forward only while the heater output is actually ON.
- `lastIdleTime` should move forward while no active heating or cooling output is running.
- Reaching setpoint must force exit from `GLYCOL_HEATING`.
- `WAITING_TO_HEAT` must be safe whether it means protection wait or PWM OFF slice.

If any bug violates one of these invariants, the bug is structural, not just a tuning issue.

---

## Recommended Code Structure

To match this design, the heating branch should be readable as 4 steps:

### Step 1: Demand

```text
bool demand = glycolHeatingDemandActive();
```

### Step 2: Protection

```text
HeatingGateResult gate = glycolHeatingProtectionGate();
```

Where `HeatingGateResult` answers:

- allowed now?
- remaining wait time?
- wait reason?

### Step 3: Window

```text
HeatingWindowResult window = glycolHeatingWindowState();
```

Where `HeatingWindowResult` answers:

- window active?
- on slice active?
- seconds elapsed in window?
- on time requested?

### Step 4: Public State Mapping

```text
if (!demand) -> IDLE
else if (!gate.allowed) -> WAITING_TO_HEAT
else if (window.on_slice_active) -> HEATING or HEATING_MIN_TIME
else -> WAITING_TO_HEAT
```

This is simpler to validate than a single large `case GLYCOL_HEATING`.

---

## Tuning Notes

- `GLYCOL_WINDOW_PERIOD = 1000s` is conservative and may feel very slow on a bench setup.
- For hardware testing, a shorter window like `120s` or `180s` makes transitions easier to observe.
- `GLYCOL_MIN_ON_TIME` should be long enough to avoid ineffective micro-bursts, but short enough not to overheat near setpoint.
- `MIN_HEAT_OFF_TIME` and `MIN_SWITCH_TIME` are safety/protection parameters, not control gains.
- `Kp_heat`, `Ki_heat`, `Kd_heat`, and `pidMax_heat` should be tuned independently from compressor cooling constants.

---

## What This Document Is For

This document is meant to prevent "symptom chasing".

When heating behaves incorrectly, the first question should not be:

"Which `if` is wrong?"

It should be:

"Which stage is wrong: demand, protection, window scheduling, or public state mapping?"

That question is much easier to answer consistently.
