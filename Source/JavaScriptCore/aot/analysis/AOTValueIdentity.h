/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEffectsDeclared.h"
#include <cstddef>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
struct Subject;

namespace Typed {

class UseLists;

constexpr bool isCast(Opcode opcode) { return interpretedOf(opcode) == Interpretation::Cast; }
constexpr bool isCastWithoutTest(Opcode opcode) { return isCast(opcode) && !mayThrow(opcode) && opcode != Opcode::Check; }

inline constexpr OpcodeSet castsAndConversions = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (isCast(opcode) || familyOf(opcode) == OperationFamily::Representation)
            opcodes.add(opcode);
    }
    return opcodes;
}();

constexpr bool isCastOrConversion(Opcode opcode) { return castsAndConversions.contains(opcode); }

ValueID sameObjectThroughCasts(const Function&, ValueID);
ValueID definitionThroughUpcasts(const Function&, ValueID);

// The test for a dead zone, or the cast that stands in its place where a store is proven to come before the read.
inline bool leavesDeadZoneBehind(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode == Opcode::CheckInitialized)
        return true;
    return value.opcode == Opcode::AssumeType && !(program.at(value.castType()).primitives & Primitive::Absent) && program.typeOf(function, value.valueOperand()).primitives & Primitive::Absent;
}

class CastChain {
public:
    class Iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = ValueID;

        Iterator() = default;
        Iterator(const Function& function, ValueID value, ValueID last)
            : m_function(&function)
            , m_value(value)
            , m_last(last)
        {
        }

        ValueID operator*() const { return m_value; }

        Iterator& operator++()
        {
            m_value = m_value == m_last ? ValueID() : m_function->at(m_value).operand(0);
            return *this;
        }

        Iterator operator++(int)
        {
            Iterator previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) { return a.m_value == b.m_value; }

    private:
        const Function* m_function { nullptr };
        ValueID m_value;
        ValueID m_last;
    };

    CastChain(const Function& function, ValueID value)
        : m_function(function)
        , m_first(value)
        , m_last(sameObjectThroughCasts(function, value))
    {
    }

    Iterator begin() const { return Iterator(m_function, m_first, m_last); }
    Iterator end() const { return { }; }
    ValueID last() const { return m_last; }

private:
    const Function& m_function;
    ValueID m_first;
    ValueID m_last;
};

class ValueIdentities {
    WTF_MAKE_NONCOPYABLE(ValueIdentities);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueIdentities);
public:
    // StartAtEntry also takes no shortcut, for comparison.
    enum class RepeatedWalks : uint8_t { StartAtFirstChangedBlock, StartAtEntry };
    // OnDemand reads the function when it is asked, so it is only for callers that ask before they edit.
    enum class Evaluation : uint8_t { Complete, OnDemand };

    // opcodesOfValues may be a superset.
    ValueIdentities(const Function&, const CFG&, RepeatedWalks = RepeatedWalks::StartAtFirstChangedBlock, const OpcodeSet* opcodesOfValues = nullptr, Evaluation = Evaluation::Complete);

    ValueID identityOf(ValueID id) const
    {
        RELEASE_ASSERT(id.index() < m_numberOfValues);
        if (m_identities.isEmpty())
            return id;
        ValueID identity = m_identities[id.index()];
        return identity ? identity : const_cast<ValueIdentities&>(*this).resolve(id);
    }

    bool areSameValue(ValueID a, ValueID b) const { return identityOf(a) == identityOf(b); }
    unsigned numberOfValues() const { return m_numberOfValues; }

    bool isComplete() const { return m_isComplete; }
    void complete();
    // Until it is complete it needs the current graph.
    void setGraph(const CFG* cfg) { m_cfg = cfg; }
    // Null if only complete() can tell.
    ValueID tryResolve(ValueID);

    void didEraseValues(std::span<const ValueID>);
    // False if the identities have to be computed again.
    bool didRenumber(const Renumbering&);

