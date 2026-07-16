#include <atomic>
#include <cmath>

#include "effect.h"
#include "registry.h"

class Distortion : public Effect {
public:
  Distortion() { param("drive", &drive, 1.0f, 50.0f); }

  void process(float* buffer, int numFrames) override {
    float d = drive.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; i++) buffer[i] = std::tanh(buffer[i] * d);
  }
  const char* name() const override { return "distortion"; }

private:
  std::atomic<float> drive{8.0f};
};

REGISTER_EFFECT(Distortion, "distortion")
