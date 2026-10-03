#include "WaterTestProtocol.h"
#include <cassert>
#include <iostream>
using namespace WaterTestCore;
using namespace WaterTestProtocol;
int main() {
  const std::string id = "a7380ef0-73be-4bab-9ac8-8f3199a8e025";
  const char *guid = "123456780000ABCD";
  const char *boot = "87dd2780-7b53-49d3-9fa7-a2c4acde0de4";
  const std::string batch = batchIdentifier(id, 12);
  assert(batch == batchIdentifier(id, 12));
  assert(batch != batchIdentifier(id, 24));
  assert(batch.substr(0, 28) == id.substr(0, 28));
  assert(batch.size() == 36);
  Record r{};
  r.kind = 2;
  r.role = 1;
  r.flags = 1 | 2;
  r.raw = -10;
  r.seq = 12;
  r.read_us = 123456789;
  r.t_us = r.read_us + 100;
  r.conversion_us = 750000;
  JsonDocument doc;
  recordToJson(doc.to<JsonObject>(), r, boot, .25);
  assert(doc["sensor_role"] == "glycol");
  assert(doc["raw_c"] == -.625);
  assert(doc["adjusted_c"] == -.375);
  assert(doc["read_us"].as<uint64_t>() == r.read_us);
  assert(doc["conversion_start_us"].as<uint64_t>() == r.read_us - 750000);
  assert(doc["sample_age_us"] == 100);
  assert(doc["pump_on"] == true);
  assert(doc["heater_on"] == false);
  assert(doc["boot_id"] == boot);
  r.flags = 0;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["quality"] == "sensor_error");
  assert(doc["raw_c"].isNull());
  assert(doc["raw_sixteenths_c"].isNull());
  assert(doc["adjusted_c"].isNull());
  r.kind = 3;
  r.role = 0;
  r.flags = 2 | 4 | 8;
  r.code = static_cast<uint8_t>(Reason::Pilot);
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["type"] == "output");
  assert(doc["applied_on"] == true);
  assert(doc["requested_on"] == true);
  assert(doc["edge"] == true);
  r.role = 1;
  r.flags = 0;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["applied_on"] == false);
  r.kind = 1;
  r.read_us = 1790380800100000ULL;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["utc_us"].as<uint64_t>() == r.read_us);
  r.kind = 4;
  r.raw = 7;
  r.code = static_cast<uint8_t>(Phase::Observe);
  r.flags = static_cast<uint8_t>(Role::Validation);
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["block_id"] == 7 && doc["analysis_role"] == "validation");
  assert(doc["observation_started_us"].isNull());
  r.code = static_cast<uint8_t>(Phase::ControllerFinalObserve);
  r.flags = static_cast<uint8_t>(Role::Controller);
  r.read_us = 100000000;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["phase"] == "controller_final_observe" && doc["analysis_role"] == "controller");
  assert(doc["observation_started_us"].as<uint64_t>() == r.read_us);
  r.kind = 7;
  r.role = 0; // Existing diagnostic records did not carry a controller-run id.
  r.code = 1;
  r.pulse = 2;
  r.raw = 312;
  r.flags = 2;
  const float rate = -.003f, coast = 123.f, endpoint = 19.25f, gain = .0125f;
  memcpy(&r.read_us,&rate,4);
  memcpy(reinterpret_cast<char *>(&r.read_us)+4,&coast,4);
  memcpy(&r.conversion_us,&endpoint,4);
  memcpy(&r.detail,&gain,4);
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["type"] == "controller" && doc["algorithm"] == "pulse_dose");
  assert(doc["controller_phase"] == 2 && doc["target_c"] == 19.5 && doc["pump_on"] == true);
  assert(doc["cooling_rate_c_per_s"].as<float>() == rate && doc["coast_s"] == 123);
  assert(doc["predicted_endpoint_c"] == 19.25 && doc["gain_c_per_on_s"].as<float>() == gain);
  assert(doc.size() == 12 && doc["controller_run"].isNull());
  r.role = 2;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["controller_run"] == 2 && doc["algorithm"] == "pulse_dose");
  assert(doc.size() == 13);
  const float missing = NAN;
  memcpy(&r.conversion_us,&missing,4);
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["predicted_endpoint_c"].isNull());
  r = {};
  r.kind = 8;
  r.pulse = 1;
  r.t_us = 200000000;
  r.read_us = 199000000;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["type"] == "controller_episode" && doc["stage"] == "initial_approach");
  assert(doc["episode"] == 1 && doc["event"] == "started" && doc["event_us"] == 199000000);
  assert(doc.size() == 8 && doc["controller_run"].isNull() && doc["algorithm"].isNull());
  r.pulse = 3;
  r.code = 1;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["stage"] == "maintenance" && doc["episode"] == 3 && doc["event"] == "settled");
  assert(doc.size() == 8 && doc["controller_run"].isNull() && doc["algorithm"].isNull());

  // Each run has its own episode 1. Run identity and selected algorithm make
  // the repeated local episode number unambiguous in the shared journal.
  r.pulse = 1;
  r.code = 0;
  for (unsigned run = 1; run <= 2; ++run) {
    r.role = run;
    r.detail = run - 1;
    doc.clear();
    recordToJson(doc.to<JsonObject>(), r, boot, 0);
    assert(doc["episode"] == 1 && doc["stage"] == "initial_approach");
    assert(doc["controller_run"] == run);
    assert(doc["algorithm"] == (run == 1 ? "predictive_coast" : "pulse_dose"));
    assert(doc.size() == 10);
  }
  r.role = 2;
  r.detail = 0; // Selected-algorithm-first order can put predictive in run 2.
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["controller_run"] == 2 && doc["algorithm"] == "predictive_coast");

  // A 1 degree F difference is not on the 1/16 C sensor grid. Preserve the
  // target's binary64 value exactly even through JSON serialization/parsing.
  const auto targetBits = [](Record &record, double target) {
    uint64_t bits = 0;
    memcpy(&bits, &target, sizeof(bits));
    record.conversion_us = uint32_t(bits);
    record.detail = uint32_t(bits >> 32);
  };
  for (unsigned run = 1; run <= 2; ++run) {
    const double start = run == 1 ? 20.0 : 19.375;
    const double target = start - 5.0 / 9.0;
    assert(std::round(target * 16) / 16 != target);
    for (unsigned event = 0; event <= 1; ++event) {
      r = {};
      r.kind = 9;
      r.role = run;
      r.code = run - 1;
      r.pulse = event;
      r.raw = std::lround(start * 16);
      r.t_us = 700000000 + run;
      r.read_us = 699000000 + run;
      targetBits(r, target);
      doc.clear();
      recordToJson(doc.to<JsonObject>(), r, boot, 0);
      assert(doc["type"] == "controller_run" && doc["run"] == run);
      assert(doc["algorithm"] == (run == 1 ? "predictive_coast" : "pulse_dose"));
      assert(doc["event"] == (event == 0 ? "started" : "finished"));
      assert(doc["event_us"].as<uint64_t>() == r.read_us);
      assert(doc["t_us"].as<uint64_t>() == r.t_us);
      assert(doc["start_c"] == start && doc["target_c"].as<double>() == target);
      assert(doc["target_c_exact"].is<const char *>());
      std::string encoded;
      serializeJson(doc, encoded);
      JsonDocument restored;
      assert(deserializeJson(restored, encoded) == DeserializationError::Ok);
      const char *exact = restored["target_c_exact"];
      char *end = nullptr;
      const double decoded = std::strtod(exact, &end);
      assert(end && *end == '\0');
      assert(memcmp(&decoded, &target, sizeof(target)) == 0);
    }
  }
  for (double target : {NAN, INFINITY, -INFINITY}) {
    targetBits(r, target);
    doc.clear();
    recordToJson(doc.to<JsonObject>(), r, boot, 0);
    assert(doc["type"] == "controller_run");
    assert(doc["target_c"].isNull() && doc["target_c_exact"].isNull());
  }
  r.role = 1;
  r.code = 1;
  targetBits(r, 20.0 - 5.0 / 9.0);
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["run"] == 1 && doc["algorithm"] == "pulse_dose");
  // Legacy run boundaries remain unchanged; only v3 finished boundaries
  // carry durable proof that the full run duration actually elapsed.
  r.kind = 9;
  r.pulse = 1;
  r.flags = 0;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["run_duration_complete"].isNull());
  for (bool complete : {false, true}) {
    r.flags = 128 | (complete ? 1 : 0);
    doc.clear();
    recordToJson(doc.to<JsonObject>(), r, boot, 0);
    assert(doc["run_duration_complete"].is<bool>());
    assert(doc["run_duration_complete"].as<bool>() == complete);
  }
  r.pulse = 0;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["run_duration_complete"].isNull());
  // New observation ordinals use all 32 bits independently of retired
  // 16-bit episode IDs. A closure's reason and measured rate qualification
  // remain distinct, including a time-limited but rate-qualified coast.
  const char *observationReasons[] = {"rate_condition", "coast_time_limit", "rate_unqualified", "interrupted"};
  for (unsigned run = 1; run <= 2; ++run) {
    for (unsigned reason = 1; reason <= 4; ++reason) {
      r = {};
      r.kind = 10;
      r.role = run;
      r.code = run - 1;
      r.pulse = reason;
      r.flags = reason == 1 ? 1 : 0;
      r.detail = 70000 + reason;
      r.read_us = 9000000000ULL + reason;
      r.t_us = r.read_us + 1000;
      const float measuredCoast = reason == 4 ? 0.f : 450.125f;
      memcpy(&r.conversion_us, &measuredCoast, sizeof(measuredCoast));
      doc.clear();
      recordToJson(doc.to<JsonObject>(), r, boot, 0);
      assert(doc["type"] == "controller_observation" && doc["controller_run"] == run);
      assert(doc["algorithm"] == (run == 1 ? "predictive_coast" : "pulse_dose"));
      assert(doc["observation"] == 70000 + reason && doc["event"] == "finished");
      assert(doc["event_us"].as<uint64_t>() == r.read_us && doc["t_us"].as<uint64_t>() == r.t_us);
      assert(doc["reason"] == observationReasons[reason - 1]);
      assert(doc["rate_qualified"].as<bool>() == (reason == 1));
      assert(doc["coast_s"].as<float>() == measuredCoast);
      assert(doc["episode"].isNull() && doc["stage"].isNull());
    }
  }
  r.pulse = 2;
  r.flags = 1;
  doc.clear();
  recordToJson(doc.to<JsonObject>(), r, boot, 0);
  assert(doc["reason"] == "coast_time_limit" && doc["rate_qualified"] == true);
  JsonDocument ack;
  ack["test_id"] = id;
  ack["device_guid"] = guid;
  ack["status"] = "stored";
  ack["batch_id"] = batch;
  auto range = ack["accepted_ranges"].to<JsonArray>().add<JsonArray>();
  range.add(12);
  range.add(23);
  assert(batchAcknowledged(ack, id, guid, batch, 12, 23));
  ack["status"] = "already_present";
  assert(batchAcknowledged(ack, id, guid, batch, 12, 23));
  assert(!batchAcknowledged(ack, id, guid, batch, 12, 24));
  assert(!batchAcknowledged(ack, id, guid, batchIdentifier(id, 24), 12, 23));
  ack["device_guid"] = "WRONG";
  assert(!batchAcknowledged(ack, id, guid, batch, 12, 23));
  ack["device_guid"] = guid;
  ack["accepted_ranges"][0][0] = nullptr;
  assert(!batchAcknowledged(ack, id, guid, batch, 0, 23));
  ack["upload_status"] = "complete";
  ack["finish_received"] = true;
  ack["missing_record_count"] = 0;
  assert(finishAcknowledged(ack, id, guid));
  ack["missing_record_count"] = 1;
  assert(!finishAcknowledged(ack, id, guid));
  ack["missing_record_count"] = nullptr;
  assert(!finishAcknowledged(ack, id, guid));
  ack["missing_record_count"] = 0;
  ack["finish_received"] = false;
  assert(!finishAcknowledged(ack, id, guid));
  ack["finish_received"] = true;
  ack["upload_status"] = "receiving";
  assert(!finishAcknowledged(ack, id, guid));
  ack["upload_status"] = "partial";
  assert(finishAcknowledged(ack, id, guid));
  std::cout
      << "water_test_protocol: legacy records, observation closures, dual-run identity, exact targets and acknowledgements passed\n";
}
