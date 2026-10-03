#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <string>

// HTTP transport only; callers own immutable requests and durable acknowledgements.
namespace WaterTestTransport {
constexpr const char *endpoint = "http://chill.fermentrack.net";
using BodySink = bool (*)(void *context, const char *data, size_t length);
// Validate the immutable source before send(): open() sends request headers.
// write() must reproduce exactly length bytes, stop on sink failure and return
// false on a source error. No complete request body needs to exist in memory.
struct BodySource {
  size_t length;
  void *context;
  bool (*write)(void *context, BodySink sink, void *sinkContext);
};
bool send(const std::string &path, bool post, const BodySource &body, JsonDocument &response, std::string &error);
} // namespace WaterTestTransport
