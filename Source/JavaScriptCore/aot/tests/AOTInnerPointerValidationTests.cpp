/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTInnerPointerValidation.h"
#include "AirCode.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3Generate.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3StackmapGenerationParams.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "LinkBuffer.h"

namespace JSC { namespace AOT {

namespace {

constexpr uint32_t pointerOrigin = 1;
constexpr uint32_t safepointOrigin = 2;
constexpr uint32_t otherOrigin = 3;

constexpr B3::HeapRange collectorState(1, 2);
constexpr B3::HeapRange weakTableStorage(2, 3);
constexpr B3::HeapRange fieldOfPointer(10, 11);
constexpr B3::HeapRange fieldOfSecondPointer(11, 12);
constexpr B3::HeapRange permanentCells(12, 13);
constexpr B3::HeapRange fieldOfCell(13, 14);
constexpr B3::HeapRange fieldOfPointerTheScanKnows(14, 15);
constexpr B3::HeapRange fieldOutsideCells(15, 16);
constexpr B3::HeapRange typeOfCell(16, 17);
constexpr B3::HeapRange contents(20, 21);
constexpr B3::HeapRange contentsWithoutCells(21, 22);

enum class Keeps : uint8_t {
    Nothing,
    OwnerBehindRead,
    OwnerInFrontOfSafepoint,
    OwnerAsLateUseOfSafepoint,
    OtherCellBehindRead,
};

// How many registers a safepoint leaves alone: with none of them every value that outlives it lies in the frame.
constexpr unsigned numbersOfPreservedRegisters[] = { 0, 1, 4 };

struct Graph {
    Graph(B3::Procedure& proc, unsigned numberOfPreservedRegisters)
        : proc(proc)
        , numberOfPreservedRegisters(numberOfPreservedRegisters)
    {
    }

    static B3::Origin originOf(uint32_t number) { return Typed::b3OriginOf(Typed::ValueID(number)); }

    B3::Value* argument(B3::BasicBlock* block, unsigned index)
    {
        return block->appendNew<B3::ArgumentRegValue>(proc, originOf(otherOrigin), GPRInfo::toArgumentRegister(index));
    }

    B3::Value* load(B3::BasicBlock* block, B3::Kind opcode, B3::Type type, B3::Value* base, int32_t offset, B3::HeapRange range, uint32_t origin = otherOrigin)
    {
        auto* result = block->appendNew<B3::MemoryValue>(proc, opcode, type, originOf(origin), base, offset);
        result->setRange(range);
        return result;
    }

    B3::Value* loadPointer(B3::BasicBlock* block, B3::Value* owner, B3::HeapRange field = fieldOfPointer)
    {
        return load(block, B3::Load, B3::Int64, owner, 16, field, pointerOrigin);
    }

    // The next access to the cell takes over the trap, and the origin with it.
    void checkCell(B3::BasicBlock* block, B3::Value* cell)
    {
        load(block, B3::trapping(B3::Load8Z), B3::Int32, cell, 5, typeOfCell, pointerOrigin);
    }

    B3::Value* readThrough(B3::BasicBlock* block, B3::Value* pointer)
    {
        return load(block, B3::Load8Z, B3::Int32, pointer, 0, contents);
    }

    B3::PatchpointValue* emptyPatchpoint(B3::BasicBlock* block, uint32_t origin)
    {
        auto* result = block->appendNew<B3::PatchpointValue>(proc, B3::Void, originOf(origin));
        result->effects = B3::Effects::none();
        result->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
        return result;
    }

    B3::PatchpointValue* safepoint(B3::BasicBlock* block)
    {
        B3::PatchpointValue* result = emptyPatchpoint(block, safepointOrigin);
        result->effects.writes = B3::HeapRange(collectorState.begin(), weakTableStorage.end());
        const auto& registers = proc.code().regsInPriorityOrder(B3::GP);
        RegisterSet clobbered;
        for (unsigned i = 0; i + numberOfPreservedRegisters < registers.size(); ++i)
            clobbered.add(registers[i], IgnoreVectors);
        result->clobberLate(clobbered);
        return result;
    }

    void keepAlive(B3::BasicBlock* block, B3::Value* cell)
    {
        B3::PatchpointValue* result = emptyPatchpoint(block, otherOrigin);
        result->effects.writesLocalState = true;
        result->effects.reads = B3::HeapRange::top();
        result->append(cell, B3::ValueRep::ColdAny);
    }

    void safepoint(B3::BasicBlock* block, Keeps keeps, B3::Value* owner)
    {
        if (keeps == Keeps::OwnerInFrontOfSafepoint)
            keepAlive(block, owner);
        B3::PatchpointValue* result = safepoint(block);
        if (keeps == Keeps::OwnerAsLateUseOfSafepoint)
            result->append(owner, B3::ValueRep::LateColdAny);
    }

    void finish(B3::BasicBlock* block, Keeps keeps, B3::Value* owner, B3::Value* other, B3::Value* result)
    {
        if (keeps == Keeps::OwnerBehindRead)
            keepAlive(block, owner);
        if (keeps == Keeps::OtherCellBehindRead)
            keepAlive(block, other);
        block->appendNew<B3::Value>(proc, B3::Return, originOf(otherOrigin), result);
    }

