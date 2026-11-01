/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3PromoteLoopMemory.h"

namespace {

Policy promotingPolicy()
{
    Policy policy = loopPolicy();
    policy.loops.promotesMemory = true;
    return policy;
}

bool promoteLoopMemoryAndValidate(Procedure& proc)
{
    bool result = promoteLoopMemory(proc);
    fixSSA(proc);
    validate(proc);
    return result;
}

// for (index = 0; index < bound; index++), rotated: tested once before the loop and then at the end of each iteration.
struct MemoryLoop {
    explicit MemoryLoop(const Policy& policy = promotingPolicy())
        : proc(policy)
        , loop(graph)
    {
    }

    BasicBlock* body() { return loop.body; }
    Value* index() { return graph.get(loop.body, loop.index); }
    Value* slot(int32_t offset = 0, HeapRange range = firstHeap) { return graph.load(loop.body, Int64, loop.pointer, offset, range); }
    void setSlot(Value* value, int32_t offset = 0, HeapRange range = firstHeap) { graph.store(loop.body, value, loop.pointer, offset, range); }
    void addIndexToSlot() { setSlot(graph.make(loop.body, Add, slot(), index())); }

    Value* loopVariantSlotAddress()
    {
        Value* chosen = graph.make(loop.body, BitAnd, index(), graph.int64(loop.body, 3));
        return graph.make(loop.body, Add, loop.pointer, graph.make(loop.body, Shl, chosen, graph.int32(loop.body, 3)));
    }

    void finish(BasicBlock* last = nullptr)
    {
        loop.endBody(last ? last : loop.body);
        loop.rotate();
    }

    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop;
};

void checkAddsIndexesToSlot(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 10, 1000 }) {
        int64_t slots[4] = { 5, 6, 7, 8 };
        run(*code, bound, slots);
        CHECK_EQ(slots[0], 5 + sumBelow(bound));
        CHECK_EQ(slots[1], 6);
    }
}

template<typename Functor>
void checkIsPromoted(const Functor& fillBody, unsigned loadsRemainingInLoop = 0)
{
    MemoryLoop loop;
    loop.addIndexToSlot();
    fillBody(loop);
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), loadsRemainingInLoop);
    CHECK_EQ(loop.proc.report().countOf(Counter::LocationsPromotedInLoops), 1u);
}

template<typename Functor>
void checkIsNotPromoted(const Functor& fillBody)
{
    MemoryLoop loop;
    loop.addIndexToSlot();
    fillBody(loop);
    loop.finish();
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    CHECK(!loop.proc.report().countOf(Counter::LocationsPromotedInLoops));
}

PatchpointValue* appendClobberingPatchpoint(MemoryLoop& loop, HeapRange range)
{
    PatchpointValue* result = loop.graph.emptyPatchpoint(loop.body());
    result->effects.writes = range;
    return result;
}

// A loop that loads and stores four slots. The first two each have their own heap range, the last two share one, and a loop-variant address may refer to any of them.
struct RandomMemoryLoop {
    static constexpr unsigned numberOfSlots = 4;

    RandomMemoryLoop(unsigned seed, bool promotes)
        : proc(policyFor(promotes))
        , loop(graph)
        , random(seed)
    {
        BasicBlock* block = loop.body;
        for (unsigned count = 1 + random.getUint32(5); count--;)
            appendStatement(block, 0);
        loop.endBody(block);
    }

    static constexpr int64_t reported = 0x7e57ed0ddba11ll;

