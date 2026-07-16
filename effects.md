# writing lua effects for modul8

This document specifies the contract for effect scripts. It is written so that a
human or an LLM can author a correct, realtime-safe effect from it without reading
the host source.

## how effects work

modul8 is a realtime mono guitar effects host. Audio flows through a
user-editable graph of effect nodes. Each node processes one block of samples at a
time, in place, on a realtime audio thread.

Effects are Lua scripts loaded with **LuaJIT**. The host scans `effects/*.lua`
next to the executable at startup (and again via *File → Reload Lua Effects*).
Every valid script becomes an effect the user can drop onto the canvas. Each
placed node gets its **own independent Lua state** — two instances of the same
effect never share variables.

The hot loop is JIT-compiled; a well-written script performs within ~2x of native
C++ and comfortably handles 48 kHz audio at small block sizes (32–1024 frames).

## file layout

```
modul8.exe
lua51.dll           luajit runtime, required
effects/            one .lua file per effect
  distortion.lua
  tremolo.lua
  ...
assets/             optional banner images referenced by scripts
  fuzz.jpg
presets/            saved rigs (host-managed, not your concern)
```

## script structure

A script is a plain Lua file. There are no `require`able host modules and no
registration calls: the host reads specific **globals** after running the file
once (top to bottom) at load time.

### globals the host reads (your "exports")

| global | type | required | meaning |
|---|---|---|---|
| `name` | string | yes | unique effect name, shown on the node and stored in presets. lowercase by convention (`"noise gate"`). must not collide with another script |
| `params` | table | no | ordered list of knob definitions, see below. omit for a knobless effect |
| `banner` | string | no | image path relative to the app folder, e.g. `"assets/fuzz.jpg"`. jpg/png/bmp. drawn on the node above the knobs (~138px wide, aspect kept — around 280x80 source works well) |
| `prepare` | function | no | `prepare(sampleRate, maxBlock)` — called before the stream starts and on every restart. allocate buffers and reset state here |
| `process` | function | yes | `process(buf, n, p)` — the audio callback, see below |

Everything else in the file (locals, helper functions) is private to your script.

### what the host provides (your "imports")

Full Lua 5.1 standard library plus LuaJIT extensions. The relevant parts:

- `math.*` — `sin`, `exp`, `abs`, `floor`, `tanh`, `pi`, etc.
- `ffi = require("ffi")` — for allocating raw float arrays (delay lines):
  `local line = ffi.new("float[?]", size)` — zero-initialized, 0-indexed.

There is no host API beyond the calling contract; you never call into the host.

## params

```lua
params = {
  { name = "drive", min = 1,  max = 50,  default = 8 },
  { name = "mode",  min = 0,  max = 3,   default = 0,   kind = "int" },
  { name = "sync",  default = 0,                        kind = "bool" },
}
```

Per entry:

- `name` (string, required) — knob label and the key you read in `process`.
- `min`, `max` (numbers, default `0`/`1`) — knob range; `min < max` required.
  Ignored for `kind = "bool"` (forced 0..1).
- `default` (number, default `min`) — initial value, clamped into range.
- `kind` (string, default `"float"`) — `"float"` renders a slider, `"int"` a
  stepped slider, `"bool"` a checkbox.

The UI edits these values live while audio runs; the host hands you the current
values each block. Param values persist in presets, keyed by `name`.

## process(buf, n, p)

Called on the **realtime audio thread**, once per block, with the effect's input
already in `buf`. Write your output back into the same buffer.

- `buf` — FFI `float*` pointer, mono samples, nominal range −1..1.
  **0-indexed**: valid indices are `buf[0] .. buf[n-1]`. Lua's usual 1-based
  indexing does not apply; reading `buf[n]` is out of bounds (no bounds check —
  it will corrupt memory or crash, not raise an error).
- `n` — frame count for this block. Do not assume a fixed value; it can be any
  size up to the `maxBlock` given to `prepare`.
- `p` — table of current param values by name: `p.drive`, `p.mode`, `p.sync`.
  `"int"` and `"bool"` kinds arrive pre-rounded (bool as `0`/`1`). The same table
  object is reused every block — read from it, never store or mutate it.

Return value is ignored.

### realtime rules (critical)

