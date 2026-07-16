#include <vector>

#include "Effect.h"
#include "Registry.h"

// small schroeder reverb: 4 parallel comb filters into 2 series allpass filters.
namespace {

class Comb {
public:
  void resize(int n) { buf.assign(n > 0 ? n : 1, 0.0f); idx = 0; }
  float process(float x, float feedback) {
    float y = buf[idx];
    buf[idx] = x + y * feedback;
    if (++idx >= (int)buf.size()) idx = 0;
    return y;
  }

private:
  std::vector<float> buf;
  int idx = 0;
};

class Allpass {
public:
  void resize(int n) { buf.assign(n > 0 ? n : 1, 0.0f); idx = 0; }
  float process(float x, float feedback) {
    float y = buf[idx];
    float out = -x + y;
    buf[idx] = x + y * feedback;
    if (++idx >= (int)buf.size()) idx = 0;
    return out;
  }

private:
  std::vector<float> buf;
  int idx = 0;
};

}  // namespace

class Reverb : public Effect {
public:
  void prepare(double sampleRate, int) override {
    // freeverb tunings in samples @44.1k, scaled to the actual sample rate
    const double scale = sampleRate / 44100.0;
    const int combTune[4] = {1116, 1188, 1277, 1356};
    const int apTune[2] = {556, 441};
    for (int i = 0; i < 4; i++) combs[i].resize((int)(combTune[i] * scale));
    for (int i = 0; i < 2; i++) allpasses[i].resize((int)(apTune[i] * scale));
  }

  void process(float* buffer, int numFrames) override {
    for (int n = 0; n < numFrames; n++) {
      float x = buffer[n];
      float wet = 0.0f;
      for (int i = 0; i < 4; i++) wet += combs[i].process(x, feedback);
      wet *= 0.25f;
      for (int i = 0; i < 2; i++) wet = allpasses[i].process(wet, 0.5f);
      buffer[n] = x * (1.0f - mix) + wet * mix;
    }
  }

  const char* name() const override { return "reverb"; }

private:
  Comb combs[4];
  Allpass allpasses[2];
  float feedback = 0.84f;
  float mix = 0.3f;
};

REGISTER_EFFECT(Reverb, "reverb")
