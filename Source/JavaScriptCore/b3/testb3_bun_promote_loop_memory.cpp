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

// for (index = 0; index < bound; index++), asked once in front and then at the end of each turn.
struct LoopOverWords {
    explicit LoopOverWords(const Policy& policy = promotingPolicy())
        : proc(policy)
        , loop(graph)
    {
    }

    BasicBlock* body() { return loop.body; }
    Value* index() { return graph.get(loop.body, loop.index); }
    Value* word(int32_t offset = 0, HeapRange range = firstHeap) { return graph.load(loop.body, Int64, loop.pointer, offset, range); }
    void setWord(Value* value, int32_t offset = 0, HeapRange range = firstHeap) { graph.store(loop.body, value, loop.pointer, offset, range); }
    void addIndexToWord() { setWord(graph.make(loop.body, Add, word(), index())); }

    Value* addressOfWordThatTurnChooses()
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

void checkAddsIndexesToWord(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 10, 1000 }) {
        int64_t words[4] = { 5, 6, 7, 8 };
        run(*code, bound, words);
        CHECK_EQ(words[0], 5 + sumBelow(bound));
        CHECK_EQ(words[1], 6);
    }
}

template<typename Functor>
void checkIsCarried(const Functor& fillBody, unsigned loadsLeftInLoop = 0)
{
    LoopOverWords loop;
    loop.addIndexToWord();
    fillBody(loop);
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), loadsLeftInLoop);
    CHECK_EQ(loop.proc.report().countOf(Counter::WordsCarriedAroundLoops), 1u);
}

template<typename Functor>
void checkIsNotCarried(const Functor& fillBody)
{
    LoopOverWords loop;
    loop.addIndexToWord();
    fillBody(loop);
    loop.finish();
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    CHECK(!loop.proc.report().countOf(Counter::WordsCarriedAroundLoops));
}

PatchpointValue* appendPatchpointThatWrites(LoopOverWords& loop, HeapRange range)
{
    PatchpointValue* result = loop.graph.emptyPatchpoint(loop.body());
    result->effects.writes = range;
    return result;
}

// A loop whose turns read and write four words. The first two have a heap each, the last two share one, and an address that the turn chooses may be any of them.
struct RandomLoopOverWords {
    static constexpr unsigned numberOfWords = 4;

    RandomLoopOverWords(unsigned seed, bool promotes)
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

    // With the pass, every load that a phase has moved or given way for is read again where it stood.
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

    static HeapRange heapOf(unsigned word) { return !word ? firstHeap : word == 1 ? secondHeap : thirdHeap; }
    // The heap that the word lies in, or one that holds that heap and more.
    HeapRange heapNamedFor(unsigned word)
    {
        if (isInArmThatNeverRuns)
            return heapOf(random.getUint32(numberOfWords));
        if (!chance(35))
            return heapOf(word);
        return word < 2 && chance(50) ? bothHeaps : allHeaps;
    }
    // The words under another name: nothing tells the compiler that the two are one.
    Value* base(BasicBlock* block)
    {
        if (!chance(25))
            return loop.pointer;
        return graph.make(block, Add, loop.pointer, graph.make(block, BitAnd, loop.extra, graph.int64(block, 1ll << 62)));
    }
    bool chance(unsigned percent) { return random.getUint32(100) < percent; }
    Value* index(BasicBlock* block) { return graph.get(block, loop.index); }

    Value* addressThatTurnChooses(BasicBlock* block)
    {
        Value* chosen = graph.make(block, BitAnd, graph.make(block, Add, index(block), graph.int64(block, random.getUint32(numberOfWords))), graph.int64(block, numberOfWords - 1));
        return graph.make(block, Add, loop.pointer, graph.make(block, Shl, chosen, graph.int32(block, 3)));
    }

