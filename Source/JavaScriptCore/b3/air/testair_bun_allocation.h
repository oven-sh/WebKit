/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_ALLOCATION_TESTS() do { \
        RUN(testFormTraitsMatchRoles()); \
        RUN(testForEachTmpWithTraitsMatchesForEachTmp()); \
        RUN(testPatchArgRolesAreStable()); \
        RUN(testInterferenceGraphsAgree()); \
        RUN(testDenseInterferenceLimit()); \
        RUN(testManySlotsShareCells()); \
        RUN(testManySlotsDropMovesToThemselves()); \
        RUN(testManySlotsInRandomGraphs()); \
        RUN(testManySlotsOfMixedSizes()); \
        RUN(testFirstFitSizes()); \
        RUN(testNoEmptyInstAfterStackAllocation()); \
        RUN(testValuesSurviveRandomClobbers()); \
        RUN(testValuesLiveAcross32And33Calls()); \
        RUN(testBlocksWithoutSuccessorsMoveToEnd()); \
        RUN(testDeadDefSurvivesAllocation()); \
        RUN(testClobberValidationSeesDeadClobbers()); \
        RUN(testClusterSplitWithinBlockIsUpstream()); \
        RUN(testCoalescedDoublesAreSpilled()); \
        RUN(testBranchDefinesTmpAtOptLevel0()); \
    } while (false)

struct StackSlotReferenceAdapter : LivenessAdapter<StackSlotReferenceAdapter> {
    static constexpr ASCIILiteral name = "StackSlotReferenceLiveness"_s;
    typedef StackSlot* Thing;

    StackSlotReferenceAdapter(Code& code)
        : LivenessAdapter<StackSlotReferenceAdapter>(code)
    {
    }

    unsigned numIndices() { return code.stackSlots().size(); }
    static bool acceptsBank(B3::Bank) { return true; }
    static bool acceptsRole(Arg::Role) { return true; }
    static unsigned valueToIndex(StackSlot* slot) { return slot->index(); }
    StackSlot* indexToValue(unsigned index) { return code.stackSlots()[index]; }
};

void testFormTraitsMatchRoles()
{
    using Greedy::Mentions;
    for (unsigned roleIndex = 0; roleIndex < Mentions::numberOfRoles; ++roleIndex) {
        Arg::Role role = static_cast<Arg::Role>(roleIndex);
        for (B3::Bank bank : { GP, FP }) {
            for (Width width : { Width8, Width16, Width32, Width64, Width128 }) {
                unsigned traits = Greedy::formTraits[ENCODE_INST_FORM(role, bank, width)];
                auto hasTrait = [&](unsigned trait) {
                    return !!(traits & trait);
                };
                CHECK(hasTrait(Mentions::earlyUse) == Arg::isEarlyUse(role));
                CHECK(hasTrait(Mentions::lateUse) == Arg::isLateUse(role));
                CHECK(hasTrait(Mentions::earlyDef) == Arg::isEarlyDef(role));
                CHECK(hasTrait(Mentions::lateDef) == Arg::isLateDef(role));
                CHECK(hasTrait(Mentions::anyUse) == Arg::isAnyUse(role));
                CHECK(hasTrait(Mentions::anyDef) == Arg::isAnyDef(role));
                CHECK(hasTrait(Mentions::zDef) == Arg::isZDef(role));
                CHECK(hasTrait(Mentions::coldUse) == Arg::isColdUse(role));
                CHECK(hasTrait(Mentions::warmUseOrDef) == (Arg::isWarmUse(role) || Arg::isAnyDef(role)));
                CHECK(((traits >> Mentions::widthShift) & Mentions::widthMask) == static_cast<unsigned>(width));
                CHECK(hasTrait(Mentions::isFP) == (bank == FP));
            }
        }
    }
    for (unsigned invalidForm : { 0u, 1u, 2u, 0xffu })
        CHECK(!Greedy::formTraits[invalidForm]);
}

static void checkArgTraits(Arg arg, Arg::Role argRole, B3::Bank argBank, Width argWidth)
{
    using Greedy::Mentions;
    Vector<std::pair<Tmp, unsigned>> expected;
    arg.forEachTmp(argRole, argBank, argWidth, [&](Tmp& tmp, Arg::Role role, B3::Bank bank, Width width) {
        expected.append({ tmp, Mentions::traitsOf(role, bank, width) });
    });
    Vector<std::pair<Tmp, unsigned>> actual;
    forEachTmpWithTraits(arg, Mentions::traitsOf(argRole, argBank, argWidth), [&](Tmp tmp, unsigned traits) {
        actual.append({ tmp, traits });
    });
    CHECK(actual == expected);
}

void testForEachTmpWithTraitsMatchesForEachTmp()
{
    using Greedy::Mentions;
    Tmp a0 = argumentTmp(0);
    Tmp a1 = argumentTmp(1);
    Vector<Arg> addresses { Arg::simpleAddr(a0), Arg::addr(a0, 8), Arg::extendedOffsetAddr(8), Arg::index(a0, a1, 8) };
#if CPU(ARM64)
    addresses.append(Arg::preIndex(a0, 8));
    addresses.append(Arg::postIndex(a0, 8));
#endif
    for (unsigned roleIndex = 0; roleIndex < Mentions::numberOfRoles; ++roleIndex) {
        Arg::Role role = static_cast<Arg::Role>(roleIndex);
        for (Width width : { Width8, Width16, Width32, Width64, Width128 }) {
            if (role != Arg::UseAddr)
                checkArgTraits(Arg(a0), role, GP, width);
            for (B3::Bank bank : { GP, FP }) {
                for (const Arg& address : addresses)
                    checkArgTraits(address, role, bank, width);
            }
        }
    }

    B3::Procedure proc;
    StackSlot* slot = proc.code().addStackSlot(8, StackSlotKind::Spill);
    for (const Arg& arg : { Arg::imm(1), Arg::bigImm(1), Arg::stack(slot), Arg::callArg(0), Arg::relCond(MacroAssembler::Equal), Arg::zeroReg() }) {
        forEachTmpWithTraits(arg, Mentions::traitsOf(Arg::Use, GP, Width64), [&](Tmp, unsigned) {
            CHECK(false);
        });
    }
}

