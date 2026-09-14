# NAMp

Two products, one tree, one amp.

Both are built on [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler) captures,
both are raw VST3 — no JUCE, no iPlug2, no VSTGUI — and both draw their panel by hand with Cairo
and FreeType. They share the same four-channel amp: the same DSP, the same instant footswitch
channel change, the same capture loading. They differ in what is built around it.

| | |
|---|---|
| **[products/rations](products/rations)** | **NAMp Rations** — the amp head as a plug-in. A VST3 bundle for Linux, Windows and macOS, an LV2 bundle on Linux, and a five-pedal pedalboard built in. [Read its README](products/rations/README.md) for what it is and how to use it. |
| **[products/rack](products/rack)** | **NAMp Rack** — a plug-in HOST for Linux, with the same amp built in as the chain anchor. It hosts VST3 and LV2 plug-ins around that amp, so the pedalboard is other people's pedals rather than five fixed ones. Windows hosts VST3 only. |

They were two repositories until recently, kept in step by hand, and that failed silently at least
once — a parameter-default fix lived in one of them for months before reaching the other. One tree
is what makes an amp fix land once.

## Building

One product per build directory. That is enforced rather than advised: the VST3 SDK writes its
bundle to a fixed path under the build directory, so two products configured into one would write
two bundles to the same place.

```sh
git submodule update --init --recursive
cmake -S . -B build                              # NAMp Rations (the default)
cmake --build build

cmake -S . -B build-rack -DNAMP_PRODUCT=rack     # NAMp Rack
cmake --build build-rack
```

The four dependencies — the VST3 SDK, NeuralAmpModelerCore, AudioDSPTools and Eigen — are
submodules at the repository root, shared by both products at one pinned version each. Each
product's own `CMakeLists.txt` is the whole of its build; the one at the root only chooses between
them.

Per-product build notes, dependencies and platform details are in each product's own directory.

## Licence

MIT — see [LICENSE](LICENSE). Each product carries its own `NOTICE` listing the third-party code it
actually links, which is not the same list for both. Captures and impulse responses are **not**
covered by that grant and neither product ships any: they are recordings made by whoever trained
them, under their own terms, and you load your own.