    Value* read(BasicBlock* block)
    {
        unsigned word = random.getUint32(numberOfWords);
        return graph.load(block, Int64, base(block), word * 8, heapNamedFor(word));
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
            unsigned word = random.getUint32(numberOfWords);
            B3::Opcode opcode = chance(50) ? Add : chance(50) ? BitXor : Sub;
            Value* address = base(block);
            graph.store(block, graph.make(block, opcode, graph.load(block, Int64, address, word * 8, heapNamedFor(word)), someValue(block)), address, word * 8, heapNamedFor(word));
            return;
        }
        if (kind < 52) {
            graph.increase(block, loop.sum, read(block));
            return;
        }
        if (kind < 60) {
            graph.increase(block, loop.sum, graph.load(block, Int64, addressThatTurnChooses(block), 0, allHeaps));
            return;
        }
        if (kind < 64) {
            unsigned word = random.getUint32(numberOfWords);
            graph.store(block, someValue(block), base(block), word * 8, heapNamedFor(word));
            return;
        }
        if (kind < 67) {
            graph.store(block, someValue(block), addressThatTurnChooses(block), 0, allHeaps);
            return;
        }
        if (kind < 71) {
            unsigned word = random.getUint32(numberOfWords);
            graph.increase(block, loop.sum, graph.make(block, ZExt32, graph.load(block, Int32, loop.pointer, word * 8 + 4 * random.getUint32(2), heapOf(word))));
            return;
        }
        if (kind < 73) {
            unsigned word = random.getUint32(numberOfWords);
            block->appendNew<MemoryValue>(proc, chance(50) ? Store8 : Store16, Origin(), graph.make(block, Trunc, someValue(block)), loop.pointer, static_cast<int32_t>(word * 8 + 2 * random.getUint32(4)), heapOf(word));
            return;
        }
        if (kind < 76) {
            appendPoll(graph, block, pollState);
            return;
        }
        if (kind < 80 && !isInArmThatNeverRuns) {
            // One word under two names. It is read and written under a heap that holds more than its own, written by the other name under its own,
            // and read under a heap that is not its own behind a test that no run passes.
            unsigned word = random.getUint32(2);
            BasicBlock* never = proc.addBlock();
            BasicBlock* join = proc.addBlock();
            bool testComesFirst = chance(50);
            auto appendTest = [&] {
                graph.branch(block, graph.make(block, Equal, loop.extra, graph.int64(block, -1)), never, join);
                graph.increase(never, loop.sum, graph.load(never, Int64, loop.pointer, word * 8, heapOf(1 - word)));
                graph.jump(never, join);
                block = join;
            };
            if (testComesFirst)
                appendTest();
            graph.store(block, graph.make(block, Add, graph.load(block, Int64, loop.pointer, word * 8, bothHeaps), index(block)), loop.pointer, word * 8, bothHeaps);
            Value* otherName = graph.make(block, Add, loop.pointer, graph.make(block, BitAnd, loop.extra, graph.int64(block, 1ll << 62)));
            graph.store(block, someValue(block), otherName, word * 8, heapOf(word));
            if (!testComesFirst)
                appendTest();
            graph.increase(block, loop.sum, graph.load(block, Int64, loop.pointer, word * 8, bothHeaps));
            return;
        }
        if (depth >= 2) {
            graph.increase(block, loop.sum, someValue(block));
            return;
        }
        if (kind < 90) {
            BasicBlock* arm = proc.addBlock();
            BasicBlock* join = proc.addBlock();
            // What no run comes to may name any heap: nothing holds of it.
            bool neverRuns = !isInArmThatNeverRuns && chance(40);
            Value* condition = neverRuns ? graph.make(block, Equal, loop.extra, graph.int64(block, -1)) : graph.make(block, BitAnd, index(block), graph.int64(block, 1 + random.getUint32(3)));
            graph.branch(block, condition, arm, join);
            isInArmThatNeverRuns |= neverRuns;
            for (unsigned count = 1 + random.getUint32(2); count--;)
                appendStatement(arm, depth + 1);
            if (neverRuns)
                isInArmThatNeverRuns = false;
            graph.jump(arm, join);
            block = join;
            return;
        }
        BasicBlock* inner = proc.addBlock();
        BasicBlock* after = proc.addBlock();
        Variable* turnsLeft = proc.addVariable(Int64);
        graph.set(block, turnsLeft, graph.make(block, BitAnd, index(block), graph.int64(block, 3)));
        graph.jump(block, inner);
        BasicBlock* lastOfInner = inner;
        for (unsigned count = 1 + random.getUint32(2); count--;)
            appendStatement(lastOfInner, depth + 1);
        graph.increase(lastOfInner, turnsLeft, -1);
        graph.branch(lastOfInner, graph.make(lastOfInner, GreaterEqual, graph.get(lastOfInner, turnsLeft), graph.int64(lastOfInner, 0)), inner, after);
        block = after;
    }

    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop;
    WeakRandom random;
    bool isInArmThatNeverRuns { false };
    PollState pollState;
};

} // anonymous namespace

