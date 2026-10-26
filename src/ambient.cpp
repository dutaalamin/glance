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

// Friendly names for AUMIDs we expect to see. Matching is a substring test on
// the AUMID, which is stable enough: the publisher hash and entry point differ
// between builds but the product name does not.
struct KnownAumid {
  const wchar_t* fragment;
  const wchar_t* label;
};

const KnownAumid kKnownAumids[] = {
    {L"WhatsAppDesktop", L"WhatsApp"},
    {L"WhatsApp", L"WhatsApp"},
    {L"Telegram", L"Telegram"},
    {L"Discord", L"Discord"},
    {L"Slack", L"Slack"},
    {L"MSTeams", L"Teams"},
    {L"ms-teams", L"Teams"},
    {L"LINE", L"LINE"},
    {L"Signal", L"Signal"},
    {L"ScreenSketch", L"Snipping Tool"},
    {L"windowscommunicationsapps", L"Mail"},
    {L"WebExperience", L"Widgets"},
    {L"XboxApp", L"Xbox"},
    {L"Netflix", L"Netflix"},
    {L"Print.PrinterCleanupTask", L"Print Cleanup"},
    {L"SecurityCenter", L"Defender"},
    {L"stablyai.orca", L"Orca"},
    {L"Spotify", L"Spotify"},
};

std::wstring labelForAumid(const std::wstring& aumid) {
  for (const auto& known : kKnownAumids) {
    if (wcsstr(aumid.c_str(), known.fragment)) return known.label;
  }
  // Unknown app: present the last dotted segment, which is at least readable.
  std::wstring base = aumid;
  const size_t bang = base.find(L'!');
  if (bang != std::wstring::npos) base = base.substr(0, bang);
  const size_t underscore = base.find(L'_');
  if (underscore != std::wstring::npos) base = base.substr(0, underscore);
  const size_t dot = base.find_last_of(L'.');
  if (dot != std::wstring::npos && dot + 1 < base.size()) base = base.substr(dot + 1);
  return base;
}

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

// Packaged apps are addressable through the shell namespace, which hands back
// their real icon even though the process is unpackaged.
IWICBitmapSource* loadIconFromAumid(IWICImagingFactory* wic, const std::wstring& aumid) {
  if (!wic || aumid.empty()) return nullptr;

  const std::wstring path = L"shell:AppsFolder\\" + aumid;
  IShellItemImageFactory* factory = nullptr;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItemImageFactory,
                                        reinterpret_cast<void**>(&factory))) ||
      !factory) {
    return nullptr;
  }

  SIZE size{32, 32};
  HBITMAP bitmap = nullptr;
  IWICBitmapSource* result = nullptr;
  if (SUCCEEDED(factory->GetImage(size, SIIGBF_ICONONLY, &bitmap)) && bitmap) {
    IWICBitmap* raw = nullptr;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &raw))) {
      result = convert(wic, raw);
      raw->Release();
    }
    DeleteObject(bitmap);
  }
  factory->Release();
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

bool AmbientSource::init(IWICImagingFactory* wic, NotifSource* notifs) {
  wic_ = wic;
  notifs_ = notifs;
  return true;
}

void AmbientSource::release() {
  for (auto& b : ambient_.badges) {
    if (b.icon) {
      b.icon->Release();
      b.icon = nullptr;
    }
    b.owner = nullptr;
  }
  ambient_.badges.clear();

  for (auto& entry : iconCache_) {
    if (entry.second) entry.second->Release();
  }
  iconCache_.clear();
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
    const bool present = (status.BatteryFlag & 128) == 0;
    if (!ambient_.hasBattery) ambient_.hasBattery = present;

    if (ambient_.hasBattery && present) {
      ambient_.batteryPct =
          status.BatteryLifePercent <= 100 ? static_cast<int>(status.BatteryLifePercent) : -1;
      ambient_.charging = (status.BatteryFlag & 8) != 0;
      ambient_.acOnline = status.ACLineStatus == 1;
    }
  }

  // --- unread counts published in window titles ---
  // Messaging apps put a real unread number there; the notification store only
  // knows how many toasts arrived, which is history rather than "unread".
  std::vector<std::pair<std::wstring, int>> titleCounts;
  std::vector<std::wstring> titlePaths;
  {
    EnumCtx ctx{&ambient_, &titleCounts, wic_, &titlePaths};
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
  }

  // --- apps with recent notifications ---
  if (notifs_) notifs_->poll();

  std::vector<AppBadge> next;
  constexpr size_t kMaxBadges = 5;

  auto iconFor = [&](const std::wstring& key) -> IWICBitmapSource* {
    auto it = iconCache_.find(key);
    if (it != iconCache_.end()) return it->second;

    IWICBitmapSource* icon = nullptr;
    if (!key.empty()) {
      // Keys starting with "shell:" carry an AUMID; anything else is a path.
      if (key.rfind(L"shell:", 0) == 0) {
        icon = loadIconFromAumid(wic_, key.substr(6));
      } else {
        icon = loadIcon(wic_, key);
      }
    }
    iconCache_[key] = icon;
    return icon;
  };

  if (notifs_) {
    for (const auto& app : notifs_->apps()) {
      if (next.size() >= kMaxBadges) break;

      AppBadge badge;
      badge.app = labelForAumid(app.aumid);
      badge.aumid = app.aumid;

      IWICBitmapSource* icon = iconFor(L"shell:" + app.aumid);
      if (!icon) continue;  // nothing to draw; a dot would be noise

      badge.wic = icon;

      // Attach a real unread count when the app publishes one in its title.
      for (const auto& tc : titleCounts) {
        if (tc.first == badge.app) {
          badge.count = tc.second;
          break;
        }
      }

      next.push_back(badge);
    }
  }

  // Title counts for apps that are not in the notification store (the toast has
  // already been dismissed but the app still knows it has unread messages).
  for (size_t i = 0; i < titleCounts.size() && next.size() < kMaxBadges; ++i) {
    const bool already = std::any_of(next.begin(), next.end(), [&](const AppBadge& b) {
      return b.app == titleCounts[i].first;
    });
    if (already) continue;

    AppBadge badge;
    badge.app = titleCounts[i].first;
    badge.count = titleCounts[i].second;

    IWICBitmapSource* icon = iconFor(titlePaths[i]);
    if (!icon) continue;
    badge.wic = icon;

    next.push_back(badge);
  }

  // Device bitmaps belong to the previous list's render target; they are rebuilt
  // by syncIcons for the new one.
  for (auto& old : ambient_.badges) {
    if (old.icon) old.icon->Release();
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
    badge.owner = nullptr;

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
