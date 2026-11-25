# Lifebook Reverb Appliance — Implementation Plan (rev. after Desktop review)

## Goal
Repurpose an existing Daisy Seed reverb algorithm (C++ firmware signal path) to run as a
soft-DSP on a Fujitsu Lifebook P1630, using an Elektron **Tonverk** as a USB-audio interface:

```
Tonverk USB audio OUT ──► Lifebook (USB capture) ──► reverb DSP ──► Lifebook (USB playback) ──► Tonverk USB audio IN
```

No analog I/O. Later, a stylus-driven control surface (portrait orientation) for live parameter
control, with tabbed layout and preset save/load — implemented as a **console/framebuffer
ncurses TUI, no X server** (see Phase 3). The system stays fully headless.

**Status:** both former blockers resolved. Repo cloned (`brianvarren/reverb`, all headers present);
Tonverk behavior understood from docs (48 kHz fixed, 4 capture / 2 playback, one ALSA card).
Remaining unknowns are all Phase-0 measurements that cannot change the architecture.

---

## Measured target hardware (probed, not assumed)

| Item | Finding | Implication |
|---|---|---|
| CPU | Intel Core 2 Duo **U9300** @ 1.2 GHz, 2 cores (Penryn) | Plenty for one reverb; it ran on a 480 MHz Cortex-M7 |
| CPU ISA | `sse, sse2, ssse3, sse4_1` — **no sse4_2, no AVX** | Compile `-march=core2 -msse4.1`; **never** x86-64-v2/AVX binaries (that's why a prebuilt native binary SIGILL'd) |
| RAM | ~2 GB total, ~1.5 GB free | Fine |
| GPU | Intel GMA 4500MHD | Irrelevant — no GL/X used |
| Display | ~1280×800 LVDS, rotated 90° (portrait) | **Framebuffer console** rotation via `fbcon=rotate:1` (no X) |
| Touch | **Fujitsu Component USB Touch Panel** (resistive, USB HID) — *not* Wacom EMR | Works well with a stylus; **read its evdev node directly** (no X pointer driver), calibrate + map coords in-app |
| OS | Debian 13 (trixie), kernel 6.x | Current ALSA era |
| Audio now | Internal HDA ALC269 only; PipeWire libs present but **no sound daemon apparently running** | Take the Tonverk `hw:` device **directly via ALSA** |
| USB | USB 2.0 host | Adequate for 4-in/2-out 48 kHz UAC |
| Toolchain | gcc/g++ 14.2, cmake 3.31, make, git, pkg-config | Build natively on the Lifebook |
| X11 | installed, but **intentionally NOT used** | Project stays headless: console + framebuffer only, no X server / WM / compositor |

---

## Current codebase state
- Repo **`brianvarren/reverb`** cloned to `~/dev/reverb` on the Lifebook — **self-contained and buildable**.
- `core/` holds the DSP headers: `reverb.h rotation.h shelf.h delay.h params.h util.h`.
- Tree also has `host/` (offline `verb_cli`), `tests/`, and `tui/` (ncurses + **miniaudio** full-duplex + `dr_wav`).
- The loose `~/Downloads/reverb-test-TUI` extract is now **superseded** by the full repo.

Engine design highlights (good for this project):
- Control-block = 48 frames; `UpdateBlock()` once per block then per-sample `Process()`.
- Lock-free UI↔audio via `std::atomic<float>` param array (snapshotted per block). No alloc/syscalls in `Render()`.
- Already sets SSE flush-to-zero / denormals-are-zero — **keep this call inside the live audio callback**.
- Stereo recirculating late network: cross-coupled feedback, 3 rotation/diffuser stages + long delay, loop shelf filters, DC blocker. Early reflections w/ up to ~170 ms predelay.

---

## Tonverk appliance configuration (device-side, set once)
- **USB CONFIG = USB AUDIO/MIDI**
- **Firmware 1.5.0** (fixes unstable class-compliant streaming with some hosts — update first)
- A **Send track routed to OUT CD** with a pass-through effect (so dry source reaches capture 3/4)
- **USB AUDIO IN = OUT AB** (or MIX AB) — **never OUT CD** (prevents the processed return re-entering the send)
- **INT TO MAIN = ON**

Channel contract: Lifebook **captures 4** channels, reads the **3/4 pair** (the Send/OUT CD feed);
Lifebook **plays 2** channels back → Tonverk USB AUDIO IN (OUT/MIX AB).

