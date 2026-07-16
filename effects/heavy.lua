-- high-gain metal distortion: tight highpass kills low-end flub before the
-- gain, then a tanh stage into a hard clipper, mid scoop and tone lowpass
-- after. scoop low = punk grind, scoop high = scooped metal chug.
name = "heavy"
params = {
  { name = "gain",  min = 1,    max = 100,  default = 35 },
  { name = "tight", min = 40,   max = 400,  default = 120 },
  { name = "scoop", min = 0,    max = 1,    default = 0.5 },
  { name = "tone",  min = 1000, max = 8000, default = 4500 },
  { name = "level", min = 0,    max = 1,    default = 0.5 },
}

local exp, tanh, pi = math.exp, math.tanh, math.pi

local sr = 48000
local hp, lpA, lpB, tn = 0, 0, 0, 0

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  hp, lpA, lpB, tn = 0, 0, 0, 0
end

local function coef(f)
  return 1 - exp(-2 * pi * f / sr)
end

function process(buf, n, p)
  local g, sc, lv = p.gain, p.scoop, p.level
  local cT = coef(p.tight)
  local cA, cB = coef(1800), coef(250)
  local cN = coef(p.tone)
  for i = 0, n - 1 do
    local x = buf[i]
    hp = hp + cT * (x - hp)
    x = x - hp
    x = tanh(x * g) * 1.5
    if x > 0.9 then x = 0.9 elseif x < -0.9 then x = -0.9 end
    lpA = lpA + cA * (x - lpA)
    lpB = lpB + cB * (x - lpB)
    x = x - sc * (lpA - lpB)
    tn = tn + cN * (x - tn)
    buf[i] = tn * lv
  end
end
