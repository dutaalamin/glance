#pragma once

#include <string>

// A tiny INI next to the user's profile. Deliberately not in the exe folder: the
// exe may live on a read-only or synced path, and these are machine preferences.
namespace settings {

struct Values {
  // Whether the app launcher (grid, quick row, See more) is drawn at all. Off
  // turns the panel into an ambient card: date, battery, volume, notifications
  // and the now-playing card.
  bool launcher = false;
};

Values load();
std::wstring path();

}  // namespace settings