void testPatchArgRolesAreStable()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    B3::Air::Special* special = code.addSpecial(makeUniqueWithoutFastMallocCheck<B3::PatchpointSpecial>());
    B3::BasicBlock* b3Root = proc.addBlock();
    for (unsigned numberOfChildren : { 0u, 1u, 2u, 3u, 10u, 50u }) {
        B3::PatchpointValue* patchpoint = b3Root->appendNew<B3::PatchpointValue>(proc, B3::Int64, B3::Origin());
        Vector<Arg> args { Arg::special(special), Arg(code.newTmp(GP)) };
        for (unsigned i = 0; i < numberOfChildren; ++i) {
            bool isDouble = i % 3 == 2;
            B3::Value* child = isDouble ? static_cast<B3::Value*>(b3Root->appendNew<B3::ConstDoubleValue>(proc, B3::Origin(), 1.5)) : b3Root->appendNew<B3::Const64Value>(proc, B3::Origin(), i);
            patchpoint->append(child, i % 2 ? B3::ValueRep::SomeRegister : B3::ValueRep::LateColdAny);
            args.append(Arg(code.newTmp(isDouble ? FP : GP)));
        }
        Inst inst(Patch, patchpoint, WTF::move(args));
        struct Seen {
            Arg arg;
            Arg::Role role;
            B3::Bank bank;
            Width width;
        };
        auto ask = [](Inst& asked) {
            Vector<Seen> result;
            asked.forEachArg([&](Arg& arg, Arg::Role role, B3::Bank bank, Width width) {
                result.append({ arg, role, bank, width });
            });
            CHECK(asked.hasCurrentForms());
            return result;
        };
        Vector<Seen> first = ask(inst);
        CHECK(first.size() == numberOfChildren + 2);
        CHECK(first[0].arg.isSpecial() && first[0].role == Arg::Use && first[0].bank == GP);
        CHECK(first[1].role == Arg::Def && first[1].bank == GP && first[1].width == Width64);
        for (unsigned i = 0; i < numberOfChildren; ++i) {
            CHECK(first[i + 2].role == (i % 2 ? Arg::Use : Arg::LateColdUse));
            CHECK(first[i + 2].bank == (i % 3 == 2 ? FP : GP));
            CHECK(first[i + 2].width == Width64);
        }
        for (unsigned round = 0; round < 3; ++round) {
            Inst copy = inst;
            if (round == 2)
                copy.fillForms();
            Vector<Seen> again = ask(round == 1 ? inst : copy);
            CHECK(again.size() == first.size());
            for (unsigned i = 0; i < first.size(); ++i)
                CHECK(again[i].arg == first[i].arg && again[i].role == first[i].role && again[i].bank == first[i].bank && again[i].width == first[i].width);
        }
        if (numberOfChildren == 3) {
            patchpoint->setConstrainedChild(0, B3::ConstrainedValue(patchpoint->child(0), B3::ValueRep::SomeRegister));
            CHECK(!inst.hasCurrentForms());
        }
    }
}

using LiveSetWithWords = BitMatrixLiveness<TmpRows>::Workset;

struct LiveSetWithoutWords {
    template<typename Functor>
    void forEachSetBit(const Functor& functor) const { set.forEachSetBit(functor); }

    const LiveSetWithWords& set;
};

enum class EdgeSource : uint8_t { Add, LiveSetWithWords, LiveSetWithoutWords };

template<typename Graph>
static void buildRandomInterferenceGraph(Graph& graph, unsigned vertexCount, EdgeSource source)
{
    using IndexType = typename Graph::IndexType;
    constexpr unsigned liveSetSize = 16;
    WeakRandom random(vertexCount);
    graph.setMaxIndex(vertexCount);
    for (unsigned step = 0; step < vertexCount; ++step) {
        LiveSetWithWords live((vertexCount + 63) / 64);
        for (unsigned i = 0; i < liveSetSize; ++i)
            live.add(random.getUint32(vertexCount));
        auto u = static_cast<IndexType>(random.getUint32(vertexCount));
        auto v = static_cast<IndexType>(random.getUint32(vertexCount));
        if (random.getUint32(2))
            live.add(u);
        if (u != v)
            graph.add(u, v);
        switch (source) {
        case EdgeSource::Add:
            live.forEachSetBit([&](unsigned other) {
                if (other != u)
                    graph.add(u, static_cast<IndexType>(other));
            });
            break;
        case EdgeSource::LiveSetWithWords:
            graph.addAllBut(u, live);
            break;
        case EdgeSource::LiveSetWithoutWords:
            graph.addAllBut(u, LiveSetWithoutWords { live });
            break;
        }
    }
    graph.mirrorEdges();
}

struct VertexNeighbors {
    Vector<unsigned> all;
    Vector<unsigned> interfering;
    Vector<unsigned> nonInterfering;
    size_t interferingCount { 0 };

    friend bool operator==(const VertexNeighbors&, const VertexNeighbors&) = default;
};

template<typename Graph>
static Vector<VertexNeighbors> collectNeighbors(Graph& graph, unsigned vertexCount, std::span<const uint64_t> others)
{
    using IndexType = typename Graph::IndexType;
    Vector<VertexNeighbors> result(vertexCount);
    for (unsigned vertex = 0; vertex < vertexCount; ++vertex) {
        auto u = static_cast<IndexType>(vertex);
        VertexNeighbors& neighbors = result[vertex];
        for (IndexType v : graph[u]) {
            CHECK(graph.contains(u, v) && graph.contains(v, u));
            neighbors.all.append(v);
        }
        graph.forEachInterferingIn(u, others, [&](unsigned v) {
            neighbors.interfering.append(v);
        });
        graph.forEachNonInterferingIn(u, others, [&](unsigned v) {
            CHECK(!graph.contains(u, static_cast<IndexType>(v)));
            neighbors.nonInterfering.append(v);
        });
        neighbors.interferingCount = graph.countInterferingIn(u, others);
        CHECK(neighbors.interferingCount == neighbors.interfering.size());
        for (Vector<unsigned>* list : { &neighbors.all, &neighbors.interfering, &neighbors.nonInterfering })
            std::ranges::sort(*list);
    }
    return result;
}

