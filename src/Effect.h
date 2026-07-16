#pragma once

// base class for every pedal/module.
// process() runs on the realtime audio thread: no allocation, no locks, no io.
// do all buffer setup in prepare(), which is called before the stream starts.
class Effect {
public:
  virtual ~Effect() = default;

  virtual void prepare(double sampleRate, int maxBlock) {}
  virtual void process(float* buffer, int numFrames) = 0;
  virtual const char* name() const = 0;
};
