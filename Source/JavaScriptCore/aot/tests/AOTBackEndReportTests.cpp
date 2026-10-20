/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTBackEndReport.h"
#include "AOTBuildContext.h"
#include "AOTCodeGenerator.h"
#include "AOTCompileOptionWords.h"
#include "AOTFrameAddressValidation.h"
#include "AOTIRTestSupport.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3SlotBaseValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "GPRInfo.h"

namespace JSC { namespace AOT {

static constexpr uint32_t addressOrigin = 1;
static constexpr uint32_t suspensionOrigin = 2;
static constexpr uint32_t otherOrigin = 3;
static constexpr uint32_t secondAddressOrigin = 4;

namespace {

enum class Suspends : bool { No, Yes };

class ReportedProgram {
public:
    static constexpr uint32_t moduleIndex = 2;
    static constexpr uint32_t numberOfValues = 16;

    ReportedProgram()
    {
        m_functionAsRead = m_test.addFunction(createFunction());
        Typed::Function derived = createFunction();
        derived.derivation.source = m_functionAsRead;
        m_derivedFunction = m_test.addFunction(WTF::move(derived));
        m_test.program.setNumberOfFunctionsAsRead(1);
    }

    static constexpr SourceLocation locationOf(uint32_t value) { return { moduleIndex, 100 + value }; }

    const Typed::Program& program() const { return m_test.program; }
    Typed::FunctionID functionAsRead() const { return m_functionAsRead; }
    Typed::FunctionID derivedFunction() const { return m_derivedFunction; }

    bool report(BuildContext& build, Typed::FunctionID function, const B3::CompilationReport& raw, std::span<const FrameAddressViolation> violations = { }) const
    {
        BackEndReport report = collectBackEndReport(raw, violations, build.options().collectsDeclines());
        return reportBackEnd(build, function, program().at(function), report, raw);
    }

private:
    static Typed::Function createFunction()
    {
        Typed::Function function;
        function.module = Typed::ModuleID(moduleIndex);
        for (uint32_t value = 0; value < numberOfValues; ++value)
            function.at(Typed::addUnlistedValue(function, Typed::Opcode::Parameter)).position = locationOf(value).position;
        return function;
    }

    Typed::TestProgram m_test;
    Typed::FunctionID m_functionAsRead;
    Typed::FunctionID m_derivedFunction;
};

} // anonymous namespace

static B3::Origin originOf(uint32_t number)
{
    return Typed::b3OriginOf(Typed::ValueID(number));
}

static bool isOrigin(B3::Origin origin, uint32_t number)
{
    return origin.dfgOrigin() == originOf(number).dfgOrigin();
}

static BitVector originsOfSuspensions()
{
    BitVector origins;
    origins.set(suspensionOrigin);
    return origins;
}

static B3::Value* appendSlotBase(B3::Procedure& proc, B3::BasicBlock* block, uint32_t origin)
{
    return block->appendNew<B3::SlotBaseValue>(proc, originOf(origin), proc.addStackSlot(8));
}

static B3::PatchpointValue* appendSuspension(B3::Procedure& proc, B3::BasicBlock* block, Suspends suspends)
{
    return block->appendNew<B3::PatchpointValue>(proc, B3::Void, originOf(suspends == Suspends::Yes ? suspensionOrigin : otherOrigin));
}

static void appendStore(B3::Procedure& proc, B3::BasicBlock* block, B3::Value* address)
{
    B3::Value* zero = block->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 0);
    block->appendNew<B3::MemoryValue>(proc, B3::Store, originOf(otherOrigin), zero, address);
}

static B3::Value* appendPhi(B3::Procedure& proc, B3::BasicBlock* block, uint32_t origin)
{
    return block->appendNew<B3::Value>(proc, B3::Phi, B3::pointerType(), originOf(origin));
}

static void appendUpsilon(B3::Procedure& proc, B3::BasicBlock* block, B3::Value* value, B3::Value* phi)
{
    block->appendNew<B3::UpsilonValue>(proc, originOf(otherOrigin), value, phi);
}

static void appendReturn(B3::Procedure& proc, B3::BasicBlock* block, B3::Value* value = nullptr)
{
    if (value)
        block->appendNew<B3::Value>(proc, B3::Return, originOf(otherOrigin), value);
    else
        block->appendNew<B3::Value>(proc, B3::Return, originOf(otherOrigin));
}

static void appendJump(B3::Procedure& proc, B3::BasicBlock* block, B3::BasicBlock* target)
{
    block->appendNew<B3::Value>(proc, B3::Jump, originOf(otherOrigin));
    block->setSuccessors(B3::FrequentedBlock(target));
}

static void appendBranch(B3::Procedure& proc, B3::BasicBlock* block, B3::BasicBlock* taken, B3::BasicBlock* notTaken)
{
    B3::Value* predicate = block->appendNew<B3::Const32Value>(proc, originOf(otherOrigin), 1);
    block->appendNew<B3::Value>(proc, B3::Branch, originOf(otherOrigin), predicate);
    block->setSuccessors(B3::FrequentedBlock(taken), B3::FrequentedBlock(notTaken));
}

template<typename GraphBuilder>
static Vector<FrameAddressViolation> findViolations(const GraphBuilder& buildGraph, Suspends suspends)
{
    B3::Procedure proc;
    buildGraph(proc, suspends);
    Vector<FrameAddressViolation> violations;
    bool changed = validateFrameAddresses(violations, originsOfSuspensions()).run->run(proc);
    AOT_TEST_CHECK(!changed);
    return violations;
}

template<typename GraphBuilder>
static void checkNoViolation(const GraphBuilder& buildGraph)
{
    AOT_TEST_CHECK(findViolations(buildGraph, Suspends::Yes).isEmpty());
}

template<typename GraphBuilder>
static void checkOneViolation(const GraphBuilder& buildGraph)
{
    Vector<FrameAddressViolation> violations = findViolations(buildGraph, Suspends::Yes);
    AOT_TEST_CHECK(violations.size() == 1);
    AOT_TEST_CHECK(isOrigin(violations[0].address, addressOrigin));
    AOT_TEST_CHECK(isOrigin(violations[0].suspension, suspensionOrigin));
    AOT_TEST_CHECK(findViolations(buildGraph, Suspends::No).isEmpty());
}

template<typename ValueBuilder>
static auto liveAcrossSuspension(const ValueBuilder& buildValue)
{
    return [buildValue](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* value = buildValue(proc, root, appendSlotBase(proc, root, otherOrigin));
        appendSuspension(proc, root, suspends);
        appendReturn(proc, root, value);
    };
}

static void testFrameAddressDeadAtSuspension()
{
    checkNoViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        appendStore(proc, root, appendSlotBase(proc, root, addressOrigin));
        appendSuspension(proc, root, suspends);
        appendReturn(proc, root);
    });
}

