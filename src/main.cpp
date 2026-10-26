#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "ambient.h"
#include "notifs.h"
#include "audio.h"

#include "launcher.h"
#include "media.h"
#include "scene.h"
#include "telemetry.h"

namespace {

constexpr UINT_PTR kSampleTimer = 1;
constexpr UINT_PTR kAnimTimer = 2;
constexpr UINT_PTR kCollapseTimer = 4;

#define WM_TRAYICON (WM_APP + 1)
#define WM_SHOW_FROM_SECOND_INSTANCE (WM_APP + 2)
#define IDM_SHOW 40001
#define IDM_EXIT 40002
#define IDM_TOGGLE 40003
constexpr UINT kCollapseDelayMs = 300;

constexpr UINT kSampleIntervalMs = 1000;
constexpr UINT kAnimIntervalMs = 15;

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IWICImagingFactory* g_wic = nullptr;
Scene* g_scene = nullptr;
Telemetry* g_telemetry = nullptr;
Launcher* g_launcher = nullptr;
AudioControl* g_audio = nullptr;
MediaSession* g_media = nullptr;
AmbientSource* g_ambient = nullptr;
NotifSource* g_notifs = nullptr;

// Which slider the mouse is currently holding. Dragging is tracked here rather
// than in Scene so the scene stays a pure renderer.
enum class DragTarget { None, Seek, Volume };
DragTarget g_drag = DragTarget::None;

ID2D1HwndRenderTarget* g_renderTarget = nullptr;
HWND g_hwnd = nullptr;
UINT g_dpi = 96;

PanelMode g_mode = PanelMode::Compact;
float g_w = layout::kNotchW;
float g_h = layout::kNotchH;

uint64_t g_lastSignature = 0;
unsigned long long g_samples = 0;
unsigned long long g_draws = 0;



NOTIFYICONDATAW g_tray{};
bool g_trayReady = false;
bool g_hidden = false;

void showTrayMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  const wchar_t* toggle = g_hidden ? L"Show island" : L"Hide island";
  AppendMenuW(menu, MF_STRING, IDM_TOGGLE, toggle);
  SetMenuDefaultItem(menu, IDM_TOGGLE, FALSE);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit Glance");

  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
}

