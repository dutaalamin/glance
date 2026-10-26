#include "launcher.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cwctype>
#include <vector>

namespace {

std::wstring expandEnv(const wchar_t* raw) {
  wchar_t out[MAX_PATH * 2]{};
  const DWORD n = ExpandEnvironmentStringsW(raw, out, static_cast<DWORD>(std::size(out)));
  if (n == 0 || n > std::size(out)) return {};
  return out;
}

bool fileExists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool resolve(const std::wstring& raw, std::wstring& resolved, bool& shellUri) {
  shellUri = false;
  if (raw.rfind(L"ms-", 0) == 0 || raw.rfind(L"shell:", 0) == 0) {
    resolved = raw;
    shellUri = true;
    return true;
  }

  const std::wstring expanded = expandEnv(raw.c_str());
  if (expanded.empty()) return false;

  if (fileExists(expanded)) {
    resolved = expanded;
    return true;
  }

  wchar_t found[MAX_PATH * 2]{};
  if (SearchPathW(nullptr, expanded.c_str(), nullptr, static_cast<DWORD>(std::size(found)), found,
                  nullptr) > 0) {
    resolved = found;
    return true;
  }
  return false;
}

IWICBitmapSource* convert(IWICImagingFactory* wic, IWICBitmap* raw) {
  if (!raw) return nullptr;

  IWICFormatConverter* conv = nullptr;
  IWICBitmapSource* result = nullptr;

  if (SUCCEEDED(wic->CreateFormatConverter(&conv)) && conv) {
    if (SUCCEEDED(conv->Initialize(raw, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      result = conv;
      conv = nullptr;
    }
    if (conv) conv->Release();
  }
  raw->Release();
  return result;
}

// The shell's classic icon API only hands back 32px, which turns soft once it
// fills a 44 DIP tile. IShellItemImageFactory returns the large assets Windows
// already caches, so the tiles stay sharp on scaled displays.
IWICBitmapSource* loadIcon(IWICImagingFactory* wic, const std::wstring& path) {
  IShellItemImageFactory* factory = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItemImageFactory,
                                            reinterpret_cast<void**>(&factory))) &&
      factory) {
    SIZE size{128, 128};
    HBITMAP bitmap = nullptr;

    if (SUCCEEDED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) &&
        bitmap) {
      IWICBitmap* raw = nullptr;
      IWICBitmapSource* result = nullptr;

      if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &raw))) {
        result = convert(wic, raw);
      }
      DeleteObject(bitmap);
      factory->Release();
      if (result) return result;
    }
    factory->Release();
  }

  SHFILEINFOW sfi{};
  if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) ||
      !sfi.hIcon) {
    return nullptr;
  }

  IWICBitmap* raw = nullptr;
  IWICBitmapSource* result = nullptr;
  if (SUCCEEDED(wic->CreateBitmapFromHICON(sfi.hIcon, &raw))) result = convert(wic, raw);
  DestroyIcon(sfi.hIcon);
  return result;
}

// Reads a .lnk without the Shell COM dance: resolves to its target path so the
// icon and the launch both point at the real executable.
std::wstring resolveShortcut(const std::wstring& lnkPath, std::wstring& args) {
  IShellLinkW* link = nullptr;
  std::wstring target;
  args.clear();

  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IShellLinkW, reinterpret_cast<void**>(&link))) &&
      link) {
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) &&
        file) {
      if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ))) {
        wchar_t buf[MAX_PATH * 2]{};
        if (SUCCEEDED(link->GetPath(buf, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) {
          target = buf;
        }
        wchar_t argBuf[1024]{};
        if (SUCCEEDED(link->GetArguments(argBuf, 1024))) args = argBuf;
      }
      file->Release();
    }
    link->Release();
  }
  return target;
}

std::vector<std::wstring> desktopShortcuts() {
  std::vector<std::wstring> out;

  wchar_t desktop[MAX_PATH]{};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
    const std::wstring pattern = std::wstring(desktop) + L"\\*.lnk";

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(std::wstring(desktop) + L"\\" + fd.cFileName);
      } while (FindNextFileW(find, &fd));
      FindClose(find);
    }
  }
  return out;
}

std::wstring baseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos) name = name.substr(0, dot);
  return name;
}

}  // namespace