void testInterferenceGraphsAgree()
{
    // The graphs do not depend on the optimization level.
    if (Options::defaultB3OptLevel() != 2)
        return;

    for (unsigned vertexCount : { 100u, 1023u, 1024u, 3000u }) {
        WeakRandom random(vertexCount);
        LiveSetWithWords others((vertexCount + 63) / 64);
        for (unsigned vertex = 0; vertex < vertexCount; ++vertex) {
            if (random.getUint32(2))
                others.add(vertex);
        }

        LargeIterableInterferenceGraph reference;
        buildRandomInterferenceGraph(reference, vertexCount, EdgeSource::Add);
        Vector<VertexNeighbors> expected = collectNeighbors(reference, vertexCount, others.words());
        size_t sumOfDegrees = 0;
        for (const VertexNeighbors& neighbors : expected)
            sumOfDegrees += neighbors.all.size();
        CHECK(sumOfDegrees);

        for (EdgeSource source : { EdgeSource::Add, EdgeSource::LiveSetWithWords, EdgeSource::LiveSetWithoutWords }) {
            DenseIterableInterferenceGraph dense;
            LargeIterableInterferenceGraph sparse;
            buildRandomInterferenceGraph(dense, vertexCount, source);
            buildRandomInterferenceGraph(sparse, vertexCount, source);
            CHECK(collectNeighbors(dense, vertexCount, others.words()) == expected);
            CHECK(collectNeighbors(sparse, vertexCount, others.words()) == expected);
            // InterferenceRows counts an edge once, UndirectedEdgesDuplicatingAdapter once per direction.
            CHECK(dense.size() * 2 == sumOfDegrees);
            CHECK(sparse.size() == sumOfDegrees);
        }
    }
}

static void checkLiveSlotsDoNotOverlap(Code& code)
{
    Liveness<StackSlotReferenceAdapter> liveness(code);
    for (BasicBlock* block : code) {
        Liveness<StackSlotReferenceAdapter>::LocalCalc localCalc(liveness, block);
        for (unsigned instIndex = block->size(); instIndex--;) {
            HashSet<intptr_t> liveOffsets;
            for (StackSlot* slot : localCalc.live())
                CHECK(liveOffsets.add(slot->offsetFromFP()).isNewEntry);
            block->at(instIndex).forEach<StackSlot*>([&](StackSlot*& slot, Arg::Role role, B3::Bank, Width) {
                if (Arg::isAnyDef(role) && !localCalc.isLive(slot))
                    CHECK(!liveOffsets.contains(slot->offsetFromFP()));
            });
            localCalc.execute(instIndex);
        }
    }
}

static void buildSpillSlotChain(Code& code, unsigned slotCount, unsigned slotsLiveTogether)
{
    constexpr unsigned slotsPerBlock = 64;
    Vector<StackSlot*> slots;
    for (unsigned i = 0; i < slotCount; ++i)
        slots.append(code.addStackSlot(8, StackSlotKind::Spill));

    BasicBlock* block = code.addBlock();
    auto appendAddition = [&](unsigned slotIndex) {
        block->append(Move, nullptr, Arg::stack(slots[slotIndex]), argumentTmp(1));
        block->append(Add64, nullptr, argumentTmp(1), returnTmp());
    };
    block->append(Move, nullptr, Arg::imm(0), returnTmp());
    for (unsigned k = 0; k < slotCount; ++k) {
        if (k && !(k % slotsPerBlock)) {
            BasicBlock* next = code.addBlock();
            appendJump(block, next);
            block = next;
        }
        block->append(Move, nullptr, argumentTmp(0), Arg::stack(slots[k]));
        if (k >= slotsLiveTogether)
            appendAddition(k - slotsLiveTogether);
    }
    for (unsigned k = slotCount - slotsLiveTogether; k < slotCount; ++k)
        appendAddition(k);
    appendReturn(block);
}

enum class InterferenceGraphForm : uint8_t { Dense, Sparse };

static Vector<intptr_t> allocateSpillSlotChain(unsigned slotCount, unsigned slotsLiveTogether, uint64_t limit, InterferenceGraphForm expectedForm)
{
    B3::Policy policy;
    policy.limits.set(B3::Limit::MaxSlotsForDenseInterference, limit);
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    buildSpillSlotChain(code, slotCount, slotsLiveTogether);
    code.resetReachability();
    allocateStackByGraphColoring(code);
    CHECK(proc.report().hitsOf(B3::Limit::MaxSlotsForDenseInterference) == (expectedForm == InterferenceGraphForm::Sparse ? 1u : 0u));
    checkLiveSlotsDoNotOverlap(code);

    Vector<intptr_t> offsets;
    for (StackSlot* slot : code.stackSlots())
        offsets.append(slot->offsetFromFP());
    return offsets;
}

void testDenseInterferenceLimit()
{
    constexpr size_t limit = B3::AirLimits::maxSlotsForDenseInterference;
    constexpr size_t slotCount = limit + 1;
    constexpr size_t bytesOfRows = slotCount * slotCount / 8;
    constexpr size_t bytesPerEdge = 2 * B3::AirLimits::bytesPerNeighborInSparseInterference;
    constexpr size_t fewSlotsLiveTogether = 64;
    constexpr size_t manySlotsLiveTogether = 128;
    static_assert(slotCount * (fewSlotsLiveTogether + 1) * bytesPerEdge < bytesOfRows);
    static_assert((slotCount - manySlotsLiveTogether) * manySlotsLiveTogether * bytesPerEdge >= bytesOfRows);

    allocateSpillSlotChain(limit, 8, limit, InterferenceGraphForm::Dense);
    allocateSpillSlotChain(slotCount, 8, limit, InterferenceGraphForm::Sparse);
    allocateSpillSlotChain(slotCount, manySlotsLiveTogether, limit, InterferenceGraphForm::Dense);

    Vector<intptr_t> offsetsFromSets = allocateSpillSlotChain(slotCount, fewSlotsLiveTogether, limit, InterferenceGraphForm::Sparse);
    Vector<intptr_t> offsetsFromRows = allocateSpillSlotChain(slotCount, fewSlotsLiveTogether, slotCount, InterferenceGraphForm::Dense);
    CHECK(offsetsFromSets == offsetsFromRows);
}

