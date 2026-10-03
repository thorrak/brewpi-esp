// Exercise the production request preparation and HTTP streaming call together.
// HTTP capture belongs only to this desktop facade, never the firmware sender.
std::string readNativeFile(const char *path) {
  std::ifstream input(Native::root + path, std::ios::binary);
  assert(input);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
void writeNativeFile(const char *path, const std::string &bytes) {
  std::ofstream output(Native::root + path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  assert(output);
}
JsonDocument legacyBatch(uint32_t next) {
  JsonDocument request;
  WaterTest::common(request);
  auto list = request["records"].to<JsonArray>();
  FILE *file = fs_open(WaterTest::journalPath, "rb");
  assert(file && fseek(file, next * sizeof(Record), SEEK_SET) == 0);
  const uint32_t end = std::min<uint32_t>(WaterTest::recordCount, next + 12);
  for (uint32_t n = next; n < end; ++n) {
    Record record{};
    assert(fread(&record, sizeof(record), 1, file) == 1 && valid(record));
    if (n == next) request["first_seq"] = record.seq;
    request["last_seq"] = record.seq;
    WaterTestProtocol::recordToJson(list.add<JsonObject>(), record, WaterTest::recordingBoot,
        WaterTest::manifest["sensors"][record.role ? "glycol" : "beer"]["calibration_offset_c"] | 0.0);
  }
  fclose(file);
  request["batch_id"] = WaterTestProtocol::batchIdentifier(WaterTest::manifest["test_id"].as<std::string>(), next);
  request["boot_id"] = WaterTest::recordingBoot;
  return request;
}
void assertLegacyBatch(const std::string &body, uint32_t next) {
  JsonDocument received;
  assert(deserializeJson(received, body) == DeserializationError::Ok);
  const auto legacy = legacyBatch(next);
  std::string legacyBody;
  serializeJson(legacy, legacyBody);
  JsonDocument expected;
  assert(deserializeJson(expected, legacyBody) == DeserializationError::Ok);
  assert(received.as<JsonVariantConst>() == expected.as<JsonVariantConst>());
  assert(received["records"].size() == std::min<uint32_t>(12, WaterTest::recordCount - next));
}
void verifyStreamingUpload(const std::string &scenario) {
  start();
  finishEligibleStopped();
  if (scenario == "upload_bounded_streaming") {
    // Refuse any allocation above the fixed serializer arena, even for stored
    // metadata substantially larger than that arena. Only the native facade
    // assembles request bodies, after observing bounded production writes.
    WaterTest::terminal["native_large_metadata_fixture"] = std::string(12000, 'x');
    assert(WaterTestStorage::saveDocument(WaterTest::finishPath, WaterTest::terminal));
    Native::uploadAllocationLimit = WaterTestUpload::workspaceBytes;
    Native::shortHttpWrite = 17;
    submit();
    assert(!Native::uploadAllocationRequests.empty() && Native::uploadWorkspace == nullptr);
    for (const auto allocation : Native::uploadAllocationRequests)
      assert(allocation == WaterTestUpload::workspaceBytes);
    assert(Native::maxHttpWrite <= 512 && Native::maxHttpRead <= 256);
    assert(Native::httpWrites > Native::payloads.size());
    assert(Native::httpInitializations == Native::httpCleanups);
    for (size_t n = 0; n < Native::payloads.size(); ++n)
      assert(Native::declaredPayloadLengths[n] == Native::payloads[n].size());
    assert(Native::payloads.front().size() > 256);
    assert(Native::payloads.back().size() > 12000);
    assert(Native::payloads.back().size() > Native::uploadAllocationLimit);
    return;
  }
  if (scenario == "upload_metadata_crc") {
    const auto original = readNativeFile(WaterTest::manifestPath);
    auto damaged = original;
    const auto at = damaged.find("Native water test");
    assert(at != std::string::npos);
    damaged[at] = 'X'; // Remains parseable, but its original CRC must fail.
    writeNativeFile(WaterTest::manifestPath, damaged);
    uploadOnce();
    assert(Native::httpInitializations == 0 && Native::payloads.empty());
    assert(!WaterTest::manifestUploaded && WaterTest::uploadedRecords == 0);
    assert(fs_exists(WaterTest::journalPath));
    writeNativeFile(WaterTest::manifestPath, original);
    uploadOnce();
    assert(WaterTest::manifestUploaded);
    while (WaterTest::uploadedRecords < WaterTest::recordCount) uploadOnce();
    const auto finish = readNativeFile(WaterTest::finishPath);
    damaged = finish;
    const auto checksum = damaged.rfind("\"crc32\":");
    assert(checksum != std::string::npos);
    const auto digit = checksum + std::strlen("\"crc32\":");
    damaged[digit] = damaged[digit] == '1' ? '2' : '1';
    writeNativeFile(WaterTest::finishPath, damaged);
    const auto initialized = Native::httpInitializations;
    uploadOnce();
    assert(Native::httpInitializations == initialized);
    assert(WaterTest::uploadState == "error" && fs_exists(WaterTest::journalPath));
    writeNativeFile(WaterTest::finishPath, finish);
    uploadOnce();
    assert(WaterTest::uploadState == "submitted");
    return;
  }
  uploadOnce();
  assert(WaterTest::manifestUploaded && WaterTest::uploadedRecords == 0);
  const auto initializations = Native::httpInitializations;
  if (scenario == "upload_workspace_allocation_failure") {
    Native::failUploadAllocation = true;
    uploadOnce();
    assert(WaterTest::uploadedRecords == 0 && Native::httpInitializations == initializations);
    assert(Native::uploadWorkspace == nullptr && fs_exists(WaterTest::journalPath));
    assert(WaterTest::uploadError.find("32768") != std::string::npos);
    assert(WaterTest::uploadError.find("2048") != std::string::npos);
    Native::failUploadAllocation = false;
    uploadOnce();
    assert(WaterTest::uploadedRecords == 12);
    assertLegacyBatch(Native::payloads.back(), 0);
    return;
  }
  if (scenario == "upload_batch_crc") {
    const auto original = readNativeFile(WaterTest::journalPath);
    assert(original.size() >= 12 * sizeof(Record));
    auto damaged = original;
    damaged[5 * sizeof(Record) + 1] ^= 1;
    writeNativeFile(WaterTest::journalPath, damaged);
    uploadOnce();
    // A corrupt sixth record must not silently send a five-record batch under
    // the identifier reserved for the immutable twelve-record batch.
    assert(Native::httpInitializations == initializations && Native::payloads.size() == 1);
    assert(WaterTest::uploadedRecords == 0 && fs_exists(WaterTest::journalPath));
    writeNativeFile(WaterTest::journalPath, original);
    uploadOnce();
    assert(WaterTest::uploadedRecords == 12);
    assertLegacyBatch(Native::payloads.back(), 0);
    return;
  }
  if (scenario == "upload_partial_write_retry") {
    Native::failWriteAfter = 73;
    uploadOnce();
    assert(Native::partialPayloads.size() == 1 && Native::partialPayloads.back().size() == 73);
    assert(Native::payloads.size() == 1 && WaterTest::uploadedRecords == 0);
    assert(fs_exists(WaterTest::journalPath));
    Native::failWriteAfter = SIZE_MAX;
    Native::shortHttpWrite = 17;
    uploadOnce();
    assert(WaterTest::uploadedRecords == 12 && Native::payloads.size() == 2);
    assert(Native::payloads.back().compare(0, 73, Native::partialPayloads.back()) == 0);
    assertLegacyBatch(Native::payloads.back(), 0);
  } else if (scenario == "upload_lost_ack_retry" || scenario == "upload_incomplete_ack_retry" ||
             scenario == "upload_large_ack_retry") {
    if (scenario == "upload_lost_ack_retry") Native::failReadAfter = 20;
    if (scenario == "upload_incomplete_ack_retry") Native::incompleteHttpResponse = true;
    if (scenario == "upload_large_ack_retry") Native::nextHttpBody = "{\"ignored\":\"" + std::string(8193, 'x') + "\"}";
    uploadOnce();
    assert(WaterTest::uploadedRecords == 0 && Native::payloads.size() == 2);
    assert(WaterTest::uploadState == "error" && fs_exists(WaterTest::journalPath));
    const auto lost = Native::payloads.back();
    Native::failReadAfter = SIZE_MAX;
    Native::incompleteHttpResponse = false;
    Native::nextHttpBody.clear();
    uploadOnce();
    assert(WaterTest::uploadedRecords == 12 && Native::payloads.size() == 3);
    assert(Native::payloads.back() == lost);
    assertLegacyBatch(lost, 0);
  } else {
    assert(false);
  }
  assert(Native::httpInitializations == Native::httpCleanups);
  assert(Native::maxHttpWrite <= 512 && Native::maxHttpRead <= 256);
  assert(Native::uploadWorkspace == nullptr && !WaterTest::uploaderBusy);
}