static void testFrameAddressRecomputed()
{
    checkNoViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        appendStore(proc, root, appendSlotBase(proc, root, addressOrigin));
        appendSuspension(proc, root, suspends);
        appendStore(proc, root, appendSlotBase(proc, root, secondAddressOrigin));
        appendReturn(proc, root);
    });
}

static void testFrameAddressEarlyArgument()
{
    for (B3::ValueRep constraint : { B3::ValueRep(B3::ValueRep::SomeRegister), B3::ValueRep(B3::ValueRep::SomeRegisterWithClobber), B3::ValueRep(B3::ValueRep::WarmAny), B3::ValueRep(B3::ValueRep::ColdAny), B3::ValueRep::reg(GPRInfo::regT0), B3::ValueRep::stackArgument(0) }) {
        checkNoViolation([&](B3::Procedure& proc, Suspends suspends) {
            B3::BasicBlock* root = proc.addBlock();
            B3::Value* base = appendSlotBase(proc, root, addressOrigin);
            appendSuspension(proc, root, suspends)->append(base, constraint);
            appendReturn(proc, root);
        });
    }
}

static void testFrameAddressSlotBase()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        appendSuspension(proc, root, suspends);
        appendStore(proc, root, base);
        appendReturn(proc, root);
    });
}

static void testFrameAddressSum()
{
    for (B3::Opcode opcode : { B3::Add, B3::Sub }) {
        for (unsigned addressChild : { 0u, 1u }) {
            checkOneViolation(liveAcrossSuspension([&](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
                B3::Value* offset = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 8);
                B3::Value* children[] = { offset, offset };
                children[addressChild] = base;
                return root->appendNew<B3::Value>(proc, opcode, originOf(addressOrigin), children[0], children[1]);
            }));
        }
    }
}

static void testFrameAddressIdentity()
{
    checkOneViolation(liveAcrossSuspension([](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
        return root->appendNew<B3::Value>(proc, B3::Identity, originOf(addressOrigin), base);
    }));
}

static void testFrameAddressFramePointer()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* framePointer = root->appendNew<B3::Value>(proc, B3::FramePointer, originOf(addressOrigin));
        appendSuspension(proc, root, suspends);
        appendReturn(proc, root, framePointer);
    });
}

