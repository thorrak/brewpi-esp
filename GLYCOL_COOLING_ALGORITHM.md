# Glycol Cooling Control Algorithm Design

## Overview

This document describes a predictive, adaptive bang-bang control algorithm for glycol-based fermentation cooling. The algorithm uses a single temperature sensor (beer temperature) and a single control output (pump on/off) to maintain precise temperature control despite significant thermal delays in the system.

## Activation

Glycol mode can be enabled and configured from the main web interface.

## System Characteristics

- **Input:** Beer temperature (single sensor)
- **Output:** Pump on/off (binary control)
- **Response delay (L):** effective learned delay from pump activation to observable temperature change; varies by cooling hardware, glycol temperature, flow, volume, and thermal coupling
- **Coast period:** ~5 minutes of continued cooling after pump stops (learned)
- **Minimum pump on/off time:** 15 seconds (pump protection)

## Core Philosophy

**Proactive, predictive, and adaptive.** The algorithm anticipates temperature changes rather than reacting to them, and continuously learns from observed behavior to improve predictions.

---

## State Machine

```
┌─────────────┐     temp rising toward trigger     ┌─────────────┐
│    IDLE     │ ─────────────────────────────────► │   COOLING   │
│  (pump off) │                                    │  (pump on)  │
└─────────────┘                                    └─────────────┘
       ▲                                                  │
       │                                                  │
       │ min_off_time elapsed                             │ predicted_final ≤ setpoint
       │ AND temp settled                                 ▼
       │                                           ┌─────────────┐
       │                                           │  COASTING   │
       │                                           │  (pump off) │
       │                                           └─────────────┘
       │                                                  │
       │         learn from cycle                         │ temp stabilized
       └──────────────────────────────────────────────────┘

                    ┌───────────────-───┐
                    │ EMERGENCY_COOLING │  (can't keep up - just run full out)
                    │    (pump on)      │
                    └────────────────-──┘
```

---

## Learned Parameters

The algorithm maintains several learned parameters that adapt to the specific system:

| Parameter | Units | Description | Initial Value |
|-----------|-------|-------------|---------------|
| `k` | minutes | Coast factor (rate-dependent model) | 5.0 |
| `C_off` | °C or °F | Average coast drop (rate-independent fallback) | 0.3 |
| `L` | seconds | Effective learned dead time from pump ON to observable cooling | 300 |
| `drift_rate` | °/min | Rate of temperature rise when idle | 0.02 |

---

## State Details

### IDLE

**Purpose:** Monitor drift, wait for cooling trigger.

```
While IDLE:
    - Track drift_rate (smoothed dT/dt while pump is off and settled)
    - Calculate lookahead using learned dead time:
        lookahead_time = L + buffer  (buffer ≈ 15-30s)
    - Calculate: anticipated_peak = current_temp + (drift_rate × lookahead_time)

    Transition to COOLING when:
        anticipated_peak ≥ setpoint + trigger_margin
        AND min_off_time has elapsed (15s)
```

**Trigger margin** is small (0.05-0.1°) to catch temperature rise early without excessive cycling.

---

### COOLING

**Purpose:** Cool the beer, predict when to stop.

```
On entry:
    - Record t_pump_on = now
    - Record temp_at_pump_on = current_temp
    - Clear cooling_confirmed flag

While COOLING:
    - Track cooling_rate (smoothed dT/dt)

    Dead time learning:
        - When cooling_rate first becomes clearly negative (< -threshold for sustained period):
            - If not cooling_confirmed:
                - L_observed = now - t_pump_on
                - Update L using EMA: L = 0.8 * L + 0.2 * L_observed
                - Set cooling_confirmed = true

    Wait for rate_settling_time (45s) before trusting predictions

    Calculate estimated coast (hybrid model):
        if abs(cooling_rate) > min_rate_for_k_model:  // e.g., 0.02 °/min
            estimated_coast = k × abs(cooling_rate)
        else:
            estimated_coast = C_off  // fallback when rate is tiny

    predicted_final = current_temp - estimated_coast

    Transition to COASTING when:
        predicted_final ≤ setpoint
        AND min_on_time has elapsed (15s)

    Transition to EMERGENCY_COOLING when:
        (see Emergency Detection below)

    SAFETY: Force transition to COASTING when:
        cooling_duration > max_continuous_on_time  // e.g., 45 min
        (Log fault condition)
```

