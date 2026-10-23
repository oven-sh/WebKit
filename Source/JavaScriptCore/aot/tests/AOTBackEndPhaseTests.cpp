/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTFrameAddressValidation.h"
#include "AOTImpliedBranchMerging.h"
#include "AOTIntegerUpdatesInMemory.h"
#include "AOTPhiNarrowing.h"
#include "AOTPureOperations.h"
#include "AOTVector.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3Generate.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3SlotBaseValue.h"
#include "B3UpsilonValue.h"
#include "B3Validate.h"
#include "B3ValueInlines.h"
#include <limits>
#include <optional>

namespace JSC { namespace AOT {

static constexpr uint32_t firstAddressOrigin = 1;
static constexpr uint32_t secondAddressOrigin = 2;
static constexpr uint32_t suspensionOrigin = 3;
static constexpr uint32_t otherOrigin = 4;

namespace {

enum class SlotBases : bool { Shared, OnePerUse };
enum class Materializes : bool { No, Yes };

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

static B3::Value* appendSlotBase(B3::Procedure& proc, B3::BasicBlock* block, B3::Air::StackSlot* slot, uint32_t origin)
{
    return block->appendNew<B3::SlotBaseValue>(proc, originOf(origin), slot);
}

static B3::Value* appendStore(B3::Procedure& proc, B3::BasicBlock* block, B3::Value* address)
{
    B3::Value* zero = block->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 0);
    return block->appendNew<B3::MemoryValue>(proc, B3::Store, originOf(otherOrigin), zero, address);
}

static void appendSuspension(B3::Procedure& proc, B3::BasicBlock* block)
{
    B3::PatchpointValue* suspension = block->appendNew<B3::PatchpointValue>(proc, B3::Void, originOf(suspensionOrigin));
    suspension->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
}

static void appendReturn(B3::Procedure& proc, B3::BasicBlock* block)
{
    block->appendNew<B3::Value>(proc, B3::Return, originOf(otherOrigin));
}

static void appendJump(B3::Procedure& proc, B3::BasicBlock* block, B3::BasicBlock* target)
{
    block->appendNew<B3::Value>(proc, B3::Jump, originOf(otherOrigin));
    block->setSuccessors(B3::FrequentedBlock(target));
}

static bool materialize(B3::Procedure& proc)
{
    return materializeSlotBasesLate().run->run(proc);
}

static Vector<FrameAddressViolation> findFrameAddressViolations(B3::Procedure& proc)
{
    Vector<FrameAddressViolation> violations;
    AOT_TEST_CHECK(!validateFrameAddresses(violations, originsOfSuspensions()).run->run(proc));
    return violations;
}

static unsigned indexOf(B3::BasicBlock* block, B3::Value* value)
{
    for (unsigned index = 0; index < block->size(); ++index) {
        if (block->at(index) == value)
            return index;
    }
    failNativeTest(__FILE__, __LINE__, "the block has the value");
}

static B3::Value* addressOf(B3::Value* store)
{
    return store->as<B3::MemoryValue>()->lastChild();
}

static void checkIsCopyOfSlotBase(B3::Value* copy, B3::Value* original)
{
    AOT_TEST_CHECK(copy != original);
    AOT_TEST_CHECK(copy->opcode() == B3::SlotBase);
    AOT_TEST_CHECK(copy->as<B3::SlotBaseValue>()->slot() == original->as<B3::SlotBaseValue>()->slot());
    AOT_TEST_CHECK(copy->origin().dfgOrigin() == original->origin().dfgOrigin());
}

static void checkStoresToOwnSlotBase(B3::BasicBlock* block, B3::Value* store, B3::Value* original)
{
    checkIsCopyOfSlotBase(addressOf(store), original);
    AOT_TEST_CHECK(indexOf(block, addressOf(store)) + 1 == indexOf(block, store));
}

static void testMaterializeSlotBases()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    B3::BasicBlock* next = proc.addBlock();
    B3::Value* firstBase = appendSlotBase(proc, root, proc.addStackSlot(8), firstAddressOrigin);
    B3::Value* firstStore = appendStore(proc, root, firstBase);
    appendJump(proc, root, next);
    B3::Value* secondBase = appendSlotBase(proc, next, proc.addStackSlot(8), secondAddressOrigin);
    B3::Value* secondStore = appendStore(proc, next, secondBase);
    appendReturn(proc, next);
    proc.resetReachability();

    AOT_TEST_CHECK(materialize(proc));

    checkStoresToOwnSlotBase(root, firstStore, firstBase);
    checkStoresToOwnSlotBase(next, secondStore, secondBase);
    AOT_TEST_CHECK(isOrigin(addressOf(firstStore)->origin(), firstAddressOrigin));
    AOT_TEST_CHECK(isOrigin(addressOf(secondStore)->origin(), secondAddressOrigin));
    B3::validate(proc);

    B3::Value* firstCopy = addressOf(firstStore);
    AOT_TEST_CHECK(materialize(proc));
    checkStoresToOwnSlotBase(root, firstStore, firstCopy);
    B3::validate(proc);
}

static void testMaterializeOneCopyPerUse()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    B3::Value* base = appendSlotBase(proc, root, proc.addStackSlot(8), firstAddressOrigin);
    B3::Value* firstStore = appendStore(proc, root, base);
    B3::Value* secondStore = appendStore(proc, root, base);
    appendReturn(proc, root);

    AOT_TEST_CHECK(materialize(proc));

    checkStoresToOwnSlotBase(root, firstStore, base);
    checkStoresToOwnSlotBase(root, secondStore, base);
    AOT_TEST_CHECK(addressOf(firstStore) != addressOf(secondStore));
    B3::validate(proc);
}

template<typename ValueBuilder>
static void checkDerivedAddressIsCopied(unsigned addressChild, const ValueBuilder& buildValue)
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    B3::Value* base = appendSlotBase(proc, root, proc.addStackSlot(16), firstAddressOrigin);
    B3::Value* derived = buildValue(proc, root, base);
    B3::Value* store = appendStore(proc, root, derived);
    appendReturn(proc, root);

    AOT_TEST_CHECK(materialize(proc));

    B3::Value* copy = addressOf(store);
    AOT_TEST_CHECK(copy != derived);
    AOT_TEST_CHECK(copy->opcode() == derived->opcode() && copy->numChildren() == derived->numChildren());
    AOT_TEST_CHECK(isOrigin(copy->origin(), secondAddressOrigin));
    for (unsigned index = 0; index < copy->numChildren(); ++index) {
        if (index == addressChild)
            checkIsCopyOfSlotBase(copy->child(index), base);
        else
            AOT_TEST_CHECK(copy->child(index) == derived->child(index));
    }
    AOT_TEST_CHECK(derived->child(addressChild) == base);

    unsigned storeIndex = indexOf(root, store);
    AOT_TEST_CHECK(indexOf(root, copy->child(addressChild)) + 2 == storeIndex);
    AOT_TEST_CHECK(indexOf(root, copy) + 1 == storeIndex);
    B3::validate(proc);
}

static void testMaterializeDerivedAddresses()
{
    for (B3::Opcode opcode : { B3::Add, B3::Sub }) {
        for (unsigned addressChild : { 0u, 1u }) {
            checkDerivedAddressIsCopied(addressChild, [&](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
                B3::Value* offset = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 8);
                B3::Value* children[] = { offset, offset };
                children[addressChild] = base;
                return root->appendNew<B3::Value>(proc, opcode, originOf(secondAddressOrigin), children[0], children[1]);
            });
        }
    }

    checkDerivedAddressIsCopied(0, [](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
        return root->appendNew<B3::Value>(proc, B3::Identity, originOf(secondAddressOrigin), base);
    });

    for (unsigned addressChild : { 1u, 2u }) {
        checkDerivedAddressIsCopied(addressChild, [&](B3::Procedure& proc, B3::BasicBlock* root, B3::Value* base) {
            B3::Value* zero = root->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 0);
            B3::Value* children[] = { root->appendNew<B3::Const32Value>(proc, originOf(otherOrigin), 1), zero, zero };
            children[addressChild] = base;
            return root->appendNew<B3::Value>(proc, B3::Select, originOf(secondAddressOrigin), children[0], children[1], children[2]);
        });
    }
}

