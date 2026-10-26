#include "audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <algorithm>

struct AudioControl::Impl {
  IMMDeviceEnumerator* enumerator = nullptr;
};

bool AudioControl::init() {
  if (!impl_) impl_ = new Impl();

  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&impl_->enumerator))) ||
      !impl_->enumerator) {
    return false;
  }

  IMMDevice* device = nullptr;
  if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) ||
      !device) {
    return false;
  }

  IAudioEndpointVolume* volume = nullptr;
  const HRESULT hr =
      device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(&volume));
  device->Release();

  if (FAILED(hr) || !volume) return false;

  endpoint_ = volume;
  refresh();
  return true;
}

void AudioControl::release() {
  if (endpoint_) {
    static_cast<IAudioEndpointVolume*>(endpoint_)->Release();
    endpoint_ = nullptr;
  }
  if (impl_) {
    if (impl_->enumerator) impl_->enumerator->Release();
    delete impl_;
    impl_ = nullptr;
  }
}

void AudioControl::refresh() {
  auto* volume = static_cast<IAudioEndpointVolume*>(endpoint_);
  if (!volume) return;

  float scalar = 0.0f;
  if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar))) {
    level_ = std::clamp(scalar, 0.0f, 1.0f);
  }

  BOOL mute = FALSE;
  if (SUCCEEDED(volume->GetMute(&mute))) muted_ = mute != FALSE;
}

void AudioControl::setLevel(float level) {
  auto* volume = static_cast<IAudioEndpointVolume*>(endpoint_);
  if (!volume) return;

  const float clamped = std::clamp(level, 0.0f, 1.0f);
  if (FAILED(volume->SetMasterVolumeLevelScalar(clamped, nullptr))) return;

  level_ = clamped;

  // Turning the wheel up should also undo a mute, which is what the hardware
  // volume keys do.
  if (clamped > 0.0f && muted_) {
    if (SUCCEEDED(volume->SetMute(FALSE, nullptr))) muted_ = false;
  }
}

void AudioControl::changeLevel(float delta) {
  setLevel(level_ + delta);
}

void AudioControl::toggleMute() {
  auto* volume = static_cast<IAudioEndpointVolume*>(endpoint_);
  if (!volume) return;

  const BOOL next = muted_ ? FALSE : TRUE;
  if (SUCCEEDED(volume->SetMute(next, nullptr))) muted_ = next != FALSE;
}
