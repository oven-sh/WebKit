/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include "AOTSchemaResultRules.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// Shared by the AOTResultRules*.cpp files. Nobody else includes this.

enum class FieldTypeRule : uint8_t { NamedRead, SlotRead, DeferringSlotRead, SlotStore };

// AOTResultRulesNatural.cpp
TypeID naturalTypeOfOperands(const TypingContext&, Opcode, NaturalTable, OperandTypes);
// Both answer null where the operands say no more than the schema does.
// numberResultOf() holds only where the operator gives a number whatever these operands turn out to be, as URShift and ToNumber always do. A never among the operands gives never.
TypeID numberResultOf(TypeArena&, Opcode, OperandTypes);
TypeID integerQuotientType(TypeArena&, Range dividends, Range divisors, QuotientRounding);
// Of RemainderOfIntegers, which gives 0 where `%` gives -0 or NaN.
TypeID wholeRemainderType(TypeArena&, Range dividends, Range divisors);

// AOTResultRulesRefined.cpp. The schema gives these a fixed natural type, and their own words or operands say more.
const OpcodeSet& opcodesWithNaturalTypeRefinedByOperation();
template<typename Operation> TypeID refineFixedNatural(const TypingContext&, const Operation&, OperandTypes, TypeID upperBound);

// AOTResultRulesDeclared.cpp. Null: nothing is proven, or at the level Expected nothing is written.
// Of a receiver's named objects PositionTypes::property() answers. Of its `unknown` within layouts PositionTypes::slot() cut by PositionTypes::readOfSlot() answers for a read of the slot,
// and for a read by name the fields' declared types, at the level Expected alone.
// A store is held to PositionTypes::slot() alone: tryTypeAcceptedBySlots().
TypeID tryTypeOfFieldRead(const TypingContext&, TypeID receiver, PropertyKeyID);
// What a binding of the global object finds: what a read of the name off that object finds.
// `mayBeShadowed`: a script that nobody has seen may declare the name, and what its binding holds lies in no cell. Then nothing is proven.
// `findsOwnProperty`: the operation's own words say that the object has the property, so PositionTypes::ownProperty() is asked first.
TypeID tryTypeOfGlobalBindingRead(const TypingContext&, PropertyKeyID, bool mayBeShadowed, bool findsOwnProperty);
TypeID tryTypeOfSlotContents(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
// `contents` cut by what PositionTypes::property() answers of the named objects of the receiver. `contents` itself where the receiver names anything else, and at the level Expected.
TypeID typeOfSlotContentsInReceiver(const TypingContext&, TypeID contents, TypeID receiver, PropertyKeyID);
TypeID tryTypeOfSetterInSlot(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID tryTypeOfProvenHalfInSlot(const TypingContext&, LayoutSetID receivers, PropertyKeyID, bool isSetter);
TypeID tryTypeOfDeferringSlotContents(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID tryTypeAcceptedBySlots(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID tryTypeOfContentsRead(const TypingContext&, TypeID container, ContentsSlot);
TypeID tryTypeOfElementRead(const TypingContext&, TypeID container, TypeID key);
// A read out of the storage of an array: GetArrayElement, GetContiguousIndex, GetOwnContiguousIndex.
// `storage`: the kind that the operation names, ArrayStorage::Other where it names none.
// `findsElement`: the operation's own words say that it finds an element. Undefined is added without it.
TypeID tryTypeOfArrayElementRead(const TypingContext&, TypeID array, ArrayStorage storage, bool findsElement, bool readsHoleAsNaN);
TypeID tryTypeOfAwaited(const TypingContext&, TypeID awaited);

// What the generated applyResultRule() and applyEdgeResultRule() call. One is made for one operation.
// The member templates are defined in AOTResultRulesInlines.h, which AOTResultRules.cpp alone includes: each takes what it needs out of the operation and calls a function that is no template.
struct ResultRules {
    using Result = TypeID;

    const TypingContext& context;
    OperandTypes operands;
    ObjectID created;
    bool wasUnproven { false };

    template<typename Operation> TypeID none(const Operation&) { return TypeTable::never; }
    template<typename Operation> TypeID constant(const Operation& operation) { return typeOfConstant(context, operation, created); }
    template<typename Operation> TypeID natural(const Operation&, const NaturalType&);
    template<typename Operation> TypeID natural(const Operation&, NaturalTable);
    template<typename Operation> TypeID sameAs(const Operation&, uint32_t operandIndex);
    template<typename Operation> TypeID allocation(const Operation&, LayoutID);
    template<typename Operation> TypeID allocation(const Operation&, BuiltinClassID);
    template<typename Operation> TypeID allocation(const Operation&, BuiltinClassID, TypeID contents);
    template<typename Operation> TypeID allocation(const Operation&, BuiltinLayoutID);
    template<typename Operation> TypeID declared(const Operation&, VariableID);
    template<typename Operation> TypeID declared(const Operation&, LayoutID, uint32_t field);
    template<typename Operation> TypeID declared(const Operation&, PropertyKeyID);
    template<typename Operation> TypeID declared(const Operation&, FunctionID, uint32_t resultIndex);
    template<typename Operation> TypeID declared(const Operation&, LayoutID);
    template<typename Operation> TypeID declared(const Operation&, ModuleID);
    // The word of Unspecified may be anything: findIllTypedOperand() and handsOperandOn() of AOTOperandKinds.h go by the mark.
    template<typename Operation> TypeID declared(const Operation& operation, TypeID type)
    {
        return opcodeOf(operation) == Opcode::Unspecified ? context.types.withPrimitives(type, Primitive::Unspecified) : type;
    }
    template<typename Operation> TypeID declared(const Operation&, BuiltinOverloadID, uint32_t resultIndex);
    template<typename Operation> TypeID declaredContents(const Operation&, uint32_t operandIndex, std::optional<ContentsSlot>);
    template<typename Operation> TypeID declaredResult(const Operation&, uint32_t operandIndex, uint32_t) { return operands[operandIndex]; }
    // The caller types parameters: an entry's by PositionTypes::entryParameter(), a join's by uniting what arrives.
    template<typename Operation> TypeID declaredParameter(const Operation&);
    template<typename Operation> TypeID unknown(const Operation&);
    // The schema's rule Cast belongs to five of the operations that state a type, which Walker::build() refuses (opcodesThatStateTypes in AOTOpcodeSets.h),
    // and to CheckInitialized, RequireObjectCoercible and RequireObject, which resultTypeOf() types in front of the dispatch. So each of these is RELEASE_ASSERT_NOT_REACHED().
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex, TypeID castType);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex, ExpectedType);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex, ProvenType);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex);
    template<typename Operation> TypeID resultOperand(const Operation&, uint32_t resultIndex);

    // Null becomes anything, with the seed that fits the operation, and at the level Proven notes that the rule's unproven side applies.
    TypeID provenOrUnknown(TypeID, UnknownSeed);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
