name = "distortion"
banner = "assets/distortion.jpg"
params = {
  { name = "drive", min = 1, max = 50, default = 8 },
}

local tanh = math.tanh

function process(buf, n, p)
  local d = p.drive
  for i = 0, n - 1 do
    buf[i] = tanh(buf[i] * d)
  end
end
