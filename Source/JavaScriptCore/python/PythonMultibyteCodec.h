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

#pragma once

#include "PythonCodecs.h"

// What the codecs for Chinese, Japanese and Korean are to what runs them: Modules/cjkcodecs/multibytecodec.h of CPython. The names are as they are there, since what is written with them is CPython's own, converted:
// see lib/convert-cjk-codecs.py.

namespace JSC { namespace Python {

using ucs2_t = uint16_t;
using DBCHAR = uint16_t;
using Py_UCS4 = char32_t;
using Py_ssize_t = ptrdiff_t;

// Eight bytes for a codec to do as it likes with. getstate() gives them, so what they come to does not go by which way round the machine keeps a number.
struct MultibyteCodec_State {
    unsigned char c[8];
};

struct MultibyteCodec;

using mbcodec_init = int (*)(JSGlobalObject*, const MultibyteCodec*);
using mbencode_func = Py_ssize_t (*)(MultibyteCodec_State*, const MultibyteCodec*, const CodePoints* data, Py_ssize_t* inpos, Py_ssize_t inlen, unsigned char** outbuf, Py_ssize_t outleft, int flags);
using mbencodeinit_func = int (*)(MultibyteCodec_State*, const MultibyteCodec*);
using mbencodereset_func = Py_ssize_t (*)(MultibyteCodec_State*, const MultibyteCodec*, unsigned char** outbuf, Py_ssize_t outleft);
using mbdecode_func = Py_ssize_t (*)(MultibyteCodec_State*, const MultibyteCodec*, const unsigned char** inbuf, Py_ssize_t inleft, TextWriter*);
using mbdecodeinit_func = int (*)(MultibyteCodec_State*, const MultibyteCodec*);
using mbdecodereset_func = Py_ssize_t (*)(MultibyteCodec_State*, const MultibyteCodec*);

struct MultibyteCodec {
    const char* encoding;
    const void* config;
    mbcodec_init codecinit;
    mbencode_func encode;
    mbencodeinit_func encinit;
    mbencodereset_func encreset;
    mbdecode_func decode;
    mbdecodeinit_func decinit;
    mbdecodereset_func decreset;
    void* modstate; // The cjkcodecs_module_state of the module that it is of, which is not the same from one to another
};

// What a codec returns. More than nothing is how many are not to be made anything of.
constexpr Py_ssize_t MBERR_TOOSMALL = -1; // There is not room for what is to be written.
constexpr Py_ssize_t MBERR_TOOFEW = -2; // What there is to read stops part way through something.
constexpr Py_ssize_t MBERR_INTERNAL = -3;
constexpr Py_ssize_t MBERR_EXCEPTION = -4; // Something has been raised.

constexpr int MBENC_FLUSH = 0x0001; // There is no more to come, so whatever can be encoded is to be.
constexpr int MBENC_MAX = MBENC_FLUSH;

// What the capsules are called that one of these modules hands another
constexpr auto mapCapsuleName = "multibytecodec.map"_s;
constexpr auto codecCapsuleName = "multibytecodec.codec"_s;

} } // namespace JSC::Python
