/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GlycolCoolingController.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>
using GlycolCooling::Algorithm;
using GlycolCooling::Controller;
using GlycolCooling::Phase;

static void equal(double a, double b) { assert(a == b || (std::isnan(a) && std::isnan(b))); }
template<typename Original>
static void sharedEqual(const GlycolCooling::Output& a, const Original& b) {
    assert(static_cast<unsigned>(a.phase) == static_cast<unsigned>(b.phase));
    assert(a.pump_on == b.pump_on && a.full_cooling == b.full_cooling);
    equal(a.temperature_c,b.temperature_c); equal(a.rate_c_per_s,b.rate_c_per_s);
    equal(a.setpoint_c,b.setpoint_c); equal(a.pulse_budget_s,b.pulse_budget_s);
    equal(a.predicted_endpoint_c,b.predicted_endpoint_c); equal(a.actual_on_s,b.actual_on_s);
    assert(a.learning_updates == b.learning_updates);
}
int main() {
    // With no selection changes the wrapper is exactly transparent, including
    // learning, missing ticks, setpoint changes, invalid inputs and duplicate ticks.
    for (Algorithm algorithm : {Algorithm::PredictiveCoast, Algorithm::PulseDose}) {
        Controller selected(algorithm);
        PredictiveCooling::Controller predictive;
        AdaptiveCooling::Controller dose;
        for (unsigned i=0;i<22000;++i) {
            const double t = i + i/1000;
            const double sensor = 21.15 + 0.3*std::sin(i/140.0);
            const double target = i < 11000 ? 21.0 : 21.1;
            const bool connected = i % 4199 != 0;
            for (int repeat=0;repeat<2;++repeat) {
                const auto output = selected.step(t,sensor,target,connected);
                if (algorithm == Algorithm::PredictiveCoast) {
                    const auto reference=predictive.step(t,sensor,target,connected);
                    sharedEqual(output,reference);
                    equal(output.coast_s,reference.coast_s);
                    equal(output.budget_gain_c_per_s,reference.budget_gain_c_per_s);
                    assert(output.response_updates==reference.response_updates);
                } else {
                    const auto reference=dose.step(t,sensor,target,connected);
                    sharedEqual(output,reference); equal(output.gain_c_per_on_s,reference.gain_c_per_on_s);
                }
            }
        }
        if (algorithm == Algorithm::PredictiveCoast) assert(selected.output().response_updates > 0);
        else assert(selected.output().learning_updates > 0);
    }
    // A selection made before the first decision behaves like direct creation.
    {
        Controller c; c.request(Algorithm::PulseDose);
        const auto o=c.step(0,21.1,21);
        assert(o.pump_on && c.selection()==Algorithm::PulseDose && !c.switchPending());
    }
    // Preserve both the outgoing minimum ON and the larger OFF requirement.
    {
        PredictiveCooling::Config p; p.min_on_s=6; p.min_off_s=8;
        AdaptiveCooling::Config a; a.min_on_s=4; a.min_off_s=3;
        Controller c(Algorithm::PredictiveCoast,p,a);
        assert(c.step(0,25,21).pump_on);
        c.request(Algorithm::PulseDose);
        for(int i=1;i<6;++i) {
            assert(c.step(i,25,21).pump_on && c.selection()==Algorithm::PredictiveCoast);
            assert(c.switchPending());
        }
        assert(!c.step(6,25,21).pump_on && c.selection()==Algorithm::PulseDose);
        assert(c.output().learning_updates==0);
        for(int i=7;i<14;++i) assert(!c.step(i,25,21).pump_on);
        assert(c.step(14,25,21).pump_on && !c.switchPending());
        c.request(Algorithm::PredictiveCoast);
        for(int i=15;i<18;++i) assert(c.step(i,25,21).pump_on);
        assert(!c.step(18,25,21).pump_on);
        for(int i=19;i<26;++i) assert(!c.step(i,25,21).pump_on);
        assert(c.step(26,25,21).pump_on);
    }
    // Re-selecting either algorithm during the OFF handoff cannot shorten the
    // outgoing minimum or postpone it through fictitious OFF edges.
    {
        PredictiveCooling::Config p; p.min_off_s=8;
        Controller c(Algorithm::PredictiveCoast,p);
        assert(c.step(0,25,21).pump_on);
        c.request(Algorithm::PulseDose); assert(!c.step(2,25,21).pump_on);
        c.request(Algorithm::PredictiveCoast); assert(!c.step(3,25,21).pump_on);
        c.request(Algorithm::PulseDose); assert(!c.step(4,25,21).pump_on);
        c.inhibit(5); c.inhibit(6); // mode/heat inhibition preserves the real t=2 edge
        for(int t=7;t<10;++t) assert(!c.step(t,25,21).pump_on);
        assert(c.step(10,25,21).pump_on);
        assert(c.selection()==Algorithm::PulseDose && !c.switchPending());
    }
    // Cancel before the OFF edge: no relay edge and no incoming observation.
    {
        Controller c; assert(c.step(0,25,21).pump_on);
        c.request(Algorithm::PulseDose); assert(c.step(1,25,21).pump_on);
        c.request(Algorithm::PredictiveCoast);
        assert(c.step(2,25,21).pump_on && !c.switchPending());
        assert(c.output().response_updates==0);
    }
    // A fault overrides minimum ON, including on a duplicate timestamp. Fault
    // recovery, repeated inhibits, a backwards/NaN clock and switch cancellation
    // cannot erase or move the actual OFF edge backwards.
    {
        Controller c; assert(c.step(10,25,21).pump_on);
        c.request(Algorithm::PulseDose);
        assert(!c.step(10,25,21,false).pump_on);
        assert(c.selection()==Algorithm::PulseDose);
        assert(!c.inhibit(10.1).pump_on);
        assert(!c.step(9,25,21).pump_on);
        assert(!c.step(std::numeric_limits<double>::quiet_NaN(),25,21).pump_on);
        assert(!c.step(11,25,21).pump_on);
        assert(c.step(12,25,21).pump_on);
        c.request(Algorithm::PredictiveCoast);
        assert(c.step(12,25,21).pump_on); // duplicate does not shorten ON
        assert(!c.step(50,25,21).pump_on); // missed calls stop at the real observed edge
        assert(!c.step(51,25,21).pump_on);
        assert(c.step(52,25,21).pump_on);
    }
    // Setpoint changes while a selection is pending cannot truncate minimum ON.
    {
        Controller c; assert(c.step(0,25,21).pump_on);
        c.request(Algorithm::PulseDose);
        assert(c.step(1,25,30).pump_on);
        assert(!c.step(2,25,30).pump_on);
        assert(!c.step(4,25,30).pump_on);
        assert(c.output().learning_updates==0);
    }
    // Learn independently, then discard interrupted coasts without wiping either
    // set of estimates. A mode/heat inhibition has the same abort semantics.
    {
        PredictiveCooling::Config p; p.observe_coast_s=5; p.max_observe_coast_s=10;
        AdaptiveCooling::Config a; a.observe_coast_s=5; a.max_observe_coast_s=10;
        Controller c(Algorithm::PredictiveCoast,p,a);
        for(int i=0;i<=20;++i) c.step(i,21.2-i*.01,21);
        const auto learned_predictive=c.output();
        assert(learned_predictive.response_updates>0);
        c.request(Algorithm::PulseDose); c.step(21,21,21);
        for(int i=22;i<=42;++i) c.step(i,21.2-(i-22)*.01,21);
        const auto learned_dose=c.output(); assert(learned_dose.learning_updates>0);
        c.request(Algorithm::PredictiveCoast); c.step(43,21,21); c.step(44,21,21);
        equal(c.output().coast_s,learned_predictive.coast_s);
        equal(c.output().budget_gain_c_per_s,learned_predictive.budget_gain_c_per_s);
        assert(c.output().response_updates==learned_predictive.response_updates);
        c.inhibit(45); c.inhibit(46);
        c.request(Algorithm::PulseDose); c.step(47,21,21); c.step(48,21,21);
        equal(c.output().gain_c_per_on_s,learned_dose.gain_c_per_on_s);
        assert(c.output().learning_updates==learned_dose.learning_updates);
    }
    for(Algorithm first : {Algorithm::PredictiveCoast,Algorithm::PulseDose}) {
        Controller c(first);
        for(int i=0;i<=5;++i)c.step(i,21.1,21);
        assert(c.output().phase==Phase::Coast);
        c.request(first==Algorithm::PredictiveCoast ? Algorithm::PulseDose : Algorithm::PredictiveCoast);
        c.step(6,21,21);
        for(int i=7;i<2500;++i)c.step(i,21,21);
        c.request(first); c.step(2500,21,21); c.step(2501,21,21);
        assert(c.output().learning_updates==0 && c.output().response_updates==0);
    }
    std::puts("glycol cooling selector: exact core parity and switching checks passed");
}