static void testMaterializeNothing()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    appendStore(proc, root, root->appendNew<B3::ArgumentRegValue>(proc, originOf(firstAddressOrigin), GPRInfo::argumentGPR0));
    appendReturn(proc, root);
    unsigned numberOfValues = proc.values().size();
    size_t blockSize = root->size();

    AOT_TEST_CHECK(!materialize(proc));

    AOT_TEST_CHECK(proc.values().size() == numberOfValues);
    AOT_TEST_CHECK(root->size() == blockSize);
}

static void testMaterializeUnusedSlotBase()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    appendSlotBase(proc, root, proc.addStackSlot(8), firstAddressOrigin);
    appendReturn(proc, root);
    size_t blockSize = root->size();

    AOT_TEST_CHECK(!materialize(proc));
    AOT_TEST_CHECK(root->size() == blockSize);
}

static void appendStoresAroundSuspension(B3::Procedure& proc, SlotBases slotBases)
{
    B3::BasicBlock* root = proc.addBlock();
    B3::Air::StackSlot* slot = proc.addStackSlot(8);
    B3::Value* base = appendSlotBase(proc, root, slot, firstAddressOrigin);
    appendStore(proc, root, base);
    appendSuspension(proc, root);
    if (slotBases == SlotBases::OnePerUse)
        base = appendSlotBase(proc, root, slot, secondAddressOrigin);
    appendStore(proc, root, base);
    appendReturn(proc, root);
}

static Vector<FrameAddressViolation> findViolations(SlotBases slotBases, Materializes materializes)
{
    B3::Procedure proc;
    appendStoresAroundSuspension(proc, slotBases);
    if (materializes == Materializes::Yes)
        AOT_TEST_CHECK(materialize(proc));
    return findFrameAddressViolations(proc);
}

static void testMaterializeSplitsSharedSlotBase()
{
    Vector<FrameAddressViolation> violations = findViolations(SlotBases::Shared, Materializes::No);
    AOT_TEST_CHECK(violations.size() == 1);
    AOT_TEST_CHECK(isOrigin(violations[0].address, firstAddressOrigin));
    AOT_TEST_CHECK(isOrigin(violations[0].suspension, suspensionOrigin));

    AOT_TEST_CHECK(findViolations(SlotBases::Shared, Materializes::Yes).isEmpty());
    AOT_TEST_CHECK(findViolations(SlotBases::OnePerUse, Materializes::No).isEmpty());
    AOT_TEST_CHECK(findViolations(SlotBases::OnePerUse, Materializes::Yes).isEmpty());
}

static void testPureFunctionAtSuspensionIsNoSuspension()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    appendSuspension(proc, root);
    B3::Value* base = appendSlotBase(proc, root, proc.addStackSlot(8), firstAddressOrigin);
    B3::PatchpointValue* conversion = root->appendNew<B3::PatchpointValue>(proc, B3::Void, originOf(suspensionOrigin));
    conversion->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
    conversion->effects = B3::Effects::none();
    conversion->pureFunction = B3::PureFunction { 1 };
    appendStore(proc, root, base);
    appendReturn(proc, root);
    AOT_TEST_CHECK(findFrameAddressViolations(proc).isEmpty());

    conversion->pureFunction = std::nullopt;
    AOT_TEST_CHECK(findFrameAddressViolations(proc).size() == 1);
}

static void appendBranch(B3::Procedure& proc, B3::BasicBlock* block, B3::BasicBlock* taken, B3::BasicBlock* notTaken)
{
    B3::Value* condition = block->appendNew<B3::ArgumentRegValue>(proc, originOf(otherOrigin), GPRInfo::argumentGPR0);
    block->appendNew<B3::Value>(proc, B3::Branch, originOf(otherOrigin), condition);
    block->setSuccessors(B3::FrequentedBlock(taken), B3::FrequentedBlock(notTaken));
}

namespace {

struct JoinedAddresses {
    B3::Value* phi;
    B3::Value* store;
    B3::BasicBlock* blockOfStore;
};

} // anonymous namespace

// Two blocks pass an address each to a Phi, which is stored through behind a suspension.
template<typename FirstBuilder, typename SecondBuilder>
static JoinedAddresses appendJoinOfAddresses(B3::Procedure& proc, const FirstBuilder& buildFirst, const SecondBuilder& buildSecond)
{
    B3::BasicBlock* root = proc.addBlock();
    B3::BasicBlock* first = proc.addBlock();
    B3::BasicBlock* second = proc.addBlock();
    B3::BasicBlock* join = proc.addBlock();
    B3::Value* phi = join->appendNew<B3::Value>(proc, B3::Phi, B3::pointerType(), originOf(secondAddressOrigin));
    appendBranch(proc, root, first, second);
    first->appendNew<B3::UpsilonValue>(proc, originOf(otherOrigin), buildFirst(first), phi);
    appendJump(proc, first, join);
    second->appendNew<B3::UpsilonValue>(proc, originOf(otherOrigin), buildSecond(second), phi);
    appendJump(proc, second, join);
    appendSuspension(proc, join);
    B3::Value* store = appendStore(proc, join, phi);
    appendReturn(proc, join);
    proc.resetReachability();
    return { phi, store, join };
}

static void testMaterializePhiOfOneSlot()
{
    B3::Procedure proc;
    B3::Air::StackSlot* slot = proc.addStackSlot(8);
    auto buildSlotBase = [&](B3::BasicBlock* block) {
        return appendSlotBase(proc, block, slot, firstAddressOrigin);
    };
    JoinedAddresses joined = appendJoinOfAddresses(proc, buildSlotBase, buildSlotBase);
    AOT_TEST_CHECK(findFrameAddressViolations(proc).size() == 1);

    AOT_TEST_CHECK(materialize(proc));

    B3::Value* address = addressOf(joined.store);
    AOT_TEST_CHECK(address->opcode() == B3::SlotBase);
    AOT_TEST_CHECK(address->as<B3::SlotBaseValue>()->slot() == slot);
    AOT_TEST_CHECK(indexOf(joined.blockOfStore, address) + 1 == indexOf(joined.blockOfStore, joined.store));
    AOT_TEST_CHECK(findFrameAddressViolations(proc).isEmpty());
}

static void testMaterializePhiOfOneDistanceIntoSlot()
{
    B3::Procedure proc;
    B3::Air::StackSlot* slot = proc.addStackSlot(32);
    auto buildSum = [&](B3::BasicBlock* block) -> B3::Value* {
        B3::Value* distance = block->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 16);
        return block->appendNew<B3::Value>(proc, B3::Add, originOf(firstAddressOrigin), appendSlotBase(proc, block, slot, firstAddressOrigin), distance);
    };
    auto buildDifference = [&](B3::BasicBlock* block) -> B3::Value* {
        B3::Value* end = block->appendNew<B3::Value>(proc, B3::Add, originOf(firstAddressOrigin), block->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 24), appendSlotBase(proc, block, slot, firstAddressOrigin));
        return block->appendNew<B3::Value>(proc, B3::Sub, originOf(firstAddressOrigin), end, block->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), 8));
    };
    JoinedAddresses joined = appendJoinOfAddresses(proc, buildSum, buildDifference);

    AOT_TEST_CHECK(materialize(proc));

    B3::Value* address = addressOf(joined.store);
    AOT_TEST_CHECK(address->opcode() == B3::Add);
    AOT_TEST_CHECK(address->child(0)->opcode() == B3::SlotBase);
    AOT_TEST_CHECK(address->child(0)->as<B3::SlotBaseValue>()->slot() == slot);
    AOT_TEST_CHECK(address->child(1)->isInt64(16));
    AOT_TEST_CHECK(findFrameAddressViolations(proc).isEmpty());
}