void setHidden(bool hidden) {
  g_hidden = hidden;
  ShowWindow(g_hwnd, hidden ? SW_HIDE : SW_SHOW);
  if (!hidden) {
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

bool initTray(HWND hwnd, HINSTANCE instance) {
  g_tray = {};
  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = hwnd;
  g_tray.uID = 1;
  g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray.uCallbackMessage = WM_TRAYICON;
  g_tray.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
  if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  wcscpy_s(g_tray.szTip, L"Glance island");

  g_trayReady = Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;
  return g_trayReady;
}

void removeTray() {
  if (!g_trayReady) return;
  Shell_NotifyIconW(NIM_DELETE, &g_tray);
  g_trayReady = false;
}

float dipsToPixels(float dips) { return dips * static_cast<float>(g_dpi) / 96.0f; }
float pixelsToDips(int px) { return static_cast<float>(px) * 96.0f / static_cast<float>(g_dpi); }

float targetWidth() {
  return g_mode == PanelMode::Compact ? layout::kNotchW : layout::kPanelW;
}
float targetHeight() {
  if (g_mode == PanelMode::Compact) return layout::kNotchH;
  const size_t apps = g_launcher ? g_launcher->apps().size() : 0;
  const bool withMedia = g_media && g_media->now().hasSession;
  const bool folderOnly = g_scene && !g_scene->folderRevealed();
  return Scene::panelHeightFor(apps, withMedia,
                               g_scene ? g_scene->notificationRowCount() : 0, folderOnly);
}

void discardRenderTarget() {
  // The cover bitmap was created by this target, so it dies with it. Freeing
  // the target first would leave Scene holding a pointer to released memory,
  // and a fresh target can easily land on the same address.
  if (g_scene) g_scene->discardArtBitmap();

  if (g_renderTarget) {
    g_renderTarget->Release();
    g_renderTarget = nullptr;
  }
}

bool ensureRenderTarget() {
  if (g_renderTarget) return true;

  RECT rc{};
  GetClientRect(g_hwnd, &rc);

  // Software rendering on purpose. A hardware context pulls in the full GPU
  // driver stack (nvgpucomp64 + nvwgf2umx, ~180 MB of address space) to draw a
  // 390 px panel a few times a second. On the CPU it is imperceptible and the
  // process stays small.
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
      D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(static_cast<UINT>(rc.right - rc.left),
                                                           static_cast<UINT>(rc.bottom - rc.top)));

  if (FAILED(g_d2d->CreateHwndRenderTarget(props, hwndProps, &g_renderTarget))) return false;
  g_renderTarget->SetDpi(static_cast<float>(g_dpi), static_cast<float>(g_dpi));
  return true;
}

void applyWindowSize() {
  const int w = static_cast<int>(dipsToPixels(g_w));
  const int h = static_cast<int>(dipsToPixels(g_h));

  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

  RECT current{};
  GetWindowRect(g_hwnd, &current);

  // Horizontally centred over the work area, hanging from the top edge. The
  // notch grows around its own centre line rather than to one side.
  const int centre = work.left + (work.right - work.left) / 2;
  const int x = centre - w / 2;
  const int y = current.top;

  SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

  if (g_renderTarget) {
    g_renderTarget->Resize(D2D1::SizeU(static_cast<UINT>(w), static_cast<UINT>(h)));
  }
}

void syncVolume() {
  if (!g_audio || !g_scene) return;
  g_audio->refresh();
  g_scene->setVolume(g_audio->level(), g_audio->muted(), g_audio->available());
}

// Called on the 1 Hz sample timer. Also notices when a track starts or stops so
// the panel can grow/shrink without the user touching it.
void applyDrag(int px) {
  if (!g_scene) return;

  const float dx = pixelsToDips(px);

  if (g_drag == DragTarget::Seek && g_media && g_scene->nowPlaying().hasSession) {
    const double dur = g_scene->nowPlaying().durationSec;
    if (dur > 0.5) {
      const double target = static_cast<double>(g_scene->fractionFromX(dx, 1.0f)) * dur;
      g_media->seekTo(target);
      g_scene->setNowPlaying(g_media->now());
      InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    return;
  }

  if (g_drag == DragTarget::Volume && g_audio && g_audio->available()) {
    g_audio->setLevel(g_scene->fractionFromX(dx, 0.0f));
    syncVolume();
    InvalidateRect(g_hwnd, nullptr, FALSE);
  }
}

void syncMedia() {
  if (!g_media || !g_scene) return;
  // While scrubbing, the sampled position is older than the drag. Letting the
  // timer overwrite it makes the knob jump backwards every second.
  if (g_drag == DragTarget::Seek) return;

  const bool hadSession = g_scene->nowPlaying().hasSession;
  const std::wstring before = g_scene->nowPlaying().title;
  const bool wasPlaying = g_scene->nowPlaying().playing;

  g_media->refresh();
  g_scene->setNowPlaying(g_media->now());

  const bool hasSession = g_scene->nowPlaying().hasSession;
  const bool isPlaying = g_scene->nowPlaying().playing;
  const std::wstring after = g_scene->nowPlaying().title;

  if (hadSession != hasSession || before != after || wasPlaying != isPlaying) {
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_mode == PanelMode::Expanded) SetTimer(g_hwnd, kAnimTimer, kAnimIntervalMs, nullptr);
  }
}

void animate() {
  const float tw = targetWidth();
  const float th = targetHeight();

  const float dw = tw - g_w;
  const float dh = th - g_h;

  if (std::fabs(dw) < 0.8f && std::fabs(dh) < 0.8f) {
    g_w = tw;
    g_h = th;
    applyWindowSize();
    KillTimer(g_hwnd, kAnimTimer);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return;
  }

  g_w += dw * 0.3f;
  g_h += dh * 0.3f;
  applyWindowSize();

  if (ensureRenderTarget()) {
    g_renderTarget->BeginDraw();
    if (g_ambient) {
      g_ambient->syncIcons(g_renderTarget);
      g_scene->setAmbient(&g_ambient->state());
    }
    g_scene->draw(g_renderTarget, g_telemetry->snap(), g_mode, g_w, g_h);
    if (g_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) discardRenderTarget();
  }
}

bool cursorInsideWindow() {
  POINT pt{};
  if (!GetCursorPos(&pt)) return false;

  RECT rc{};
  if (!GetWindowRect(g_hwnd, &rc)) return false;

  return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

void setMode(PanelMode mode) {
  if (g_mode == mode) return;
  g_mode = mode;
  SetTimer(g_hwnd, kAnimTimer, kAnimIntervalMs, nullptr);

  if (g_telemetry) g_telemetry->setHeavyMode(mode == PanelMode::Expanded);

  // Every hover starts from the folder, so the panel always opens the same size.
  if (g_scene && mode == PanelMode::Compact) g_scene->setFolderRevealed(false);
}


void reportStats() {
  wchar_t msg[256]{};
  swprintf(msg, 256, L"glance: %llu samples, %llu paints, %llu skipped\n", g_samples, g_draws,
           g_samples > g_draws ? g_samples - g_draws : 0);
  OutputDebugStringW(msg);
  fwprintf(stderr, L"%ls", msg);
  fflush(stderr);
}

void attachConsole() {
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  freopen("CONOUT$", "w", stderr);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_TIMER:
      if (wParam == kSampleTimer && g_telemetry) {
        g_telemetry->sample();
        // Volume and the track can change from outside this app - hardware keys,
        // the Windows overlay, the player itself. Without polling them the panel
        // shows whatever it last wrote, which reads as a slider that does nothing.
        syncVolume();
        syncMedia();
        if (g_ambient) g_ambient->poll();
        ++g_samples;
        const uint64_t sig = g_telemetry->signature();
        if (sig != g_lastSignature) {
          g_lastSignature = sig;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      } else if (wParam == kAnimTimer) {
        animate();
      } else if (wParam == kCollapseTimer) {
        if (!cursorInsideWindow()) {
          g_scene->setHoveredTile(-1);
          setMode(PanelMode::Compact);
        }
        KillTimer(hwnd, kCollapseTimer);
      }
      return 0;

    case WM_MOUSEMOVE: {
      KillTimer(hwnd, kCollapseTimer);
      if (g_mode == PanelMode::Compact) setMode(PanelMode::Expanded);

      if (g_drag != DragTarget::None) {
        applyDrag(GET_X_LPARAM(lParam));
        return 0;
      }

      if (g_scene) {
        const float hx = pixelsToDips(GET_X_LPARAM(lParam));
        const float hy = pixelsToDips(GET_Y_LPARAM(lParam));
        const int index = g_scene->hitTestLauncher(hx, hy, g_mode);
        const bool hotMore = g_scene->hitTestFolder(hx, hy, g_mode);
        if (index != g_scene->hoveredTile() || hotMore != g_scene->hoveredSeeMore()) {
          g_scene->setHoveredTile(index);
          g_scene->setHoveredSeeMore(hotMore);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }

      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd;
      TrackMouseEvent(&tme);
      return 0;
    }

    case WM_MOUSELEAVE:
      // Resizing the window during the expand animation makes Windows fire
      // WM_MOUSELEAVE even though the pointer is still on the panel. Collapsing
      // immediately caused the panel to flicker open/closed, so the decision is
      // deferred and re-checked against the real cursor position.
      SetTimer(hwnd, kCollapseTimer, kCollapseDelayMs, nullptr);
      return 0;

    case WM_LBUTTONDBLCLK: {
      // Double-click hides, but only on empty space. A double-click that lands
      // on a tile is two launches, not a hide gesture.
      bool overTile = false;
      if (g_mode == PanelMode::Expanded && g_scene) {
        const float dx = pixelsToDips(GET_X_LPARAM(lParam));
        const float dy = pixelsToDips(GET_Y_LPARAM(lParam));
        // The folder button counts as an interactive target too. Without this,
        // revealing the grid and clicking again in the same spot arrives as a
        // double-click and hides the island instead of opening anything.
        overTile = g_scene->hitTestLauncher(dx, dy, g_mode) >= 0 ||
                   g_scene->hitTestFolder(dx, dy, g_mode);
      }
      if (!overTile) setHidden(true);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      if (g_mode != PanelMode::Expanded || !g_scene) return 0;

      const float dx = pixelsToDips(GET_X_LPARAM(lParam));
      const float dy = pixelsToDips(GET_Y_LPARAM(lParam));

      if (g_scene->hitTestSeek(dx, dy, g_mode)) {
        g_drag = DragTarget::Seek;
        g_scene->setDragging(true);
        SetCapture(hwnd);
        applyDrag(GET_X_LPARAM(lParam));
        return 0;
      }
      if (g_scene->hitTestVolumeSlider(dx, dy, g_mode)) {
        g_drag = DragTarget::Volume;
        g_scene->setDragging(true);
        SetCapture(hwnd);
        applyDrag(GET_X_LPARAM(lParam));
        return 0;
      }
      return 0;
    }

    case WM_LBUTTONUP: {
      if (g_drag != DragTarget::None) {
        applyDrag(GET_X_LPARAM(lParam));
        g_drag = DragTarget::None;
        if (g_scene) g_scene->setDragging(false);
        ReleaseCapture();
        // Let the player settle, then re-read so the knob lands where it landed.
        Sleep(120);
        syncMedia();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      if (g_mode == PanelMode::Expanded && g_scene && g_launcher) {
        const int px = GET_X_LPARAM(lParam);

        if (g_scene->hitTestFolder(pixelsToDips(px), pixelsToDips(GET_Y_LPARAM(lParam)),
                                   g_mode)) {
          g_scene->setFolderRevealed(true);
          g_scene->setHoveredSeeMore(false);

          // Snap straight to the new height instead of animating. The animated
          // path resizes the window repeatedly, and each resize fires
          // WM_MOUSELEAVE - which schedules a collapse, and the collapse resets
          // the reveal. One instant resize has neither problem.
          KillTimer(hwnd, kCollapseTimer);
          g_h = targetHeight();
          g_w = targetWidth();
          applyWindowSize();
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        const int py = GET_Y_LPARAM(lParam);
        const float dx = pixelsToDips(px);
        const float dy = pixelsToDips(py);

        const int media = g_scene->hitTestMedia(dx, dy, g_mode);
        if (media >= 0 && g_media) {
          if (media == 0) g_media->previous();
          else if (media == 1) g_media->playPause();
          else g_media->next();
          // Give the player a beat to change state, then re-read.
          Sleep(120);
          syncMedia();
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }

        const int index = g_scene->hitTestLauncher(dx, dy, g_mode);
        if (index >= 0) {
          g_launcher->launch(static_cast<size_t>(index));
          // A freshly opened window steals the foreground; re-assert topmost so
          // the notch does not end up buried behind it.
          SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
      }
      return 0;
    }

    case WM_MOUSEWHEEL: {
      if (!g_audio || !g_audio->available()) return 0;

      const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      // 5% per notch: fine enough to land on a comfortable level, coarse enough
      // that a few flicks cross the whole range.
      g_audio->changeLevel(delta > 0 ? 0.05f : -0.05f);
      syncVolume();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_CAPTURECHANGED:
      // Capture can be taken away (Alt+Tab, another window stealing it, a
      // shell overlay). Without this the drag target stayed set, and that flag
      // gates the 1 Hz media refresh - the card froze until restart.
      if (g_drag != DragTarget::None) {
        g_drag = DragTarget::None;
        if (g_scene) g_scene->setDragging(false);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    case WM_MBUTTONUP:
      // Middle-click toggles mute, matching common media-key behaviour.
      if (g_audio && g_audio->available()) {
        g_audio->toggleMute();
        syncVolume();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    case WM_RBUTTONUP:
      // Right-click anywhere on the island tucks it away; it comes back from
      // the tray icon.
      setHidden(true);
      return 0;

    case WM_SHOW_FROM_SECOND_INSTANCE:
      // A second launch is treated as "bring the island back", not as a new
      // process. Two copies would mean two tray icons.
      setHidden(false);
      SetForegroundWindow(hwnd);
      return 0;

    case WM_TRAYICON:
      if (LOWORD(lParam) == WM_LBUTTONUP) {
        setHidden(!g_hidden);
      } else if (LOWORD(lParam) == WM_RBUTTONUP) {
        showTrayMenu(hwnd);
      }
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDM_SHOW:
          setHidden(false);
          return 0;
        case IDM_TOGGLE:
          setHidden(!g_hidden);
          return 0;
        case IDM_EXIT:
          DestroyWindow(hwnd);
          return 0;
        default:
          break;
      }
      return 0;

    case WM_CLOSE:
      // Closing the island hides it; the process stays alive in the tray so it
      // can be brought back with a single click.
      setHidden(true);
      return 0;

    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd, &ps);

      if (ensureRenderTarget() && g_telemetry) {
        syncVolume();
        g_renderTarget->BeginDraw();
        if (g_ambient) {
      g_ambient->syncIcons(g_renderTarget);
      g_scene->setAmbient(&g_ambient->state());
    }
    g_scene->draw(g_renderTarget, g_telemetry->snap(), g_mode, g_w, g_h);

        if (g_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
          discardRenderTarget();
        } else {
          ++g_draws;
        }
      }

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_DISPLAYCHANGE:
      applyWindowSize();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_DESTROY:
      reportStats();
      KillTimer(hwnd, kSampleTimer);
      KillTimer(hwnd, kAnimTimer);
      KillTimer(hwnd, kCollapseTimer);
      removeTray();
      discardRenderTarget();
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void enableDpiAwareness() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) return;

  using SetCtxFn = BOOL(WINAPI*)(HANDLE);
  auto setCtx =
      reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (setCtx && setCtx(reinterpret_cast<HANDLE>(-4))) return;

  using SetFn = BOOL(WINAPI*)();
  auto setAware = reinterpret_cast<SetFn>(GetProcAddress(user32, "SetProcessDPIAware"));
  if (setAware) setAware();
}

void roundWindowCorners(HWND hwnd) {
  // Windows 11 only; ignored elsewhere.
  enum : DWORD { kCornerPreference = 33 };
  const int round = 2;
  DwmSetWindowAttribute(hwnd, kCornerPreference, &round, sizeof(round));
}

bool createPanelWindow(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  // CS_DBLCLKS is required for WM_LBUTTONDBLCLK to ever arrive.
  wc.style = CS_DROPSHADOW | CS_DBLCLKS;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"GlanceNotchWindow";

  if (!RegisterClassExW(&wc)) return false;

  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

  const int w = static_cast<int>(dipsToPixels(layout::kNotchW));
  const int h = static_cast<int>(dipsToPixels(layout::kNotchH));
  const int top = work.top + static_cast<int>(dipsToPixels(layout::kTopMargin));
  const int centre = work.left + (work.right - work.left) / 2;

  g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Glance",
                           WS_POPUP, centre - w / 2, top, w, h, nullptr, nullptr, instance,
                           nullptr);
  if (!g_hwnd) return false;

  roundWindowCorners(g_hwnd);
  initTray(g_hwnd, instance);
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);

  SetTimer(g_hwnd, kSampleTimer, kSampleIntervalMs, nullptr);
  return true;
}

bool savePng(const wchar_t* path, const Snapshot& snap, PanelMode mode, float scale) {
  // Sample audio and the media session first: both feed the layout, and the
  // panel height depends on whether a player is active.
  if (g_audio) {
    g_audio->refresh();
    g_scene->setVolume(g_audio->level(), g_audio->muted(), g_audio->available());
  }
  if (g_media) {
    // Manager handle, track properties and the cover art all arrive through
    // separate async chains, so keep ticking until the card is fully populated
    // (or give up after ~3 s, which is what an idle machine will do).
    for (int i = 0; i < 30; ++i) {
      g_media->refresh();
      const NowPlaying& np = g_media->now();
      if (np.hasSession && !np.title.empty() && !np.art.empty()) break;
      if (np.hasSession && !np.title.empty() && i > 26) break;  // no art offered

      Sleep(100);
    }
    g_scene->setNowPlaying(g_media->now());
  }

  if (g_ambient) {
    g_ambient->poll();
    g_scene->setAmbient(&g_ambient->state());
    g_ambient->syncIcons(nullptr);
  }

  const float lw = mode == PanelMode::Compact ? layout::kNotchW : layout::kPanelW;
  const size_t apps = g_launcher ? g_launcher->apps().size() : 0;
  const float lh =
      mode == PanelMode::Compact ? layout::kNotchH
                                 : Scene::panelHeightFor(apps, g_media && g_media->now().hasSession,
                                                         g_scene->notificationRowCount(),
                                                         !g_scene->folderRevealed());

  const UINT width = static_cast<UINT>(lw * scale);
  const UINT height = static_cast<UINT>(lh * scale);

  IWICBitmap* bitmap = nullptr;
  if (FAILED(g_wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &bitmap))) {
    return false;
  }

  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * scale,
      96.0f * scale);

  ID2D1RenderTarget* rt = nullptr;
  if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bitmap, props, &rt))) {
    bitmap->Release();
    return false;
  }

  rt->BeginDraw();
  if (g_ambient) {
    g_ambient->syncIcons(rt);
    g_scene->setAmbient(&g_ambient->state());
  }
  g_scene->draw(rt, snap, mode, lw, lh);
  rt->EndDraw();

  bool ok = false;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;

  if (SUCCEEDED(g_wic->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
      SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
      SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
      SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) &&
      SUCCEEDED(frame->WriteSource(bitmap, nullptr)) && SUCCEEDED(frame->Commit()) &&
      SUCCEEDED(encoder->Commit())) {
    ok = true;
  }

  if (frame) frame->Release();
  if (encoder) encoder->Release();
  // Same rule as discardRenderTarget: the cached cover bitmap belongs to this
  // target, so drop it before the target goes away.
  g_scene->discardArtBitmap();

  if (stream) stream->Release();
  rt->Release();
  bitmap->Release();
  return ok;
}

