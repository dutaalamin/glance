#pragma once

#include <windows.h>

#include <cstdint>

#include <string>
#include <vector>

// Apps with recent Windows notifications.
//
// Source is the notification store itself:
//   %LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db
// because the documented API is unusable here - UserNotificationListener reports
// GetAccessStatus = Allowed for an unpackaged process but GetNotificationsAsync
// never completes. The database is an undocumented internal format, so every
// failure mode degrades to "no badges" rather than an error, and only two columns
// are read: which app, and when. Notification content is never touched.
//
// Opening it read-only is safe while the WPN service holds it: the store is in
// WAL mode, so readers do not block writers and recent notifications are visible.

struct NotifApp {
  std::wstring aumid;
  std::wstring label;  // friendly name when known, otherwise trimmed AUMID
  int count = 0;       // notifications inside the window
  uint64_t lastArrival = 0;  // FILETIME, 100 ns since 1601

  // Text carried by the toast payload, when the app supplies any. Plenty of
  // apps send images only (Netflix, Widgets), so both may be empty - the row
  // then falls back to the app name and a relative time.
  std::wstring title;
  std::wstring body;
};

class NotifSource {
 public:
  bool init();
  void release();

  // Re-queries the store. Cheap: one indexed GROUP BY over a few dozen rows.
  void poll();

  const std::vector<NotifApp>& apps() const { return apps_; }

  // How far back a notification still counts. Older ones are history: an icon
  // that never leaves stops meaning anything.
  void setWindowMinutes(int minutes) { windowMinutes_ = minutes; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::vector<NotifApp> apps_;
  int windowMinutes_ = 60;
};
