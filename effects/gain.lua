name = "gain"
banner = "assets/gain.jpg"
params = {
  { name = "gain", min = 0, max = 2, default = 0.8 },
}

function process(buf, n, p)
  local g = p.gain
  for i = 0, n - 1 do
    buf[i] = buf[i] * g
  end
end
