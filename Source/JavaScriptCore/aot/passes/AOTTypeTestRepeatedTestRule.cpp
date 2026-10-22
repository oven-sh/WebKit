/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTCFG.h"
#include "AOTGenericCopy.h"
#include "AOTNaturalLoops.h"
#include "AOTTypeTestConditions.h"
#include <utility>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

using SubjectAndType = std::pair<ValueID, TypeID>;

static bool canBeDecidedEarlier(TestSiteReporter& reporter, const Value& test)
{
    if (test.opcode != Opcode::TypeTest)
        return false;
    const Type& type = reporter.typeOf(test.valueOperand());
    const Type& tested = reporter.types().at(test.testedType());
    return type.overlaps(tested, reporter.program()) && !type.isSubsetOf(tested, reporter.program())
        && placeabilityOf(reporter.analyses(), makeTypeTest(test.valueOperand(), test.testedType())) == TestPlaceability::CanBePlaced;
}

static SubjectAndType subjectAndTypeOf(TestSiteReporter& reporter, const Value& test)
{
    return { reporter.analyses().identityOf(test.valueOperand()), test.testedType() };
}

static const NaturalLoop* findLoopAround(TestSiteReporter& reporter, BlockID block)
{
    if (!reporter.analyses().cfg().hasCycle())
        return nullptr;
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(block);
    while (loop && isClosedOnlyByWaysBack(reporter.analyses(), *loop))
        loop = loops.innerMostOuterLoop(*loop);
    return loop;
}

static bool isDefinedOutsideLoopAround(TestSiteReporter& reporter, ValueID value, BlockID block)
{
    const NaturalLoop* loop = findLoopAround(reporter, block);
    if (!loop)
        return false;
    ValueLocation location = reporter.analyses().useLists().locationOf(value);
    return location.placement != ValuePlacement::Detached && !reporter.analyses().naturalLoops().belongsTo(location.block, *loop);
}

// What stands behind the passing edge of the test does not run in the copy, where the test has failed.
static bool holdsAccessOrCallWhereTestFails(TestSiteReporter& reporter, const NaturalLoop& loop)
{
    const Function& function = reporter.function();
    FunctionAnalyses& analyses = reporter.analyses();
    const Terminator& terminator = function.at(reporter.point().block).terminator;
    BlockID wherePassed;
    if (terminator.isConditionalBranch() && terminator.conditionOperand() == reporter.use() && analyses.hasSingleIncomingEdge(terminator.taken().block))
        wherePassed = terminator.taken().block;

    for (unsigned index = 0; index < loop.size(); ++index) {
        if (wherePassed && analyses.dominators().dominates(wherePassed, loop.at(index)))
            continue;
        const Block& block = function.at(loop.at(index));
        if (accessClassOf(block.terminator.kind) != AccessClass::None)
            return true;
        for (ValueID value : block.values) {
            if (accessClassOf(function.at(value).opcode) != AccessClass::None)
                return true;
        }
    }
    return false;
}

enum class RarerSide : uint8_t { Neither, WherePassed, WhereFailed };

static RarerSide rarerSideOf(TestSiteReporter& reporter)
{
    BlockID block = reporter.point().block;
    const Terminator& terminator = reporter.function().at(block).terminator;
    if (!terminator.isConditionalBranch() || terminator.conditionOperand() != reporter.use())
        return RarerSide::Neither;
    BlockFrequency whenPassed = BlockFrequency::Normal;
    BlockFrequency whenFailed = BlockFrequency::Normal;
    forEachOutgoingEdge(reporter.function(), block, [&](Edge, const Target& target) {
        if (&target == &terminator.taken())
            whenPassed = reporter.function().at(target.block).frequency;
        else if (&target == &terminator.notTaken())
            whenFailed = reporter.function().at(target.block).frequency;
    });
    if (isRarerThan(whenPassed, whenFailed))
        return RarerSide::WherePassed;
    return isRarerThan(whenFailed, whenPassed) ? RarerSide::WhereFailed : RarerSide::Neither;
}

