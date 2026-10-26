#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ProcInfo {
  std::string name;
  uint64_t memMB = 0;
};

struct Snapshot {
  bool valid = false;

  float cpuPct = 0.0f;
  unsigned cores = 0;

  float ramPct = 0.0f;
  uint64_t ramUsedMB = 0;
  uint64_t ramTotalMB = 0;

  double diskMBs = 0.0;
  double netMbps = 0.0;

  std::vector<ProcInfo> top;

  int hour = 0;
  int minute = 0;
  std::wstring date;
};

// Samples Windows performance counters. Everything here is local-only:
// no network calls, no telemetry upload.
class Telemetry {
 public:
  Telemetry();
  ~Telemetry();

  bool init();
  void sample();

  // Process enumeration is the expensive part (OpenProcess on every PID). The
  // notch is collapsed most of the time, so it only refreshes the process list
  // while the panel is actually open.
  void setHeavyMode(bool heavy) { heavy_ = heavy; }
  const Snapshot& snap() const { return snap_; }

  // Rounded key used to detect "nothing changed" so the UI can skip drawing.
  uint64_t signature() const;

 private:
  void fillClock();
  Snapshot snap_;
  bool heavy_ = false;
  struct Impl;
  Impl* impl_ = nullptr;
};