static void testFrameAddressPatchpointWithSuspensionOrigin()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        appendSuspension(proc, root, suspends);
        appendStore(proc, root, base);
        appendSuspension(proc, root, suspends);
        appendReturn(proc, root);
    });

    Vector<FrameAddressViolation> violations = findViolations([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        appendSuspension(proc, root, suspends);
        appendSuspension(proc, root, suspends);
        appendStore(proc, root, base);
        appendReturn(proc, root);
    }, Suspends::Yes);
    AOT_TEST_CHECK(violations.size() == 2);
    for (const FrameAddressViolation& violation : violations) {
        AOT_TEST_CHECK(isOrigin(violation.address, addressOrigin));
        AOT_TEST_CHECK(isOrigin(violation.suspension, suspensionOrigin));
    }

    checkNoViolation([](B3::Procedure& proc, Suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        B3::Value* zero = root->appendNew<B3::Const64Value>(proc, originOf(suspensionOrigin), 0);
        root->appendNew<B3::MemoryValue>(proc, B3::Store, originOf(suspensionOrigin), zero, base);
        appendStore(proc, root, base);
        appendReturn(proc, root);
    });
}

static void testFrameAddressPhi()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::BasicBlock* left = proc.addBlock();
        B3::BasicBlock* right = proc.addBlock();
        B3::BasicBlock* join = proc.addBlock();
        B3::Value* phi = appendPhi(proc, join, addressOrigin);
        appendBranch(proc, root, left, right);
        appendUpsilon(proc, left, appendSlotBase(proc, left, otherOrigin), phi);
        appendJump(proc, left, join);
        appendUpsilon(proc, right, appendSlotBase(proc, right, otherOrigin), phi);
        appendJump(proc, right, join);
        appendSuspension(proc, join, suspends);
        appendReturn(proc, join, phi);
    });
}

static void testFrameAddressSelect()
{
    for (unsigned addressChild : { 1u, 2u }) {
        checkOneViolation(liveAcrossSuspension([&](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
            B3::Value* zero = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 0);
            B3::Value* children[] = { root->appendNew<B3::Const32Value>(proc, originOf(otherOrigin), 1), zero, zero };
            children[addressChild] = base;
            return root->appendNew<B3::Value>(proc, B3::Select, originOf(addressOrigin), children[0], children[1], children[2]);
        }));
    }
}

static void testFrameAddressLoop()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::BasicBlock* loop = proc.addBlock();
        B3::BasicBlock* afterLoop = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        appendJump(proc, root, loop);
        appendStore(proc, loop, base);
        appendSuspension(proc, loop, suspends);
        appendBranch(proc, loop, loop, afterLoop);
        appendReturn(proc, afterLoop);
    });
}

static void testFrameAddressUpsilonBeforeTerminalSuspension()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::BasicBlock* next = proc.addBlock();
        B3::Value* phi = appendPhi(proc, next, addressOrigin);
        appendUpsilon(proc, root, appendSlotBase(proc, root, otherOrigin), phi);
        appendSuspension(proc, root, suspends)->effects.terminal = true;
        root->setSuccessors(B3::FrequentedBlock(next));
        appendReturn(proc, next, phi);
    });
}

static void testFrameAddressPhiReadAfterUpsilon()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::BasicBlock* loop = proc.addBlock();
        B3::BasicBlock* afterLoop = proc.addBlock();
        B3::Value* phi = appendPhi(proc, loop, addressOrigin);
        appendUpsilon(proc, root, appendSlotBase(proc, root, otherOrigin), phi);
        appendJump(proc, root, loop);
        appendSuspension(proc, loop, suspends);
        appendUpsilon(proc, loop, appendSlotBase(proc, loop, otherOrigin), phi);
        appendBranch(proc, loop, loop, afterLoop);
        appendReturn(proc, afterLoop, phi);
    });
}

static void testFrameAddressDerivedInLaterRound()
{
    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::BasicBlock* join = proc.addBlock();
        B3::BasicBlock* predecessor = proc.addBlock();
        appendJump(proc, root, predecessor);
        B3::Value* phi = appendPhi(proc, join, otherOrigin);
        B3::Value* offset = join->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 8);
        B3::Value* sum = join->appendNew<B3::Value>(proc, B3::Add, originOf(addressOrigin), phi, offset);
        appendSuspension(proc, join, suspends);
        appendReturn(proc, join, sum);
        appendUpsilon(proc, predecessor, appendSlotBase(proc, predecessor, otherOrigin), phi);
        appendJump(proc, predecessor, join);
    });
}

