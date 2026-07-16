-- small schroeder reverb: 4 parallel comb filters into 2 series allpass
-- filters. freeverb tunings in samples @44.1k, scaled to the actual rate.
name = "reverb"
params = {
  { name = "mix",   min = 0, max = 1,    default = 0.3 },
  { name = "decay", min = 0, max = 0.95, default = 0.84 },
}

local ffi = require("ffi")
local floor = math.floor

local combTune = { 1116, 1188, 1277, 1356 }
local apTune = { 556, 441 }
local comb, combSize, combIdx = {}, {}, {}
local ap, apSize, apIdx = {}, {}, {}

function prepare(sampleRate, maxBlock)
  local scale = sampleRate / 44100
  for i = 1, 4 do
    combSize[i] = floor(combTune[i] * scale)
    comb[i] = ffi.new("float[?]", combSize[i])
    combIdx[i] = 0
  end
  for i = 1, 2 do
    apSize[i] = floor(apTune[i] * scale)
    ap[i] = ffi.new("float[?]", apSize[i])
    apIdx[i] = 0
  end
end

prepare(48000, 0)

function process(b, n, p)
  local fb, mx = p.decay, p.mix
  for s = 0, n - 1 do
    local x = b[s]
    local wet = 0
    for i = 1, 4 do
      local line, idx = comb[i], combIdx[i]
      local y = line[idx]
      line[idx] = x + y * fb
      idx = idx + 1
      if idx >= combSize[i] then idx = 0 end
      combIdx[i] = idx
      wet = wet + y
    end
    wet = wet * 0.25
    for i = 1, 2 do
      local line, idx = ap[i], apIdx[i]
      local y = line[idx]
      local out = -wet + y
      line[idx] = wet + y * 0.5
      idx = idx + 1
      if idx >= apSize[i] then idx = 0 end
      apIdx[i] = idx
      wet = out
    end
    b[s] = x * (1 - mx) + wet * mx
  end
end
