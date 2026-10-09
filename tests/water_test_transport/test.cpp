using namespace WaterTestTransport;
using namespace NativeTransport;

// Production accepts only streaming sources. Small test requests use the same
// interface without adding a whole-body convenience API to the firmware.
const BodySource emptyObject{2, nullptr, [](void *, BodySink sink, void *context) {
  return sink(context, "{}", 2);
}};
int main() {
  JsonDocument response;
  std::string error;
  std::string payload(18000, 'x');
  auto singleBytes = [](void *context, BodySink sink, void *sinkContext) {
    const auto &bytes = *static_cast<std::string *>(context);
    for (size_t n = 0; n < bytes.size(); ++n)
      if (!sink(sinkContext, bytes.data() + n, 1)) return false;
    return true;
  };
  const BodySource source{payload.size(), &payload, singleBytes};
  reset();
  assert(send("/test", true, source, response, error));
  assert(request == payload && writtenLength == int(payload.size()));
  assert(largestWrite <= 512 && cleanupCalls == 1 && fetchCalls == 1);
  assert(response["accepted_ranges"][0][1] == 12);

  reset();
  assert(send("/test", false, emptyObject, response, error));
  assert(request == "{}" && cleanupCalls == 1);
  reset();
  assert(!send("/test", false, BodySource{1, &payload, singleBytes}, response, error));
  assert(fetchCalls == 0 && cleanupCalls == 1 && response.isNull());
  reset();
  assert(!send("/test", false, BodySource{payload.size() + 1, &payload, singleBytes}, response, error));
  assert(fetchCalls == 0 && cleanupCalls == 1);
  reset();
  writeAfter = 600;
  assert(!send("/test", false, source, response, error));
  assert(request.size() < payload.size() && fetchCalls == 0 && cleanupCalls == 1);
  reset();
  maxWrite = 0;
  assert(!send("/test", false, source, response, error));
  assert(cleanupCalls == 1 && fetchCalls == 0);

  reset(); connected = false;
  assert(!send("/test", false, source, response, error) && initCalls == 0);
  reset(); allocationFailure = true;
  assert(!send("/test", false, source, response, error) && cleanupCalls == 0);
  reset(); headerResult = ESP_FAIL;
  assert(!send("/test", false, source, response, error) && cleanupCalls == 1 && request.empty());
  reset(); openResult = ESP_FAIL; connectionError = 1234; socketError = 113; code = 0;
  assert(!send("/test", false, source, response, error));
  assert(cleanupCalls == 1 && request.empty());
  assert(error.find("ESP_ERR_CONNECTION_FAILED") != std::string::npos && error.find("socket error 113") != std::string::npos);

  reset(); advance = 1000000;
  assert(!send("/test", false, source, response, error));
  assert(error.find("ESP_ERR_TIMEOUT") != std::string::npos && now <= 30000000 && cleanupCalls == 1);
  reset(); readResult = -ESP_ERR_HTTP_EAGAIN;
  assert(!send("/test", false, emptyObject, response, error));
  assert(error.find("ESP_ERR_HTTP_EAGAIN") != std::string::npos && cleanupCalls == 1);
  reset(); advance = 1000000;
  assert(!send("/test", false, emptyObject, response, error));
  assert(error.find("ESP_ERR_TIMEOUT") != std::string::npos && cleanupCalls == 1);

  reset();
  reply.insert(reply.size() - 1, ",\"ignored\":\"" + std::string(6000, 'a') + "\"");
  unknownLength = true;
  assert(send("/test", false, emptyObject, response, error));
  assert(response["ignored"].isNull() && response["status"] == "stored" && cleanupCalls == 1);
  for (bool chunked : {false, true}) {
    reset(); reply.append(8192, ' '); unknownLength = chunked;
    assert(!send("/test", false, emptyObject, response, error));
    assert(error.find("exceeded 8192 bytes") != std::string::npos && cleanupCalls == 1);
  }
  reset(); reply += "{}";
  assert(!send("/test", false, emptyObject, response, error));
  assert(error == "Invalid server acknowledgement.");
  reset(); truncated = true;
  assert(!send("/test", false, emptyObject, response, error) && cleanupCalls == 1);
  reset("{\"test_id\":\"" + std::string(3000, 'x') + "\"}");
  assert(!send("/test", false, emptyObject, response, error));
  assert(error == "Invalid server acknowledgement." && response.isNull());
  reset("{not JSON");
  assert(!send("/test", false, emptyObject, response, error));

  reset("{\"error\":\"Each controller max_duration_s must be 7200.\\nTry again\"}"); code = 400;
  assert(!send("/test", false, emptyObject, response, error));
  assert(error.find("status 400") != std::string::npos && error.find("7200. Try again") != std::string::npos);
  reset("{\"error\":\"" + std::string(600, 'x') + "\"}"); code = 400;
  assert(!send("/test", false, emptyObject, response, error));
  assert(error.find(std::string(256, 'x') + "...") != std::string::npos);
  reset("{\"detail\":\"" + std::string(255, 'a') + "🌡" + std::string(200, 'b') + "\"}"); code = 400;
  assert(!send("/test", false, emptyObject, response, error));
  assert(error.find(std::string(255, 'a') + "...") != std::string::npos && error.find("🌡") == std::string::npos);
  reset("<html>do not show</html>"); code = 302;
  assert(!send("/test", false, emptyObject, response, error));
  assert(error.find("without redirect") != std::string::npos && error.find("<html>") == std::string::npos);
  puts("water_test_transport: bounded streaming, partial writes, exact lengths, filtered acknowledgements, corruption, disconnects, timeouts and cleanup passed");
}
