#include "activate.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

namespace {

// IApplicationActivationManager declared by hand: the SDK header pulls in
// winnt.h/kernelspecs.h, which cannot be mixed with MinGW's CRT. The vtable
// order comes from shobjidl_core.h (ActivateApplication, ActivateForFile,
// ActivateForProtocol).
const IID kIID_AppActivationManager = {
    0x2e941141, 0x7f97, 0x4756, {0xba, 0x1d, 0x9d, 0xec, 0xde, 0x89, 0x4a, 0x3d}};
const CLSID kCLSID_AppActivationManager = {
    0x45ba127d, 0x10a8, 0x46ea, {0x8a, 0xb7, 0x56, 0xea, 0x90, 0x78, 0x94, 0x3c}};

struct IAppActivationVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(void*);
  ULONG(STDMETHODCALLTYPE* Release)(void*);
  HRESULT(STDMETHODCALLTYPE* ActivateApplication)(void*, LPCWSTR, LPCWSTR, DWORD, DWORD*);
  HRESULT(STDMETHODCALLTYPE* ActivateForFile)(void*, LPCWSTR, void*, LPCWSTR, DWORD*);
  HRESULT(STDMETHODCALLTYPE* ActivateForProtocol)(void*, LPCWSTR, void*, DWORD*);
};

}  // namespace

bool activateAumid(const std::wstring& aumid) {
  if (aumid.empty()) return false;

  void* manager = nullptr;
  if (SUCCEEDED(CoCreateInstance(kCLSID_AppActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                 kIID_AppActivationManager, &manager)) &&
      manager) {
    auto* vtbl = *static_cast<IAppActivationVtbl**>(manager);
    DWORD pid = 0;
    // AO_NONE: no special option. A null arguments string means "just open".
    const HRESULT hr = vtbl->ActivateApplication(manager, aumid.c_str(), nullptr, 0, &pid);
    vtbl->Release(manager);
    if (SUCCEEDED(hr)) return true;
  }

  // Fallback: the shell namespace entry. Works for apps that the activation
  // manager refuses, and for plain executables that registered an AUMID.
  const std::wstring path = L"shell:AppsFolder\\" + aumid;
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                        SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(result) > 32;
}
