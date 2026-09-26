#pragma once

#include "GlycolCoolingController.h"

class GlycolTuningStore {
public:
    bool load(GlycolCooling::Controller& controller);
    // Call after applying outputs, while both heating and cooling are OFF.
    bool saveIfChanged(const GlycolCooling::Controller& controller, uint32_t now_ms);

    static constexpr auto filename = "/glycolTuning.json";

private:
    GlycolCooling::Tuning saved_ = {
        {PredictiveCooling::Config().initial_coast_s, PredictiveCooling::Config().startup_budget_c_per_s, 0, 0},
        {AdaptiveCooling::Config().initial_gain_c_per_on_s, 0}
    };
    uint32_t failed_at_ms_ = 0;
    bool retry_pending_ = false;
};