    B3::Procedure& proc;
    unsigned numberOfPreservedRegisters;
};

InnerPointerRules rules()
{
    InnerPointerRules result;
    result.fieldsThatHoldPointers.append(fieldOfPointer);
    result.fieldsThatHoldPointers.append(fieldOfSecondPointer);
    result.fieldsThatHoldPointers.append(fieldOfPointerTheScanKnows);
    result.fieldsThatHoldPointers.append(fieldOutsideCells);
    result.fieldsOutsideCells.append(fieldOutsideCells);
    result.fieldsThatHoldPointersTheScanKnows.append(fieldOfPointerTheScanKnows);
    result.contentsWithoutCells.append(contentsWithoutCells);
    result.permanentCells.append(permanentCells);
    result.collectorState = collectorState;
    result.weakTableStorage = weakTableStorage;
    return result;
}

unsigned numberOfAccessesThatTookTraps(B3::Air::Code& code)
{
    unsigned result = 0;
    for (B3::Air::BasicBlock* block : code) {
        for (B3::Air::Inst& inst : *block) {
            if (!inst.kind.effects || !inst.origin->traps())
                continue;
            for (B3::Air::Arg& arg : inst.args())
                result += arg.isMemory() && arg.offset() != inst.origin->as<B3::MemoryValue>()->offset();
        }
    }
    return result;
}

enum class OriginsOfAccessesThatTakeTraps : uint8_t { Lost, Kept };

template<typename Functor>
InnerPointerFindings check(unsigned numberOfPreservedRegisters, const Functor& build, bool loadsStayInFront = true, unsigned numberOfChecksOfCells = 0, OriginsOfAccessesThatTakeTraps origins = OriginsOfAccessesThatTakeTraps::Kept)
{
    B3::Policy policy;
    policy.allocation.keepsValuesAfterLowering = true;
    policy.air.peepholes.trapReach = 4096;
    policy.validation.keepsOriginsOfAccessesThatTakeTraps = origins == OriginsOfAccessesThatTakeTraps::Kept;
    B3::Procedure proc(policy);
    if (loadsStayInFront)
        proc.setHeapThatLoadsStayInFrontOf(weakTableStorage);
    Graph graph(proc, numberOfPreservedRegisters);
    build(graph);
    B3::prepareForGeneration(proc);
    AOT_TEST_CHECK(!proc.report().failure);
    AOT_TEST_CHECK(numberOfAccessesThatTookTraps(proc.code()) == numberOfChecksOfCells);
    InnerPointerFindings findings;
    findInnerPointerViolations(proc.code(), rules(), findings);
    for (const InnerPointerViolation& violation : findings.violations) {
        AOT_TEST_CHECK(violation.pointer.dfgOrigin() == Graph::originOf(pointerOrigin).dfgOrigin());
        AOT_TEST_CHECK(violation.safepoint.dfgOrigin() == Graph::originOf(safepointOrigin).dfgOrigin());
    }
    return findings;
}

bool holdsOwner(Keeps keeps)
{
    return keeps == Keeps::OwnerBehindRead || keeps == Keeps::OwnerAsLateUseOfSafepoint;
}

constexpr Keeps allKeeps[] = { Keeps::Nothing, Keeps::OwnerBehindRead, Keeps::OwnerInFrontOfSafepoint, Keeps::OwnerAsLateUseOfSafepoint, Keeps::OtherCellBehindRead };

template<typename Functor>
void checkEveryForm(const Functor& build, Keeps alsoHoldsOwner = Keeps::OwnerBehindRead, unsigned numberOfChecksOfCells = 0, OriginsOfAccessesThatTakeTraps origins = OriginsOfAccessesThatTakeTraps::Kept)
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (Keeps keeps : allKeeps) {
            InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) { build(graph, keeps); }, true, numberOfChecksOfCells, origins);
            AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
            AOT_TEST_CHECK(findings.violations.isEmpty() == (holdsOwner(keeps) || keeps == alsoHoldsOwner));
        }
    }
}

} // anonymous namespace

static void testPointerReadBehindSafepoint()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* pointer = graph.loadPointer(root, owner);
        graph.safepoint(root, keeps, owner);
        graph.finish(root, keeps, owner, other, graph.readThrough(root, pointer));
    });
}

static void testPointerNotReadBehindSafepoint()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        InnerPointerFindings findings = check(numberOfPreservedRegisters, [](Graph& graph) {
            B3::BasicBlock* root = graph.proc.addBlock();
            B3::Value* owner = graph.argument(root, 0);
            B3::Value* byte = graph.readThrough(root, graph.loadPointer(root, owner));
            graph.safepoint(root);
            graph.finish(root, Keeps::Nothing, owner, owner, byte);
        });
        AOT_TEST_CHECK(!findings.numberOfPointersAcrossSafepoints);
        AOT_TEST_CHECK(findings.violations.isEmpty());
    }
}

static void testAddressComputedInFrontOfSafepoint()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* address = root->appendNew<B3::Value>(graph.proc, B3::Add, Graph::originOf(otherOrigin), graph.loadPointer(root, owner), graph.argument(root, 2));
        B3::PatchpointValue* opaque = graph.emptyPatchpoint(root, otherOrigin);
        opaque->effects.writesLocalState = true;
        opaque->append(address, B3::ValueRep::SomeRegister);
        graph.safepoint(root, keeps, owner);
        graph.finish(root, keeps, owner, other, graph.readThrough(root, address));
    });
}

static void testPointerLoadedThroughPointer()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* pointer = graph.loadPointer(root, graph.loadPointer(root, owner), fieldOfSecondPointer);
        graph.safepoint(root, keeps, owner);
        graph.finish(root, keeps, owner, other, graph.readThrough(root, pointer));
    });
}

static void testPointerWhoseLoadTookTrap()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        graph.checkCell(root, owner);
        B3::Value* pointer = graph.loadPointer(root, owner);
        graph.safepoint(root, keeps, owner);
        graph.finish(root, keeps, owner, other, graph.readThrough(root, pointer));
    }, Keeps::OwnerBehindRead, 1);
}

