/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDeclaredType.h"
#include "AOTLayout.h"
#include "AOTOpcode.h"
#include "AOTSchemaResultRules.h"
#include "AOTType.h"
#include "AOTTypeArena.h"
#include "TypedArrayType.h"
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class Program;

enum class TypeLevel : uint8_t { Proven, Expected };

class PositionTypes {
public:
    virtual ~PositionTypes() = default;

    virtual TypeID variable(TypeLevel, VariableID) const = 0;
    virtual TypeID environmentField(TypeLevel, LayoutID, uint32_t field) const = 0;
    virtual bool environmentLiesAmongVariables(LayoutID) const { return false; }
    virtual TypeID slot(TypeLevel, LayoutID, uint32_t field) const = 0;
    virtual TypeID readOfSlot(TypeLevel, LayoutID, uint32_t) const { return { }; }
    virtual TypeID halfOfAccessorOfField(TypeLevel, LayoutID, uint32_t, bool) const { return { }; }
    virtual std::optional<uint32_t> appendedField(LayoutID, PropertyKeyID) const { return std::nullopt; }
    virtual TypeID foundAboveObjectsOf(TypeLevel, LayoutID, PropertyKeyID) const { return { }; }
    virtual TypeID readByNameOffObjectsOf(TypeLevel, LayoutID, PropertyKeyID) const { return { }; }
    virtual LayoutID classAbove(LayoutID) const { return { }; }
    virtual TypeID property(TypeLevel, ObjectID, PropertyKeyID) const = 0;
    virtual TypeID result(TypeLevel, FunctionID, uint32_t resultIndex) const = 0;
    virtual TypeID resultOfBodyThatNeverSuspends(TypeLevel, FunctionID) const { return { }; }
    virtual TypeID entryParameter(TypeLevel, FunctionID, uint32_t entryParameterIndex) const = 0;
    virtual bool entryParameterIsLeftOutOfCalls(FunctionID, uint32_t) const { return false; }
    virtual TypeID capture(TypeLevel, FunctionID, uint32_t) const { return { }; }
    virtual TypeID captureOfObject(TypeLevel, ObjectID, uint32_t) const { return { }; }
    virtual TypeID resultOfCallBeingTyped(TypeLevel, uint32_t resultIndex) const = 0;
    virtual TypeID resultOfReadBeingTyped(TypeLevel) const { return { }; }
    virtual TypeID contentsBeingTyped(TypeLevel) const { return { }; }
    virtual TypeID resultOfSelector(TypeLevel, uint32_t) const { return { }; }
    virtual TypeID resultOfTableRow(TypeLevel, uint32_t, LayoutID, bool&) const { return { }; }

