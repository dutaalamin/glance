#include "media.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>
#include <wincodec.h>  // HSTRING type only; the functions are resolved at runtime.

#include <algorithm>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// The Windows SDK ships C++/WinRT and WRL headers for these APIs, but they are
// Microsoft-compiler specific (_uuidof, WRL version gates) and cannot be parsed
// by MinGW. The vtable orders below were taken from
//   Windows Kits/10/Include/10.0.18362.0/winrt/windows.media.control.h
// and windows.foundation.h, in declaration order. Every IInspectable-derived
// interface therefore starts six slots in (IUnknown's three, then GetIids,
// GetRuntimeClassName, GetTrustLevel).
// ---------------------------------------------------------------------------

namespace {

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using RoInitializeFn = HRESULT(WINAPI*)(int);

RoGetActivationFactoryFn pRoGetActivationFactory = nullptr;
RoInitializeFn pRoInitialize = nullptr;

// MinGW ships no import library for the WinRT string helpers, so they come from
// combase.dll the same way the activation factory does.
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, HSTRING*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
using WindowsGetStringRawBufferFn = const wchar_t*(WINAPI*)(HSTRING, UINT32*);

WindowsCreateStringFn pWindowsCreateString = nullptr;
WindowsDeleteStringFn pWindowsDeleteString = nullptr;
WindowsGetStringRawBufferFn pWindowsGetStringRawBuffer = nullptr;

bool loadRuntime() {
  if (pRoGetActivationFactory) return true;

  HMODULE combase = LoadLibraryW(L"combase.dll");
  if (!combase) return false;

  pRoGetActivationFactory =
      reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
  pRoInitialize = reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize"));
  pWindowsCreateString = reinterpret_cast<WindowsCreateStringFn>(
      GetProcAddress(combase, "WindowsCreateString"));
  pWindowsDeleteString =
      reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
  pWindowsGetStringRawBuffer = reinterpret_cast<WindowsGetStringRawBufferFn>(
      GetProcAddress(combase, "WindowsGetStringRawBuffer"));

  return pRoGetActivationFactory && pWindowsCreateString && pWindowsDeleteString &&
         pWindowsGetStringRawBuffer;
}

// --- GUIDs (from the SDK header, midl interface attributes) ---
const IID kIID_SessionManagerStatics = {
    0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
const IID kIID_SessionManager = {
    0xcace8eac, 0xe86e, 0x504a, {0xab, 0x31, 0x5f, 0xf8, 0xff, 0x1b, 0xce, 0x49}};
const IID kIID_Session = {
    0x7148c835, 0x9b14, 0x5ae2, {0xab, 0x85, 0xdc, 0x9b, 0x1c, 0x14, 0xe1, 0xa8}};
const IID kIID_MediaProperties = {
    0x68856cf6, 0xadb4, 0x54b2, {0xac, 0x16, 0x05, 0x83, 0x79, 0x07, 0xac, 0xb6}};
const IID kIID_PlaybackInfo = {
    0x94b4b6cf, 0xe8ba, 0x51ad, {0x87, 0xa7, 0xc1, 0x0a, 0xde, 0x10, 0x61, 0x27}};
const IID kIID_AsyncInfo = {
    0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIID_TimelineProperties = {
    0xede34136, 0x6f25, 0x588d, {0x8e, 0xcf, 0xea, 0x5b, 0x67, 0x35, 0xaa, 0xa5}};

// --- minimal ABI shapes ---
struct IInspectableVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(void*);
  ULONG(STDMETHODCALLTYPE* Release)(void*);
  HRESULT(STDMETHODCALLTYPE* GetIids)(void*, ULONG*, IID**);
  HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(void*, int*);
};

struct IInspectable {
  IInspectableVtbl* vtbl;
};

enum AsyncStatusAbi { kStarted = 0, kCompleted = 1, kCanceled = 2, kError = 3 };

// IAsyncOperation<T> in the ABI is IInspectable followed directly by the
// operation methods; IAsyncInfo (and therefore get_Status) is a separate
// interface reached through QueryInterface. Verified against the SDK's own C
// vtable struct __FIAsyncOperation_1_HSTRINGVtbl / IAsyncInfoVtbl in
// windows.foundation.h and asyncinfo.h.
struct IAsyncOperationVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* put_Completed)(void*, void*);
  HRESULT(STDMETHODCALLTYPE* get_Completed)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* GetResults)(void*, void**);
};

