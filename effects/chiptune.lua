-- gameboy guitar: replaces the note with a square wave. the input is
-- double-lowpassed so zero crossings follow the fundamental, an envelope
-- follower keeps picking dynamics, and a sample-hold adds console grit.
-- works best on single notes, gloriously broken on chords.
name = "chiptune"
params = {
  { name = "track", min = 100, max = 2000, default = 500 },
  { name = "crush", min = 1,   max = 32,   default = 6, kind = "int" },
  { name = "mix",   min = 0,   max = 1,    default = 1 },
}

local exp, abs, pi = math.exp, math.abs, math.pi

local sr = 48000
local lp1, lp2, env = 0, 0, 0
local held, count = 0, 0

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  lp1, lp2, env = 0, 0, 0
  held, count = 0, 0
end

function process(buf, n, p)
  local c = 1 - exp(-2 * pi * p.track / sr)
  local rel = exp(-1 / (0.05 * sr))
  local ds, mx = p.crush, p.mix
  for i = 0, n - 1 do
    local x = buf[i]
    lp1 = lp1 + c * (x - lp1)
    lp2 = lp2 + c * (lp1 - lp2)
    local a = abs(x)
    if a > env then env = a else env = env * rel end
    local sq = lp2 >= 0 and env or -env
    if count == 0 then held = sq end
    count = count + 1
    if count >= ds then count = 0 end
    buf[i] = x * (1 - mx) + held * 0.8 * mx
  end
end
