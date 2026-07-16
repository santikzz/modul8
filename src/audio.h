#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <RtAudio.h>

#include "effect.h"

class Graph;

struct AudioDevice {
  unsigned int id;
  std::string name;
  unsigned int channels;
  bool isDefault;
};

// one node of the flattened, audio-thread-ready graph. buffers are indices into
// RenderPlan::buffers; the node sums its inputs into outBuf, then runs fx in place.
struct RenderStep {
  Effect* fx;                 // null for the io / passthrough nodes
  std::atomic<bool>* bypass;  // null = never bypassed
  std::vector<int> inputs;    // buffer indices summed into outBuf
  int outBuf;
};

// immutable render form of a Graph, built on the ui thread and swapped onto the
// audio thread atomically. owns its scratch buffers; the effects live in the Graph.
struct RenderPlan {
  std::vector<RenderStep> steps;            // topological order
  std::vector<std::vector<float>> buffers;  // one per node
  int inputBuf = -1;                        // callback writes mic here
  int outputBuf = -1;                       // callback reads speakers here
};

// owns the rtaudio stream and the active render plan. the plan can be hot-swapped
// while streaming: swapPlan() exchanges the pointer and, using a hazard handshake,
// waits until the audio thread is no longer inside the old plan before freeing it.
// effect creation, prepare() and destruction all stay on the calling (ui) thread.
class AudioEngine {
public:
  ~AudioEngine();

  bool hasDevices() { return !audio.getDeviceIds().empty(); }
  std::vector<AudioDevice> inputDevices() { return devices(true); }
  std::vector<AudioDevice> outputDevices() { return devices(false); }

  bool start(unsigned int inId, unsigned int outId, Graph& graph,
             unsigned int sampleRate = 48000, unsigned int bufferFrames = 256);
  void stop();

  // hand a freshly built plan to the audio thread; frees the previous one safely.
  void swapPlan(std::unique_ptr<RenderPlan> plan);

  // live mixer controls, read on the audio thread.
  void setInputGain(float g) { inGain_.store(g, std::memory_order_relaxed); }
  void setOutputGain(float g) { outGain_.store(g, std::memory_order_relaxed); }
  void setBypass(bool b) { bypassAll_.store(b, std::memory_order_relaxed); }

  bool running() const { return running_.load(std::memory_order_acquire); }
  unsigned int sampleRate() const { return sampleRate_; }
  unsigned int bufferFrames() const { return bufferFrames_; }
  unsigned int xruns() const { return xruns_.load(); }
  // peak |sample| of the last rendered block, post output gain. 1.0 = clipping.
  // ch 0 = left, 1 = right; a mono output reports the same value on both.
  float outPeak(int ch) const { return outPeak_[ch & 1].load(std::memory_order_relaxed); }

private:
  static int callback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                      double streamTime, RtAudioStreamStatus status, void* userData);

  std::vector<AudioDevice> devices(bool wantInput);

  RtAudio audio;
  std::atomic<RenderPlan*> active_{nullptr};
  std::atomic<RenderPlan*> hazard_{nullptr};
  std::atomic<unsigned int> xruns_{0};
  std::atomic<bool> running_{false};
  std::atomic<float> inGain_{1.0f};
  std::atomic<float> outGain_{1.0f};
  std::atomic<bool> bypassAll_{false};
  std::atomic<float> outPeak_[2]{0.0f, 0.0f};
  unsigned int sampleRate_ = 48000;
  unsigned int bufferFrames_ = 256;
  unsigned int inChannels_ = 1;
  unsigned int outChannels_ = 2;
};
