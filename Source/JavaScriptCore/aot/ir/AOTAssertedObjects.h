/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTKnownClosures.h"
#include "AOTTypeClosure.h"
#include <array>
#include <bit>
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class TypeArena;

struct PositionReference {
    enum class Kind : uint8_t { EntryParameter, Result, Variable, Field };

    static PositionReference entryParameter(FunctionID function, uint32_t index) { return { Kind::EntryParameter, function.index(), index }; }
    static PositionReference result(FunctionID function, uint32_t index) { return { Kind::Result, function.index(), index }; }
    static PositionReference variable(VariableID variable) { return { Kind::Variable, variable.index(), 0 }; }
    static PositionReference field(FieldReference field) { return { Kind::Field, field.layout.index(), field.field }; }

    FunctionID function() const { return FunctionID(owner); }
    VariableID variable() const { return VariableID(owner); }
    LayoutID layout() const { return LayoutID(owner); }

    Kind kind { Kind::Variable };
    uint32_t owner { 0 };
    uint32_t index { 0 };

    friend bool operator==(const PositionReference&, const PositionReference&) = default;
    friend auto operator<=>(const PositionReference&, const PositionReference&) = default;
};

// Null where the function, the layout or the list has no such entry: the verifier has not seen the program.
const DeclaredType* findPosition(const Program&, PositionReference);
DeclaredType* findPosition(Program&, PositionReference);

inline bool admitsObjects(const DeclaredType& declared) { return declared.flags.contains(PositionFlags::AdmitsObjects); }

// In a build that reports mismatches a position promises nothing, and what is compared with a value that enters it is its written part.
inline TypeID DeclaredType::* testedPartOf(const DeclaredType& declared, MismatchPolicy policy)
{
    return policy == MismatchPolicy::Report && declared.written ? &DeclaredType::written : &DeclaredType::type;
}

// `e as T` lets through every object that e may be by its checked type. `written` is what T itself promises, and is left out where that is all the request lets through.
bool mayYieldUnnamedObject(const Program&, const DeclaredType& request);

// What an operation names when it reads a position or a closure: the variable, the layout of an environment or of a private field, the name of a member, the function
// whose closure it creates or calls.
struct Mention {
    enum class Kind : uint8_t { Variable, Layout, Key, Function };
    static constexpr size_t numberOfKinds = 4;

    Kind kind { Kind::Variable };
    uint32_t index { 0 };

    friend bool operator==(const Mention&, const Mention&) = default;
    friend auto operator<=>(const Mention&, const Mention&) = default;
};

std::optional<Mention> mentionOf(const Value&);

class MentionSet {
public:
    void add(Mention mention) { m_bits[static_cast<size_t>(mention.kind)].set(mention.index); }
    bool contains(Mention mention) const { return m_bits[static_cast<size_t>(mention.kind)].get(mention.index); }

private:
    std::array<BitVector, Mention::numberOfKinds> m_bits;
};

// Where the asserted object that a value may be has come from, seen from the function that holds the value: from the function itself (an assertion in it, a position it
// reads), or from one of its entry parameters. A function that hands a parameter on to its result gives an asserted object to the callers that passed it one, and to no other.
class AssertedOrigins {
public:
    static constexpr AssertedOrigins function() { return AssertedOrigins(1); }
    static constexpr AssertedOrigins entryParameter(uint32_t index) { return index < numberOfDistinctParameters ? AssertedOrigins(2u << index) : function(); }

    constexpr AssertedOrigins() = default;

    constexpr bool isEmpty() const { return !m_bits; }
    constexpr bool containsFunction() const { return m_bits & 1; }
    constexpr bool containsAll(AssertedOrigins other) const { return !(other.m_bits & ~m_bits); }
    constexpr uint32_t size() const { return std::popcount(m_bits); }
    constexpr void add(AssertedOrigins other) { m_bits |= other.m_bits; }

    void forEachEntryParameter(const auto& functor) const
    {
        for (uint32_t bits = m_bits >> 1; bits; bits &= bits - 1)
            functor(static_cast<uint32_t>(std::countr_zero(bits)));
    }

    friend constexpr bool operator==(AssertedOrigins, AssertedOrigins) = default;

private:
    static constexpr uint32_t numberOfDistinctParameters = 31;

    constexpr explicit AssertedOrigins(uint32_t bits)
        : m_bits(bits)
    {
    }

    uint32_t m_bits { 0 };
};

class AdmittedKinds {
public:
    ValueKinds at(PositionReference) const;
    bool add(PositionReference, ValueKinds);

private:
    struct Entry {
        PositionReference position;
        ValueKinds kinds;
    };

    Vector<Entry> m_entriesByPosition;
};

// The value, and the operation that stores, passes or returns it.
struct AssertedObjectUse {
    ValueID value;
    Origin origin;
};

// Elements: what enters is an element of the array in the position. Only a rest parameter is reached so: its array is new with each call.
enum class PositionPart : uint8_t { Value, Elements };

struct ReachedPosition {
    PositionReference position;
    PositionPart part { PositionPart::Value };
    AssertedObjectUse use;
    AssertedOrigins origins { AssertedOrigins::function() };
    ValueKinds otherKinds;
};

// `e as T` lets every object through, whatever T names. What such an object reaches in one function.
struct AssertedObjectFindings {
    bool didTypeAllValues { false };
    bool didFollowAssertedObjects { false };
    uint32_t numberOfSources { 0 };
    uint32_t numberOfAdmittingRequests { 0 };
    uint32_t numberOfRequestsAdmittingOtherKinds { 0 };
    Vector<ReachedPosition> positions;
    Vector<AssertedObjectUse> argumentsOfUnknownCallees;
};

struct AssertedObjectInputs {
    const Program& program;
    const KnownClosures& closures;
    const BitVector& keysOfAdmittingFields;
    const MentionSet& holdersOfAdmittingResults;
    std::span<const AssertedOrigins> originsOfResults;
    const AdmittedKinds& admittedKinds;
    const FlowResults& noFlowResults;
};

// Types the values of the function unless they are typed, types again what depends on a position that admits objects, and lets asserted objects through the requests of the
// function. No block is cut behind a value of type never: a position that is widened later may give the value a type.
// A source is an assertion, a read of a position that admits objects or has taken a kind, or an origin of the result of a callee. None of them goes away, so a function with as
// many sources as the last time has the same ones, and nothing about it has changed.
// Values of other kinds (AOTTypeClosure.h) are followed the same way, from the requests that none of them passes.
AssertedObjectFindings findAssertedObjects(const AssertedObjectInputs&, FunctionID, Function&, uint32_t previousNumberOfSources, TypeArena&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
