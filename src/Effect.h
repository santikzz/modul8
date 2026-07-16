#pragma once

#include <atomic>
#include <vector>

// a tweakable parameter, exposed to the ui so it can render the right widget.
// the value is an atomic owned by the effect: the audio thread reads it, the ui
// thread writes it, no locks. bool/int are stored in the same float and rounded
// on read; kind only tells the ui what control to draw.
struct Param {
  enum Kind { Float, Int, Bool };
  const char* name;
  Kind kind;
  std::atomic<float>* value;
  float min;
  float max;
};

// base class for every pedal/module.
// process() runs on the realtime audio thread: no allocation, no locks, no io.
// do all buffer setup in prepare(), which is called before the stream starts.
class Effect {
public:
  virtual ~Effect() = default;

  virtual void prepare(double sampleRate, int maxBlock) {}
  virtual void process(float* buffer, int numFrames) = 0;
  virtual const char* name() const = 0;

  // params declared in the constructor via param(); read-only view for the ui.
  const std::vector<Param>& params() const { return params_; }

protected:
  void param(const char* name, std::atomic<float>* value, float min, float max,
             Param::Kind kind = Param::Float) {
    params_.push_back({name, kind, value, min, max});
  }

private:
  std::vector<Param> params_;
};