static void testMaterializePhiThatGoesAroundLoop()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    B3::BasicBlock* loop = proc.addBlock();
    B3::BasicBlock* done = proc.addBlock();
    B3::Air::StackSlot* slot = proc.addStackSlot(8);
    B3::Value* phi = loop->appendNew<B3::Value>(proc, B3::Phi, B3::pointerType(), originOf(secondAddressOrigin));
    root->appendNew<B3::UpsilonValue>(proc, originOf(otherOrigin), appendSlotBase(proc, root, slot, firstAddressOrigin), phi);
    appendJump(proc, root, loop);
    appendSuspension(proc, loop);
    B3::Value* store = appendStore(proc, loop, phi);
    loop->appendNew<B3::UpsilonValue>(proc, originOf(otherOrigin), phi, phi);
    appendBranch(proc, loop, loop, done);
    appendReturn(proc, done);
    proc.resetReachability();
    AOT_TEST_CHECK(!findFrameAddressViolations(proc).isEmpty());

    AOT_TEST_CHECK(materialize(proc));

    AOT_TEST_CHECK(addressOf(store)->opcode() == B3::SlotBase);
    AOT_TEST_CHECK(addressOf(store)->as<B3::SlotBaseValue>()->slot() == slot);
}

static void checkPhiIsNotSplit(B3::Procedure& proc, const JoinedAddresses& joined)
{
    materialize(proc);

    AOT_TEST_CHECK(addressOf(joined.store) == joined.phi);
    Vector<FrameAddressViolation> violations = findFrameAddressViolations(proc);
    AOT_TEST_CHECK(violations.size() == 1);
    AOT_TEST_CHECK(isOrigin(violations[0].address, secondAddressOrigin));
    AOT_TEST_CHECK(isOrigin(violations[0].suspension, suspensionOrigin));
}

static void testMaterializeDoesNotSplitPhiOfTwoSlots()
{
    B3::Procedure proc;
    auto buildOwnSlotBase = [&](B3::BasicBlock* block) {
        return appendSlotBase(proc, block, proc.addStackSlot(8), firstAddressOrigin);
    };
    checkPhiIsNotSplit(proc, appendJoinOfAddresses(proc, buildOwnSlotBase, buildOwnSlotBase));
}

static void testMaterializeDoesNotSplitPhiOfSlotAndOtherValue()
{
    B3::Procedure proc;
    auto buildSlotBase = [&](B3::BasicBlock* block) {
        return appendSlotBase(proc, block, proc.addStackSlot(8), firstAddressOrigin);
    };
    auto buildArgument = [&](B3::BasicBlock* block) -> B3::Value* {
        return block->appendNew<B3::ArgumentRegValue>(proc, originOf(otherOrigin), GPRInfo::argumentGPR1);
    };
    checkPhiIsNotSplit(proc, appendJoinOfAddresses(proc, buildSlotBase, buildArgument));
}

static void testMaterializeSplitsSharedFramePointer()
{
    B3::Procedure proc;
    B3::BasicBlock* root = proc.addBlock();
    B3::Value* frame = root->appendNew<B3::Value>(proc, B3::FramePointer, originOf(firstAddressOrigin));
    appendStore(proc, root, frame);
    appendSuspension(proc, root);
    B3::Value* store = appendStore(proc, root, frame);
    appendReturn(proc, root);
    AOT_TEST_CHECK(findFrameAddressViolations(proc).size() == 1);

    AOT_TEST_CHECK(materialize(proc));

    AOT_TEST_CHECK(addressOf(store) != frame);
    AOT_TEST_CHECK(addressOf(store)->opcode() == B3::FramePointer);
    AOT_TEST_CHECK(indexOf(root, addressOf(store)) + 1 == indexOf(root, store));
    AOT_TEST_CHECK(findFrameAddressViolations(proc).isEmpty());
}

static void testPhaseRunsAfterOptimization()
{
    B3::ClientPhase phase = materializeSlotBasesLate();
    AOT_TEST_CHECK(phase.point == B3::ClientPhase::Point::AfterOptimization);
    AOT_TEST_CHECK(phase.name == "materializeSlotBasesLate"_s);
    AOT_TEST_CHECK(phase.run);
}

static Vector<FrameAddressViolation> findViolationsInPipeline(Materializes materializes)
{
    Vector<FrameAddressViolation> violations;
    B3::Policy policy;
    if (materializes == Materializes::Yes)
        policy.clientPhases.append(materializeSlotBasesLate());
    policy.clientPhases.append(validateFrameAddresses(violations, originsOfSuspensions()));

    B3::Procedure proc(policy);
    appendStoresAroundSuspension(proc, SlotBases::OnePerUse);
    B3::prepareForGeneration(proc);
    AOT_TEST_CHECK(!proc.report().failure);
    return violations;
}

static void testMaterializeInPipeline()
{
    Vector<FrameAddressViolation> violations = findViolationsInPipeline(Materializes::No);
    AOT_TEST_CHECK(violations.size() == 1);
    AOT_TEST_CHECK(isOrigin(violations[0].suspension, suspensionOrigin));

    AOT_TEST_CHECK(findViolationsInPipeline(Materializes::Yes).isEmpty());
}

namespace {

struct TwoTests {
    B3::BasicBlock* first;
    B3::BasicBlock* second;
    B3::BasicBlock* likely;
    B3::BasicBlock* firstTarget;
    B3::BasicBlock* secondTarget;
    B3::Value* word;
};

struct TwoTestsShape {
    int32_t valueThatLeavesFirst { 0 };
    B3::Opcode secondRelation { B3::Below };
    int32_t secondConstant { 100 };
    B3::FrequencyClass frequencyOfFirstTarget { B3::FrequencyClass::Rare };
    B3::FrequencyClass frequencyOfSecondTarget { B3::FrequencyClass::Rare };
    bool loadsBetweenTests { false };
};

} // anonymous namespace

// first: word == valueThatLeavesFirst ? firstTarget : second. second: word secondRelation secondConstant ? secondTarget : likely.
static TwoTests appendTwoTests(B3::Procedure& proc, const TwoTestsShape& shape)
{
    TwoTests blocks { proc.addBlock(), proc.addBlock(), proc.addBlock(), proc.addBlock(), proc.addBlock(), nullptr };
    B3::Origin origin = originOf(otherOrigin);
    B3::Value* argument = blocks.first->appendNew<B3::ArgumentRegValue>(proc, origin, GPRInfo::argumentGPR0);
    blocks.word = blocks.first->appendNew<B3::Value>(proc, B3::Trunc, origin, argument);

    B3::Value* leavesFirst = blocks.first->appendNew<B3::Value>(proc, B3::Equal, origin, blocks.word, blocks.first->appendNew<B3::Const32Value>(proc, origin, shape.valueThatLeavesFirst));
    blocks.first->appendNew<B3::Value>(proc, B3::Branch, origin, leavesFirst);
    blocks.first->setSuccessors(B3::FrequentedBlock(blocks.firstTarget, shape.frequencyOfFirstTarget), B3::FrequentedBlock(blocks.second));

    if (shape.loadsBetweenTests)
        blocks.second->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int32, origin, argument);
    B3::Value* leavesSecond = blocks.second->appendNew<B3::Value>(proc, shape.secondRelation, origin, blocks.word, blocks.second->appendNew<B3::Const32Value>(proc, origin, shape.secondConstant));
    blocks.second->appendNew<B3::Value>(proc, B3::Branch, origin, leavesSecond);
    blocks.second->setSuccessors(B3::FrequentedBlock(blocks.secondTarget, shape.frequencyOfSecondTarget), B3::FrequentedBlock(blocks.likely));

    appendReturn(proc, blocks.likely);
    appendReturn(proc, blocks.firstTarget);
    appendReturn(proc, blocks.secondTarget);
    proc.resetReachability();
    return blocks;
}