struct IAsyncInfoVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_Id)(void*, UINT32*);
  HRESULT(STDMETHODCALLTYPE* get_Status)(void*, int*);
  HRESULT(STDMETHODCALLTYPE* get_ErrorCode)(void*, HRESULT*);
  HRESULT(STDMETHODCALLTYPE* Cancel)(void*);
  HRESULT(STDMETHODCALLTYPE* Close)(void*);
};

struct IAsyncOperation {
  IAsyncOperationVtbl* vtbl;
};

struct ISessionManagerStaticsVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* RequestAsync)(void*, IAsyncOperation**);
};

struct ISessionManagerVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* GetCurrentSession)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* GetSessions)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* add_CurrentSessionChanged)(void*, void*, void*);
  HRESULT(STDMETHODCALLTYPE* remove_CurrentSessionChanged)(void*, void*);
  HRESULT(STDMETHODCALLTYPE* add_SessionsChanged)(void*, void*, void*);
  HRESULT(STDMETHODCALLTYPE* remove_SessionsChanged)(void*, void*);
};

struct ISessionVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_SourceAppUserModelId)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* TryGetMediaPropertiesAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* GetTimelineProperties)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* GetPlaybackInfo)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* TryPlayAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryPauseAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryStopAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryRecordAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryFastForwardAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryRewindAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TrySkipNextAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TrySkipPreviousAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryChangeChannelUpAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryChangeChannelDownAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryTogglePlayPauseAsync)(void*, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryChangeAutoRepeatModeAsync)(void*, int, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryChangePlaybackRateAsync)(void*, double, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryChangeShuffleActiveAsync)(void*, bool, IAsyncOperation**);
  HRESULT(STDMETHODCALLTYPE* TryChangePlaybackPositionAsync)(void*, LONGLONG,
                                                             IAsyncOperation**);
};

// Slots 5..8 (TrackNumber, Genres, AlbumTrackCount, PlaybackType) are declared
// only so that get_Thumbnail lands on slot 9, where the SDK header puts it.
struct IMediaPropertiesVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_Title)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* get_Subtitle)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* get_AlbumArtist)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* get_Artist)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* get_AlbumTitle)(void*, HSTRING*);
  HRESULT(STDMETHODCALLTYPE* get_TrackNumber)(void*, INT32*);
  HRESULT(STDMETHODCALLTYPE* get_Genres)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* get_AlbumTrackCount)(void*, INT32*);
  HRESULT(STDMETHODCALLTYPE* get_PlaybackType)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* get_Thumbnail)(void*, void**);
};

struct IPlaybackInfoVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_Controls)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* get_PlaybackStatus)(void*, int*);
};

struct ITimelinePropertiesVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_StartTime)(void*, LONGLONG*);
  HRESULT(STDMETHODCALLTYPE* get_EndTime)(void*, LONGLONG*);
  HRESULT(STDMETHODCALLTYPE* get_MinSeekTime)(void*, LONGLONG*);
  HRESULT(STDMETHODCALLTYPE* get_MaxSeekTime)(void*, LONGLONG*);
  HRESULT(STDMETHODCALLTYPE* get_Position)(void*, LONGLONG*);
  HRESULT(STDMETHODCALLTYPE* get_LastUpdatedTime)(void*, LONGLONG*);
};

struct ISessionManager { ISessionManagerVtbl* vtbl; };
struct ISession { ISessionVtbl* vtbl; };
struct IMediaProperties { IMediaPropertiesVtbl* vtbl; };
struct IPlaybackInfo { IPlaybackInfoVtbl* vtbl; };
struct ITimelineProperties { ITimelinePropertiesVtbl* vtbl; };

// AsyncStatus: 0 started, 1 completed, 2 canceled, 3 error. -1 when the object
// exposes no IAsyncInfo, which callers treat as "not ready yet".
int asyncStatus(void* operation) {
  if (!operation) return -1;

  void* raw = nullptr;
  const HRESULT hr = static_cast<IInspectable*>(operation)->vtbl->QueryInterface(
      operation, kIID_AsyncInfo, &raw);
  if (FAILED(hr) || !raw) return -1;

  IAsyncInfoVtbl* vtbl = *static_cast<IAsyncInfoVtbl**>(raw);
  int status = -1;
  vtbl->get_Status(raw, &status);
  vtbl->base.Release(raw);
  return status;
}