    // With the pass enabled, every load that a phase moved or replaced is reloaded at its original position and compared.
    static Policy policyFor(bool promotes)
    {
        Policy policy = optimizingPolicy();
        policy.loops.promotesMemory = promotes;
        if (!promotes)
            return policy;
        policy.validation.logsMovedLoads = true;
        policy.clientPhases.append(ClientPhase {
            ClientPhase::Point::AfterSinking,
            "checkMovedLoads"_s,
            createSharedTask<bool(Procedure&)>([](Procedure& proc) {
                return proc.movedLoadLog().insertChecks(proc, createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
                    jit.move(CCallHelpers::TrustedImm64(reported), GPRInfo::returnValueGPR);
                    jit.emitFunctionEpilogue();
                    jit.ret();
                }));
            })
        });
        return policy;
    }

    static HeapRange trueRangeOf(unsigned slot) { return !slot ? firstHeap : slot == 1 ? secondHeap : thirdHeap; }
    // The slot's own heap range, or a wider range that contains it.
    HeapRange rangeFor(unsigned slot)
    {
        if (isInDeadArm)
            return trueRangeOf(random.getUint32(numberOfSlots));
        if (!chance(35))
            return trueRangeOf(slot);
        return slot < 2 && chance(50) ? bothHeaps : allHeaps;
    }
    // An alias of the base address that the compiler cannot see through.
    Value* base(BasicBlock* block)
    {
        if (!chance(25))
            return loop.pointer;
        return graph.make(block, Add, loop.pointer, graph.make(block, BitAnd, loop.extra, graph.int64(block, 1ll << 62)));
    }
    bool chance(unsigned percent) { return random.getUint32(100) < percent; }
    Value* index(BasicBlock* block) { return graph.get(block, loop.index); }

    Value* loopVariantAddress(BasicBlock* block)
    {
        Value* chosen = graph.make(block, BitAnd, graph.make(block, Add, index(block), graph.int64(block, random.getUint32(numberOfSlots))), graph.int64(block, numberOfSlots - 1));
        return graph.make(block, Add, loop.pointer, graph.make(block, Shl, chosen, graph.int32(block, 3)));
    }

    Value* read(BasicBlock* block)
    {
        unsigned slot = random.getUint32(numberOfSlots);
        return graph.load(block, Int64, base(block), slot * 8, rangeFor(slot));
    }

    Value* someValue(BasicBlock* block)
    {
        switch (random.getUint32(6)) {
        case 0:
            return index(block);
        case 1:
            return graph.get(block, loop.sum);
        case 2:
            return loop.extra;
        case 3:
            return graph.int64(block, random.getUint32(1000));
        default:
            return read(block);
        }
    }

    void appendStatement(BasicBlock*& block, unsigned depth)
    {
        unsigned kind = random.getUint32(100);
        if (kind < 40) {
            unsigned slot = random.getUint32(numberOfSlots);
            B3::Opcode opcode = chance(50) ? Add : chance(50) ? BitXor : Sub;
            Value* address = base(block);
            graph.store(block, graph.make(block, opcode, graph.load(block, Int64, address, slot * 8, rangeFor(slot)), someValue(block)), address, slot * 8, rangeFor(slot));
            return;
        }
        if (kind < 52) {
            graph.increase(block, loop.sum, read(block));
            return;
        }
        if (kind < 60) {
            graph.increase(block, loop.sum, graph.load(block, Int64, loopVariantAddress(block), 0, allHeaps));
            return;
        }
        if (kind < 64) {
            unsigned slot = random.getUint32(numberOfSlots);
            graph.store(block, someValue(block), base(block), slot * 8, rangeFor(slot));
            return;
        }
        if (kind < 67) {
            graph.store(block, someValue(block), loopVariantAddress(block), 0, allHeaps);
            return;
        }
        if (kind < 71) {
            unsigned slot = random.getUint32(numberOfSlots);
            graph.increase(block, loop.sum, graph.make(block, ZExt32, graph.load(block, Int32, loop.pointer, slot * 8 + 4 * random.getUint32(2), trueRangeOf(slot))));
            return;
        }
        if (kind < 73) {
            unsigned slot = random.getUint32(numberOfSlots);
            block->appendNew<MemoryValue>(proc, chance(50) ? Store8 : Store16, Origin(), graph.make(block, Trunc, someValue(block)), loop.pointer, static_cast<int32_t>(slot * 8 + 2 * random.getUint32(4)), trueRangeOf(slot));
            return;
        }
        if (kind < 76) {
            appendPoll(graph, block, pollState);
            return;
        }
        if (kind < 80 && !isInDeadArm) {
            // One slot accessed through two aliases. It is loaded and stored under a range wider than its own, stored through the alias under its own range,
            // and loaded under a wrong range behind a branch that is never taken.
            unsigned slot = random.getUint32(2);
            BasicBlock* never = proc.addBlock();
            BasicBlock* join = proc.addBlock();
            bool branchComesFirst = chance(50);
            auto appendTest = [&] {
                graph.branch(block, graph.make(block, Equal, loop.extra, graph.int64(block, -1)), never, join);
                graph.increase(never, loop.sum, graph.load(never, Int64, loop.pointer, slot * 8, trueRangeOf(1 - slot)));
                graph.jump(never, join);
                block = join;
            };
            if (branchComesFirst)
                appendTest();
            graph.store(block, graph.make(block, Add, graph.load(block, Int64, loop.pointer, slot * 8, bothHeaps), index(block)), loop.pointer, slot * 8, bothHeaps);
            Value* aliasedBase = graph.make(block, Add, loop.pointer, graph.make(block, BitAnd, loop.extra, graph.int64(block, 1ll << 62)));
            graph.store(block, someValue(block), aliasedBase, slot * 8, trueRangeOf(slot));
            if (!branchComesFirst)
                appendTest();
            graph.increase(block, loop.sum, graph.load(block, Int64, loop.pointer, slot * 8, bothHeaps));
            return;
        }
        if (depth >= 2) {
            graph.increase(block, loop.sum, someValue(block));
            return;
        }
        if (kind < 90) {
            BasicBlock* arm = proc.addBlock();
            BasicBlock* join = proc.addBlock();
            // Dead code may use any heap range, since nothing it claims has to be accurate.
            bool isDead = !isInDeadArm && chance(40);
            Value* condition = isDead ? graph.make(block, Equal, loop.extra, graph.int64(block, -1)) : graph.make(block, BitAnd, index(block), graph.int64(block, 1 + random.getUint32(3)));
            graph.branch(block, condition, arm, join);
            isInDeadArm |= isDead;
            for (unsigned count = 1 + random.getUint32(2); count--;)
                appendStatement(arm, depth + 1);
            if (isDead)
                isInDeadArm = false;
            graph.jump(arm, join);
            block = join;
            return;
        }
        BasicBlock* inner = proc.addBlock();
        BasicBlock* after = proc.addBlock();
        Variable* iterationsLeft = proc.addVariable(Int64);
        graph.set(block, iterationsLeft, graph.make(block, BitAnd, index(block), graph.int64(block, 3)));
        graph.jump(block, inner);
        BasicBlock* lastOfInner = inner;
        for (unsigned count = 1 + random.getUint32(2); count--;)
            appendStatement(lastOfInner, depth + 1);
        graph.increase(lastOfInner, iterationsLeft, -1);
        graph.branch(lastOfInner, graph.make(lastOfInner, GreaterEqual, graph.get(lastOfInner, iterationsLeft), graph.int64(lastOfInner, 0)), inner, after);
        block = after;
    }

    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop;
    WeakRandom random;
    bool isInDeadArm { false };
    PollState pollState;
};

} // anonymous namespace

