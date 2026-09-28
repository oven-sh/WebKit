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

#include "JSTypedArrays.h"
#include "PythonCodecs.h"

namespace JSC { namespace Python {

// bytes and bytearray are both Uint8Arrays. One whose class is bytes, or derived from it, is a bytes, and nothing in Python changes what
// is in it. Any other is a bytearray, and that includes every Uint8Array that JavaScript makes.
enum class BytesKind : uint8_t { None, Bytes, ByteArray };
BytesKind bytesKindOf(JSValue);
inline bool isBytes(JSValue value) { return bytesKindOf(value) == BytesKind::Bytes; }
inline bool isByteArray(JSValue value) { return bytesKindOf(value) == BytesKind::ByteArray; }

JSUint8Array* newBytes(JSGlobalObject*, std::span<const uint8_t>);
JSUint8Array* newByteArray(JSGlobalObject*, std::span<const uint8_t>);

// What is in anything that has bytes in it, one after another: bytes, bytearray, a memoryview that skips none, and any typed array or
// ArrayBuffer of JavaScript's. It is good until something is called that could resize it. Nothing if it is no such thing.
std::optional<std::span<const uint8_t>> tryBufferOf(JSValue);
// The same, raising TypeError: a bytes-like object is required, not 'str'.
std::optional<std::span<const uint8_t>> bufferOf(JSGlobalObject*, JSValue);

String reprOfBytes(std::span<const uint8_t>);
int64_t hashOfBytes(std::span<const uint8_t>);

void initializeBytesTypes(JSGlobalObject*);

} } // namespace JSC::Python
