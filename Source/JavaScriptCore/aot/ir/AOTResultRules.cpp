/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRules.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTResultRulesInlines.h"
#include "AOTResultRulesInternal.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

const PositionTypes& PositionTypes::knowingNothing()
{
    class KnowingNothing final : public PositionTypes {
    public:
        constexpr KnowingNothing() = default;

        TypeID variable(TypeLevel, VariableID) const final { return { }; }
        TypeID environmentField(TypeLevel, LayoutID, uint32_t) const final { return { }; }
        TypeID slot(TypeLevel, LayoutID, uint32_t) const final { return { }; }
        TypeID property(TypeLevel, ObjectID, PropertyKeyID) const final { return { }; }
        TypeID result(TypeLevel, FunctionID, uint32_t) const final { return { }; }
        TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
        TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }
    };

    static constinit const KnowingNothing positions { };
    return positions;
}

TypeID TypingContext::typeIDOfSlowCase(const DeclaredType& declared) const
{
    switch (level) {
    case TypeLevel::Proven: {
        TypeID proven = program.provenTypeOf(declared);
        if (believesWhatFrontEndHasEstablished || !declared.isEstablished() || !at(proven).isJavaScriptValue())
            return proven;
        return at(proven).primitives & Primitive::Absent ? TypeTable::anythingOrAbsent : TypeTable::anything;
    }
    case TypeLevel::Expected:
        return declared.expected().predicted();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static_assert([] {
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (!static_cast<uint16_t>(seedOfUnknownResultByRowOf(opcode)))
            return false;
    }
    return true;
}());

TypeID ResultRules::provenOrUnknown(TypeID type, UnknownSeed seed)
{
    if (type)
        return type;
    if (context.level == TypeLevel::Proven)
        wasUnproven = true;
    return context.types.anything(seed);
}

static constexpr OpcodeSet callsOfNamedFunctionInResultRules { Opcode::CallDirect, Opcode::CallTable };
static constexpr OpcodeSet callsThatGiveBoundsOfPartsInResultRules { Opcode::BoundsOfRegExpMatch };
static_assert((opcodesWithCountedResults | callsOfNamedFunctionInResultRules) == opcodesWithCountedResults);
static_assert(opcodesWithCountedResults.contains(Opcode::Results) && opcodesWithCountedResults.contains(Opcode::DestructureIterable));

template<typename Operation>
std::optional<uint32_t> numberOfCountedResultsOf(const Program& program, const Operation& operation)
{
    Opcode opcode = opcodeOf(operation);
    if (!opcodesThatMayHaveSeveralResults.contains(opcode))
        return std::nullopt;
    if (opcode == Opcode::Results)
        return operation.numberOfVariadicOperands();
    if (opcode == Opcode::DestructureIterable)
        return operation.count();
    if (callsThatGiveBoundsOfPartsInResultRules.contains(opcode))
        return 2 * operation.count();
    if (opcode == Opcode::BoundsOfStringSplit)
        return 2 * operation.count() + 1;
    if (callsOfNamedFunctionInResultRules.contains(opcode)) {
        size_t numberOfResults = program.at(operation.function()).results.size();
        if (numberOfResults == 1)
            return std::nullopt;
        return static_cast<uint32_t>(numberOfResults);
    }
    return numberOfDeclaredResultsOf(opcode);
}

template<typename Operation>
static bool hasRuleForResultAt(const Program& program, const Operation& operation, uint32_t resultIndex)
{
    if (callsOfNamedFunctionInResultRules.contains(opcodeOf(operation)))
        return true;
    return resultIndex < numberOfResultsOf(program, operation).value_or(1);
}

static ALWAYS_INLINE bool holdsNeverAmongOperandsOfRule(OperandTypes operands)
{
    for (TypeID type : operands.types) {
        if (isNeverType(type))
            return true;
    }
    return false;
}

static_assert(OperandIndex::CheckInitialized::value == OperandIndex::RequireObjectCoercible::value && OperandIndex::CheckInitialized::value == OperandIndex::RequireObject::value);

static ALWAYS_INLINE TypeID resultTypeOfCheckThatThrows(TypeArena& types, Opcode check, TypeID checked)
{
    if (!checked)
        return { };
    if (check == Opcode::CheckInitialized)
        return types.withoutPrimitives(checked, Primitive::Absent);
    if (check == Opcode::RequireObjectCoercible)
        return types.withoutNullish(checked);
    RELEASE_ASSERT(check == Opcode::RequireObject);
    return types.withOnlyObjects(checked);
}

static_assert([] {
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode access = static_cast<Opcode>(index);
        std::optional<Opcode> generic = genericOperationOf(access);
        if (generic && resultRuleOf(*generic) != ResultRule::Unknown && resultRuleOf(*generic) != resultRuleOf(access))
            return false;
    }
    return true;
}());