static void testPointerLoadedThroughPointerWhoseLoadTookTrap()
{
    for (OriginsOfAccessesThatTakeTraps origins : { OriginsOfAccessesThatTakeTraps::Lost, OriginsOfAccessesThatTakeTraps::Kept }) {
        checkEveryForm([](Graph& graph, Keeps keeps) {
            B3::BasicBlock* root = graph.proc.addBlock();
            B3::Value* owner = graph.argument(root, 0);
            B3::Value* other = graph.argument(root, 1);
            graph.checkCell(root, owner);
            B3::Value* pointer = graph.loadPointer(root, graph.loadPointer(root, owner), fieldOutsideCells);
            graph.safepoint(root, keeps, owner);
            graph.finish(root, keeps, owner, other, graph.readThrough(root, pointer));
        }, Keeps::OwnerBehindRead, 1, origins);
    }
}

// The cell that an owner was loaded from is no owner.
static void testOwnerWhoseLoadTookTrap()
{
    for (OriginsOfAccessesThatTakeTraps origins : { OriginsOfAccessesThatTakeTraps::Lost, OriginsOfAccessesThatTakeTraps::Kept }) {
        checkEveryForm([](Graph& graph, Keeps keeps) {
            B3::BasicBlock* root = graph.proc.addBlock();
            B3::Value* holder = graph.argument(root, 0);
            graph.checkCell(root, holder);
            B3::Value* owner = graph.load(root, B3::Load, B3::Int64, holder, 8, fieldOfCell);
            B3::Value* pointer = graph.loadPointer(root, owner);
            graph.safepoint(root, keeps, owner);
            graph.finish(root, keeps, owner, holder, graph.readThrough(root, pointer));
        }, Keeps::OwnerBehindRead, 1, origins);
    }
}

static void testPointerOnlyCompared()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* compared = graph.argument(root, 2);
        B3::Value* pointer = graph.loadPointer(root, owner);
        graph.safepoint(root, keeps, owner);
        graph.finish(root, keeps, owner, other, root->appendNew<B3::Value>(graph.proc, B3::Equal, Graph::originOf(otherOrigin), pointer, compared));
    });
}

static void testPointerHandedToSafepoint()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (bool handsOwner : { false, true }) {
            InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                B3::BasicBlock* root = graph.proc.addBlock();
                B3::Value* owner = graph.argument(root, 0);
                B3::Value* pointer = graph.loadPointer(root, owner);
                B3::PatchpointValue* safepoint = graph.safepoint(root);
                safepoint->append(pointer, B3::ValueRep::SomeRegister);
                if (handsOwner)
                    safepoint->append(owner, B3::ValueRep::SomeRegister);
                root->appendNew<B3::Value>(graph.proc, B3::Return, Graph::originOf(otherOrigin), root->appendNew<B3::Const32Value>(graph.proc, Graph::originOf(otherOrigin), 0));
            });
            AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
            AOT_TEST_CHECK(findings.violations.isEmpty() == handsOwner);
        }
    }
}

// The safepoint is handed the pointer and its owner, and what names no cell is read through the pointer behind it.
static void testPointerHandedToSafepointAndReadBehindIt()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (B3::HeapRange field : { fieldOfPointer, fieldOfPointerTheScanKnows }) {
            for (bool handsOwner : { false, true }) {
                InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                    B3::BasicBlock* root = graph.proc.addBlock();
                    B3::Value* owner = graph.argument(root, 0);
                    B3::Value* pointer = graph.loadPointer(root, owner, field);
                    B3::PatchpointValue* safepoint = graph.safepoint(root);
                    safepoint->append(pointer, B3::ValueRep::SomeRegister);
                    if (handsOwner)
                        safepoint->append(owner, B3::ValueRep::SomeRegister);
                    root->appendNew<B3::Value>(graph.proc, B3::Return, Graph::originOf(otherOrigin), graph.load(root, B3::Load, B3::Int64, pointer, 0, contentsWithoutCells));
                });
                AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
                AOT_TEST_CHECK(findings.violations.isEmpty() == (handsOwner && field == fieldOfPointerTheScanKnows));
            }
        }
    }
}

static void testSafepointInOtherBlock()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::Origin origin = Graph::originOf(otherOrigin);
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::BasicBlock* collects = graph.proc.addBlock();
        B3::BasicBlock* reads = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* pointer = graph.loadPointer(root, owner);
        root->appendNew<B3::Value>(graph.proc, B3::Branch, origin, graph.argument(root, 2));
        root->setSuccessors(B3::FrequentedBlock(collects), B3::FrequentedBlock(reads));
        graph.safepoint(collects, keeps, owner);
        collects->appendNew<B3::Value>(graph.proc, B3::Jump, origin);
        collects->setSuccessors(B3::FrequentedBlock(reads));
        graph.finish(reads, keeps, owner, other, graph.readThrough(reads, pointer));
    });
}

