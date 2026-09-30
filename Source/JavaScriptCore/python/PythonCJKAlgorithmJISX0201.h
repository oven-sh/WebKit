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

// lib/convert-cjk-codecs.py made this from Modules/cjkcodecs/alg_jisx0201.h of CPython 3.14. It is not to be changed by hand.
// It is for the files that are made with it to include, in a namespace, and nothing else.

#define JISX0201_R_ENCODE(c, assi)                      \
    if ((c) < 0x80 && (c) != 0x5c && (c) != 0x7e) {     \
        (assi) = (c);                                   \
    }                                                   \
    else if ((c) == 0x00a5) {                           \
        (assi) = 0x5c;                                  \
    }                                                   \
    else if ((c) == 0x203e) {                           \
        (assi) = 0x7e;                                  \
    }

#define JISX0201_K_ENCODE(c, assi)                      \
    if ((c) >= 0xff61 && (c) <= 0xff9f) {               \
        (assi) = (c) - 0xfec0;                          \
    }

#define JISX0201_ENCODE(c, assi)                        \
    JISX0201_R_ENCODE(c, assi)                          \
    else JISX0201_K_ENCODE(c, assi)

#define JISX0201_R_DECODE_CHAR(c, assi)                 \
    if ((c) < 0x5c) {                                   \
        (assi) = (c);                                   \
    }                                                   \
    else if ((c) == 0x5c) {                             \
        (assi) = 0x00a5;                                \
    }                                                   \
    else if ((c) < 0x7e) {                              \
        (assi) = (c);                                   \
    }                                                   \
    else if ((c) == 0x7e) {                             \
        (assi) = 0x203e;                                \
    }                                                   \
    else if ((c) == 0x7f) {                             \
        (assi) = 0x7f;                                  \
    }

#define JISX0201_R_DECODE(c, writer)                    \
    if ((c) < 0x5c) {                                   \
        OUTCHAR(c);                                     \
    }                                                   \
    else if ((c) == 0x5c) {                             \
        OUTCHAR(0x00a5);                                \
    }                                                   \
    else if ((c) < 0x7e) {                              \
        OUTCHAR(c);                                     \
    }                                                   \
    else if ((c) == 0x7e) {                             \
        OUTCHAR(0x203e);                                \
    }                                                   \
    else if ((c) == 0x7f) {                             \
        OUTCHAR(0x7f);                                  \
    }

#define JISX0201_K_DECODE(c, writer)                    \
    if ((c) >= 0xa1 && (c) <= 0xdf) {                   \
        OUTCHAR(0xfec0 + (c));                          \
    }
#define JISX0201_K_DECODE_CHAR(c, assi)                 \
    if ((c) >= 0xa1 && (c) <= 0xdf) {                   \
        (assi) = 0xfec0 + (c);                          \
    }
#define JISX0201_DECODE(c, writer)                      \
    JISX0201_R_DECODE(c, writer)                        \
    else JISX0201_K_DECODE(c, writer)