---

### COASTING

**Purpose:** Observe the coast, measure for learning.

```
On entry:
    - Record temp_at_pump_off = current_temp
    - Record cooling_rate_at_pump_off = cooling_rate
    - Record cooling_duration = now - t_pump_on
    - min_temp_reached = current_temp

While COASTING:
    - Track temperature
    - Update min_temp_reached whenever we see a new minimum

    SAFETY: Force pump OFF if:
        current_temp < setpoint - safety_margin_low  // e.g., 0.5-1.0°
        (This catches "model went off the rails" cases)

    Transition to IDLE when:
        - Temperature has stabilized (dT/dt ≈ 0 or slightly positive)
        - OR temperature has started rising
        - AND min_off_time has elapsed

    On exit:
        Perform learning (see Learning section)
        Log cycle data
```

---

### EMERGENCY_COOLING

**Purpose:** We can't cool fast enough—run continuously and do our best.

```
On entry:
    - Log warning (user should check glycol temperature)
    - Pump ON (stays on)
    - Record emergency_entry_time = now

While EMERGENCY_COOLING:
    - Track cooling_rate
    - Track current_temp

    SAFETY: Force pump OFF if:
        emergency_duration > max_continuous_on_time
        OR current_temp < setpoint - safety_margin_low

    Transition to COOLING when (with hysteresis):
        current_temp ≤ setpoint
        OR (cooling is effective AND predicted to reach setpoint within exit_horizon)
        AND emergency_duration > min_emergency_dwell_time  // e.g., 2 min
```

---

## Emergency Detection (Horizon-Based)

Rather than triggering on instantaneous rate, use a horizon-based prediction:

```cpp
// In COOLING state, after rate_settling_time has elapsed:

float horizon_minutes = 20.0;  // Look 20 minutes ahead
float predicted_temp_at_horizon = current_temp + (cooling_rate * horizon_minutes);

// If we can't reach setpoint even in 20 minutes of continuous cooling:
if (predicted_temp_at_horizon > setpoint + emergency_margin) {
    // AND we've been trying for a while:
    if (cooling_duration > emergency_detection_time) {  // e.g., 90s
        transition_to(EMERGENCY_COOLING);
    }
}
```

This prevents false triggers when glycol is "barely keeping up" with a shallow but effective cooling slope.

---

## Predictive Model (Hybrid)

### Core Prediction

The central question while cooling: **"If I turn off the pump RIGHT NOW, where will the temperature end up?"**

```cpp
float estimated_coast;

if (abs(cooling_rate) > min_rate_for_k_model) {
    // Rate is substantial - use rate-dependent model
    estimated_coast = k * abs(cooling_rate);
} else {
    // Rate is tiny (near steady state) - use average coast
    estimated_coast = C_off;
}

float predicted_final = current_temp - estimated_coast;
```

**Turn off when:** `predicted_final ≤ setpoint`

### Why Two Models?

- **k × |cooling_rate|** works well when actively cooling at a meaningful rate
- **C_off** (simple average coast) works better near steady state where rate is tiny but thermal mass still causes coasting
- Using C_off also provides a sanity check on k updates

---

## Adaptive Learning

### Cycle Validity Checks

Not all cycles are equally informative. **Only update learned parameters when:**

```cpp
bool cycle_is_valid_for_training =
    cooling_duration >= min_training_duration        // e.g., 180s (3 min)
    AND (temp_at_pump_on - temp_at_pump_off) >= min_training_drop  // e.g., 0.2°
    AND no_setpoint_change_during_cycle
    AND abs(cooling_rate_at_pump_off) > min_training_rate;  // e.g., 0.01 °/min
```

