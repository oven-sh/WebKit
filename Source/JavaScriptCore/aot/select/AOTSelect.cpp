/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeShapes.h"
#include "AOTSelectionContext.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"

namespace JSC { namespace AOT { namespace Typed {

static PassOutcome clearPredictions(Function& function)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (Value& value : function.values) {
        if (!value.written() && !value.prediction())
            continue;
        value.setWritten(TypeID());
        value.setPrediction(TypeID());
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

static bool removePredictOperations(FunctionPassContext& pass)
{
    Function& function = pass.function;
    auto isPredict = [&](ValueID id) {
        return function.at(id).opcode == Opcode::Predict;
    };

    ValueSubstitution substitution(function);
    for (Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (isPredict(id))
                substitution.replace(id, function.at(id).valueOperand());
        }
        block.values.removeAllMatching(isPredict);
    }
    if (substitution.isEmpty())
        return false;

    substitution.apply(function, &pass.analyses);
    return true;
}

static void restoreExactTypes(FunctionPassContext& pass, SelectionContext& context)
{
    std::span<const ValueID> pending = context.narrowedValues();
    if (pending.empty())
        return;

    TypePropagationInputs inputs { pass.program, pass.id, pass.function, pass.analyses, pass.types, pass.build };
    TypePropagation propagation(inputs);
    if (context.hasWidenedValue())
        propagation.restartAllValues();
    else
        propagation.restartDependentsOf(pending);
    propagation.solveTypes();
    TypePropagationOutcome outcome = propagation.finish();

    context.didRestoreTypes();
    if (outcome.didChangeControlFlow())
        context.didRenumberValues();
}

PassOutcome select(FunctionPassContext& pass)
{
    bool removedValues = removePredictOperations(pass);
    PassOutcome outcome = removedValues ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= clearPredictions(pass.function);
    outcome |= chooseCodeShapes(pass, ChosenShapes::ChooseAgain);

    SelectionContext context(pass);
    for (const SelectionRule& rule : selectionRules()) {
        if (!context.isEnabled(rule.id))
            continue;
        rule.apply(context);
        restoreExactTypes(pass, context);
    }
    if (context.didRewrite()) {
        outcome = PassOutcome::Changed;
        // The program is still in form Prepared, so a return call that a rule demoted left what its casts predict as Predict operations.
        removedValues |= removePredictOperations(pass);
    }

    if (context.needsCompaction() || removedValues) {
        compactFunction(pass.function);
        pass.analyses.didChangeControlFlow();
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
