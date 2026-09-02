# Rations

A four-channel amp head built on [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler)
captures. A **raw VST3** plug-in — no JUCE, no iPlug2, no VSTGUI — for **Linux and Windows**, plus
a JACK standalone on Linux for playing it without a DAW.

A `.nam` capture freezes an amp at one knob position on one channel. A real amp head has several
channels, each with its own gain range, and you change channel with your foot mid-song. Rations
gives you that back.

## What it is

**Four channels — Clean, Crunch, OD1 and OD2 until you rename them.** Each loads its own bank of
captures, and its dial sweeps that whole bank *continuously*: rest on one capture and you are
playing it exactly, in between you are hearing the two either side blended. Capture your amp at
each mark of its own gain control and that dial is the amp's gain control, at the amp's own
spacing.

**Exactly one channel sounds at a time**, chosen by a bat switch or a MIDI footswitch. The change
is instant and inaudible — about 18 ms, with no click and no gap — which is the whole reason this
plug-in exists. It is not a crossfade over a hard swap: the three idle channels are fed
continuously on a worker thread so the one you stomp to is already exact when you get there.

**Around them:** a shared Threshold / Bass / Middle / Treble section, Input and Output, and BYPASS
/ EQ / GATE. A cabinet page that loads one or two impulse responses with a blend between them. A
pedalboard of five pedals — Boost and Chorus before the amp, Flanger, Delay and Reverb after it.
A settings page behind the gear carrying the capture loaders, a trim per channel, the MIDI-learn
rows and the output section.

**You load your own captures.** Nothing ships in the bundle — a fresh instance comes up with four
empty channels, which is an ordinary state rather than an error. Point each loader at a folder of
`.nam` files or at a single one; the folder's name becomes the channel's name, and you can type
over it.

Captures must be feed-forward (WaveNet or ConvNet). An LSTM capture is refused rather than
silently accepted, because the click-free sweep rests on the model's output being a function of a
bounded input window, and an LSTM's cell state has unbounded memory.

## Building

Dependencies are pinned git submodules. cairo, FreeType and fontconfig come from the system.

```
git submodule update --init --recursive NeuralAmpModelerCore AudioDSPTools eigen
git submodule update --init vst3sdk
git -C vst3sdk submodule update --init base cmake pluginterfaces public.sdk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That produces `build/VST3/Release/Rations.vst3`. Copy or symlink it into `~/.vst3/`.

### The standalone (Linux)

The same build also produces `build/rations-standalone` when JACK's development files are present
(`libjack-jackd2-dev`): the amp without a DAW, in a window of its own, with the plug-in's own
editor in it and a MIDI port for a footswitch.

```
./build/rations-standalone
```

It is a **host**, not a second copy of the plug-in: it loads `Rations.vst3` the way a DAW does —
which is what keeps the editor resolving its art and fonts by the same route in both — looking in
`$RATIONS_VST3`, then beside itself, then the build tree, `~/.vst3`, `/usr/local/lib/vst3` and
`/usr/lib/vst3`. It registers `Rations:in`, `Rations:out_l`, `Rations:out_r` and `Rations:midi_in`,
connects the audio to the first physical ports it finds, and leaves the MIDI port for you to patch.
What it was playing is kept in `~/.config/Rations/standalone.state`; `--no-state` starts empty.

`scripts/makedist-linux.sh` packages the two into a tarball with a launcher entry and an
`install.sh`. There is no standalone on Windows — that release is the plug-in only.

### Windows, cross-built from Linux

No Windows machine and no VM: MinGW-w64, with the graphics dependencies built into a private
static sysroot, verified under Wine.

```
scripts/build-win-deps.sh          # zlib, libpng, pixman, freetype, cairo
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
cmake --build build-win
scripts/makedist-windows.sh        # gates, installer, ZIP into dist/
```

Everything is linked statically, so the bundle has no redistributable and nothing sits beside the
binary. Wine is a smoke test, not certification — a real Windows machine is the gate before a
release.

## Testing

The proofs are the point, not an afterthought — a fast channel switch that is not *exact* is a
worse product than a slow one. `tools/` holds offline proofs that drive the built bundle the way a
host does, and `scripts/` holds the gates that run them:

| | |
|---|---|
| `scripts/switch-gate.sh` | the channel switch in a live JACK process callback, zero xruns |
| `scripts/ir-gate.sh` | the two-IR blend against real cabinet impulse responses |
| `scripts/pedal-gate.sh` | each pedal against the source its topology was derived from |
| `rations_switchcheck` | post-catch-up convergence on a running reference to within 1e-6 |
| `rations_racecheck` | the prime worker under ThreadSanitizer |
| `panelrender` | every editor page rendered offline, with art and text-clearance audits |

The offline proofs need capture banks to run against, since the plug-in ships none: pass
`--captures <dir>` (a directory holding one subdirectory per channel) or set
`$RATIONS_TEST_CAPTURES`. `rations_pedalcheck` needs none.

## Licence

MIT — see [LICENSE](LICENSE). No GPLv3 code and no JUCE header enters the include graph, which is
a deliberate constraint rather than an accident.

Third-party attribution is in [NOTICE](NOTICE): the VST3 SDK (MIT), NeuralAmpModelerCore and
AudioDSPTools (MIT), Eigen (MPL-2.0), NanoSVG and WDL (zlib), and the fonts (OFL / Apache-2.0).
It matters more for the Windows build than the Linux one, because that bundle statically links
cairo, pixman, FreeType, libpng and zlib and therefore redistributes them.

Captures are not covered by the MIT grant on the code — they are their authors' own work, and
none ship here.