### Learning the Coast Factor (k)

After each valid cooling cycle:

```cpp
float actual_coast = temp_at_pump_off - min_temp_reached;

// Update C_off (simple average coast) - always, for valid cycles
C_off = 0.85 * C_off + 0.15 * actual_coast;
C_off = constrain(C_off, 0.05, 2.0);  // reasonable bounds

// Only update k if rate was meaningful
if (abs(cooling_rate_at_pump_off) > min_training_rate) {
    float observed_k = actual_coast / abs(cooling_rate_at_pump_off);

    // Clamp observed_k BEFORE using it (prevents single bad cycle from wrecking k)
    observed_k = constrain(observed_k, 0.5, 20.0);

    // Sanity check against C_off
    // If observed_k implies a coast wildly different from C_off, be skeptical
    float implied_coast_at_typical_rate = observed_k * 0.05;  // typical rate
    if (implied_coast_at_typical_rate > C_off * 3.0 ||
        implied_coast_at_typical_rate < C_off * 0.3) {
        // This k seems unreasonable - reduce learning rate
        alpha = 0.05;
    } else {
        // Normal adaptive learning rate based on prediction error
        float prediction_error = setpoint - min_temp_reached;
        // positive = overshot cold, negative = undershot

        if (abs(prediction_error) > 0.5) {
            alpha = 0.5;   // Big miss - adapt fast
        } else if (abs(prediction_error) > 0.2) {
            alpha = 0.3;   // Medium miss
        } else {
            alpha = 0.15;  // Small miss - fine tuning
        }
    }

    // Update k with exponential moving average
    k = (1.0 - alpha) * k + alpha * observed_k;
    k = constrain(k, 1.0, 15.0);  // 1-15 minutes equivalent
}
```

### Learning the Drift Rate

Updated continuously while IDLE (after settling):

```cpp
// Use slightly faster adaptation to catch fermentation spikes
drift_rate = 0.85 * drift_rate + 0.15 * current_rate;

// Clamp to reasonable bounds
drift_rate = constrain(drift_rate, 0.0, 0.5);  // °/min, only positive (warming)
```

### Learning the Dead Time (L)

Updated when cooling is first confirmed each cycle:

```cpp
// When cooling_rate first becomes clearly negative after pump ON:
if (!cooling_confirmed && cooling_rate < -confirmation_threshold) {
    // Require sustained negative rate (e.g., 3 consecutive readings)
    if (++negative_rate_count >= 3) {
        L_observed = (now - t_pump_on) / 1000.0;  // convert to seconds
        L_observed = constrain(L_observed, 5.0, 300.0);  // reasonable bounds
        L = 0.8 * L + 0.2 * L_observed;
        cooling_confirmed = true;
    }
} else if (cooling_rate >= -confirmation_threshold) {
    negative_rate_count = 0;  // reset counter
}
```

---

## Rate Measurement

Reliable rate measurement (dT/dt) is critical. Sensor noise and changing dynamics require good filtering.

### Sliding Window Linear Regression

