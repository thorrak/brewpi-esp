#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace GlycolCooling {

// Observation telemetry describes the production policy's own response window.
// It never changes actuation or declares that the physical plant has settled.
enum class ObservationReason : uint8_t { None, RateCondition, CoastTimeLimit, RateUnqualified, Interrupted };
struct Observation {
    uint32_t sequence = 0;
    ObservationReason reason = ObservationReason::None;
    double started_s = 0, off_s = 0, ended_s = 0;
    bool rate_qualified = false;
};

// Shared bounded measurements for the frozen cooling policies. Appending,
// expiry and the four independent rebase sums must retain this order for
// parity with the Python reference. Controller policy and learning stay in
// each algorithm. An overflow is reported to the caller, which fails OFF.
class Measurements {
public:
    void clear() {
        rate_qualified_ = false;
        samples_.clear();
        short_samples_.clear();
        sum_y_ = sum_ty_ = sum_t_ = sum_tt_ = short_sum_ = 0.0;
        time_origin_ = std::numeric_limits<double>::quiet_NaN();
        rate_started_s_ = std::numeric_limits<double>::quiet_NaN();
    }

    bool observe(double time_s, double value_c, double rate_window_s,
                 double measurement_window_s, double& temperature_c, double& rate_c_per_s) {
        if (!std::isfinite(time_origin_)) {
            time_origin_ = time_s;
            rate_started_s_ = time_s;
        }
        const double local_time = time_s - time_origin_;
        if (!samples_.append({time_s, value_c})) return false;
        sum_y_ += value_c;
        sum_ty_ += local_time * value_c;
        sum_t_ += local_time;
        sum_tt_ += local_time * local_time;
        while (samples_.size && samples_.at(0).time_s < time_s - rate_window_s) {
            const Sample old = samples_.pop();
            const double local_t = old.time_s - time_origin_;
            sum_y_ -= old.value_c;
            sum_ty_ -= local_t * old.value_c;
            sum_t_ -= local_t;
            sum_tt_ -= local_t * local_t;
        }
        if (local_time >= 3600.0) {
            time_origin_ = time_s;
            // Separate ordered sums match Python's four generator sums.
            sum_y_ = sum_ty_ = sum_t_ = sum_tt_ = 0.0;
            for (std::size_t i = 0; i < samples_.size; ++i) sum_y_ += samples_.at(i).value_c;
            for (std::size_t i = 0; i < samples_.size; ++i) {
                const Sample& sample = samples_.at(i);
                sum_ty_ += (sample.time_s - time_s) * sample.value_c;
            }
            for (std::size_t i = 0; i < samples_.size; ++i) sum_t_ += samples_.at(i).time_s - time_s;
            for (std::size_t i = 0; i < samples_.size; ++i) {
                const double t = samples_.at(i).time_s - time_s;
                sum_tt_ += t * t;
            }
        }
        if (!short_samples_.append({time_s, value_c})) return false;
        short_sum_ += value_c;
        while (short_samples_.at(0).time_s < time_s - measurement_window_s) {
            short_sum_ -= short_samples_.pop().value_c;
        }
        temperature_c = short_sum_ / static_cast<double>(short_samples_.size);
        const double n = static_cast<double>(samples_.size);
        const double denominator = n * sum_tt_ - sum_t_ * sum_t_;
        // Three cached probe readings can differ by one quantum and imply an
        // enormous rate. Require a full startup window and retained coverage,
        // including after missing ticks. Allow one normal two-second probe
        // refresh at the rolling edge (at most 10% for shorter windows).
        const bool mature = time_s - rate_started_s_ >= rate_window_s;
        const double span = time_s - samples_.at(0).time_s;
        const double tolerance = std::min(0.1 * rate_window_s, 2.0);
        const bool usable = samples_.size >= 3 && mature && span >= rate_window_s - tolerance && denominator > 1e-9;
        rate_c_per_s = usable
            ? (n * sum_ty_ - sum_t_ * sum_y_) / denominator : 0.0;
        // Keep the frozen numerical behavior above. Telemetry additionally
        // requires continuous control-tick coverage, so a substituted zero or
        // a window spanning a missing-tick gap is never evidence of settling.
        rate_qualified_ = usable;
        for (std::size_t i = 1; rate_qualified_ && i < samples_.size; ++i)
            rate_qualified_ = samples_.at(i).time_s - samples_.at(i - 1).time_s <= 2.0 + 1e-9;
        return true;
    }
    bool rateQualified() const { return rate_qualified_; }

private:
    struct Sample { double time_s; double value_c; };
    // Append precedes expiry, exactly as in the Python reference. Spare space
    // accommodates 91 rate samples + one append and 13 short samples + append.
    static constexpr std::size_t kRateCapacity = 128;
    static constexpr std::size_t kShortCapacity = 32;
    template <std::size_t N> struct Samples {
        Sample values[N];
        std::size_t begin = 0;
        std::size_t size = 0;
        void clear() { begin = 0; size = 0; }
        const Sample& at(std::size_t i) const { return values[(begin + i) % N]; }
        bool append(Sample sample) {
            if (size == N) return false;
            values[(begin + size) % N] = sample;
            ++size;
            return true;
        }
        Sample pop() {
            Sample sample = values[begin];
            begin = (begin + 1) % N;
            --size;
            return sample;
        }
    };

    Samples<kRateCapacity> samples_;
    Samples<kShortCapacity> short_samples_;
    double sum_y_ = 0, sum_ty_ = 0, sum_t_ = 0, sum_tt_ = 0, short_sum_ = 0;
    double time_origin_ = std::numeric_limits<double>::quiet_NaN();
    double rate_started_s_ = std::numeric_limits<double>::quiet_NaN();
    bool rate_qualified_ = false;
};

} // namespace GlycolCooling