The GC is stopped and stepped minimally by the host, but allocation churn will
still eventually stall the audio thread. Inside `process`:

- **No allocation**: no table constructors `{}`, no string concatenation, no
  closures, no `ffi.new`. Allocate everything in `prepare` or at file scope.
- No `print`, no `io.*`, no `os.*`.
- Hoist `math.*` lookups to file-scope locals (`local sin = math.sin`) — cheaper
  and JIT-friendlier.
- Keep per-sample state (phase, indices, envelopes) in file-scope locals, not in
  tables you index by string every sample.

`prepare` runs off the audio thread (stream stopped), so allocation there is fine.

### error behavior

If the script fails to load (syntax error, missing `name`/`process`, bad params,
duplicate name), it is skipped and the error is printed to the debug console.
If `process` or `prepare` raises at runtime, the host prints the error once and
**bypasses the effect permanently** (audio passes through dry) until it is
re-added or reloaded. Errors never crash the host, but a bypassed effect is a
broken effect — treat the rules above as hard requirements.

## lifecycle

1. **Load** — file runs top to bottom once (per placed node). File-scope code is
   your constructor: seed state so `process` works even before `prepare` runs
   (allocate delay lines for a default 48000 rate).
2. **prepare(sampleRate, maxBlock)** — stream is about to start (or restart, e.g.
   after a device/buffer change). Reallocate rate-dependent buffers, reset state.
   May be called multiple times.
3. **process(buf, n, p)** — repeatedly, on the audio thread.
4. There is no destructor; the host frees the Lua state when the node is removed.

Editing a script and choosing *File → Reload Lua Effects* re-registers it; nodes
already on the canvas keep the old code until re-added.

## minimal example

```lua
name = "gain"
params = {
  { name = "gain", min = 0, max = 2, default = 0.8 },
}

function process(buf, n, p)
  local g = p.gain
  for i = 0, n - 1 do
    buf[i] = buf[i] * g
  end
end
```

## full example: stateful effect with a delay line

```lua
-- single-tap feedback delay
name = "echo"
banner = "assets/echo.jpg"          -- optional
params = {
  { name = "time",     min = 20, max = 1000, default = 300 },  -- ms
  { name = "feedback", min = 0,  max = 0.95, default = 0.4 },
  { name = "mix",      min = 0,  max = 1,    default = 0.4 },
}

local ffi = require("ffi")
local floor = math.floor

local sr, size, line, idx

function prepare(sampleRate, maxBlock)
  sr = sampleRate
  size = floor(sr * 1.2) + 1          -- room for the longest delay
  line = ffi.new("float[?]", size)    -- allocation: allowed here only
  idx = 0
end

prepare(48000, 0)  -- seed state so process is safe before the first prepare

function process(buf, n, p)
  local d = floor(p.time * 0.001 * sr)
  if d < 1 then d = 1 end
  if d > size - 1 then d = size - 1 end
  local fb, mx = p.feedback, p.mix
  for i = 0, n - 1 do
    local r = idx - d
    if r < 0 then r = r + size end
    local wet = line[r]
    line[idx] = buf[i] + wet * fb
    buf[i] = buf[i] * (1 - mx) + wet * mx
    idx = idx + 1
    if idx >= size then idx = 0 end
  end
end
```

Patterns worth copying from the bundled scripts:

- `tremolo.lua` — LFO with phase kept across blocks, rate-dependent increment.
- `flanger.lua` — fractional (interpolated) delay-line read.
- `reverb.lua` — multiple parallel/series lines held in tables of FFI arrays,
  with sizes and indices in parallel Lua tables.
- `noisegate.lua` — per-sample envelope follower with smoothing coefficients
  computed once per block from params.

## checklist for a new effect

1. One file in `effects/`, unique `name` global.
2. `params` with sensible ranges and defaults; knob order = table order.
3. `process(buf, n, p)` loops `0 .. n-1`, writes in place, allocates nothing.
4. State lives in file-scope locals; buffers via `ffi.new` in `prepare`; seed
   with a top-level `prepare(48000, 0)` call.
5. Derive all timing from the `sampleRate` given to `prepare` — never hardcode.
6. Keep output bounded (clip or scale); feedback params should max below 1.
7. Reload via *File → Reload Lua Effects*; load errors appear in the debug
   console (*Settings → show debug console*).