// One more slot than an interference graph with 16-bit indexes holds.
constexpr unsigned slotCountBeyondGraphs = std::numeric_limits<uint16_t>::max();

static size_t countDistinctOffsets(Code& code)
{
    HashSet<intptr_t> offsets;
    for (StackSlot* slot : code.stackSlots())
        offsets.add(slot->offsetFromFP());
    return offsets.size();
}

static void appendSlotStoreAndLoad(BasicBlock* block, StackSlot* slot)
{
    block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    block->append(Move, nullptr, Arg::stack(slot), argumentTmp(1));
}

void testManySlotsShareCells()
{
    for (unsigned slotsLiveTogether : { 1u, 8u, 100u }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        // With the most slots, liveness is solved in slices.
        buildSpillSlotChain(code, slotsLiveTogether == 100 ? 2 * slotCountBeyondGraphs : slotCountBeyondGraphs, slotsLiveTogether);
        code.resetReachability();
        allocateStackByGraphColoring(code);
        checkLiveSlotsDoNotOverlap(code);
        CHECK(countDistinctOffsets(code) == slotsLiveTogether + 1);
    }

    {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        buildSpillSlotChain(code, slotCountBeyondGraphs, slotCountBeyondGraphs);
        code.resetReachability();
        allocateStackByGraphColoring(code);
        CHECK(countDistinctOffsets(code) == slotCountBeyondGraphs);
    }

    // The first two slots are dead while the others are used, so every slot fits into one cell.
    {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        Vector<StackSlot*> slots;
        for (unsigned i = 0; i < slotCountBeyondGraphs; ++i)
            slots.append(code.addStackSlot(8, StackSlotKind::Spill));
        BasicBlock* block = code.addBlock();
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (i && !(i % 64)) {
                BasicBlock* next = code.addBlock();
                appendJump(block, next);
                block = next;
            }
            appendSlotStoreAndLoad(block, slots[i]);
        }
        appendSlotStoreAndLoad(block, slots[1]);
        appendSlotStoreAndLoad(block, slots[0]);
        appendReturn(block);
        code.resetReachability();
        allocateStackByGraphColoring(code);
        checkLiveSlotsDoNotOverlap(code);
        CHECK(countDistinctOffsets(code) == 1);
    }
}

void testManySlotsDropMovesToThemselves()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    Vector<StackSlot*> slots;
    for (unsigned i = 0; i < slotCountBeyondGraphs; ++i)
        slots.append(code.addStackSlot(8, StackSlotKind::Spill));
    BasicBlock* block = code.addBlock();
    for (unsigned i = 0; i < slots.size(); ++i) {
        if (i && !(i % 64)) {
            BasicBlock* next = code.addBlock();
            appendJump(block, next);
            block = next;
        }
        block->append(Move, nullptr, argumentTmp(0), Arg::stack(slots[i]));
        block->append(Move, nullptr, Arg::stack(slots[i]), Arg::stack(slots[i]), argumentTmp(2));
        block->append(Move, nullptr, Arg::stack(slots[i]), argumentTmp(1));
    }
    appendReturn(block);
    code.resetReachability();
    allocateStackByGraphColoring(code);
    checkLiveSlotsDoNotOverlap(code);
    for (BasicBlock* block : code) {
        for (Inst& inst : *block)
            CHECK(inst.args().size() != 3);
    }
}

void testManySlotsInRandomGraphs()
{
    constexpr unsigned sharedSlotCount = 48;
    for (unsigned seed = 1; seed <= 4; ++seed) {
        // With the most blocks, liveness is solved in slices.
        const unsigned blockCount = seed % 2 ? 256 : 2048;
        const unsigned slotsPerBlock = (slotCountBeyondGraphs + blockCount - 1) / blockCount;
        WeakRandom random(seed);
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        Vector<StackSlot*> sharedSlots;
        for (unsigned i = 0; i < sharedSlotCount; ++i)
            sharedSlots.append(code.addStackSlot(8, StackSlotKind::Spill));
        Vector<BasicBlock*> blocks;
        for (unsigned i = 0; i < blockCount; ++i)
            blocks.append(code.addBlock());
        for (unsigned i = 0; i < blockCount; ++i) {
            BasicBlock* block = blocks[i];
            for (unsigned k = 0; k < slotsPerBlock; ++k) {
                appendSlotStoreAndLoad(block, code.addStackSlot(8, StackSlotKind::Spill));
                if (random.getUint32(8))
                    continue;
                StackSlot* shared = sharedSlots[random.getUint32(sharedSlotCount)];
                if (random.getUint32(2))
                    block->append(Move, nullptr, argumentTmp(0), Arg::stack(shared));
                else
                    block->append(Move, nullptr, Arg::stack(shared), argumentTmp(1));
            }
            if (i + 1 == blockCount)
                appendReturn(block);
            else if (random.getUint32(3))
                appendBranchTest(block, argumentTmp(0), blocks[i + 1], blocks[1 + random.getUint32(blockCount - 1)]);
            else
                appendJump(block, blocks[i + 1]);
        }
        code.resetReachability();
        CHECK(code.stackSlots().size() >= slotCountBeyondGraphs);
        allocateStackByGraphColoring(code);
        checkLiveSlotsDoNotOverlap(code);
        CHECK(countDistinctOffsets(code) <= sharedSlotCount + 1);
    }
}

