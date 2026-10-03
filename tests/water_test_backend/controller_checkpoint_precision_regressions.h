#pragma once

constexpr double checkpointPrecisionGain = .010509570386312271;

void verifyCheckpointPrecision(JsonVariantConst result) {
  assertExact(result["final_tuning_exact"]["budget_gain_c_per_s"], checkpointPrecisionGain);
  assert(result["final_tuning"]["budget_gain_c_per_s"].as<double>() == checkpointPrecisionGain);
  assert(result["final_tuning"]["learning_updates"] == 7);
  assert(result["final_tuning"]["response_updates"] == 11);
  assert(result["target_c"].as<double>() == std::strtod(result["target_c_exact"], nullptr));
}

void verifyCheckpointPrecisionUpload() {
  verifyCheckpointPrecision(WaterTest::terminal["controllers"][0]);
  std::string finishBytes;
  assert(WaterTest::loadDocumentPayload(WaterTest::finishPath, finishBytes));
  assert(finishBytes.find("\"budget_gain_c_per_s\":0.01050957,") != std::string::npos);
  // Reloading an already durable finish must not cause the uploader to rewrite
  // it, even though the JSON parser may select float32 for its numeric companion.
  JsonDocument reloaded;
  assert(WaterTest::loadDocument(WaterTest::finishPath, reloaded));
  WaterTest::terminal = reloaded;
  submit();
  assert(Native::payloads.back() == finishBytes);
}

void controllerCheckpointPrecisionRegression(bool reboot) {
  start();
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  WaterTest::serviceProgram(true);
  assert(WaterTest::testController);
  auto learned = WaterTest::testController->tuning();
  learned.predictive = {300.12345678901234, checkpointPrecisionGain, 7, 11};
  assert(WaterTest::testController->restoreTuning(learned));
  Native::clock += 2000000;
  p.sample(Native::clock / 1e6, p.targetC, true, Native::clock / 1e6);
  p.deadline = p.controllerRunDeadline = Native::clock / 1e6;
  p.tick(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Controller);
  assert(p.phase == Phase::ControllerTransition && WaterTest::controllerCheckpointed[0]);
  std::string checkpointBytes;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerOnePath, checkpointBytes));
  assert(checkpointBytes.find("\"budget_gain_c_per_s\":0.01050957,") != std::string::npos);
  // Reproduce the real serializer failure: a plain load/save loses additional
  // precision even though the independent exact string remains intact.
  JsonDocument plain;
  assert(WaterTest::loadDocument(WaterTest::controllerOnePath, plain));
  std::string roundedAgain;
  serializeJson(plain, roundedAgain);
  assert(roundedAgain.find("\"budget_gain_c_per_s\":0.01051,") != std::string::npos);
  JsonDocument restored;
  assert(WaterTest::loadControllerCheckpoint(1, restored));
  verifyCheckpointPrecision(restored.as<JsonVariantConst>());
  if (reboot) {
    WaterTest::closeJournal();
    return;
  }
  p.finish(Native::clock / 1e6, End::Stopped, Reason::UserStop);
  WaterTest::applyProgram(Phase::ControllerTransition);
  assert(!WaterTest::active());
  std::string retained;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerOnePath, retained) && retained == checkpointBytes);
  verifyCheckpointPrecisionUpload();
}

void snapshotExactRestorationRegression() {
  JsonDocument snapshot;
  snapshot["numeric_encoding"] = "binary64-decimal-v1";
  for (const char *group : {"configuration", "initial_tuning", "final_tuning"}) {
    const std::string exact = std::string(group) + "_exact";
    WaterTestControllerSnapshot::number(snapshot[group].to<JsonObject>(),
                                       snapshot[exact].to<JsonObject>(), "gain", checkpointPrecisionGain);
    snapshot[group]["learning_updates"] = 7;
  }
  snapshot["target_c"] = 19.444444444444443;
  snapshot["target_c_exact"] = "19.444444444444443";
  std::string serialized;
  serializeJson(snapshot, serialized);
  JsonDocument restored;
  assert(deserializeJson(restored, serialized) == DeserializationError::Ok);
  assert(WaterTestControllerSnapshot::restoreExactNumbers(restored.as<JsonObject>()));
  for (const char *group : {"configuration", "initial_tuning", "final_tuning"}) {
    assert(restored[group]["gain"].as<double>() == checkpointPrecisionGain);
    assert(restored[group]["learning_updates"].is<unsigned>());
    assert(restored[group]["learning_updates"] == 7);
    assertExact(restored[std::string(group) + "_exact"]["gain"], checkpointPrecisionGain);
  }
  for (const char *invalid : {"nan", "inf", "1e9999", "1e-9999", "0x1p2", " 1", "1 ", "1junk", "", "+"}) {
    restored["target_c_exact"] = invalid;
    assert(!WaterTestControllerSnapshot::restoreExactNumbers(restored.as<JsonObject>()));
  }
  restored["target_c_exact"] = "19.444444444444443";
  restored["target_c"] = "19.444444444444443";
  assert(!WaterTestControllerSnapshot::restoreExactNumbers(restored.as<JsonObject>()));
  restored["target_c"] = nullptr;
  restored["target_c_exact"] = nullptr;
  restored["final_tuning"] = nullptr;
  restored["final_tuning_exact"] = nullptr;
  assert(WaterTestControllerSnapshot::restoreExactNumbers(restored.as<JsonObject>()));
  restored["numeric_encoding"] = "unknown";
  assert(!WaterTestControllerSnapshot::restoreExactNumbers(restored.as<JsonObject>()));
}