static void testPromoteLoopMemoryPromotesLocation()
{
    MemoryLoop loop;
    loop.addIndexToSlot();
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
    CHECK_EQ(valueCountInLoops(loop.proc, Store), 1u);
    CHECK_EQ(numberOfValues(loop.proc, Load), 1u);
    CHECK_EQ(loop.proc.report().countOf(Counter::LocationsPromotedInLoops), 1u);
    checkAddsIndexesToSlot(loop.proc);
}

// A value placed by an InsertionSet has no owner until the owners are reset, and a value that a phase moved has a stale one.
static void testPromoteLoopMemoryResetsValueOwners()
{
    for (bool ownersAreStale : { false, true }) {
        MemoryLoop loop;
        loop.addIndexToSlot();
        loop.finish();
        for (Value* value : loop.proc.values())
            value->owner = ownersAreStale ? loop.proc[loop.proc.size() - 1] : nullptr;
        CHECK(promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
        CHECK_EQ(loop.proc.report().countOf(Counter::LocationsPromotedInLoops), 1u);
        checkAddsIndexesToSlot(loop.proc);
    }
}

static void testPromoteLoopMemoryPromotesEveryNumericType()
{
    for (Type type : { Int32, Int64, Float, Double }) {
        MemoryLoop loop;
        Graph& graph = loop.graph;
        BasicBlock* body = loop.body();
        Value* one = type == Int32 ? graph.int32(body, 1) : type == Int64 ? graph.int64(body, 1) : type == Double ? graph.number(body, 1) : body->appendNew<ConstFloatValue>(loop.proc, Origin(), 1.0f);
        graph.store(body, graph.make(body, Add, graph.load(body, type, loop.loop.pointer, 0, firstHeap), one), loop.loop.pointer, 0, firstHeap);
        loop.finish();
        CHECK(promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
        auto code = compileProc(loop.proc);
        int64_t slots[2] = { 0, 9 };
        run(*code, 100, slots);
        if (type == Int32)
            CHECK_EQ(slots[0], 100);
        else if (type == Int64)
            CHECK_EQ(slots[0], 100);
        else if (type == Double)
            CHECK_EQ(std::bit_cast<double>(slots[0]), 100.0);
        else
            CHECK_EQ(std::bit_cast<float>(static_cast<int32_t>(slots[0])), 100.0f);
        CHECK_EQ(slots[1], 9);
    }
}

// The stores are kept, so any other load of the slot still sees the right value in memory.
static void testPromoteLoopMemoryPromotesLocationAlsoLoadedThroughOtherAddresses()
{
    MemoryLoop loop;
    Graph& graph = loop.graph;
    Value* other = graph.load(loop.body(), Int64, loop.loopVariantSlotAddress(), 0, firstHeap);
    loop.setSlot(graph.make(loop.body(), Add, loop.slot(), other));
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 1u);
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 2, 5, 9, 40 }) {
        int64_t slots[4] = { 1, 2, 3, 4 };
        int64_t expected[4] = { 1, 2, 3, 4 };
        for (int64_t index = 0; index < bound; ++index)
            expected[0] += expected[index & 3];
        run(*code, bound, slots);
        for (unsigned i = 0; i < 4; ++i)
            CHECK_EQ(slots[i], expected[i]);
    }
}