void testManySlotsOfMixedSizes()
{
    for (bool areLiveTogether : { false, true }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        Vector<StackSlot*> slots;
        for (unsigned i = 0; i < slotCountBeyondGraphs; ++i)
            slots.append(code.addStackSlot(i % 3 ? i % 3 * 4 : 16, StackSlotKind::Spill));
        constexpr unsigned usedSlotCount = 300;
        BasicBlock* root = code.addBlock();
        auto opcodeFor = [](StackSlot* slot) {
            return slot->byteSize() == 4 ? Move32 : Move;
        };
        for (unsigned i = 0; i < usedSlotCount; ++i) {
            root->append(opcodeFor(slots[i]), nullptr, argumentTmp(0), Arg::stack(slots[i]));
            if (!areLiveTogether)
                root->append(opcodeFor(slots[i]), nullptr, Arg::stack(slots[i]), argumentTmp(1));
        }
        for (unsigned i = 0; areLiveTogether && i < usedSlotCount; ++i)
            root->append(opcodeFor(slots[i]), nullptr, Arg::stack(slots[i]), argumentTmp(1));
        appendReturn(root);
        code.resetReachability();
        allocateStackByGraphColoring(code);

        intptr_t lowestOffset = 0;
        for (StackSlot* slot : slots) {
            CHECK(slot->offsetFromFP() < 0);
            CHECK(!(slot->offsetFromFP() % static_cast<intptr_t>(slot->alignment())));
            lowestOffset = std::min(lowestOffset, slot->offsetFromFP());
        }
        Vector<StackSlot*> slotAtByte(FillWith { }, static_cast<size_t>(-lowestOffset), nullptr);
        for (unsigned i = 0; areLiveTogether && i < usedSlotCount; ++i) {
            for (unsigned byte = 0; byte < slots[i]->byteSize(); ++byte)
                CHECK(!std::exchange(slotAtByte[-slots[i]->offsetFromFP() - 1 - byte], slots[i]));
        }
        if (!areLiveTogether)
            CHECK(-lowestOffset <= 16 + 8 + 4 + 16);
    }
}

enum class SlotLifetimes : uint8_t { Overlapping, Disjoint };

static Vector<StackSlot*> buildSlotsOfMixedSizes(Code& code, SlotLifetimes lifetimes)
{
    Vector<StackSlot*> slots;
    for (unsigned byteSize : { 4u, 8u, 16u, 4u, 16u, 8u, 4u, 4u, 8u, 16u, 4u })
        slots.append(code.addStackSlot(byteSize, StackSlotKind::Spill));

    BasicBlock* root = code.addBlock();
    if (lifetimes == SlotLifetimes::Overlapping) {
        for (StackSlot* slot : slots)
            appendSlotStore(root, slot);
    }
    for (StackSlot* slot : slots) {
        if (lifetimes == SlotLifetimes::Disjoint)
            appendSlotStore(root, slot);
        appendSlotLoad(root, slot);
    }
    appendReturn(root);
    code.resetReachability();
    return slots;
}

static void checkFirstFit(const Vector<StackSlot*>& slotsInAssignmentOrder, const StackColoring& coloring, SlotLifetimes lifetimes)
{
    BitVector unitsOfEarlierSlots;
    for (StackSlot* slot : slotsInAssignmentOrder) {
        CHECK(coloring.unitsOfSlot[slot->index()]);
        StackColoring::UnitRange units = *coloring.unitsOfSlot[slot->index()];
        const unsigned end = units.last + 1;
        const unsigned unitsNeeded = slot->byteSize() / StackColoring::bytesPerUnit;
        const unsigned unitsPerAlignment = slot->alignment() / StackColoring::bytesPerUnit;
        CHECK(end - units.first == unitsNeeded);
        CHECK(!(end % unitsPerAlignment));
        CHECK(end <= coloring.numberOfUnits);
        if (lifetimes == SlotLifetimes::Disjoint) {
            CHECK(!units.first);
            continue;
        }

        auto overlapsEarlierSlot = [&](unsigned candidateEnd) {
            for (unsigned unit = candidateEnd - unitsNeeded; unit < candidateEnd; ++unit) {
                if (unitsOfEarlierSlots.get(unit))
                    return true;
            }
            return false;
        };
        for (unsigned candidateEnd = WTF::roundUpToMultipleOf(unitsPerAlignment, unitsNeeded); candidateEnd < end; candidateEnd += unitsPerAlignment)
            CHECK(overlapsEarlierSlot(candidateEnd));
        CHECK(!overlapsEarlierSlot(end));
        for (unsigned unit = units.first; unit < end; ++unit)
            unitsOfEarlierSlots.set(unit);
    }
}

void testFirstFitSizes()
{
    for (SlotLifetimes lifetimes : { SlotLifetimes::Overlapping, SlotLifetimes::Disjoint }) {
        B3::Policy policy;
        letAirEmitFrame(policy.air);
        B3::Procedure coloredProcedure(policy);
        coloredProcedure.setOptLevel(2);
        Vector<StackSlot*> coloredSlots = buildSlotsOfMixedSizes(coloredProcedure.code(), lifetimes);
        StackColoring coloring = allocateStackByGraphColoring(coloredProcedure.code());
        checkFirstFit(coloredSlots, coloring, lifetimes);

        B3::Procedure proc;
        proc.setOptLevel(2);
        Vector<StackSlot*> slots = buildSlotsOfMixedSizes(proc.code(), lifetimes);
        CHECK(allocateStackByGraphColoring(proc.code()).unitsOfSlot.isEmpty());
        for (size_t i = 0; i < slots.size(); ++i) {
            intptr_t bytesBelowFramePointer = StackColoring::bytesPerUnit * (coloring.unitsOfSlot[coloredSlots[i]->index()]->last + 1);
            CHECK(slots[i]->offsetFromFP() == -bytesBelowFramePointer);
        }
    }
}