```cpp
// Keep last N samples (60-90 seconds worth)
// Calculate slope via least-squares fit

struct Sample {
    uint32_t timestamp_ms;
    float temp;             // Units: °C or °F (be consistent!)
};

CircularBuffer<Sample, 30> rate_buffer;  // 30 samples at 2-3s intervals

float calculate_rate() {
    // Guard against insufficient data
    if (rate_buffer.size() < 5) return 0.0;

    // Guard against stale data (large time gaps)
    uint32_t newest = rate_buffer[rate_buffer.size() - 1].timestamp_ms;
    uint32_t oldest = rate_buffer[0].timestamp_ms;
    if (newest - oldest > 180000) {  // > 3 minutes of data is stale
        // Clear old samples or return 0
        return 0.0;
    }

    // Linear regression: temp = rate * time + intercept
    float sum_t = 0, sum_temp = 0, sum_t2 = 0, sum_t_temp = 0;
    float t0 = rate_buffer[0].timestamp_ms;

    for (int i = 0; i < rate_buffer.size(); i++) {
        float t = (rate_buffer[i].timestamp_ms - t0) / 60000.0;  // minutes
        float temp = rate_buffer[i].temp;
        sum_t += t;
        sum_temp += temp;
        sum_t2 += t * t;
        sum_t_temp += t * temp;
    }

    int n = rate_buffer.size();
    float denominator = n * sum_t2 - sum_t * sum_t;

    // Guard against division by zero (all samples at same time)
    if (abs(denominator) < 0.0001) return 0.0;

    float rate = (n * sum_t_temp - sum_t * sum_temp) / denominator;

    return rate;  // Units: °/min (same units as temp, per minute)
}
```

---

## Safety Features

### Hard Temperature Limits

```cpp
// In COASTING or any state:
if (current_temp < setpoint - safety_margin_low) {
    // Beer is too cold! Force pump OFF, skip to IDLE
    pump_off();
    log_warning("Safety limit: beer too cold");
    transition_to(IDLE);
}
```

### Maximum Continuous Run Time

```cpp
// In COOLING or EMERGENCY_COOLING:
if (cooling_duration > max_continuous_on_time) {
    pump_off();
    log_fault("Max continuous run time exceeded");
    transition_to(COASTING);  // or a FAULT state
}
```

### Setpoint Change Handling

```cpp
void on_setpoint_change(float new_setpoint) {
    setpoint = new_setpoint;
    setpoint_changed_this_cycle = true;  // Mark cycle as invalid for training

    // Immediate re-evaluation
    if (state == COOLING) {
        if (new_setpoint > current_temp) {
            // New setpoint is above current temp - stop cooling immediately
            transition_to(COASTING);
        }
        // Otherwise, continue cooling with new target
    }
}
```

---

## Timing Parameters

| Parameter | Value | Units | Rationale |
|-----------|-------|-------|-----------|
| `min_on_time` | 15 | seconds | Pump protection |
| `min_off_time` | 15 | seconds | Pump protection |
| `rate_settling_time` | 45 | seconds | Need stable rate before trusting predictions |
| `emergency_detection_time` | 90 | seconds | Give cooling time to take effect |
| `emergency_horizon` | 20 | minutes | Look-ahead for "can't catch up" detection |
| `min_emergency_dwell_time` | 120 | seconds | Prevent bouncing in/out of emergency |
| `max_continuous_on_time` | 45 | minutes | Safety limit |

---

## Initial/Default Values

For first-time operation (no learned values):

| Parameter | Initial Value | Units | Notes |
|-----------|---------------|-------|-------|
| `k` | 5.0 | minutes | Conservative coast factor |
| `C_off` | 0.3 | ° | Typical coast drop |
| `L` | 300 | seconds | Conservative effective dead time; learned from observed system response |
| `drift_rate` | 0.02 | °/min | Typical ambient-driven drift |
| `trigger_margin` | 0.1 | ° | Small hysteresis |
| `safety_margin_low` | 0.5 | ° | Hard limit below setpoint |

---

## Configurable Parameters

These should be exposed for advanced users to tune without firmware changes:

```cpp
struct GlycolConfig {
    // Timing
    uint16_t min_on_time_s = 15;
    uint16_t min_off_time_s = 15;
    uint16_t rate_settling_time_s = 45;
    uint16_t max_continuous_on_time_min = 45;

    // Learning thresholds
    float min_training_rate = 0.01;       // °/min - minimum rate to trust for k training
    float min_training_duration_s = 180;  // seconds - minimum cycle length to learn from
    float min_training_drop = 0.2;        // ° - minimum temp drop to learn from

    // Safety
    float safety_margin_low = 0.5;        // ° below setpoint

    // Prediction
    float min_rate_for_k_model = 0.02;    // °/min - below this, use C_off instead
    float trigger_margin = 0.1;           // ° above setpoint to trigger cooling

    // Emergency
    float emergency_horizon_min = 20.0;   // minutes to look ahead
    uint16_t emergency_detection_time_s = 90;
    uint16_t min_emergency_dwell_time_s = 120;
};
```