static void testPromoteLoopMemorySkipsClobberedLocation()
{
    // The neighboring slot, in the same heap range.
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.setSlot(loop.index(), 8);
    });
    // A loop-variant address.
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.loopVariantSlotAddress(), 0, firstHeap);
    });
    // A different loop-invariant address.
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.loop.extra, 0, firstHeap);
    });
    // A wider heap range that contains this one.
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.loop.extra, 0, bothHeaps);
    });
    // Part of the slot.
    for (B3::Opcode opcode : { Store8, Store16 }) {
        checkIsNotPromoted([&](MemoryLoop& loop) {
            loop.body()->appendNew<MemoryValue>(loop.proc, opcode, Origin(), loop.graph.make(loop.body(), Trunc, loop.index()), loop.loop.pointer, 0, firstHeap);
        });
    }
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.graph.store(loop.body(), loop.graph.make(loop.body(), Trunc, loop.index()), loop.loop.pointer, 0, firstHeap);
    });
    // The same bits accessed as a different type.
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.graph.store(loop.body(), loop.graph.make(loop.body(), IToD, loop.index()), loop.loop.pointer, 0, firstHeap);
    });
    // A write that is not a store.
    checkIsNotPromoted([](MemoryLoop& loop) {
        appendClobberingPatchpoint(loop, firstHeap);
    });
    checkIsNotPromoted([](MemoryLoop& loop) {
        appendClobberingPatchpoint(loop, HeapRange::top());
    });

    // A disjoint heap range cannot alias.
    checkIsPromoted([](MemoryLoop& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.loopVariantSlotAddress(), 0, secondHeap);
        appendClobberingPatchpoint(loop, thirdHeap);
    });
}

static void testPromoteLoopMemoryKeepsLoadOfDifferentWidth()
{
    checkIsPromoted([](MemoryLoop& loop) {
        Value* half = loop.graph.load(loop.body(), Int32, loop.loop.pointer, 0, firstHeap);
        loop.graph.increase(loop.body(), loop.loop.sum, loop.graph.make(loop.body(), ZExt32, half));
    }, 1);
}

// A narrow store drops the upper bits and a narrow load extends the result, so the stored value is not the value loaded back.
static void testPromoteLoopMemorySkipsNarrowAccesses()
{
    struct Part {
        B3::Opcode load;
        B3::Opcode store;
    };
    for (Part part : { Part { Load8Z, Store8 }, Part { Load8S, Store8 }, Part { Load16Z, Store16 }, Part { Load16S, Store16 } }) {
        MemoryLoop loop;
        Graph& graph = loop.graph;
        BasicBlock* body = loop.body();
        Value* read = body->appendNew<MemoryValue>(loop.proc, part.load, Origin(), loop.loop.pointer, 0, firstHeap);
        graph.increase(body, loop.loop.sum, graph.make(body, SExt32, read));
        body->appendNew<MemoryValue>(loop.proc, part.store, Origin(), graph.make(body, Add, read, graph.int32(body, 77)), loop.loop.pointer, 0, firstHeap);
        loop.finish();
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    }
}

static void testPromoteLoopMemorySkipsAccessesThatTrap()
{
    for (bool loadTraps : { false, true }) {
        MemoryLoop loop;
        Graph& graph = loop.graph;
        BasicBlock* body = loop.body();
        Value* read = body->appendNew<MemoryValue>(loop.proc, loadTraps ? trapping(Load) : B3::Kind(Load), Int64, Origin(), loop.loop.pointer, 0, firstHeap);
        body->appendNew<MemoryValue>(loop.proc, loadTraps ? B3::Kind(Store) : trapping(Store), Origin(), graph.make(body, Add, read, loop.index()), loop.loop.pointer, 0, firstHeap);
        loop.finish();
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    }
}

static void testPromoteLoopMemorySkipsLoopVariantAddress()
{
    MemoryLoop loop;
    Graph& graph = loop.graph;
    Value* address = loop.loopVariantSlotAddress();
    graph.store(loop.body(), graph.make(loop.body(), Add, graph.load(loop.body(), Int64, address, 0, firstHeap), loop.index()), address, 0, firstHeap);
    loop.finish();
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
}

// A write to a range disjoint from a load's range cannot be observed by that load. The range of a store to the location proves nothing about that.
static void testPromoteLoopMemoryChecksAliasingPerLoadRange()
{
    enum class Other : uint8_t { None, InSecondHeap, InFirstHeap };
    for (bool storeNamesMore : { false, true }) {
        for (Other other : { Other::None, Other::InSecondHeap, Other::InFirstHeap }) {
            MemoryLoop loop;
            loop.setSlot(loop.graph.make(loop.body(), Add, loop.slot(0, storeNamesMore ? firstHeap : bothHeaps), loop.index()), 0, storeNamesMore ? bothHeaps : firstHeap);
            if (other != Other::None)
                loop.graph.store(loop.body(), loop.index(), loop.loopVariantSlotAddress(), 8, other == Other::InFirstHeap ? firstHeap : secondHeap);
            loop.finish();
            CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), other == Other::None || (storeNamesMore && other == Other::InSecondHeap));
        }
    }
}

