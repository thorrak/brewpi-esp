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
int main() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  feed(p, 299);
  assert(!p.pump && p.pulse == 0);
  feed(p, 300);
  assert(p.pump && p.pulse == 1 && p.deadline == 310);
  p.stop(300.1);
  assert(p.active() && p.pump);
  p.tick(301.9);
  assert(p.pump);
  p.tick(302);
  assert(!p.pump && p.outcome == End::Stopped && p.totalPump == 2);
  assert(p.start(0, 20, 2, 2));
  feed(p, 300);
  p.sample(300.1, 20, false, 300.1);
  assert(!p.pump && p.outcome == End::Failed && p.reason == Reason::SensorFault);
  assert(p.start(0, 20, 2, 2));
  feed(p, 300);
  p.tick(310.1);
  assert(!p.pump && p.reason == Reason::SensorStale);
  assert(p.start(0, 20, 2, 2));
  feed(p, 310);
  assert(!p.pump && p.phase == Phase::Observe && p.deadline == 1510);
  feed(p, 1510);
  assert(p.pump && p.pulse == 2 && p.deadline == 1540); // no response: bounded 30-second pulse
  feed(p, 1540);
  feed(p, 2740);
  assert(p.pump && p.pulse == 3 && p.deadline == 2800);
  feed(p, 4000);
  assert(!p.active() && p.outcome == End::Inconclusive && p.reason == Reason::NoResponse && p.totalPump == 100);
  assert(p.start(0, 20, 2, 2));
  feed(p, 310);
  feed(p, 1510, 19.4);
  assert(p.pump && p.deadline == 1515); // strong response shrinks next pulse to5sec
  feed(p, 1515, 19.4);
  feed(p, 2715, 19.2);
  assert(p.deadline == 2755); // medium response40sec
  feed(p, 3955, 19.2);
  assert(p.outcome == End::Completed);
  assert(p.start(0, 20, 30, 2));
  feed(p, 300);
  assert(p.deadline == 330);
  p.stop(302);
  feed(p, 329);
  assert(p.pump);
  feed(p, 330);
  assert(!p.pump && p.totalPump == 30);
  assert(!p.start(0, 20, 61, 2));
  assert(!p.start(0, 20, 2, 1201));
  assert(p.start(0, 20, 2, 2));
  feed(p, 300);
  p.sample(301, 16.9, true, 301);
  assert(!p.pump && p.reason == Reason::TemperatureLimit);
  assert(p.start(0, 8, 2, 2));
  feed(p, 300);
  p.sample(301, 4, true, 301);
  assert(!p.pump && p.reason == Reason::TemperatureLimit);
  assert(p.start(0, 20, 2, 2));
  p.lastSample = 5400;
  p.tick(5400);
  assert(p.reason == Reason::RuntimeLimit);
  assert(p.start(0, 20, 2, 2));
  feed(p, 300);
  p.finish(300.01, End::Failed, Reason::StorageFailure);
  assert(!p.pump && p.reason == Reason::StorageFailure);
  assert(p.start(0, 20, 2, 2));
  feed(p, 300);
  p.finish(300.01, End::Failed, Reason::QueueOverflow);
  assert(!p.pump && p.reason == Reason::QueueOverflow);
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
  // Verify any one-bit flash corruption is detected, includingCRC itself.
  for (size_t byte = 0; byte < sizeof(r); ++byte)
    for (unsigned bit = 0; bit < 8; ++bit) {
      Record bad = r;
      reinterpret_cast<unsigned char *>(&bad)[byte] ^= 1 << bit;
      assert(!valid(bad));
    }
  // Recovery uses only a valid prefix; no corrupted later frame is accepted.
  Record journal[3] = {r, r, r};
  journal[1].seq = 99;
  unsigned prefix = 0;
  for (auto item : journal) {
    if (!valid(item))
      break;
    ++prefix;
  }
  assert(prefix == 1);
  std::puts("water_test_core: all scheduler, interlock, fault, bounds and journal tests passed");
}