private:
    class PassedValues;
    class MergingParameters;

    // What a walk may still find for a parameter. Each is less than the one before.
    enum class Expectation : uint8_t {
        IsPassedOneValue,
        ReceivesWhatEarlierParameterReceives,
        IsValueOfItsOwn,
    };

    void computeAll();
    void walk(const Function&, const CFG&, uint32_t firstOrderIndex);
    void forgetIdentitiesFrom(const Function&, const CFG&, uint32_t firstOrderIndex);
    PassedValues passedValuesInto(const Function&, const CFG&, uint32_t orderIndex);
    ValueID identityOfParameter(const PassedValues&, uint32_t parameterIndex, MergingParameters&);
    ValueID identityOfParameterWithSingleEdge(const PassedValues&, uint32_t parameterIndex);
    bool receiveSameValues(const PassedValues&, uint32_t earlierIndex, uint32_t parameterIndex) const;
    std::optional<uint32_t> expectLessOfParametersThatDoNotHold(const Function&, const CFG&);

    struct ListedParameter {
        uint32_t walkPosition;
        BlockID block;
        uint32_t index;
    };

    void recordWalkPositions();
    const ListedParameter& listedParameterAt(uint32_t walkPosition) const LIFETIME_BOUND;
    ValueID resolve(ValueID);
    ValueID tryResolve(ValueID, ValueID& blockingParameter);
    ValueID tryResolvePassedValue(ValueID passed, ValueID parameter);
    ValueID tryResolveMergeParameter(ValueID, const ListedParameter&);
    ValueID tryResolveParametersInOrder(const PassedValues&, std::span<const ValueID> passedValues, ValueID);
    ValueID tryResolveLoopParameter(const PassedValues&, uint32_t parameterIndex, uint32_t walkPosition);
    bool receiveDifferentValues(const PassedValues&, uint32_t earlierIndex, uint32_t parameterIndex);

    const Function& m_function;
    const CFG* m_cfg;
    // Empty if every value is its own identity. Until it is complete, null for a value that nobody has asked about.
    Vector<ValueID> m_identities;
    unsigned m_numberOfValues;
    bool m_isComplete { false };
    bool m_convergedOnFirstWalk { true };

    // The order in which a walk assigns identities, shifted left by one; the low bit marks a block parameter.
    static constexpr uint32_t notListed = UINT32_MAX;
    Vector<uint32_t> m_walkPositions;
    Vector<ListedParameter> m_listedParameters;
    Vector<BlockID, 8> m_blocksBeingResolved;
    size_t m_remainingOnDemandWork { 0 };

    Vector<Expectation> m_expectations;
    bool m_didReadUnknownIdentity { false };
    std::optional<uint32_t> m_firstBlockThatReadUnknownIdentity;

    static constexpr uint32_t notKept = UINT32_MAX;
    bool m_takesShortcuts;
    bool m_keepsWhatWalksRead { false };
    bool m_didWalk { false };
    Vector<ValueID> m_passedValues;
    Vector<uint32_t> m_firstPassedValue;
    Vector<ValueID> m_operandsOfCasts;
};

Vector<ValueID, 2> findSingleUseCastChain(const Function&, const UseLists&, ValueID user);
std::optional<ValueID> constantThroughCasts(const Program&, const Function&, ValueID);
std::optional<int64_t> integerConstantThroughCasts(const Program&, const Function&, ValueID);
bool isRematerializable(const Value&);
bool isDeferredClosure(const Function&, ValueID);
// What a read gave that takes the outer object for an inner object which is not made yet.
bool isBorrowedInnerObject(const Function&, ValueID);
// What such a value passes through and stays what it is. A test for undefined and null looks at the reference alone.
constexpr bool handsOnBorrowedInnerObject(Opcode opcode)
{
    constexpr OpcodeSet others { Opcode::RequireObjectCoercible, Opcode::Box, Opcode::Unbox, Opcode::Decode, Opcode::Compress };
    return isCastWithoutTest(opcode) || others.contains(opcode);
}
// What an operation does with such a value.
enum class UseOfBorrowedObject : uint8_t {
    // It hands on what it takes.
    HandsOn,
    // It reads or writes a slot, or asks the cell for a state that an object which was never made is in.
    Borrows,
    // It wants the object itself.
    Holds,
};
UseOfBorrowedObject useOfBorrowedInnerObject(const Program&, const Function&, const Value& user, uint32_t operandIndex, ValueID borrowed);
// The parameter that the operation stands for. The choice of representations may have put conversions between them. Null where the parameter has gone and the operation is a cast like any other.
ValueID parameterOf(const Function&, const Value& borrowedParameter);
bool hasCodeAtDefinition(const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
