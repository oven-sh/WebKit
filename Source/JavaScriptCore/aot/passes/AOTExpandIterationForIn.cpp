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

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EnumerationPosition {
    ValueID open;
    ProgramPoint pointAfterOpen;
    StateVariable position;
};

struct UnthreadedStep {
    ValueID value;
    ProgramPoint pointAfterValue;
    size_t enumeration;
};

} // anonymous namespace

PassOutcome threadForInPositions(FunctionPassContext& pass, IRBuilder& builder)
{
    Function& function = pass.function;
    Vector<EnumerationPosition, 2> enumerations;
    Vector<UnthreadedStep, 4> steps;
    {
        const UseLists& useLists = pass.analyses.useLists();
        forEachValueSite(pass.analyses, function, [&](const ValueSite& site) {
            const Value& value = function.at(site.value);
            if (value.opcode != Opcode::ForInStep || value.positionOperand())
                return;
            ValueID open = value.enumeratorOperand();
            RELEASE_ASSERT(function.at(open).opcode == Opcode::ForInOpen);
            size_t enumeration = enumerations.findIf([&](const EnumerationPosition& candidate) {
                return candidate.open == open;
            });
            if (enumeration == notFound) {
                enumeration = enumerations.size();
                enumerations.append(EnumerationPosition { open, useLists.pointAfter(open), { } });
            }
            steps.append(UnthreadedStep { site.value, useLists.pointAfter(site.value), enumeration });
        });
    }
    if (steps.isEmpty())
        return PassOutcome::Unchanged;

    SSARepair repair(builder, pass.analyses);
    for (EnumerationPosition& enumeration : enumerations) {
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
        function.at(step.value).setPositionOperand(position.anyDefinition);
        repair.addDefinition(position.variable, next);
    }

    builder.applyInsertions();
    pass.analyses.didChangeValues();
    repair.run();
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