// --- WinRT streams, used only to pull the cover art bytes out of SMTC -------------
//
// Same situation as the SMTC interfaces above: the SDK's C++ headers are
// MSVC-only, so the shapes are declared by hand. The vtable orders come from
// windows.storage.streams.h (IInputStream, IRandomAccessStream, IBuffer,
// IBufferFactory) and robuffer.h (IBufferByteAccess); IAsyncOperationWithProgress
// is IInspectable + put_Progress + get_Progress + put_Completed + get_Completed +
// GetResults, with IAsyncInfo again reached through QueryInterface.

const IID kIID_RandomAccessStreamReference = {
    0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
const IID kIID_RandomAccessStream = {
    0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const IID kIID_InputStream = {
    0x905a0fe2, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const IID kIID_Buffer = {
    0x905a0fe0, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const IID kIID_BufferByteAccess = {
    0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const IID kIID_BufferFactory = {
    0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};

// IAsyncOperationWithProgress<T, U>: the operation methods come first, then the
// two progress accessors, then the completion pair and GetResults.
struct IAsyncProgressVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* put_Progress)(void*, void*);
  HRESULT(STDMETHODCALLTYPE* get_Progress)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* put_Completed)(void*, void*);
  HRESULT(STDMETHODCALLTYPE* get_Completed)(void*, void**);
  HRESULT(STDMETHODCALLTYPE* GetResults)(void*, void**);
};

struct IAsyncProgress {
  IAsyncProgressVtbl* vtbl;
};

struct IRandomAccessStreamReferenceVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* OpenReadAsync)(void*, IAsyncOperation**);
};

struct IRandomAccessStreamVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_Size)(void*, UINT64*);
  HRESULT(STDMETHODCALLTYPE* put_Size)(void*, UINT64);
  HRESULT(STDMETHODCALLTYPE* GetInputStreamAt)(void*, UINT64, void**);
  HRESULT(STDMETHODCALLTYPE* GetOutputStreamAt)(void*, UINT64, void**);
  HRESULT(STDMETHODCALLTYPE* get_Position)(void*, UINT64*);
  HRESULT(STDMETHODCALLTYPE* Seek)(void*, UINT64);
  HRESULT(STDMETHODCALLTYPE* CloneStream)(void**, ...);
};

struct IInputStreamVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* ReadAsync)(void*, void*, UINT32, int, IAsyncProgress**);
};

struct IBufferVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* get_Capacity)(void*, UINT32*);
  HRESULT(STDMETHODCALLTYPE* get_Length)(void*, UINT32*);
  HRESULT(STDMETHODCALLTYPE* put_Length)(void*, UINT32);
};

struct IBufferFactoryVtbl {
  IInspectableVtbl base;
  HRESULT(STDMETHODCALLTYPE* Create)(void*, UINT32, void**);
};

struct IBufferByteAccessVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(void*);
  ULONG(STDMETHODCALLTYPE* Release)(void*);
  HRESULT(STDMETHODCALLTYPE* get_Buffer)(void*, BYTE**);
};

void releaseInspectable(void* p) {
  if (!p) return;
  static_cast<IInspectable*>(p)->vtbl->Release(p);
}

int asyncProgressStatus(IAsyncProgress* operation) {
  if (!operation) return -1;
  void* info = nullptr;
  const HRESULT hr = reinterpret_cast<IInspectable*>(operation)->vtbl->QueryInterface(
      operation, kIID_AsyncInfo, &info);
  if (FAILED(hr) || !info) return -1;
  IAsyncInfoVtbl* vtbl = *static_cast<IAsyncInfoVtbl**>(info);
  int status = -1;
  vtbl->get_Status(info, &status);
  vtbl->base.Release(info);
  return status;
}

std::wstring readHString(HSTRING value) {
  if (!value) return {};
  UINT32 len = 0;
  const wchar_t* raw = pWindowsGetStringRawBuffer(value, &len);
  std::wstring out;
  if (raw && len) out.assign(raw, len);
  pWindowsDeleteString(value);
  return out;
}

