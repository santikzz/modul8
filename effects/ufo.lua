-- flying saucer: ring modulator whose carrier pitch is swept up and down by a
-- slow lfo. metallic inharmonic tones that warble like a b-movie ufo.
name = "ufo"
params = {
  { name = "freq",  min = 50,  max = 1000, default = 300 },
  { name = "sweep", min = 0,   max = 1,    default = 0.6 },
  { name = "speed", min = 0.1, max = 10,   default = 0.7 },
  { name = "mix",   min = 0,   max = 1,    default = 0.8 },
}

local sin = math.sin
local tau = 2 * math.pi

local sr, cphase, sphase = 48000, 0, 0

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  cphase, sphase = 0, 0
end

function process(buf, n, p)
  local sinc = tau * p.speed / sr
  local base, sweep, mx = p.freq, p.sweep, p.mix
  for i = 0, n - 1 do
    local cf = base * (1 + sweep * 0.8 * sin(sphase))
    cphase = cphase + tau * cf / sr
    if cphase > tau then cphase = cphase - tau end
    sphase = sphase + sinc
    if sphase > tau then sphase = sphase - tau end
    local wet = buf[i] * sin(cphase)
    buf[i] = buf[i] * (1 - mx) + wet * mx
  end
end