static void testSafepointInLoop()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::Origin origin = Graph::originOf(otherOrigin);
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::BasicBlock* loop = graph.proc.addBlock();
        B3::BasicBlock* done = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* limit = graph.argument(root, 2);
        B3::Value* pointer = graph.loadPointer(root, owner);
        auto* first = root->appendNew<B3::UpsilonValue>(graph.proc, origin, root->appendNew<B3::Const64Value>(graph.proc, origin, 0));
        root->appendNew<B3::Value>(graph.proc, B3::Jump, origin);
        root->setSuccessors(B3::FrequentedBlock(loop));

        B3::Value* index = loop->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
        first->setPhi(index);
        graph.safepoint(loop, keeps, owner);
        B3::Value* byte = graph.readThrough(loop, loop->appendNew<B3::Value>(graph.proc, B3::Add, origin, pointer, index));
        B3::Value* next = loop->appendNew<B3::Value>(graph.proc, B3::Add, origin, index, loop->appendNew<B3::Value>(graph.proc, B3::ZExt32, origin, byte));
        loop->appendNew<B3::UpsilonValue>(graph.proc, origin, next, index);
        loop->appendNew<B3::Value>(graph.proc, B3::Branch, origin, loop->appendNew<B3::Value>(graph.proc, B3::Below, origin, next, limit));
        loop->setSuccessors(B3::FrequentedBlock(loop), B3::FrequentedBlock(done));
        graph.finish(done, keeps, owner, other, next);
    }, Keeps::OwnerInFrontOfSafepoint);
}

static void testPointerLoadedAgainInLoopFromOwnerMadeInFrontOfIt()
{
    for (bool entersThroughBlockOfItsOwn : { false, true }) {
        checkEveryForm([&](Graph& graph, Keeps keeps) {
            B3::Origin origin = Graph::originOf(otherOrigin);
            B3::BasicBlock* root = graph.proc.addBlock();
            B3::BasicBlock* entry = entersThroughBlockOfItsOwn ? graph.proc.addBlock() : root;
            B3::BasicBlock* loop = graph.proc.addBlock();
            B3::BasicBlock* done = graph.proc.addBlock();
            B3::Value* other = graph.argument(root, 1);
            B3::Value* limit = graph.argument(root, 2);
            B3::Value* owner = graph.load(root, B3::Load, B3::Int64, graph.argument(root, 0), 8, fieldOfCell);
            B3::Value* first = graph.loadPointer(root, owner);
            graph.safepoint(root, keeps, owner);
            B3::Value* index = loop->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
            B3::Value* pointer = loop->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
            B3::Value* result = done->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
            if (entersThroughBlockOfItsOwn) {
                root->appendNew<B3::UpsilonValue>(graph.proc, origin, limit, result);
                root->appendNew<B3::Value>(graph.proc, B3::Branch, origin, limit);
                root->setSuccessors(B3::FrequentedBlock(entry), B3::FrequentedBlock(done));
            }
            entry->appendNew<B3::UpsilonValue>(graph.proc, origin, entry->appendNew<B3::Const64Value>(graph.proc, origin, 0), index);
            entry->appendNew<B3::UpsilonValue>(graph.proc, origin, first, pointer);
            entry->appendNew<B3::Value>(graph.proc, B3::Jump, origin);
            entry->setSuccessors(B3::FrequentedBlock(loop));

            B3::Value* byte = graph.readThrough(loop, loop->appendNew<B3::Value>(graph.proc, B3::Add, origin, pointer, index));
            if (keeps == Keeps::OwnerBehindRead)
                graph.keepAlive(loop, owner);
            if (keeps == Keeps::OtherCellBehindRead)
                graph.keepAlive(loop, other);
            graph.safepoint(loop)->effects.writes = B3::HeapRange(collectorState.begin(), fieldOfPointer.end());
            loop->appendNew<B3::UpsilonValue>(graph.proc, origin, graph.loadPointer(loop, keeps == Keeps::OwnerBehindRead ? owner : other), pointer);
            B3::Value* next = loop->appendNew<B3::Value>(graph.proc, B3::Add, origin, index, loop->appendNew<B3::Value>(graph.proc, B3::ZExt32, origin, byte));
            loop->appendNew<B3::UpsilonValue>(graph.proc, origin, next, index);
            loop->appendNew<B3::UpsilonValue>(graph.proc, origin, next, result);
            loop->appendNew<B3::Value>(graph.proc, B3::Branch, origin, loop->appendNew<B3::Value>(graph.proc, B3::Below, origin, next, limit));
            loop->setSuccessors(B3::FrequentedBlock(loop), B3::FrequentedBlock(done));
            done->appendNew<B3::Value>(graph.proc, B3::Return, origin, result);
        });
    }
}

static void testJoinedPointers()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::Origin origin = Graph::originOf(otherOrigin);
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::BasicBlock* blocks[] = { graph.proc.addBlock(), graph.proc.addBlock() };
        B3::BasicBlock* joined = graph.proc.addBlock();
        B3::Value* other = graph.argument(root, 2);
        B3::Value* owners[] = { graph.argument(root, 0), graph.argument(root, 1) };
        root->appendNew<B3::Value>(graph.proc, B3::Branch, origin, other);
        root->setSuccessors(B3::FrequentedBlock(blocks[0]), B3::FrequentedBlock(blocks[1]));
        B3::Value* owner = joined->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
        B3::Value* pointer = joined->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
        for (unsigned i = 0; i < 2; ++i) {
            blocks[i]->appendNew<B3::UpsilonValue>(graph.proc, origin, owners[i], owner);
            blocks[i]->appendNew<B3::UpsilonValue>(graph.proc, origin, graph.loadPointer(blocks[i], owners[i]), pointer);
            blocks[i]->appendNew<B3::Value>(graph.proc, B3::Jump, origin);
            blocks[i]->setSuccessors(B3::FrequentedBlock(joined));
        }
        graph.safepoint(joined, keeps, owner);
        graph.finish(joined, keeps, owner, other, graph.readThrough(joined, pointer));
    });
}

