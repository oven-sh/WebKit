/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTSweep.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static constexpr uint32_t mostArgumentsPassedOnByCount = 4;

static ValueID rootOfOperandOfForwardedTailCall(const Code& code, Reference operand)
{
    return operand && operand.isValue() ? rootOfCellInSweptCode(code, operand.id()) : ValueID();
}

bool passOnOwnArgumentsByCount(Walker& walker, const BodyValue& call, const Draft& written, Reference callee, Reference thisValueOrNull, Reference arguments)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !walker.opensGuesses() || walker.isUnreachable())
        return false;
    Reference count = lengthOfFreshArguments(walker, arguments);
    if (!count || !count.isValue())
        return false;
    std::optional<ListBehindParametersOfArguments> behind = listBehindParametersOfFreshArguments(walker, arguments, true);
    if (!behind || !behind->firstPosition || behind->firstPosition > mostArgumentsPassedOnByCount)
        return false;
    const Code& code = walker.code();
    ValueID object = rootOfOperandOfForwardedTailCall(code, arguments);
    if (!object || code.at(object).opcode != Opcode::CreateUnmappedArguments || !isListAsMade(walker, arguments))
        return false;
    if (rootOfOperandOfForwardedTailCall(code, callee) == object || rootOfOperandOfForwardedTailCall(code, thisValueOrNull) == object)
        return false;
    Vector<Reference, mostArgumentsPassedOnByCount> parameters;
    for (uint32_t position = 0; position < behind->firstPosition; ++position) {
        parameters.append(elementOfFreshArguments(walker, arguments, position));
        if (!parameters.last())
            return false;
    }
    if (walker.program().at(walker.currentFunction()).isModuleBody())
        return false;

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.arrivals().leaveAnything(listed);

    bool returnsAtOnce = walker.unitReturnsAtOnce(written);
    BlockID behindCall;
    Reference resultBehindCall;
    if (!returnsAtOnce) {
        behindCall = walker.addBlock();
        resultBehindCall = walker.addParameter(behindCall, { }, Representation::JSValue);
    }
    auto buildCallAndEndArm = [&](Draft&& forwarded) {
        forwarded.setCalleeOperand(callee);
        if (thisValueOrNull)
            forwarded.setThisValueOperand(thisValueOrNull);
        forwarded.setCalleeName(written.calleeName());
        forwarded.setExpectedResult(written.expectedResult());
        forwarded.expected = written.expected;
        forwarded.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
        forwarded.inputValueGiven = written.inputValueGiven;
        walker.bind(call.id(), { });
        if (!callForwardedDirectly(walker, call, forwarded)) {
            if (Reference result = walker.build(WTF::move(forwarded)))
                walker.bind(call.id(), result);
        }
        Reference result = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
        walker.bind(call.id(), { });
        if (!result)
            return;
        if (returnsAtOnce)
            walker.returnAtOnce(result);
        else
            walker.jumpTo(behindCall, singleElementSpan(result));
    };

    Type typeOfCount = walker.types().at(walker.typeOf(count));
    for (uint32_t numberPassed = behind->firstPosition + 1; numberPassed-- && !walker.isUnreachable();) {
        if (typeOfCount.isInteger() && !typeOfCount.integers.contains(numberPassed))
            continue;
        Draft number(Opcode::Integer, originInFrontOfCall);
        number.setInteger(numberPassed);
        Draft test(Opcode::StrictEq, originInFrontOfCall);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(count);
        test.setRightOperand(walker.constant(WTF::move(number)));
        if (walker.has(Mechanism::TypedOperations))
            test.setForm(formAllowedForTypedOperator(walker, test));
        Reference isNumberPassed = walker.build(WTF::move(test));
        if (!isNumberPassed)
            return true;
        BlockID passesParameters = walker.addBlock();
        BlockID otherwise = walker.addBlock(numberPassed ? BlockFrequency::Normal : BlockFrequency::Unlikely);
        walker.branch(isNumberPassed, passesParameters, otherwise);

        walker.continueIn(passesParameters);
        Draft plain(Opcode::Call, written.origin());
        walker.chooseConservativeFields(plain);
        plain.setArgumentsOperands(parameters.span().first(numberPassed));
        buildCallAndEndArm(WTF::move(plain));
        walker.continueIn(otherwise);
    }

    if (!walker.isUnreachable()) {
        Draft all(Opcode::NewArray, originInFrontOfCall);
        walker.chooseConservativeFields(all);
        all.standsForNoValueOfInput = true;
        all.setContents(TypeTable::anything);
        all.setVariadicOperands(parameters.span());
        Reference list = walker.build(WTF::move(all));
        noteFreshList(walker, list);
        Draft append(Opcode::AppendElements, originInFrontOfCall);
        walker.chooseConservativeFields(append);
        append.setArrayOperand(list);
        append.setSourceOperand(behind->list);
        if (list)
            walker.build(WTF::move(append));
        if (!walker.isUnreachable()) {
            Draft withList(Opcode::CallList, written.origin());
            walker.chooseConservativeFields(withList);
            withList.setListOperand(list);
            buildCallAndEndArm(WTF::move(withList));
        }
    }

    if (returnsAtOnce)
        return true;
    walker.continueIn(behindCall);
    if (!walker.isUnreachable())
        walker.bind(call.id(), resultBehindCall);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
