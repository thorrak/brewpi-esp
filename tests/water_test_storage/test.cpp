using namespace WaterTestStorage;

void writeFile(const char *path, const std::string &bytes) {
  FILE *file = fs_open(path, "wb");
  assert(file);
  assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  assert(fclose(file) == 0);
}
std::string fileBytes(const char *path) {
  FILE *file = fs_open(path, "rb");
  assert(file);
  std::string bytes;
  char buffer[512];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), file)))
    bytes.append(buffer, n);
  fclose(file);
  return bytes;
}
void bounded(bool enabled) {
  Native::maximumNew = enabled ? 1024 : std::numeric_limits<size_t>::max();
  Native::largestNew = 0;
}
void rejected(const std::string &bytes) {
  writeFile("/damaged.json", bytes);
  size_t length = 0;
  JsonDocument doc;
  assert(!readDocumentPayload("/damaged.json", nullptr, 0, length));
  assert(!loadDocument("/damaged.json", doc));
  assert(doc.isNull());
}

int main(int argc, char **argv) {
  assert(argc == 2 && strlen(argv[1]) < sizeof(Native::root));
  strcpy(Native::root, argv[1]);
  JsonDocument document;
  document["test_id"] = "fc73f39a-2222-4000-8888-000000000077";
  document["escaped"] = "Quotes \" \\ newline\n tab\t control\001 UTF-8 café 🌡";
  for (unsigned i = 0; i < 260; ++i) {
    char key[40];
    snprintf(key, sizeof(key), "configuration_field_%u", i);
    document["configuration"][key] = 12345.6789;
  }
  std::string expected;
  serializeJson(document, expected);
  assert(expected.size() > 7681 && expected.size() < 16000);
  auto buffer = static_cast<char *>(std::malloc(expected.size()));
  assert(buffer);
  bounded(true);
  assert(saveDocument(manifestPath, document));
  const size_t saveLargest = Native::largestNew;
  size_t length = 0;
  assert(readDocumentPayload(manifestPath, nullptr, 0, length) && length == expected.size());
  assert(readDocumentPayload(manifestPath, buffer, expected.size(), length) && length == expected.size());
  assert(memcmp(buffer, expected.data(), length) == 0);
  assert(!readDocumentPayload(manifestPath, buffer, expected.size() - 1, length));
  JsonDocument recovered;
  assert(loadDocument(manifestPath, recovered));
  assert(recovered["test_id"] == document["test_id"]);
  assert(recovered["escaped"] == document["escaped"]);
  assert(recovered["configuration"].size() == 260);
  const size_t readLargest = Native::largestNew;
  bounded(false);
  std::free(buffer);
  const std::string original = fileBytes(manifestPath);
  // New streamed envelopes must remain readable by the previous JSON loader.
  JsonDocument envelope;
  assert(deserializeJson(envelope, original) == DeserializationError::Ok);
  assert(envelope["payload"].as<std::string>() == expected);
  assert(envelope["crc32"].as<uint32_t>() == WaterTestCore::checksum(expected.data(), expected.size()));

  // Previous writer's envelope, including escaped Unicode and control data.
  std::string legacy;
  serializeJson(envelope, legacy);
  writeFile("/legacy.json", legacy);
  bounded(true);
  assert(loadDocument("/legacy.json", recovered));
  assert(recovered["escaped"] == document["escaped"]);
  bounded(false);
  std::string originalPayload;
  assert(loadDocumentPayload("/legacy.json", originalPayload) && originalPayload == expected);

  // A torn replacement or failed durable write leaves the last complete file.
  writeFile("/water-test-manifest.json.tmp", "{\"payload\":\"torn");
  assert(loadDocument(manifestPath, recovered));
  Native::failSync = true;
  document["test_id"] = "replacement";
  assert(!saveDocument(manifestPath, document));
  Native::failSync = false;
  assert(fileBytes(manifestPath) == original && !fs_exists("/water-test-manifest.json.tmp"));

  auto corrupt = legacy;
  corrupt[corrupt.find("fc73")] = 'a';
  rejected(corrupt);
  rejected(legacy.substr(0, legacy.size() / 2));
  rejected(legacy.substr(0, legacy.size() - 1));
  rejected(legacy + "garbage");
  rejected("{\"payload\":\"bad\\q\",\"crc32\":0}");
  rejected("{\"payload\":\"bad\\ud800\",\"crc32\":0}");
  rejected("{\"payload\":\"bad\\udc00\",\"crc32\":0}");
  rejected("{\"payload\":\"{}\",\"crc32\":4294967296}");
  rejected("{\"payload\":\"{}\",\"crc32\":-1}");
  rejected(std::string(maximumDocumentBytes + 1, ' '));

  JsonDocument escapedEnvelope;
  const std::string unicodePayload = "{\"value\":\"café 🌡\"}";
  std::string unicode = "{\"payload\":\"{\\\"value\\\":\\\"caf\\u00e9 \\ud83c\\udf21\\\"}\",\"crc32\":";
  unicode += std::to_string(WaterTestCore::checksum(unicodePayload.data(), unicodePayload.size())) + "}";
  writeFile("/unicode.json", unicode);
  assert(loadDocumentPayload("/unicode.json", originalPayload) && originalPayload == unicodePayload);
  assert(loadDocument("/unicode.json", recovered) && recovered["value"] == "café 🌡");

  // The same byte bound governs saving and loading; oversized replacements
  // cannot silently create a document that recovery will later refuse.
  JsonDocument large;
  large["data"] = std::string(maximumDocumentBytes, 'x');
  bounded(true);
  assert(!saveDocument(manifestPath, large));
  bounded(false);
  assert(fileBytes(manifestPath) == original);
  printf("water_test_storage: %zu-byte payload saved/recovered with largest C++ allocations save=%zu read=%zu; legacy, CRC, Unicode, truncation, atomic failure and bounds passed\n",
         expected.size(), saveLargest, readLargest);
}
