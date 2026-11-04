/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRules.h"

#if ENABLE(AOT)

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
        TypeID contents(TypeLevel, ObjectID, ContentsSlot) const final { return { }; }
        TypeID result(TypeLevel, FunctionID, uint32_t) const final { return { }; }
        TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
        TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }
    };

    static constinit const KnowingNothing positions { };
    return positions;
}

TypeID TypingContext::typeIDOf(const DeclaredType& declared) const
{
    switch (level) {
    case TypeLevel::Proven:
        return program.provenTypeOf(declared);
    case TypeLevel::Expected:
        return declared.expected().predicted();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static OperandUses usesOfNamedOperandsInResultRules(Opcode opcode)
{
    OperandUses uses;
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode); ++operandIndex)
        uses.add(operandUsesOf(opcode, operandIndex));
    return uses;
}

static UnknownSeed seedOfUnknownResultByRow(Opcode opcode)
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
    if (family == OperationFamily::JSONParse)
        return UnknownSeed::DataFromOutside;
    if (family == OperationFamily::Binding || family == OperationFamily::Access)
        return UnknownSeed::ContentsOfEscapedObject;

    OperandUses uses = usesOfNamedOperandsInResultRules(opcode);
    if (uses.contains(OperandUse::Calls))
        return UnknownSeed::ResultOfUnknownCallee;
    if (uses.contains(OperandUse::ReadsContents) || interpretedOf(opcode) == Interpretation::Read)
        return UnknownSeed::ContentsOfEscapedObject;
    return UnknownSeed::UndescribedNativeResult;
}

// It follows from the row alone, and nearly every rule asks for it. Zero: nobody has asked yet. Whoever fills a place writes what everybody else would.
UnknownSeed seedOfUnknownResultOf(Opcode opcode)
{
    static_assert(sizeof(UnknownSeed) == sizeof(uint16_t));
    static std::array<std::atomic<uint16_t>, numberOfOpcodes> seedsByOpcode;
    std::atomic<uint16_t>& place = seedsByOpcode[static_cast<unsigned>(opcode)];
    if (uint16_t known = place.load(std::memory_order_relaxed))
        return static_cast<UnknownSeed>(known);
    UnknownSeed seed = seedOfUnknownResultByRow(opcode);
    RELEASE_ASSERT(static_cast<uint16_t>(seed));
    place.store(static_cast<uint16_t>(seed), std::memory_order_relaxed);
    return seed;
}

TypeID ResultRules::provenOrUnknown(TypeID type, UnknownSeed seed)
{
    if (type)
        return type;
    if (context.level == TypeLevel::Proven)
        wasUnproven = true;
    return context.types.anything(seed);
}

static constexpr OpcodeSet callsOfNamedFunctionInResultRules { Opcode::CallDirect, Opcode::CallTable };

template<typename Operation>
std::optional<uint32_t> numberOfResultsOf(const Program& program, const Operation& operation)
{
    Opcode opcode = opcodeOf(operation);
    if (!opcodesThatMayHaveSeveralResults.contains(opcode))
        return std::nullopt;
    if (opcode == Opcode::Results)
        return operation.numberOfVariadicOperands();
    if (opcode == Opcode::DestructureIterable)
        return operation.count();
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
    // How many results a direct call hands back is Plan's to say. The rule asks PositionTypes, which may know, and bounds the index itself.
    if (callsOfNamedFunctionInResultRules.contains(opcodeOf(operation)))
        return true;
    return resultIndex < numberOfResultsOf(program, operation).value_or(1);
}

static bool holdsNeverAmongOperandsOfRule(OperandTypes operands)
{
    return std::ranges::any_of(operands.types, [](TypeID type) {
        return isNeverType(type);
    });
}

// They have the schema's rule Cast and state no type: each throws for what it turns away, so what goes on is the rest of its operand.
static constexpr OpcodeSet checksThatThrowInResultRules = checksThatHandOnOrThrow;
static_assert(OperandIndex::CheckInitialized::value == OperandIndex::RequireObjectCoercible::value && OperandIndex::CheckInitialized::value == OperandIndex::RequireObject::value);

static TypeID resultTypeOfCheckThatThrows(TypeArena& types, Opcode check, TypeID checked)
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

