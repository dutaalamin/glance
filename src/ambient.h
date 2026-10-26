#pragma once

#include <d2d1.h>
#include <wincodec.h>

#include <string>
#include <vector>

// Ambient idle information: the things worth glancing at without opening
// anything - battery, and a count of unread messages per app.
//
// Unread counts come from window titles, not from the Windows notification
// centre. The notification centre API (UserNotificationListener) reports
// "Allowed" for an unpackaged process but its GetNotificationsAsync never
// completes - measured, ten seconds with no result - so it cannot be used from
// a plain exe. Titles are what the app itself decides to publish, which makes
// them both reliable and free of any privacy question.
struct AppBadge {
  std::wstring app;  // "WhatsApp"
  int count = 0;

  IWICBitmapSource* wic = nullptr;  // icon, DPI independent
  ID2D1Bitmap* icon = nullptr;      // per render target
  ID2D1RenderTarget* owner = nullptr;
};

struct Ambient {
  bool hasBattery = false;
  int batteryPct = 0;
  bool charging = false;
  bool acOnline = false;

  std::vector<AppBadge> badges;

  // Sum used by the caller to decide whether anything is worth drawing.
  int totalUnread() const {
    int sum = 0;
    for (const auto& b : badges) sum += b.count;
    return sum;
  }
};

class AmbientSource {
 public:
  bool init(IWICImagingFactory* wic);
  void release();

  // Cheap enough for the 2 Hz path: one GetSystemPowerStatus and one window
  // enumeration. Icons are only loaded when an app first shows a count.
  void poll();

  const Ambient& state() const { return ambient_; }

  // Rebuilds device-dependent icon bitmaps for the given target. Same pattern
  // as the launcher: WIC sources are kept, D2D bitmaps are rebuilt on demand.
  void syncIcons(ID2D1RenderTarget* rt);

 private:
  IWICImagingFactory* wic_ = nullptr;
  Ambient ambient_;
};