static void testPromoteLoopMemoryCarriesWord()
{
    LoopOverWords loop;
    loop.addIndexToWord();
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
    CHECK_EQ(valueCountInLoops(loop.proc, Store), 1u);
    CHECK_EQ(numberOfValues(loop.proc, Load), 1u);
    CHECK_EQ(loop.proc.report().countOf(Counter::WordsCarriedAroundLoops), 1u);
    checkAddsIndexesToWord(loop.proc);
}

static void testPromoteLoopMemoryCarriesNumbersOfEveryKind()
{
    for (Type type : { Int32, Int64, Float, Double }) {
        LoopOverWords loop;
        Graph& graph = loop.graph;
        BasicBlock* body = loop.body();
        Value* one = type == Int32 ? graph.int32(body, 1) : type == Int64 ? graph.int64(body, 1) : type == Double ? graph.number(body, 1) : body->appendNew<ConstFloatValue>(loop.proc, Origin(), 1.0f);
        graph.store(body, graph.make(body, Add, graph.load(body, type, loop.loop.pointer, 0, firstHeap), one), loop.loop.pointer, 0, firstHeap);
        loop.finish();
        CHECK(promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
        auto code = compileProc(loop.proc);
        int64_t words[2] = { 0, 9 };
        run(*code, 100, words);
        if (type == Int32)
            CHECK_EQ(words[0], 100);
        else if (type == Int64)
            CHECK_EQ(words[0], 100);
        else if (type == Double)
            CHECK_EQ(std::bit_cast<double>(words[0]), 100.0);
        else
            CHECK_EQ(std::bit_cast<float>(static_cast<int32_t>(words[0])), 100.0f);
        CHECK_EQ(words[1], 9);
    }
}

// The stores stay, so whoever else reads the word out of memory finds it there.
static void testPromoteLoopMemoryCarriesWordThatIsReadUnderOtherAddresses()
{
    LoopOverWords loop;
    Graph& graph = loop.graph;
    Value* other = graph.load(loop.body(), Int64, loop.addressOfWordThatTurnChooses(), 0, firstHeap);
    loop.setWord(graph.make(loop.body(), Add, loop.word(), other));
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 1u);
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 2, 5, 9, 40 }) {
        int64_t words[4] = { 1, 2, 3, 4 };
        int64_t expected[4] = { 1, 2, 3, 4 };
        for (int64_t index = 0; index < bound; ++index)
            expected[0] += expected[index & 3];
        run(*code, bound, words);
        for (unsigned i = 0; i < 4; ++i)
            CHECK_EQ(words[i], expected[i]);
    }
}

static void testPromoteLoopMemorySkipsWordThatIsWrittenOtherwise()
{
    // The word beside it, in the same heap.
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.setWord(loop.index(), 8);
    });
    // A word that the turn chooses.
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.addressOfWordThatTurnChooses(), 0, firstHeap);
    });
    // Under another address that stays the same.
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.loop.extra, 0, firstHeap);
    });
    // A heap that holds this one.
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.loop.extra, 0, bothHeaps);
    });
    // Part of the word.
    for (B3::Opcode opcode : { Store8, Store16 }) {
        checkIsNotCarried([&](LoopOverWords& loop) {
            loop.body()->appendNew<MemoryValue>(loop.proc, opcode, Origin(), loop.graph.make(loop.body(), Trunc, loop.index()), loop.loop.pointer, 0, firstHeap);
        });
    }
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.graph.store(loop.body(), loop.graph.make(loop.body(), Trunc, loop.index()), loop.loop.pointer, 0, firstHeap);
    });
    // The same bits as a number of another kind.
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.graph.store(loop.body(), loop.graph.make(loop.body(), IToD, loop.index()), loop.loop.pointer, 0, firstHeap);
    });
    // By what is no store.
    checkIsNotCarried([](LoopOverWords& loop) {
        appendPatchpointThatWrites(loop, firstHeap);
    });
    checkIsNotCarried([](LoopOverWords& loop) {
        appendPatchpointThatWrites(loop, HeapRange::top());
    });

    // Another heap is another place.
    checkIsCarried([](LoopOverWords& loop) {
        loop.graph.store(loop.body(), loop.index(), loop.addressOfWordThatTurnChooses(), 0, secondHeap);
        appendPatchpointThatWrites(loop, thirdHeap);
    });
}

