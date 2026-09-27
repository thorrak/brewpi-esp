#pragma once
#include <ArduinoJson.h>
#include <string>
#include <string_view>

// HTTP transport only; callers own immutable requests and durable acknowledgements.
namespace WaterTestTransport {
constexpr const char *endpoint = "http://chill.fermentrack.net";
bool send(const std::string &path, bool post, std::string_view body, JsonDocument &response, std::string &error);
} // namespace WaterTestTransport
