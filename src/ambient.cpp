#include "ambient.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwchar>

namespace {

// Apps whose title carries an unread count. Matching is on the executable name
// because that survives app updates, where window classes change.
struct KnownApp {
  const wchar_t* exe;
  const wchar_t* label;
};

const KnownApp kKnown[] = {
    {L"WhatsApp.exe", L"WhatsApp"},
    {L"Telegram.exe", L"Telegram"},
    {L"Discord.exe", L"Discord"},
    {L"slack.exe", L"Slack"},
    {L"Teams.exe", L"Teams"},
    {L"ms-teams.exe", L"Teams"},
    {L"LINE.exe", L"LINE"},
    {L"Signal.exe", L"Signal"},
    {L"msedgewebview2.exe", L""},  // never matched; placeholder for ordering
};

// Pulls the first "(N)" out of a title. Both "(3) WhatsApp" and "Slack (3)" are
// in use, so the position is not assumed.
int unreadFromTitle(const wchar_t* title) {
  if (!title) return 0;

  for (const wchar_t* p = title; *p; ++p) {
    if (*p != L'(') continue;

    int value = 0;
    bool digits = false;
    const wchar_t* q = p + 1;
    while (*q >= L'0' && *q <= L'9' && (q - p) <= 4) {
      value = value * 10 + (*q - L'0');
      digits = true;
      ++q;
    }
    // Require a closing paren directly after the digits: "(3)" yes, "(2024)" no
    // longer than 4 digits, and "(3x)" no.
    if (digits && *q == L')') return value;
  }
  return 0;
}

std::wstring processName(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return {};

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};

  wchar_t path[MAX_PATH]{};
  DWORD size = MAX_PATH;
  std::wstring name;
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    name = slash ? slash + 1 : path;
  }
  CloseHandle(process);
  return name;
}

std::wstring processPath(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return {};

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};

  wchar_t path[MAX_PATH]{};
  DWORD size = MAX_PATH;
  const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
  CloseHandle(process);
  return ok ? std::wstring(path) : std::wstring();
}

// Same conversion the launcher uses: WIC source to a premultiplied BGRA bitmap
// that Direct2D can draw without a colour-key surprise.
IWICBitmapSource* convert(IWICImagingFactory* wic, IWICBitmapSource* source) {
  IWICFormatConverter* converter = nullptr;
  if (FAILED(wic->CreateFormatConverter(&converter)) || !converter) return nullptr;

  IWICBitmapSource* result = nullptr;
  if (SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom))) {
    result = converter;
    converter = nullptr;
  }
  if (converter) converter->Release();
  return result;
}

IWICBitmapSource* loadIcon(IWICImagingFactory* wic, const std::wstring& path) {
  if (!wic || path.empty()) return nullptr;

  IShellItemImageFactory* factory = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItemImageFactory,
                                            reinterpret_cast<void**>(&factory))) &&
      factory) {
    SIZE size{32, 32};
    HBITMAP bitmap = nullptr;
    if (SUCCEEDED(factory->GetImage(size, SIIGBF_ICONONLY, &bitmap)) && bitmap) {
      IWICBitmap* raw = nullptr;
      IWICBitmapSource* result = nullptr;
      if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &raw))) {
        result = convert(wic, raw);
        raw->Release();
      }
      DeleteObject(bitmap);
      factory->Release();
      if (result) return result;
    }
    factory->Release();
  }

  SHFILEINFOW info{};
  if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) ||
      !info.hIcon) {
    return nullptr;
  }

  IWICBitmap* raw = nullptr;
  IWICBitmapSource* result = nullptr;
  if (SUCCEEDED(wic->CreateBitmapFromHICON(info.hIcon, &raw))) {
    result = convert(wic, raw);
    raw->Release();
  }
  DestroyIcon(info.hIcon);
  return result;
}

struct EnumCtx {
  Ambient* ambient;
  // Highest count seen per label, so two windows of the same app do not add up
  // to a number the user never saw.
  std::vector<std::pair<std::wstring, int>>* found;
  IWICImagingFactory* wic;
  std::vector<std::wstring>* paths;
};

