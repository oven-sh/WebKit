/*
 * Copyright (C) 2012-2017 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/IndexingType.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/Options.h>

namespace JSC {

// What is loaded from an array of int32s or of doubles is boxed anew, so that a whole float would come out a plain number.
// Given one, such an array becomes a contiguous one.
ALWAYS_INLINE bool isInt32ForInt32Shape(JSValue value)
{
    return Options::guardsWholeFloats(21) ? value.isPlainInt32() : value.isInt32();
}

// (NaN cannot be put there either, which is for whoever asks to see to: it is what a hole looks like.)
ALWAYS_INLINE bool isNumberForDoubleShape(JSValue value)
{
    return value.isNumber() && !(value.isWholeFloat() && Options::guardsWholeFloats(22));
}

inline IndexingType indexingTypeForValue(JSValue value)
{
    if (isInt32ForInt32Shape(value))
        return Int32Shape;

    if (isNumberForDoubleShape(value) && value.asNumber() == value.asNumber() && Options::allowDoubleShape())
        return DoubleShape;

    return ContiguousShape;
}

} // namespace JSC