static void checkTestsAreLeftAlone(const TwoTestsShape& shape)
{
    B3::Procedure proc;
    TwoTests blocks = appendTwoTests(proc, shape);
    AOT_TEST_CHECK(!mergeImpliedBranches(proc));
    AOT_TEST_CHECK(blocks.first->last()->opcode() == B3::Branch);
    AOT_TEST_CHECK(blocks.first->taken().block() == blocks.firstTarget);
    AOT_TEST_CHECK(blocks.second->taken().block() == blocks.secondTarget);
}

static void testMergeImpliedBranches()
{
    B3::Procedure proc;
    TwoTests blocks = appendTwoTests(proc, { });
    B3::Value* firstBranch = blocks.first->last();

    AOT_TEST_CHECK(mergeImpliedBranches(proc));
    B3::validate(proc);

    AOT_TEST_CHECK(blocks.first->last()->opcode() == B3::Jump);
    AOT_TEST_CHECK(blocks.first->successorBlock(0) == blocks.second);
    AOT_TEST_CHECK(blocks.second->notTaken().block() == blocks.likely);

    B3::BasicBlock* dispatch = blocks.second->taken().block();
    AOT_TEST_CHECK(blocks.second->taken().frequency() == B3::FrequencyClass::Rare);
    AOT_TEST_CHECK(dispatch->size() == 1 && dispatch->last() == firstBranch);
    AOT_TEST_CHECK(dispatch->taken().block() == blocks.firstTarget);
    AOT_TEST_CHECK(dispatch->notTaken().block() == blocks.secondTarget);
}

static void testMergeImpliedBranchesOfNegatedTest()
{
    B3::Procedure proc;
    TwoTests blocks = appendTwoTests(proc, { });
    // word ? second : firstTarget.
    blocks.first->last()->child(0) = blocks.word;
    blocks.first->setSuccessors(B3::FrequentedBlock(blocks.second), B3::FrequentedBlock(blocks.firstTarget, B3::FrequencyClass::Rare));
    proc.resetReachability();

    AOT_TEST_CHECK(mergeImpliedBranches(proc));
    B3::validate(proc);

    B3::BasicBlock* dispatch = blocks.second->taken().block();
    AOT_TEST_CHECK(dispatch->taken().block() == blocks.secondTarget);
    AOT_TEST_CHECK(dispatch->notTaken().block() == blocks.firstTarget);
}

static void testMergeNeedsSecondTestToFail()
{
    checkTestsAreLeftAlone({ .valueThatLeavesFirst = 100 });
    checkTestsAreLeftAlone({ .valueThatLeavesFirst = -1 });
    checkTestsAreLeftAlone({ .secondRelation = B3::Above });
}

static void testMergeComparesSignedAndUnsigned()
{
    B3::Procedure proc;
    appendTwoTests(proc, { .valueThatLeavesFirst = -1, .secondRelation = B3::LessThan });
    AOT_TEST_CHECK(mergeImpliedBranches(proc));
}

// first: word & mask ? second : firstTarget. second: word secondRelation secondConstant ? secondTarget : likely.
static TwoTests appendTestOfBitsAndTestOfWord(B3::Procedure& proc, int32_t mask, const TwoTestsShape& shape)
{
    TwoTests blocks = appendTwoTests(proc, shape);
    B3::Origin origin = originOf(otherOrigin);
    B3::Value* branch = blocks.first->last();
    blocks.first->values().removeLast();
    B3::Value* bits = blocks.first->appendNew<B3::Value>(proc, B3::BitAnd, origin, blocks.word, blocks.first->appendNew<B3::Const32Value>(proc, origin, mask));
    branch->child(0) = bits;
    blocks.first->append(branch);
    blocks.first->setSuccessors(B3::FrequentedBlock(blocks.second), B3::FrequentedBlock(blocks.firstTarget, B3::FrequencyClass::Rare));
    proc.resetReachability();
    return blocks;
}

static void testMergeTestOfBitsIntoOrderOfWord()
{
    constexpr int32_t highBits = static_cast<int32_t>(0xfffe0000);
    {
        B3::Procedure proc;
        TwoTests blocks = appendTestOfBitsAndTestOfWord(proc, highBits, { .secondRelation = B3::Below, .secondConstant = highBits });
        B3::Value* firstBranch = blocks.first->last();
        AOT_TEST_CHECK(mergeImpliedBranches(proc));
        B3::validate(proc);
        AOT_TEST_CHECK(blocks.first->last()->opcode() == B3::Jump);
        B3::BasicBlock* dispatch = blocks.second->taken().block();
        AOT_TEST_CHECK(dispatch->last() == firstBranch);
        AOT_TEST_CHECK(dispatch->taken().block() == blocks.secondTarget && dispatch->notTaken().block() == blocks.firstTarget);
    }
    auto isLeftAlone = [](int32_t mask, const TwoTestsShape& shape) {
        B3::Procedure proc;
        TwoTests blocks = appendTestOfBitsAndTestOfWord(proc, mask, shape);
        return !mergeImpliedBranches(proc) && blocks.first->last()->opcode() == B3::Branch;
    };
    // The largest word without a bit of the mask is 0x1ffff.
    AOT_TEST_CHECK(!isLeftAlone(highBits, { .secondRelation = B3::Below, .secondConstant = 0x20000 }));
    AOT_TEST_CHECK(isLeftAlone(highBits, { .secondRelation = B3::Below, .secondConstant = 0x1ffff }));
    AOT_TEST_CHECK(!isLeftAlone(highBits, { .secondRelation = B3::BelowEqual, .secondConstant = 0x1ffff }));
    AOT_TEST_CHECK(isLeftAlone(highBits, { .secondRelation = B3::Above, .secondConstant = 0 }));
    AOT_TEST_CHECK(isLeftAlone(highBits, { .secondRelation = B3::NotEqual, .secondConstant = 0x40000 }));
    AOT_TEST_CHECK(isLeftAlone(1, { .secondRelation = B3::Below, .secondConstant = highBits }));
}

static void testMergeIntoBranchWithTwoLikelyWays()
{
    B3::Procedure proc;
    TwoTests blocks = appendTwoTests(proc, { .frequencyOfSecondTarget = B3::FrequencyClass::Normal });
    B3::Value* firstBranch = blocks.first->last();
    AOT_TEST_CHECK(mergeImpliedBranches(proc));
    B3::validate(proc);
    AOT_TEST_CHECK(blocks.first->last()->opcode() == B3::Jump);
    AOT_TEST_CHECK(blocks.second->notTaken().block() == blocks.likely);
    B3::BasicBlock* dispatch = blocks.second->taken().block();
    AOT_TEST_CHECK(blocks.second->taken().frequency() == B3::FrequencyClass::Normal);
    AOT_TEST_CHECK(dispatch->last() == firstBranch);
    AOT_TEST_CHECK(dispatch->taken().block() == blocks.firstTarget && dispatch->taken().frequency() == B3::FrequencyClass::Rare);
    AOT_TEST_CHECK(dispatch->notTaken().block() == blocks.secondTarget);
}

static void testMergeKeepsLikelyBranch()
{
    checkTestsAreLeftAlone({ .frequencyOfFirstTarget = B3::FrequencyClass::Normal });
}