static void testPromoteLoopMemoryLeavesLoadOfAnotherWidthInMemory()
{
    checkIsCarried([](LoopOverWords& loop) {
        Value* half = loop.graph.load(loop.body(), Int32, loop.loop.pointer, 0, firstHeap);
        loop.graph.increase(loop.body(), loop.loop.sum, loop.graph.make(loop.body(), ZExt32, half));
    }, 1);
}

// What is stored loses its upper bits and what is read is made wider again: the value that goes in is not the value that comes out.
static void testPromoteLoopMemorySkipsPartsOfWords()
{
    struct Part {
        B3::Opcode load;
        B3::Opcode store;
    };
    for (Part part : { Part { Load8Z, Store8 }, Part { Load8S, Store8 }, Part { Load16Z, Store16 }, Part { Load16S, Store16 } }) {
        LoopOverWords loop;
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
        LoopOverWords loop;
        Graph& graph = loop.graph;
        BasicBlock* body = loop.body();
        Value* read = body->appendNew<MemoryValue>(loop.proc, loadTraps ? trapping(Load) : B3::Kind(Load), Int64, Origin(), loop.loop.pointer, 0, firstHeap);
        body->appendNew<MemoryValue>(loop.proc, loadTraps ? B3::Kind(Store) : trapping(Store), Origin(), graph.make(body, Add, read, loop.index()), loop.loop.pointer, 0, firstHeap);
        loop.finish();
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    }
}

static void testPromoteLoopMemorySkipsWordThatTurnChooses()
{
    LoopOverWords loop;
    Graph& graph = loop.graph;
    Value* address = loop.addressOfWordThatTurnChooses();
    graph.store(loop.body(), graph.make(loop.body(), Add, graph.load(loop.body(), Int64, address, 0, firstHeap), loop.index()), address, 0, firstHeap);
    loop.finish();
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
}

// What is written under a heap apart from the one that a load names is not what that load reads. The heap that a store names says nothing of that.
static void testPromoteLoopMemoryGoesByHeapsThatLoadsOfWordName()
{
    enum class Other : uint8_t { None, InSecondHeap, InFirstHeap };
    for (bool storeNamesMore : { false, true }) {
        for (Other other : { Other::None, Other::InSecondHeap, Other::InFirstHeap }) {
            LoopOverWords loop;
            loop.setWord(loop.graph.make(loop.body(), Add, loop.word(0, storeNamesMore ? firstHeap : bothHeaps), loop.index()), 0, storeNamesMore ? bothHeaps : firstHeap);
            if (other != Other::None)
                loop.graph.store(loop.body(), loop.index(), loop.addressOfWordThatTurnChooses(), 8, other == Other::InFirstHeap ? firstHeap : secondHeap);
            loop.finish();
            CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), other == Other::None || (storeNamesMore && other == Other::InSecondHeap));
        }
    }
}

// An access that no turn comes to says nothing about where the word lies.
static void testPromoteLoopMemoryGoesByNoHeapOfAnAccessThatNeverRuns()
{
    LoopOverWords loop;
    Graph& graph = loop.graph;
    BasicBlock* never = loop.proc.addBlock();
    BasicBlock* next = loop.proc.addBlock();
    loop.setWord(graph.make(loop.body(), Add, loop.word(0, bothHeaps), loop.index()), 0, bothHeaps);
    graph.store(loop.body(), graph.int64(loop.body(), 100), loop.loop.extra, 0, secondHeap);
    graph.branch(loop.body(), graph.make(loop.body(), LessThan, loop.index(), graph.int64(loop.body(), 0)), never, next);
    graph.increase(never, loop.loop.sum, graph.load(never, Int64, loop.loop.pointer, 0, firstHeap));
    graph.jump(never, next);
    graph.increase(next, loop.loop.sum, graph.load(next, Int64, loop.loop.pointer, 0, bothHeaps));
    loop.finish(next);
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 2, 5 }) {
        int64_t words[1] = { 5 };
        CHECK_EQ(run(*code, bound, words, std::bit_cast<int64_t>(&words[0])), 100 * bound);
    }
}