double ticksToSeconds(LONGLONG ticks) { return static_cast<double>(ticks) / 10000000.0; }

// Current UTC time as seconds since 1601-01-01, matching DateTime / FILETIME.
double nowAsFiletimeSeconds() {
  FILETIME ft{};
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  return static_cast<double>(value.QuadPart) / 10000000.0;
}

}  // namespace

struct MediaSession::Impl {
  ISessionManager* manager = nullptr;
  IAsyncOperation* managerOp = nullptr;
  ISession* session = nullptr;
  IAsyncOperation* propsOp = nullptr;
  bool runtimeReady = false;

  // Cover-art pipeline. Driven one step at a time from refresh(); every step is
  // an async WinRT call whose result is harvested on a later tick, so a slow or
  // unresponsive player can never stall the UI thread.
  IWICImagingFactory* wic = nullptr;
  IAsyncOperation* artOpenOp = nullptr;   // OpenReadAsync on the thumbnail
  void* artStream = nullptr;              // IRandomAccessStream
  IAsyncProgress* artReadOp = nullptr;    // ReadAsync of the whole stream
  uint32_t artVersion = 0;
  bool artPending = false;                // a chain is in flight
  bool artTried = false;                  // already attempted for this track
  void* artThumb = nullptr;               // IRandomAccessStreamReference
  void* bufferFactory = nullptr;          // IBufferFactory, activated once

  void releaseArt() {
    if (artOpenOp) {
      artOpenOp->vtbl->base.Release(artOpenOp);
      artOpenOp = nullptr;
    }
    if (artReadOp) {
      artReadOp->vtbl->base.Release(artReadOp);
      artReadOp = nullptr;
    }
    releaseInspectable(artStream);
    artStream = nullptr;
    artPending = false;
  }

  void releaseThumb() {
    releaseInspectable(artThumb);
    artThumb = nullptr;
  }

  // Track changed: drop the old chain and let the new one start.
  void resetArt() {
    releaseArt();
    releaseThumb();
    artTried = false;
  }
};

bool MediaSession::init(IWICImagingFactory* wic) {
  if (!impl_) impl_ = new Impl();
  impl_->wic = wic;

  if (!loadRuntime()) return false;
  if (FAILED(pRoInitialize(1 /* RO_INIT_MULTITHREADED */))) {
    // Already initialised on this thread is fine.
  }

  HSTRING className = nullptr;
  const wchar_t* name =
      L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
  if (FAILED(
          pWindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &className))) {
    return false;
  }

  void* factory = nullptr;
  const HRESULT hr = pRoGetActivationFactory(className, kIID_SessionManagerStatics, &factory);
  pWindowsDeleteString(className);

  if (FAILED(hr) || !factory) return false;

  auto* statics = static_cast<ISessionManagerStaticsVtbl**>(factory);
  const HRESULT requestHr = (*statics)->RequestAsync(factory, &impl_->managerOp);
  (*statics)->base.Release(factory);

  impl_->runtimeReady = SUCCEEDED(requestHr) && impl_->managerOp;
  return impl_->runtimeReady;
}

