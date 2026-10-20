/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

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

static bool isDefinedOutsideLoopAround(TestSiteReporter& reporter, ValueID value, BlockID block)
{
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(block);
    if (!loop)
        return false;
    ValueLocation location = reporter.analyses().useLists().locationOf(value);
    return location.placement != ValuePlacement::Detached && !loops.belongsTo(location.block, *loop);
}

// A pass that splits a value by its type tests it at each use. Such a test is a use of its own proof: where one test in front of them all or on the entry of the loop
// has passed, the operand is narrowed and the test folds. The planner keeps the site only where that saves a test.
void reportRepeatedTests(TestSiteReporter& reporter)
{
    HashMap<SubjectAndType, unsigned> numberOfTests;
    reporter.forEachCandidate([&](const Value& test) {
        if (canBeDecidedEarlier(reporter, test))
            ++numberOfTests.add(subjectAndTypeOf(reporter, test), 0).iterator->value;
    });

    reporter.forEachCandidate([&](const Value& test) {
        if (!canBeDecidedEarlier(reporter, test))
            return;
        SubjectAndType tested = subjectAndTypeOf(reporter, test);
        if (numberOfTests.get(tested) < 2 && !isDefinedOutsideLoopAround(reporter, tested.first, reporter.point().block))
            return;
        reporter.report({ makeTypeTest(test.valueOperand(), test.testedType()) }, Effects::none());
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