static void testFrameAddressLateArgument()
{
    for (B3::ValueRep constraint : { B3::ValueRep(B3::ValueRep::LateColdAny), B3::ValueRep(B3::ValueRep::SomeLateRegister), B3::ValueRep::lateReg(GPRInfo::regT0) }) {
        checkOneViolation([&](B3::Procedure& proc, Suspends suspends) {
            B3::BasicBlock* root = proc.addBlock();
            B3::Value* base = appendSlotBase(proc, root, addressOrigin);
            appendSuspension(proc, root, suspends)->append(base, constraint);
            appendReturn(proc, root);
        });
    }

    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        B3::PatchpointValue* suspension = appendSuspension(proc, root, suspends);
        suspension->append(base, B3::ValueRep::LateColdAny);
        suspension->append(base, B3::ValueRep::LateColdAny);
        appendReturn(proc, root);
    });

    checkOneViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* base = appendSlotBase(proc, root, addressOrigin);
        appendSuspension(proc, root, suspends)->append(base, B3::ValueRep::LateColdAny);
        appendReturn(proc, root, base);
    });
}

static void testFrameAddressReportOrder()
{
    Vector<FrameAddressViolation> violations = findViolations([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* late = appendSlotBase(proc, root, otherOrigin);
        B3::Value* first = appendSlotBase(proc, root, addressOrigin);
        B3::Value* second = appendSlotBase(proc, root, secondAddressOrigin);
        appendSuspension(proc, root, suspends)->append(late, B3::ValueRep::LateColdAny);
        appendStore(proc, root, second);
        appendStore(proc, root, first);
        appendReturn(proc, root);
    }, Suspends::Yes);
    AOT_TEST_CHECK(violations.size() == 3);
    AOT_TEST_CHECK(isOrigin(violations[0].address, addressOrigin));
    AOT_TEST_CHECK(isOrigin(violations[1].address, secondAddressOrigin));
    AOT_TEST_CHECK(isOrigin(violations[2].address, otherOrigin));
}

static void testFrameAddressPatchpointWithOtherOrigin()
{
    for (B3::Origin origin : { originOf(otherOrigin), originOf(addressOrigin), originOf(suspensionOrigin + 64), B3::Origin() }) {
        checkNoViolation([&](B3::Procedure& proc, Suspends) {
            B3::BasicBlock* root = proc.addBlock();
            B3::Value* base = appendSlotBase(proc, root, addressOrigin);
            root->appendNew<B3::PatchpointValue>(proc, B3::Void, origin);
            appendStore(proc, root, base);
            appendReturn(proc, root);
        });
    }
}

static void testFrameAddressPatchpointResult()
{
    checkNoViolation([](B3::Procedure& proc, Suspends suspends) {
        B3::BasicBlock* root = proc.addBlock();
        B3::PatchpointValue* result = root->appendNew<B3::PatchpointValue>(proc, B3::pointerType(), originOf(addressOrigin));
        appendSuspension(proc, root, suspends);
        appendReturn(proc, root, result);
    });
}

static void testFrameAddressLoadedValue()
{
    checkNoViolation(liveAcrossSuspension([](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
        return root->appendNew<B3::MemoryValue>(proc, B3::Load, B3::pointerType(), originOf(addressOrigin), base);
    }));
}

static void testFrameAddressComparedValue()
{
    checkNoViolation(liveAcrossSuspension([](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
        B3::Value* zero = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 0);
        return root->appendNew<B3::Value>(proc, B3::Equal, originOf(addressOrigin), base, zero);
    }));
}

static void testFrameAddressShiftedValue()
{
    checkNoViolation(liveAcrossSuspension([](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
        B3::Value* shiftAmount = root->appendNew<B3::Const32Value>(proc, originOf(otherOrigin), 3);
        return root->appendNew<B3::Value>(proc, B3::ZShr, originOf(addressOrigin), base, shiftAmount);
    }));
}

static void testFrameAddressSelectPredicate()
{
    checkNoViolation(liveAcrossSuspension([](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
        B3::Value* zero = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 0);
        B3::Value* one = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 1);
        return root->appendNew<B3::Value>(proc, B3::Select, originOf(addressOrigin), base, zero, one);
    }));
}

