-- asymmetric hard clipping: gnarlier and buzzier than the tanh distortion, and
-- the uneven clip points add even harmonics for that classic fuzz voice.
name = "fuzz"
banner = "assets/fuzz.jpg"
params = {
  { name = "drive", min = 1, max = 100, default = 20 },
  { name = "level", min = 0, max = 1,   default = 0.6 },
}

function process(buf, n, p)
  local d, l = p.drive, p.level
  for i = 0, n - 1 do
    local x = buf[i] * d
    if x > 1 then x = 1 end
    if x < -0.8 then x = -0.8 end
    buf[i] = x * l
  end
end