static void testChosenOwner()
{
    checkEveryForm([](Graph& graph, Keeps keeps) {
        B3::Origin origin = Graph::originOf(otherOrigin);
        B3::BasicBlock* root = graph.proc.addBlock();
        B3::Value* owner = graph.argument(root, 0);
        B3::Value* other = graph.argument(root, 1);
        B3::Value* chosen = root->appendNew<B3::Value>(graph.proc, B3::Select, origin, graph.argument(root, 2), owner, root->appendNew<B3::Const64Value>(graph.proc, origin, 10));
        B3::Value* pointer = graph.loadPointer(root, owner);
        graph.safepoint(root, keeps, chosen);
        graph.finish(root, keeps, chosen, other, graph.readThrough(root, pointer));
    });
}

static void testPointerOrWordOfPinnedRegister()
{
    constexpr GPRReg fixedRegister = GPRInfo::regCS1;
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (bool isPinned : { false, true }) {
            for (bool isChosen : { false, true }) {
                InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                    B3::Origin origin = Graph::originOf(otherOrigin);
                    if (isPinned)
                        graph.proc.pinRegister(fixedRegister);
                    B3::BasicBlock* root = graph.proc.addBlock();
                    B3::Value* fixed = root->appendNew<B3::ArgumentRegValue>(graph.proc, origin, fixedRegister);
                    B3::Value* holder = graph.argument(root, 0);
                    B3::Value* hasPointer = graph.argument(root, 1);
                    graph.safepoint(root);
                    B3::Value* owner = graph.load(root, B3::Load, B3::Int64, holder, 8, fieldOfCell);
                    if (isChosen) {
                        B3::Value* chosen = root->appendNew<B3::Value>(graph.proc, B3::Select, origin, hasPointer, graph.loadPointer(root, owner), fixed);
                        graph.finish(root, Keeps::Nothing, owner, holder, graph.readThrough(root, chosen));
                        return;
                    }
                    B3::BasicBlock* blocks[] = { graph.proc.addBlock(), graph.proc.addBlock() };
                    B3::BasicBlock* joined = graph.proc.addBlock();
                    root->appendNew<B3::Value>(graph.proc, B3::Branch, origin, hasPointer);
                    root->setSuccessors(B3::FrequentedBlock(blocks[0]), B3::FrequentedBlock(blocks[1]));
                    B3::Value* either = joined->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
                    B3::Value* values[] = { graph.loadPointer(blocks[0], owner), fixed };
                    for (unsigned i = 0; i < 2; ++i) {
                        blocks[i]->appendNew<B3::UpsilonValue>(graph.proc, origin, values[i], either);
                        blocks[i]->appendNew<B3::Value>(graph.proc, B3::Jump, origin);
                        blocks[i]->setSuccessors(B3::FrequentedBlock(joined));
                    }
                    // With this the block is not copied into the two in front of it.
                    graph.keepAlive(joined, hasPointer);
                    graph.finish(joined, Keeps::Nothing, owner, holder, graph.readThrough(joined, either));
                });
                AOT_TEST_CHECK(findings.violations.isEmpty() == isPinned);
                AOT_TEST_CHECK(!findings.numberOfPointersAcrossSafepoints == isPinned);
            }
        }
    }
}

static unsigned numberOfChoicesOnFlags(B3::Air::Code& code)
{
    unsigned result = 0;
    for (B3::Air::BasicBlock* block : code) {
        for (B3::Air::Inst& inst : *block)
            result += inst.kind.opcode == B3::Air::MoveConditionallyOnFlags64;
    }
    return result;
}

// A choice and a branch ask the same comparison. It is made once, and the choice reads the flags.
static void testOwnerChosenOnFlags()
{
    bool hasSeenKeptOwnerChosenOnFlags = false;
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (Keeps keeps : allKeeps) {
            B3::Policy policy;
            policy.allocation.keepsValuesAfterLowering = true;
            policy.air.peepholes.reusesConditionFlags = true;
            B3::Procedure proc(policy);
            proc.setHeapThatLoadsStayInFrontOf(weakTableStorage);
            Graph graph(proc, numberOfPreservedRegisters);
            B3::Origin origin = Graph::originOf(otherOrigin);
            B3::BasicBlock* root = proc.addBlock();
            B3::BasicBlock* isCell = proc.addBlock();
            B3::BasicBlock* isNoCell = proc.addBlock();
            B3::Value* owner = graph.argument(root, 0);
            B3::Value* other = graph.argument(root, 1);
            B3::Value* word = graph.argument(root, 2);
            B3::Value* namesCell = root->appendNew<B3::Value>(proc, B3::Above, origin, word, root->appendNew<B3::Const64Value>(proc, origin, 10));
            B3::Value* chosen = root->appendNew<B3::Value>(proc, B3::Select, origin, namesCell, owner, word);
            root->appendNew<B3::Value>(proc, B3::Branch, origin, namesCell);
            root->setSuccessors(B3::FrequentedBlock(isCell), B3::FrequentedBlock(isNoCell));
            isNoCell->appendNew<B3::Value>(proc, B3::Return, origin, isNoCell->appendNew<B3::Const32Value>(proc, origin, 0));
            B3::Value* pointer = graph.loadPointer(isCell, owner);
            graph.safepoint(isCell, keeps, chosen);
            graph.finish(isCell, keeps, chosen, other, graph.readThrough(isCell, pointer));
            B3::prepareForGeneration(proc);
            AOT_TEST_CHECK(!proc.report().failure);
            hasSeenKeptOwnerChosenOnFlags |= holdsOwner(keeps) && numberOfChoicesOnFlags(proc.code());
            InnerPointerFindings findings;
            findInnerPointerViolations(proc.code(), rules(), findings);
            AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
            AOT_TEST_CHECK(findings.violations.isEmpty() == holdsOwner(keeps));
        }
    }
    AOT_TEST_CHECK(!isX86_64() || hasSeenKeptOwnerChosenOnFlags);
}

