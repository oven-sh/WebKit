/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTRuleSupport.h"
#include "AOTSplitUnions.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EnumerationPosition {
    ValueID open;
    ProgramPoint pointAfterOpen;
    bool hasUnthreadedStep { false };
    StateVariable position;
};

struct UnthreadedStep {
    ValueID value;
    ProgramPoint pointAfterValue;
    size_t enumeration;
};

} // anonymous namespace

static ProgramPoint pointAfter(const ValueSite& site)
{
    return { site.block, site.index + 1 };
}

PassOutcome threadForInPositions(FunctionPassContext& pass, IRBuilder& builder)
{
    Function& function = pass.function;
    Vector<EnumerationPosition, 2> enumerations;
    HashMap<ValueID, size_t> enumerationOfOpen;
    Vector<UnthreadedStep, 4> steps;
    // An open dominates its steps, so the walk comes to it first.
    forEachValueSite(pass.analyses, function, [&](const ValueSite& site) {
        const Value& value = function.at(site.value);
        if (value.opcode == Opcode::ForInOpen) {
            enumerationOfOpen.add(site.value, enumerations.size());
            enumerations.append(EnumerationPosition { .open = site.value, .pointAfterOpen = pointAfter(site) });
            return;
        }
        if (value.opcode != Opcode::ForInStep || value.positionOperand())
            return;
        auto enumeration = enumerationOfOpen.find(value.enumeratorOperand());
        RELEASE_ASSERT(enumeration != enumerationOfOpen.end());
        enumerations[enumeration->value].hasUnthreadedStep = true;
        steps.append(UnthreadedStep { site.value, pointAfter(site), enumeration->value });
    });
    if (steps.isEmpty())
        return PassOutcome::Unchanged;

    SSARepair repair(builder, pass.analyses);
    for (EnumerationPosition& enumeration : enumerations) {
        if (!enumeration.hasUnthreadedStep)
            continue;
        builder.setOrigin(function.at(enumeration.open).origin());
        ValueID zero = builder.insertBefore(enumeration.pointAfterOpen, makeInteger(0));
        enumeration.position = StateVariable { repair.addVariable(zero), zero };
    }

    for (const UnthreadedStep& step : steps) {
        const StateVariable& position = enumerations[step.enumeration].position;
        builder.setOrigin(function.at(step.value).origin());
        ValueID found = builder.insertBefore(step.pointAfterValue, makeResultAt(step.value, static_cast<uint32_t>(ForInStepResult::Position)));
        ValueID one = builder.insertBefore(step.pointAfterValue, makeInteger(1));
        ValueID next = builder.insertBefore(step.pointAfterValue, IRBuilder::make(Opcode::Add, { found, one }));
        function.edit(step.value).setPositionOperand(position.anyDefinition);
        repair.addDefinition(position.variable, next);
    }

    builder.applyInsertions();
    pass.analyses.didChangeValues();
    repair.run();
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
