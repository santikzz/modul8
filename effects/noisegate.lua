-- peak-follower gate: opens fast when the signal crosses the threshold and
-- closes over the release time. the applied gain is smoothed so it does not click.
name = "noise gate"
params = {
  { name = "threshold", min = 0,  max = 0.3,  default = 0.02 },
  { name = "release",   min = 10, max = 1000, default = 200 },
}

local exp, abs = math.exp, math.abs

local sr, env, gain = 48000, 0, 0

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  env = 0
  gain = 0
end

function process(buf, n, p)
  local relCoef = exp(-1 / (p.release * 0.001 * sr))
  local atkCoef = exp(-1 / (0.002 * sr))
  local thr = p.threshold
  for i = 0, n - 1 do
    local a = abs(buf[i])
    if a > env then env = a else env = env * relCoef end
    local target = env >= thr and 1 or 0
    local c = target > gain and atkCoef or relCoef
    gain = target + (gain - target) * c
    buf[i] = buf[i] * gain
  end
end
