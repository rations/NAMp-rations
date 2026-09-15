# NAMp

Two products, one tree, one amp — and five pedals that go with them.

Everything here is built on [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler)
captures, everything is raw VST3 — no JUCE, no iPlug2, no VSTGUI — and everything draws its panel
by hand with Cairo and FreeType. The two products share the same four-channel amp: the same DSP,
the same instant footswitch channel change, the same capture loading. They differ in what is built
around it.

| | |
|---|---|
| **[products/rations](products/rations)** | **NAMp Rations** — the amp head as a plug-in, for your DAW. A VST3 bundle for Linux, Windows and macOS (macOS is an experimental, untested build — reporting your result in the issues tab on GitHub is the only way I can improve it, as I do not have a Mac), an LV2 bundle on Linux, and a five-pedal pedalboard built in. |
| **[products/rack](products/rack)** | **NAMp Rack** — the amp as a program you run on its own, for Linux and Windows, with the same amp built in as the chain anchor. It is also a plug-in HOST: you load other people's plug-ins into the chain before and after the amp, so the pedalboard is whatever you own rather than five fixed ones. Linux hosts VST3 and LV2; Windows hosts VST3 only. **The five Rations Pedals ship with it.** |

**If you want the amp in your DAW, that is NAMp Rations. If you want to plug in and play without a
DAW, that is NAMp Rack.** You do not have to choose: there is **one download per platform** and it
holds both, plus the pedals. The installer asks which of the three you want.

Nothing here ships any captures. A fresh instance of either comes up with four empty channels,
which is an ordinary state rather than an error, and the first thing to do after installing is
load your own into them.

## What is on the releases page

