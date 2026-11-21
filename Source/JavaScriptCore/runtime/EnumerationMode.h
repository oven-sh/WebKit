/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <cstdint>

namespace JSC {

// Which own keys a PropertyNameArrayBuilder takes.
// Strings is two bits. The second one asks for the index keys an object enumerates from its
// own storage: the elements of an array or a typed array, the characters of a String object,
// the mapped arguments. A caller that has no use for them (a completer, an inspector) passes
// NonIndexStrings, and the loops that build one Identifier for each of them do not run.
// Only those loops test the bit. A key that comes from anywhere else is added as it is: a
// name an ownKeys trap returns, an export of a module namespace that is named "0".
enum class PropertyNameMode : uint8_t {
    Symbols = 1 << 0,
    NonIndexStrings = 1 << 1,
    NonIndexStringsAndSymbols = Symbols | NonIndexStrings,
    Strings = NonIndexStrings | 1 << 2,
    StringsAndSymbols = Symbols | Strings,
};

enum class PrivateSymbolMode : uint8_t {
    Include,
    Exclude
};

enum class DontEnumPropertiesMode : uint8_t {
    Include,
    Exclude
};

} // namespace JSC
