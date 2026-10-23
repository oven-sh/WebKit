/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayElementPresence.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTTypedArrayKinds.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned maxValuesFollowed = 8;

// SinkAllocations and NarrowStorage take such an array apart by its accesses, which stay as they are. The result of `x.map(f)` is an allocation too, and its reads get the test.
static constexpr OpcodeSet localArrayAllocations {
    Opcode::NewArray, Opcode::NewArrayLike, Opcode::NewArrayWithCapacity, Opcode::NewContiguousArray, Opcode::NewDoubleArray, Opcode::CreateRest,
};

static constexpr OpcodeSet equalityComparisons { Opcode::Eq, Opcode::StrictEq };

// Each applies ToNumeric to an operand that is a primitive.
static constexpr OpcodeSet operatorsOnNumericOperands {
    Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow,
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Negate, Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement, Opcode::BitNot,
    Opcode::ToNumber, Opcode::ToInt32, Opcode::ToUint32,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
};

static ValueID parameterFilledBy(const Function& function, const Use& use)
{
    return function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
}

// What passes a test of its type goes on under the narrower type, as behind a cast. What fails it has left for other code.
static void forEachValueThatPassed(const Function& function, const UseLists& useLists, ValueID test, uint32_t operandIndex, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    constexpr uint32_t takenTargetIndex = 0;
    for (const Use& use : useLists.usesOf(test)) {
        if (use.kind != Use::Kind::TerminatorOperand)
            continue;
        const Terminator& terminator = function.at(use.block).terminator;
        if (!terminator.isConditionalBranch())
            continue;

        const Target& taken = terminator.taken();
        uint32_t firstResult = firstEdgeResultParameter(function, use.block, takenTargetIndex);
        for (uint32_t resultIndex = 0; resultIndex < taken.proofs.size(); ++resultIndex) {
            if (taken.proofs[resultIndex] == operandIndex)
                functor(function.at(taken.block).parameters[firstResult + resultIndex]);
        }
    }
}

static bool asksWhetherValueIsAbsent(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::ToBoolean || user.opcode == Opcode::TypeOf)
        return true;
    // `??` and `?.` are tests for undefined and null. A test for a number is what a check of the element has become, and it folds once the read is typed.
    if (user.opcode == Opcode::TypeTest)
        return program.at(user.testedType()).isUndefinedOrNull();
    // A comparison with what is never undefined or null looks for a value.
    if (equalityComparisons.contains(user.opcode))
        return program.typeOf(function, user.operand(1 - operandIndex)).mayBeUndefinedOrNull();
    return false;
}

bool isTestedForAbsence(FunctionAnalyses& analyses, ValueID read)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();

    Vector<ValueID, maxValuesFollowed> followed { read };
    auto follow = [&](ValueID value) {
        if (followed.size() == maxValuesFollowed || followed.contains(value))
            return;
        if (program.typeOf(function, value).mayBeUndefinedOrNull())
            followed.append(value);
    };

    for (size_t index = 0; index < followed.size(); ++index) {
        for (const Use& use : useLists.usesOf(followed[index])) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = function.at(use.user);
                if (asksWhetherValueIsAbsent(program, function, user, use.operandIndex))
                    return true;
                if (isCast(user.opcode) && !use.operandIndex)
                    follow(use.user);
                else if (user.opcode == Opcode::TypeTest)
                    forEachValueThatPassed(function, useLists, use.user, use.operandIndex, follow);
                break;
            }
            // The cases of a switch are numbers or strings: what is absent goes where every other value goes.
            case Use::Kind::TerminatorOperand:
                if (function.at(use.block).terminator.isConditionalBranch())
                    return true;
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
                follow(parameterFilledBy(function, use));
                break;
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return false;
}

static bool convertsOperandToNumber(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (operatorsOnNumericOperands.contains(user.opcode))
        return true;
    // Beside a string the sum would hold the text "undefined".
    return user.opcode == Opcode::Add && program.typeOf(function, user.operand(1 - operandIndex)).isNumber();
}

bool everyUseConvertsToNumber(const Program& program, FunctionAnalyses& analyses, ValueID read)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();

    bool isUsed = false;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;

            // A cast that throws tells undefined from NaN.
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode) && !use.operandIndex) {
                worklist.append(use.user);
                continue;
            }
            if (!convertsOperandToNumber(program, function, user, use.operandIndex))
                return false;
            isUsed = true;
        }
    }
    return isUsed;
}

static bool isTakenApartByItsAccesses(const Program& program, const Value& array)
{
    if (localArrayAllocations.contains(array.opcode))
        return true;
    // The first elements of such a result are delivered without the array, each as the element or undefined.
    return array.opcode == Opcode::CallBuiltin && program.builtins().at(array.overload()).code.prefixForm;
}

static bool hasFormWithoutAbsentElement(const Program& program, const Function& function, const PresenceQuery& query)
{
    switch (query.form) {
    case ElementAccessForm::Array:
        return query.kind == Access::Read && hasArrayStorageForm(query.arrayStorage)
            && !isTakenApartByItsAccesses(program, function.at(sameObjectThroughCasts(function, query.receiver)));
    // A store outside a typed array does nothing, and no program relies on that.
    case ElementAccessForm::TypedArray:
        return (query.kind == Access::Read || query.kind == Access::Store) && hasInlineElementAccess(query.typedArrayType);
    case ElementAccessForm::ByKey:
    case ElementAccessForm::NamedKey:
    case ElementAccessForm::String:
    case ElementAccessForm::ArgumentsElement:
    case ElementAccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isInvariantInLoopAround(FunctionPassContext& context, const PresenceQuery& query)
{
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(query.block);
    if (!loop)
        return false;

    const UseLists& useLists = context.analyses.useLists();
    for (ValueID candidate : CastChain(context.function, query.receiver)) {
        ValueLocation location = useLists.locationOf(candidate);
        if (location.placement != ValuePlacement::Detached && !loops.belongsTo(location.block, *loop))
            return true;
    }
    return false;
}

// That a hole reads as undefined is a fact about the prototypes, whose test pays only in front of a loop.
static bool canReadHolesAsNaN(FunctionPassContext& context, const PresenceQuery& query)
{
    if (query.arrayStorage != ArrayStorage::Double || !query.keyIsInteger)
        return false;

    // The read keeps its type less undefined, which has to admit the NaN. An operand of any other type is tested by its operator in the same round, and a test is no conversion.
    const Type& type = context.program.typeOf(context.function, query.access);
    if (!type.isNumberOrNullish() || !type.mayBeNaN())
        return false;
    return isInvariantInLoopAround(context, query) && everyUseConvertsToNumber(context.program, context.analyses, query.access);
}

std::optional<PresenceTest> choosePresenceTest(FunctionPassContext& context, const PresenceQuery& query)
{
    if (!hasFormWithoutAbsentElement(context.program, context.function, query))
        return std::nullopt;
    if (context.analyses.blockFrequencies().classOf(query.block) == BlockFrequency::Rare)
        return std::nullopt;
    if (query.kind == Access::Read && isTestedForAbsence(context.analyses, query.access))
        return std::nullopt;

    if (query.form == ElementAccessForm::TypedArray)
        return PresenceTest::InsideView;
    if (query.isDense && query.keyIsInteger)
        return PresenceTest::InBounds;
    if (canReadHolesAsNaN(context, query))
        return PresenceTest::InBoundsWithHolesAsNaN;
    return PresenceTest::ElementPresent;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
