// Copyright 2026 NAMp authors
// SPDX-License-Identifier: ISC
//
// Compatibility shim so jalv's lv2_evbuf.[ch] compile here unmodified in substance.
//
// The vendored files are jalv 1.8.2's, byte-for-byte except that each replaces one include with
// this header. They need exactly two things this tree does not otherwise supply:
//
//   * jalv_config.h, which is jalv's build-configuration header. lv2_evbuf.c reads a single symbol
//     from it, USE_POSIX_MEMALIGN. Vendoring the whole configuration header to answer one question
//     would import a pile of unrelated feature switches, so the answer is given directly. It is 1
//     because posix_memalign is POSIX.1-2001 and this project is Linux-only; an LV2_Atom_Sequence
//     must be 8-byte aligned, and malloc on glibc already guarantees 16, so this is belt and braces
//     rather than a portability question.
//
//   * ZIX_REALTIME, an annotation macro. It marks a function as callable from a real-time thread and
//     expands to nothing that affects code generation. It arrived in zix 0.8; the development
//     machine's zix is 0.6.2, whose zix/attributes.h does not define it. Defining it empty when
//     absent keeps the upstream annotations in place — they document exactly the property the
//     real-time path here cares about — without pinning a newer zix for a macro.
//
//     AND THE HEADER ITSELF MAY NOT BE THERE AT ALL, which is why the include is guarded rather
//     than written plainly. zix is a standalone library only from Debian trixie onwards; on the
//     RELEASE machine (Debian 12 / Devuan 5, which is where this is packaged for the ABI baseline
//     in scripts/dist-common.sh) lilv 0.24.14 vendors zix PRIVATELY and installs no zix headers,
//     so <zix/attributes.h> does not exist and the build stopped dead on this file. Measured, not
//     deduced: that is the error the first release run on the packaging machine produced.
//
//     Guarding costs nothing, because ZIX_REALTIME is the ONLY zix token these two vendored files
//     use — grep them — and the fallback below already supplies it. Where zix IS installed the
//     include is taken and upstream's definition wins, so the development machine compiles exactly
//     what it compiled before; where it is not, the annotation degrades to the empty macro it
//     expands to anyway. There is no third behaviour to get wrong.
//
// See NOTICE for the licence record.

#ifndef NAMP_JALV_COMPAT_H
#define NAMP_JALV_COMPAT_H

#if defined(__has_include)
#  if __has_include(<zix/attributes.h>)
#    include <zix/attributes.h>
#  endif
#endif

#ifndef ZIX_REALTIME
#  define ZIX_REALTIME
#endif

#ifndef USE_POSIX_MEMALIGN
#  define USE_POSIX_MEMALIGN 1
#endif

#endif // NAMP_JALV_COMPAT_H
