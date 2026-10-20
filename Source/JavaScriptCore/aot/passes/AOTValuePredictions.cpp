/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValuePredictions.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static DeclaredType* findDeclaredEntryParameter(Function& function, ValueID value)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span().first(function.numberOfEntryParameters());
    auto parameter = std::ranges::find(parameters, value);
    return parameter == parameters.end() ? nullptr : function.declaredEntryParameter(static_cast<unsigned>(parameter - parameters.begin()));
}

// A prediction where the value is defined reaches every use of it and stays when the values are typed again.
bool predictValues(FunctionPassContext& context, std::span<const ValuePrediction> predictions)
{
    Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);

    struct PlannedPrediction {
        ValueID value;
        TypeID predicted;
        ProgramPoint point;
        Vector<Use, 4> uses;
    };
    Vector<PlannedPrediction, 4> plannedPredictions;
    Vector<ValueID, 4> changed;
    for (const ValuePrediction& prediction : predictions) {
        TypeID type = function.at(prediction.value).type;
        TypeID predicted = context.types.intersectionOf(type, prediction.predicted);
        if (predicted == TypeTable::never || predicted == type)
            continue;
        if (DeclaredType* declared = findDeclaredEntryParameter(function, prediction.value)) {
            declared->predicted = predicted;
            changed.append(prediction.value);
            continue;
        }
        if (useLists.locationOf(prediction.value).placement == ValuePlacement::Detached)
            continue;
        PlannedPrediction planned { prediction.value, predicted, useLists.pointAfter(prediction.value), { } };
        for (const Use& use : useLists.usesOf(prediction.value)) {
            // The handler of a block is entered with what is there at the head of the block.
            if (use.kind != Use::Kind::HandlerArgument && acceptsCastOfValue(function, use))
                planned.uses.append(use);
        }
        plannedPredictions.append(WTF::move(planned));
    }

    // The first insertion ends the life of the use lists.
    for (const PlannedPrediction& planned : plannedPredictions) {
        Value predict = IRBuilder::make(Opcode::Predict, { planned.value });
        predict.setPredicted(planned.predicted);
        builder.setOrigin(function.at(planned.value).origin());
        ValueID predictedValue = builder.insertBefore(planned.point, WTF::move(predict));
        for (const Use& use : planned.uses)
            replaceUse(function, use, predictedValue);
        changed.append(predictedValue);
    }
    if (changed.isEmpty())
        return false;

    builder.applyInsertions();
    context.analyses.didChangeValues();
    narrowValueTypes(TypePropagationInputs { context.program, context.id, function, context.analyses, context.types, context.build }, changed.span());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