int runSnapshot(const wchar_t* path, PanelMode mode) {
  g_telemetry->sample();
  Sleep(900);
  g_telemetry->sample();

  const bool ok = savePng(path, g_telemetry->snap(), mode, 2.0f);
  fwprintf(stderr, L"glance: snapshot %ls\n", ok ? L"written" : L"failed");
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  attachConsole();

  // Only one island may exist. A second launch hands the request to the running
  // instance (it shows itself) and exits, so the tray never shows a duplicate.
  HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\GlanceIslandSingleton");
  const bool alreadyRunning = instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS;

  if (alreadyRunning && argc < 2) {
    if (HWND existing = FindWindowW(L"GlanceNotchWindow", L"Glance")) {
      PostMessageW(existing, WM_SHOW_FROM_SECOND_INSTANCE, 0, 0);
    }
    if (instanceMutex) CloseHandle(instanceMutex);
    return 0;
  }

  // MULTITHREADED on purpose: the SMTC cover-art chain is four nested WinRT
  // async operations (open stream -> read -> decode). In a single-threaded
  // apartment those completions are only delivered while this thread pumps
  // messages, which is true for the live window but not for `--snapshot`, so
  // the art silently never arrived. In the MTA they complete on a thread-pool
  // thread and need no pump. Nothing here uses STA-only features.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return 1;

  static const IID kIDWriteFactory = {
      0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIDWriteFactory,
                                 reinterpret_cast<IUnknown**>(&g_dwrite)))) {
    return 1;
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&g_wic)))) {
    return 1;
  }

  g_launcher = new Launcher();
  if (!g_launcher->load(g_wic)) {
    fwprintf(stderr, L"glance: no launcher targets resolved\n");
  }

  g_scene = new Scene();
  if (!g_scene->init(g_dwrite, g_d2d, g_launcher)) return 1;

  g_telemetry = new Telemetry();
  if (!g_telemetry->init()) return 1;

  g_notifs = new NotifSource();
  if (!g_notifs->init()) {
    fwprintf(stderr, L"glance: notification store unavailable\n");
  }

  g_ambient = new AmbientSource();
  if (!g_ambient->init(g_wic, g_notifs)) {
    fwprintf(stderr, L"glance: ambient info unavailable\n");
  }

  g_media = new MediaSession();
  if (!g_media->init(g_wic)) {
    fwprintf(stderr, L"glance: media session unavailable\n");
  }

  g_audio = new AudioControl();
  if (!g_audio->init()) {
    // Not fatal: the panel still works, the volume row shows "--".
    fwprintf(stderr, L"glance: audio endpoint unavailable\n");
  }

  if (argc >= 3 && std::string(argv[1]) == "--snapshot") {
    std::wstring path;
    const char* raw = argv[2];
    int len = MultiByteToWideChar(CP_UTF8, 0, raw, -1, nullptr, 0);
    path.resize(static_cast<size_t>(len));
    MultiByteToWideChar(CP_UTF8, 0, raw, -1, path.data(), len);
    path.pop_back();

    const bool expanded = (argc >= 4 && std::string(argv[3]) == "expanded");
    const int rc = runSnapshot(path.c_str(), expanded ? PanelMode::Expanded : PanelMode::Compact);

    g_launcher->release();
    delete g_launcher;
    g_scene->release();
    delete g_scene;
    if (g_audio) {
      g_audio->release();
      delete g_audio;
    }
    if (g_media) {
      g_media->release();
      delete g_media;
    }
    if (g_ambient) {
      g_ambient->release();
      delete g_ambient;
    }
    if (g_notifs) {
      g_notifs->release();
      delete g_notifs;
    }
    delete g_telemetry;
    if (g_wic) g_wic->Release();
    if (g_dwrite) g_dwrite->Release();
    if (g_d2d) g_d2d->Release();
    return rc;
  }

  enableDpiAwareness();

  HINSTANCE instance = GetModuleHandleW(nullptr);
  g_dpi = GetDpiForSystem();

  if (!createPanelWindow(instance)) return 1;

  g_telemetry->sample();
  g_lastSignature = g_telemetry->signature();
  InvalidateRect(g_hwnd, nullptr, FALSE);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  g_launcher->release();
  delete g_launcher;
  g_scene->release();
  delete g_scene;
  if (g_audio) {
    g_audio->release();
    delete g_audio;
  }
  if (g_media) {
    g_media->release();
    delete g_media;
  }
  if (g_ambient) {
    g_ambient->release();
    delete g_ambient;
  }
  if (g_notifs) {
    g_notifs->release();
    delete g_notifs;
  }
  delete g_telemetry;
  if (g_wic) g_wic->Release();
  if (g_dwrite) g_dwrite->Release();
  if (g_d2d) g_d2d->Release();
  if (instanceMutex) CloseHandle(instanceMutex);
  return 0;
}