// The range of an access that never executes proves nothing about the location.
static void testPromoteLoopMemoryIgnoresRangeOfAccessThatNeverExecutes()
{
    MemoryLoop loop;
    Graph& graph = loop.graph;
    BasicBlock* never = loop.proc.addBlock();
    BasicBlock* next = loop.proc.addBlock();
    loop.setSlot(graph.make(loop.body(), Add, loop.slot(0, bothHeaps), loop.index()), 0, bothHeaps);
    graph.store(loop.body(), graph.int64(loop.body(), 100), loop.loop.extra, 0, secondHeap);
    graph.branch(loop.body(), graph.make(loop.body(), LessThan, loop.index(), graph.int64(loop.body(), 0)), never, next);
    graph.increase(never, loop.loop.sum, graph.load(never, Int64, loop.loop.pointer, 0, firstHeap));
    graph.jump(never, next);
    graph.increase(next, loop.loop.sum, graph.load(next, Int64, loop.loop.pointer, 0, bothHeaps));
    loop.finish(next);
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 2, 5 }) {
        int64_t slots[1] = { 5 };
        CHECK_EQ(run(*code, bound, slots, std::bit_cast<int64_t>(&slots[0])), 100 * bound);
    }
}

// The preheader load executes whether or not the load it was cloned from does, so it uses the union of the ranges of all accesses to the location.
static void testPromoteLoopMemoryPreheaderLoadUsesUnionOfRanges()
{
    MemoryLoop loop;
    loop.setSlot(loop.graph.make(loop.body(), Add, loop.slot(0, firstHeap), loop.index()), 0, bothHeaps);
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    unsigned numberOfLoads = 0;
    for (Value* value : loop.proc.values()) {
        if (value->opcode() != Load)
            continue;
        ++numberOfLoads;
        CHECK(value->as<MemoryValue>()->range() == bothHeaps);
    }
    CHECK_EQ(numberOfLoads, 1u);
}

static void testPromoteLoopMemorySkipsImmutableLoads()
{
    MemoryLoop loop;
    MemoryValue* read = loop.graph.load(loop.body(), Int64, loop.loop.pointer, 0, firstHeap);
    read->setReadsMutability(Mutability::Immutable);
    loop.setSlot(loop.graph.make(loop.body(), Add, read, loop.index()));
    loop.finish();
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
}

static void testPromoteLoopMemorySkipsFencesAndAtomics()
{
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.body()->appendNew<FenceValue>(loop.proc, Origin());
    });
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.body()->appendNew<AtomicValue>(loop.proc, AtomicXchgAdd, Origin(), Width64, loop.index(), loop.loop.extra, 0, secondHeap);
    });
    checkIsNotPromoted([](MemoryLoop& loop) {
        loop.body()->appendNew<MemoryValue>(loop.proc, Load, Int64, Origin(), loop.loop.extra, 0, secondHeap, secondHeap);
    });
}

// If the loop may run zero iterations, a preheader load would access memory that the program never accesses.
static void testPromoteLoopMemorySkipsAccessNotGuaranteedToExecute()
{
    {
        MemoryLoop loop;
        loop.addIndexToSlot();
        loop.loop.endBody(loop.body());
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
        auto code = compileProc(loop.proc);
        CHECK_EQ(run(*code, 0, nullptr), 0);
    }
    {
        MemoryLoop loop;
        Graph& graph = loop.graph;
        BasicBlock* conditionalAccess = loop.proc.addBlock();
        BasicBlock* next = loop.proc.addBlock();
        graph.branch(loop.body(), loop.loop.extra, conditionalAccess, next);
        graph.store(conditionalAccess, graph.make(conditionalAccess, Add, graph.load(conditionalAccess, Int64, loop.loop.pointer, 0, firstHeap), graph.get(conditionalAccess, loop.loop.index)), loop.loop.pointer, 0, firstHeap);
        graph.jump(conditionalAccess, next);
        loop.finish(next);
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
        auto code = compileProc(loop.proc);
        CHECK_EQ(run(*code, 10, nullptr, 0), 0);
        int64_t slots[1] = { 5 };
        run(*code, 10, slots, 1);
        CHECK_EQ(slots[0], 5 + sumBelow(10));
    }
}

static void testPromoteLoopMemorySkipsLoopWithSidewaysExit()
{
    {
        MemoryLoop loop;
        PatchpointValue* exit = loop.graph.emptyPatchpoint(loop.body());
        exit->effects.exitsSideways = true;
        exit->effects.reads = HeapRange::top();
        loop.addIndexToSlot();
        loop.finish();
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    }
    {
        PollState state;
        MemoryLoop loop;
        appendPoll(loop.graph, loop.body(), state)->effects.exitsSideways = true;
        loop.addIndexToSlot();
        loop.finish();
        CHECK(promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
        CHECK_EQ(valueCountInLoops(loop.proc, Store), 1u);
        checkAddsIndexesToSlot(loop.proc);
    }
}

static void testPromoteLoopMemoryPromotesLocationStoredOnOnePath()
{
    MemoryLoop loop;
    Graph& graph = loop.graph;
    BasicBlock* stores = loop.proc.addBlock();
    BasicBlock* next = loop.proc.addBlock();
    Value* read = loop.slot();
    graph.increase(loop.body(), loop.loop.sum, read);
    graph.branch(loop.body(), graph.make(loop.body(), BitAnd, loop.index(), graph.int64(loop.body(), 1)), stores, next);
    graph.store(stores, graph.make(stores, Add, read, graph.get(stores, loop.loop.index)), loop.loop.pointer, 0, firstHeap);
    graph.jump(stores, next);
    loop.finish(next);
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 2, 3, 10, 101 }) {
        int64_t slots[1] = { 5 };
        int64_t slot = 5;
        int64_t sum = 0;
        for (int64_t index = 0; index < bound; ++index) {
            sum += slot;
            if (index & 1)
                slot += index;
        }
        CHECK_EQ(run(*code, bound, slots), sum);
        CHECK_EQ(slots[0], slot);
    }
}