void testNoEmptyInstAfterStackAllocation()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    proc.setNeedsUsedRegisters(false);
    Code& code = proc.code();
    Arg first = Arg::stack(code.addStackSlot(8, StackSlotKind::Spill));
    Arg second = Arg::stack(code.addStackSlot(8, StackSlotKind::Spill));
    BasicBlock* root = code.addBlock();
    BasicBlock* next = code.addBlock();
    BasicBlock* redundantReload = code.addBlock();
    BasicBlock* deadStore = code.addBlock();
    BasicBlock* selfMove = code.addBlock();
    BasicBlock* done = code.addBlock();

    root->append(Move, nullptr, argumentTmp(0), first);
    appendBranchTest(root, argumentTmp(1), redundantReload, next);
    appendBranchTest(next, argumentTmp(2), deadStore, selfMove);
    redundantReload->append(Move, nullptr, first, argumentTmp(0));
    deadStore->append(Move, nullptr, argumentTmp(1), second);
    deadStore->append(Move, nullptr, second, argumentTmp(3));
    selfMove->append(Move, nullptr, first, first, argumentTmp(2));
    for (BasicBlock* block : { redundantReload, deadStore, selfMove })
        appendJump(block, done);
    done->append(Move, nullptr, first, returnTmp());
    appendReturn(done);

    auto compilation = compile(proc);
    for (BasicBlock* block : code) {
        for (Inst& inst : *block)
            CHECK(inst);
    }
    CHECK(invoke<int64_t>(*compilation, 5ll, 1ll, 0ll) == 5);
    CHECK(invoke<int64_t>(*compilation, 5ll, 0ll, 1ll) == 5);
    CHECK(invoke<int64_t>(*compilation, 5ll, 0ll, 0ll) == 5);
}

static RegisterSet chooseRandomCallClobbers(WeakRandom& random)
{
    RegisterSet result;
    callClobberedRegisters().forEach([&](Reg reg) {
        if (random.getUint32(2))
            result.add(reg, IgnoreVectors);
    });
    return result;
}

static B3::PatchpointValue* addClobberingPatchpoint(B3::Procedure& proc, B3::Type type, const RegisterSet& clobbers)
{
    bool hasResult = type != B3::Void;
    B3::PatchpointValue* patchpoint = addPatchpoint(proc, type);
    patchpoint->clobberLate(clobbers);
    patchpoint->setGenerator([hasResult, clobbers](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        if (hasResult)
            jit.add64(CCallHelpers::TrustedImm32(1), params[1].gpr(), params[0].gpr());
        clobbers.forEach([&](Reg reg) {
            if (!hasResult || reg != Reg(params[0].gpr()))
                jit.move(CCallHelpers::TrustedImm32(-1), reg.gpr());
        });
    });
    return patchpoint;
}

static void checkValuesSurviveClobbers(bool enablesAllocatorFields, unsigned seed)
{
    constexpr unsigned argumentCount = 6;
    auto argumentValue = [](unsigned index) {
        return 1000003ull * (index + 1);
    };

    WeakRandom random(seed);
    B3::Policy policy;
    if (enablesAllocatorFields)
        enableAllAllocatorFields(policy.air);
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    B3::Air::Special* special = patchpointSpecialOf(code);
    BasicBlock* root = code.addBlock();

    unsigned valueCount = 4 + random.getUint32(24);
    Vector<Tmp> values;
    Vector<uint64_t> model;
    for (unsigned i = 0; i < valueCount; ++i) {
        values.append(code.newTmp(GP));
        model.append(argumentValue(i % argumentCount) + i);
        root->append(Move, nullptr, argumentTmp(i % argumentCount), values[i]);
        root->append(Add64, nullptr, Arg::imm(i), values[i]);
    }

    unsigned stepCount = 10 + random.getUint32(40);
    for (unsigned step = 0; step < stepCount; ++step) {
        unsigned target = random.getUint32(valueCount);
        unsigned source = random.getUint32(valueCount);
        switch (random.getUint32(3)) {
        case 0:
            root->append(Add64, nullptr, values[source], values[target]);
            model[target] += model[source];
            break;
        case 1:
            root->append(Patch, addClobberingPatchpoint(proc, B3::Void, chooseRandomCallClobbers(random)), Arg::special(special));
            break;
        case 2: {
            Tmp argumentRegister = argumentTmp(random.getUint32(argumentCount));
            RegisterSet clobbers = chooseRandomCallClobbers(random);
            clobbers.add(argumentRegister.reg(), IgnoreVectors);
            B3::PatchpointValue* patchpoint = addClobberingPatchpoint(proc, B3::Int64, clobbers);
            patchpoint->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::reg(argumentRegister.reg()));
            Tmp result = code.newTmp(GP);
            root->append(Move, nullptr, values[source], argumentRegister);
            root->append(Patch, patchpoint, Arg::special(special), result, argumentRegister);
            model[target] = model[source] + 1;
            values[target] = result;
            break;
        }
        }
    }

    Tmp sum = code.newTmp(GP);
    uint64_t expected = 0;
    root->append(Move, nullptr, Arg::imm(0), sum);
    for (unsigned i = 0; i < valueCount; ++i) {
        root->append(Add64, nullptr, values[i], sum);
        expected += model[i];
    }
    root->append(Move, nullptr, sum, returnTmp());
    appendReturn(root);

    auto compilation = compile(proc);
    int64_t actual = invoke<int64_t>(*compilation,
        static_cast<int64_t>(argumentValue(0)), static_cast<int64_t>(argumentValue(1)), static_cast<int64_t>(argumentValue(2)),
        static_cast<int64_t>(argumentValue(3)), static_cast<int64_t>(argumentValue(4)), static_cast<int64_t>(argumentValue(5)));
    CHECK(static_cast<uint64_t>(actual) == expected);
}

void testValuesSurviveRandomClobbers()
{
    for (bool enablesAllocatorFields : { false, true }) {
        for (unsigned seed = 1; seed <= 100; ++seed)
            checkValuesSurviveClobbers(enablesAllocatorFields, seed);
    }
}

