/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTType.h"
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

// What is known of the functions of the language itself, where a program cannot put others in their place
// (Options::useImmutableIntrinsics()). They go by their numbers: ImmutableIntrinsics.

struct BuiltinSignature {
    enum class Condition : uint8_t {
        Always,
        IfFirstArgumentIsNoObject, // Or there is none.
        IfThisIsHolder, // It is called on the object that it was found on.
    };
    Type result; // Of a call that comes back.
    Condition condition;
};
std::optional<BuiltinSignature> signatureOfIntrinsic(unsigned number);

// Of `new` of it.
std::optional<Type> resultOfConstructingIntrinsic(unsigned number);

// What a property of that name of a value of that type is: a value that is not an object has no properties of its own to speak of, and
// nothing can be done about where it gets the rest from. Zero: not known, or not a cell.
unsigned intrinsicFoundOnPrimitive(Type receiver, const StringImpl& name);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
