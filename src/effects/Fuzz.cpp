#include <atomic>

#include "effect.h"
#include "registry.h"

// asymmetric hard clipping: gnarlier and buzzier than the tanh distortion, and
// the uneven clip points add even harmonics for that classic fuzz voice.
class Fuzz : public Effect {
public:
  Fuzz() {
    param("drive", &drive, 1.0f, 100.0f);
    param("level", &level, 0.0f, 1.0f);
  }

  void process(float* buffer, int numFrames) override {
    float d = drive.load(std::memory_order_relaxed);
    float l = level.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; i++) {
      float x = buffer[i] * d;
      if (x > 1.0f) x = 1.0f;
      if (x < -0.8f) x = -0.8f;
      buffer[i] = x * l;
    }
  }
  const char* name() const override { return "fuzz"; }

private:
  std::atomic<float> drive{20.0f};
  std::atomic<float> level{0.6f};
};

REGISTER_EFFECT(Fuzz, "fuzz")
