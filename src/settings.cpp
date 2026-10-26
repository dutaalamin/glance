#include "settings.h"

#include <shlobj.h>
#include <windows.h>

namespace settings {

std::wstring path() {
  static const std::wstring resolved = [] {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return std::wstring();
    const std::wstring dir = std::wstring(appdata) + L"\\Glance";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
  }();
  return resolved;
}

Values load() {
  Values values;
  const std::wstring file = path();
  if (file.empty()) return values;

  // Written as on/off rather than 1/0 so the file is readable when edited by hand.
  wchar_t buf[16]{};
  GetPrivateProfileStringW(L"panel", L"launcher", L"off", buf, 16, file.c_str());
  values.launcher = _wcsicmp(buf, L"on") == 0;
  return values;
}

}  // namespace settings
