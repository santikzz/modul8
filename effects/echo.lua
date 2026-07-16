-- single-tap feedback delay. the line is sized for the longest delay in
-- prepare; process only reads/writes it, so nothing allocates per block.
name = "echo"
banner = "assets/echo.jpg"
params = {
  { name = "time",     min = 20, max = 1000, default = 300 },
  { name = "feedback", min = 0,  max = 0.95, default = 0.4 },
  { name = "mix",      min = 0,  max = 1,    default = 0.4 },
}

local ffi = require("ffi")
local floor = math.floor

local sr, size, buf, idx

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  size = floor(sr * 1.2) + 1
  buf = ffi.new("float[?]", size)
  idx = 0
end

prepare(48000, 0)

function process(b, n, p)
  local d = floor(p.time * 0.001 * sr)
  if d < 1 then d = 1 end
  if d > size - 1 then d = size - 1 end
  local fb, mx = p.feedback, p.mix
  for i = 0, n - 1 do
    local r = idx - d
    if r < 0 then r = r + size end
    local delayed = buf[r]
    buf[idx] = b[i] + delayed * fb
    b[i] = b[i] * (1 - mx) + delayed * mx
    idx = idx + 1
    if idx >= size then idx = 0 end
  end
end