// It runs whether or not a turn comes to the load that it was made from, so it names every heap that is named for the word.
static void testPromoteLoopMemoryReadsAheadOfLoopUnderEveryHeapThatIsNamedForWord()
{
    LoopOverWords loop;
    loop.setWord(loop.graph.make(loop.body(), Add, loop.word(0, firstHeap), loop.index()), 0, bothHeaps);
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

static void testPromoteLoopMemorySkipsWordThatIsSaidNeverToChange()
{
    LoopOverWords loop;
    MemoryValue* read = loop.graph.load(loop.body(), Int64, loop.loop.pointer, 0, firstHeap);
    read->setReadsMutability(Mutability::Immutable);
    loop.setWord(loop.graph.make(loop.body(), Add, read, loop.index()));
    loop.finish();
    CHECK(!promoteLoopMemoryAndValidate(loop.proc));
}

static void testPromoteLoopMemorySkipsFencesAndAtomics()
{
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.body()->appendNew<FenceValue>(loop.proc, Origin());
    });
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.body()->appendNew<AtomicValue>(loop.proc, AtomicXchgAdd, Origin(), Width64, loop.index(), loop.loop.extra, 0, secondHeap);
    });
    checkIsNotCarried([](LoopOverWords& loop) {
        loop.body()->appendNew<MemoryValue>(loop.proc, Load, Int64, Origin(), loop.loop.extra, 0, secondHeap, secondHeap);
    });
}

// Read in front of a loop that may run no turn, the word would be touched where the program touches nothing.
static void testPromoteLoopMemorySkipsWordThatMayNotBeTouched()
{
    {
        LoopOverWords loop;
        loop.addIndexToWord();
        loop.loop.endBody(loop.body());
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
        auto code = compileProc(loop.proc);
        CHECK_EQ(run(*code, 0, nullptr), 0);
    }
    {
        LoopOverWords loop;
        Graph& graph = loop.graph;
        BasicBlock* touches = loop.proc.addBlock();
        BasicBlock* next = loop.proc.addBlock();
        graph.branch(loop.body(), loop.loop.extra, touches, next);
        graph.store(touches, graph.make(touches, Add, graph.load(touches, Int64, loop.loop.pointer, 0, firstHeap), graph.get(touches, loop.loop.index)), loop.loop.pointer, 0, firstHeap);
        graph.jump(touches, next);
        loop.finish(next);
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
        auto code = compileProc(loop.proc);
        CHECK_EQ(run(*code, 10, nullptr, 0), 0);
        int64_t words[1] = { 5 };
        run(*code, 10, words, 1);
        CHECK_EQ(words[0], 5 + sumBelow(10));
    }
}

static void testPromoteLoopMemorySkipsLoopThatIsLeftSideways()
{
    {
        LoopOverWords loop;
        PatchpointValue* exit = loop.graph.emptyPatchpoint(loop.body());
        exit->effects.exitsSideways = true;
        exit->effects.reads = HeapRange::top();
        loop.addIndexToWord();
        loop.finish();
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
    }
    {
        PollState state;
        LoopOverWords loop;
        appendPoll(loop.graph, loop.body(), state)->effects.exitsSideways = true;
        loop.addIndexToWord();
        loop.finish();
        CHECK(promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
        CHECK_EQ(valueCountInLoops(loop.proc, Store), 1u);
        checkAddsIndexesToWord(loop.proc);
    }
}

static void testPromoteLoopMemoryCarriesWordThatOneWayStores()
{
    LoopOverWords loop;
    Graph& graph = loop.graph;
    BasicBlock* stores = loop.proc.addBlock();
    BasicBlock* next = loop.proc.addBlock();
    Value* read = loop.word();
    graph.increase(loop.body(), loop.loop.sum, read);
    graph.branch(loop.body(), graph.make(loop.body(), BitAnd, loop.index(), graph.int64(loop.body(), 1)), stores, next);
    graph.store(stores, graph.make(stores, Add, read, graph.get(stores, loop.loop.index)), loop.loop.pointer, 0, firstHeap);
    graph.jump(stores, next);
    loop.finish(next);
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 2, 3, 10, 101 }) {
        int64_t words[1] = { 5 };
        int64_t word = 5;
        int64_t sum = 0;
        for (int64_t index = 0; index < bound; ++index) {
            sum += word;
            if (index & 1)
                word += index;
        }
        CHECK_EQ(run(*code, bound, words), sum);
        CHECK_EQ(words[0], word);
    }
}

