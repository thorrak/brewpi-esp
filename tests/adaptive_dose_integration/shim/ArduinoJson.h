#pragma once
// Persistence is deliberately disabled in the offline adapter. These methods
// only allow the original GlycolParams persistence code to compile unchanged.
class JsonValue {
public:
    template<class T> bool is() const { return false; }
    template<class T> operator T() const { return T{}; }
    template<class T> JsonValue& operator=(const T&) { return *this; }
};
class JsonDocument {
public:
    JsonValue operator[](const char*) const { return {}; }
};