static void testBackEndReportFailure()
{
    struct FailureCase {
        B3::FailureKind failure;
        InternalErrorKind error;
    };
    static constexpr FailureCase cases[] = {
        { B3::FailureKind::AbsoluteAddress, InternalErrorKind::AbsoluteAddressInProcedure },
        { B3::FailureKind::RelocationNotLowered, InternalErrorKind::RelocationNotLowered },
        { B3::FailureKind::VariableReadBeforeSet, InternalErrorKind::VariableReadBeforeSet },
    };

    for (const FailureCase& failureCase : cases) {
        B3::CompilationReport report;
        report.failure = B3::Failure { failureCase.failure, originOf(addressOrigin) };
        BackEndReport result = collectBackEndReport(report, { }, true);
        AOT_TEST_CHECK(result.findings.size() == 1);
        AOT_TEST_CHECK(result.findings[0].kind == failureCase.error);
        AOT_TEST_CHECK(isOrigin(result.findings[0].origin, addressOrigin));
        AOT_TEST_CHECK(result.declines.isEmpty());
        AOT_TEST_CHECK(result.limitHits.isEmpty());
    }

    BackEndReport resultWithoutFailure = collectBackEndReport(B3::CompilationReport { }, { }, true);
    AOT_TEST_CHECK(resultWithoutFailure.findings.isEmpty());
    AOT_TEST_CHECK(resultWithoutFailure.declines.isEmpty());
    AOT_TEST_CHECK(resultWithoutFailure.limitHits.isEmpty());
}

static void testBackEndReportProcedure()
{
    B3::Procedure proc;
    AOT_TEST_CHECK(collectBackEndReport(proc.report(), { }, false).findings.isEmpty());

    proc.fail(B3::FailureKind::RelocationNotLowered, originOf(addressOrigin));
    BackEndReport result = collectBackEndReport(proc.report(), { }, false);
    AOT_TEST_CHECK(result.findings.size() == 1);
    AOT_TEST_CHECK(result.findings[0].kind == InternalErrorKind::RelocationNotLowered);
    AOT_TEST_CHECK(isOrigin(result.findings[0].origin, addressOrigin));
}

static void testBackEndReportViolations()
{
    B3::CompilationReport report;
    report.failure = B3::Failure { B3::FailureKind::VariableReadBeforeSet, originOf(5) };
    Vector<FrameAddressViolation> violations {
        FrameAddressViolation { originOf(6), originOf(suspensionOrigin) },
        FrameAddressViolation { originOf(7), originOf(suspensionOrigin) },
    };

    BackEndReport result = collectBackEndReport(report, violations.span(), false);
    AOT_TEST_CHECK(result.findings.size() == 3);
    AOT_TEST_CHECK(result.findings[0].kind == InternalErrorKind::VariableReadBeforeSet);
    AOT_TEST_CHECK(isOrigin(result.findings[0].origin, 5));
    AOT_TEST_CHECK(result.findings[1].kind == InternalErrorKind::FrameAddressAcrossSuspension);
    AOT_TEST_CHECK(isOrigin(result.findings[1].origin, 6));
    AOT_TEST_CHECK(result.findings[2].kind == InternalErrorKind::FrameAddressAcrossSuspension);
    AOT_TEST_CHECK(isOrigin(result.findings[2].origin, 7));
    AOT_TEST_CHECK(result.declines.isEmpty());
}

static B3::CompilationReport reportWithRemarks()
{
    B3::CompilationReport report;
    report.remarks.append(B3::Remark { B3::RemarkKind::LoopNotVersioned, B3::GaveUpReason::LimitReached, originOf(5), B3::Limit::MaxClonedValuesPerLoopVersion });
    report.remarks.append(B3::Remark { B3::RemarkKind::LoopNotVersioned, B3::GaveUpReason::NotInnermost, originOf(6), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::LoopVersioned, B3::GaveUpReason::None, originOf(7), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::LoopNotVersioned, B3::GaveUpReason::SeveralEntries, originOf(8), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::PollNotStripMined, B3::GaveUpReason::NotCounted, originOf(9), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::LoopRotated, B3::GaveUpReason::None, originOf(10), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::PollStripMined, B3::GaveUpReason::None, originOf(11), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::RangeChecksRemoved, B3::GaveUpReason::None, originOf(12), std::nullopt });
    report.remarks.append(B3::Remark { B3::RemarkKind::LoopBoundNotClamped, B3::GaveUpReason::BoundNotInvariant, originOf(13), std::nullopt });
    return report;
}

static void checkDecline(const BackEndDecline& decline, Wire::DeclineKind kind, LimitID limit, Wire::LoopDeclineReason reason, uint32_t origin)
{
    AOT_TEST_CHECK(decline.kind == kind);
    AOT_TEST_CHECK(decline.limit == limit);
    AOT_TEST_CHECK(decline.reason == reason);
    AOT_TEST_CHECK(isOrigin(decline.origin, origin));
}