// An access that tests its own guess is typed as its generic operation. Where that one's result is not unknown, the access has the same rule, which then answers for both sides.
static_assert([] {
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode access = static_cast<Opcode>(index);
        std::optional<Opcode> generic = genericOperationOf(access);
        if (generic && resultRuleOf(*generic) != ResultRule::Unknown && resultRuleOf(*generic) != resultRuleOf(access))
            return false;
    }
    return true;
}());

// Every other opcode has one rule for both sides, which answers the same when it is asked again.
// Of a result among several no column says whether it has two rules. Results lists none: each of its results is an operand.
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

// Both sides of a read that tests its own guess read one member of one receiver, so what the rule of the generic operation proves of these operands holds of what either side finds.
// What the read itself names is only guessed: the layouts, the kind of storage. Null: nothing is proven.
template<typename Operation>
static TypeID tryProvenTypeOfReadThatTestsItsOwnGuess(const TypingContext& context, const Operation& read, Opcode generic, OperandTypes operands)
{
    if (generic == Opcode::PropGet)
        return tryTypeOfFieldRead(context, operands[*receiverOf(opcodeOf(read))], read.key());
    RELEASE_ASSERT(generic == Opcode::ElemGet);
    return tryTypeOfElementRead(context, operands[OperandIndex::ElemGet::object], operands[OperandIndex::ElemGet::property]);
}

template<typename Operation>
TypeID resultTypeOf(const TypingContext& context, const Operation& operation, OperandTypes operands, ObjectID created, uint32_t resultIndex)
{
    if (holdsNeverAmongOperandsOfRule(operands))
        return TypeTable::never;

    Opcode opcode = opcodeOf(operation);
    // `unknown` without a bound among what is read off or called: no cell, no layout and no row proves anything, so each rule below comes to the same end by a longer way.
    if (context.level == TypeLevel::Proven && (opcode == Opcode::PropGet || opcode == Opcode::Call) && !resultIndex) {
        static_assert(!OperandIndex::PropGet::object && !OperandIndex::Call::callee);
        TypeID subject = operands[0];
        bool looksAtSubject = opcode == Opcode::Call || !operation.isFreeName();
        if (looksAtSubject && subject && !holdsOnlyPrimitives(subject)) {
            Type type = context.at(subject);
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
    if (checksThatThrowInResultRules.contains(opcode))
        return resultTypeOfCheckThatThrows(context.types, opcode, operands[OperandIndex::CheckInitialized::value]);
    if (!hasRuleForResultAt(context.program, operation, resultIndex))
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

ExpectedType expectationBeside(TypeArena& types, TypeID proven, TypeID computedAtLevelExpected)
{
    if (!proven || !computedAtLevelExpected || computedAtLevelExpected == proven)
        return { };
    TypeID met = types.intersectionOf(proven, computedAtLevelExpected);
    if (isNeverType(met) || met == proven)
        return { };
    // What an intersection keeps of `unknown` may lie outside one of its sides.
    if (!types.isSubset(met, proven))
        return { };
    // Two types that differ in their seeds alone say the same. What is written of the contents of a class says more, and isSubset() passes over it.
    if (types.isSubset(proven, met) && !writtenContentsSayMoreThan(types, proven, met))
        return { };
    return ExpectedType(met);
}

// What `return` hands over in a coroutine is not what its caller gets, and a constructor's caller gets the object where the result is none.
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
    // Beside `unknown` no function is the only one.
    if (!type.isJavaScriptValue() || type.hasUnknown())
        return { };
    // A call of what is no object throws.
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
        // A heritage that names neither a class of the program nor one of the engine may turn out to be any class.
        if (!record.base)
            return record.has(LayoutFlags::HasHeritage) && !record.writtenBuiltinBase;
        layout = record.base;
    }
    return true;
}

LayoutSetID layoutsSelectedByNewTarget(const TypingContext& context, LayoutID written, TypeID newTarget)
{
    // That a class has the base it was written with is Facts' to prove, and the `prototype` of a plain function may be stored to. So the answer is a guess.
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
template TypeID edgeResultTypeOf<TerminatorDraft>(const TypingContext&, const TerminatorDraft&, OperandTypes, uint32_t, uint32_t);
template TypeID edgeResultTypeOf<Terminator>(const TypingContext&, const Terminator&, OperandTypes, uint32_t, uint32_t);
template std::optional<uint32_t> numberOfResultsOf<Draft>(const Program&, const Draft&);
template std::optional<uint32_t> numberOfResultsOf<Operation>(const Program&, const Operation&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