namespace {

// Decodes PNG/JPEG bytes to a square-ish BGRA buffer. Cover-crops rather than
// squashing, so non-square artwork keeps its proportions inside the round slot.
bool decodeArt(IWICImagingFactory* wic, const std::vector<uint8_t>& bytes, int side,
               std::vector<uint32_t>& out, int& outW, int& outH) {
  if (!wic || bytes.empty()) return false;

  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICBitmapClipper* clipper = nullptr;
  IWICFormatConverter* converter = nullptr;
  bool ok = false;

  auto cleanup = [&] {
    if (converter) converter->Release();
    if (clipper) clipper->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
  };

  if (FAILED(wic->CreateStream(&stream)) || !stream) {
    cleanup();
    return false;
  }
  if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                          static_cast<DWORD>(bytes.size())))) {
    cleanup();
    return false;
  }
  if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                          &decoder)) ||
      !decoder) {
    cleanup();
    return false;
  }
  if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
    cleanup();
    return false;
  }

  UINT srcW = 0, srcH = 0;
  if (FAILED(frame->GetSize(&srcW, &srcH)) || srcW == 0 || srcH == 0) {
    cleanup();
    return false;
  }

  // Scale so the shorter edge covers `side`, then crop the centre square.
  const UINT shorter = srcW < srcH ? srcW : srcH;
  const UINT scaledW = static_cast<UINT>(static_cast<uint64_t>(srcW) * side / shorter);
  const UINT scaledH = static_cast<UINT>(static_cast<uint64_t>(srcH) * side / shorter);

  if (FAILED(wic->CreateBitmapScaler(&scaler)) || !scaler ||
      FAILED(scaler->Initialize(frame, scaledW, scaledH, WICBitmapInterpolationModeFant))) {
    cleanup();
    return false;
  }
  WICRect cropRect{static_cast<INT>((scaledW - side) / 2),
                   static_cast<INT>((scaledH - side) / 2), static_cast<INT>(side),
                   static_cast<INT>(side)};
  if (FAILED(wic->CreateBitmapClipper(&clipper)) || !clipper ||
      FAILED(clipper->Initialize(scaler, &cropRect))) {
    cleanup();
    return false;
  }
  if (FAILED(wic->CreateFormatConverter(&converter)) || !converter ||
      FAILED(converter->Initialize(clipper, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    cleanup();
    return false;
  }

  std::vector<uint32_t> pixels(static_cast<size_t>(side) * side, 0);
  const UINT stride = static_cast<UINT>(side) * 4;
  if (SUCCEEDED(converter->CopyPixels(nullptr, stride,
                                      static_cast<UINT>(pixels.size() * 4),
                                      reinterpret_cast<BYTE*>(pixels.data())))) {
    out = std::move(pixels);
    outW = side;
    outH = side;
    ok = true;
  }

  cleanup();
  return ok;
}

}  // namespace

void MediaSession::release() {
  if (!impl_) return;

  impl_->releaseArt();
  impl_->releaseThumb();
  releaseInspectable(impl_->bufferFactory);
  impl_->bufferFactory = nullptr;

  if (impl_->propsOp) {
    impl_->propsOp->vtbl->base.Release(impl_->propsOp);
    impl_->propsOp = nullptr;
  }
  if (impl_->session) {
    impl_->session->vtbl->base.Release(impl_->session);
    impl_->session = nullptr;
  }
  if (impl_->managerOp) {
    impl_->managerOp->vtbl->base.Release(impl_->managerOp);
    impl_->managerOp = nullptr;
  }
  if (impl_->manager) {
    impl_->manager->vtbl->base.Release(impl_->manager);
    impl_->manager = nullptr;
  }

  delete impl_;
  impl_ = nullptr;
}

