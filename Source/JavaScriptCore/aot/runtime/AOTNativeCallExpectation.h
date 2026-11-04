/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCodegenScratchWords.h"
#include "AOTRecordTypes.h"
#include <algorithm>
#include <array>

namespace JSC {

class JSGlobalObject;
class JSObject;

namespace AOT {

// What the compiler believes about one call: the overload it resolved, if any, and the effects it gave the call.
struct NativeCallExpectation {
    static constexpr unsigned mayThrowBit = 32;
    static constexpr unsigned mayCollectBit = 33;
    static constexpr unsigned matchesAnyOverloadBit = 34;
    static constexpr unsigned mayEnterCompiledCodeBit = 35;
    static constexpr unsigned trustsNativeCodeBit = 36;
    static constexpr unsigned namesMoreBuiltinsBit = 37;
    static constexpr unsigned maxNamedBuiltins = 4;
    static constexpr unsigned overloadShift = 40;
    static_assert(numberOfEnumerators<AbstractHeap> <= mayThrowBit);

    static constexpr uint64_t overwrittenSite = std::numeric_limits<uint64_t>::max();

    // What the caller left for this callee. The words are taken, so no later call finds them.
    JS_EXPORT_PRIVATE static NativeCallExpectation take(JSGlobalObject* realm, JSObject* callee);

    constexpr uint64_t encodedEffects() const
    {
        uint64_t overloadNumber = overload ? overload.index() + 1 : 0;
        return writes.toRaw() | static_cast<uint64_t>(mayThrow) << mayThrowBit | static_cast<uint64_t>(mayCollect) << mayCollectBit | static_cast<uint64_t>(matchesAnyOverload) << matchesAnyOverloadBit | static_cast<uint64_t>(mayEnterCompiledCode) << mayEnterCompiledCodeBit
            | static_cast<uint64_t>(trustsNativeCode) << trustsNativeCodeBit | static_cast<uint64_t>(namesMoreBuiltins) << namesMoreBuiltinsBit | overloadNumber << overloadShift;
    }

    constexpr uint64_t encodedSite() const { return static_cast<uint64_t>(function.index()) << 32 | value.index(); }

    constexpr uint64_t encodedBuiltins() const
    {
        uint64_t word = 0;
        for (unsigned index = 0; index < maxNamedBuiltins; ++index)
            word |= static_cast<uint64_t>(static_cast<uint16_t>(namedBuiltins[index].index())) << 16 * index;
        return word;
    }

    void name(BuiltinID builtin)
    {
        for (BuiltinID& named : namedBuiltins) {
            if (named == builtin)
                return;
            if (!named) {
                named = builtin;
                return;
            }
        }
        namesMoreBuiltins = true;
    }

    bool names(BuiltinID builtin) const { return namesMoreBuiltins || std::ranges::find(namedBuiltins, builtin) != namedBuiltins.end(); }

    static NativeCallExpectation decode(const ExpectedCallWords& words)
    {
        NativeCallExpectation result;
        result.isKnown = true;
        result.writes = AbstractHeaps::fromRaw(words.effects & ((1ull << mayThrowBit) - 1));
        result.mayThrow = words.effects >> mayThrowBit & 1;
        result.mayCollect = words.effects >> mayCollectBit & 1;
        result.matchesAnyOverload = words.effects >> matchesAnyOverloadBit & 1;
        result.mayEnterCompiledCode = words.effects >> mayEnterCompiledCodeBit & 1;
        result.trustsNativeCode = words.effects >> trustsNativeCodeBit & 1;
        result.namesMoreBuiltins = words.effects >> namesMoreBuiltinsBit & 1;
        for (unsigned index = 0; index < maxNamedBuiltins; ++index)
            result.namedBuiltins[index] = BuiltinID(static_cast<uint16_t>(words.builtins >> 16 * index));
        if (uint64_t overloadNumber = words.effects >> overloadShift)
            result.overload = BuiltinOverloadID(static_cast<uint32_t>(overloadNumber - 1));
        result.function = FunctionID(static_cast<uint32_t>(words.site >> 32));
        result.value = Typed::ValueID(static_cast<uint32_t>(words.site));
        return result;
    }

    bool isKnown { false };
    bool mayThrow { true };
    bool mayCollect { true };
    bool mayEnterCompiledCode { true };
    // The program runs no code that the compiler has not seen, so proofs rest on what native code does not do to an object it is handed.
    bool trustsNativeCode { false };
    // Without an overload: the built-ins that the analysis took for possible callees here, and the ones that native code may call with any operand.
    bool namesMoreBuiltins { false };
    std::array<BuiltinID, maxNamedBuiltins> namedBuiltins { };
    // The call is one of the built-in of the overload, and the operands decide which of its overloads it is.
    bool matchesAnyOverload { false };
    AbstractHeaps writes;
    BuiltinOverloadID overload;
    FunctionID function;
    Typed::ValueID value;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
