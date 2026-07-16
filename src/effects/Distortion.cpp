#include <cmath>

#include "Effect.h"
#include "Registry.h"

class Distortion : public Effect {
public:
  void process(float* buffer, int numFrames) override {
    for (int i = 0; i < numFrames; i++) buffer[i] = std::tanh(buffer[i] * drive);
  }
  const char* name() const override { return "distortion"; }

private:
  float drive = 8.0f;
};

REGISTER_EFFECT(Distortion, "distortion")
