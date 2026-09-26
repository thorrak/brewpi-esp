#pragma once

#include "GlycolCoolingController.h"

class GlycolTuningStore {
public:
    bool load(GlycolCooling::Controller& controller, uint32_t now_ms);
    // Call after applying outputs, while both heating and cooling are OFF.
    // Returns false if changed tuning is waiting to be saved or a write fails.
    bool saveIfChanged(const GlycolCooling::Controller& controller, uint32_t now_ms);

    static constexpr auto filename = "/glycolTuning.json";

private:
    GlycolCooling::Tuning saved_ = {
        {PredictiveCooling::Config().initial_coast_s, PredictiveCooling::Config().startup_budget_c_per_s, 0, 0},
        {AdaptiveCooling::Config().initial_gain_c_per_on_s, 0}
    };
    uint32_t saved_at_ms_ = 0;
    uint32_t failed_at_ms_ = 0;
    bool save_delay_active_ = false;
    bool retry_pending_ = false;
};
