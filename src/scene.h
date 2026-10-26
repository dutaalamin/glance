#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include "ambient.h"
#include "launcher.h"
#include "media.h"
#include "telemetry.h"

// One window, anchored to the top centre of the work area.
//
// Resting: a notch the size of a phone's Dynamic Island - just the clock.
// Hovering: it grows downward into a panel with the launcher and metrics.
// The window never moves sideways; it expands around its own centre line.
namespace layout {
constexpr float kNotchW = 236.0f;
constexpr float kNotchH = 40.0f;
constexpr float kPanelW = 452.0f;
constexpr float kPanelH = 318.0f;
constexpr float kTopMargin = 10.0f;
}  // namespace layout

enum class PanelMode { Compact, Expanded };

class Scene {
 public:
  // Height that exactly fits the launcher grid (and the now-playing card when
  // something is playing), so the panel never shows a pool of empty space.
  static float panelHeightFor(size_t appCount, bool withMedia);

  bool init(IDWriteFactory* dwrite, ID2D1Factory* d2d, Launcher* launcher);
  void draw(ID2D1RenderTarget* rt, const Snapshot& snap, PanelMode mode, float width,
            float height);

  // Returns the launcher tile under the point, or -1. Only meaningful while
  // expanded; call after a draw so the tile rects are current.
  int hitTestLauncher(float x, float y, PanelMode mode) const;

  // Master volume, drawn as a slider row under the grid. Fed from AudioControl
  // once per second by the paint loop.
  // Now-playing card above the grid. Fed once per second from MediaSession.
  void setNowPlaying(const NowPlaying& np) { media_ = np; }
  const NowPlaying& nowPlaying() const { return media_; }

  // 0 = previous, 1 = play/pause, 2 = next, -1 = nothing. Only valid while
  // expanded and only after a draw.
  int hitTestMedia(float x, float y, PanelMode mode) const;

  // Seek bar and volume slider. The bar is generous vertically (a 5 px track
  // with padding) so it can actually be grabbed with a mouse.
  bool hitTestSeek(float x, float y, PanelMode mode) const;
  bool hitTestVolumeSlider(float x, float y, PanelMode mode) const;
  // Position of x inside the track, clamped to 0..1.
  float fractionFromX(float x, float seekBar) const;

  void setDragging(bool dragging) { dragging_ = dragging; }

  // Cover art for the media card. Built lazily per render target because the
  // panel paints to both an HDC target and a WIC target for snapshots.
  ID2D1Bitmap* artBitmap(ID2D1RenderTarget* rt);

  // Drops the cached cover bitmap. Device-dependent resources belong to the
  // render target that created them, so this MUST be called before that target
  // is released - otherwise the cache check can match a dangling pointer.
  void discardArtBitmap();

  // Idle-strip extras: battery and unread badges. `source` must outlive the
  // scene and must already have had syncIcons() called for the active target.
  void setAmbient(const Ambient* source) { ambient_ = source; }

  void setVolume(float level, bool muted, bool available) {
    volume_ = level;
    muted_ = muted;
    volumeAvailable_ = available;
  }

  // Highlighted tile, -1 for none. Set from WM_MOUSEMOVE so hovering a tile
  // reads as "this will open" before the click.
  void setHoveredTile(int index) { hoveredTile_ = index; }
  int hoveredTile() const { return hoveredTile_; }

  void release();

 private:
  void drawNotch(ID2D1RenderTarget* rt, const Snapshot& s, float w, float h, bool expanded);

  // Width of `text` in `format`, for centring a group of text plus icons.
  // Returns 0 when the factory is unavailable.
  float measureText(const wchar_t* text, IDWriteTextFormat* format);
  void drawPanel(ID2D1RenderTarget* rt, const Snapshot& s, float w, float h, float top);
  void drawLauncherGrid(ID2D1RenderTarget* rt, float x, float y, float w);
  float drawMediaCard(ID2D1RenderTarget* rt, float x, float y, float w,
                      const Snapshot& snap);

  ID2D1Factory* d2d_ = nullptr;
  IDWriteFactory* dwrite_ = nullptr;  // only for measuring the clock in the notch
  Launcher* launcher_ = nullptr;

  IDWriteTextFormat* time_ = nullptr;
  IDWriteTextFormat* timeNotch_ = nullptr;
  IDWriteTextFormat* date_ = nullptr;
  IDWriteTextFormat* label_ = nullptr;
  IDWriteTextFormat* value_ = nullptr;
  IDWriteTextFormat* small_ = nullptr;
  IDWriteTextFormat* row_ = nullptr;
  IDWriteTextFormat* rowRight_ = nullptr;
  IDWriteTextFormat* tile_ = nullptr;
  IDWriteTextFormat* tiny_ = nullptr;

  ID2D1Bitmap* artBitmap_ = nullptr;
  uint32_t artBuiltVersion_ = 0;
  ID2D1RenderTarget* artTarget_ = nullptr;

  // Tile rects from the last expanded paint, in DIPs.
  D2D1_RECT_F tileRects_[12]{};
  int tileCount_ = 0;
  int hoveredTile_ = -1;

  const Ambient* ambient_ = nullptr;
  NowPlaying media_;
  D2D1_RECT_F mediaButtons_[3]{};
  bool mediaButtonsValid_ = false;
  D2D1_RECT_F progressRect_{};
  D2D1_RECT_F volumeRect_{};
  bool dragging_ = false;

  float volume_ = 0.0f;
  bool muted_ = false;
  bool volumeAvailable_ = false;
};
