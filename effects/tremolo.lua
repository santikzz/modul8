-- lua effect script. contract:
--   name    = unique effect name (string, required)
--   params  = list of { name, min, max, default, kind = "float"|"int"|"bool" }
--   prepare(sampleRate, maxBlock)  optional, called before the stream starts
--   process(buf, n, p)             required, buf is a float* (0-indexed!), n is
--                                  the frame count, p.<param name> holds knob values
-- process runs on the realtime audio thread: avoid creating tables/strings in it.

name = "tremolo"
params = {
  { name = "rate",  min = 0.5, max = 15, default = 5 },
  { name = "depth", min = 0,   max = 1,  default = 0.6 },
}

local sr = 48000
local phase = 0
local tau = 2 * math.pi

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  phase = 0
end

function process(buf, n, p)
  local inc = tau * p.rate / sr
  local depth = p.depth
  for i = 0, n - 1 do
    local lfo = 1 - depth * 0.5 * (1 + math.sin(phase))
    buf[i] = buf[i] * lfo
    phase = phase + inc
    if phase > tau then phase = phase - tau end
  end
end
