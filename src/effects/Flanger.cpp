#include <atomic>
#include <cmath>
#include <vector>

#include "effect.h"
#include "registry.h"

// short lfo-swept delay with feedback. the moving delay line, mixed back with the
// dry signal, sweeps a comb filter across the spectrum for the whoosh.
class Flanger : public Effect {
public:
  Flanger() {
    param("rate", &rate, 0.05f, 5.0f);
    param("depth", &depth, 0.0f, 1.0f);
    param("feedback", &feedback, 0.0f, 0.9f);
    param("mix", &mix, 0.0f, 1.0f);
  }

  void prepare(double sampleRate, int) override {
    sr_ = (float)sampleRate;
    buf_.assign((size_t)(0.03f * sr_) + 2, 0.0f);  // 30 ms line
    idx_ = 0;
    phase_ = 0.0f;
  }

  void process(float* buffer, int numFrames) override {
    float rt = rate.load(std::memory_order_relaxed);
    float dp = depth.load(std::memory_order_relaxed);
    float fb = feedback.load(std::memory_order_relaxed);
    float mx = mix.load(std::memory_order_relaxed);
    int n = (int)buf_.size();
    float base = 0.001f * sr_;       // 1 ms base delay
    float sweep = 0.005f * sr_ * dp;  // up to 5 ms of sweep
    float inc = 2.0f * 3.14159265f * rt / sr_;
    for (int i = 0; i < numFrames; i++) {
      float lfo = 0.5f * (1.0f + std::sin(phase_));
      phase_ += inc;
      if (phase_ > 6.2831853f) phase_ -= 6.2831853f;

      float ds = base + sweep * lfo;
      float rp = (float)idx_ - ds;
      while (rp < 0.0f) rp += n;
      int r0 = (int)rp;
      int r1 = r0 + 1 >= n ? 0 : r0 + 1;
      float frac = rp - (float)r0;
      float delayed = buf_[r0] * (1.0f - frac) + buf_[r1] * frac;

      buf_[idx_] = buffer[i] + delayed * fb;
      buffer[i] = buffer[i] * (1.0f - mx) + delayed * mx;
      if (++idx_ >= n) idx_ = 0;
    }
  }
  const char* name() const override { return "flanger"; }

private:
  std::atomic<float> rate{0.3f};
  std::atomic<float> depth{0.7f};
  std::atomic<float> feedback{0.3f};
  std::atomic<float> mix{0.5f};
  std::vector<float> buf_;
  int idx_ = 0;
  float phase_ = 0.0f;
  float sr_ = 48000.0f;
};

REGISTER_EFFECT(Flanger, "flanger")
