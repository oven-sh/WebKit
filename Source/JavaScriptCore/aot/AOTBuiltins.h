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

// A method of strings that there is a way of calling with just what it takes, as it is: the operation that the other tiers have for it.
// (Options::aotCallsMethodsDirectly().)
struct DirectMethod {
    enum class Takes : uint8_t { Nothing, String, Int32, StringAndInt32, Int32AndInt32, NothingAndZero };
    enum class Returns : uint8_t { Boolean, Int32, String, WhetherIndex };
    ASCIILiteral name;
    Takes takes;
    Returns returns;
    uint16_t operation; // An Entry.
    unsigned argumentCountIncludingThis() const
    {
        switch (takes) {
        case Takes::Nothing:
        case Takes::NothingAndZero:
            return 1;
        case Takes::String:
        case Takes::Int32:
            return 2;
        case Takes::StringAndInt32:
        case Takes::Int32AndInt32:
            return 3;
        }
        return 0;
    }
};
// From one. Zero: there is none.
unsigned directMethodOfStrings(const StringImpl& name, unsigned argumentCountIncludingThis);
const DirectMethod& directMethod(unsigned number);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