static void testOwnerThatStays()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (B3::HeapRange range : { permanentCells, fieldOfCell }) {
            InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                B3::BasicBlock* root = graph.proc.addBlock();
                B3::Value* owner = graph.load(root, B3::Load, B3::Int64, graph.argument(root, 0), 8, range);
                B3::Value* pointer = graph.loadPointer(root, owner);
                graph.safepoint(root);
                graph.finish(root, Keeps::Nothing, owner, owner, graph.readThrough(root, pointer));
            });
            AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
            AOT_TEST_CHECK(findings.violations.isEmpty() == (range == permanentCells));
        }
    }
}

static void testPointerTheScanKnows()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (B3::HeapRange field : { fieldOfPointer, fieldOfPointerTheScanKnows }) {
            for (B3::HeapRange range : { contents, contentsWithoutCells }) {
                InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                    B3::BasicBlock* root = graph.proc.addBlock();
                    B3::Value* pointer = graph.loadPointer(root, graph.argument(root, 0), field);
                    graph.safepoint(root);
                    root->appendNew<B3::Value>(graph.proc, B3::Return, Graph::originOf(otherOrigin), graph.load(root, B3::Load, B3::Int64, pointer, 0, range));
                });
                AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
                AOT_TEST_CHECK(findings.violations.isEmpty() == (field == fieldOfPointerTheScanKnows && range == contentsWithoutCells));
            }
            InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                B3::BasicBlock* root = graph.proc.addBlock();
                B3::Value* pointer = graph.loadPointer(root, graph.argument(root, 0), field);
                graph.safepoint(root);
                root->appendNew<B3::Value>(graph.proc, B3::Return, Graph::originOf(otherOrigin), graph.load(root, B3::Load, B3::Double, pointer, 0, contents));
            });
            AOT_TEST_CHECK(findings.violations.isEmpty() == (field == fieldOfPointerTheScanKnows));
        }
    }
}

// One load of the pointer serves two ways. What is read on one of them may name cells, what is read on the other names none.
static void testPointerThatGivesCellsOnOneWay()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (B3::HeapRange field : { fieldOfPointer, fieldOfPointerTheScanKnows }) {
            for (bool keepsOwnerWhereCellsAreRead : { false, true }) {
                InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                    B3::Origin origin = Graph::originOf(otherOrigin);
                    B3::BasicBlock* root = graph.proc.addBlock();
                    B3::BasicBlock* readsCells = graph.proc.addBlock();
                    B3::BasicBlock* readsNumbers = graph.proc.addBlock();
                    B3::Value* owner = graph.argument(root, 0);
                    B3::Value* pointer = graph.loadPointer(root, owner, field);
                    root->appendNew<B3::Value>(graph.proc, B3::Branch, origin, graph.argument(root, 1));
                    root->setSuccessors(B3::FrequentedBlock(readsCells), B3::FrequentedBlock(readsNumbers));
                    graph.safepoint(readsCells);
                    graph.finish(readsCells, keepsOwnerWhereCellsAreRead ? Keeps::OwnerBehindRead : Keeps::Nothing, owner, owner, graph.load(readsCells, B3::Load, B3::Int64, pointer, 0, contents));
                    graph.safepoint(readsNumbers);
                    graph.finish(readsNumbers, Keeps::Nothing, owner, owner, graph.load(readsNumbers, B3::Load, B3::Int64, pointer, 0, contentsWithoutCells));
                });
                AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
                AOT_TEST_CHECK(findings.violations.size() == static_cast<size_t>(!keepsOwnerWhereCellsAreRead) + (field == fieldOfPointer));
            }
        }
    }
}

static void testPointerThatGivesCellsInOneLoop()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (bool keepsOwnerWhereCellsAreRead : { false, true }) {
            InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                B3::Origin origin = Graph::originOf(otherOrigin);
                B3::BasicBlock* root = graph.proc.addBlock();
                B3::Value* owner = graph.argument(root, 0);
                B3::Value* limit = graph.argument(root, 2);
                B3::Value* pointer = graph.loadPointer(root, owner, fieldOfPointerTheScanKnows);
                auto addLoop = [&](B3::HeapRange range, bool keepsOwner) {
                    B3::BasicBlock* entry = graph.proc.addBlock();
                    B3::BasicBlock* loop = graph.proc.addBlock();
                    B3::BasicBlock* done = graph.proc.addBlock();
                    auto* first = entry->appendNew<B3::UpsilonValue>(graph.proc, origin, entry->appendNew<B3::Const64Value>(graph.proc, origin, 0));
                    entry->appendNew<B3::Value>(graph.proc, B3::Jump, origin);
                    entry->setSuccessors(B3::FrequentedBlock(loop));
                    B3::Value* index = loop->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, origin);
                    first->setPhi(index);
                    graph.safepoint(loop);
                    B3::Value* word = graph.load(loop, B3::Load, B3::Int64, loop->appendNew<B3::Value>(graph.proc, B3::Add, origin, pointer, index), 0, range);
                    if (keepsOwner)
                        graph.keepAlive(loop, owner);
                    B3::Value* next = loop->appendNew<B3::Value>(graph.proc, B3::Add, origin, index, word);
                    loop->appendNew<B3::UpsilonValue>(graph.proc, origin, next, index);
                    loop->appendNew<B3::Value>(graph.proc, B3::Branch, origin, loop->appendNew<B3::Value>(graph.proc, B3::Below, origin, next, limit));
                    loop->setSuccessors(B3::FrequentedBlock(loop), B3::FrequentedBlock(done));
                    done->appendNew<B3::Value>(graph.proc, B3::Return, origin, next);
                    return entry;
                };
                B3::BasicBlock* loopOverCells = addLoop(contents, keepsOwnerWhereCellsAreRead);
                B3::BasicBlock* loopOverNumbers = addLoop(contentsWithoutCells, false);
                root->appendNew<B3::Value>(graph.proc, B3::Branch, origin, graph.argument(root, 1));
                root->setSuccessors(B3::FrequentedBlock(loopOverCells), B3::FrequentedBlock(loopOverNumbers));
            });
            AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
            AOT_TEST_CHECK(findings.violations.size() == static_cast<size_t>(!keepsOwnerWhereCellsAreRead));
        }
    }
}