bool Launcher::load(IWICImagingFactory* wic) {
  wic_ = wic;

  struct Candidate {
    std::wstring name;
    std::wstring target;
    std::wstring args;
    std::wstring iconSource;
  };

  std::vector<Candidate> list;

  auto add = [&list](const wchar_t* name, const wchar_t* target, const wchar_t* icon) {
    list.push_back({name, target, L"", icon ? icon : L""});
  };

  // Pinned first: these are always useful and have no desktop shortcut.
  add(L"Files", L"explorer.exe", nullptr);
  add(L"Terminal", L"wt.exe", nullptr);
  add(L"Settings", L"ms-settings:",
      L"%SystemRoot%\\ImmersiveControlPanel\\SystemSettings.exe");
  add(L"Browser", L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe", nullptr);

  // Then whatever the person actually keeps on their desktop. Scanning beats a
  // hand-maintained list: the island follows the desktop instead of drifting
  // out of date.
  std::vector<std::wstring> seen;
  for (const auto& name : {L"files", L"terminal", L"settings", L"browser"}) seen.push_back(name);

  for (const auto& lnk : desktopShortcuts()) {
    std::wstring args;
    std::wstring target = resolveShortcut(lnk, args);
    if (target.empty() || !fileExists(target)) continue;

    std::wstring name = baseName(lnk);
    if (name == L"Glance") continue;  // do not offer to launch ourselves

    // Tile-friendly labels: the full desktop name does not fit two lines.
    if (name == L"Chrome Remote Desktop") name = L"Remote Desktop";
    if (name == L"Rockstar Games Launcher") name = L"Rockstar";
    if (name == L"Visual Studio Code") name = L"VS Code";
    if (name == L"Antigravity IDE") name = L"Antigravity";
    if (name.find(L"Blender") == 0) name = L"Blender";

    // Discord and friends launch through an updater stub that requires an
    // argument; skip stubs rather than opening a dead window.
    if (baseName(target) == L"Update") continue;

    std::wstring lower = name;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));

    bool duplicate = false;
    for (const auto& s : seen) {
      if (s == lower) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    seen.push_back(lower);
    list.push_back({name, target, args, L""});
  }

  if (list.size() > 12) list.resize(12);

  for (const auto& c : list) {
    std::wstring target;
    bool shellUri = false;
    if (!resolve(c.target.c_str(), target, shellUri)) continue;

    AppEntry entry;
    entry.name = c.name;
    entry.target = target;
    entry.args = c.args;

    std::wstring iconPath;
    if (!shellUri) {
      iconPath = target;
    } else if (!c.iconSource.empty()) {
      iconPath = expandEnv(c.iconSource.c_str());
    }

    if (!iconPath.empty() && fileExists(iconPath)) {
      entry.wic = loadIcon(wic, iconPath);
    }

    apps_.push_back(std::move(entry));
  }

  return !apps_.empty();
}

void Launcher::syncIcons(ID2D1RenderTarget* rt) {
  if (!rt) return;

  for (auto& app : apps_) {
    if (!app.wic) continue;
    if (app.icon && app.owner == rt) continue;

    if (app.icon) {
      app.icon->Release();
      app.icon = nullptr;
    }
    app.owner = nullptr;

    ID2D1Bitmap* bitmap = nullptr;
    if (SUCCEEDED(rt->CreateBitmapFromWicBitmap(app.wic, nullptr, &bitmap)) && bitmap) {
      app.icon = bitmap;
      app.owner = rt;
    }
  }
}

void Launcher::release() {
  for (auto& app : apps_) {
    if (app.icon) {
      app.icon->Release();
      app.icon = nullptr;
    }
    if (app.wic) {
      app.wic->Release();
      app.wic = nullptr;
    }
    app.owner = nullptr;
  }
  apps_.clear();
}

bool Launcher::launch(size_t index) const {
  if (index >= apps_.size()) return false;

  const std::wstring& target = apps_[index].target;
  const std::wstring& args = apps_[index].args;
  const HINSTANCE result =
      ShellExecuteW(nullptr, L"open", target.c_str(), args.empty() ? nullptr : args.c_str(),
                    nullptr, SW_SHOWNORMAL);

  return reinterpret_cast<INT_PTR>(result) > 32;
}