static void testMergeMovesNoLoadInFrontOfTest()
{
    checkTestsAreLeftAlone({ .loadsBetweenTests = true });
}

namespace {

enum class TestBehindTestForNaN : uint8_t { IsWhole, FitsInteger, FitsIntegerAtEitherEnd, IsBelowTen, IsWholeNumberOfAnother };

} // anonymous namespace

// first: number != number ? firstTarget : second. second: the number passes the test ? likely : secondTarget.
static TwoTests appendTestForNaNAndTestOfNumber(B3::Procedure& proc, TestBehindTestForNaN test)
{
    TwoTests blocks { proc.addBlock(), proc.addBlock(), proc.addBlock(), proc.addBlock(), proc.addBlock(), nullptr };
    B3::Origin origin = originOf(otherOrigin);
    B3::Value* number = blocks.first->appendNew<B3::ArgumentRegValue>(proc, origin, FPRInfo::argumentFPR0);
    B3::Value* other = blocks.first->appendNew<B3::ArgumentRegValue>(proc, origin, FPRInfo::argumentFPR1);
    blocks.first->appendNew<B3::Value>(proc, B3::Branch, origin, blocks.first->appendNew<B3::Value>(proc, B3::NotEqual, origin, number, number));
    blocks.first->setSuccessors(B3::FrequentedBlock(blocks.firstTarget, B3::FrequencyClass::Rare), B3::FrequentedBlock(blocks.second));

    B3::PatchpointValue* truncated = blocks.second->appendNew<B3::PatchpointValue>(proc, B3::Int64, origin);
    truncated->append(test == TestBehindTestForNaN::IsWholeNumberOfAnother ? other : number, B3::ValueRep::SomeRegister);
    truncated->effects = B3::Effects::none();
    truncated->pureFunction = Typed::pureFunction(Typed::PureOperation::TruncateToInt64, 0, true);
    auto constant = [&](int64_t value) {
        return blocks.second->appendNew<B3::Const64Value>(proc, origin, value);
    };
    B3::Value* passes = nullptr;
    switch (test) {
    case TestBehindTestForNaN::IsWhole:
        passes = blocks.second->appendNew<B3::Value>(proc, B3::Equal, origin, blocks.second->appendNew<B3::Value>(proc, B3::IToD, origin, truncated), number);
        break;
    case TestBehindTestForNaN::IsWholeNumberOfAnother:
        passes = blocks.second->appendNew<B3::Value>(proc, B3::Equal, origin, blocks.second->appendNew<B3::Value>(proc, B3::IToD, origin, truncated), other);
        break;
    case TestBehindTestForNaN::FitsInteger:
        passes = blocks.second->appendNew<B3::Value>(proc, B3::NotEqual, origin, truncated, constant(std::numeric_limits<int64_t>::min()));
        break;
    case TestBehindTestForNaN::FitsIntegerAtEitherEnd:
        passes = blocks.second->appendNew<B3::Value>(proc, B3::Above, origin, blocks.second->appendNew<B3::Value>(proc, B3::Add, origin, truncated, constant(std::numeric_limits<int64_t>::min() + 1)), constant(1));
        break;
    case TestBehindTestForNaN::IsBelowTen:
        passes = blocks.second->appendNew<B3::Value>(proc, B3::LessThan, origin, truncated, constant(10));
        break;
    }
    blocks.second->appendNew<B3::Value>(proc, B3::Branch, origin, passes);
    blocks.second->setSuccessors(B3::FrequentedBlock(blocks.likely), B3::FrequentedBlock(blocks.secondTarget, B3::FrequencyClass::Rare));

    appendReturn(proc, blocks.likely);
    appendReturn(proc, blocks.firstTarget);
    appendReturn(proc, blocks.secondTarget);
    proc.resetReachability();
    return blocks;
}

static void checkTestForNaNIsAskedBehind(TestBehindTestForNaN test)
{
    B3::Procedure proc;
    TwoTests blocks = appendTestForNaNAndTestOfNumber(proc, test);
    B3::Value* firstBranch = blocks.first->last();
    AOT_TEST_CHECK(mergeImpliedBranches(proc));
    B3::validate(proc);
    AOT_TEST_CHECK(blocks.first->last()->opcode() == B3::Jump && blocks.first->successorBlock(0) == blocks.second);
    AOT_TEST_CHECK(blocks.second->taken().block() == blocks.likely);
    B3::BasicBlock* dispatch = blocks.second->notTaken().block();
    AOT_TEST_CHECK(blocks.second->notTaken().frequency() == B3::FrequencyClass::Rare);
    AOT_TEST_CHECK(dispatch->size() == 1 && dispatch->last() == firstBranch);
    AOT_TEST_CHECK(dispatch->taken().block() == blocks.firstTarget && dispatch->notTaken().block() == blocks.secondTarget);
}

static void checkTestForNaNStays(TestBehindTestForNaN test)
{
    B3::Procedure proc;
    TwoTests blocks = appendTestForNaNAndTestOfNumber(proc, test);
    AOT_TEST_CHECK(!mergeImpliedBranches(proc));
    AOT_TEST_CHECK(blocks.first->last()->opcode() == B3::Branch && blocks.first->taken().block() == blocks.firstTarget);
}

static void testMergeTestForNaNIntoTestOfNumber()
{
    checkTestForNaNIsAskedBehind(TestBehindTestForNaN::IsWhole);
    // What NaN is truncated to differs between the processors.
    if (isX86_64()) {
        checkTestForNaNIsAskedBehind(TestBehindTestForNaN::FitsInteger);
        checkTestForNaNIsAskedBehind(TestBehindTestForNaN::FitsIntegerAtEitherEnd);
    } else {
        checkTestForNaNStays(TestBehindTestForNaN::FitsInteger);
        checkTestForNaNStays(TestBehindTestForNaN::FitsIntegerAtEitherEnd);
    }
    // The least int64 is below ten: NaN would pass.
    checkTestForNaNStays(TestBehindTestForNaN::IsBelowTen);
    checkTestForNaNStays(TestBehindTestForNaN::IsWholeNumberOfAnother);
}