void MediaSession::refresh() {
  if (!impl_ || !impl_->runtimeReady) return;

  // Manager arrives asynchronously, once.
  if (!impl_->manager) {
    const int status = asyncStatus(impl_->managerOp);
    if (status < 0) return;

    if (status == kCompleted) {
      void* raw = nullptr;
      if (SUCCEEDED(impl_->managerOp->vtbl->GetResults(impl_->managerOp, &raw)) && raw) {
        impl_->manager = static_cast<ISessionManager*>(raw);
      }
      impl_->managerOp->vtbl->base.Release(impl_->managerOp);
      impl_->managerOp = nullptr;
    } else if (status == kError || status == kCanceled) {
      impl_->managerOp->vtbl->base.Release(impl_->managerOp);
      impl_->managerOp = nullptr;
      impl_->runtimeReady = false;
      return;
    }

    if (!impl_->manager) {
      now_ = NowPlaying{};
      return;
    }
  }

  // Re-read the current session every tick: the user may switch players.
  void* rawSession = nullptr;
  if (FAILED(impl_->manager->vtbl->GetCurrentSession(impl_->manager, &rawSession)) ||
      !rawSession) {
    if (impl_->session) {
      impl_->session->vtbl->base.Release(impl_->session);
      impl_->session = nullptr;
    }
    if (impl_->propsOp) {
      impl_->propsOp->vtbl->base.Release(impl_->propsOp);
      impl_->propsOp = nullptr;
    }
    now_ = NowPlaying{};
    impl_->resetArt();
    return;
  }

  auto* session = static_cast<ISession*>(rawSession);
  if (impl_->session && impl_->session != session) {
    if (impl_->propsOp) {
      impl_->propsOp->vtbl->base.Release(impl_->propsOp);
      impl_->propsOp = nullptr;
    }
  }
  if (impl_->session && impl_->session != session) {
    impl_->session->vtbl->base.Release(impl_->session);
  }
  impl_->session = session;

  now_.hasSession = true;

  // Playback status is a synchronous call.
  void* rawPlayback = nullptr;
  if (SUCCEEDED(impl_->session->vtbl->GetPlaybackInfo(impl_->session, &rawPlayback)) &&
      rawPlayback) {
    auto* playback = static_cast<IPlaybackInfo*>(rawPlayback);
    int status = 0;
    if (SUCCEEDED(playback->vtbl->get_PlaybackStatus(playback, &status))) {
      now_.playing = (status == 4);  // Playing
    }
    releaseInspectable(playback);
  }

  // Timeline is synchronous as well.
  void* rawTimeline = nullptr;
  if (SUCCEEDED(impl_->session->vtbl->GetTimelineProperties(impl_->session, &rawTimeline)) &&
      rawTimeline) {
    auto* timeline = static_cast<ITimelineProperties*>(rawTimeline);
    LONGLONG start = 0, end = 0, position = 0;
    if (SUCCEEDED(timeline->vtbl->get_StartTime(timeline, &start)) &&
        SUCCEEDED(timeline->vtbl->get_EndTime(timeline, &end)) &&
        SUCCEEDED(timeline->vtbl->get_Position(timeline, &position))) {
      now_.positionSec = std::max(0.0, ticksToSeconds(position) - ticksToSeconds(start));
      now_.durationSec = std::max(0.0, ticksToSeconds(end) - ticksToSeconds(start));

      // LastUpdatedTime is a Windows.Foundation.DateTime: 100 ns ticks since
      // 1601-01-01 UTC, the same base as FILETIME. Anchor the position there so
      // currentPositionSec() can extrapolate from the moment it was true.
      LONGLONG updated = 0;
      if (SUCCEEDED(timeline->vtbl->get_LastUpdatedTime(timeline, &updated)) && updated > 0) {
        now_.anchorUnix = ticksToSeconds(updated);
      } else {
        now_.anchorUnix = nowAsFiletimeSeconds();
      }
    }
    releaseInspectable(timeline);
  }

  // Media properties are async but stable per track: start once, harvest later.
  if (!impl_->propsOp) {
    impl_->session->vtbl->TryGetMediaPropertiesAsync(impl_->session, &impl_->propsOp);
  }
  if (!impl_->propsOp) return;

  const int propStatus = asyncStatus(impl_->propsOp);
  if (propStatus < 0) return;

  if (propStatus == kCompleted) {
    void* rawProps = nullptr;
    if (SUCCEEDED(impl_->propsOp->vtbl->GetResults(impl_->propsOp, &rawProps)) && rawProps) {
      auto* props = static_cast<IMediaProperties*>(rawProps);
      HSTRING value = nullptr;

      // Read into locals first: a fresh properties object is produced on every
      // harvest, so the text alone says whether this is a different track.
      std::wstring title, artist, album;
      if (SUCCEEDED(props->vtbl->get_Title(props, &value))) title = readHString(value);
      value = nullptr;
      if (SUCCEEDED(props->vtbl->get_Artist(props, &value))) artist = readHString(value);
      value = nullptr;
      if (SUCCEEDED(props->vtbl->get_AlbumTitle(props, &value))) album = readHString(value);

      // Only a changed track invalidates the cover. Resetting on every harvest
      // killed the art chain before it could finish, because the properties
      // object is rebuilt each second.
      if (title != now_.title || artist != now_.artist || album != now_.album) {
        impl_->resetArt();
        now_.art.clear();
        now_.artW = 0;
        now_.artH = 0;
        ++now_.artVersion;
      }

      now_.title = std::move(title);
      now_.artist = std::move(artist);
      now_.album = std::move(album);

      // Keep the thumbnail reference: opening it is a separate async chain and
      // the properties object is released right here.
      void* thumb = nullptr;
      // Properties are re-harvested every second. Only take the thumbnail the
      // first time for a track - re-taking it each tick restarted the art chain
      // before it could ever finish.
      if (!impl_->artThumb) {
        if (SUCCEEDED(props->vtbl->get_Thumbnail(props, &thumb)) && thumb) {
          impl_->artThumb = thumb;
          impl_->artTried = false;
        }
      }

      releaseInspectable(props);
    }
    impl_->propsOp->vtbl->base.Release(impl_->propsOp);
    impl_->propsOp = nullptr;
  } else if (propStatus == kError || propStatus == kCanceled) {
    impl_->propsOp->vtbl->base.Release(impl_->propsOp);
    impl_->propsOp = nullptr;
  }

  pumpArt();
}

