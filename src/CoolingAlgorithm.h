#pragma once

#include <cstdint>
#include <cstring>

namespace GlycolCooling {

enum class Algorithm : uint8_t { PredictiveCoast, PulseDose };

inline const char* selectionName(Algorithm algorithm) {
    return algorithm == Algorithm::PulseDose ? "pulse_dose" : "predictive_coast";
}

inline bool parseAlgorithm(const char* value, Algorithm& algorithm) {
    if (value && std::strcmp(value, "predictive_coast") == 0) {
        algorithm = Algorithm::PredictiveCoast;
        return true;
    }
    if (value && std::strcmp(value, "pulse_dose") == 0) {
        algorithm = Algorithm::PulseDose;
        return true;
    }
    return false;
}

} // namespace GlycolCooling