---

## Phase 0 — Prove the plumbing (hardware reality check, ~no code)
Runs in **parallel with Phase 1**. Cheapest first step of the whole project.

1. **Update Tonverk to 1.5.0** first.
2. Plug Tonverk into Lifebook USB (USB AUDIO/MIDI mode). Enumerate:
   `lsusb`, `cat /proc/asound/cards`, `arecord -l`, `aplay -l` → expect **4 capture / 2 playback on one card**.
3. Capabilities: `arecord -D hw:X,0 --dump-hw-params` (and playback) → record **UAC1 vs UAC2**,
   **sample format** (S16/S24/S32), confirm 48 kHz fixed, period/buffer bounds.
4. **Channel-map check:** route a sound to **OUT CD**; confirm it appears **only on capture 3/4** (not 1/2).
5. **Loop test:** set **USB AUDIO IN = OUT AB**; send a one-shot through and confirm it **comes back once
   and does not run away** (no feedback loop) — validates the routing contract before any DSP.
6. **Period sweep:** try 128/256/512 frames @ 48 kHz; log xruns (`/proc/asound/card*/pcm*/sub*/status`).
7. Confirm no PipeWire/Pulse daemon auto-claims the device (we want exclusive `hw:`).

**Exit criteria:** stable passthrough at a known period size, documented format + channel map.

---

## Phase 1 — Build & run the DSP on the Lifebook (synthetic input)
Can run in **parallel with Phase 0** (no hardware needed).

1. Repo already cloned at `~/dev/reverb` (done).
2. `apt install libncurses-dev` if not present; miniaudio/dr_wav are vendored single-headers.
3. CMake flags for this CPU:
   ```
   -O2 -march=core2 -msse4.1 -mtune=core2 -mfpmath=sse -fno-math-errno
   ```
   (No AVX; keep the explicit FTZ/DAZ; avoid `-ffast-math` surprises.)
4. Build + run `verb_cli` (offline) and `verb_tests`; validate numeric correctness against the
   reference WAVs in the repo (`cmp_*.wav`).
5. Build the ncurses **TUI**, run with the internal test generator; verify audio correctness and
   **measure CPU headroom** on one core.

**Exit criteria:** TUI runs on the Lifebook, reverb correct, comfortable CPU headroom.

---

## Phase 2 — Live USB audio I/O (the core repurposing)
1. miniaudio **duplex device** bound to the Tonverk via ALSA (`hw:X`), 48 kHz, format from Phase 0.
2. **Open all 4 capture channels, read the 3/4 pair**; the capture pair is a **config setting**
   (default 3/4) so it's easy to change. Play back the stereo result to the 2 playback channels.
3. Data callback `(pOutput, pInput, frameCount)`:
   - `frameCount` not guaranteed a multiple of 48 → drive the engine's 48-frame control cadence with
     an **internal sample counter**. Per-sample `Process(inL,inR)->(outL,outR)`.
   - **Keep the SSE flush-to-zero call inside the callback.**
   - Strictly RT-safe: no alloc/lock/log.
4. **Start at 256 frames.** Latency here only adds predelay — not a correctness issue — so prioritize
   zero xruns over minimum latency, then tighten if headroom allows.
5. RT scheduling: `audio` group; `@audio rtprio 95 / memlock` in limits.conf; `SCHED_FIFO` audio
   thread; `mlockall` at startup.
6. Keep ncurses TUI as control surface; add level meters + xrun counter.

**Exit criteria:** Tonverk OUT CD → reverb → Tonverk USB AUDIO IN, glitch-free sustained, acceptable latency.

---

## Phase 3 — Console TUI control surface (portrait, stylus) — NO X
**Hard constraint:** fully headless. **No X server, no WM, no compositor.** Framebuffer-console
ncurses TUI extending the Phase 1/2 TUI. GPU/GL irrelevant.

1. **Toolkit:** ncurses (default) — optionally notcurses later for richer drawing.
2. **Portrait (text console):** boot cmdline `fbcon=rotate:1` (90° CW; `:3` CCW) via GRUB/extlinux.
   Verify on i915 KMS; fallback is drawing the UI pre-rotated in-app.