// The owner is needed up to the last read that may give a cell, and no further.
static void testCellsReadBetweenSafepoints()
{
    for (unsigned numberOfPreservedRegisters : numbersOfPreservedRegisters) {
        for (bool computesAddressFirst : { false, true }) {
            for (bool keepsOwnerWhereCellsAreRead : { false, true }) {
                InnerPointerFindings findings = check(numberOfPreservedRegisters, [&](Graph& graph) {
                    B3::Origin origin = Graph::originOf(otherOrigin);
                    B3::BasicBlock* root = graph.proc.addBlock();
                    B3::Value* owner = graph.argument(root, 0);
                    B3::Value* pointer = graph.loadPointer(root, owner, fieldOfPointerTheScanKnows);
                    B3::Value* address = pointer;
                    if (computesAddressFirst) {
                        address = root->appendNew<B3::Value>(graph.proc, B3::Add, origin, pointer, graph.argument(root, 1));
                        B3::PatchpointValue* opaque = graph.emptyPatchpoint(root, otherOrigin);
                        opaque->effects.writesLocalState = true;
                        opaque->append(address, B3::ValueRep::SomeRegister);
                    }
                    graph.safepoint(root);
                    B3::Value* cell = graph.load(root, B3::Load, B3::Int64, address, 0, contents);
                    if (keepsOwnerWhereCellsAreRead)
                        graph.keepAlive(root, owner);
                    graph.safepoint(root);
                    B3::Value* number = graph.load(root, B3::Load, B3::Int64, pointer, 8, contentsWithoutCells);
                    root->appendNew<B3::Value>(graph.proc, B3::Return, origin, root->appendNew<B3::Value>(graph.proc, B3::Add, origin, cell, number));
                });
                AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
                AOT_TEST_CHECK(findings.violations.isEmpty() == keepsOwnerWhereCellsAreRead);
            }
        }
    }
}

static void testLoadFoldedIntoUserBehindSafepoint()
{
    for (bool loadsStayInFront : { true, false }) {
        InnerPointerFindings findings = check(4, [](Graph& graph) {
            B3::BasicBlock* root = graph.proc.addBlock();
            B3::Value* owner = graph.argument(root, 0);
            B3::Value* addend = root->appendNew<B3::Value>(graph.proc, B3::Trunc, Graph::originOf(otherOrigin), graph.argument(root, 1));
            B3::Value* word = graph.load(root, B3::Load, B3::Int32, graph.loadPointer(root, owner), 0, contents);
            graph.keepAlive(root, owner);
            graph.safepoint(root);
            graph.finish(root, Keeps::Nothing, owner, owner, root->appendNew<B3::Value>(graph.proc, B3::Add, Graph::originOf(otherOrigin), word, addend));
        }, loadsStayInFront);
        AOT_TEST_CHECK(findings.violations.isEmpty() == (loadsStayInFront || !isX86()));
    }
}

// (words[0] + other[8]) + words[8]: the sum takes in the load that stands next to it. The load of the same offset further down reads something else and has an instruction of its own.
static void testLoadFoldedIntoUserAboveOtherLoadOfSameOffset()
{
    for (B3::HeapRange rangeOfLastWord : { contentsWithoutCells, contents }) {
        InnerPointerFindings findings = check(4, [&](Graph& graph) {
            B3::Origin origin = Graph::originOf(otherOrigin);
            B3::BasicBlock* root = graph.proc.addBlock();
            B3::Value* pointer = graph.loadPointer(root, graph.argument(root, 0), fieldOfPointerTheScanKnows);
            B3::Value* other = graph.argument(root, 1);
            graph.safepoint(root);
            B3::Value* first = graph.load(root, B3::Load, B3::Int32, pointer, 0, contentsWithoutCells);
            B3::Value* elsewhere = graph.load(root, B3::Load, B3::Int32, other, 8, contents);
            B3::Value* sum = root->appendNew<B3::Value>(graph.proc, B3::Add, origin, first, elsewhere);
            B3::Value* last = graph.load(root, B3::Load, B3::Int32, pointer, 8, rangeOfLastWord);
            root->appendNew<B3::Value>(graph.proc, B3::Return, origin, root->appendNew<B3::Value>(graph.proc, B3::Add, origin, sum, last));
        });
        AOT_TEST_CHECK(findings.numberOfPointersAcrossSafepoints);
        AOT_TEST_CHECK(findings.violations.isEmpty() == (rangeOfLastWord == contentsWithoutCells));
    }
}

static uint64_t registersAtSafepoint[MacroAssembler::numberOfRegisters()];

