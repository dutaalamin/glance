#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include "telemetry.h"

// winsock2 + ws2ipdef must precede windows.h: MIB_IF_ROW2 (GetIfTable2) is only
// declared when _WS2IPDEF_ is defined.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <cmath>

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace {

std::string toUtf8(const wchar_t* w) {
  if (!w) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
  return out;
}

uint64_t fileTimeToU64(const FILETIME& ft) {
  return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

struct Telemetry::Impl {
  PDH_HQUERY diskQuery = nullptr;
  PDH_HCOUNTER diskCounter = nullptr;
  bool diskReady = false;

  FILETIME prevIdle{};
  FILETIME prevKernel{};
  FILETIME prevUser{};
  bool cpuPrimed = false;

  uint64_t prevNetBytes = 0;
  uint64_t prevNetTick = 0;
};

Telemetry::Telemetry() : impl_(new Impl()) {}

Telemetry::~Telemetry() {
  if (impl_) {
    if (impl_->diskQuery) PdhCloseQuery(impl_->diskQuery);
    delete impl_;
  }
}

bool Telemetry::init() {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  snap_.cores = si.dwNumberOfProcessors;

  // --- CPU baseline ---
  FILETIME idle{}, kernel{}, user{};
  if (GetSystemTimes(&idle, &kernel, &user)) {
    impl_->prevIdle = idle;
    impl_->prevKernel = kernel;
    impl_->prevUser = user;
    impl_->cpuPrimed = true;
  }

  // --- Disk throughput via PDH ---
  if (PdhOpenQueryW(nullptr, 0, &impl_->diskQuery) == ERROR_SUCCESS) {
    if (PdhAddEnglishCounterW(impl_->diskQuery, L"\\PhysicalDisk(_Total)\\Disk Bytes/sec", 0,
                              &impl_->diskCounter) == ERROR_SUCCESS) {
      PdhCollectQueryData(impl_->diskQuery);
      impl_->diskReady = true;
    }
  }

  // --- Network baseline ---
  impl_->prevNetTick = GetTickCount64();

  return true;
}

// Clock and date are two GetLocalTime reads plus one swprintf: cheap enough to
// run on both paths. Leaving `date` unset on the collapsed path was a bug - the
// idle strip renders it, and it came out blank whenever heavy mode was off
// (which is always the case for `--snapshot`).
void Telemetry::fillClock() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  snap_.hour = st.wHour;
  snap_.minute = st.wMinute;

  static const wchar_t* kDays[] = {L"Sunday", L"Monday",   L"Tuesday", L"Wednesday",
                                   L"Thursday", L"Friday", L"Saturday"};
  static const wchar_t* kMonths[] = {L"January", L"February", L"March",     L"April",
                                     L"May",     L"June",     L"July",      L"August",
                                     L"September", L"October", L"November", L"December"};

  wchar_t buf[64]{};
  swprintf(buf, 64, L"%ls, %d %ls %d", kDays[st.wDayOfWeek % 7], st.wDay,
           kMonths[(st.wMonth - 1) % 12], st.wYear);
  snap_.date = buf;
}

void Telemetry::sample() {
  // CPU: delta of idle vs total. kernel time already includes idle time.
  FILETIME idle{}, kernel{}, user{};
  if (impl_->cpuPrimed && GetSystemTimes(&idle, &kernel, &user)) {
    const uint64_t i = fileTimeToU64(idle);
    const uint64_t k = fileTimeToU64(kernel);
    const uint64_t u = fileTimeToU64(user);

    const uint64_t dIdle = i - fileTimeToU64(impl_->prevIdle);
    const uint64_t dTotal = (k - fileTimeToU64(impl_->prevKernel)) +
                            (u - fileTimeToU64(impl_->prevUser));

    if (dTotal > 0) {
      double pct = 100.0 * (1.0 - static_cast<double>(dIdle) / static_cast<double>(dTotal));
      pct = std::clamp(pct, 0.0, 100.0);
      snap_.cpuPct = static_cast<float>(pct);
    }

    impl_->prevIdle = idle;
    impl_->prevKernel = kernel;
    impl_->prevUser = user;
  }

  // RAM
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    snap_.ramPct = static_cast<float>(ms.dwMemoryLoad);
    snap_.ramTotalMB = ms.ullTotalPhys / (1024ull * 1024ull);
    snap_.ramUsedMB = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024ull * 1024ull);
  }

  // Disk
  if (impl_->diskReady && PdhCollectQueryData(impl_->diskQuery) == ERROR_SUCCESS) {
    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(impl_->diskCounter, PDH_FMT_DOUBLE, nullptr, &value) ==
            ERROR_SUCCESS &&
        value.CStatus == PDH_CSTATUS_VALID_DATA) {
      snap_.diskMBs = std::max(0.0, value.doubleValue / (1024.0 * 1024.0));
    }
  }

  // Network
  PMIB_IF_TABLE2 table = nullptr;
  if (GetIfTable2(&table) == NO_ERROR && table) {
    uint64_t total = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
      const MIB_IF_ROW2& row = table->Table[i];
      if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      if (row.OperStatus != IfOperStatusUp) continue;
      total += row.InOctets + row.OutOctets;
    }
    FreeMibTable(table);

    const uint64_t now = GetTickCount64();
    if (impl_->prevNetTick && now > impl_->prevNetTick && total >= impl_->prevNetBytes) {
      const double seconds = static_cast<double>(now - impl_->prevNetTick) / 1000.0;
      const double bits = static_cast<double>(total - impl_->prevNetBytes) * 8.0;
      snap_.netMbps = bits / seconds / 1'000'000.0;
    }
    impl_->prevNetBytes = total;
    impl_->prevNetTick = now;
  }

  // Top processes by working set. Skipped entirely while collapsed: this is
  // the only expensive call in the sampler.
  if (!heavy_) {
    fillClock();
    snap_.valid = true;
    return;
  }

  DWORD pids[1024]{};
  DWORD needed = 0;
  std::vector<ProcInfo> procs;

  if (EnumProcesses(pids, sizeof(pids), &needed)) {
    const DWORD count = needed / sizeof(DWORD);
    wchar_t path[MAX_PATH]{};

    for (DWORD i = 0; i < count; ++i) {
      if (pids[i] == 0) continue;

      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
      if (!h) continue;

      PROCESS_MEMORY_COUNTERS_EX pmc{};
      if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
          const wchar_t* base = wcsrchr(path, L'\\');
          base = base ? base + 1 : path;

          ProcInfo info;
          info.name = toUtf8(base);
          info.memMB = pmc.WorkingSetSize / (1024ull * 1024ull);

          if (!info.name.empty() && info.memMB > 0) procs.push_back(std::move(info));
        }
      }
      CloseHandle(h);
    }
  }

  // Chrome and Electron apps show up as many processes with the same image
  // name. Sum them so the three rows mean "three heaviest applications",
  // not "three heaviest process objects".
  std::sort(procs.begin(), procs.end(),
            [](const ProcInfo& a, const ProcInfo& b) { return a.name < b.name; });

  std::vector<ProcInfo> merged;
  for (const auto& p : procs) {
    if (!merged.empty() && merged.back().name == p.name) {
      merged.back().memMB += p.memMB;
    } else {
      merged.push_back(p);
    }
  }

  std::sort(merged.begin(), merged.end(),
            [](const ProcInfo& a, const ProcInfo& b) { return a.memMB > b.memMB; });

  procs.swap(merged);
  if (procs.size() > 4) procs.resize(4);
  snap_.top = std::move(procs);

  fillClock();
  snap_.valid = true;
}

uint64_t Telemetry::signature() const {
  // Quantise so ordinary noise does not force a repaint. A panel that redraws
  // on jitter is a panel that wakes the GPU once a second for nothing.
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };

  mix(static_cast<uint64_t>(std::lround(snap_.cpuPct)));
  mix(static_cast<uint64_t>(std::lround(snap_.ramPct)));
  mix(static_cast<uint64_t>(std::lround(snap_.diskMBs * 2.0)));
  mix(static_cast<uint64_t>(std::lround(snap_.netMbps * 2.0)));
  mix(static_cast<uint64_t>(snap_.hour * 60 + snap_.minute));

  for (const auto& p : snap_.top) {
    for (unsigned char c : p.name) mix(c);
    mix(p.memMB / 8);
  }

  return h;
}