static void testBackEndReportDeclines()
{
    B3::CompilationReport report = reportWithRemarks();

    BackEndReport result = collectBackEndReport(report, { }, true);
    AOT_TEST_CHECK(result.findings.isEmpty());
    AOT_TEST_CHECK(result.declines.size() == 4);
    checkDecline(result.declines[0], Wire::DeclineKind::LoopNotVersioned, LimitID::MaxClonedValuesPerLoopVersion, Wire::LoopDeclineReason::LimitReached, 5);
    checkDecline(result.declines[1], Wire::DeclineKind::LoopNotVersioned, LimitID::None, Wire::LoopDeclineReason::SeveralEntries, 8);
    checkDecline(result.declines[2], Wire::DeclineKind::LoopPollNotStripMined, LimitID::None, Wire::LoopDeclineReason::NotCounted, 9);
    checkDecline(result.declines[3], Wire::DeclineKind::LoopBoundNotClamped, LimitID::None, Wire::LoopDeclineReason::BoundNotInvariant, 13);

    AOT_TEST_CHECK(collectBackEndReport(report, { }, false).declines.isEmpty());
}

static void testBackEndReportRecordsDeclines()
{
    ReportedProgram program;
    Wire::CompileOptions options = makeDefaultCompileOptions();
    AOT_TEST_CHECK(!applyOptionWord(options, "report=declines"_s));
    BuildContext build(options);

    AOT_TEST_CHECK(!program.report(build, program.functionAsRead(), reportWithRemarks()));
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());

    Vector<Wire::DeclineRecord> declines = build.takeDeclines(&program.program());
    AOT_TEST_CHECK(declines.size() == 4);
    constexpr uint32_t values[] = { 5, 8, 9, 13 };
    constexpr Wire::DeclineKind kinds[] = { Wire::DeclineKind::LoopNotVersioned, Wire::DeclineKind::LoopNotVersioned, Wire::DeclineKind::LoopPollNotStripMined, Wire::DeclineKind::LoopBoundNotClamped };
    constexpr Wire::LoopDeclineReason reasons[] = { Wire::LoopDeclineReason::LimitReached, Wire::LoopDeclineReason::SeveralEntries, Wire::LoopDeclineReason::NotCounted, Wire::LoopDeclineReason::BoundNotInvariant };
    for (size_t index = 0; index < declines.size(); ++index) {
        AOT_TEST_CHECK(declines[index].kind == kinds[index]);
        AOT_TEST_CHECK(decodeDeclineReason(declines[index].kind, declines[index].reason) == DeclineReason { reasons[index] });
        AOT_TEST_CHECK(!declines[index].count);
        AOT_TEST_CHECK(declines[index].limit == (index ? LimitID::None : LimitID::MaxClonedValuesPerLoopVersion));
        AOT_TEST_CHECK(declines[index].witness == (Wire::Witness { program.functionAsRead().index(), values[index], ReportedProgram::locationOf(values[index]) }));
    }

    BuildContext buildWithoutDeclines(makeDefaultCompileOptions());
    AOT_TEST_CHECK(!program.report(buildWithoutDeclines, program.functionAsRead(), reportWithRemarks()));
    AOT_TEST_CHECK(buildWithoutDeclines.takeDeclines(&program.program()).isEmpty());
}

static void testBackEndReportRecordsFindings()
{
    ReportedProgram program;
    B3::CompilationReport raw;
    raw.failure = B3::Failure { B3::FailureKind::RelocationNotLowered, originOf(5) };
    Vector<FrameAddressViolation> violations { FrameAddressViolation { originOf(6), originOf(suspensionOrigin) } };

    for (Typed::FunctionID function : { program.functionAsRead(), program.derivedFunction() }) {
        BuildContext build(makeDefaultCompileOptions());
        AOT_TEST_CHECK(program.report(build, function, raw, violations.span()));
        AOT_TEST_CHECK(build.diagnostics().hasErrors());

        build.diagnostics().finish(&program.program());
        const Vector<InternalError>& errors = build.diagnostics().internalErrors();
        AOT_TEST_CHECK(errors.size() == 2);
        AOT_TEST_CHECK(errors[0].kind == InternalErrorKind::RelocationNotLowered);
        AOT_TEST_CHECK(errors[1].kind == InternalErrorKind::FrameAddressAcrossSuspension);
        for (size_t index = 0; index < errors.size(); ++index) {
            uint32_t value = 5 + index;
            AOT_TEST_CHECK(errors[index].function == program.functionAsRead());
            AOT_TEST_CHECK(errors[index].value == (function == program.functionAsRead() ? Typed::ValueID(value) : Typed::ValueID()));
            AOT_TEST_CHECK(errors[index].location == ReportedProgram::locationOf(value));
        }
        AOT_TEST_CHECK(build.diagnostics().userDiagnostics().isEmpty());
    }
}

