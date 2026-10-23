/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Match {
    SelectionGroup group;
    TypeID testedType;
};

} // anonymous namespace

// The taken edge would pass `absent` where the program holds undefined.
static bool passesValueOnTakenEdge(SelectionContext& context, ValueID test)
{
    const Function& function = context.function();
    return std::ranges::any_of(context.analyses().useLists().usesOf(test), [&](const Use& use) {
        if (use.kind != Use::Kind::TerminatorOperand)
            return false;
        const Terminator& terminator = function.at(use.block).terminator;
        return terminator.isConditionalBranch() && !terminator.taken().proofs.isEmpty();
    });
}

static bool isEncodedReadUsedOnlyBy(SelectionContext& context, ValueID read, ValueID test)
{
    const Value& value = context.function().at(read);
    return value.opcode == Opcode::GetModuleVariable
        && context.slots().variable(value.variable()).encoding == VariableEncoding::UndefinedAsZero
        && context.singleUser(read) == test;
}

void selectEncodedVariableTest(SelectionContext& context)
{
    const Program& program = context.program();
    const Function& function = context.function();
    Vector<Match> matches;
    for (ValueID test : context.candidates(Opcode::TypeTest)) {
        const Value& value = function.at(test);
        const Type& tested = program.at(value.testedType());
        if (!tested.isUndefinedOrNull() || !(tested.primitives & Primitive::Undefined))
            continue;
        ValueID read = value.valueOperand();
        if (!isEncodedReadUsedOnlyBy(context, read, test) || passesValueOnTakenEdge(context, test))
            continue;
        Type encoded = tested.withoutPrimitives(Primitive::Undefined).withPrimitives(Primitive::Absent);
        Match match { groupOf({ read, test }), context.pass().types.intern(encoded) };
        if (context.tryClaim(SelectionRuleID::EncodedVariableTest, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }

    context.rewriteGroups(SelectionRuleID::EncodedVariableTest, matches, [](GroupRewriter& rewriter, const Match& match) {
        const Value& read = rewriter.original(match.group.members.first().value);
        Value encodedRead = IRBuilder::make(Opcode::GetEncodedModuleVariable);
        encodedRead.setVariable(read.variable());
        encodedRead.setOrigin(read.origin());
        ValueID word = rewriter.insert(WTF::move(encodedRead));
        Value& test = rewriter.replaceRoot(Opcode::TypeTest);
        test.setValueOperand(word);
        test.setTestedType(match.testedType);
        rewriter.leaveUnclaimed(match.group.root);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