static void checkNaNTestMovesBehindLengthTest(B3::Opcode lengthRelation, bool lengthIsNonNegative, bool expectsMerge)
{
    B3::Procedure proc;
    B3::Origin origin = originOf(otherOrigin);
    B3::BasicBlock* nanTest = proc.addBlock();
    B3::BasicBlock* exactnessTest = proc.addBlock();
    B3::BasicBlock* lengthTest = proc.addBlock();
    B3::BasicBlock* continuation = proc.addBlock();
    B3::BasicBlock* holeCase = proc.addBlock();
    B3::BasicBlock* fractionCase = proc.addBlock();
    B3::BasicBlock* outOfBoundsCase = proc.addBlock();

    B3::Value* number = nanTest->appendNew<B3::ArgumentRegValue>(proc, origin, FPRInfo::argumentFPR0);
    B3::Value* argument = nanTest->appendNew<B3::ArgumentRegValue>(proc, origin, GPRInfo::argumentGPR0);
    B3::Value* length = lengthIsNonNegative ? nanTest->appendNew<B3::Value>(proc, B3::ZExt32, origin, nanTest->appendNew<B3::Value>(proc, B3::Trunc, origin, argument)) : argument;
    nanTest->appendNew<B3::Value>(proc, B3::Branch, origin, nanTest->appendNew<B3::Value>(proc, B3::NotEqual, origin, number, number));
    nanTest->setSuccessors(B3::FrequentedBlock(holeCase, B3::FrequencyClass::Rare), B3::FrequentedBlock(exactnessTest));

    B3::PatchpointValue* truncated = exactnessTest->appendNew<B3::PatchpointValue>(proc, B3::Int64, origin);
    truncated->append(number, B3::ValueRep::SomeRegister);
    truncated->effects = B3::Effects::none();
    truncated->pureFunction = Typed::pureFunction(Typed::PureOperation::TruncateToInt64, 0, true);
    B3::Value* isExact = exactnessTest->appendNew<B3::Value>(proc, B3::EqualOrUnordered, origin, exactnessTest->appendNew<B3::Value>(proc, B3::IToD, origin, truncated), number);
    exactnessTest->appendNew<B3::Value>(proc, B3::Branch, origin, isExact);
    exactnessTest->setSuccessors(B3::FrequentedBlock(lengthTest), B3::FrequentedBlock(fractionCase, B3::FrequencyClass::Rare));

    bool indexIsLeft = lengthRelation == B3::Below;
    B3::Value* isInBounds = lengthTest->appendNew<B3::Value>(proc, lengthRelation, origin, indexIsLeft ? truncated : length, indexIsLeft ? length : truncated);
    lengthTest->appendNew<B3::Value>(proc, B3::Branch, origin, isInBounds);
    lengthTest->setSuccessors(B3::FrequentedBlock(continuation), B3::FrequentedBlock(outOfBoundsCase, B3::FrequencyClass::Rare));

    for (B3::BasicBlock* block : { continuation, holeCase, fractionCase, outOfBoundsCase })
        appendReturn(proc, block);
    proc.resetReachability();

    B3::Value* nanBranch = nanTest->last();
    AOT_TEST_CHECK(mergeImpliedBranches(proc) == expectsMerge);
    B3::validate(proc);
    if (!expectsMerge) {
        AOT_TEST_CHECK(nanTest->last() == nanBranch && nanTest->taken().block() == holeCase);
        return;
    }
    AOT_TEST_CHECK(nanTest->last()->opcode() == B3::Jump && nanTest->successorBlock(0) == exactnessTest);
    AOT_TEST_CHECK(exactnessTest->taken().block() == lengthTest && exactnessTest->notTaken().block() == fractionCase);
    AOT_TEST_CHECK(lengthTest->taken().block() == continuation && lengthTest->notTaken().frequency() == B3::FrequencyClass::Rare);
    B3::BasicBlock* dispatch = lengthTest->notTaken().block();
    AOT_TEST_CHECK(dispatch->size() == 1 && dispatch->last() == nanBranch);
    AOT_TEST_CHECK(dispatch->taken().block() == holeCase && dispatch->notTaken().block() == outOfBoundsCase);
}

static void testMergeTestForNaNIntoLengthTest()
{
    checkNaNTestMovesBehindLengthTest(B3::Below, true, isX86_64());
    checkNaNTestMovesBehindLengthTest(B3::Above, true, isX86_64());
    checkNaNTestMovesBehindLengthTest(B3::Below, false, false);
}

namespace {

enum class AddendKind : uint8_t { Constant, SignExtended, ZeroExtended };

struct IntegerUpdate {
    B3::BasicBlock* first;
    B3::BasicBlock* fastPath;
    B3::BasicBlock* slowPath;
    B3::Value* address;
    B3::Value* oldValue;
    B3::Value* addend;
    B3::Value* sum;
};

struct IntegerUpdateShape {
    int64_t addend { 1 };
    B3::Opcode relation { B3::LessEqual };
    int64_t bound { std::numeric_limits<int32_t>::max() };
    bool storesBetween { false };
    bool returnsSum { false };
    int32_t storeOffset { 16 };
    AddendKind addendKind { AddendKind::Constant };
    std::optional<B3::IntRange> addendRange { };
    std::optional<int64_t> bias { };
    bool subtracts { false };
    bool returnsOldValue { false };
    bool loadsBeforeStore { false };
};

} // anonymous namespace

static B3::Policy policyTrustingKnownRanges()
{
    B3::Policy policy;
    policy.knownProperties.trustsKnownRanges = true;
    return policy;
}

static IntegerUpdate appendIntegerUpdate(B3::Procedure& proc, const IntegerUpdateShape& shape)
{
    IntegerUpdate update { proc.addBlock(), proc.addBlock(), proc.addBlock(), nullptr, nullptr, nullptr, nullptr };
    B3::Origin origin = originOf(otherOrigin);
    update.address = update.first->appendNew<B3::ArgumentRegValue>(proc, origin, GPRInfo::argumentGPR0);
    update.oldValue = update.first->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int32, origin, update.address, 16);
    B3::Value* extended = update.first->appendNew<B3::Value>(proc, B3::SExt32, origin, update.oldValue);
    B3::Value* extendedAddend = nullptr;
    if (shape.addendKind == AddendKind::Constant)
        extendedAddend = update.first->appendNew<B3::Const64Value>(proc, origin, shape.addend);
    else {
        update.addend = update.first->appendNew<B3::Value>(proc, B3::Trunc, origin, update.first->appendNew<B3::ArgumentRegValue>(proc, origin, GPRInfo::argumentGPR1));
        if (shape.addendRange)
            proc.setKnownRange(update.addend, *shape.addendRange);
        extendedAddend = update.first->appendNew<B3::Value>(proc, shape.addendKind == AddendKind::SignExtended ? B3::SExt32 : B3::ZExt32, origin, update.addend);
    }
    update.sum = update.first->appendNew<B3::Value>(proc, shape.subtracts ? B3::Sub : B3::Add, origin, extended, extendedAddend);
    if (shape.storesBetween)
        update.first->appendNew<B3::MemoryValue>(proc, B3::Store, origin, update.oldValue, update.address, 32);
    B3::Value* compared = update.sum;
    if (shape.bias)
        compared = update.first->appendNew<B3::Value>(proc, B3::Add, origin, update.sum, update.first->appendNew<B3::Const64Value>(proc, origin, *shape.bias));
    B3::Value* fits = update.first->appendNew<B3::Value>(proc, shape.relation, origin, compared, update.first->appendNew<B3::Const64Value>(proc, origin, shape.bound));
    update.first->appendNew<B3::Value>(proc, B3::Branch, origin, fits);
    update.first->setSuccessors(B3::FrequentedBlock(update.fastPath), B3::FrequentedBlock(update.slowPath, B3::FrequencyClass::Rare));

    B3::Value* result = shape.returnsSum ? update.sum : shape.returnsOldValue ? extended : update.address;
    if (shape.loadsBeforeStore)
        result = update.fastPath->appendNew<B3::Value>(proc, B3::SExt32, origin, update.fastPath->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int32, origin, update.address, 16));
    update.fastPath->appendNew<B3::MemoryValue>(proc, B3::Store, origin, update.fastPath->appendNew<B3::Value>(proc, B3::Trunc, origin, update.sum), update.address, shape.storeOffset);
    update.fastPath->appendNew<B3::Value>(proc, B3::Return, origin, result);
    update.slowPath->appendNew<B3::Value>(proc, B3::Return, origin, update.sum);
    proc.resetReachability();
    return update;
}

static bool isLoadOfUpdatedInteger(const IntegerUpdate& update, B3::Value* value)
{
    auto* load = value->as<B3::MemoryValue>();
    return load && load->opcode() == B3::Load && load->type() == B3::Int32 && load->lastChild() == update.address && load->offset() == 16;
}

static void checkSumIsRematerialized(const IntegerUpdate& update, const IntegerUpdateShape& shape)
{
    B3::Value* clonedSum = update.slowPath->last()->child(0);
    AOT_TEST_CHECK(clonedSum != update.sum && clonedSum->owner == update.slowPath && clonedSum->opcode() == update.sum->opcode());
    if (shape.addendKind == AddendKind::Constant)
        AOT_TEST_CHECK(clonedSum->child(1)->isInt64(shape.addend));
    else
        AOT_TEST_CHECK(clonedSum->child(1) != update.sum->child(1) && clonedSum->child(1)->owner == update.slowPath && clonedSum->child(1)->opcode() == update.sum->child(1)->opcode() && clonedSum->child(1)->child(0) == update.addend);
    AOT_TEST_CHECK(clonedSum->child(0)->opcode() == B3::SExt32 && isLoadOfUpdatedInteger(update, clonedSum->child(0)->child(0)) && clonedSum->child(0)->child(0)->owner == update.slowPath);
}

