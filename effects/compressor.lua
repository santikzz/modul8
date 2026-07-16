-- feed-forward compressor: peak envelope drives gain reduction above the
-- threshold, attack/release smooth both the envelope and the applied gain
-- so the pumping stays musical instead of clicky.
name = "compressor"
params = {
  { name = "threshold", min = 0.02, max = 1,    default = 0.25 },
  { name = "ratio",     min = 1,    max = 20,   default = 4 },
  { name = "attack",    min = 0.1,  max = 100,  default = 5 },
  { name = "release",   min = 10,   max = 1000, default = 120 },
  { name = "makeup",    min = 1,    max = 4,    default = 1.5 },
}

local exp, abs = math.exp, math.abs

local sr, env, gain = 48000, 0, 1

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  env = 0
  gain = 1
end

function process(buf, n, p)
  local atk = exp(-1 / (p.attack * 0.001 * sr))
  local rel = exp(-1 / (p.release * 0.001 * sr))
  local thr, ratio, mk = p.threshold, p.ratio, p.makeup
  for i = 0, n - 1 do
    local a = abs(buf[i])
    if a > env then env = a + (env - a) * atk else env = a + (env - a) * rel end
    local target = 1
    if env > thr then target = (thr + (env - thr) / ratio) / env end
    local c = target < gain and atk or rel
    gain = target + (gain - target) * c
    local out = buf[i] * gain * mk
    if out > 1 then out = 1 elseif out < -1 then out = -1 end
    buf[i] = out
  end
end
