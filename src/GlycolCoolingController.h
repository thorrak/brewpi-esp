#pragma once

#include "GlycolCoolingAlgorithm.h"
#include "AdaptiveDoseController.h"
#include "PredictiveCoastController.h"

namespace GlycolCooling {

enum class Phase : uint8_t {
    Idle, Cool, Coast, DisabledOrSensorFault, SetpointChangeWait, AlgorithmSwitchWait
};

struct Output {
    Phase phase;
    bool pump_on;
    bool full_cooling;
    double temperature_c;
    double rate_c_per_s;
    double setpoint_c;
    uint32_t learning_updates;
    double coast_s;
    double budget_gain_c_per_s;
    uint32_t response_updates;
    double gain_c_per_on_s;
    double pulse_budget_s;
    double predicted_endpoint_c;
    double actual_on_s;
};

struct Tuning {
    PredictiveCooling::Tuning predictive;
    AdaptiveCooling::Tuning pulse_dose;
};

// The algorithms retain separate learned estimates. This coordinator owns the
// actual relay edge across algorithm changes; neither core sees fictitious ON
// commands while a handoff or external heat/mode gate blocks the output.
class Controller {
public:
    explicit Controller(Algorithm initial = Algorithm::PredictiveCoast,
                        const PredictiveCooling::Config& predictive = PredictiveCooling::Config(),
                        const AdaptiveCooling::Config& dose = AdaptiveCooling::Config());
    void request(Algorithm algorithm) { requested_ = algorithm; }
    Output step(double time_s, double sensor_c, double setpoint_c, bool connected = true);
    Output inhibit(double time_s);
    // Re-enter normal control after a separately owned experiment. Establish a
    // conservative OFF interval at handoff even though these cores did not
    // observe the experiment's physical pulses.
    void externalOff(double time_s);
    void reset(Algorithm initial = Algorithm::PredictiveCoast);
    Tuning tuning() const;
    bool restoreTuning(const Tuning& tuning);
    const Output& output() const { return output_; }
    Algorithm selection() const { return active_; }
    Algorithm requestedSelection() const { return requested_; }
    bool switchPending() const { return active_ != requested_ || handoff_wait_; }
    const char* algorithmVersion() const;
    double minOnSeconds() const;
    double minOffSeconds() const;
    const PredictiveCooling::Config& predictiveConfiguration() const { return predictive_.configuration(); }
    const AdaptiveCooling::Config& doseConfiguration() const { return dose_.configuration(); }
    static const char* phaseName(Phase phase);
private:
    PredictiveCooling::Controller predictive_;
    AdaptiveCooling::Controller dose_;
    Algorithm active_, requested_;
    Output output_;
    double last_time_s_, last_edge_s_, off_minimum_s_;
    bool handoff_wait_, fresh_;
    void copyActiveOutput();
    void inhibitCores(double time_s);
    double validClock(double time_s) const;
    void trackEdge(bool was_on, double time_s);
};

} // namespace GlycolCooling