void testValuesLiveAcross32And33Calls()
{
    // Two fifths of the values have enough uses to pay for 33 splits. No CPU has a callee save for each of them.
    constexpr unsigned valueCount = 40;
    static_assert(B3::AirLimits::maxClobberSitesPerSplit == 32);
    int64_t memory[valueCount];
    for (unsigned i = 0; i < valueCount; ++i)
        memory[i] = (i + 1) * 1000003ll;
    for (unsigned callCount : { 32u, 33u }) {
        for (bool limitsClobberSites : { false, true }) {
            B3::Policy policy;
            policy.air.splitting.limitsClobberSitesPerSplit = limitsClobberSites;
            B3::Procedure proc(policy);
            int64_t expected = buildSumAcrossCalls(proc, valueCount, callCount, memory);
            CHECK(compileAndRun<int64_t>(proc, memory) == expected);
            CHECK(!!proc.report().hitsOf(B3::Limit::MaxClobberSitesPerSplit) == (limitsClobberSites && callCount == 33));
        }
    }
}

static void checkDenseBlockOrder(Code& code, std::initializer_list<BasicBlock*> expectedOrder)
{
    checkBlockOrder(code, expectedOrder);
    for (unsigned index = 0; index < code.size(); ++index)
        CHECK(code[index]->index() == index);
}

void testBlocksWithoutSuccessorsMoveToEnd()
{
    {
        B3::Procedure proc;
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* firstExit = code.addBlock();
        BasicBlock* unreachable = code.addBlock();
        BasicBlock* middle = code.addBlock();
        BasicBlock* secondExit = code.addBlock();
        BasicBlock* last = code.addBlock();
        BasicBlock* thirdExit = code.addBlock();
        appendBranchTest(root, argumentTmp(0), firstExit, middle);
        appendBranchTest(middle, argumentTmp(1), secondExit, last);
        appendJump(last, thirdExit);
        for (BasicBlock* block : { firstExit, unreachable, secondExit, thirdExit })
            appendReturn(block, 1);
        code.resetReachability();
        moveBlocksWithoutSuccessorsToEnd(code);
        checkDenseBlockOrder(code, { root, middle, last, firstExit, secondExit, thirdExit });
    }
    {
        B3::Procedure proc;
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* jump = code.addBlock();
        BasicBlock* exit = code.addBlock();
        appendReturn(root, 1);
        appendJump(jump, exit);
        appendReturn(exit, 2);
        moveBlocksWithoutSuccessorsToEnd(code);
        checkDenseBlockOrder(code, { root, jump, exit });
    }
    for (bool movesBlocks : { false, true }) {
        B3::Policy policy;
        policy.validation.level = B3::ValidationLevel::AtEachPhase;
        policy.air.layout.movesBlocksWithoutSuccessorsToEnd = movesBlocks;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        BasicBlock* root = proc.code().addBlock();
        PhaseRecordingSpecial* recorder = appendPhaseRecorder(proc.code(), root);
        appendReturn(root, 1);
        prepareForGeneration(proc.code());
        CHECK(recorder->didRun("eliminateDeadCode"_s));
        CHECK(recorder->didRun("moveBlocksWithoutSuccessorsToEnd"_s) == movesBlocks);
    }
}

void testDeadDefSurvivesAllocation()
{
    constexpr unsigned valueCount = 30;
    int64_t memory[valueCount];
    for (unsigned i = 0; i < valueCount; ++i)
        memory[i] = (i + 1) * 1000003ll;
    B3::Procedure proc;
    int64_t expected = buildSumAcrossCalls(proc, valueCount, 1, memory);
    Code& code = proc.code();
    BasicBlock* root = code[0];
    Tmp sum = root->at(root->size() - 2).args()[0].tmp();
    root->insts().insert(root->size() / 2, Inst(Move, nullptr, Arg::imm(1), code.newTmp(GP)));
    root->insts().insert(root->size() / 2, Inst(Move64ToDouble, nullptr, sum, code.newTmp(FP)));

    code.resetReachability();
    allocateRegistersByGreedy(code);
    for (Inst& inst : *root) {
        inst.forEachTmpFast([&](Tmp& tmp) {
            CHECK(tmp.isReg());
        });
    }
    CHECK(compileAndRun<int64_t>(proc, memory) == expected);
}

void testClobberValidationSeesDeadClobbers()
{
    CHECK(Options::airValidateGreedRegAlloc());
    for (bool alsoClobbersEarly : { false, true }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        const Vector<Reg>& registers = code.regsInPriorityOrder(GP);
        RegisterSet clobbers;
        clobbers.add(*std::ranges::find_if(registers, [](Reg reg) {
            return reg != Reg(GPRInfo::argumentGPR0) && reg != Reg(GPRInfo::returnValueGPR);
        }), IgnoreVectors);
        B3::PatchpointValue* patchpoint = addClobberingPatchpoint(proc, B3::Void, clobbers);
        if (alsoClobbersEarly)
            patchpoint->clobberEarly(clobbers);

        BasicBlock* root = code.addBlock();
        Tmp base = code.newTmp(GP);
        Tmp sum = code.newTmp(GP);
        Vector<Tmp> values;
        Vector<int64_t> memory;
        int64_t expected = 0;
        root->append(Move, nullptr, argumentTmp(0), base);
        for (unsigned i = 0; i < registers.size(); ++i) {
            values.append(code.newTmp(GP));
            memory.append((i + 1) * 1000003ll);
            expected += memory[i];
            root->append(Move, nullptr, Arg::addr(base, static_cast<int32_t>(i * sizeof(int64_t))), values[i]);
        }
        root->append(Patch, patchpoint, Arg::special(patchpointSpecialOf(code)));
        root->append(Move, nullptr, Arg::imm(0), sum);
        for (Tmp value : values)
            root->append(Add64, nullptr, value, sum);
        root->append(Move, nullptr, sum, returnTmp());
        appendReturn(root);
        CHECK(compileAndRun<int64_t>(proc, memory.span().data()) == expected);
    }
}

static void checkInstText(BasicBlock* block, std::initializer_list<Inst> expected)
{
    CHECK(block->size() == expected.size());
    unsigned index = 0;
    for (const Inst& inst : expected)
        CHECK(toString(block->at(index++)) == toString(inst));
}