BOOL CALLBACK enumProc(HWND hwnd, LPARAM data) {
  auto* ctx = reinterpret_cast<EnumCtx*>(data);

  if (!IsWindowVisible(hwnd)) return TRUE;
  if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

  wchar_t title[512]{};
  if (GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;

  const int count = unreadFromTitle(title);
  if (count <= 0) return TRUE;

  const std::wstring exe = processName(hwnd);
  for (const auto& known : kKnown) {
    if (_wcsicmp(exe.c_str(), known.exe) != 0) continue;
    if (!known.label[0]) break;

    const std::wstring label = known.label;
    auto& list = *ctx->found;
    auto it = std::find_if(list.begin(), list.end(),
                           [&](const std::pair<std::wstring, int>& e) {
                             return e.first == label;
                           });
    if (it == list.end()) {
      list.emplace_back(label, count);
      ctx->paths->push_back(processPath(hwnd));
    } else if (count > it->second) {
      it->second = count;
    }
    break;
  }
  return TRUE;
}

}  // namespace

bool AmbientSource::init(IWICImagingFactory* wic) {
  wic_ = wic;
  return true;
}

void AmbientSource::release() {
  for (auto& b : ambient_.badges) {
    if (b.icon) {
      b.icon->Release();
      b.icon = nullptr;
    }
    if (b.wic) {
      b.wic->Release();
      b.wic = nullptr;
    }
    b.owner = nullptr;
  }
  ambient_.badges.clear();
}

void AmbientSource::poll() {
  // --- battery ---
  // GLANCE_FAKE_BATTERY lets the battery readout be exercised on a desktop with
  // no battery. Format: "<percent>" or "<percent>,charging". Diagnostics only.
  if (const wchar_t* forced = _wgetenv(L"GLANCE_FAKE_BATTERY")) {
    int pct = _wtoi(forced);
    ambient_.hasBattery = true;
    ambient_.batteryPct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    ambient_.charging = wcschr(forced, L',') != nullptr;
    ambient_.acOnline = ambient_.charging;
  }

  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status)) {
    // BatteryFlag 128 means "no system battery": a desktop, where a battery
    // readout would be a permanent lie.
    // The override above wins on a machine with no battery; otherwise trust
    // the system flag.
    const bool present = (status.BatteryFlag & 128) == 0;
    if (!ambient_.hasBattery) ambient_.hasBattery = present;
    if (ambient_.hasBattery && present) {
      ambient_.batteryPct =
          status.BatteryLifePercent <= 100 ? static_cast<int>(status.BatteryLifePercent) : -1;
      ambient_.charging = (status.BatteryFlag & 8) != 0;
      ambient_.acOnline = status.ACLineStatus == 1;
    } else if (!ambient_.hasBattery) {
      // Only reset when the override did not already supply a value.
      ambient_.batteryPct = 0;
      ambient_.charging = false;
      ambient_.acOnline = status.ACLineStatus == 1;
    }
  }

  // --- unread badges ---
  std::vector<std::pair<std::wstring, int>> foundList;
  std::vector<std::wstring> paths;
  EnumCtx ctx{&ambient_, &foundList, wic_, &paths};
  EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));

  // Rebuild the badge list, reusing icons for apps that were already showing so
  // a per-second poll does not re-decode them.
  std::vector<AppBadge> next;
  next.reserve(foundList.size());

  for (size_t i = 0; i < foundList.size(); ++i) {
    AppBadge badge;
    badge.app = foundList[i].first;
    badge.count = foundList[i].second;

    auto existing = std::find_if(ambient_.badges.begin(), ambient_.badges.end(),
                                 [&](const AppBadge& b) { return b.app == badge.app; });
    if (existing != ambient_.badges.end()) {
      badge.wic = existing->wic;
      badge.icon = existing->icon;
      badge.owner = existing->owner;
      existing->wic = nullptr;
      existing->icon = nullptr;
      existing->owner = nullptr;
    } else if (wic_) {
      badge.wic = loadIcon(wic_, paths[i]);
    }

    next.push_back(std::move(badge));
  }

  // Anything left in the old list was not carried over: free it.
  for (auto& old : ambient_.badges) {
    if (old.icon) old.icon->Release();
    if (old.wic) old.wic->Release();
  }

  ambient_.badges = std::move(next);
}

void AmbientSource::syncIcons(ID2D1RenderTarget* rt) {
  if (!rt) return;

  for (auto& badge : ambient_.badges) {
    if (!badge.wic) continue;
    if (badge.icon && badge.owner == rt) continue;

    if (badge.icon) {
      badge.icon->Release();
      badge.icon = nullptr;
    }

    IWICFormatConverter* converter = nullptr;
    if (FAILED(wic_->CreateFormatConverter(&converter)) || !converter) continue;

    if (SUCCEEDED(converter->Initialize(badge.wic, GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
      rt->CreateBitmapFromWicBitmap(converter, nullptr, &badge.icon);
    }
    converter->Release();
    badge.owner = badge.icon ? rt : nullptr;
  }
}