static void testDeadValuesAreOverwritten()
{
    constexpr uint64_t deadValue = 0x1234567890abcdefull;
    // The safepoint needs one register for itself.
    for (unsigned numberOfPreservedRegisters : { 1u, 4u }) {
        B3::Origin origin = Graph::originOf(otherOrigin);
        B3::Policy policy;
        policy.allocation.keepsValuesAfterLowering = true;
        B3::Procedure proc(policy);
        Graph graph(proc, numberOfPreservedRegisters);
        B3::BasicBlock* root = proc.addBlock();
        B3::Value* dead = graph.argument(root, 0);
        B3::Value* first = graph.argument(root, 1);
        B3::Value* second = graph.argument(root, 2);
        B3::Value* sum = root->appendNew<B3::Value>(proc, B3::Add, origin, first, root->appendNew<B3::Value>(proc, B3::Mul, origin, second, root->appendNew<B3::Const64Value>(proc, origin, 5)));
        graph.keepAlive(root, dead);
        B3::PatchpointValue* safepoint = graph.safepoint(root);
        safepoint->numGPScratchRegisters = 1;
        safepoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            GPRReg scratch = params.gpScratch(0);
            for (unsigned index = 0; index < MacroAssembler::numberOfRegisters(); ++index) {
                GPRReg reg = Reg::fromIndex(index).gpr();
                if (reg == scratch || reg == MacroAssembler::stackPointerRegister)
                    continue;
                jit.move(CCallHelpers::TrustedImmPtr(&registersAtSafepoint[index]), scratch);
                jit.storePtr(reg, CCallHelpers::Address(scratch));
            }
        });
        root->appendNew<B3::Value>(proc, B3::Return, origin, root->appendNew<B3::Value>(proc, B3::Add, origin, sum, root->appendNew<B3::Value>(proc, B3::Add, origin, first, second)));
        B3::prepareForGeneration(proc);
        AOT_TEST_CHECK(!proc.report().failure);
        AOT_TEST_CHECK(overwriteDeadValuesAtSafepoints(proc.code(), rules()));

        CCallHelpers jit;
        B3::generate(proc, jit);
        LinkBuffer linkBuffer(jit, nullptr);
        MacroAssemblerCodeRef<JSEntryPtrTag> code = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT code that overwrites dead values");
        uint64_t result = std::bit_cast<uint64_t (SYSV_ABI *)(uint64_t, uint64_t, uint64_t)>(untagCFunctionPtr<JSEntryPtrTag>(code.code().taggedPtr()))(deadValue, 7, 9);
        AOT_TEST_CHECK(result == 7 + 9 * 5 + 7 + 9);
        for (uint64_t value : registersAtSafepoint)
            AOT_TEST_CHECK(value != deadValue);
    }
}

std::span<const NativeTest> innerPointerValidationTests()
{
    static constexpr NativeTest tests[] = {
        { "inner-pointer-read-behind-safepoint"_s, testPointerReadBehindSafepoint },
        { "inner-pointer-not-read-behind-safepoint"_s, testPointerNotReadBehindSafepoint },
        { "inner-pointer-address-computed-in-front-of-safepoint"_s, testAddressComputedInFrontOfSafepoint },
        { "inner-pointer-loaded-through-pointer"_s, testPointerLoadedThroughPointer },
        { "inner-pointer-whose-load-took-trap"_s, testPointerWhoseLoadTookTrap },
        { "inner-pointer-loaded-through-pointer-whose-load-took-trap"_s, testPointerLoadedThroughPointerWhoseLoadTookTrap },
        { "inner-pointer-owner-whose-load-took-trap"_s, testOwnerWhoseLoadTookTrap },
        { "inner-pointer-only-compared"_s, testPointerOnlyCompared },
        { "inner-pointer-handed-to-safepoint"_s, testPointerHandedToSafepoint },
        { "inner-pointer-handed-to-safepoint-and-read-behind-it"_s, testPointerHandedToSafepointAndReadBehindIt },
        { "inner-pointer-safepoint-in-other-block"_s, testSafepointInOtherBlock },
        { "inner-pointer-safepoint-in-loop"_s, testSafepointInLoop },
        { "inner-pointer-loaded-again-in-loop-from-owner-made-in-front-of-it"_s, testPointerLoadedAgainInLoopFromOwnerMadeInFrontOfIt },
        { "inner-pointer-joined"_s, testJoinedPointers },
        { "inner-pointer-chosen-owner"_s, testChosenOwner },
        { "inner-pointer-or-word-of-pinned-register"_s, testPointerOrWordOfPinnedRegister },
        { "inner-pointer-owner-chosen-on-flags"_s, testOwnerChosenOnFlags },
        { "inner-pointer-owner-that-stays"_s, testOwnerThatStays },
        { "inner-pointer-the-scan-knows"_s, testPointerTheScanKnows },
        { "inner-pointer-gives-cells-on-one-way"_s, testPointerThatGivesCellsOnOneWay },
        { "inner-pointer-gives-cells-in-one-loop"_s, testPointerThatGivesCellsInOneLoop },
        { "inner-pointer-cells-read-between-safepoints"_s, testCellsReadBetweenSafepoints },
        { "inner-pointer-load-folded-into-user-behind-safepoint"_s, testLoadFoldedIntoUserBehindSafepoint },
        { "inner-pointer-load-folded-into-user-above-other-load-of-same-offset"_s, testLoadFoldedIntoUserAboveOtherLoadOfSameOffset },
        { "inner-pointer-dead-values-are-overwritten"_s, testDeadValuesAreOverwritten },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