// Advances the cover-art chain by at most one completed step. Called from
// refresh(), so a slow player costs one getter per tick, never a blocked frame.
void MediaSession::pumpArt() {
  if (!impl_ || !impl_->wic) return;

  // Start: open the thumbnail the player handed us.
  if (!impl_->artPending && !impl_->artTried && impl_->artThumb) {
    impl_->artTried = true;
    auto* ref = static_cast<IRandomAccessStreamReferenceVtbl**>(impl_->artThumb);
    HRESULT oa = (*ref)->OpenReadAsync(impl_->artThumb, &impl_->artOpenOp);
    if (SUCCEEDED(oa) &&
        impl_->artOpenOp) {
      impl_->artPending = true;
    }
    return;
  }

  if (!impl_->artPending) return;

  // Step: the stream opened.
  if (impl_->artOpenOp) {
    const int status = asyncStatus(impl_->artOpenOp);
    if (status < 0) return;
    if (status != kCompleted) {
      if (status == kError || status == kCanceled) impl_->releaseArt();
      return;
    }

    void* raw = nullptr;
    HRESULT gr = impl_->artOpenOp->vtbl->GetResults(impl_->artOpenOp, &raw);
    if (SUCCEEDED(gr) && raw) {
      impl_->artStream = raw;
    }
    impl_->artOpenOp->vtbl->base.Release(impl_->artOpenOp);
    impl_->artOpenOp = nullptr;

    if (!impl_->artStream) {
      impl_->releaseArt();
      return;
    }

    // The object handed back is IRandomAccessStreamWithContentType, whose vtable
    // has IClosable (and therefore Close) ahead of the stream methods. QI to the
    // plain IRandomAccessStream so slot 6 really is get_Size.
    void* pure = nullptr;
    if (FAILED(reinterpret_cast<IInspectable*>(impl_->artStream)->vtbl->QueryInterface(
            impl_->artStream, kIID_RandomAccessStream, &pure)) ||
        !pure) {
      impl_->releaseArt();
      return;
    }

    auto* stream = static_cast<IRandomAccessStreamVtbl**>(pure);
    UINT64 size = 0;
    if (FAILED((*stream)->get_Size(pure, &size)) || size == 0 ||
        size > 8u * 1024u * 1024u) {
      releaseInspectable(pure);
      impl_->releaseArt();
      return;
    }

    void* input = nullptr;
    HRESULT gi = (*stream)->GetInputStreamAt(pure, 0, &input);
    if (FAILED(gi) || !input) {
      impl_->releaseArt();
      return;
    }

    // Buffer factory, activated once for the life of the object.
    if (!impl_->bufferFactory) {
      HSTRING cls = nullptr;
      const wchar_t* name = L"Windows.Storage.Streams.Buffer";
      if (SUCCEEDED(pWindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &cls))) {
        void* factory = nullptr;
        if (SUCCEEDED(pRoGetActivationFactory(cls, kIID_BufferFactory, &factory))) {
          impl_->bufferFactory = factory;
        }
        pWindowsDeleteString(cls);
      }
    }

    if (!impl_->bufferFactory) {
      releaseInspectable(input);
      impl_->releaseArt();
      return;
    }

    void* buffer = nullptr;
    auto* factory = static_cast<IBufferFactoryVtbl**>(impl_->bufferFactory);
    const UINT32 wanted = static_cast<UINT32>(size);
    if (FAILED((*factory)->Create(impl_->bufferFactory, wanted, &buffer)) || !buffer) {
      releaseInspectable(input);
      impl_->releaseArt();
      return;
    }

    auto* in = static_cast<IInputStreamVtbl**>(input);
    // InputStreamOptions::Partial - take what the stream has, we know the size.
    const HRESULT hr = (*in)->ReadAsync(input, buffer, wanted, 0, &impl_->artReadOp);
    releaseInspectable(input);
    releaseInspectable(buffer);

    if (FAILED(hr) || !impl_->artReadOp) {
      impl_->releaseArt();
      return;
    }
    return;
  }

  // Step: the bytes arrived.
  if (impl_->artReadOp) {
    const int status = asyncProgressStatus(impl_->artReadOp);
    if (status < 0) return;
    if (status != kCompleted) {
      if (status == kError || status == kCanceled) impl_->releaseArt();
      return;
    }

    void* raw = nullptr;
    std::vector<uint8_t> bytes;
    if (SUCCEEDED(impl_->artReadOp->vtbl->GetResults(impl_->artReadOp, &raw)) && raw) {
      UINT32 length = 0;
      auto* buffer = static_cast<IBufferVtbl**>(raw);
      if (SUCCEEDED((*buffer)->get_Length(raw, &length)) && length > 0) {
        void* access = nullptr;
        if (SUCCEEDED(reinterpret_cast<IInspectable*>(raw)->vtbl->QueryInterface(
                raw, kIID_BufferByteAccess, &access)) &&
            access) {
          BYTE* data = nullptr;
          auto* vtbl = *static_cast<IBufferByteAccessVtbl**>(access);
          if (SUCCEEDED(vtbl->get_Buffer(access, &data)) && data) {
            bytes.assign(data, data + length);
          }
          vtbl->Release(access);
        }
      }
      releaseInspectable(raw);
    }

    impl_->artReadOp->vtbl->base.Release(impl_->artReadOp);
    impl_->artReadOp = nullptr;

    // 96 px covers a 40 px slot on a 200% display without shimmering.
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    if (decodeArt(impl_->wic, bytes, 96, pixels, w, h)) {
      now_.art = std::move(pixels);
      now_.artW = w;
      now_.artH = h;
      ++now_.artVersion;
    }
    impl_->releaseArt();
  }
}

