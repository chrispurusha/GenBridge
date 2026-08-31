# Third-Party Software

GenBridge is licensed under the **GNU General Public License v3.0** (see `LICENSE`).

This file records what third-party code is actually **inside the shipped `GenBridge.vst3`
binary**, which is not the same as what the repository can build against. It is the list that
matters for a binary release, because two of these carry attribution requirements that must
travel with the `.dmg`.

Verified against the built binary with `otool -L`, `nm -u` and `strings`, not from the build
script alone — the two disagree, and the binary is the thing that ships.

---

## In the shipped binary

### FreeType 2 (statically linked)

- **Version bundled**: 2.14.3
- **Website**: https://freetype.org
- **Source**: https://github.com/freetype/freetype
- **Licence**: The FreeType Project License (FTL) — see `SynthLib/ThirdParty/freetype/docs/FTL.TXT`

FreeType is dual-licensed FTL or GPLv2. **GenBridge takes the FTL option.** FreeType's own
`LICENSE.TXT` is explicit that the FTL "is compatible to the GNU General Public License version 3,
but not version 2" — so a GPLv3 project must take the FTL, and a GPLv2 one could not.

**Binary redistribution obligation (FTL §2):** a binary distribution must provide, *in the
distribution documentation*, a disclaimer stating the software is based in part on the work of
the FreeType Team. This is mandatory, not a courtesy. `do-release` writes it into the `.dmg`'s
`Read Me First.txt` and copies `FTL.TXT` into the disk image; the release fails if either file is
missing rather than shipping without them.

The FTL also asks (§3) that neither party use the other's name for promotional purposes without
permission.

### VST3 SDK — interface headers and IID definitions (statically linked)

- **Website**: https://github.com/steinbergmedia/vst3sdk
- **Licence**: MIT, Copyright (c) 2025 Steinberg Media Technologies GmbH

Only four SDK translation units are compiled in — `funknown.cpp`, `coreiids.cpp`,
`vstinitiids.cpp` and `commoniids.cpp` — plus the `pluginterfaces/` headers. No CMake, no VSTGUI,
no `public.sdk` helper classes. Those four files contain no logic; they exist to instantiate the
interface IDs the headers declare.

Since 2025 the SDK is MIT, **not** the older dual GPLv3/proprietary arrangement, so it is
compatible with GPLv3 without the proprietary-licence question arising.

**Binary redistribution obligation (MIT):** the copyright notice and permission notice must be
included in all copies or substantial portions. `do-release` copies the SDK's `LICENSE.txt` into
the `.dmg`.

---

## NOT in the shipped binary

Listed because `SynthLib/THIRD_PARTY.md` covers them and they are easy to assume are present.

| Library | Why it is absent |
|---|---|
| **GLFW** (zlib/libpng) | Compiled out by `G2_VST3_BUILD`. A plug-in is handed a window by its host and never creates one. Confirmed: zero GLFW strings and zero undefined GLFW symbols in the binary. |
| **libusb** (LGPL v2.1) | Used by G2-Edit to talk to a Nord G2. GenBridge speaks CoreAudio and CoreMIDI and never links it. |

Everything else the plug-in links is an Apple system framework: CoreAudio, CoreMIDI,
CoreFoundation, Cocoa/AppKit, Metal, QuartzCore, OpenGL, libc++, libobjc, libSystem.

---

## SynthLib

`SynthLib/` is a submodule of first-party code under GPLv3 (`SynthLib/LICENSE`), with the licence
header on every source file. It is compiled into the plug-in as source, so it raises no separate
distribution obligation beyond GenBridge's own.

Its `THIRD_PARTY.md` describes the libraries SynthLib *can* bundle across all four projects, which
is a broader list than what any one binary contains — see the table above.

---

## Source availability (GPLv3 §6)

Anyone given a GenBridge binary receives GPLv3 rights, which include the right to the
corresponding source. Publishing the `.dmg` therefore requires the source for that version to be
reachable by the people who get it — a public repository at the tagged version is the simplest way
to satisfy this, and is what the `.dmg` points at.
