/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding: a switch lists no case that the proven type of its subject rules out, and none that comes to where the rest comes to.
// At a suspension: nothing. At a throw: a SwitchString may throw where it resolves a rope, and Walker::endBlockLeadingTo() leads its handler edge.
// Withheld: every case is listed as it is written.

#include "config.h"
#include "AOTWalkBranches.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTShapeTest.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!OperandIndex::Switch::subject && !OperandIndex::SwitchString::subject && !OperandIndex::SwitchOnShape::subject);

// A case that lists no layout, or only layouts that are never allocated, passes for no object.
static bool anyLayoutOfCaseHasShapeInWalkSwitches(Walker& walker, LayoutSetID layouts)
{
    const ShapeTable& shapes = walker.plan().shapes();
    return std::ranges::any_of(membersOf(walker.types(), layouts), [&](LayoutID layout) {
        return !shapes.shapesOf(layout).isEmpty();
    });
}

// Null: nothing may be concluded from the type, or it holds whatever a case of that kind can name.
static TypeID typeThatMayRuleOutCasesInWalkSwitches(Walker& walker, TerminatorKind kind, Reference subject)
{
    TypeID type = walker.typeOf(subject);
    if (!type || isNeverType(type))
        return { };
    Type known = walker.types().at(type);
    if (known.mayBeUnspecified())
        return { };

    bool holdsEveryCase = false;
    switch (kind) {
    case TerminatorKind::Switch:
        holdsEveryCase = (known.primitives & Primitive::Integer) && known.integers == Range::full();
        break;
    case TerminatorKind::SwitchString:
        holdsEveryCase = known.primitives & Primitive::String;
        break;
    default:
        holdsEveryCase = known.anyObject;
        break;
    }
    return holdsEveryCase ? TypeID() : type;
}

// `switch (node.kind)`: what lay in the slot says which layouts can have held it.
static bool isReadThatNarrowsItsHolderInWalkSwitches(const Code& code, ValueID value)
{
    while (opcodesThatGiveSameValue.contains(code.at(value).opcode))
        value = code.at(value).valueOperand();
    return readsThatNarrowTheirHolders.contains(code.at(value).opcode);
}

// `left` lists every case that the type leaves, and there is one. A case that comes to where the rest comes to needs no test. No value is listed twice: BodyVerifier::verifyTerminatorOperation().
static void leaveCasesOnWayOfRestUntestedInWalkSwitches(Walker& walker, const BodyTerminator& terminator, const TerminatorDraft& asWritten, CasesLeftOfSwitch& left)
{
    const BodyIndex& index = walker.currentBodyIndex();
    // BodyTargets::operator[] passes every target in front of the one that it is asked for.
    Vector<BodyTarget, 16> targets;
    for (BodyTarget target : terminator.targets())
        targets.append(target);
    WayBehindTerminator wayOfRest = wayBehindTarget(walker, terminator.block(), left.rest, targets[left.rest]);

    Vector<uint32_t, 16> testedCases;
    for (uint32_t caseIndex : left.cases) {
        if (!comeToSamePlace(index, wayBehindTarget(walker, terminator.block(), caseIndex, targets[caseIndex]), wayOfRest))
            testedCases.append(caseIndex);
    }
    if (testedCases.size() == left.cases.size())
        return;

    // A range has no holes. A tested case between two that are not would come back into the type of the subject on the edge of the rest, and its layouts into the type of the holder.
    Reference subject = asWritten.subjectOperand();
    TypeID subjectType = walker.typeOf(subject);
    if (asWritten.kind == TerminatorKind::Switch && subject.isValue() && subjectType && !isNeverType(subjectType) && isReadThatNarrowsItsHolderInWalkSwitches(walker.code(), subject.id())) {
        auto integersLeftToRestBy = [&](std::span<const uint32_t> cases) {
            Vector<int64_t, 16> integers;
            for (uint32_t caseIndex : cases)
                integers.append(asWritten.cases()[caseIndex]);
            return walker.types().at(narrowByOtherwise(walker.types(), subjectType, integers.span())).integers;
        };
        Range leftByEveryCase = integersLeftToRestBy(left.cases.span());
        Range leftByTestedCases = integersLeftToRestBy(testedCases.span());
        bool someTestedCaseComesBack = std::ranges::any_of(testedCases, [&](uint32_t caseIndex) {
            int64_t integer = asWritten.cases()[caseIndex];
            return leftByTestedCases.contains(integer) && !leftByEveryCase.contains(integer);
        });
        if (someTestedCaseComesBack)
            return;
    }
    left.cases = WTF::move(testedCases);
}

