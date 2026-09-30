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

#include "PythonBytes.h"

namespace JSC { namespace Python {

// What the modules that hash have in common: Modules/hashlib.h of CPython. Those of the engine's are in PythonHashModules.cpp. _hashlib, which is over a library that a host may have, is the host's.

// HASHLIB_GIL_MINSIZE: how much there has to be for CPython to let other threads run meanwhile. A program can see it.
constexpr int minimumSizeToHashWithoutTheGIL = 2048;

// GET_BUFFER_VIEW_OR_ERROR(). Empty if it raised.
JS_EXPORT_PRIVATE Buffer bufferToHash(JSGlobalObject*, JSValue);
// _Py_hashlib_data_argument(): what is to be hashed to begin with can be called `data`, or `string` as it once was. Empty if there is none, or if it raised.
JS_EXPORT_PRIVATE JSValue dataToHash(JSGlobalObject*, JSValue data, JSValue string);
// _Py_strhex()
JS_EXPORT_PRIVATE String hexOf(std::span<const uint8_t>);

} } // namespace JSC::Python
