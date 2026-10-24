/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTIRFunction.h"
#include "AOTLayout.h"
#include "AOTSharedMemo.h"
#include "AOTThreshold.h"
#include "AOTType.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class Program;

// Proven: what every pass may rely on. Expected: the best that is known, which a test has to confirm before anything relies on it.
enum class TypeLevel : uint8_t { Proven, Expected };

enum class FieldTypeRule : uint8_t { NamedRead, SlotRead, DeferringSlotRead, SlotStore };

// What the fields under a name give for receivers with many layouts. That depends on the tables of the program only, and costs a lookup and a union per layout.
class FieldReadTypes {
public:
    AOT_CLASS_THRESHOLD(size_t, fewestReceivers, 8);

    struct Entry {
        LayoutSetID receivers;
        PropertyKeyID key;
        TypeLevel level { TypeLevel::Proven };
        FieldTypeRule rule { FieldTypeRule::NamedRead };
        TypeID type;
    };

    const Entry* find(LayoutSetID receivers, PropertyKeyID, TypeLevel, FieldTypeRule) const LIFETIME_BOUND;
    void add(const Entry&);

private:
    Vector<Entry> m_entries;
    HashedIndex m_index;
};

class PendingTyping;

struct TypingMemo {
    // Where whoever types values of the function says so. Null where nobody keeps track.
    PendingTyping* pendingTyping { nullptr };
    FieldReadTypes fieldReads;
    TypeMemo results;
    TypeMemo idsInArena;
    TypeMemo joins;
    TypeMemo narrowerTypes;
};

// The same answers for every thread that types functions while the layouts stay as they are.
class SharedFieldTypes {
    WTF_MAKE_NONCOPYABLE(SharedFieldTypes);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SharedFieldTypes);
public:
    using ListID = SharedLayoutLists::ListID;

    struct Entry {
        ListID receivers { 0 };
        PropertyKeyID key;
        TypeLevel level { TypeLevel::Proven };
        FieldTypeRule rule { FieldTypeRule::NamedRead };
        std::optional<Type> type;
    };

    SharedFieldTypes() = default;

    SharedLayoutLists& lists() { return m_lists; }
    const Entry* find(ListID receivers, PropertyKeyID, TypeLevel, FieldTypeRule) const LIFETIME_BOUND;
    const Entry& add(Entry&&) LIFETIME_BOUND;

private:
    SharedLayoutLists m_lists;
    SharedMemo<Entry> m_entries;
};

// What somebody who types a copy of a function has proven about the positions of the program, beyond what their types say.
class KnownPositionTypes {
public:
    virtual ~KnownPositionTypes() = default;
    // None where the type of the position says all there is.
    virtual std::optional<Type> find(const DeclaredType&) const = 0;
};

struct TypingContext {
    const Program& program;
    const Function& function;
    TypeArena& types;
    TypeLevel level { TypeLevel::Proven };
    TypingMemo* memo { nullptr };
    const KnownPositionTypes* knownPositions { nullptr };
    // Of the propagation of types alone: see Value::isExpectedNever().
    bool seesValuesExpectedNever { false };

    TypeID typeIDOf(const Value& value) const
    {
        if (level == TypeLevel::Proven)
            return value.type;
        return seesValuesExpectedNever ? value.expectedOrTypeForTyping() : value.expectedOrType();
    }
    TypeID typeIDOf(ValueID id) const { return typeIDOf(function.at(id)); }
    const Type& typeOf(ValueID id) const { return types.at(typeIDOf(id)); }
    // What a read of the position gives.
    TypeID typeIDOf(const DeclaredType&) const;
    const Type& typeOf(const DeclaredType& declared) const { return types.at(typeIDOf(declared)); }
    TypingContext atLevel(TypeLevel newLevel) const { return { program, function, types, newLevel, memo, knownPositions, seesValuesExpectedNever }; }
    TypingContext expecting() const { return atLevel(TypeLevel::Expected); }
};

