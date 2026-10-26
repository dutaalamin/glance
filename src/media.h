#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct IWICImagingFactory;

// Now-playing via the Windows System Media Transport Controls.
//
// This is the same public API the Windows volume overlay uses, so anything that
// shows up there (Spotify, browsers, Apple Music, VLC) shows up here too.
// Nothing private is read: only what the player chose to expose.
struct NowPlaying {
  bool hasSession = false;
  bool playing = false;
  std::wstring title;
  std::wstring artist;
  std::wstring album;
  double positionSec = 0.0;   // as of anchorUnix, straight from the player
  double durationSec = 0.0;

  // SMTC hands out Position together with LastUpdatedTime: the position is only
  // guaranteed correct at that instant. Players (Windows Media Player, browsers)
  // refresh it a few times a minute at best, so the raw value looks frozen.
  // Seconds since the Windows epoch (1601-01-01 UTC); 0 means "no anchor yet".
  double anchorUnix = 0.0;

  // Cover art, decoded once per track to a small BGRA (premultiplied) buffer.
  // Kept as pixels rather than a device bitmap because the panel draws to two
  // different render targets - the live window and the PNG snapshot.
  std::vector<uint32_t> art;
  int artW = 0;
  int artH = 0;
  uint32_t artVersion = 0;  // bumped whenever `art` changes, so renders can cache

  bool valid() const { return hasSession && !title.empty(); }

  // Where the track actually is right now. Extrapolates from the anchor while
  // playing, so the bar and the timer move smoothly between player updates.
  double currentPositionSec() const;
};

class MediaSession {
 public:
  // `wic` is used only to decode cover art; pass nullptr to skip art entirely.
  bool init(IWICImagingFactory* wic);
  void release();

  // Cheap: reads cached state, kicks off an async property fetch when the
  // track changes, and harvests it once ready. Safe to call at 1 Hz.
  void refresh();

  const NowPlaying& now() const { return now_; }

  void playPause();
  void next();
  void previous();

  // Jumps to `seconds` inside the current track. The player may refuse (a live
  // stream has no seek range), in which case nothing changes.
  void seekTo(double seconds);

 private:
  void pumpArt();

  struct Impl;
  Impl* impl_ = nullptr;
  NowPlaying now_;
};
