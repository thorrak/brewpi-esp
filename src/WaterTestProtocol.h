#pragma once
#include "WaterTestCore.h"
#include <ArduinoJson.h>
#include <string>

// Shared by firmware and host contract tests. No network or hardware dependencies.
namespace WaterTestProtocol {
using namespace WaterTestCore;
inline void recordToJson(JsonObject out, const WaterTestCore::Record &r, const char *bootId,
                         double calibrationOffsetC) {
  out["boot_id"] = bootId;
  out["seq"] = r.seq;
  out["t_us"] = r.t_us;
  switch (r.kind) {
  case 0:
    out["type"] = "boot";
    out["reason"] = reasonName(static_cast<Reason>(r.code));
    out["reset_reason"] = r.detail;
    out["initial_outputs"]["pump_on"] = false;
    out["initial_outputs"]["heater_on"] = false;
    break;
  case 1:
    out["type"] = "clock_sync";
    out["utc_us"] = r.read_us;
    out["source"] = "startup_ntp";
    out["status"] = "synced";
    break;
  case 2: {
    out["type"] = "sample";
    out["sensor_role"] = r.role ? "glycol" : "beer";
    out["quality"] = (r.flags & 1) ? "ok" : "sensor_error";
    if (r.flags & 1) {
      out["raw_sixteenths_c"] = r.raw;
      out["raw_c"] = r.raw / 16.0;
      double offset = calibrationOffsetC;
      out["adjusted_c"] = r.raw / 16.0 + offset;
      out["decision_c"] = r.raw / 16.0 + offset;
    } else {
      out["raw_c"] = nullptr;
      out["raw_sixteenths_c"] = nullptr;
      out["adjusted_c"] = nullptr;
      out["decision_c"] = nullptr;
    }
    out["conversion_start_us"] = r.read_us >= r.conversion_us ? r.read_us - r.conversion_us : 0;
    out["read_us"] = r.read_us;
    out["sample_age_us"] = r.t_us >= r.read_us ? r.t_us - r.read_us : 0;
    out["pump_on"] = bool(r.flags & 2);
    out["heater_on"] = false;
    break;
  }
  case 3:
    out["type"] = "output";
    out["actuator"] = r.role ? "heater" : "pump";
    out["requested_on"] = bool(r.flags & 4);
    out["applied_on"] = r.role ? false : bool(r.flags & 2);
    out["edge"] = bool(r.flags & 8);
    out["reason"] = reasonName(static_cast<Reason>(r.code));
    break;
  case 4:
    out["type"] = "phase";
    out["phase"] = phaseName(static_cast<Phase>(r.code));
    out["pulse_number"] = r.pulse;
    out["planned_remaining_s"] = r.detail;
    break;
  default:
    out["type"] = r.kind == 6 ? "gap" : "fault";
    out["reason"] = reasonName(static_cast<Reason>(r.code));
    out["terminated"] = true;
    break;
  }
}
inline bool acknowledged(JsonVariantConst response, const std::string &testId, const char *deviceGuid) {
  return response["test_id"] == testId && response["device_guid"] == deviceGuid &&
         (response["status"] == "stored" || response["status"] == "already_present");
}
inline bool batchAcknowledged(JsonVariantConst response, const std::string &testId, const char *guid,
                              const std::string &batchId, uint32_t first, uint32_t last) {
  auto ranges = response["accepted_ranges"].as<JsonArrayConst>();
  return acknowledged(response, testId, guid) && response["batch_id"] == batchId && ranges.size() == 1 &&
         ranges[0].is<JsonArrayConst>() && ranges[0].size() == 2 && ranges[0][0].is<uint32_t>() &&
         ranges[0][1].is<uint32_t>() && ranges[0][0].as<uint32_t>() == first && ranges[0][1].as<uint32_t>() == last;
}
inline bool finishAcknowledged(JsonVariantConst response, const std::string &testId, const char *guid) {
  return acknowledged(response, testId, guid) &&
         (response["upload_status"] == "complete" || response["upload_status"] == "partial") &&
         response["missing_record_count"].is<uint32_t>() && response["missing_record_count"].as<uint32_t>() == 0 &&
         response["finish_received"] == true;
}
inline std::string batchIdentifier(const std::string &testId, uint32_t firstIndex) {
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned>(firstIndex));
  std::string id = testId;
  id.replace(28, 8, suffix);
  return id;
}
} // namespace WaterTestProtocol