static void testBackEndReportFindingWithoutValue()
{
    ReportedProgram program;
    for (B3::Origin origin : { B3::Origin(), originOf(ReportedProgram::numberOfValues) }) {
        B3::CompilationReport raw;
        raw.failure = B3::Failure { B3::FailureKind::AbsoluteAddress, origin };
        BuildContext build(makeDefaultCompileOptions());
        AOT_TEST_CHECK(program.report(build, program.functionAsRead(), raw));

        build.diagnostics().finish(nullptr);
        const Vector<InternalError>& errors = build.diagnostics().internalErrors();
        AOT_TEST_CHECK(errors.size() == 1);
        AOT_TEST_CHECK(errors[0].kind == InternalErrorKind::AbsoluteAddressInProcedure);
        AOT_TEST_CHECK(errors[0].function == program.functionAsRead());
        AOT_TEST_CHECK(!errors[0].value);
        AOT_TEST_CHECK(errors[0].location == noSourceLocation);
    }
}

static void testBackEndReportStatistics()
{
    ReportedProgram program;
    BuildContext build(makeDefaultCompileOptions());
    Statistics expected;

    AOT_TEST_CHECK(!program.report(build, program.functionAsRead(), B3::CompilationReport { }));
    AOT_TEST_CHECK(build.statistics().values() == expected.values());

    B3::CompilationReport raw;
    raw.counters[static_cast<size_t>(B3::Counter::LoopsVersioned)] = 4;
    raw.counters[static_cast<size_t>(B3::Counter::LoopsRotated)] = 5;
    raw.counters[static_cast<size_t>(B3::Counter::PollsStripMined)] = 6;
    raw.counters[static_cast<size_t>(B3::Counter::RangeChecksRemoved)] = 7;
    raw.counters[static_cast<size_t>(B3::Counter::LoopBoundsClamped)] = 8;
    raw.counters[static_cast<size_t>(B3::Counter::KnownInductionsUsed)] = 9;
    raw.counters[static_cast<size_t>(B3::Counter::TrapsFoldedIntoAccesses)] = 10;
    AOT_TEST_CHECK(!program.report(build, program.functionAsRead(), raw));

    expected.count(StatisticID::LoopsVersioned, 4);
    expected.count(StatisticID::LoopsRotated, 5);
    expected.count(StatisticID::LoopPollsStripMined, 6);
    expected.count(StatisticID::RangeChecksRemoved, 7);
    expected.count(StatisticID::LoopBoundsClamped, 8);
    expected.count(StatisticID::KnownInductionsUsed, 9);
    expected.count(StatisticID::NullChecksFoldedIntoAccesses, 10);
    AOT_TEST_CHECK(build.statistics().values() == expected.values());
    AOT_TEST_CHECK(build.takeLosses(&program.program()).isEmpty());
}

static void testLimitNamesAgree()
{
    for (unsigned i = 0; i < B3::numberOfLimits; ++i) {
        auto limit = static_cast<B3::Limit>(i);
        AOT_TEST_CHECK(limitIDOf(limit) != LimitID::None);
        AOT_TEST_CHECK(nameOf(limitIDOf(limit)) == B3::nameOf(limit));
        AOT_TEST_CHECK(stageOf(limitIDOf(limit)) == Stage::GenerateCode);
        AOT_TEST_CHECK(canBeLowered(limitIDOf(limit)));
        for (unsigned j = 0; j < i; ++j)
            AOT_TEST_CHECK(limitIDOf(limit) != limitIDOf(static_cast<B3::Limit>(j)));
    }
}

static void testLimitHitsAreCollected()
{
    B3::CompilationReport report;
    report.limitHits[static_cast<size_t>(B3::Limit::MaxHoistedLoadsPerLoop)] = { 3, originOf(5) };
    report.limitHits[static_cast<size_t>(B3::Limit::MaxThreadedPathCost)] = { 1, originOf(6) };
    static_assert(B3::Limit::MaxThreadedPathCost < B3::Limit::MaxHoistedLoadsPerLoop);

    for (bool collectsDeclines : { false, true }) {
        BackEndReport result = collectBackEndReport(report, { }, collectsDeclines);
        AOT_TEST_CHECK(result.findings.isEmpty() && result.declines.isEmpty());
        AOT_TEST_CHECK(result.limitHits.size() == 2);
        AOT_TEST_CHECK(result.limitHits[0].limit == LimitID::MaxThreadedPathCost);
        AOT_TEST_CHECK(result.limitHits[0].count == 1);
        AOT_TEST_CHECK(isOrigin(result.limitHits[0].firstOrigin, 6));
        AOT_TEST_CHECK(result.limitHits[1].limit == LimitID::MaxHoistedLoadsPerLoop);
        AOT_TEST_CHECK(result.limitHits[1].count == 3);
        AOT_TEST_CHECK(isOrigin(result.limitHits[1].firstOrigin, 5));
    }
}