void testClusterSplitWithinBlockIsUpstream()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    Tmp base = argumentTmp(0);
    Vector<Tmp> allowed;
    for (Reg reg : Vector<Reg>(code.regsInPriorityOrder(GP))) {
        if (allowed.size() < 2 && reg != base.reg())
            allowed.append(Tmp(reg));
        else
            code.pinRegister(reg);
    }
    auto word = [&](unsigned index) {
        return Arg::addr(base, static_cast<int32_t>(index * sizeof(int64_t)));
    };

    BasicBlock* defines = code.addBlock();
    BasicBlock* crowded = code.addBlock();
    BasicBlock* uses = code.addBlock();
    Tmp split = code.newTmp(GP);
    Tmp firstFastTmp = code.newTmp(GP);
    Tmp secondFastTmp = code.newTmp(GP);
    code.addFastTmp(firstFastTmp);
    code.addFastTmp(secondFastTmp);
    defines->append(Move, nullptr, word(0), split);
    defines->append(Add64, nullptr, Arg::imm(1), split);
    appendJump(defines, crowded);
    crowded->append(Move, nullptr, word(1), firstFastTmp);
    crowded->append(Move, nullptr, word(2), secondFastTmp);
    crowded->append(Add64, nullptr, firstFastTmp, secondFastTmp);
    crowded->append(Move, nullptr, secondFastTmp, word(3));
    appendJump(crowded, uses);
    uses->append(Move, nullptr, split, word(4));
    uses->append(Move, nullptr, split, word(5));
    uses->append(Oops, nullptr);

    code.resetReachability();
    allocateRegistersByGreedy(code);

    CHECK(code.stackSlots().size() == 1);
    StackSlot* slot = code.stackSlots()[0];
    CHECK(slot->isSpill() && slot->byteSize() == sizeof(int64_t));
    checkInstText(defines, { Inst(Move, nullptr, word(0), allowed[0]), Inst(Add64, nullptr, Arg::imm(1), allowed[0]), Inst(Move, nullptr, allowed[0], Arg::stack(slot)), Inst(Jump, nullptr) });
    checkInstText(crowded, { Inst(Move, nullptr, word(1), allowed[1]), Inst(Move, nullptr, word(2), allowed[0]), Inst(Add64, nullptr, allowed[1], allowed[0]), Inst(Move, nullptr, allowed[0], word(3)), Inst(Jump, nullptr) });
    checkInstText(uses, { Inst(Move, nullptr, Arg::stack(slot), allowed[0]), Inst(Move, nullptr, allowed[0], word(4)), Inst(Move, nullptr, allowed[0], word(5)), Inst(Oops, nullptr) });
}

void testCoalescedDoublesAreSpilled()
{
    constexpr unsigned pairCount = 40;
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();

    Vector<Tmp> copies;
    double memory[pairCount];
    double expected = 0;
    for (unsigned i = 0; i < pairCount; ++i) {
        memory[i] = i + 0.5;
        expected += memory[i];
        Tmp original = code.newTmp(FP);
        copies.append(code.newTmp(FP));
        root->append(MoveDouble, nullptr, Arg::addr(argumentTmp(0), static_cast<int32_t>(i * sizeof(double))), original);
        root->append(MoveDouble, nullptr, original, copies[i]);
    }

    B3::PatchpointValue* patchpoint = addPatchpoint(proc);
    patchpoint->clobberLate(RegisterSet::allFPRs());
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        RegisterSet::allFPRs().forEach([&](Reg reg) {
            jit.moveZeroToDouble(reg.fpr());
        });
    });
    root->append(Patch, patchpoint, Arg::special(patchpointSpecialOf(code)));

    Tmp sum = code.newTmp(FP);
    root->append(Move, nullptr, Arg::imm(0), argumentTmp(1));
    root->append(Move64ToDouble, nullptr, argumentTmp(1), sum);
    for (Tmp copy : copies)
        root->append(AddDouble, nullptr, copy, sum, sum);
    root->append(MoveDouble, nullptr, sum, Tmp(FPRInfo::returnValueFPR));
    root->append(RetDouble, nullptr, Tmp(FPRInfo::returnValueFPR));

    CHECK(compileAndRun<double>(proc, memory) == expected);
}

void testBranchDefinesTmpAtOptLevel0()
{
    enum class Case : uint8_t { ReadAfterJoin, ReadOnOnePath, NeverRead, SuccessorIsJoin };
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
    for (Case testCase : { Case::ReadAfterJoin, Case::ReadOnOnePath, Case::NeverRead, Case::SuccessorIsJoin }) {
        B3::Procedure proc;
        proc.setOptLevel(0);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* overflowed = code.addBlock();
        BasicBlock* fits = testCase == Case::SuccessorIsJoin ? nullptr : code.addBlock();
        BasicBlock* join = code.addBlock();
        Tmp sum = code.newTmp(GP);
        Tmp other = code.newTmp(GP);
        Tmp kept = code.newTmp(GP);

        appendChecked(root, Move, argumentTmp(0), sum);
        appendChecked(root, Move, argumentTmp(1), other);
        appendChecked(root, Move, argumentTmp(2), kept);
        appendChecked(root, BranchAdd32, Arg::resCond(MacroAssembler::Overflow), other, sum);
        root->setSuccessors(overflowed, fits ? fits : join);
        appendJump(overflowed, join);
        if (testCase == Case::ReadOnOnePath)
            appendChecked(fits, Move, sum, kept);
        if (fits)
            appendJump(fits, join);
        bool returnsSum = testCase == Case::ReadAfterJoin || testCase == Case::SuccessorIsJoin;
        appendChecked(join, Move, returnsSum ? sum : kept, returnTmp());
        appendReturn(join);

        auto compilation = compile(proc);
        CHECK(invokeInt64(*compilation, 1, 2, 9) == (testCase == Case::NeverRead ? 9 : 3));
        CHECK(invokeInt64(*compilation, maxInt32, 1, 9) == (returnsSum ? maxInt32 + 1 : 9));
    }
}