static void testPromoteLoopMemoryReadsInFrontOfOutermostLoop()
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
    CHECK_EQ(proc.report().countOf(Counter::WordsCarriedAroundLoops), 1u);
    CHECK_EQ(valueCountInLoops(proc, Load), 0u);
    CHECK_EQ(numberOfValues(root, Load), 1u);
    auto code = compileProc(proc);
    int64_t words[1] = { 5 };
    run(*code, 10, words, 7);
    CHECK_EQ(words[0], 5 + 7 * sumBelow(10));
}

// Fields and variables have a heap each. A loop that writes many of them is not asked about every store for every load.
static void testPromoteLoopMemoryReadsEachAccessAFewTimes()
{
    for (unsigned numberOfWords : { 100u, 1000u, 10000u }) {
        LoopOverWords loop;
        for (unsigned word = 0; word < numberOfWords; ++word) {
            HeapRange heap { 100 + word, 101 + word };
            loop.setWord(loop.graph.make(loop.body(), Add, loop.word(word * 8, heap), loop.index()), word * 8, heap);
        }
        loop.finish();
        CHECK(promoteLoopMemory(loop.proc));
        CHECK_EQ(loop.proc.report().countOf(Counter::WordsCarriedAroundLoops), numberOfWords);
        CHECK(loop.proc.report().countOf(Counter::AccessesReadForWordsCarriedAroundLoops) <= 4 * numberOfWords);
    }
}

// Why a word stays in memory is counted, so that a build can say what the pass finds in a program.
static void testPromoteLoopMemorySaysWhyWordsAreLeft()
{
    auto check = [](Counter reason, const auto& fillBody) {
        LoopOverWords loop;
        fillBody(loop);
        CHECK(!promoteLoopMemoryAndValidate(loop.proc));
        CHECK_EQ(loop.proc.report().countOf(Counter::LoopsSearchedForWordsToCarry), 1u);
        CHECK_EQ(loop.proc.report().countOf(Counter::WordsReadAndWrittenInLoops), 1u);
        CHECK_EQ(loop.proc.report().countOf(reason), 1u);
    };
    check(Counter::WordsLeftForWritesOnLikelyPaths, [](LoopOverWords& loop) {
        loop.addIndexToWord();
        appendPatchpointThatWrites(loop, firstHeap);
        loop.finish();
    });
    check(Counter::WordsLeftForWritesOnRarePaths, [](LoopOverWords& loop) {
        loop.addIndexToWord();
        BasicBlock* rare = loop.proc.addBlock();
        BasicBlock* next = loop.proc.addBlock();
        loop.graph.branch(loop.body(), loop.loop.extra, rarely(rare), FrequentedBlock(next));
        loop.graph.emptyPatchpoint(rare)->effects.writes = firstHeap;
        loop.graph.jump(rare, next);
        loop.finish(next);
    });
    check(Counter::WordsLeftForStoresUnderOtherAddresses, [](LoopOverWords& loop) {
        loop.addIndexToWord();
        loop.graph.store(loop.body(), loop.index(), loop.loop.extra, 0, firstHeap);
        loop.finish();
    });
    auto addIndexToWordAt = [](LoopOverWords& loop, Value* address) {
        loop.graph.store(loop.body(), loop.graph.make(loop.body(), Add, loop.graph.load(loop.body(), Int64, address, 0, firstHeap), loop.index()), address, 0, firstHeap);
        loop.finish();
    };
    check(Counter::WordsLeftForAddressThatChangesWithTurns, [&](LoopOverWords& loop) {
        addIndexToWordAt(loop, loop.addressOfWordThatTurnChooses());
    });
    check(Counter::WordsLeftForAddressReadInLoop, [&](LoopOverWords& loop) {
        addIndexToWordAt(loop, loop.graph.make(loop.body(), Add, loop.word(0, secondHeap), loop.graph.int64(loop.body(), 8)));
    });
    check(Counter::WordsLeftForAddressNotMovedOutOfLoop, [&](LoopOverWords& loop) {
        addIndexToWordAt(loop, loop.graph.make(loop.body(), Add, loop.loop.pointer, loop.loop.extra));
    });
    check(Counter::WordsLeftBesideCallsOnLikelyPaths, [](LoopOverWords& loop) {
        loop.addIndexToWord();
        loop.graph.callLikePatchpoint(loop.body());
        loop.finish();
    });
    check(Counter::WordsLeftThatMayNotBeTouched, [](LoopOverWords& loop) {
        PatchpointValue* exit = loop.graph.emptyPatchpoint(loop.body());
        exit->effects.exitsSideways = true;
        exit->effects.reads = HeapRange::top();
        loop.addIndexToWord();
        loop.finish();
    });
}

