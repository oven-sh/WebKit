/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFunctionAnalyses.h"
#include "AOTJSONSerializerBuilder.h"
#include "AOTResolveCallsDirectCall.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool passesOperandsUnchanged(const BuiltinCallPlan& plan)
{
    return std::ranges::all_of(plan.steps, [](const OperandStep& step) {
        switch (step.kind) {
        case OperandStepKind::Pass:
        case OperandStepKind::PassUndefined:
            return true;
        case OperandStepKind::Convert:
        case OperandStepKind::RequireObjectCoercible:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static std::optional<BuiltinCallPlan> planTextFormCall(FunctionPassContext& pass, const OwnKeyWalkSite& site)
{
    TypingContext typing { pass.program, pass.function, pass.types };
    BuiltinOperandTypes operands = operandTypesOf(typing, builtinCallOf(pass.program, pass.function.at(site.value)));
    operands.arguments[site.sourceArgument] = TypeTable::string;

    BuiltinCallPlan plan = planBuiltinCall({ { typing, site.textForm, BuiltinCallMode::Call, operands, operands }, pass.analyses.effectContext() });
    if (!plan.isProven() || !plan.requirements.isEmpty() || !passesOperandsUnchanged(plan))
        return std::nullopt;
    if (site.sourceArgument >= plan.numberOfArguments)
        return std::nullopt;
    return plan;
}

static ValueID appendTextFormCall(IRBuilder& builder, const Value& original, const BuiltinCallPlan& plan, uint32_t textArgument, ValueID text)
{
    BuiltinCallOperands operands { original.receiverOperand(), Vector<ValueID, 4>(original.argumentsOperands()) };
    operands.arguments[textArgument] = text;
    BuiltinCallOperands passedOperands = insertOperandSteps(plan, operands, { }, [&](auto&& value) {
        return builder.append(WTF::move(value));
    });

    BuiltinCallStamps stamps { original.expectedResult(), original.accessSite(), original.allocationSite() };
    return builder.append(makeBuiltinCall(plan.overload, BuiltinCallForm::Whole, passedOperands, stamps));
}

bool OwnKeyWalkExpander::expandStringify(const OwnKeyWalkSite& site)
{
    IRBuilder& builder = this->builder();
    if (!JSONSerializerBuilder(builder, inputs(), ValueID()).hasPlanFor(site.source))
        return false;
    // A string builder for one string, number, boolean or null costs three times what the stub of the call needs to write it.
    if (site.walk == OwnKeyWalk::Stringify && !m_pass.program.typeOf(m_pass.function, site.source).hasObjects())
        return false;

    std::optional<BuiltinCallPlan> textFormCall;
    if (site.walk == OwnKeyWalk::StringifyIntoText) {
        textFormCall = planTextFormCall(m_pass, site);
        if (!textFormCall)
            return false;
    }

    m_originalOperation = builder.function().at(site.value);
    IRBuilder::Expansion expansion = builder.beginExpansion({ site.block, site.index });
    BlockID generic = builder.addBlock(BlockFrequency::Rare);
    BlockID noText = builder.addBlock(BlockFrequency::Rare);

    ValueID stringBuilder = builder.append(Opcode::StringBuilderBegin, { });
    JSONSerializerBuilder serializer(builder, inputs(), stringBuilder);
    serializer.appendRoot(site.source, noText);
    serializer.commit();

    ValueID result = builder.append(Opcode::StringBuilderFinish, { stringBuilder });
    if (textFormCall)
        result = appendTextFormCall(builder, m_originalOperation, *textFormCall, site.sourceArgument, result);
    builder.endExpansion(expansion, convertTo(builder, result, DeclaredType { .type = m_originalOperation.type }));

    builder.moveToEndOf(noText);
    builder.append(Opcode::StringBuilderDiscard, { stringBuilder });
    builder.emitJump(generic);

    builder.moveToEndOf(generic);
    builder.endExpansion(expansion, emitOriginalOperation(site));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
