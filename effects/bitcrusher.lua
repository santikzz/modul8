name = "bitcrusher"
banner = "assets/bitcrusher.jpg"
params = {
  { name = "bits",       min = 2, max = 16, default = 8, kind = "int" },
  { name = "downsample", min = 1, max = 16, default = 4, kind = "int" },
}

local held = 0
local count = 0

function process(buf, n, p)
  local levels = 2 ^ (p.bits - 1)
  for i = 0, n - 1 do
    if count == 0 then
      held = math.floor(buf[i] * levels + 0.5) / levels
    end
    count = count + 1
    if count >= p.downsample then count = 0 end
    buf[i] = held
  end
end