static void testLimitHitBecomesLoss()
{
    ReportedProgram program;
    B3::CompilationReport raw;
    raw.limitHits[static_cast<size_t>(B3::Limit::MaxHoistedLoadsPerLoop)] = { 3, originOf(5) };

    for (Typed::FunctionID function : { program.functionAsRead(), program.derivedFunction() }) {
        BuildContext build(makeDefaultCompileOptions());
        AOT_TEST_CHECK(!program.report(build, function, raw));
        AOT_TEST_CHECK(!build.diagnostics().hasErrors());

        Statistics expected;
        expected.count(hitsOf(LimitID::MaxHoistedLoadsPerLoop), 3);
        AOT_TEST_CHECK(build.statistics().values() == expected.values());

        Vector<Wire::LossRecord> losses = build.takeLosses(&program.program());
        AOT_TEST_CHECK(losses.size() == 1);
        AOT_TEST_CHECK(losses[0].limit == LimitID::MaxHoistedLoadsPerLoop);
        AOT_TEST_CHECK(losses[0].stage == stageOf(LimitID::MaxHoistedLoadsPerLoop));
        AOT_TEST_CHECK(losses[0].layout == Wire::none);
        AOT_TEST_CHECK(losses[0].count == 3);
        AOT_TEST_CHECK(losses[0].witness.function == program.functionAsRead().index());
        AOT_TEST_CHECK(losses[0].witness.value == (function == program.functionAsRead() ? 5 : Wire::none));
        AOT_TEST_CHECK(losses[0].witness.location == ReportedProgram::locationOf(5));
    }
}

std::span<const NativeTest> backEndReportTests()
{
    static constexpr NativeTest tests[] = {
        { "frame-address-dead-at-suspension"_s, testFrameAddressDeadAtSuspension },
        { "frame-address-recomputed"_s, testFrameAddressRecomputed },
        { "frame-address-early-argument"_s, testFrameAddressEarlyArgument },
        { "frame-address-slot-base"_s, testFrameAddressSlotBase },
        { "frame-address-sum"_s, testFrameAddressSum },
        { "frame-address-identity"_s, testFrameAddressIdentity },
        { "frame-address-frame-pointer"_s, testFrameAddressFramePointer },
        { "frame-address-patchpoint-with-suspension-origin"_s, testFrameAddressPatchpointWithSuspensionOrigin },
        { "frame-address-phi"_s, testFrameAddressPhi },
        { "frame-address-select"_s, testFrameAddressSelect },
        { "frame-address-loop"_s, testFrameAddressLoop },
        { "frame-address-upsilon-before-terminal-suspension"_s, testFrameAddressUpsilonBeforeTerminalSuspension },
        { "frame-address-phi-read-after-upsilon"_s, testFrameAddressPhiReadAfterUpsilon },
        { "frame-address-derived-in-later-round"_s, testFrameAddressDerivedInLaterRound },
        { "frame-address-late-argument"_s, testFrameAddressLateArgument },
        { "frame-address-report-order"_s, testFrameAddressReportOrder },
        { "frame-address-patchpoint-with-other-origin"_s, testFrameAddressPatchpointWithOtherOrigin },
        { "frame-address-patchpoint-result"_s, testFrameAddressPatchpointResult },
        { "frame-address-loaded-value"_s, testFrameAddressLoadedValue },
        { "frame-address-compared-value"_s, testFrameAddressComparedValue },
        { "frame-address-shifted-value"_s, testFrameAddressShiftedValue },
        { "frame-address-select-predicate"_s, testFrameAddressSelectPredicate },
        { "back-end-report-failure"_s, testBackEndReportFailure },
        { "back-end-report-procedure"_s, testBackEndReportProcedure },
        { "back-end-report-violations"_s, testBackEndReportViolations },
        { "back-end-report-declines"_s, testBackEndReportDeclines },
        { "back-end-report-records-declines"_s, testBackEndReportRecordsDeclines },
        { "back-end-report-records-findings"_s, testBackEndReportRecordsFindings },
        { "back-end-report-finding-without-value"_s, testBackEndReportFindingWithoutValue },
        { "back-end-report-statistics"_s, testBackEndReportStatistics },
        { "back-end-report-limit-names-agree"_s, testLimitNamesAgree },
        { "back-end-report-limit-hits-are-collected"_s, testLimitHitsAreCollected },
        { "back-end-report-limit-hit-becomes-loss"_s, testLimitHitBecomesLoss },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