---

## Data Logging

Each cooling cycle should log for debugging/tuning:

```cpp
struct CycleLog {
    uint32_t timestamp;
    float temp_at_start;
    float temp_at_pump_off;
    float min_temp_reached;
    float setpoint;
    uint16_t cooling_duration_s;
    float cooling_rate_at_pump_off;  // °/min
    float predicted_coast;
    float actual_coast;
    float prediction_error;
    float k_before;
    float k_after;
    float C_off_after;
    float L_observed;
    bool was_emergency;
    bool was_valid_for_training;
};
```

---

## Edge Cases

### Setpoint Changes Mid-Cycle
Immediately re-evaluate. If new setpoint is above current temp, stop cooling. Mark cycle as invalid for training (don't update k or C_off).

### Very Short Cycles
If we trigger cooling but hit the stop condition almost immediately, honor min_on_time. If cycle is shorter than min_training_duration, don't use it for learning.

### Coast Exceeds Prediction Significantly
C_off acts as a sanity check. If observed_k implies a coast wildly different from C_off, reduce the learning rate for that update.

### Temperature Sensor Noise/Glitches
The linear regression rate calculation provides robustness. Consider adding outlier rejection for extreme single readings.

### Glycol Warmer Than Beer
Cooling rate would be positive (warming!). This triggers emergency detection since we can't reach setpoint. User should be alerted.

### Rate Near Zero at Pump-Off
When cooling_rate is tiny (approaching glycol temp or steady state), the k model breaks down. The hybrid model falls back to C_off in this case.

### Division by Zero
All divisions by rate are guarded with minimum thresholds. The rate calculation guards against zero denominator.

---

## Algorithm Summary

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CONTROL LOOP                                │
├─────────────────────────────────────────────────────────────────────┤
│  PREDICT:  Where will we end up if we act now?                      │
│            • IDLE: anticipated_peak = temp + drift_rate × (L+buffer)│
│            • COOLING: predicted_final = temp - estimated_coast      │
│              where estimated_coast = k×rate (or C_off if rate tiny) │
│                                                                     │
│  DECIDE:   Compare prediction to setpoint                           │
│            • Start cooling when anticipated_peak ≥ setpoint         │
│            • Stop cooling when predicted_final ≤ setpoint           │
│                                                                     │
│  LEARN:    After each valid cycle, compare prediction to reality    │
│            • Update k based on coast prediction error               │
│            • Update C_off as simple average coast                   │
│            • Update L when cooling is first detected                │
│            • Adapt faster when errors are large                     │
│            • Skip learning for short/interrupted cycles             │
│                                                                     │
│  SAFETY:   Hard limits that override prediction                     │
│            • Max continuous ON time (45 min)                        │
│            • Min temperature limit (setpoint - margin)              │
│            • Setpoint change handling                               │
│                                                                     │
│  FALLBACK: If predictions aren't working, run full out              │
│            • Horizon-based "can't catch up" detection               │
│            • Hysteresis on entry/exit                               │
│            • Return to predictive control when back on track        │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Future Considerations

### Heating
This document focuses on cooling only. Heating control will need to be addressed separately and may have different dynamics (e.g., heating elements vs heat exchange, different response times, different thermal mass considerations).

### Duty-Cycle Modulation
For very tight control near setpoint, a PWM-style approach within fixed time windows could be layered on top of this algorithm. This would allow finer-grained control when the error is small. Not implemented in V1.

### Regime-Based Parameters
The current hybrid model (k vs C_off) handles most cases. If needed, separate k values could be maintained for different operating regimes (high temp, near setpoint, near glycol temp). The C_off fallback makes this less critical.
