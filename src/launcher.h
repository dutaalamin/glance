#pragma once

#include <d2d1.h>
#include <wincodec.h>

#include <string>
#include <vector>

struct AppEntry {
  std::wstring name;
  std::wstring target;
  std::wstring args;

  IWICBitmapSource* wic = nullptr;  // cached, DPI-independent
  ID2D1Bitmap* icon = nullptr;      // created per render target
  ID2D1RenderTarget* owner = nullptr;
};

// A short, editable list of things worth one click from the island. Entries
// whose target cannot be found are dropped at load, so a machine without
// Windows Terminal simply shows one tile fewer instead of a dead button.
class Launcher {
 public:
  bool load(IWICImagingFactory* wic);
  void release();

  // Rebuilds D2D bitmaps when the render target changes (resize, DPI change,
  // device loss). Cheap: icons are only re-created when actually stale.
  void syncIcons(ID2D1RenderTarget* rt);

  const std::vector<AppEntry>& apps() const { return apps_; }
  bool launch(size_t index) const;

 private:
  std::vector<AppEntry> apps_;
  IWICImagingFactory* wic_ = nullptr;
};
