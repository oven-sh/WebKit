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

    static Policy policyFor(bool promotes)
    {
        Policy policy = optimizingPolicy();
        policy.loops.promotesMemory = promotes;
        return policy;
    }

    static HeapRange heapOf(unsigned word) { return !word ? firstHeap : word == 1 ? secondHeap : thirdHeap; }
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
        return graph.load(block, Int64, loop.pointer, word * 8, heapOf(word));
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
            graph.store(block, graph.make(block, opcode, graph.load(block, Int64, loop.pointer, word * 8, heapOf(word)), someValue(block)), loop.pointer, word * 8, heapOf(word));
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
            graph.store(block, someValue(block), loop.pointer, word * 8, heapOf(word));
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
        if (kind < 78) {
            appendPoll(graph, block, pollState);
            return;
        }
        if (depth >= 2) {
            graph.increase(block, loop.sum, someValue(block));
            return;
        }
        if (kind < 90) {
            BasicBlock* arm = proc.addBlock();
            BasicBlock* join = proc.addBlock();
            graph.branch(block, graph.make(block, BitAnd, index(block), graph.int64(block, 1 + random.getUint32(3))), arm, join);
            for (unsigned count = 1 + random.getUint32(2); count--;)
                appendStatement(arm, depth + 1);
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

// The word lies in every heap that an access to it names: what writes outside one of them does not write the word.
static void testPromoteLoopMemoryGoesByNarrowestHeapThatIsNamedForWord()
{
    enum class Other : uint8_t { None, InHeapOfStoreAlone, InHeapOfBoth };
    for (bool storeNamesMore : { false, true }) {
        for (Other other : { Other::None, Other::InHeapOfStoreAlone, Other::InHeapOfBoth }) {
            LoopOverWords loop;
            loop.setWord(loop.graph.make(loop.body(), Add, loop.word(0, storeNamesMore ? firstHeap : bothHeaps), loop.index()), 0, storeNamesMore ? bothHeaps : firstHeap);
            if (other != Other::None)
                loop.graph.store(loop.body(), loop.index(), loop.addressOfWordThatTurnChooses(), 8, other == Other::InHeapOfBoth ? firstHeap : secondHeap);
            loop.finish();
            CHECK_EQ(promoteLoopMemoryAndValidate(loop.proc), other != Other::InHeapOfBoth);
        }
    }
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
    RUN(testPromoteLoopMemoryGoesByNarrowestHeapThatIsNamedForWord());
    RUN(testPromoteLoopMemorySkipsWordThatIsSaidNeverToChange());
    RUN(testPromoteLoopMemorySkipsFencesAndAtomics());
    RUN(testPromoteLoopMemorySkipsWordThatMayNotBeTouched());
    RUN(testPromoteLoopMemorySkipsLoopThatIsLeftSideways());
    RUN(testPromoteLoopMemoryCarriesWordThatOneWayStores());
    RUN(testPromoteLoopMemoryReadsInFrontOfOutermostLoop());
    RUN(testPromoteLoopMemoryReadsEachAccessAFewTimes());
    RUN(testPromoteLoopMemoryOverlooksOtherWritesOnlyWhereToldTo());
    RUN(testPromoteLoopMemoryOnRandomLoops());
    RUN(testPipelinePromotesLoopMemoryOnlyWhereAsked());
}

#endif // ENABLE(B3_JIT)
