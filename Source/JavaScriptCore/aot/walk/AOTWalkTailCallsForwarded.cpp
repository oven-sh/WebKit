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

OperatorForm operatorFormFor(Walker&, const Draft&);

static constexpr uint32_t mostArgumentsPassedOnByCount = 4;

static ValueID rootOfOperandOfForwardedTailCall(const Code& code, Reference operand)
{
    return operand && operand.isValue() ? underlyingCellOf(code, operand.id()) : ValueID();
}

bool passOnOwnArgumentsByCount(Walker& walker, const BodyValue& call, const Draft& written, Reference callee, Reference thisValueOrNull, Reference arguments)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !walker.opensGuesses() || walker.isUnreachable())
        return false;
    Reference count = lengthOfFreshArguments(walker, arguments);
    if (!count || !count.isValue())
        return false;
    std::optional<ListBehindParametersOfArguments> behind = listBehindParametersOfFreshArguments(walker, arguments, true);
    if (!behind || behind->firstPosition > mostArgumentsPassedOnByCount)
        return false;
    const Code& code = walker.code();
    ValueID object = rootOfOperandOfForwardedTailCall(code, arguments);
    if (!object || code.at(object).opcode != Opcode::CreateUnmappedArguments || !isKnownPristine(walker, arguments))
        return false;
    bool readsArgumentsOfEntry = walker.walksBodyBehindEntry() && !code.at(object).origin().inlineFrame;
    if (!behind->firstPosition && !readsArgumentsOfEntry)
        return false;
    uint32_t mostPassedOnByCount = readsArgumentsOfEntry ? mostArgumentsPassedOnByCount : behind->firstPosition;
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
    for (uint32_t position = behind->firstPosition; position < mostPassedOnByCount; ++position) {
        parameters.append(walker.argumentOfEntry(position, originInFrontOfCall));
        if (!parameters.last())
            return true;
    }
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(listed);

    bool returnsAtOnce = walker.unitReturnsAtOnce(written);
    BlockID continuationBlock;
    Reference mergedResult;
    if (!returnsAtOnce) {
        continuationBlock = walker.addBlock();
        mergedResult = walker.addParameter(continuationBlock, { }, Representation::JSValue);
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
            walker.jumpTo(continuationBlock, singleElementSpan(result));
    };

    Type typeOfCount = walker.types().at(walker.typeOf(count));
    for (uint32_t numberPassed = mostPassedOnByCount + 1; numberPassed-- && !walker.isUnreachable();) {
        if (typeOfCount.isInteger() && !typeOfCount.integers.contains(numberPassed))
            continue;
        Draft number(Opcode::Integer, originInFrontOfCall);
        number.setInteger(numberPassed);
        Draft test(Opcode::StrictEq, originInFrontOfCall);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(count);
        test.setRightOperand(walker.constant(WTF::move(number)));
        if (walker.has(Mechanism::TypedOperations))
            test.setForm(operatorFormFor(walker, test));
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
        all.setVariadicOperands(parameters.span().first(behind->firstPosition));
        Reference listBehindParameters = readsArgumentsOfEntry ? walker.rematerializeHiddenRestArray() : behind->list;
        if (!listBehindParameters) {
            walker.fail("the list of arguments of an entry cannot be made again"_s);
            return true;
        }
        bool standsForWholeList = readsArgumentsOfEntry && !behind->firstPosition;
        Reference list = standsForWholeList ? listBehindParameters : walker.build(WTF::move(all));
        if (!standsForWholeList)
            noteFreshList(walker, list);
        Draft append(Opcode::AppendElements, originInFrontOfCall);
        walker.chooseConservativeFields(append);
        append.setArrayOperand(list);
        append.setSourceOperand(listBehindParameters);
        if (list && !standsForWholeList)
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
    walker.continueIn(continuationBlock);
    if (!walker.isUnreachable())
        walker.bind(call.id(), mergedResult);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
