#pragma once
#include <ArduinoJson.h>
#include <string>

// HTTP transport only; callers own immutable requests and durable acknowledgements.
namespace WaterTestTransport {
constexpr const char *endpoint = "http://chill.fermentrack.net";
bool send(const std::string &path, bool post, const std::string &body, JsonDocument &response, std::string &error);
} // namespace WaterTestTransport