3. **Touch via evdev (no X):**
   - Open the Fujitsu panel's `/dev/input/eventN`; read `EV_ABS` `ABS_X/ABS_Y` + `BTN_TOUCH` (libevdev).
   - Read raw min/max via `EVIOCGABS`; **4-point calibration** persisted to a file —
     **calibration also handles the 90° rotation** (one combined transform), mapping coords → terminal cells.
   - evdev reader on its own thread → lock-free queue → UI thread. Never touch the RT audio thread.
4. **Widgets (character-cell):** vertical filled-bar / filled-block controls using **only glyphs the
   console font actually has** (verify block glyphs render; fall back to ASCII/color fills if not).
   Stylus drag sets value; large pen-friendly hit zones; live-update engine atomics.
5. **Tabs** for parameter groups (early / late / tone / global).
6. **Launch:** bare VT (systemd service / getty on tty1). No `startx`, no `.xinitrc`. Autostarts as appliance.

**Exit criteria:** portrait console TUI, stylus adjusts params in real time while audio runs, tabs navigate — no X.

**Risks:** fbcon rotation quirks on i915 KMS; hand-rolled calibration+rotation transform; resistive
jitter/latency (debounce/smooth); evdev permissions (udev/`input` group / on the VT); keeping
evdev+UI off the RT audio thread on 2 cores.

---

## Phase 4 — Preset management
1. File-based preset store (one file per preset; human-readable, no heavy deps).
2. **Save atomically** (write temp + `rename()`), so a crash mid-save can't corrupt a preset.
3. Load applies to atomics (optionally smoothed to avoid zipper noise).
4. UI: a preset tab — list, save-as, load, overwrite, delete; name entry via on-screen keypad (stylus) or keyboard.
5. Decide default preset + boot-restore behavior.

**Exit criteria:** create/recall presets entirely from the console TUI via stylus.

---

## Still unknown — resolved in Phase 0 (none affect architecture)
- **UAC1 vs UAC2 + sample format** → only affects the format-conversion code.
- **Smallest xrun-free period size** on this USB 2.0 controller → tune from 256.
- **Can a Send FX pass sound through unchanged?** If not, use **insert mode** with dry/wet mix done in the reverb.
- **Does the volume knob scale USB 1/2?** Doesn't matter — we read 3/4.

## Cross-cutting decisions (settled unless noted)
1. Repo: `brianvarren/reverb` at `~/dev/reverb` — **done**.
2. TUI library: **ncurses** (notcurses optional later).
3. Sound-server: **exclusive ALSA `hw:`**.
4. Capture pair: **3/4** (config-settable).
5. Rotation: **`fbcon=rotate:1`** (confirm direction on hardware).
6. Compiler: **`-march=core2 -msse4.1`** (penryn is Clang-only; GCC rejects it).

---
## RESULTS — Phase 0 & 1 complete (2026-11-25)
- **Phase 0:** Tonverk set to USB CONFIG=USB AUDIO/MIDI → one card, S24_3LE @ 48k, 4 capture / 2 playback. Channel map confirmed: OUT CD → capture **3/4** (1/2 silent). Playback → USB AUDIO IN=OUT AB return verified audible. Period range 48–48000.
- **Phase 1:** Builds with **-march=core2 -msse4.1 -mtune=core2 -mfpmath=sse**. 21 tests / 59430 assertions pass; Lifebook output bit-identical to local Xeon render.
- **CPU headroom:** with SSE flush-to-zero/DAZ ON, reverb runs **~8.9x realtime** on the Lifebook (23s for 205s audio). Without FTZ the denormal-laden tail makes it >8x slower (unusable). **FTZ/DAZ MUST be set in the Phase-2 audio thread.**

## RESULTS — Phase 2 core complete (2026-11-25)
- Live duplex **pristine** at 256-frame buffer: 0 xruns, 0 blowups, ~13% of one core, 48 kHz.
- **miniaudio duplex REJECTED** — its bridge glitches on this hardware (7s silence then growing stutter) despite 0 DSP overruns; raw ALSA concurrent duplex is provably clean, so the bridge was the fault.
- **New path = `verb_live` (tui/live.cpp):** direct ALSA, `hw:1,0` capture(4ch)+playback(2ch) S24_3LE, `snd_pcm_link` (single clock), blocking read→reverb(ch3/4)→write loop, inline S24_3LE↔float. Reuses Engine::RenderLive.
- TODO: realtime limits (`@audio rtprio 95 / memlock`, user in `audio` group) for SCHED_FIFO — insurance for when the Phase-3 TUI shares the CPU.