[github.com/rations/NAMp-rations/releases](https://github.com/rations/NAMp-rations/releases)

**One file per platform. Each holds all three things**, and installs them with one script or one
installer.

| file | what it is |
|---|---|
| `NAMp-<v>-linux-x86_64.tar.gz` | the plug-in (VST3 + LV2), the program, and the five pedals — `./install.sh` |
| `NAMp-<v>-windows-x86_64.zip` | the same three, with `NAMp-install.exe` |
| `NAMp-rations-<v>-macos.zip` | the plug-in only, universal, **[experimental](#macos-experimental)** — there is no macOS build of the program |
| `namp-rack-<v>-corresponding-source.tar.gz` | source for the Windows program — see [Licence](#licence) |

Inside the Linux and Windows archives:

```
plugin/     NAMp Rations — the VST3, and on Linux the LV2 as well
rack/       NAMp Rack — the program, and on Linux its menu entry and icons
pedals/     the five Rations pedals, as ordinary VST3 bundles
```

Each part can be left out at install time, and each can be copied into place by hand instead;
nothing in the archive depends on anything else in it being installed.

The five pedals are **also their own release**, from
[github.com/rations/rations-pedals](https://github.com/rations/rations-pedals) — the same five
plug-ins, plus a standalone JACK application for each one so you can play a pedal on its own. You
do not need that download if you have NAMp Rack: the plug-ins in it are the same plug-ins. Take it
if you want the pedals without the amp, or want the standalones.

---

# NAMp Rations

![The NAMp Rations amp head](docs/amp-head.png)

![The pedalboard page](docs/pedalboard.png)

A four-channel amp head as a **raw VST3** plug-in for **Linux, Windows and macOS**, plus an **LV2**
build on Linux. The LV2 is the same amp and not a port: the same DSP and the same panel behind a
second set of callbacks, so pick whichever your host likes better.

A `.nam` capture freezes an amp at one knob position on one channel. A real amp head has several
channels, each with its own gain range, and you change channel with your foot mid-song. NAMp Rations
gives you that back.

## What it is

**Four channels — Clean, Crunch, OD1 and OD2 until you rename them.** Each loads its own bank of
captures, and its dial sweeps that whole bank *continuously*: rest on one capture and you are
playing it exactly, in between you are hearing the two either side blended. Capture your amp at
each mark of its own gain control and that dial is the amp's gain control, at the amp's own
spacing.

Number your captures 1 to 10 — `Gain-1.nam`, `Gain-2.nam` and so on. Each bank loads up to 64
captures at a time, so you can load folders of all different captures if you like.

**Exactly one channel sounds at a time**, chosen by a bat switch or a MIDI footswitch. The change
is instant and inaudible — about 18 ms, with no click and no gap — which is the whole reason this
plug-in exists. It is not a crossfade over a hard swap: the three idle channels are fed
continuously on a worker thread so the one you stomp to is already exact when you get there.

**Around them:** a shared Threshold / Bass / Middle / Treble section, Input and Output, and BYPASS
/ EQ / GATE. A cabinet page that loads one or two impulse responses with a blend between them. A
pedalboard of five pedals — Boost and Chorus before the amp, Flanger, Delay and Reverb after it.
A settings page behind the "Captures, MIDI, Settings" button, top right, carrying the capture
loaders, a trim per channel, the MIDI-learn rows and the output section.

**You load your own captures.** Nothing ships in the bundle. Point each loader at a folder of
`.nam` files or at a single one; the folder's name becomes the channel's name, and you can type
over it.

Captures must be feed-forward (WaveNet or ConvNet). An LSTM capture is refused rather than
silently accepted, because the click-free sweep rests on the model's output being a function of a
bounded input window, and an LSTM's cell state has unbounded memory.

---

# NAMp Rack

The same amp, in its own window, with no DAW — and a rack that hosts other people's plug-ins
before and after it.

**It is one program and it is self-contained.** The amp, its art and its fonts are inside the
binary, so there is nothing to install first and nothing for it to go looking for.

**The rack is the pedalboard, opened up.** NAMp Rations has five pedals drawn onto a page; the Rack
deletes that page and hosts plug-ins instead — yours, anybody's — in a chain before the amp and
after it. The pre-amp section is mono and the post-amp section is stereo, because that is where the
amp makes it stereo. Adding and removing plug-ins while audio is running is safe and is not heard.

**The five Rations Pedals come with it**, and they arrive as ordinary plug-ins rather than as a
built-in feature: they go into your plug-in folder, the Rack finds them by scanning it like any
other, and so does every other host on the machine. Nothing about them is special-cased.

**Plug-ins are scanned in a separate process on purpose.** One that crashes while it is being
examined takes that throwaway process with it and is recorded as bad rather than retried, so a
single broken plug-in on your disk cannot stop the Rack starting.

**Audio.** On **Linux** it is a JACK client — start a JACK server first (qjackctl, or
`jackd -R -d alsa -r 48000 -p 256`), or run a PipeWire desktop, which provides one. With no server
it still opens, so you can set your captures and your rack up, but it makes no sound and says so.
On **Windows** it opens **ASIO** first, because that is what your interface's own driver exposes
and what the latency depends on, and falls back to WASAPI on a machine with no ASIO driver.
`--list-devices` shows what it can see.

**Hosting differs by platform, by construction rather than by omission.** Linux hosts VST3 **and**
LV2. Windows hosts VST3 only: lilv and suil are Linux libraries and there is no Windows build of
this program that includes them, so LV2 plug-ins will not appear in the list there.

---

# Rations Pedals

Five stompboxes, each its own plug-in, each drawn at 1:1 with its own enclosure:

| | |
|---|---|
| `RationsBoost` | a Tube Screamer-style overdrive — Drive, Tone, Level |
| `RationsChorus` | two modulated taps per channel — Rate, Depth, Mix |
| `RationsFlanger` | swept comb with feedback — Rate, Depth, Manual, Regen |
| `RationsDelay` | tempo-syncable, optional ping-pong — Time, Feedback, Tone, Mix, Sync, Ping-Pong |
| `RationsReverb` | a Freeverb-lineage room — Decay, Tone, Pre-delay, Mix |

Install one, install all five; none of them needs any of the others, and none of them needs the
amp. Each has a footswitch that crossfades rather than clicking — a pedal switched off is genuinely
out of circuit and stops costing the audio thread anything — a separate bat toggle wired to the
host's own bypass parameter, and one MIDI-learn row for switching it with a foot controller.

**They reach you two ways and the plug-ins are identical either way.** They are in the NAMp Rack
download, installed with it. They are also their own release at
[rations-pedals](https://github.com/rations/rations-pedals), which adds a standalone JACK
application per pedal and a Windows installer. If you install both, the second one simply replaces
the first in your plug-in folder — you will not end up with two of each.

---

# Installing

Nothing below needs root or an administrator except where it says so, and nothing is installed
outside your home directory on Linux or macOS.

## Linux

One archive, one script, all three parts.

```
tar xf NAMp-*-linux-x86_64.tar.gz
cd NAMp-*/
./install.sh
```

| goes to | what |
|---|---|
| `~/.vst3/NAMp-rations.vst3` | the plug-in, VST3 |
| `~/.lv2/rations.lv2` | the plug-in, LV2 |
| `~/.vst3/RationsBoost.vst3` … `RationsReverb.vst3` | the five pedals |
| `~/.local/bin/namp-rack` | the program |
| `~/.local/share/applications/namp-rack.desktop` | its menu entry |
| `~/.local/share/icons/hicolor/<size>/apps/namp-rack.png` | its icon, at 48, 64, 128 and 256 |

Nothing needs root and nothing lands outside your home directory. Then rescan plug-ins in your
DAW. `./install.sh --uninstall` removes every one of those again.

**The VST3 and the LV2 are the same amp twice** and are interchangeable — your host will find both
if both are installed, so use whichever it handles best and ignore the other. If a host shows you
an older version of the LV2, check for `rations.lv2` under `/usr/lib/lv2` or `/usr/local/lib/lv2`:
a copy there shadows the one in your home directory. The install script says so if it finds one.

**The pedals go to `~/.vst3`** because that is one of the folders the Rack's own scan looks in, so
they appear in its plug-in list after a rescan — and so does every other VST3 you already have
there. Your DAW will find them as well. They are ordinary plug-ins with no privileged route into
the Rack; delete them and it is unaffected.

**You do not have to install any of it.** Copy any bundle into `~/.vst3` or `~/.lv2` by hand, and
run the program straight out of the archive with `./rack/namp-rack` — it also scans its own
directory, so the pedals beside it are found without installing anything.

`--uninstall` deliberately does **not** remove your settings: `~/.config/NAMp-rations` and
`~/.config/NAMp-Rack` hold your saved racks, which captures were loaded, your audio device choice
and your MIDI bindings, and `~/.cache/NAMp-Rack` holds the plug-in scan cache, which can be
deleted at any time and is rebuilt by a rescan.

**Requirements:** the plug-in, the LV2 and the pedals need cairo, freetype2, fontconfig and libX11,
which a desktop Linux install already has. The program needs those plus lilv and suil, the JACK
client library and a running JACK server. On Debian / Devuan / Ubuntu:
`sudo apt install jackd2 liblilv-0-0 libsuil-0-0`. On a PipeWire desktop, `pipewire-jack` provides
both the library and the server.

### Rations Pedals on their own

Only if you want them without NAMp Rack, or want the standalone applications.

```
tar xf RationsPedals-*-linux-x86_64.tar.gz
cd RationsPedals-*/
./install.sh
```

| goes to | what |
|---|---|
| `~/.vst3/Rations<Pedal>.vst3` | the five plug-ins |
| `~/.local/bin/rations-<pedal>-standalone` | one JACK application per pedal |
| `~/.local/share/applications/rations-<pedal>.desktop` | five menu entries |
| `~/.local/share/icons/hicolor/<size>/apps/rations-<pedal>.png` | their icons |

`./install.sh boost reverb` installs just those two. `./install.sh --uninstall` removes them all.

## Windows

One archive, one installer, all three parts.

**With the installer.** Run `NAMp-install.exe`. It asks which of the three you want — the plug-in,
the pedals, the program — and installs those. It is not code-signed, so SmartScreen shows a blue
"Windows protected your PC" box: click *More info*, then *Run anyway*, or install by hand instead;
the two put exactly the same folders in exactly the same places.

| run as | plug-in and pedals | program |
|---|---|---|
| administrator | `C:\Program Files\Common Files\VST3\` (every user) | `C:\Program Files\NAMp\` |
| normally | `%LOCALAPPDATA%\Programs\Common\VST3\` (just you) | `%LOCALAPPDATA%\Programs\NAMp\` |

It tells you which, and you can change the VST3 folder. The program gets a Start Menu entry. Remove
all of it later from *Apps & features*, or with the uninstaller it leaves beside the program.

**By hand.** The same bundles are loose in the ZIP, which is the fallback for a machine whose
SmartScreen or antivirus refuses an unsigned installer:

| | |
|---|---|
| `plugin\NAMp-rations.vst3` | copy the whole **folder** into one of the two VST3 directories above |
| `pedals\Rations<Pedal>.vst3` | the same, five more folders |
| `rack\namp-rack.exe` | the whole program — put it anywhere and run it |

Then rescan plug-ins in your DAW. Do not rename anything inside a bundle: each carries its own art
and fonts in `Contents\Resources`, and the binary in `Contents\x86_64-win` must keep the bundle's
own name or no host will load it. To uninstall a hand-installed copy, delete the folder.

Either way, keep only **one** copy of each bundle: hosts scan both directories, so a copy in each
shows up twice in the plug-in list. The installer offers to remove the other one for you.

The Rack also scans its own directory, so leaving `pedals\` beside the .exe works without copying
anything anywhere.

Your settings live in `%APPDATA%\NAMp-Rack` — saved racks, which captures were loaded, your audio
device choice and your MIDI bindings. The plug-in scan cache is in `%LOCALAPPDATA%\NAMp-Rack` and
can be deleted at any time; a rescan rebuilds it.

**Audio.** If you have an audio interface, install its manufacturer's ASIO driver and use that.
ASIO4ALL is a wrapper rather than a driver and will work, but the latency is not what a real driver
gives you. With no ASIO driver at all the Rack falls back to WASAPI.

**Requirements:** 64-bit Windows and nothing else. cairo, FreeType, libpng and zlib are statically
linked, and so is the C++ runtime. There is no 32-bit build.

**Note on the licence:** `namp-rack.exe` is the one binary in this archive that is **GPLv3 rather
than MIT**, because it has the ASIO SDK compiled in. The plug-in and the pedals beside it are MIT
and stay MIT — see [Licence](#licence).

### Rations Pedals on their own

Only if you want them without NAMp Rack, or want the standalone applications. The ZIP holds
`RationsPedals-install.exe`, which puts the same five folders in the same two places the NAMp
Rations installer offers, with the same SmartScreen warning and the same *Apps & features* entry.

## macOS (experimental)

**NAMp Rations only.** NAMp Rack is a JACK client and a Linux and Windows program; the macOS
equivalent would be a different program rather than the same one rebuilt. There is no macOS build
of the pedals either.

**Read this part before the instructions.** The macOS build is offered as an experiment, and the
honest summary is that it may not work:

- **Nobody who works on this has a Mac.** Linux and Windows are both built and *proved* on one
  Linux machine — Windows works because Wine runs every check on it there. There is no equivalent
  for macOS, so the build is made by GitHub's macOS runners and has never been played by anyone
  who could fix it.
- **The sound is unmeasured on macOS.** On Linux and Windows a set of offline proofs asserts exact
  figures — the channel switch converging on a continuously-running reference to within 1e-6, the
  output modes to three decimal places. Those proofs need capture files, captures are their
  authors' work rather than ours, and so they cannot run on a hosted build machine. What *is*
  checked on both Mac architectures is that the plug-in builds, loads, passes Steinberg's own
  validator, exports exactly the three entry points a host calls, links nothing outside the system,
  and renders all four editor pages, with the Apple Silicon and Intel renders compared against each
  other pixel for pixel. That is a floor, not a guarantee.

If it misbehaves, [an issue](https://github.com/rations/NAMp-rations/issues) with your Mac, your
DAW and what happened is genuinely useful — it is the only way any of this gets found.

The bundle is **universal**: one file carrying both Apple Silicon and Intel, macOS 11 (Big Sur) or
later. Nothing else to install — cairo, FreeType, libpng, pixman and zlib are built into it.

**Installing needs Terminal once, for about thirty seconds, and you do not have to type any file
names.** It cannot be done entirely in Finder because the plug-in is not notarized by Apple (that
needs a paid developer account); macOS marks anything downloaded as quarantined and refuses to load
quarantined code that Apple has not signed off, so the flag has to be cleared. `install.sh` does
that for you.

1. **Double-click the .zip** you downloaded. A folder appears beside it.
2. **Open Terminal:** hold **Command** and press the **space bar**, type `terminal`, press
   **Return**.
3. In that window type the four letters `bash` and then **one space**. Do not press Return yet.
4. **Drag the file `install.sh`** out of the folder and drop it onto the Terminal window — its
   location appears after what you typed, so there is nothing to spell.
5. Press **Return**.

It prints `Installed:` and a location. Rescan plug-ins in your DAW and NAMp Rations is there. The
plug-in goes to `~/Library/Audio/Plug-Ins/VST3`; nothing needs an administrator password and
nothing is put outside your home folder.

To remove it, repeat those steps and type one space and `--uninstall` before pressing Return.

If you would rather copy it by hand: drag `NAMp-rations.vst3` into
`~/Library/Audio/Plug-Ins/VST3`, then run `xattr -cr ~/Library/Audio/Plug-Ins/VST3/NAMp-rations.vst3`
in Terminal once. That last step is not optional — without it the DAW finds the plug-in, fails to
load it, and usually does not say why.

---

# Building

One product per build directory. That is enforced rather than advised: the VST3 SDK writes its
bundle to a fixed path under the build directory, so two products configured into one would write
two bundles to the same place.

```sh
# Not `--init --recursive`: the SDK's doc/, tutorials/ and vstgui4/ are not built here, and two of
# the other submodules declare nested copies of trees this repository already has at its root.
git submodule update --init NeuralAmpModelerCore AudioDSPTools eigen rations-pedals
git submodule update --init vst3sdk
git -C vst3sdk submodule update --init base cmake pluginterfaces public.sdk

cmake -S . -B build                              # NAMp Rations (the default)
cmake --build build

cmake -S . -B build-rack -DNAMP_PRODUCT=rack     # NAMp Rack, and the five pedals with it
cmake --build build-rack
```

The four dependencies — the VST3 SDK, NeuralAmpModelerCore, AudioDSPTools and Eigen — are
submodules at the repository root, shared by both products at one pinned version each. The pedals
are a fifth submodule, pinned the same way and built by the Rack's own build;
`-DNAMPRACK_BUILD_PEDALS=OFF` leaves them out. Each product's own `CMakeLists.txt` is the whole of
its build; the one at the root only chooses between them.

Per-product build notes, dependencies and platform details are in each product's own directory.

## Packaging a release

The release is **one package per platform**, so it is built by one script at the root rather than
one per product:

```sh
scripts/makedist-linux.sh      # -> dist/NAMp-<v>-linux-x86_64.tar.gz
scripts/makedist-windows.sh    # -> dist/NAMp-<v>-windows-x86_64.zip, with NAMp-install.exe in it
```

Each configures and builds both products — two build directories, for the reason above — and calls
`products/*/scripts/stage-{linux,windows}.sh` to gate and stage them into one tree. The per-artefact
checks live in those stage scripts, beside the product that owns them: what each binary links, what
it exports, what is inside each bundle, the ASIO markers, the pixel-for-pixel comparison of the
Windows editor against the Linux one. What the root scripts own is what is genuinely shared — the
version (cross-checked between the two products before anything is built), the licence files, the
install script or installer, and the archive.

macOS is `products/rations/scripts/makedist-mac.sh` and stays per-product, because there is no
macOS build of the Rack for it to be packaged with.

Both archives are byte-reproducible from a given commit: the members' timestamps, order and
ownership are normalised, and the Windows PE timestamps are zeroed after stripping, which `strip`
would otherwise refill from the wall clock.

# Licence

## The short version

**MIT** — see [LICENSE](LICENSE) — for all of the source, and for every binary on the releases page
**except one**.

**The Windows build of NAMp Rack is GPLv3.** Nothing else is. If you are not downloading that file,
the rest of this section is background.

## Why one binary is GPLv3

NAMp Rack on Windows hosts **ASIO**, because that is what an audio interface's own driver exposes
and what the latency depends on. Steinberg's ASIO SDK is dual-licensed — a proprietary agreement,
or GPLv3 — and this project takes the **GPLv3 arm** rather than enter a private contract. That
makes the resulting binary a combined work containing GPLv3 code, so it is conveyed under GPLv3.

**No source file changed licence, and none had to.** MIT combines into a GPLv3 work; the
compatibility runs that way round. The project's code is MIT wherever you find it, including in the
source of that very binary.

What is GPLv3: `namp-rack.exe` for Windows, built with ASIO.
What is MIT: everything else — the Linux Rack, both VST3 bundles, the LV2 bundle, the five pedals,
and a Windows Rack built without ASIO.

**Your rights, and the source.** You may run, study, modify, redistribute and distribute modified
versions of that binary under GPLv3. Its Complete Corresponding Source is published beside it as
`namp-rack-<v>-corresponding-source.tar.gz` — this repository at a named commit, its five pinned
submodules at their exact SHAs, the five libraries statically linked into it as their upstream
archives, and the ASIO SDK files compiled in, with a SHA-256 of every file. The build is
bit-reproducible, so you can rebuild it and compare the bytes rather than take anyone's word for
it. The archive itself carries `COPYING.GPL-3`, `CORRESPONDING-SOURCE.txt` and a one-page
`LICENSE-windows-asio.txt`.

Running proprietary plug-ins in the GPLv3 Rack is your own act and GPLv3 says nothing about it —
every VST3 you own works there as it would in any other host.

## Third-party components

[NOTICE](NOTICE) at the root of this repository carries the full text and the reasoning for all of
it, and ships in every archive. The pedals carry their own beside them, as `pedals/NOTICE`. What
follows is the summary.

**In everything:**

| | | |
|---|---|---|
| VST 3 SDK 3.8.0 | MIT | the plug-in ABI and the hosting classes |
| NeuralAmpModelerCore | MIT | the amp models |
| AudioDSPTools | MIT | DSP support for the above |
| Eigen | MPL-2.0 | the linear-algebra backend |
| NanoSVG | zlib | the interface's SVG icons |
| ToneStack (from NeuralAmpModelerPlugin) | MIT | the Bass / Middle / Treble stage |
| Michroma-Regular.ttf | SIL Open Font License 1.1 | the panel legends |
| Roboto-Regular.ttf | Apache License 2.0 | the value readouts |

**NAMp Rations also:** WDL (zlib) for the pedalboard's reverb engine, derived from the
public-domain Freeverb by Jezar at Dreampoint.

**NAMp Rack on Linux also:** the LV2 stack — lilv, suil, serd, sord, sratom, zix and jalv's
`lv2_evbuf.c` — all **ISC**, plus the LV2 specification itself (ISC).

**The graphics stack differs by platform, and that changes what is redistributed.** On Linux cairo,
FreeType, fontconfig and libX11 are system libraries: present on your machine, linked at run time,
and not shipped here. On Windows there is no system copy of any of them, so the Windows binaries
**statically link and therefore redistribute** them:

| | | |
|---|---|---|
| cairo 1.18.4 | LGPL 2.1-or-later **or** MPL 1.1 | see the election below |
| pixman 0.44.0 | MIT | |
| FreeType 2.13.3 | FTL **or** GPLv2 | the FTL is elected |
| libpng 1.6.48 | libpng/zlib | |
| zlib 1.3.1 | zlib | |

**Two of those are dual-licensed and the choice is recorded rather than left open.**

- **cairo** is taken under **LGPL 2.1**. For the MIT binaries either arm would serve. For the GPLv3
  Windows Rack the election is required rather than preferred: MPL 1.1 is not GPL-compatible, and
  LGPL 2.1 section 3 is what expressly permits applying the GPL to a copy of the library. Static
  linking is permitted by LGPL 2.1 section 6 on the condition that you can relink against a
  modified cairo, and that condition is met by publication: the complete source is public, the
  exact cairo version and the exact configuration used to build it are in
  `scripts/build-win-deps.sh`, and the whole program rebuilds from that with one command.
- **FreeType** is taken under the **FTL**, which its own licence file records as compatible with
  GPLv3 but not GPLv2. Its required credit, verbatim: *Portions of this software are copyright (c)
  2024 The FreeType Project (www.freetype.org). All rights reserved.*

**The Windows installers** are built with **NSIS 3.11** (zlib-style licence), which is a build tool
and contributes nothing to the include graph.

**ASIO**, in the Windows Rack only: Steinberg ASIO SDK 2.3.4, copyright (c) 2025 Steinberg Media
Technologies GmbH, under the GPLv3 arm of its dual licence. The SDK's `host/` files carry a
BSD-3-Clause notice of their own, reproduced in full in [NOTICE](NOTICE) as that licence
requires of a binary redistribution.

> ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other
> countries.

VST is a trademark of Steinberg Media Technologies GmbH.

## Captures and impulse responses

**Not covered by any of the above, and nothing here ships any.** They are recordings made by
whoever trained them, under their own terms. You load your own, and what you may do with them is
between you and whoever made them.
