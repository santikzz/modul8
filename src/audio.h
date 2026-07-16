#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <RtAudio.h>

#include "effect.h"

struct AudioDevice {
  unsigned int id;
  std::string name;
  unsigned int channels;
  bool isDefault;
};

// owns the rtaudio stream and the effect chain. keeps all realtime/audio
// concerns out of main and the ui, so imgui can drive it later.
class AudioEngine {
public:
  ~AudioEngine();

  bool hasDevices() { return !audio.getDeviceIds().empty(); }
  std::vector<AudioDevice> inputDevices() { return devices(true); }
  std::vector<AudioDevice> outputDevices() { return devices(false); }

  // builds the chain from effect names; unknown names go into unknown, in order.
  void setChain(const std::vector<std::string>& names,
                std::vector<std::string>& unknown);

  bool start(unsigned int inId, unsigned int outId, unsigned int sampleRate = 48000,
             unsigned int bufferFrames = 256);
  void stop();

  unsigned int sampleRate() const { return sampleRate_; }
  unsigned int bufferFrames() const { return bufferFrames_; }
  unsigned int xruns() const { return state.xruns.load(); }
  std::vector<std::string> chainNames() const;

private:
  struct State {
    std::vector<std::unique_ptr<Effect>> chain;
    std::vector<float> mono;
    unsigned int inChannels = 1;
    unsigned int outChannels = 2;
    std::atomic<unsigned int> xruns{0};
  };

  static int callback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                      double streamTime, RtAudioStreamStatus status, void* userData);

  std::vector<AudioDevice> devices(bool wantInput);

  RtAudio audio;
  State state;
  unsigned int sampleRate_ = 48000;
  unsigned int bufferFrames_ = 256;
};