static void testPromoteLoopMemoryCarriesWordBesideCallBehindRareEdge()
{
    LoopOverWords loop;
    loop.addIndexToWord();
    BasicBlock* rare = loop.proc.addBlock();
    BasicBlock* next = loop.proc.addBlock();
    loop.graph.branch(loop.body(), loop.loop.extra, rarely(rare), FrequentedBlock(next));
    loop.graph.callLikePatchpoint(rare);
    loop.graph.jump(rare, next);
    loop.finish(next);
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    CHECK_EQ(valueCountInLoops(loop.proc, Load), 0u);
    auto code = compileProc(loop.proc);
    for (int64_t takesRareWay : { 0, 1 }) {
        int64_t words[1] = { 5 };
        run(*code, 100, words, takesRareWay);
        CHECK_EQ(words[0], 5 + sumBelow(100));
    }
}

static void testPromoteLoopMemoryLeavesWordsThatClientTakesOutOfLoops()
{
    for (HeapRange heap : { firstHeap, secondHeap }) {
        Policy policy = promotingPolicy();
        policy.loops.isTakenOutOfLoopsByClient = [](Procedure&, const MemoryValue& memory) {
            return memory.range() == firstHeap;
        };
        LoopOverWords loop { policy };
        loop.setWord(loop.graph.make(loop.body(), Add, loop.word(0, heap), loop.index()), 0, heap);
        loop.finish();
        CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), heap == secondHeap);
        CHECK_EQ(loop.proc.report().countOf(Counter::WordsLeftToPhasesOfClient), heap == firstHeap ? 1u : 0u);
    }
}

// As with every phase that gives the users of a load another value: the word is read again where the load stood.
static void testPromoteLoopMemoryLogsLoadsThatGiveWay()
{
    constexpr int64_t reported = -42;
    struct Case {
        bool storesUnderOtherAddress;
        bool overlooksStore;
    };
    for (auto [storesUnderOtherAddress, overlooksStore] : { Case { false, false }, Case { true, false }, Case { true, true } }) {
        Policy policy = promotingPolicy();
        policy.validation.logsMovedLoads = true;
        policy.validation.overlooksWritesToWordsCarriedAroundLoops = overlooksStore;
        LoopOverWords loop { policy };
        loop.addIndexToWord();
        if (storesUnderOtherAddress)
            loop.graph.store(loop.body(), loop.graph.int64(loop.body(), 1000), loop.loop.extra, 0, firstHeap);
        loop.finish();
        bool isCarried = !storesUnderOtherAddress || overlooksStore;
        CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), isCarried);
        bool isChecked = loop.proc.movedLoadLog().insertChecks(loop.proc, createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
            jit.move(CCallHelpers::TrustedImm64(reported), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        }));
        CHECK_EQ(isChecked, isCarried);
        auto code = compileProc(loop.proc);
        for (bool isSameWord : { false, true }) {
            for (int64_t bound : { 0, 1, 2, 9 }) {
                int64_t words[2] = { 5, 6 };
                int64_t result = run(*code, bound, words, std::bit_cast<int64_t>(&words[isSameWord ? 0 : 1]));
                CHECK_EQ(result == reported, overlooksStore && isSameWord && bound >= 2);
            }
        }
    }
}

static void testPromoteLoopMemoryOverlooksOtherWritesOnlyWhereToldTo()
{
    Policy policy = promotingPolicy();
    policy.validation.overlooksWritesToWordsCarriedAroundLoops = true;
    LoopOverWords loop { policy };
    loop.addIndexToWord();
    loop.graph.store(loop.body(), loop.graph.int64(loop.body(), 0), loop.addressOfWordThatTurnChooses(), 0, firstHeap);
    loop.finish();
    CHECK(promoteLoopMemoryAndValidate(loop.proc));
    auto code = compileProc(loop.proc);
    int64_t words[4] = { 5, 6, 7, 8 };
    run(*code, 6, words);
    // As the program is written, the turn with index 4 clears the word and the next adds 5.
    CHECK(words[0] != 5);
}

