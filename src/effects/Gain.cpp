#include <atomic>

#include "effect.h"
#include "registry.h"

class Gain : public Effect {
public:
  Gain() { param("gain", &gain, 0.0f, 2.0f); }

  void process(float* buffer, int numFrames) override {
    float g = gain.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; i++) buffer[i] *= g;
  }
  const char* name() const override { return "gain"; }

private:
  std::atomic<float> gain{0.8f};
};

REGISTER_EFFECT(Gain, "gain")