Type computeResultType(const TypingContext&, const Value&);
TypeID computeResultTypeID(const TypingContext&, const Value&);
Type computeResultType(const TypingContext&, const Value& producer, uint32_t resultIndex);
Type computeOperatorResultType(const TypingContext&, Opcode, std::span<const Type> operandTypes);
// What an operation with a natural result answers whatever its operands are. None for an operation with another rule.
std::optional<Type> tryWidestNaturalTypeOf(const Value&);

// The operations whose rule reads the type of a variable or the record of a function. Those get narrower between two schedules, and nobody types what reads them again.
const OpcodeSet& opcodesTypedFromNarrowedRecords();

TypeID resultTypeOf(const Program&, const Function&, const Value&, TypeArena&, TypingMemo* = nullptr);
// Null where nothing narrower than the type is expected.
TypeID expectedTypeOf(const Program&, const Function&, const Value&, TypeArena&, TypingMemo* = nullptr);

inline constexpr OpcodeSet opcodesThatMayHaveSeveralResults = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        ResultRule rule = resultRuleOf(static_cast<Opcode>(index));
        if (rule == ResultRule::Results || rule == ResultRule::Declared)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

std::optional<uint32_t> numberOfResultsOf(const Program&, const Function&, const Value&);

// The one function of the program that the callee of a call by value is, by its type.
FunctionID onlyCalleeOf(const Program&, const Type& callee);

// What a Convert becomes. Nothing: the value is inside the type, or the position is only expected to hold its type and nothing is tested.
struct ConvertResolution {
    CastResolution cast { CastResolution::Nothing };
    TypeID castType;
};

ConvertResolution resolveConvert(const TypingContext&, const Type& proven, const DeclaredType&);
// What the converted value has at the level of the context, where it had `source` at that level.
Type typeOfConvert(const TypingContext&, const Type& source, const Type& proven, const DeclaredType&);

Type typeOfConstant(const TypingContext&, const Value&);
Type typeOfData(const TypingContext&, const DataNode&);
Type typeOfAllocatedLayout(const TypingContext&, LayoutID);
// A constructor allocates what new.target asks for: the prototype of another class selects an instance of that class, or an ordinary object where that class has no shape.
bool mayAllocateOtherLayout(const Program&, const Value& allocation);
Type typeOfDeclaredLayout(const TypingContext&, LayoutID);
// What the read gives where its object has one of these layouts.
Type typeOfSlotRead(const TypingContext&, const Value& read, std::span<const LayoutID> receivers);
Type typeOfContentsRead(const TypingContext&, const Type& container, ContentsSlot);
Type typeOfElementRead(const TypingContext&, const Type& container, const Type& key);
Type typeOfAwaited(const TypingContext&, const Type& awaited);
Type elementTypeOf(TypedArrayType);

enum class FieldTypeState : uint8_t { Unknown, Proven };

// Whether a read by name finds a value of the field's type. The type bounds what the slot holds, and a property that has left its slot holds anything.
FieldTypeState fieldTypeStateOf(const Program&, FieldReference);

// No operand of another operation has a required type.
const OpcodeSet& opcodesWithRequiredOperandTypes();
TypeID requiredTypeOfOperand(const TypingContext&, const Value& user, uint32_t operandIndex);
TypeID requiredTypeOfOperand(const TypingContext&, const Terminator& user, uint32_t operandIndex);

// What stands behind the required type of an operand. A slot is written without a test, so what is stored there has to be proven. An operation that is not known to do otherwise stores into a slot.
enum class RequiredTypeHolder : uint8_t { StoredSlot, InitializedSlot, PositionThatFlowFollows };
RequiredTypeHolder holderOfRequiredTypes(const Program&, const Value& user);
TypeID declaredEntryTypeOf(const TypingContext&, const Function& callee, unsigned entryParameterIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