static void testPromoteLoopMemoryOnRandomLoops()
{
    constexpr unsigned numberOfLoops = 1500;
    unsigned numberOfLoopsThatCarry = 0;
    for (unsigned seed = 0; seed < numberOfLoops; ++seed) {
        RandomLoopOverWords plain { seed, false };
        RandomLoopOverWords promoted { seed, true };
        auto plainCode = compileProc(plain.proc);
        auto promotedCode = compileProc(promoted.proc);
        CHECK(!plain.proc.report().countOf(Counter::WordsCarriedAroundLoops));
        numberOfLoopsThatCarry += !!promoted.proc.report().countOf(Counter::WordsCarriedAroundLoops);
        for (int64_t bound : { 0, 1, 2, 3, 7, 50 }) {
            constexpr unsigned numberOfWords = RandomLoopOverWords::numberOfWords;
            int64_t expected[numberOfWords + 2] = { -1, 11, 0x1234567890abcdefll, -7, 1ll << 40, -1 };
            int64_t words[numberOfWords + 2] = { -1, 11, 0x1234567890abcdefll, -7, 1ll << 40, -1 };
            int64_t expectedResult = run(*plainCode, bound, expected + 1, seed * 3);
            int64_t result = run(*promotedCode, bound, words + 1, seed * 3);
            if (result != expectedResult || memcmp(words, expected, sizeof(words))) {
                RandomLoopOverWords shown { seed, true };
                dataLogLn("The loop with seed ", seed, " and ", bound, " turns answers ", result, " for ", expectedResult, ":\n", shown.proc);
                CRASH();
            }
        }
    }
    // Loops that are left alone would show nothing.
    if (Options::defaultB3OptLevel() >= 2)
        CHECK(numberOfLoopsThatCarry * 4 > numberOfLoops);
}

static void testPipelinePromotesLoopMemoryOnlyWhereAsked()
{
    for (bool isAsked : { false, true }) {
        Policy policy = optimizingPolicy();
        policy.loops.promotesMemory = isAsked;
        LoopOverWords loop { policy };
        loop.addIndexToWord();
        loop.loop.endBody(loop.body());
        checkAddsIndexesToWord(loop.proc);
        CHECK_EQ(loop.proc.report().countOf(Counter::WordsCarriedAroundLoops), isAsked && Options::defaultB3OptLevel() >= 2 ? 1u : 0u);
    }
}

void addBunLoopMemoryTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testPromoteLoopMemoryCarriesWord());
    RUN(testPromoteLoopMemoryCarriesNumbersOfEveryKind());
    RUN(testPromoteLoopMemoryCarriesWordThatIsReadUnderOtherAddresses());
    RUN(testPromoteLoopMemorySkipsWordThatIsWrittenOtherwise());
    RUN(testPromoteLoopMemoryLeavesLoadOfAnotherWidthInMemory());
    RUN(testPromoteLoopMemorySkipsPartsOfWords());
    RUN(testPromoteLoopMemorySkipsAccessesThatTrap());
    RUN(testPromoteLoopMemorySkipsWordThatTurnChooses());
    RUN(testPromoteLoopMemoryGoesByHeapsThatLoadsOfWordName());
    RUN(testPromoteLoopMemoryGoesByNoHeapOfAnAccessThatNeverRuns());
    RUN(testPromoteLoopMemoryReadsAheadOfLoopUnderEveryHeapThatIsNamedForWord());
    RUN(testPromoteLoopMemorySkipsWordThatIsSaidNeverToChange());
    RUN(testPromoteLoopMemorySkipsFencesAndAtomics());
    RUN(testPromoteLoopMemorySkipsWordThatMayNotBeTouched());
    RUN(testPromoteLoopMemorySkipsLoopThatIsLeftSideways());
    RUN(testPromoteLoopMemoryCarriesWordThatOneWayStores());
    RUN(testPromoteLoopMemoryReadsInFrontOfOutermostLoop());
    RUN(testPromoteLoopMemoryReadsEachAccessAFewTimes());
    RUN(testPromoteLoopMemorySaysWhyWordsAreLeft());
    RUN(testPromoteLoopMemoryCarriesWordBesideCallBehindRareEdge());
    RUN(testPromoteLoopMemoryLeavesWordsThatClientTakesOutOfLoops());
    RUN(testPromoteLoopMemoryLogsLoadsThatGiveWay());
    RUN(testPromoteLoopMemoryOverlooksOtherWritesOnlyWhereToldTo());
    RUN(testPromoteLoopMemoryOnRandomLoops());
    RUN(testPipelinePromotesLoopMemoryOnlyWhereAsked());
}

#endif // ENABLE(B3_JIT)
