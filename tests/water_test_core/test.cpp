#include "WaterTestCore.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace WaterTestCore;

static void feed(Program &p, double until, double c = 20) {
  double now = p.lastSample;
  while (now < until && p.active()) {
    now = std::min(until, now + 1);
    p.sample(now, c, true, now);
    p.tick(now);
  }
}
static void startPulse(Program &p, uint32_t minimumOn = 2) {
  assert(p.start(0, 20, minimumOn, 2));
  feed(p, 300);
  assert(p.pump && p.pulse == 1 && p.completedPulses == 0);
}
static void sensorTolerance() {
  static_assert(freshnessSeconds == 30);
  static_assert(OneWireSensorPolicy::connectedTimeoutUs == 30000000);
  Program p;
  startPulse(p);
  p.sample(300.1, 4, false, 300.1);
  p.sample(301, NAN, true, 301);
  p.sample(302, INFINITY, true, 302);
  assert(p.active() && p.pump);
  assert(p.lastSample == 300 && p.latestC == 20 && p.minimumC == 20);
  p.sample(305, 19.8, true, 305);
  assert(p.lastSample == 305 && p.latestC == 19.8 && p.minimumC == 19.8);
  p.tick(310);
  assert(!p.pump && p.phase == Phase::Observe && p.completedPulses == 1);
  for (double now : {316., 320., 335.}) {
    p.sample(now, 0, false, now);
    p.tick(now);
    assert(p.active() && p.lastSample == 305 && p.latestC == 19.8);
  }
  p.sample(335.001, NAN, false, 335.001);
  assert(!p.active() && p.reason == Reason::SensorStale && p.outcome == End::Failed);
  assert(!p.submissionEligible());

  startPulse(p);
  for (unsigned now = 301; now <= 330; ++now) {
    p.sample(now, NAN, false, now);
    p.tick(now);
    assert(p.active() && p.lastSample == 300 && p.latestC == 20);
  }
  assert(!p.pump && p.completedPulses == 1);
  p.tick(330.001);
  assert(p.reason == Reason::SensorStale && !p.submissionEligible());

  startPulse(p);
  p.tick(310);
  p.tick(330);
  assert(p.active());
  p.tick(330.001);
  assert(!p.active() && p.reason == Reason::SensorStale);

  startPulse(p);
  p.sample(315, NAN, false, 315);
  p.tick(315);
  p.sample(329, 19.9, true, 329);
  assert(p.active() && p.lastSample == 329 && p.latestC == 19.9);
  p.tick(359);
  assert(p.active());
  p.tick(359.001);
  assert(!p.active() && p.reason == Reason::SensorStale);
}
static void submissionRules() {
  Program p;
  assert(!p.submissionEligible());
  assert(p.start(0, 20, 2, 2));
  p.stop(2);
  assert(p.outcome == End::Stopped && p.pulse == 0 && !p.submissionEligible());

  startPulse(p);
  p.stop(300.1);
  assert(p.active() && p.pump);
  p.tick(301.9);
  assert(p.pump && !p.submissionEligible());
  p.tick(302);
  assert(!p.pump && p.outcome == End::Stopped && p.totalPump == 2);
  assert(p.completedPulses == 0 && !p.submissionEligible());

  startPulse(p);
  p.stop(309.999);
  assert(p.outcome == End::Stopped && p.completedPulses == 0 && !p.submissionEligible());

  startPulse(p);
  p.stop(310);
  assert(p.outcome == End::Stopped && p.completedPulses == 1 && p.totalPump == 10);
  assert(p.submissionEligible());
  p.finish(311, End::Stopped, Reason::UserStop);
  assert(p.completedPulses == 1 && p.totalPump == 10);

  startPulse(p);
  feed(p, 310);
  assert(p.phase == Phase::Observe && p.completedPulses == 1 && !p.submissionEligible());
  p.stop(310.1);
  assert(p.outcome == End::Stopped && p.submissionEligible());

  startPulse(p);
  feed(p, 310);
  p.stop(340);
  assert(p.outcome == End::Stopped && p.completedPulses == 1 && p.submissionEligible());

  startPulse(p);
  feed(p, 310);
  p.stop(340.001);
  assert(p.outcome == End::Failed && p.reason == Reason::SensorStale);
  assert(p.completedPulses == 1 && !p.submissionEligible());

  startPulse(p);
  feed(p, 310);
  p.sample(maximumSeconds, 20, true, maximumSeconds);
  p.stop(maximumSeconds);
  assert(p.outcome == End::Inconclusive && p.reason == Reason::RuntimeLimit);
  assert(p.completedPulses == 1 && !p.submissionEligible());

  startPulse(p);
  feed(p, 310);
  p.stop(maximumSeconds);
  assert(p.outcome == End::Failed && p.reason == Reason::SensorStale);
  assert(!p.submissionEligible());

  startPulse(p, 30);
  assert(p.deadline == 330);
  p.stop(302);
  feed(p, 329);
  assert(p.pump && p.completedPulses == 0);
  feed(p, 330);
  assert(!p.pump && p.totalPump == 30 && p.completedPulses == 1 && p.submissionEligible());

  startPulse(p);
  feed(p, 1510);
  assert(p.pump && p.pulse == 2 && p.completedPulses == 1);
  p.stop(1510.1);
  feed(p, 1512);
  assert(p.outcome == End::Stopped && p.completedPulses == 1 && p.totalPump == 12);
  assert(p.submissionEligible());

  for (Reason reason : {Reason::SensorStale, Reason::StorageFailure, Reason::QueueOverflow, Reason::UnexpectedOutput}) {
    startPulse(p);
    feed(p, 310);
    p.finish(311, End::Failed, reason);
    assert(p.completedPulses == 1 && !p.submissionEligible());
  }
  for (Reason reason : {Reason::RuntimeLimit, Reason::NoResponse, Reason::PumpBudget}) {
    startPulse(p);
    feed(p, 310);
    p.finish(311, End::Inconclusive, reason);
    assert(p.completedPulses == 1 && !p.submissionEligible());
  }
  startPulse(p);
  p.sample(301, 16.9, true, 301);
  assert(!p.pump && p.reason == Reason::TemperatureLimit && !p.submissionEligible());
  startPulse(p);
  p.sample(310, 16.9, true, 310);
  assert(p.outcome == End::Stopped && p.completedPulses == 1 && p.submissionEligible());
}
static void schedulerAndLimits() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  feed(p, 299);
  assert(!p.pump && p.pulse == 0);
  feed(p, 300);
  assert(p.pump && p.pulse == 1 && p.deadline == 310);
  feed(p, 310);
  assert(!p.pump && p.phase == Phase::Observe && p.deadline == 1510);
  feed(p, 1510);
  assert(p.pump && p.pulse == 2 && p.deadline == 1540);
  feed(p, 1540);
  feed(p, 2740);
  assert(p.pump && p.pulse == 3 && p.deadline == 2800);
  feed(p, 4000);
  assert(!p.active() && p.outcome == End::Inconclusive && p.reason == Reason::NoResponse && p.totalPump == 100);
  assert(p.completedPulses == 3 && !p.submissionEligible());

  assert(p.start(0, 20, 2, 2));
  feed(p, 310);
  feed(p, 1510, 19.4);
  assert(p.pump && p.deadline == 1515);
  feed(p, 1515, 19.4);
  feed(p, 2715, 19.2);
  assert(p.deadline == 2755);
  feed(p, 3955, 19.2);
  assert(p.outcome == End::Completed && p.completedPulses == 3 && p.submissionEligible());
  assert(!p.start(0, 20, 61, 2));
  assert(!p.start(0, 20, 2, 1201));
  assert(p.start(0, 8, 2, 2));
  feed(p, 300);
  p.sample(301, 4, true, 301);
  assert(!p.pump && p.reason == Reason::TemperatureLimit);
  assert(p.start(0, 20, 2, 2));
  p.lastSample = 5400;
  p.tick(5400);
  assert(p.reason == Reason::RuntimeLimit && !p.submissionEligible());

  startPulse(p);
  p.finish(300.01, End::Failed, Reason::StorageFailure);
  assert(!p.pump && p.reason == Reason::StorageFailure);
  startPulse(p);
  p.finish(300.01, End::Failed, Reason::QueueOverflow);
  assert(!p.pump && p.reason == Reason::QueueOverflow);
}
static void recordIntegrity() {
  Record r{};
  r.seq = 17;
  r.t_us = 1234;
  r.kind = 2;
  r.raw = 320;
  seal(r);
  assert(valid(r));
  r.raw++;
  assert(!valid(r));
  r.raw--;
  assert(valid(r));
  for (size_t byte = 0; byte < sizeof(r); ++byte)
    for (unsigned bit = 0; bit < 8; ++bit) {
      Record bad = r;
      reinterpret_cast<unsigned char *>(&bad)[byte] ^= 1 << bit;
      assert(!valid(bad));
    }
  Record journal[3] = {r, r, r};
  journal[1].seq = 99;
  unsigned prefix = 0;
  for (auto item : journal) {
    if (!valid(item))
      break;
    ++prefix;
  }
  assert(prefix == 1);
}
int main() {
  sensorTolerance();
  submissionRules();
  schedulerAndLimits();
  recordIntegrity();
  std::puts("water_test_core: scheduler, sensor tolerance, submission, limits and journal tests passed");
}