static void testPromoteLoopMemoryHoistsLoadToOutermostLoop()
{
    Procedure proc { promotingPolicy() };
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outer = proc.addBlock();
    BasicBlock* inner = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* innerBound = arguments[0];
    Value* pointer = arguments[1];
    Value* outerBound = arguments[2];
    Variable* outerIndex = proc.addVariable(Int64);
    Variable* innerIndex = proc.addVariable(Int64);
    graph.set(root, outerIndex, graph.int64(root, 0));
    graph.jump(root, outer);
    graph.set(outer, innerIndex, graph.int64(outer, 0));
    graph.jump(outer, inner);
    graph.store(inner, graph.make(inner, Add, graph.load(inner, Int64, pointer, 0, firstHeap), graph.get(inner, innerIndex)), pointer, 0, firstHeap);
    graph.increase(inner, innerIndex, 1);
    graph.branch(inner, graph.make(inner, LessThan, graph.get(inner, innerIndex), innerBound), inner, latch);
    graph.increase(latch, outerIndex, 1);
    graph.branch(latch, graph.make(latch, LessThan, graph.get(latch, outerIndex), outerBound), outer, exit);
    graph.ret(exit, graph.int64(exit, 0));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(promoteLoopMemoryAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LocationsPromotedInLoops), 1u);
    CHECK_EQ(valueCountInLoops(proc, Load), 0u);
    CHECK_EQ(numberOfValues(root, Load), 1u);
    auto code = compileProc(proc);
    int64_t slots[1] = { 5 };
    run(*code, 10, slots, 7);
    CHECK_EQ(slots[0], 5 + 7 * sumBelow(10));
}

// Fields and variables each have their own heap range. A loop that stores to many of them must not compare every store with every load.
static void testPromoteLoopMemoryVisitsEachAccessFewTimes()
{
    for (unsigned numberOfSlots : { 100u, 1000u, 10000u }) {
        MemoryLoop loop;
        for (unsigned slot = 0; slot < numberOfSlots; ++slot) {
            HeapRange heap { 100 + slot, 101 + slot };
            loop.setSlot(loop.graph.make(loop.body(), Add, loop.slot(slot * 8, heap), loop.index()), slot * 8, heap);
        }
        loop.finish();
        CHECK(promoteLoopMemory(loop.proc));
        CHECK_EQ(loop.proc.report().countOf(Counter::LocationsPromotedInLoops), numberOfSlots);
        CHECK(loop.proc.report().countOf(Counter::MemoryPromotionAccessesVisited) <= 4 * numberOfSlots);
    }
}

// Each reason for not promoting a location has a counter, so a build can report what the pass found.
static void testPromoteLoopMemoryCountsReasonsNotPromoted()
{
    auto check = [](Counter reason, const auto& fillBody) {
        MemoryLoop loop;
        fillBody(loop);
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(loop.proc.report().countOf(Counter::LoopsConsideredForMemoryPromotion), 1u);
        CHECK_EQ(loop.proc.report().countOf(Counter::LocationsLoadedAndStoredInLoops), 1u);
        CHECK_EQ(loop.proc.report().countOf(reason), 1u);
    };
    check(Counter::NotPromotedClobberedOnLikelyPaths, [](MemoryLoop& loop) {
        loop.addIndexToSlot();
        appendClobberingPatchpoint(loop, firstHeap);
        loop.finish();
    });
    check(Counter::NotPromotedClobberedOnRarePaths, [](MemoryLoop& loop) {
        loop.addIndexToSlot();
        BasicBlock* rare = loop.proc.addBlock();
        BasicBlock* next = loop.proc.addBlock();
        loop.graph.branch(loop.body(), loop.loop.extra, rarely(rare), FrequentedBlock(next));
        loop.graph.emptyPatchpoint(rare)->effects.writes = firstHeap;
        loop.graph.jump(rare, next);
        loop.finish(next);
    });
    check(Counter::NotPromotedMayAliasStores, [](MemoryLoop& loop) {
        loop.addIndexToSlot();
        loop.graph.store(loop.body(), loop.index(), loop.loop.extra, 0, firstHeap);
        loop.finish();
    });
    auto addIndexToSlotAt = [](MemoryLoop& loop, Value* address) {
        loop.graph.store(loop.body(), loop.graph.make(loop.body(), Add, loop.graph.load(loop.body(), Int64, address, 0, firstHeap), loop.index()), address, 0, firstHeap);
        loop.finish();
    };
    check(Counter::NotPromotedLoopVariantAddress, [&](MemoryLoop& loop) {
        addIndexToSlotAt(loop, loop.loopVariantSlotAddress());
    });
    check(Counter::NotPromotedAddressLoadedInLoop, [&](MemoryLoop& loop) {
        addIndexToSlotAt(loop, loop.graph.make(loop.body(), Add, loop.slot(0, secondHeap), loop.graph.int64(loop.body(), 8)));
    });
    check(Counter::NotPromotedAddressNotHoisted, [&](MemoryLoop& loop) {
        addIndexToSlotAt(loop, loop.graph.make(loop.body(), Add, loop.loop.pointer, loop.loop.extra));
    });
    check(Counter::NotPromotedCallsOnLikelyPaths, [](MemoryLoop& loop) {
        loop.addIndexToSlot();
        loop.graph.callLikePatchpoint(loop.body());
        loop.finish();
    });
    check(Counter::NotPromotedNotGuaranteedToExecute, [](MemoryLoop& loop) {
        PatchpointValue* exit = loop.graph.emptyPatchpoint(loop.body());
        exit->effects.exitsSideways = true;
        exit->effects.reads = HeapRange::top();
        loop.addIndexToSlot();
        loop.finish();
    });
}