static std::optional<Value> testInFrontOf(TestSiteReporter& reporter, const Value& test, RarerSide rarerSide)
{
    if (rarerSide != RarerSide::WherePassed)
        return makeTypeTest(test.valueOperand(), test.testedType());
    Type rest = reporter.typeOf(test.valueOperand()).without(reporter.types().at(test.testedType()), reporter.types()).withoutClassProofs().withUntestableBuiltinsWidened();
    if (rest.overlaps(reporter.types().at(test.testedType()), reporter.program()))
        return std::nullopt;
    Value testForRest = makeTypeTest(test.valueOperand(), reporter.types().intern(WTF::move(rest)));
    if (placeabilityOf(reporter.analyses(), testForRest) != TestPlaceability::CanBePlaced)
        return std::nullopt;
    return testForRest;
}

// Behind a test that failed stands the copy of the failed tests, where nothing else is tested either. That is for a surprise, or for the side that is marked as the rarer one.
// Of `null === current`, asked five times of a value that is an object as often as it is null, both sides occur: the five tests stay, each is one comparison,
// and the code on both sides of them is tested code. So it is with `excludeKinds === undefined` in a loop over objects: in front of the loop it would send every round of a call that names kinds through that copy.
// A loop that holds no access and no call but behind the passing edge of the test loses nothing in the copy: there one test in front decides for all the rounds, whichever way it goes.
static bool isSurpriseWhereItFails(TestSiteReporter& reporter, const Value& testInFront)
{
    Type expected = reporter.types().at(reporter.function().at(testInFront.valueOperand()).expectedOrType()).withoutClassProofs();
    return expected.isSubsetOf(reporter.types().at(testInFront.testedType()), reporter.program());
}

// A pass that splits a value by its type tests it at each use. Such a test is a use of its own proof: where one test in front of them all or on the entry of the loop
// has passed, the operand is narrowed and the test folds. The planner keeps the site only where that saves a test.
void reportRepeatedTests(TestSiteReporter& reporter)
{
    HashMap<SubjectAndType, unsigned> numberOfTests;
    BitVector canBeDecided;
    reporter.forEachCandidate([&](const Value& test) {
        if (!canBeDecidedEarlier(reporter, test))
            return;
        canBeDecided.set(reporter.use().index());
        ++numberOfTests.add(subjectAndTypeOf(reporter, test), 0).iterator->value;
    });
    if (canBeDecided.isEmpty())
        return;

    reporter.forEachCandidate([&](const Value& test) {
        if (!canBeDecided.get(reporter.use().index()))
            return;
        SubjectAndType tested = subjectAndTypeOf(reporter, test);
        bool isInvariantInLoop = isDefinedOutsideLoopAround(reporter, tested.first, reporter.point().block);
        if (numberOfTests.get(tested) < 2 && !isInvariantInLoop)
            return;
        RarerSide rarerSide = rarerSideOf(reporter);
        std::optional<Value> testInFront = testInFrontOf(reporter, test, rarerSide);
        bool losesNothingInCopy = isInvariantInLoop && !holdsAccessOrCallWhereTestFails(reporter, *findLoopAround(reporter, reporter.point().block));
        if (testInFront && (losesNothingInCopy || rarerSide != RarerSide::Neither || isSurpriseWhereItFails(reporter, *testInFront)))
            reporter.report({ *testInFront }, Effects::none());
    });
}

// The earlier test narrows the value as it is defined. This test keeps its operand: what the casts on the way to it have added, such as a proof of flow about the class,
// goes on to the operations behind it, which may rely on it without a guard. SimplifyControlFlow decides it as a test behind an edge of a test of the same value for the same type.
void rewriteRepeatedTest(TestSiteRewriter& rewriter)
{
    rewriter.useValue().setValueOperand(rewriter.tests()[0].valueOperand());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