static void checkComparesOldValue(const IntegerUpdateShape& shape, int32_t oldValueBound)
{
    B3::Procedure proc(policyTrustingKnownRanges());
    IntegerUpdate update = appendIntegerUpdate(proc, shape);
    AOT_TEST_CHECK(updateIntegersInMemory(proc));
    B3::validate(proc);

    B3::Value* comparison = update.first->last()->child(0);
    AOT_TEST_CHECK(comparison->opcode() == shape.relation && isLoadOfUpdatedInteger(update, comparison->child(0)) && comparison->child(1)->isInt32(oldValueBound));
    B3::Value* store = update.fastPath->at(update.fastPath->size() - 2);
    AOT_TEST_CHECK(store->opcode() == B3::Store && store->child(0)->opcode() == B3::Add && store->child(0)->type() == B3::Int32);
    AOT_TEST_CHECK(isLoadOfUpdatedInteger(update, store->child(0)->child(0)) && store->child(0)->child(0)->owner == update.fastPath && store->child(0)->child(1)->isInt32(static_cast<int32_t>(shape.addend)));
    AOT_TEST_CHECK(store->child(0)->child(0) != comparison->child(0));
    checkSumIsRematerialized(update, shape);
}

static void checkUsesOverflowFlag(const IntegerUpdateShape& shape, std::optional<int32_t> minStoredValue = std::nullopt)
{
    B3::Procedure proc(policyTrustingKnownRanges());
    IntegerUpdate update = appendIntegerUpdate(proc, shape);
    AOT_TEST_CHECK(updateIntegersInMemory(proc));
    B3::validate(proc);

    B3::Value* branch = update.first->last();
    AOT_TEST_CHECK(branch->opcode() == B3::Branch && branch->child(0)->opcode() == B3::NotEqual);
    B3::Value* exactSum = branch->child(0)->child(1);
    B3::Value* wrappedSum = branch->child(0)->child(0)->child(0);
    AOT_TEST_CHECK(branch->child(0)->child(0)->opcode() == B3::SExt32 && wrappedSum->opcode() == B3::Trunc && wrappedSum->child(0) == exactSum);
    AOT_TEST_CHECK(exactSum != update.sum && exactSum->opcode() == update.sum->opcode() && exactSum->owner == update.first);
    AOT_TEST_CHECK(exactSum->child(0)->opcode() == B3::SExt32 && isLoadOfUpdatedInteger(update, exactSum->child(0)->child(0)) && exactSum->child(0)->child(0) != update.oldValue);
    if (update.addend)
        AOT_TEST_CHECK(exactSum->child(1)->opcode() == B3::SExt32 && exactSum->child(1)->child(0) == update.addend);
    else
        AOT_TEST_CHECK(exactSum->child(1)->isInt64(shape.addend));
    auto* store = update.first->at(update.first->size() - 4)->as<B3::MemoryValue>();
    AOT_TEST_CHECK(store && store->opcode() == B3::Store && store->child(0) == wrappedSum && store->lastChild() == update.address && store->offset() == 16);
    for (B3::Value* value : *update.fastPath)
        AOT_TEST_CHECK(value->opcode() != B3::Store);

    B3::BasicBlock* undoBlock = update.first->successorBlock(0);
    AOT_TEST_CHECK(update.first->numSuccessors() == 2 && update.first->successor(0).isRare());
    AOT_TEST_CHECK(undoBlock != update.slowPath && undoBlock->numSuccessors() == 1 && undoBlock->successorBlock(0) == update.slowPath);
    AOT_TEST_CHECK(update.slowPath->numPredecessors() == 1 && update.slowPath->predecessor(0) == undoBlock);
    auto* undoStore = undoBlock->at(undoBlock->size() - 2)->as<B3::MemoryValue>();
    AOT_TEST_CHECK(undoStore && undoStore->opcode() == B3::Store && undoStore->lastChild() == update.address && undoStore->offset() == 16);
    B3::Value* undoneValue = undoStore->child(0);
    AOT_TEST_CHECK(undoneValue->opcode() == (shape.subtracts ? B3::Add : B3::Sub) && undoneValue->type() == B3::Int32 && isLoadOfUpdatedInteger(update, undoneValue->child(0)) && undoneValue->child(0)->owner == undoBlock);
    AOT_TEST_CHECK(update.addend ? undoneValue->child(1) == update.addend : undoneValue->child(1)->isInt32(static_cast<int32_t>(shape.addend)));

    if (minStoredValue) {
        B3::BasicBlock* minCheck = update.first->successorBlock(1);
        AOT_TEST_CHECK(minCheck != update.fastPath && minCheck->numSuccessors() == 2 && minCheck->successorBlock(0) == undoBlock && minCheck->successor(0).isRare() && minCheck->successorBlock(1) == update.fastPath);
        B3::Value* isBelowMin = minCheck->last()->child(0);
        AOT_TEST_CHECK(isBelowMin->opcode() == B3::LessThan && isLoadOfUpdatedInteger(update, isBelowMin->child(0)) && isBelowMin->child(0)->owner == minCheck && isBelowMin->child(1)->isInt32(*minStoredValue));
    } else
        AOT_TEST_CHECK(update.first->successorBlock(1) == update.fastPath);

    checkSumIsRematerialized(update, shape);
    if (shape.returnsOldValue)
        AOT_TEST_CHECK(update.fastPath->last()->child(0)->child(0) == update.oldValue);
}

static void checkIntegerUpdateStays(const IntegerUpdateShape& shape)
{
    B3::Procedure proc(policyTrustingKnownRanges());
    IntegerUpdate update = appendIntegerUpdate(proc, shape);
    AOT_TEST_CHECK(!updateIntegersInMemory(proc));
    AOT_TEST_CHECK(update.first->last()->opcode() == B3::Branch);
}

static void testUpdateIntegersInMemory()
{
    if (!isX86_64()) {
        checkIntegerUpdateStays({ });
        return;
    }
    constexpr int64_t minSlotValue = -2147483643;
    constexpr int64_t slotValueSpan = std::numeric_limits<int32_t>::max() - minSlotValue;
    checkUsesOverflowFlag({ });
    checkUsesOverflowFlag({ .addend = 3 });
    checkUsesOverflowFlag({ .returnsOldValue = true });
    checkUsesOverflowFlag({ .relation = B3::LessThan, .bound = 2147483648 });
    checkUsesOverflowFlag({ .relation = B3::BelowEqual, .bound = slotValueSpan, .addendKind = AddendKind::SignExtended, .bias = -minSlotValue }, static_cast<int32_t>(minSlotValue));
    checkUsesOverflowFlag({ .relation = B3::BelowEqual, .bound = slotValueSpan, .addendKind = AddendKind::SignExtended, .bias = -minSlotValue, .subtracts = true }, static_cast<int32_t>(minSlotValue));
    checkUsesOverflowFlag({ .addendKind = AddendKind::ZeroExtended, .addendRange = B3::IntRange(0, 3) });
    checkUsesOverflowFlag({ .addendKind = AddendKind::SignExtended, .addendRange = B3::IntRange(0, std::numeric_limits<int32_t>::max()) });
    checkComparesOldValue({ .addend = -1, .relation = B3::GreaterEqual, .bound = minSlotValue }, static_cast<int32_t>(minSlotValue + 1));
    checkComparesOldValue({ .loadsBeforeStore = true }, std::numeric_limits<int32_t>::max() - 1);

    checkIntegerUpdateStays({ .addend = -1 });
    checkIntegerUpdateStays({ .relation = B3::BelowEqual });
    checkIntegerUpdateStays({ .bound = 1000, .addendKind = AddendKind::SignExtended, .addendRange = B3::IntRange(0, 3) });
    checkIntegerUpdateStays({ .addendKind = AddendKind::SignExtended });
    checkIntegerUpdateStays({ .addendKind = AddendKind::ZeroExtended });
    checkIntegerUpdateStays({ .addendKind = AddendKind::SignExtended, .addendRange = B3::IntRange(0, 3), .subtracts = true });
    checkIntegerUpdateStays({ .storesBetween = true });
    checkIntegerUpdateStays({ .returnsSum = true });
    checkIntegerUpdateStays({ .storeOffset = 20 });
}