static void testPromoteLoopMemoryPromotesDespiteCallOnRarePath()
{
    MemoryLoop loop;
    loop.addIndexToSlot();
    BasicBlock* rare = loop.proc.addBlock();
    BasicBlock* next = loop.proc.addBlock();
    loop.graph.branch(loop.body(), loop.loop.extra, rarely(rare), FrequentedBlock(next));
    loop.graph.callLikePatchpoint(rare);
    loop.graph.jump(rare, next);
    loop.finish(next);
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
    auto code = compileProc(loop.proc);
    for (int64_t takesRarePath : { 0, 1 }) {
        int64_t slots[1] = { 5 };
        run(*code, 100, slots, takesRarePath);
        CHECK_EQ(slots[0], 5 + sumBelow(100));
    }
}

static void testPromoteLoopMemorySkipsLocationsHandledByClient()
{
    for (HeapRange heap : { firstHeap, secondHeap }) {
        Policy policy = promotingPolicy();
        policy.loops.clientHoistsOutOfLoops = [](Procedure&, const MemoryValue& memory) {
            return memory.range() == firstHeap;
        };
        MemoryLoop loop { policy };
        loop.setSlot(loop.graph.make(loop.body(), Add, loop.slot(0, heap), loop.index()), 0, heap);
        loop.finish();
        CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), heap == secondHeap);
        CHECK_EQ(loop.proc.report().countOf(Counter::NotPromotedHandledByClient), heap == firstHeap ? 1u : 0u);
    }
}

// Like every phase that replaces a load, the pass logs it so the location can be reloaded at the original position and compared.
static void testPromoteLoopMemoryLogsReplacedLoads()
{
    constexpr int64_t reported = -42;
    struct Case {
        bool storesThroughAlias;
        bool ignoresStore;
    };
    for (auto [storesThroughAlias, ignoresStore] : { Case { false, false }, Case { true, false }, Case { true, true } }) {
        Policy policy = promotingPolicy();
        policy.validation.logsMovedLoads = true;
        policy.validation.ignoresWritesToPromotedLocations = ignoresStore;
        MemoryLoop loop { policy };
        loop.addIndexToSlot();
        if (storesThroughAlias)
            loop.graph.store(loop.body(), loop.graph.int64(loop.body(), 1000), loop.loop.extra, 0, firstHeap);
        loop.finish();
        bool isPromoted = !storesThroughAlias || ignoresStore;
        CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), isPromoted);
        bool isChecked = loop.proc.movedLoadLog().insertChecks(loop.proc, createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
            jit.move(CCallHelpers::TrustedImm64(reported), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        }));
        CHECK_EQ(isChecked, isPromoted);
        auto code = compileProc(loop.proc);
        for (bool isSameSlot : { false, true }) {
            for (int64_t bound : { 0, 1, 2, 9 }) {
                int64_t slots[2] = { 5, 6 };
                int64_t result = run(*code, bound, slots, std::bit_cast<int64_t>(&slots[isSameSlot ? 0 : 1]));
                CHECK_EQ(result == reported, ignoresStore && isSameSlot && bound >= 2);
            }
        }
    }
}

