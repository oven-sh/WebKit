/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// lib/convert-cjk-codecs.py made this from Modules/cjkcodecs/emu_jisx0213_2000.h of CPython 3.14. It is not to be changed by hand.
// It is for the files that are made with it to include, in a namespace, and nothing else.

/* These routines may be quite inefficient, but it's used only to emulate old
 * standards. */

#ifndef EMULATE_JISX0213_2000_ENCODE_INVALID
#  define EMULATE_JISX0213_2000_ENCODE_INVALID 1
#endif

#define EMULATE_JISX0213_2000_ENCODE_BMP(config, assi, c)               \
    if ((config) == (void *)2000 && (                                   \
                    (c) == 0x9B1C || (c) == 0x4FF1 ||                   \
                    (c) == 0x525D || (c) == 0x541E ||                   \
                    (c) == 0x5653 || (c) == 0x59F8 ||                   \
                    (c) == 0x5C5B || (c) == 0x5E77 ||                   \
                    (c) == 0x7626 || (c) == 0x7E6B)) {                  \
        return EMULATE_JISX0213_2000_ENCODE_INVALID;                    \
    }                                                                   \
    else if ((config) == (void *)2000 && (c) == 0x9B1D) {               \
        (assi) = 0x8000 | 0x7d3b;                                       \
    }

#define EMULATE_JISX0213_2000_ENCODE_EMP(config, assi, c)               \
    if ((config) == (void *)2000 && (c) == 0x20B9F) {                   \
        return EMULATE_JISX0213_2000_ENCODE_INVALID;                    \
    }

#ifndef EMULATE_JISX0213_2000_DECODE_INVALID
#  define EMULATE_JISX0213_2000_DECODE_INVALID 2
#endif

#define EMULATE_JISX0213_2000_DECODE_PLANE1(config, assi, c1, c2)       \
    if ((config) == (void *)2000 &&                                     \
                    (((c1) == 0x2E && (c2) == 0x21) ||                  \
                     ((c1) == 0x2F && (c2) == 0x7E) ||                  \
                     ((c1) == 0x4F && (c2) == 0x54) ||                  \
                     ((c1) == 0x4F && (c2) == 0x7E) ||                  \
                     ((c1) == 0x74 && (c2) == 0x27) ||                  \
                     ((c1) == 0x7E && (c2) == 0x7A) ||                  \
                     ((c1) == 0x7E && (c2) == 0x7B) ||                  \
                     ((c1) == 0x7E && (c2) == 0x7C) ||                  \
                     ((c1) == 0x7E && (c2) == 0x7D) ||                  \
                     ((c1) == 0x7E && (c2) == 0x7E))) {                 \
        return EMULATE_JISX0213_2000_DECODE_INVALID;                    \
    }

#define EMULATE_JISX0213_2000_DECODE_PLANE2(config, writer, c1, c2)     \
    if ((config) == (void *)2000 && (c1) == 0x7D && (c2) == 0x3B) {     \
        OUTCHAR(0x9B1D);                                                \
    }

#define EMULATE_JISX0213_2000_DECODE_PLANE2_CHAR(config, assi, c1, c2)  \
    if ((config) == (void *)2000 && (c1) == 0x7D && (c2) == 0x3B) {     \
        (assi) = 0x9B1D;                                                \
    }