static void checkPhiNarrowing(bool hasWideUser)
{
    B3::Procedure proc;
    B3::Origin origin = originOf(otherOrigin);
    B3::BasicBlock* root = proc.addBlock();
    B3::BasicBlock* extendedCase = proc.addBlock();
    B3::BasicBlock* wideCase = proc.addBlock();
    B3::BasicBlock* constantCase = proc.addBlock();
    B3::BasicBlock* otherCases = proc.addBlock();
    B3::BasicBlock* continuation = proc.addBlock();

    B3::Value* wide = root->appendNew<B3::ArgumentRegValue>(proc, origin, GPRInfo::argumentGPR0);
    B3::Value* word = root->appendNew<B3::Value>(proc, B3::Trunc, origin, root->appendNew<B3::ArgumentRegValue>(proc, origin, GPRInfo::argumentGPR1));
    root->appendNew<B3::Value>(proc, B3::Branch, origin, word);
    root->setSuccessors(B3::FrequentedBlock(extendedCase), B3::FrequentedBlock(otherCases));
    otherCases->appendNew<B3::Value>(proc, B3::Branch, origin, wide);
    otherCases->setSuccessors(B3::FrequentedBlock(wideCase), B3::FrequentedBlock(constantCase));

    B3::UpsilonValue* extendedUpsilon = extendedCase->appendNew<B3::UpsilonValue>(proc, origin, extendedCase->appendNew<B3::Value>(proc, B3::SExt32, origin, word));
    B3::UpsilonValue* wideUpsilon = wideCase->appendNew<B3::UpsilonValue>(proc, origin, wide);
    B3::UpsilonValue* constantUpsilon = constantCase->appendNew<B3::UpsilonValue>(proc, origin, constantCase->appendNew<B3::Const64Value>(proc, origin, (1ll << 40) + 7));
    for (B3::BasicBlock* block : { extendedCase, wideCase, constantCase }) {
        block->appendNew<B3::Value>(proc, B3::Jump, origin);
        block->setSuccessors(B3::FrequentedBlock(continuation));
    }
    B3::Value* phi = continuation->appendNew<B3::Value>(proc, B3::Phi, B3::Int64, origin);
    for (B3::UpsilonValue* upsilon : { extendedUpsilon, wideUpsilon, constantUpsilon })
        upsilon->setPhi(phi);
    B3::Value* result = continuation->appendNew<B3::Value>(proc, B3::ZExt32, origin, continuation->appendNew<B3::Value>(proc, B3::Trunc, origin, phi));
    if (hasWideUser)
        result = continuation->appendNew<B3::Value>(proc, B3::Add, origin, result, phi);
    continuation->appendNew<B3::Value>(proc, B3::Return, origin, result);
    proc.resetReachability();

    AOT_TEST_CHECK(narrowTruncatedPhis(proc) == !hasWideUser);
    B3::validate(proc);
    if (hasWideUser) {
        AOT_TEST_CHECK(extendedUpsilon->opcode() == B3::Upsilon && extendedUpsilon->phi() == phi);
        return;
    }
    B3::Value* narrowPhi = result->child(0);
    AOT_TEST_CHECK(narrowPhi->opcode() == B3::Phi && narrowPhi->type() == B3::Int32 && narrowPhi->owner == continuation);
    auto inputFrom = [&](B3::BasicBlock* block) {
        B3::UpsilonValue* upsilon = nullptr;
        for (B3::Value* value : *block) {
            if (value->opcode() != B3::Upsilon)
                continue;
            AOT_TEST_CHECK(!upsilon);
            upsilon = value->as<B3::UpsilonValue>();
        }
        AOT_TEST_CHECK(upsilon && upsilon->phi() == narrowPhi);
        return upsilon->child(0);
    };
    AOT_TEST_CHECK(inputFrom(extendedCase) == word);
    AOT_TEST_CHECK(inputFrom(wideCase)->opcode() == B3::Trunc && inputFrom(wideCase)->child(0) == wide);
    AOT_TEST_CHECK(inputFrom(constantCase)->isInt32(7));
}

static void testNarrowTruncatedPhis()
{
    checkPhiNarrowing(false);
    checkPhiNarrowing(true);
}

std::span<const NativeTest> backEndPhaseTests()
{
    static constexpr NativeTest tests[] = {
        { "materialize-slot-bases"_s, testMaterializeSlotBases },
        { "materialize-one-copy-per-use"_s, testMaterializeOneCopyPerUse },
        { "materialize-derived-addresses"_s, testMaterializeDerivedAddresses },
        { "materialize-nothing"_s, testMaterializeNothing },
        { "materialize-unused-slot-base"_s, testMaterializeUnusedSlotBase },
        { "materialize-splits-shared-slot-base"_s, testMaterializeSplitsSharedSlotBase },
        { "pure-function-at-suspension-is-no-suspension"_s, testPureFunctionAtSuspensionIsNoSuspension },
        { "materialize-phi-of-one-slot"_s, testMaterializePhiOfOneSlot },
        { "materialize-phi-of-one-distance-into-slot"_s, testMaterializePhiOfOneDistanceIntoSlot },
        { "materialize-phi-that-goes-around-loop"_s, testMaterializePhiThatGoesAroundLoop },
        { "materialize-does-not-split-phi-of-two-slots"_s, testMaterializeDoesNotSplitPhiOfTwoSlots },
        { "materialize-does-not-split-phi-of-slot-and-other-value"_s, testMaterializeDoesNotSplitPhiOfSlotAndOtherValue },
        { "materialize-splits-shared-frame-pointer"_s, testMaterializeSplitsSharedFramePointer },
        { "materialize-runs-after-optimization"_s, testPhaseRunsAfterOptimization },
        { "materialize-in-pipeline"_s, testMaterializeInPipeline },
        { "merge-implied-branches"_s, testMergeImpliedBranches },
        { "merge-implied-branches-of-negated-test"_s, testMergeImpliedBranchesOfNegatedTest },
        { "merge-needs-second-test-to-fail"_s, testMergeNeedsSecondTestToFail },
        { "merge-compares-signed-and-unsigned"_s, testMergeComparesSignedAndUnsigned },
        { "merge-test-of-bits-into-order-of-word"_s, testMergeTestOfBitsIntoOrderOfWord },
        { "merge-into-branch-with-two-likely-ways"_s, testMergeIntoBranchWithTwoLikelyWays },
        { "merge-keeps-likely-branch"_s, testMergeKeepsLikelyBranch },
        { "merge-moves-no-load-in-front-of-test"_s, testMergeMovesNoLoadInFrontOfTest },
        { "merge-test-for-nan-into-test-of-number"_s, testMergeTestForNaNIntoTestOfNumber },
        { "merge-test-for-nan-into-length-test"_s, testMergeTestForNaNIntoLengthTest },
        { "update-integers-in-memory"_s, testUpdateIntegersInMemory },
        { "narrow-truncated-phis"_s, testNarrowTruncatedPhis },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
