#include "audio.h"

#include <cstring>
#include <iostream>

#include "registry.h"

AudioEngine::~AudioEngine() { stop(); }

int AudioEngine::callback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                          double /*streamTime*/, RtAudioStreamStatus status,
                          void* userData) {
  auto* state = static_cast<State*>(userData);
  auto* out = static_cast<float*>(outputBuffer);
  auto* in = static_cast<float*>(inputBuffer);

  if (status) state->xruns++;

  if (!in || nFrames > state->mono.size()) {
    std::memset(out, 0, nFrames * state->outChannels * sizeof(float));
    return 0;
  }

  float* mono = state->mono.data();
  for (unsigned int i = 0; i < nFrames; i++)
    mono[i] = in[i * state->inChannels];

  for (auto& fx : state->chain) fx->process(mono, (int)nFrames);

  for (unsigned int i = 0; i < nFrames; i++)
    for (unsigned int c = 0; c < state->outChannels; c++)
      out[i * state->outChannels + c] = mono[i];

  return 0;
}

std::vector<AudioDevice> AudioEngine::devices(bool wantInput) {
  std::vector<AudioDevice> out;
  for (auto id : audio.getDeviceIds()) {
    RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
    unsigned int ch = wantInput ? info.inputChannels : info.outputChannels;
    if (ch == 0) continue;
    bool isDefault = wantInput ? info.isDefaultInput : info.isDefaultOutput;
    out.push_back({id, info.name, ch, isDefault});
  }
  return out;
}

void AudioEngine::setChain(const std::vector<std::string>& names,
                           std::vector<std::string>& unknown) {
  state.chain.clear();
  for (auto& n : names) {
    auto fx = Registry::create(n);
    if (!fx) {
      unknown.push_back(n);
      continue;
    }
    state.chain.push_back(std::move(fx));
  }
}

bool AudioEngine::start(unsigned int inId, unsigned int outId, unsigned int sampleRate,
                        unsigned int bufferFrames) {
  RtAudio::DeviceInfo outInfo = audio.getDeviceInfo(outId);
  state.inChannels = 1;  // guitar is mono
  state.outChannels = outInfo.outputChannels >= 2 ? 2 : 1;

  RtAudio::StreamParameters oParams, iParams;
  oParams.deviceId = outId;
  oParams.nChannels = state.outChannels;
  iParams.deviceId = inId;
  iParams.nChannels = state.inChannels;

  sampleRate_ = sampleRate;
  bufferFrames_ = bufferFrames;  // ~5ms @48k; lower = less latency, more risk

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;

  if (audio.openStream(&oParams, &iParams, RTAUDIO_FLOAT32, sampleRate_, &bufferFrames_,
                       &callback, &state, &options)) {
    std::cerr << audio.getErrorText() << "\n";
    return false;
  }

  // openStream may have adjusted bufferFrames_; size scratch + prepare effects now.
  state.mono.assign(bufferFrames_, 0.0f);
  for (auto& fx : state.chain) fx->prepare(sampleRate_, (int)bufferFrames_);

  if (audio.startStream()) {
    std::cerr << audio.getErrorText() << "\n";
    return false;
  }
  return true;
}

void AudioEngine::stop() {
  if (audio.isStreamRunning()) audio.stopStream();
  if (audio.isStreamOpen()) audio.closeStream();
}

std::vector<std::string> AudioEngine::chainNames() const {
  std::vector<std::string> out;
  for (auto& fx : state.chain) out.push_back(fx->name());
  return out;
}
