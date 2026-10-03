#pragma once

#include "GlycolCoolingController.h"
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// JSON numbers are convenient for readers, but ArduinoJson rounds doubles when
// writing them. The parallel 17-digit strings preserve binary64 values through
// durable JSON reloads and immutable upload retries.
namespace WaterTestControllerSnapshot {
inline void number(JsonObject values, JsonObject exact, const char *key, double value) {
  if (!std::isfinite(value)) {
    values[key] = nullptr;
    exact[key] = nullptr;
    return;
  }
  char decimal[32];
  snprintf(decimal, sizeof(decimal), "%.17g", value);
  values[key] = value;
  exact[key] = decimal;
}
// Reloading a rounded JSON number can select ArduinoJson's float32 storage;
// serializing that float again loses more digits. Restore the exact double in
// memory before assembling a new document, without rewriting durable payloads.
inline bool restoreNumber(JsonVariant value, JsonVariantConst exact) {
  if (value.isNull() && exact.isNull())
    return true;
  if (!value.is<double>() || !exact.is<const char *>())
    return false;
  const char *decimal = exact.as<const char *>();
  const size_t length = std::strlen(decimal);
  if (!length || length >= 32)
    return false;
  // Our encoding is a decimal %.17g string. strtod alone also accepts whitespace,
  // hexadecimal numbers and nonfinite values, which are not this encoding.
  for (size_t n = 0; n < length; ++n)
    if ((decimal[n] < '0' || decimal[n] > '9') && decimal[n] != '.' && decimal[n] != '-' &&
        decimal[n] != '+' && decimal[n] != 'e' && decimal[n] != 'E')
      return false;
  char *end = nullptr;
  errno = 0;
  const double restored = std::strtod(decimal, &end);
  if (end != decimal + length || !std::isfinite(restored) || errno == ERANGE)
    return false;
  return value.set(restored);
}
inline bool restoreNumbers(JsonVariant values, JsonVariantConst exact) {
  if (values.isNull() && exact.isNull())
    return true;
  if (!values.is<JsonObject>() || !exact.is<JsonObjectConst>())
    return false;
  for (JsonPairConst field : exact.as<JsonObjectConst>())
    if (!restoreNumber(values[field.key()], field.value()))
      return false;
  return true;
}
inline bool restoreExactNumbers(JsonObject snapshot) {
  if (snapshot["numeric_encoding"] != "binary64-decimal-v1")
    return false;
  return restoreNumbers(snapshot["configuration"], snapshot["configuration_exact"]) &&
         restoreNumbers(snapshot["initial_tuning"], snapshot["initial_tuning_exact"]) &&
         restoreNumbers(snapshot["final_tuning"], snapshot["final_tuning_exact"]) &&
         restoreNumber(snapshot["target_c"], snapshot["target_c_exact"]);
}
template <typename Configuration> struct Field {
  const char *name;
  double Configuration::*member;
};
template <typename Configuration, size_t N>
inline void configuration(JsonObject values, JsonObject exact, const Configuration &config,
                          const Field<Configuration> (&fields)[N]) {
  for (const auto &field : fields)
    number(values, exact, field.name, config.*(field.member));
}
inline void configuration(JsonObject values, JsonObject exact, const PredictiveCooling::Config &config) {
  using C = PredictiveCooling::Config;
  const Field<C> fields[] = {
      {"min_on_s", &C::min_on_s}, {"min_off_s", &C::min_off_s},
      {"rate_window_s", &C::rate_window_s}, {"measurement_window_s", &C::measurement_window_s},
      {"deadband_c", &C::deadband_c}, {"initial_coast_s", &C::initial_coast_s},
      {"min_coast_estimate_s", &C::min_coast_estimate_s}, {"max_coast_estimate_s", &C::max_coast_estimate_s},
      {"learning_fraction", &C::learning_fraction}, {"rate_floor_c_per_s", &C::rate_floor_c_per_s},
      {"observe_coast_s", &C::observe_coast_s}, {"max_observe_coast_s", &C::max_observe_coast_s},
      {"near_target_c", &C::near_target_c}, {"startup_pulse_s", &C::startup_pulse_s},
      {"startup_budget_c_per_s", &C::startup_budget_c_per_s}, {"restart_margin_c", &C::restart_margin_c},
      {"budget_learning_fraction", &C::budget_learning_fraction},
      {"minimum_budget_gain_c_per_s", &C::minimum_budget_gain_c_per_s},
      {"maximum_blind_budget_s", &C::maximum_blind_budget_s}};
  configuration(values, exact, config, fields);
}
inline void configuration(JsonObject values, JsonObject exact, const AdaptiveCooling::Config &config) {
  using C = AdaptiveCooling::Config;
  const Field<C> fields[] = {
      {"min_on_s", &C::min_on_s}, {"min_off_s", &C::min_off_s},
      {"rate_window_s", &C::rate_window_s}, {"measurement_window_s", &C::measurement_window_s},
      {"deadband_c", &C::deadband_c}, {"initial_gain_c_per_on_s", &C::initial_gain_c_per_on_s},
      {"minimum_gain_c_per_on_s", &C::minimum_gain_c_per_on_s},
      {"maximum_gain_c_per_on_s", &C::maximum_gain_c_per_on_s}, {"dose_fraction", &C::dose_fraction},
      {"learning_fraction", &C::learning_fraction}, {"observe_coast_s", &C::observe_coast_s},
      {"max_observe_coast_s", &C::max_observe_coast_s}, {"initial_probe_s", &C::initial_probe_s},
      {"far_probe_s", &C::far_probe_s}, {"far_error_c", &C::far_error_c},
      {"saturation_dose_s", &C::saturation_dose_s}, {"unresolved_error_c", &C::unresolved_error_c},
      {"stop_horizon_s", &C::stop_horizon_s}, {"settled_rate_c_per_s", &C::settled_rate_c_per_s}};
  configuration(values, exact, config, fields);
}
inline bool tuning(JsonObject values, JsonObject exact, const GlycolCooling::Controller &controller) {
  const auto tuning = controller.tuning();
  if (controller.selection() == GlycolCooling::Algorithm::PulseDose) {
    number(values, exact, "gain_c_per_on_s", tuning.pulse_dose.gain_c_per_on_s);
    values["learning_updates"] = tuning.pulse_dose.learning_updates;
    return tuning.pulse_dose.learning_updates != 0;
  }
  number(values, exact, "coast_s", tuning.predictive.coast_s);
  number(values, exact, "budget_gain_c_per_s", tuning.predictive.budget_gain_c_per_s);
  values["learning_updates"] = tuning.predictive.learning_updates;
  values["response_updates"] = tuning.predictive.response_updates;
  return tuning.predictive.learning_updates != 0 || tuning.predictive.response_updates != 0;
}
inline void initial(JsonObject out, const GlycolCooling::Controller &controller, const char *implementationId) {
  out["selection"] = GlycolCooling::selectionName(controller.selection());
  out["version"] = controller.algorithmVersion();
  out["implementation_id"] = implementationId;
  out["tuning_schema_version"] = 1;
  out["numeric_encoding"] = "binary64-decimal-v1";
  out["initialization"] = "fresh_defaults";
  auto config = out["configuration"].to<JsonObject>();
  auto exact = out["configuration_exact"].to<JsonObject>();
  if (controller.selection() == GlycolCooling::Algorithm::PulseDose)
    configuration(config, exact, controller.doseConfiguration());
  else
    configuration(config, exact, controller.predictiveConfiguration());
  tuning(out["initial_tuning"].to<JsonObject>(), out["initial_tuning_exact"].to<JsonObject>(), controller);
}
inline void final(JsonObject out, JsonObjectConst initial, const GlycolCooling::Controller *controller,
                  bool interrupted, uint64_t startedUs, double targetC, uint64_t capturedUs) {
  for (auto key : {"selection", "version", "implementation_id", "tuning_schema_version", "numeric_encoding"})
    out[key] = initial[key];
  if (interrupted) {
    out["initialized"] = nullptr;
    out["learning_status"] = "unavailable_after_restart";
  } else {
    out["initialized"] = controller != nullptr;
    out["learning_status"] = "not_started";
  }
  if (!controller || interrupted) {
    out["final_tuning"] = nullptr;
    out["final_tuning_exact"] = nullptr;
    return;
  }
  const bool learned = tuning(out["final_tuning"].to<JsonObject>(), out["final_tuning_exact"].to<JsonObject>(),
                              *controller);
  out["learning_status"] = learned ? "learned" : "no_updates";
  out["started_us"] = startedUs;
  out["captured_at_us"] = capturedUs;
  char target[32];
  snprintf(target, sizeof(target), "%.17g", targetC);
  out["target_c"] = targetC;
  out["target_c_exact"] = target;
}
} // namespace WaterTestControllerSnapshot