void MediaSession::playPause() {
  if (!impl_ || !impl_->session) return;
  IAsyncOperation* op = nullptr;
  if (now_.playing) {
    impl_->session->vtbl->TryPauseAsync(impl_->session, &op);
  } else {
    impl_->session->vtbl->TryPlayAsync(impl_->session, &op);
  }
  if (op) op->vtbl->base.Release(op);
}

void MediaSession::next() {
  if (!impl_ || !impl_->session) return;
  IAsyncOperation* op = nullptr;
  impl_->session->vtbl->TrySkipNextAsync(impl_->session, &op);
  if (op) op->vtbl->base.Release(op);
}

double NowPlaying::currentPositionSec() const {
  double pos = positionSec;

  // Only extrapolate while playing and while the anchor is plausible. A player
  // that reports an anchor in the future (clock skew) must not drag the bar
  // past the position it actually gave us.
  if (playing && anchorUnix > 0.0) {
    const double drift = nowAsFiletimeSeconds() - anchorUnix;
    if (drift > 0.0 && drift < 3600.0) pos += drift;
  }

  if (durationSec > 0.0 && pos > durationSec) pos = durationSec;
  return pos < 0.0 ? 0.0 : pos;
}

void MediaSession::seekTo(double seconds) {
  if (!impl_ || !impl_->session || seconds < 0.0) return;
  // The API takes 100 ns ticks since the start of the track.
  const LONGLONG ticks = static_cast<LONGLONG>(seconds * 10000000.0);
  IAsyncOperation* op = nullptr;
  impl_->session->vtbl->TryChangePlaybackPositionAsync(impl_->session, ticks, &op);
  if (op) op->vtbl->base.Release(op);

  // Move the local snapshot at once so the bar does not snap back to the old
  // spot on the next repaint while the player catches up.
  now_.positionSec = seconds;
  now_.anchorUnix = nowAsFiletimeSeconds();
}

void MediaSession::previous() {
  if (!impl_ || !impl_->session) return;
  IAsyncOperation* op = nullptr;
  impl_->session->vtbl->TrySkipPreviousAsync(impl_->session, &op);
  if (op) op->vtbl->base.Release(op);
}
