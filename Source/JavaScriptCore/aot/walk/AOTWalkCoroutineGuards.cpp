/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The guard of a coroutine stands in its ramp: the parameters are tested once, in front of the start, and the CoroutineBody takes what the tests have proven however often it is resumed.
// At a suspension: nothing, the ramp never suspends, and either body saves its own frame. At a throw: either body hands back what its role says, so the two starts meet.
// Where a test fails the SlowBody is started, which takes nothing for granted. Withheld (ParameterGuards): the plan gives the function one body, and nothing is built here.

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTPlan.h"

namespace JSC { namespace AOT { namespace Typed {

bool rampTestsParametersOf(const CallingConventions& conventions, FunctionID function)
{
    FunctionConvention convention = conventions.of(function);
    return convention.has(CodeRole::CoroutineBody) && convention.has(CodeRole::SlowBody);
}

// It leaves no arrivals: what the callers of the ramp have left speaks of the parameters of the bodies too.
static Reference buildStartInCoroutineGuards(Walker& walker, CodeRole role, std::span<const Reference> operands, Origin origin)
{
    Draft start(Opcode::CallDirect, origin);
    walker.chooseConservativeFields(start);
    start.setFunction(walker.unit().function);
    start.setRole(role);
    start.setNumberOfBoundArguments(0);
    start.setIsNeverInlined(true);
    start.setVariadicOperands(operands);
    return walker.build(WTF::move(start));
}

// BuilderOfEntries::buildParameterGuard(), with calls that come back in the place of its tail calls: the ramp still has to hand back the driver.
Reference buildStartBehindTestsOfRamp(Walker& walker, std::span<const Reference> operands, Origin origin)
{
    FunctionID function = walker.unit().function;
    const CallingConventions& conventions = walker.plan().conventions();
    if (!rampTestsParametersOf(conventions, function))
        return { };
    TypeArena& types = walker.types();
    origin.isTailCall = false;
    unsigned numberOfEntryParameters = static_cast<unsigned>(operands.size() - 1);

    // The plan chose the tests before anything was walked. Where a test can never pass, or leaves something behind it that no type says, no call comes through.
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        TypeID tested = conventions.typeBehindGuard(function, index);
        if (!tested)
            continue;
        TypeID behindTest = types.intersectionOf(walker.typeOf(operands[index]), tested);
        if (!isNeverType(behindTest) && types.isSubset(behindTest, tested))
            continue;
        return buildStartInCoroutineGuards(walker, CodeRole::SlowBody, operands, origin);
    }

    BlockID failing;
    Vector<Reference, 16> passed(operands);
    for (unsigned index = 0; index < numberOfEntryParameters && !walker.isUnreachable(); ++index) {
        TypeID tested = conventions.typeBehindGuard(function, index);
        if (!tested || types.isSubset(walker.typeOf(operands[index]), tested))
            continue;

        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(operands[index]);
        test.setTestedType(tested);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            break;

        BlockID passing = walker.addBlock();
        if (!failing)
            failing = walker.addBlock(BlockFrequency::Rare);
        walker.branch(passes, passing, failing);
        walker.continueIn(passing);

        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(operands[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        passed[index] = walker.build(WTF::move(narrowed));
    }

    // A passed test may leave a type that says less than the test did. The CoroutineBody takes only what is proven.
    Reference startedBehindTests;
    if (!walker.isUnreachable()) {
        bool allIsProven = true;
        for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
            TypeID tested = conventions.typeBehindGuard(function, index);
            allIsProven &= !tested || types.isSubset(walker.typeOf(passed[index]), tested);
        }
        startedBehindTests = allIsProven ? buildStartInCoroutineGuards(walker, CodeRole::CoroutineBody, passed.span(), origin) : buildStartInCoroutineGuards(walker, CodeRole::SlowBody, operands, origin);
    }
    if (walker.hasFailed() || !failing)
        return startedBehindTests;

    BlockID meeting = walker.addBlock();
    Reference started = walker.addParameter(meeting, { }, Representation::JSValue);
    bool someWayArrives = false;
    if (startedBehindTests) {
        walker.jumpTo(meeting, singleElementSpan(startedBehindTests));
        someWayArrives = true;
    }
    walker.continueIn(failing);
    if (Reference startedSlowly = buildStartInCoroutineGuards(walker, CodeRole::SlowBody, operands, origin)) {
        walker.jumpTo(meeting, singleElementSpan(startedSlowly));
        someWayArrives = true;
    }
    if (!someWayArrives || walker.hasFailed())
        return { };
    walker.continueIn(meeting);
    return started;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
