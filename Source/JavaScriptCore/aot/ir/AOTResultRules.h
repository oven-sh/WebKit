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

enum class TypeLevel : uint8_t { Proven, Expected };

enum class FieldTypeRule : uint8_t { NamedRead, SlotRead, DeferringSlotRead, SlotStore };

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
    PendingTyping* pendingTyping { nullptr };
    FieldReadTypes fieldReads;
    TypeMemo results;
    TypeMemo idsInArena;
    TypeMemo joins;
    TypeMemo narrowerTypes;
};

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

class KnownPositionTypes {
public:
    virtual ~KnownPositionTypes() = default;
    virtual std::optional<Type> find(const DeclaredType&) const = 0;
};

struct TypingContext {
    const Program& program;
    const Function& function;
    TypeArena& types;
    TypeLevel level { TypeLevel::Proven };
    TypingMemo* memo { nullptr };
    const KnownPositionTypes* knownPositions { nullptr };
    bool seesValuesExpectedNever { false };

    TypeID typeIDOf(const Value& value) const
    {
        if (level == TypeLevel::Proven)
            return value.type;
        return seesValuesExpectedNever ? value.expectedOrTypeForTyping() : value.expectedOrType();
    }
    TypeID typeIDOf(ValueID id) const { return typeIDOf(function.at(id)); }
    const Type& typeOf(ValueID id) const { return types.at(typeIDOf(id)); }
    TypeID typeIDOf(const DeclaredType&) const;
    const Type& typeOf(const DeclaredType& declared) const { return types.at(typeIDOf(declared)); }
    TypingContext atLevel(TypeLevel newLevel) const { return { program, function, types, newLevel, memo, knownPositions, seesValuesExpectedNever }; }
    TypingContext expecting() const { return atLevel(TypeLevel::Expected); }
};

Type computeResultType(const TypingContext&, const Value&);
TypeID computeResultTypeID(const TypingContext&, const Value&);
Type computeResultType(const TypingContext&, const Value& producer, uint32_t resultIndex);
Type computeOperatorResultType(const TypingContext&, Opcode, std::span<const Type> operandTypes);
std::optional<Type> tryWidestNaturalTypeOf(const Value&);

const OpcodeSet& opcodesTypedFromNarrowedRecords();

TypeID resultTypeOf(const Program&, const Function&, const Value&, TypeArena&, TypingMemo* = nullptr);
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

FunctionID onlyCalleeOf(const Program&, const Type& callee);

struct ConvertResolution {
    CastResolution cast { CastResolution::Nothing };
    TypeID castType;
};

TypeID targetOfConvert(const TypingContext&, const DeclaredType&);
ConvertResolution resolveConvert(const TypingContext&, const Type& proven, const DeclaredType&);
Type typeOfConvert(const TypingContext&, const Type& source, const Type& proven, const DeclaredType&);

Type typeOfConstant(const TypingContext&, const Value&);
Type typeOfData(const TypingContext&, const DataNode&);
Type typeOfAllocatedLayout(const TypingContext&, LayoutID);
bool mayAllocateOtherLayout(const Program&, const Value& allocation);
Type typeOfDeclaredLayout(const TypingContext&, LayoutID);
Type typeOfSlotRead(const TypingContext&, const Value& read, std::span<const LayoutID> receivers);
Type typeOfContentsRead(const TypingContext&, const Type& container, ContentsSlot);
Type typeOfElementRead(const TypingContext&, const Type& container, const Type& key);
Type typeOfAwaited(const TypingContext&, const Type& awaited);
Type elementTypeOf(TypedArrayType);

enum class FieldTypeState : uint8_t { Unknown, Proven };

FieldTypeState fieldTypeStateOf(const Program&, FieldReference);

const OpcodeSet& opcodesWithRequiredOperandTypes();
TypeID requiredTypeOfOperand(const TypingContext&, const Value& user, uint32_t operandIndex);
TypeID requiredTypeOfOperand(const TypingContext&, const Terminator& user, uint32_t operandIndex);

enum class RequiredTypeHolder : uint8_t { StoredSlot, InitializedSlot, PositionThatFlowFollows };
RequiredTypeHolder holderOfRequiredTypes(const Program&, const Value& user);
TypeID declaredEntryTypeOf(const TypingContext&, const Function& callee, unsigned entryParameterIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
