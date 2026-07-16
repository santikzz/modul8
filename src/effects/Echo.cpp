#include <atomic>
#include <vector>

#include "effect.h"
#include "registry.h"

// single-tap feedback delay. the buffer is sized for the longest delay in prepare;
// process only reads/writes it, so nothing allocates on the audio thread.
class Echo : public Effect {
public:
  Echo() {
    param("time", &timeMs, 20.0f, 1000.0f);
    param("feedback", &feedback, 0.0f, 0.95f);
    param("mix", &mix, 0.0f, 1.0f);
  }

  void prepare(double sampleRate, int) override {
    sr_ = (float)sampleRate;
    buf_.assign((size_t)(sr_ * 1.2f) + 1, 0.0f);  // up to 1.2 s
    idx_ = 0;
  }

  void process(float* buffer, int numFrames) override {
    float t = timeMs.load(std::memory_order_relaxed);
    float fb = feedback.load(std::memory_order_relaxed);
    float mx = mix.load(std::memory_order_relaxed);
    int n = (int)buf_.size();
    int d = (int)(t * 0.001f * sr_);
    if (d < 1) d = 1;
    if (d > n - 1) d = n - 1;
    for (int i = 0; i < numFrames; i++) {
      int r = idx_ - d;
      if (r < 0) r += n;
      float delayed = buf_[r];
      buf_[idx_] = buffer[i] + delayed * fb;
      buffer[i] = buffer[i] * (1.0f - mx) + delayed * mx;
      if (++idx_ >= n) idx_ = 0;
    }
  }
  const char* name() const override { return "echo"; }

private:
  std::atomic<float> timeMs{300.0f};
  std::atomic<float> feedback{0.4f};
  std::atomic<float> mix{0.4f};
  std::vector<float> buf_;
  int idx_ = 0;
  float sr_ = 48000.0f;
};

REGISTER_EFFECT(Echo, "echo")
