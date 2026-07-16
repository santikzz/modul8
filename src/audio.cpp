#include "audio.h"

#include <cstring>
#include <iostream>
#include <thread>

#include "graph.h"

AudioEngine::~AudioEngine() {
  stop();
  delete active_.exchange(nullptr);
}

int AudioEngine::callback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                          double /*streamTime*/, RtAudioStreamStatus status, void* userData) {
  auto* e = static_cast<AudioEngine*>(userData);
  auto* out = static_cast<float*>(outputBuffer);
  auto* in = static_cast<float*>(inputBuffer);
  unsigned int oc = e->outChannels_, ic = e->inChannels_;

  if (status) e->xruns_++;

  // publish the plan we are about to read so the ui thread cannot free it under us.
  RenderPlan* p;
  do {
    p = e->active_.load(std::memory_order_acquire);
    e->hazard_.store(p, std::memory_order_release);
  } while (p != e->active_.load(std::memory_order_acquire));

  if (!p || !in || nFrames > e->bufferFrames_) {
    std::memset(out, 0, nFrames * oc * sizeof(float));
    e->outPeak_[0].store(0.0f, std::memory_order_relaxed);
    e->outPeak_[1].store(0.0f, std::memory_order_relaxed);
    e->hazard_.store(nullptr, std::memory_order_release);
    return 0;
  }

  float inGain = e->inGain_.load(std::memory_order_relaxed);
  float* inbuf = p->buffers[p->inputBuf].data();
  for (unsigned int i = 0; i < nFrames; i++) inbuf[i] = in[i * ic] * inGain;

  float* obuf = inbuf;  // global bypass routes the dry input straight through
  if (!e->bypassAll_.load(std::memory_order_relaxed)) {
    for (auto& s : p->steps) {
      float* buf = p->buffers[s.outBuf].data();
      if (s.inputs.empty()) {
        if (s.outBuf != p->inputBuf) std::memset(buf, 0, nFrames * sizeof(float));
      } else {
        const float* first = p->buffers[s.inputs[0]].data();
        for (unsigned int i = 0; i < nFrames; i++) buf[i] = first[i];
        for (size_t k = 1; k < s.inputs.size(); k++) {
          const float* src = p->buffers[s.inputs[k]].data();
          for (unsigned int i = 0; i < nFrames; i++) buf[i] += src[i];
        }
      }
      bool bypass = s.bypass && s.bypass->load(std::memory_order_relaxed);
      if (s.fx && !bypass) s.fx->process(buf, (int)nFrames);
    }
    obuf = p->buffers[p->outputBuf].data();
  }

  float outGain = e->outGain_.load(std::memory_order_relaxed);
  float peak[2] = {0.0f, 0.0f};
  for (unsigned int i = 0; i < nFrames; i++) {
    for (unsigned int c = 0; c < oc; c++) {
      float s = obuf[i] * outGain;
      out[i * oc + c] = s;
      float mag = s < 0.0f ? -s : s;
      if (mag > peak[c & 1]) peak[c & 1] = mag;
    }
  }
  if (oc == 1) peak[1] = peak[0];
  e->outPeak_[0].store(peak[0], std::memory_order_relaxed);
  e->outPeak_[1].store(peak[1], std::memory_order_relaxed);

  e->hazard_.store(nullptr, std::memory_order_release);
  return 0;
}

void AudioEngine::swapPlan(std::unique_ptr<RenderPlan> plan) {
  RenderPlan* raw = plan.release();
  RenderPlan* old = active_.exchange(raw, std::memory_order_acq_rel);
  if (!old) return;
  // wait out any callback still inside the old plan, then free it on this thread.
  while (running_.load(std::memory_order_acquire) &&
         hazard_.load(std::memory_order_acquire) == old)
    std::this_thread::yield();
  delete old;
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

bool AudioEngine::start(unsigned int inId, unsigned int outId, Graph& graph,
                        unsigned int sampleRate, unsigned int bufferFrames) {
  RtAudio::DeviceInfo outInfo = audio.getDeviceInfo(outId);
  inChannels_ = 1;  // guitar is mono
  outChannels_ = outInfo.outputChannels >= 2 ? 2 : 1;

  RtAudio::StreamParameters oParams, iParams;
  oParams.deviceId = outId;
  oParams.nChannels = outChannels_;
  iParams.deviceId = inId;
  iParams.nChannels = inChannels_;

  sampleRate_ = sampleRate;
  bufferFrames_ = bufferFrames;  // ~5ms @48k; lower = less latency, more risk

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;

  if (audio.openStream(&oParams, &iParams, RTAUDIO_FLOAT32, sampleRate_, &bufferFrames_,
                       &callback, this, &options)) {
    std::cerr << audio.getErrorText() << "\n";
    return false;
  }

  // openStream may have adjusted bufferFrames_; prepare + build the plan to match.
  graph.prepareAll(sampleRate_, (int)bufferFrames_);
  swapPlan(graph.buildPlan((int)bufferFrames_));

  if (audio.startStream()) {
    std::cerr << audio.getErrorText() << "\n";
    return false;
  }
  running_.store(true, std::memory_order_release);
  return true;
}

void AudioEngine::stop() {
  running_.store(false, std::memory_order_release);
  outPeak_[0].store(0.0f, std::memory_order_relaxed);
  outPeak_[1].store(0.0f, std::memory_order_relaxed);
  if (audio.isStreamRunning()) audio.stopStream();
  if (audio.isStreamOpen()) audio.closeStream();
}
