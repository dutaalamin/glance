#pragma once

// Master output volume through WASAPI. Polling rather than change callbacks:
// the panel samples once a second anyway, and a callback would need a COM
// object with a lifetime tied to the audio device.
class AudioControl {
 public:
  bool init();
  void release();

  void refresh();

  float level() const { return level_; }
  bool muted() const { return muted_; }
  bool available() const { return endpoint_ != nullptr; }

  void setLevel(float level);
  void changeLevel(float delta);
  void toggleMute();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  void* endpoint_ = nullptr;  // IAudioEndpointVolume*
  float level_ = 0.0f;
  bool muted_ = false;
};
