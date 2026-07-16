-- short lfo-swept delay with feedback. the moving delay line, mixed back with
-- the dry signal, sweeps a comb filter across the spectrum for the whoosh.
name = "flanger"
params = {
  { name = "rate",     min = 0.05, max = 5,   default = 0.3 },
  { name = "depth",    min = 0,    max = 1,   default = 0.7 },
  { name = "feedback", min = 0,    max = 0.9, default = 0.3 },
  { name = "mix",      min = 0,    max = 1,   default = 0.5 },
}

local ffi = require("ffi")
local floor, sin = math.floor, math.sin
local tau = 2 * math.pi

local sr, size, buf, idx, phase

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  size = floor(0.03 * sr) + 2
  buf = ffi.new("float[?]", size)
  idx = 0
  phase = 0
end

prepare(48000, 0)

function process(b, n, p)
  local base = 0.001 * sr
  local sweep = 0.005 * sr * p.depth
  local inc = tau * p.rate / sr
  local fb, mx = p.feedback, p.mix
  for i = 0, n - 1 do
    local lfo = 0.5 * (1 + sin(phase))
    phase = phase + inc
    if phase > tau then phase = phase - tau end

    local ds = base + sweep * lfo
    local rp = idx - ds
    while rp < 0 do rp = rp + size end
    local r0 = floor(rp)
    local r1 = r0 + 1
    if r1 >= size then r1 = 0 end
    local frac = rp - r0
    local delayed = buf[r0] * (1 - frac) + buf[r1] * frac

    buf[idx] = b[i] + delayed * fb
    b[i] = b[i] * (1 - mx) + delayed * mx
    idx = idx + 1
    if idx >= size then idx = 0 end
  end
end