static constexpr OpcodeSet opcodesWithOtherResultRuleWhereUnproven = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        bool listsRuleForEachResult = resultRuleOf(opcode) == ResultRule::Results && opcode != Opcode::Results;
        if (provenResultRuleOf(opcode) || listsRuleForEachResult)
            opcodes.add(opcode);
    }
    return opcodes;
}();
static_assert(opcodesWithOtherResultRuleWhereUnproven.contains(Opcode::PropGet) && opcodesWithOtherResultRuleWhereUnproven.contains(Opcode::MapNext));
static_assert(!opcodesWithOtherResultRuleWhereUnproven.contains(Opcode::GetModuleVariable) && !opcodesWithOtherResultRuleWhereUnproven.contains(Opcode::CallDirect));

static_assert(OperandIndex::GetArrayElement::array == OperandIndex::ElemGet::object && OperandIndex::GetArrayElement::position == OperandIndex::ElemGet::property);
static_assert(OperandIndex::GetTypedArrayElement::view == OperandIndex::ElemGet::object && OperandIndex::GetTypedArrayElement::position == OperandIndex::ElemGet::property);

template<typename Operation>
static TypeID tryProvenTypeOfReadThatTestsItsOwnGuess(const TypingContext& context, const Operation& read, Opcode generic, OperandTypes operands)
{
    if (generic == Opcode::PropGet)
        return tryTypeOfFieldRead(context, operands[*receiverOf(opcodeOf(read))], read.key());
    RELEASE_ASSERT(generic == Opcode::ElemGet);
    return tryTypeOfElementRead(context, operands[OperandIndex::ElemGet::object], operands[OperandIndex::ElemGet::property]);
}

