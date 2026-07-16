#include "Effect.h"
#include "Registry.h"

class Gain : public Effect {
public:
  void process(float* buffer, int numFrames) override {
    for (int i = 0; i < numFrames; i++) buffer[i] *= gain;
  }
  const char* name() const override { return "gain"; }

private:
  float gain = 0.8f;
};

REGISTER_EFFECT(Gain, "gain")
