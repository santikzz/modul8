-- rhythmic chopper: square-wave gate that slices the signal on and off.
-- slow = helicopter, fast = robotic buzz. the gate edge is smoothed over
-- ~2ms so the chops do not click.
name = "stutter"
params = {
  { name = "rate",  min = 1, max = 40,   default = 8 },
  { name = "duty",  min = 0.1, max = 0.9, default = 0.5 },
  { name = "depth", min = 0, max = 1,    default = 1 },
}

local exp = math.exp

local sr, phase, gate = 48000, 0, 1

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  phase, gate = 0, 1
end

function process(buf, n, p)
  local inc = p.rate / sr
  local duty, depth = p.duty, p.depth
  local sm = exp(-1 / (0.002 * sr))
  for i = 0, n - 1 do
    local target = phase < duty and 1 or (1 - depth)
    gate = target + (gate - target) * sm
    buf[i] = buf[i] * gate
    phase = phase + inc
    if phase >= 1 then phase = phase - 1 end
  end
end