static constexpr OpcodeSet opcodesLookedAtInFrontOfResultRules = []() consteval {
    OpcodeSet opcodes = passThroughChecks | callsOfNamedFunctionInResultRules | opcodesWithCountedResults;
    opcodes.add(Opcode::PropGet);
    opcodes.add(Opcode::Call);
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        if (genericOperationOf(static_cast<Opcode>(index)))
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

template<typename Operation>
TypeID resultTypeOf(const TypingContext& context, const Operation& operation, OperandTypes operands, ObjectID created, uint32_t resultIndex)
{
    if (holdsNeverAmongOperandsOfRule(operands))
        return TypeTable::never;

    Opcode opcode = opcodeOf(operation);
    if (context.program.opcodeInjectedAsOtherKind == static_cast<unsigned>(opcode) + 1) [[unlikely]]
        return opcode == Opcode::TypeOf || opcode == Opcode::ToString || opcode == Opcode::Strcat ? TypeTable::symbol : TypeTable::nullish;
    if (opcodesLookedAtInFrontOfResultRules.contains(opcode)) {
        if (context.level == TypeLevel::Proven && (opcode == Opcode::PropGet || opcode == Opcode::Call) && !resultIndex) {
            static_assert(!OperandIndex::PropGet::object && !OperandIndex::Call::callee);
            TypeID subject = operands[0];
            bool looksAtSubject = opcode == Opcode::Call || !operation.isFreeName();
            if (looksAtSubject && subject && !holdsOnlyPrimitives(subject)) {
                const Type& type = context.types.recordOf(subject);
                if (type.hasUnboundedUnknown() && type.isJavaScriptValue()) {
                    TypeID applied = opcode == Opcode::Call ? context.positions.resultOfCallBeingTyped(context.level, 0) : TypeID();
                    return applied ? applied : context.types.anything(seedOfUnknownResultOf(opcode));
                }
            }
        }
        if (context.level == TypeLevel::Proven && testsItsOwnGuess(operation)) {
            Opcode generic = *genericOperationOf(opcode);
            if (resultRuleOf(generic) == ResultRule::Unknown) {
                TypeID proven = tryProvenTypeOfReadThatTestsItsOwnGuess(context, operation, generic, operands);
                return proven ? proven : context.types.anything(seedOfUnknownResultOf(generic));
            }
        }
        if (passThroughChecks.contains(opcode))
            return resultTypeOfCheckThatThrows(context.types, opcode, operands[OperandIndex::CheckInitialized::value]);
        if (!hasRuleForResultAt(context.program, operation, resultIndex))
            return context.types.anything(seedOfUnknownResultOf(opcode));
    } else if (resultIndex)
        return context.types.anything(seedOfUnknownResultOf(opcode));

    ResultRules rules { context, operands, created };
    TypeID result = applyResultRule(rules, operation, resultIndex, ResultSide::Proven);
    if (rules.wasUnproven && opcodesWithOtherResultRuleWhereUnproven.contains(opcode))
        result = applyResultRule(rules, operation, resultIndex, ResultSide::Unproven);
    return result;
}

template<typename Terminator>
TypeID edgeResultTypeOf(const TypingContext& context, const Terminator& terminator, OperandTypes operands, uint32_t targetIndex, uint32_t resultIndex)
{
    std::span<const TargetInfo> targets = targetInfosOf(terminatorKindOf(terminator));
    if (targetIndex >= targets.size() || resultIndex >= targets[targetIndex].numberOfResults)
        return { };
    if (holdsNeverAmongOperandsOfRule(operands))
        return TypeTable::never;

    ResultRules rules { context, operands, { } };
    TypeID result = applyEdgeResultRule(rules, terminator, targetIndex, resultIndex, ResultSide::Proven);
    if (!rules.wasUnproven)
        return result;
    return applyEdgeResultRule(rules, terminator, targetIndex, resultIndex, ResultSide::Unproven);
}

static bool calleeHandsBackItsDeclaredResult(const Function& function)
{
    if (!canBeCalledByValueOf(function.role) || function.isCoroutine() || function.results.size() != 1)
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FunctionID onlyCalleeOf(const TypingContext& context, TypeID callee)
{
    if (!callee || holdsOnlyPrimitives(callee))
        return { };
    Type type = context.at(callee);
    if (!type.isJavaScriptValue() || type.hasUnknown())
        return { };
    FunctionID only = singleFunctionOf(context.types, type.isObject() ? type : context.at(context.types.withOnlyObjects(callee)));
    if (!only || !calleeHandsBackItsDeclaredResult(context.program.at(only)))
        return { };
    return only;
}

static bool mayLieAtOrBelowClassOfAllocation(const Program& program, LayoutID layout, LayoutID written)
{
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        if (layout == written)
            return true;
        const Layout& record = program.at(layout);
        if (!record.base)
            return record.has(LayoutFlags::HasHeritage) && !record.writtenBuiltinBase;
        layout = record.base;
    }
    return true;
}

LayoutSetID layoutsSelectedByNewTarget(const TypingContext& context, LayoutID written, TypeID newTarget)
{
    if (context.level == TypeLevel::Proven)
        return { };
    if (!written || !newTarget || holdsOnlyPrimitives(newTarget))
        return { };
    Type type = context.at(newTarget);
    if (!type.isObject() || !type.hasOnlyKnownCallees() || !type.hasOnlyClosures())
        return { };

    Vector<LayoutID, 8> selected;
    bool everyClassMayLieBelow = true;
    forEachFunctionOf(context.types, type, [&](FunctionID constructor) {
        LayoutID constructed = context.program.at(constructor).constructedClass;
        if (!constructed || !mayLieAtOrBelowClassOfAllocation(context.program, constructed, written)) {
            everyClassMayLieBelow = false;
            return;
        }
        selected.append(constructed);
    });
    if (!everyClassMayLieBelow || selected.isEmpty())
        return { };

    std::sort(selected.begin(), selected.end());
    selected.shrink(std::unique(selected.begin(), selected.end()) - selected.begin());
    return context.types.layoutSets().ofSorted(selected.span());
}

template TypeID resultTypeOf<Draft>(const TypingContext&, const Draft&, OperandTypes, ObjectID, uint32_t);
template TypeID resultTypeOf<Operation>(const TypingContext&, const Operation&, OperandTypes, ObjectID, uint32_t);
template TypeID resultTypeOf<BodyValue>(const TypingContext&, const BodyValue&, OperandTypes, ObjectID, uint32_t);
template TypeID edgeResultTypeOf<TerminatorDraft>(const TypingContext&, const TerminatorDraft&, OperandTypes, uint32_t, uint32_t);
template TypeID edgeResultTypeOf<Terminator>(const TypingContext&, const Terminator&, OperandTypes, uint32_t, uint32_t);
template std::optional<uint32_t> numberOfCountedResultsOf<Draft>(const Program&, const Draft&);
template std::optional<uint32_t> numberOfCountedResultsOf<Operation>(const Program&, const Operation&);
template std::optional<uint32_t> numberOfCountedResultsOf<BodyValue>(const Program&, const BodyValue&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