CasesLeftOfSwitch casesLeftOfSwitch(Walker& walker, const BodyTerminator& terminator, const TerminatorDraft& asWritten)
{
    uint32_t numberOfCases = terminator.numberOfCases();
    TypingContext typing = walker.typing();
    TypeID subjectType = typeThatMayRuleOutCasesInWalkSwitches(walker, asWritten.kind, asWritten.subjectOperand());

    CasesLeftOfSwitch left;
    left.rest = numberOfCases;
    for (uint32_t caseIndex = 0; caseIndex < numberOfCases; ++caseIndex) {
        if (asWritten.kind == TerminatorKind::SwitchOnShape && !anyLayoutOfCaseHasShapeInWalkSwitches(walker, asWritten.layoutSets()[caseIndex]))
            continue;
        if (subjectType && isNeverType(narrowSubjectByCase(typing, asWritten, subjectType, caseIndex)))
            continue;
        left.cases.append(caseIndex);
    }

    // Two cases of a SwitchOnShape may name one layout, and the first of them takes it.
    if (!left.cases.isEmpty() && asWritten.kind != TerminatorKind::SwitchOnShape)
        leaveCasesOnWayOfRestUntestedInWalkSwitches(walker, terminator, asWritten, left);
    return left;
}

template<typename Case, typename Cases>
static Vector<Case, 16> casesKeptInWalkSwitches(const Cases& cases, const CasesLeftOfSwitch& left)
{
    Vector<Case, 16> kept;
    kept.reserveInitialCapacity(left.cases.size());
    for (uint32_t caseIndex : left.cases)
        kept.append(cases[caseIndex]);
    return kept;
}

bool Walker::endBlockWithCasesLeft(const BodyTerminator& terminator)
{
    // A constant is for onlyTargetKnownToFolding().
    uint32_t numberOfCases = terminator.numberOfCases();
    Reference subject = referenceOf(terminator.operand(0));
    if (!numberOfCases || !subject || !subject.isValue())
        return false;

    TerminatorDraft draft(terminator.kind(), m_origin);
    copyFields(terminator, draft, [&](ValueID operand) {
        return referenceOf(operand);
    });
    CasesLeftOfSwitch left = casesLeftOfSwitch(*this, terminator, draft);
    if (left.cases.size() == numberOfCases)
        return false;

    Vector<EdgeToTargetOfBody, 16> edges;
    for (uint32_t caseIndex : left.cases)
        edges.append(EdgeToTargetOfBody { terminator.block(), caseIndex });
    edges.append(EdgeToTargetOfBody { terminator.block(), left.rest });

    if (left.cases.isEmpty())
        draft.become(TerminatorKind::Jump);
    else if (draft.kind == TerminatorKind::Switch)
        draft.setCases(casesKeptInWalkSwitches<int64_t>(draft.cases(), left).span());
    else if (draft.kind == TerminatorKind::SwitchString)
        draft.setStrings(casesKeptInWalkSwitches<StringID>(draft.strings(), left).span());
    else
        draft.setLayoutSets(casesKeptInWalkSwitches<LayoutSetID>(draft.layoutSets(), left).span());
    endBlockLeadingTo(WTF::move(draft), edges.span());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
