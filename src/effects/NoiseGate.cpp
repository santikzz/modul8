#include <atomic>
#include <cmath>

#include "effect.h"
#include "registry.h"

// peak-follower gate: opens fast when the signal crosses the threshold and closes
// over the release time. the applied gain is smoothed so it does not click.
class NoiseGate : public Effect {
public:
  NoiseGate() {
    param("threshold", &threshold, 0.0f, 0.3f);
    param("release", &releaseMs, 10.0f, 1000.0f);
  }

  void prepare(double sampleRate, int) override {
    sr_ = (float)sampleRate;
    env_ = 0.0f;
    gain_ = 0.0f;
  }

  void process(float* buffer, int numFrames) override {
    float thr = threshold.load(std::memory_order_relaxed);
    float rel = releaseMs.load(std::memory_order_relaxed);
    float relCoef = std::exp(-1.0f / ((rel * 0.001f) * sr_));
    float atkCoef = std::exp(-1.0f / (0.002f * sr_));  // 2 ms open ramp
    for (int i = 0; i < numFrames; i++) {
      float a = std::fabs(buffer[i]);
      env_ = a > env_ ? a : env_ * relCoef;  // instant attack, decaying release
      float target = env_ >= thr ? 1.0f : 0.0f;
      float c = target > gain_ ? atkCoef : relCoef;
      gain_ = target + (gain_ - target) * c;
      buffer[i] *= gain_;
    }
  }
  const char* name() const override { return "noise gate"; }

private:
  std::atomic<float> threshold{0.02f};
  std::atomic<float> releaseMs{200.0f};
  float sr_ = 48000.0f;
  float env_ = 0.0f;
  float gain_ = 0.0f;
};

REGISTER_EFFECT(NoiseGate, "noise gate")
