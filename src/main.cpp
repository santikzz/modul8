#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <RtAudio.h>

#include "Effect.h"
#include "Registry.h"

struct AudioState {
  std::vector<std::unique_ptr<Effect>> chain;
  std::vector<float> mono;
  unsigned int inChannels = 1;
  unsigned int outChannels = 2;
  std::atomic<unsigned int> xruns{0};
};

static int audioCallback(void* outputBuffer, void* inputBuffer,
                         unsigned int nFrames, double /*streamTime*/,
                         RtAudioStreamStatus status, void* userData) {
  auto* state = static_cast<AudioState*>(userData);
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

static unsigned int pickDevice(RtAudio& audio, bool wantInput) {
  auto ids = audio.getDeviceIds();
  std::vector<unsigned int> candidates;
  std::cout << "\n" << (wantInput ? "input" : "output") << " devices:\n";
  for (auto id : ids) {
    RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
    unsigned int ch = wantInput ? info.inputChannels : info.outputChannels;
    if (ch == 0) continue;
    candidates.push_back(id);
    bool isDefault = wantInput ? info.isDefaultInput : info.isDefaultOutput;
    std::cout << "  [" << candidates.size() - 1 << "] " << info.name << " (" << ch
              << " ch)" << (isDefault ? " *default" : "") << "\n";
  }
  if (candidates.empty()) {
    std::cerr << "no suitable " << (wantInput ? "input" : "output") << " device\n";
    std::exit(1);
  }
  std::cout << "pick number: ";
  size_t choice = 0;
  std::cin >> choice;
  if (choice >= candidates.size()) choice = 0;
  return candidates[choice];
}

static std::vector<std::string> pickEffects() {
  std::cout << "\navailable effects:\n";
  for (auto& n : Registry::names()) std::cout << "  " << n << "\n";
  std::cout << "enter chain in order (space separated), e.g. distortion reverb:\n> ";
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  std::string line;
  std::getline(std::cin, line);

  std::vector<std::string> chain;
  std::istringstream iss(line);
  std::string tok;
  while (iss >> tok) chain.push_back(tok);
  return chain;
}

int main() {
  RtAudio audio;
  if (audio.getDeviceIds().empty()) {
    std::cerr << "no audio devices found\n";
    return 1;
  }

  unsigned int inId = pickDevice(audio, true);
  unsigned int outId = pickDevice(audio, false);
  std::vector<std::string> chainNames = pickEffects();

  RtAudio::DeviceInfo outInfo = audio.getDeviceInfo(outId);

  auto state = std::make_unique<AudioState>();
  state->inChannels = 1;  // guitar is mono
  state->outChannels = outInfo.outputChannels >= 2 ? 2 : 1;

  for (auto& n : chainNames) {
    auto fx = Registry::create(n);
    if (!fx) {
      std::cerr << "unknown effect: " << n << " (skipped)\n";
      continue;
    }
    state->chain.push_back(std::move(fx));
  }

  RtAudio::StreamParameters oParams, iParams;
  oParams.deviceId = outId;
  oParams.nChannels = state->outChannels;
  iParams.deviceId = inId;
  iParams.nChannels = state->inChannels;

  unsigned int sampleRate = 48000;
  unsigned int bufferFrames = 256;  // ~5ms @48k; lower = less latency, more risk

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;

  if (audio.openStream(&oParams, &iParams, RTAUDIO_FLOAT32, sampleRate, &bufferFrames,
                       &audioCallback, state.get(), &options)) {
    std::cerr << audio.getErrorText() << "\n";
    return 1;
  }

  // openStream may have adjusted bufferFrames; size scratch + prepare effects now.
  state->mono.assign(bufferFrames, 0.0f);
  for (auto& fx : state->chain) fx->prepare(sampleRate, (int)bufferFrames);

  if (audio.startStream()) {
    std::cerr << audio.getErrorText() << "\n";
    return 1;
  }

  std::cout << "\nrunning: " << sampleRate << " Hz, " << bufferFrames << " frames\nchain: ";
  for (auto& fx : state->chain) std::cout << fx->name() << " ";
  std::cout << "\npress Enter to stop...\n";
  std::cin.get();

  audio.stopStream();
  if (audio.isStreamOpen()) audio.closeStream();
  if (state->xruns) std::cout << "xruns (glitches): " << state->xruns << "\n";
  return 0;
}