    enum class QuestionOfEachObject : uint8_t { Property, Contents };
    virtual TypeID askedOfEachObject(QuestionOfEachObject, TypeLevel, ObjectSetID, uint32_t, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const { return askEachObject(); }
    enum class QuestionOfEachLayout : uint8_t { ContentsOfSlot, FieldReadByName, FoundAboveObjects, ReadByNameOffObjects, ResultOfTableRow };
    virtual TypeID askedOfEachLayout(QuestionOfEachLayout, TypeLevel, LayoutSetID, PropertyKeyID, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const { return askEachLayout(); }
    virtual bool collectQueriedLayouts(PropertyKeyID, LayoutSetID, bool includesInheritedWrites, Vector<LayoutID, 16>&) const { return false; }

    static const PositionTypes& knowingNothing();
};

struct TypingContext {
    const Program& program;
    TypeArena& types;
    const PositionTypes& positions;
    TypeLevel level { TypeLevel::Proven };
    bool believesWhatFrontEndHasEstablished { true };

    ALWAYS_INLINE Type at(TypeID id) const { return types.at(id); }
    ALWAYS_INLINE TypeID typeIDOf(const DeclaredType& declared) const { return level == TypeLevel::Expected ? declared.expected().predicted() : typeIDOfSlowCase(declared); }
    TypeID typeIDOfSlowCase(const DeclaredType&) const;
    TypingContext atLevel(TypeLevel newLevel) const { return { program, types, positions, newLevel, believesWhatFrontEndHasEstablished }; }
    TypingContext expecting() const { return atLevel(TypeLevel::Expected); }
};

struct OperandTypes {
    std::span<const TypeID> types;

    uint32_t size() const { return static_cast<uint32_t>(types.size()); }
    TypeID operator[](uint32_t operandIndex) const { return operandIndex < types.size() ? types[operandIndex] : TypeID(); }
};

constexpr std::optional<Opcode> genericOperationOf(Opcode access)
{
    switch (access) {
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlot:
    case Opcode::GetDeferredClosureSlotOrUndefined:
        return Opcode::PropGet;
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return Opcode::PropSet;
    case Opcode::GetArrayElement:
    case Opcode::GetTypedArrayElement:
        return Opcode::ElemGet;
    case Opcode::PutArrayElement:
    case Opcode::PutTypedArrayElement:
        return Opcode::ElemSet;
    case Opcode::HasArrayElement:
        return Opcode::ElemHas;
    default:
        return std::nullopt;
    }
}

template<HoldsOpcode Operation>
bool testsItsOwnGuess(const Operation& operation)
{
    return genericOperationOf(opcodeOf(operation)) && operation.level() == KnowledgeLevel::Expected;
}

template<typename Operation> TypeID resultTypeOf(const TypingContext&, const Operation&, OperandTypes, ObjectID created = { }, uint32_t resultIndex = 0);
template<typename Terminator> TypeID edgeResultTypeOf(const TypingContext&, const Terminator&, OperandTypes, uint32_t targetIndex, uint32_t resultIndex);
template<typename Operation> std::optional<uint32_t> numberOfResultsOf(const Program&, const Operation&);
template<typename Operation> std::optional<uint32_t> numberOfCountedResultsOf(const Program&, const Operation&);

inline constexpr OpcodeSet opcodesThatMayHaveSeveralResults = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        ResultRule rule = resultRuleOf(static_cast<Opcode>(index));
        if (rule == ResultRule::Results || rule == ResultRule::Declared)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

inline constexpr OpcodeSet opcodesWithCountedResults = []() consteval {
    OpcodeSet opcodes { Opcode::CallDirect, Opcode::CallTable };
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        if (resultRuleOf(static_cast<Opcode>(index)) == ResultRule::Results)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();


template<typename Operation>
ALWAYS_INLINE std::optional<uint32_t> numberOfResultsOf(const Program& program, const Operation& operation)
{
    if (!opcodesWithCountedResults.contains(opcodeOf(operation)))
        return std::nullopt;
    return numberOfCountedResultsOf(program, operation);
}

TypeID naturalTypeFrom(TypeArena&, const NaturalType&);
TypeID widestPrimitiveResultOf(Opcode);
TypeID widestPrimitiveResultOf(TypeArena&, Opcode producer, uint32_t resultIndex);
inline constexpr OpcodeSet recordsTypedByWhatIsProvenOfContents { Opcode::ResultAt, Opcode::CallInline, Opcode::CallBuiltin, Opcode::GetValueAtPlaceInMap };
inline constexpr OpcodeSet readsTypedByWhatIsProvenOfInput { Opcode::ElemGet, Opcode::GetArrayElement, Opcode::GetContiguousIndex, Opcode::GetOwnContiguousIndex };

constexpr UnknownSeed seedOfUnknownResultByRowOf(Opcode opcode)
{
    switch (raisesOf(opcode)) {
    case ProgramEvent::EvaluatesText:
    case ProgramEvent::LoadsModule:
        return UnknownSeed::UnseenCode;
    case ProgramEvent::None:
        break;
    }

    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
        return UnknownSeed::ResultOfUnknownCallee;
    case EffectRule::Access:
        return UnknownSeed::ContentsOfEscapedObject;
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return UnknownSeed::UndescribedNativeResult;
    case EffectRule::Declared:
    case EffectRule::Operator:
        break;
    }

    switch (accessOf(opcode)) {
    case Access::Read:
        return UnknownSeed::ContentsOfEscapedObject;
    case Access::None:
    case Access::Store:
    case Access::Define:
    case Access::Has:
        break;
    }

    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return UnknownSeed::ResultOfUnknownCallee;
    case AccessClass::Slot:
    case AccessClass::ByName:
        return UnknownSeed::ContentsOfEscapedObject;
    case AccessClass::None:
        break;
    }

    OperationFamily family = familyOf(opcode);
    if (family == OperationFamily::Exception)
        return UnknownSeed::CaughtException;
    if (family == OperationFamily::Arguments)
        return UnknownSeed::ParameterOfEscapedFunction;
    if (family == OperationFamily::Binding || family == OperationFamily::Access)
        return UnknownSeed::ContentsOfEscapedObject;

    OperandUses uses = usesOfNamedOperandsOf(opcode);
    if (uses.contains(OperandUse::Calls))
        return UnknownSeed::ResultOfUnknownCallee;
    if (uses.contains(OperandUse::ReadsContents) || interpretedOf(opcode) == Interpretation::Read)
        return UnknownSeed::ContentsOfEscapedObject;
    return UnknownSeed::UndescribedNativeResult;
}
ALWAYS_INLINE UnknownSeed seedOfUnknownResultOf(Opcode opcode) { return ColumnTable<Opcode, UnknownSeed, seedOfUnknownResultByRowOf>::at(opcode); }
TypeID resultTypeOfOperator(const TypingContext&, Opcode, std::span<const TypeID> operandTypes);
TypeID typeAfterStepOfOne(TypeArena&, Opcode, std::span<const TypeID> operandTypes);
Range quotientsRoundedEitherWay(Range dividends, Range divisors);
template<typename Operation> TypeID typeOfConstant(const TypingContext&, const Operation&, ObjectID created = { });
TypeID typeOfKeyConstant(const TypingContext&, PropertyKeyID);
TypeID typeOfBuiltinObject(const TypingContext&, BuiltinID);
BuiltinID findNativeFunction(const BuiltinTable&, BuiltinID binding);
TypeID typeOfData(const TypingContext&, const DataNode&, ObjectID created = { });
TypeID typeOfAllocatedLayout(const TypingContext&, LayoutID, ObjectID created = { });
TypeID typeOfDeclaredLayout(const TypingContext&, LayoutID);
TypeID typeOfContentsRead(const TypingContext&, TypeID container, ContentsSlot);
TypeID typeOfElementRead(const TypingContext&, TypeID container, TypeID key);
TypeID typeOfAwaited(const TypingContext&, TypeID awaited);
TypeID elementTypeOf(TypedArrayType);
LayoutSetID layoutsSelectedByNewTarget(const TypingContext&, LayoutID written, TypeID newTarget);

FunctionID onlyCalleeOf(const TypingContext&, TypeID callee);

TypeID targetOfConvert(const TypingContext&, const DeclaredType&);
TypeID tryTypeWrittenAbove(const TypingContext&, LayoutID, PropertyKeyID);
bool changesCodeWhereProven(const TypeArena&, TypeID tested);
bool describesBagOfOptions(const TypeArena&, TypeID written);

const OpcodeSet& opcodesWithRequiredOperandTypes();
template<typename Operation> TypeID requiredTypeOfOperand(const TypingContext&, const Operation& user, uint32_t operandIndex);
template<typename Operation> TypeID requiredTypeOfDeclaredOperand(const TypingContext&, const Operation& user, uint32_t operandIndex);
TypeID declaredEntryTypeOf(const TypingContext&, FunctionID callee, unsigned entryParameterIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