static void testPromoteLoopMemoryIgnoresClobbersOnlyForTesting()
{
    Policy policy = promotingPolicy();
    policy.validation.ignoresWritesToPromotedLocations = true;
    MemoryLoop loop { policy };
    loop.addIndexToSlot();
    loop.graph.store(loop.body(), loop.graph.int64(loop.body(), 0), loop.loopVariantSlotAddress(), 0, firstHeap);
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    auto code = compileProc(loop.proc);
    int64_t slots[4] = { 5, 6, 7, 8 };
    run(*code, 6, slots);
    // As written, iteration 4 clears the slot and the next iteration adds 5.
    CHECK(slots[0] != 5);
}

static void testPromoteLoopMemoryOnRandomLoops()
{
    constexpr unsigned numberOfLoops = 1500;
    unsigned numberOfLoopsWithPromotion = 0;
    for (unsigned seed = 0; seed < numberOfLoops; ++seed) {
        RandomMemoryLoop plain { seed, false };
        RandomMemoryLoop promoted { seed, true };
        auto plainCode = compileProc(plain.proc);
        auto promotedCode = compileProc(promoted.proc);
        CHECK(!plain.proc.report().countOf(Counter::LocationsPromotedInLoops));
        numberOfLoopsWithPromotion += !!promoted.proc.report().countOf(Counter::LocationsPromotedInLoops);
        for (int64_t bound : { 0, 1, 2, 3, 7, 50 }) {
            constexpr unsigned numberOfSlots = RandomMemoryLoop::numberOfSlots;
            int64_t expected[numberOfSlots + 2] = { -1, 11, 0x1234567890abcdefll, -7, 1ll << 40, -1 };
            int64_t slots[numberOfSlots + 2] = { -1, 11, 0x1234567890abcdefll, -7, 1ll << 40, -1 };
            int64_t expectedResult = run(*plainCode, bound, expected + 1, seed * 3);
            int64_t result = run(*promotedCode, bound, slots + 1, seed * 3);
            if (result != expectedResult || memcmp(slots, expected, sizeof(slots))) {
                RandomMemoryLoop shown { seed, true };
                dataLogLn("Seed ", seed, ", ", bound, " iterations: got ", result, ", expected ", expectedResult, ":\n", shown.proc);
                CRASH();
            }
        }
    }
    // Loops with nothing promoted would test nothing.
    if (Options::defaultB3OptLevel() >= 2)
        CHECK(numberOfLoopsWithPromotion * 4 > numberOfLoops);
}

static void testPipelinePromotesLoopMemoryOnlyWhenEnabled()
{
    for (bool isAsked : { false, true }) {
        Policy policy = optimizingPolicy();
        policy.loops.promotesMemory = isAsked;
        MemoryLoop loop { policy };
        loop.addIndexToSlot();
        loop.loop.endBody(loop.body());
        checkAddsIndexesToSlot(loop.proc);
        CHECK_EQ(loop.proc.report().countOf(Counter::LocationsPromotedInLoops), isAsked && Options::defaultB3OptLevel() >= 2 ? 1u : 0u);
    }
}

void addBunLoopMemoryTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testPromoteLoopMemoryPromotesLocation());
    RUN(testPromoteLoopMemoryResetsValueOwners());
    RUN(testPromoteLoopMemoryPromotesEveryNumericType());
    RUN(testPromoteLoopMemoryPromotesLocationAlsoLoadedThroughOtherAddresses());
    RUN(testPromoteLoopMemorySkipsClobberedLocation());
    RUN(testPromoteLoopMemoryKeepsLoadOfDifferentWidth());
    RUN(testPromoteLoopMemorySkipsNarrowAccesses());
    RUN(testPromoteLoopMemorySkipsAccessesThatTrap());
    RUN(testPromoteLoopMemorySkipsLoopVariantAddress());
    RUN(testPromoteLoopMemoryChecksAliasingPerLoadRange());
    RUN(testPromoteLoopMemoryIgnoresRangeOfAccessThatNeverExecutes());
    RUN(testPromoteLoopMemoryPreheaderLoadUsesUnionOfRanges());
    RUN(testPromoteLoopMemorySkipsImmutableLoads());
    RUN(testPromoteLoopMemorySkipsFencesAndAtomics());
    RUN(testPromoteLoopMemorySkipsAccessNotGuaranteedToExecute());
    RUN(testPromoteLoopMemorySkipsLoopWithSidewaysExit());
    RUN(testPromoteLoopMemoryPromotesLocationStoredOnOnePath());
    RUN(testPromoteLoopMemoryHoistsLoadToOutermostLoop());
    RUN(testPromoteLoopMemoryVisitsEachAccessFewTimes());
    RUN(testPromoteLoopMemoryCountsReasonsNotPromoted());
    RUN(testPromoteLoopMemoryPromotesDespiteCallOnRarePath());
    RUN(testPromoteLoopMemorySkipsLocationsHandledByClient());
    RUN(testPromoteLoopMemoryLogsReplacedLoads());
    RUN(testPromoteLoopMemoryIgnoresClobbersOnlyForTesting());
    RUN(testPromoteLoopMemoryOnRandomLoops());
    RUN(testPipelinePromotesLoopMemoryOnlyWhenEnabled());
}

#endif // ENABLE(B3_JIT)
