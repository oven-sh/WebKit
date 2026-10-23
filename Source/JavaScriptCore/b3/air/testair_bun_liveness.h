/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_LIVENESS_TESTS() do { \
        RUN(testBitMatrixLivenessMatchesLiveness()); \
        RUN(testSlicedBitMatrixLivenessMatchesDense()); \
        RUN(testLayoutBoundariesMatchLiveness()); \
        RUN(testSlicedLayoutBoundariesMatchDense()); \
        RUN(testSlicedSolveCallsAdapterInOrder()); \
        RUN(testAllocatorLivenessInSlices()); \
        RUN(testUsedRegistersMatchReference()); \
        RUN(testRegLivenessMatchesTmpLiveness()); \
    } while (false)

struct TmpRows {
    explicit TmpRows(Code& code)
        : code(code)
    {
    }

    unsigned numIndices() const { return Tmp::absoluteIndexEnd(code, GP); }
    bool needsLiveAtTail(BasicBlock*) const { return true; }

    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail) const
    {
        Vector<unsigned, 8> uses;
        Vector<unsigned, 8> defs;
        auto collect = [&](Inst& inst, bool (*isUse)(Arg::Role), bool (*isDef)(Arg::Role)) {
            inst.forEachTmp([&](Tmp& tmp, Arg::Role role, B3::Bank bank, Width) {
                if (bank != GP)
                    return;
                unsigned index = AbsoluteTmpMapper<GP>::absoluteIndex(tmp);
                if (isUse(role))
                    uses.append(index);
                if (isDef(role))
                    defs.append(index);
            });
        };
        auto isInWords = [&](unsigned index) {
            return index / 64 >= firstWord && index / 64 < firstWord + numberOfWords;
        };
        auto bitOf = [](unsigned index) {
            return static_cast<uint64_t>(1) << (index % 64);
        };

        bool didSetBit = false;
        for (size_t boundary = block->size() + 1; boundary--;) {
            uses.shrink(0);
            defs.shrink(0);
            if (boundary < block->size())
                collect(block->at(boundary), Arg::isEarlyUse, Arg::isEarlyDef);
            if (boundary)
                collect(block->at(boundary - 1), Arg::isLateUse, Arg::isLateDef);
            uint64_t* useRow = boundary == block->size() ? tail : gen;
            for (unsigned index : uses) {
                if (!isInWords(index))
                    continue;
                useRow[index / 64 - firstWord] |= bitOf(index);
                didSetBit = true;
            }
            for (unsigned index : defs) {
                if (!isInWords(index))
                    continue;
                gen[index / 64 - firstWord] &= ~bitOf(index);
                kill[index / 64 - firstWord] |= bitOf(index);
                didSetBit = true;
            }
        }
        return didSetBit;
    }

    Code& code;
};

struct SyntheticRows {
    static constexpr unsigned numberOfIndices = 513 * 64;
    static constexpr unsigned genRow = 1;
    static constexpr unsigned killRow = 2;
    static constexpr unsigned tailRow = 3;

    static uint64_t word(unsigned blockIndex, size_t wordIndex, unsigned row)
    {
        uint64_t hash = blockIndex * 2654435761ull ^ wordIndex * 40503ull ^ row * 2246822519ull;
        hash ^= hash >> 15;
        if (row == killRow)
            return hash * 0x9E3779B97F4A7C15ull;
        return hash % 5 ? 0 : 1ull << ((hash >> 8) % 64);
    }

    unsigned numIndices() const { return numberOfIndices; }
    bool needsLiveAtTail(BasicBlock*) const { return true; }

    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail) const
    {
        uint64_t anyBit = 0;
        for (size_t i = 0; i < numberOfWords; ++i) {
            gen[i] = word(block->index(), firstWord + i, genRow);
            kill[i] = word(block->index(), firstWord + i, killRow);
            tail[i] = word(block->index(), firstWord + i, tailRow);
            anyBit |= gen[i] | kill[i] | tail[i];
        }
        return anyBit;
    }
};

struct SyntheticColumnRows {
    explicit SyntheticColumnRows(size_t wordIndex)
        : wordIndex(wordIndex)
    {
    }

    unsigned numIndices() const { return 64; }
    bool needsLiveAtTail(BasicBlock*) const { return true; }

    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail) const
    {
        ASSERT_UNUSED(firstWord, !firstWord);
        ASSERT_UNUSED(numberOfWords, numberOfWords == 1);
        *gen = SyntheticRows::word(block->index(), wordIndex, SyntheticRows::genRow);
        *kill = SyntheticRows::word(block->index(), wordIndex, SyntheticRows::killRow);
        *tail = SyntheticRows::word(block->index(), wordIndex, SyntheticRows::tailRow);
        return *gen | *kill | *tail;
    }

    size_t wordIndex;
};

template<typename Rows>
struct WithLayoutBoundaries : Rows {
    using Rows::Rows;
    static constexpr bool recordsLayoutBoundaries = true;
};

struct RecordingRows : SyntheticRows {
    struct Call {
        size_t firstWord;
        size_t numberOfWords;
        unsigned blockIndex;
    };

    void willComputeInSlices(size_t words)
    {
        CHECK(calls.isEmpty());
        CHECK(!wordsPerSlice);
        wordsPerSlice = words;
    }

    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail)
    {
        calls.append({ firstWord, numberOfWords, block->index() });
        return SyntheticRows::fillRows(block, firstWord, numberOfWords, gen, kill, tail);
    }

    Vector<Call> calls;
    std::optional<size_t> wordsPerSlice;
};

// Compilation: valid Air in which the passes before reportUsedRegisters remove nothing: no self-move, repeated constant, jump-only block or one-target branch.
enum class RandomGraphUse : uint8_t { Analysis, Compilation };

template<typename AppendExtraInst>
static void buildRandomGraph(Code& code, WeakRandom& random, const Vector<Tmp>& tmps, RandomGraphUse use, const AppendExtraInst& appendExtraInst)
{
    unsigned blockCount = 2 + random.getUint32(23);
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < blockCount; ++i)
        blocks.append(code.addBlock());

    unsigned firstTarget = use == RandomGraphUse::Compilation ? 1 : 0;
    unsigned nextImmediate = 0;
    auto randomTmp = [&] {
        return tmps[random.getUint32(tmps.size())];
    };
    auto randomTarget = [&] {
        return blocks[firstTarget + random.getUint32(blockCount - firstTarget)];
    };
    auto appendMove = [](BasicBlock* block, Tmp source, Tmp destination) {
        if (source != destination)
            block->append(Move, nullptr, source, destination);
    };

    for (BasicBlock* block : blocks) {
        appendExtraInst(block);
        CHECK(use == RandomGraphUse::Analysis || block->size());
        unsigned instCount = random.getUint32(7);
        for (unsigned i = 0; i < instCount; ++i) {
            Tmp source = randomTmp();
            Tmp destination = randomTmp();
            switch (random.getUint32(4)) {
            case 0:
                appendMove(block, source, destination);
                break;
            case 1:
                block->append(Add64, nullptr, source, destination);
                break;
            case 2:
                block->append(Move, nullptr, Arg::imm(nextImmediate++), destination);
                break;
            case 3:
                appendMove(block, argumentTmp(0), destination);
                break;
            }
            appendExtraInst(block);
        }

        BasicBlock* firstSuccessor = randomTarget();
        BasicBlock* secondSuccessor = randomTarget();
        switch (random.getUint32(3)) {
        case 0:
            appendJump(block, firstSuccessor);
            break;
        case 1:
            if (use == RandomGraphUse::Compilation && firstSuccessor == secondSuccessor)
                appendJump(block, firstSuccessor);
            else
                appendBranchTest(block, randomTmp(), firstSuccessor, secondSuccessor);
            break;
        case 2:
            appendMove(block, randomTmp(), returnTmp());
            appendReturn(block);
            break;
        }
    }
    code.resetReachability();
}

static void checkTailLivenessMatchesReference(Code& code)
{
    BitMatrixLiveness<TmpRows> fast(code, code);
    fast.compute();
    CHECK(fast.status() == BitMatrixLivenessStatus::Solved);
    Liveness<TmpLivenessAdapter<GP>> reference(code);
    auto fastWorkset = fast.makeWorkset();
    auto referenceWorkset = reference.makeWorkset();
    for (BasicBlock* block : code) {
        fast.copyLiveAtTailInto(fastWorkset, block);
        reference.copyLiveAtTailInto(referenceWorkset, block);
        for (unsigned index = 0; index < fast.numIndices(); ++index)
            CHECK(fastWorkset.contains(index) == referenceWorkset.contains(index));
    }
}

static void buildRandomTmpGraph(Code& code, unsigned seed)
{
    WeakRandom random(seed);
    Vector<Tmp> tmps;
    unsigned tmpCount = 3 + random.getUint32(38);
    for (unsigned i = 0; i < tmpCount; ++i)
        tmps.append(code.newTmp(GP));
    Arg slot = Arg::stack(code.addStackSlot(8, StackSlotKind::Spill));
    buildRandomGraph(code, random, tmps, RandomGraphUse::Analysis, [&](BasicBlock* block) {
        if (!random.getUint32(4))
            block->append(Move, nullptr, slot, slot, tmps[random.getUint32(tmpCount)]);
    });
}

static void checkRandomGraphLiveness(unsigned seed)
{
    B3::Procedure proc;
    buildRandomTmpGraph(proc.code(), seed);
    checkTailLivenessMatchesReference(proc.code());
}

static bool isLiveAtTail(Code& code, BasicBlock* block, Tmp tmp)
{
    BitMatrixLiveness<TmpRows> liveness(code, code);
    liveness.compute();
    auto workset = liveness.makeWorkset();
    liveness.copyLiveAtTailInto(workset, block);
    return workset.contains(AbsoluteTmpMapper<GP>::absoluteIndex(tmp));
}

static void checkTerminalLateUses()
{
    B3::Procedure proc;
    Code& code = proc.code();
    BasicBlock* entry = code.addBlock();
    BasicBlock* root = code.addBlock();
    Tmp definedInEntry = code.newTmp(GP);
    Tmp definedInRoot = code.newTmp(GP);
    B3::PatchpointValue* patchpoint = addPatchpoint(proc);
    patchpoint->effects.terminal = true;
    for (unsigned i = 0; i < 2; ++i)
        patchpoint->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::LateColdAny);

    entry->append(Move, nullptr, Arg::imm(1), definedInEntry);
    appendJump(entry, root);
    root->append(Move, nullptr, Arg::imm(1), definedInRoot);
    root->append(Patch, patchpoint, Arg::special(patchpointSpecialOf(code)), definedInEntry, definedInRoot);
    code.resetReachability();

    checkTailLivenessMatchesReference(code);
    CHECK(isLiveAtTail(code, root, definedInEntry));
    CHECK(isLiveAtTail(code, root, definedInRoot));
    CHECK(isLiveAtTail(code, entry, definedInEntry));
    CHECK(!isLiveAtTail(code, entry, definedInRoot));
}

void testBitMatrixLivenessMatchesLiveness()
{
    for (unsigned seed = 1; seed <= 50; ++seed)
        checkRandomGraphLiveness(seed);
    checkTerminalLateUses();

    B3::Procedure proc;
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* left = code.addBlock();
    BasicBlock* right = code.addBlock();
    BasicBlock* join = code.addBlock();
    Tmp dead = code.newTmp(GP);
    appendBranchTest(root, argumentTmp(0), left, right);
    for (BasicBlock* block : { left, right }) {
        block->append(Move, nullptr, Arg::imm(1), dead);
        appendJump(block, join);
    }
    appendReturn(join, 0);
    code.resetReachability();

    for (BasicBlock* block : code)
        CHECK(!isLiveAtTail(code, block, dead));
}

void testSlicedBitMatrixLivenessMatchesDense()
{
    constexpr unsigned blockCount = 2049;
    constexpr size_t wordsPerSet = SyntheticRows::numberOfIndices / 64;
    static_assert(blockCount * wordsPerSet > B3::AirLimits::maxWordsPerLivenessMatrix);

    B3::Procedure proc;
    Code& code = proc.code();
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < blockCount; ++i)
        blocks.append(code.addBlock());
    for (unsigned i = 0; i + 1 < blockCount; ++i)
        appendBranchTest(blocks[i], argumentTmp(0), blocks[i + 1], blocks[(i * 7 + 3) % blockCount]);
    appendReturn(blocks.last());
    code.resetReachability();

    BitMatrixLiveness<SyntheticRows> sliced(code);
    sliced.compute();
    CHECK(sliced.status() == BitMatrixLivenessStatus::Solved);
    Vector<uint64_t> slicedTails;
    slicedTails.reserveInitialCapacity(blockCount * wordsPerSet);
    auto slicedWorkset = sliced.makeWorkset();
    for (BasicBlock* block : blocks) {
        sliced.copyLiveAtTailInto(slicedWorkset, block);
        slicedTails.append(slicedWorkset.words());
    }

    for (size_t column = 0; column < wordsPerSet; ++column) {
        BitMatrixLiveness<SyntheticColumnRows> dense(code, column);
        dense.compute();
        CHECK(dense.status() == BitMatrixLivenessStatus::Solved);
        auto denseWorkset = dense.makeWorkset();
        for (unsigned i = 0; i < blockCount; ++i) {
            dense.copyLiveAtTailInto(denseWorkset, blocks[i]);
            CHECK(denseWorkset.words()[0] == slicedTails[i * wordsPerSet + column]);
        }
    }
}

struct BoundaryIndices {
    explicit BoundaryIndices(Code& code)
        : onlyAtTail(code.size())
        , onlyAtHeadOfNextBlock(code.size())
    {
    }

    template<typename Solver>
    void append(Code& code, const Solver& solver, unsigned firstIndex)
    {
        auto appendTo = [&](Vector<unsigned>& indices) {
            return [&indices, firstIndex](unsigned index) {
                CHECK(indices.isEmpty() || indices.last() < firstIndex + index);
                indices.append(firstIndex + index);
            };
        };
        solver.forEachLiveAtHeadOfFirstBlock(appendTo(atHeadOfFirstBlock));
        for (BasicBlock* block : code) {
            solver.forEachLiveOnlyAtTail(block, appendTo(onlyAtTail[block->index()]));
            solver.forEachLiveOnlyAtHeadOfNextBlock(block, appendTo(onlyAtHeadOfNextBlock[block->index()]));
        }
    }

    friend bool operator==(const BoundaryIndices&, const BoundaryIndices&) = default;

    Vector<Vector<unsigned>> onlyAtTail;
    Vector<Vector<unsigned>> onlyAtHeadOfNextBlock;
    Vector<unsigned> atHeadOfFirstBlock;
};

static BoundaryIndices computeReferenceBoundaryIndices(Code& code)
{
    UnifiedTmpLiveness liveness(code);
    BoundaryIndices result(code);
    unsigned indexEnd = Tmp::absoluteIndexEnd(code, GP);
    for (unsigned index = 1; index < indexEnd; ++index) {
        Tmp tmp = AbsoluteTmpMapper<GP>::tmpFromAbsoluteIndex(index);
        if (liveness.liveAtHead(*code.begin()).contains(tmp))
            result.atHeadOfFirstBlock.append(index);
        for (BasicBlock* block : code) {
            BasicBlock* next = code.findNextBlock(block);
            bool isAtTail = liveness.liveAtTail(block).contains(tmp);
            bool isAtHeadOfNextBlock = next && liveness.liveAtHead(next).contains(tmp);
            if (isAtTail && !isAtHeadOfNextBlock)
                result.onlyAtTail[block->index()].append(index);
            if (isAtHeadOfNextBlock && !isAtTail)
                result.onlyAtHeadOfNextBlock[block->index()].append(index);
        }
    }
    return result;
}

void testLayoutBoundariesMatchLiveness()
{
    for (unsigned seed = 1; seed <= 50; ++seed) {
        B3::Procedure proc;
        Code& code = proc.code();
        buildRandomTmpGraph(code, seed);

        BitMatrixLiveness<WithLayoutBoundaries<TmpRows>> liveness(code, code);
        liveness.compute();
        CHECK(liveness.status() == BitMatrixLivenessStatus::Solved);
        BoundaryIndices indices(code);
        indices.append(code, liveness, 0);
        CHECK(indices == computeReferenceBoundaryIndices(code));
    }
}

static void buildGraphWithGaps(Code& code, unsigned blockCount)
{
    Vector<BasicBlock*> reachableBlocks;
    for (unsigned i = 0; i < blockCount; ++i) {
        BasicBlock* block = code.addBlock();
        if (i && !(i % 8))
            appendReturn(block);
        else
            reachableBlocks.append(block);
    }
    for (unsigned i = 0; i + 1 < reachableBlocks.size(); ++i)
        appendBranchTest(reachableBlocks[i], argumentTmp(0), reachableBlocks[i + 1], reachableBlocks[(i * 7 + 3) % reachableBlocks.size()]);
    appendReturn(reachableBlocks.last());
    code.resetReachability();
}

void testSlicedLayoutBoundariesMatchDense()
{
    constexpr unsigned blockCount = 2049;
    constexpr size_t wordsPerSet = SyntheticRows::numberOfIndices / 64;
    static_assert(blockCount * wordsPerSet > B3::AirLimits::maxWordsPerLivenessMatrix);

    B3::Procedure proc;
    Code& code = proc.code();
    buildGraphWithGaps(code, blockCount);
    CHECK(code.size() == blockCount);
    CHECK(!code[blockCount - 1]);

    BitMatrixLiveness<WithLayoutBoundaries<SyntheticRows>> sliced(code);
    sliced.compute();
    CHECK(sliced.status() == BitMatrixLivenessStatus::Solved);
    BoundaryIndices slicedIndices(code);
    slicedIndices.append(code, sliced, 0);

    BoundaryIndices denseIndices(code);
    for (size_t column = 0; column < wordsPerSet; ++column) {
        BitMatrixLiveness<WithLayoutBoundaries<SyntheticColumnRows>> dense(code, column);
        dense.compute();
        CHECK(dense.status() == BitMatrixLivenessStatus::Solved);
        denseIndices.append(code, dense, static_cast<unsigned>(column * 64));
    }
    CHECK(slicedIndices == denseIndices);
}

void testSlicedSolveCallsAdapterInOrder()
{
    constexpr unsigned denseBlockCount = 3;
    constexpr unsigned slicedBlockCount = 2049;
    constexpr size_t wordsPerSet = SyntheticRows::numberOfIndices / 64;
    static_assert(denseBlockCount * wordsPerSet <= B3::AirLimits::maxWordsPerLivenessMatrix);
    static_assert(slicedBlockCount * wordsPerSet > B3::AirLimits::maxWordsPerLivenessMatrix);

    for (unsigned blockCount : { denseBlockCount, slicedBlockCount }) {
        B3::Procedure proc;
        Code& code = proc.code();
        buildGraphWithGaps(code, blockCount);

        BitMatrixLiveness<RecordingRows> liveness(code);
        liveness.compute();
        CHECK(liveness.status() == BitMatrixLivenessStatus::Solved);
        CHECK(liveness.wordsPerSlice.has_value() == (blockCount == slicedBlockCount));

        size_t wordsPerCall = liveness.wordsPerSlice.value_or(wordsPerSet);
        CHECK((wordsPerCall < wordsPerSet) == (blockCount == slicedBlockCount));
        size_t callIndex = 0;
        for (size_t firstWord = 0; firstWord < wordsPerSet; firstWord += wordsPerCall) {
            for (BasicBlock* block : code) {
                CHECK(callIndex < liveness.calls.size());
                const RecordingRows::Call& call = liveness.calls[callIndex++];
                CHECK(call.firstWord == firstWord);
                CHECK(call.numberOfWords == std::min(wordsPerCall, wordsPerSet - firstWord));
                CHECK(call.blockIndex == block->index());
            }
        }
        CHECK(callIndex == liveness.calls.size());
    }
}

void testAllocatorLivenessInSlices()
{
    constexpr unsigned blockCount = 2100;
    constexpr unsigned tmpsPerBlock = 16;
    constexpr unsigned blocksFromDefToUse = 3;
    constexpr unsigned definingBlockCount = blockCount - 1 - blocksFromDefToUse;
    constexpr unsigned tmpCount = definingBlockCount * tmpsPerBlock;
    static_assert(tmpCount >= 33000);
    static_assert(blockCount * ((tmpCount + 63) / 64) > B3::AirLimits::maxWordsPerLivenessMatrix);

    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < blockCount; ++i)
        blocks.append(code.addBlock());
    Vector<Tmp> tmps;
    for (unsigned i = 0; i < tmpCount; ++i)
        tmps.append(code.newTmp(GP));
    Tmp sum = code.newTmp(GP);

    blocks[0]->append(Move, nullptr, Arg::imm(0), sum);
    for (unsigned blockIndex = 0; blockIndex + 1 < blockCount; ++blockIndex) {
        BasicBlock* block = blocks[blockIndex];
        for (unsigned i = 0; i < tmpsPerBlock; ++i) {
            if (blockIndex >= blocksFromDefToUse)
                block->append(Add64, nullptr, tmps[(blockIndex - blocksFromDefToUse) * tmpsPerBlock + i], sum);
            if (blockIndex < definingBlockCount)
                block->append(Move, nullptr, Arg::bigImm(blockIndex * tmpsPerBlock + i), tmps[blockIndex * tmpsPerBlock + i]);
        }
        if (blockIndex + 2 < blockCount)
            appendBranchTest(block, argumentTmp(0), blocks[blockIndex + 1], blocks.last());
        else
            appendJump(block, blocks.last());
    }
    blocks.last()->append(Move, nullptr, sum, returnTmp());
    appendReturn(blocks.last());

    auto compilation = compile(proc);
    CHECK(invoke<int64_t>(*compilation, 1ll) == tmpCount * (tmpCount - 1ll) / 2);
    CHECK(!invoke<int64_t>(*compilation, 0ll));
}

enum class PatchOperand : uint8_t { None, LateUse, EarlyDef, EarlyClobber, LateClobber };

struct UsedRegistersRecorder {
    explicit UsedRegistersRecorder(B3::Procedure& proc)
        : proc(proc)
        , special(patchpointSpecialOf(proc.code()))
    {
        proc.setOptLevel(2);
        // With a spill slot to place, allocateStackByGraphColoring() removes the register allocator's Padding, as it does in code that spills.
        proc.code().addStackSlot(8, StackSlotKind::Spill);
    }

    B3::PatchpointValue* appendPatch(BasicBlock* block, PatchOperand operand = PatchOperand::None, Tmp tmp = Tmp())
    {
        B3::PatchpointValue* patchpoint = addPatchpoint(proc, operand == PatchOperand::EarlyDef ? B3::Int64 : B3::Void);
        unsigned index = patchpoints.size();
        patchpoints.append(patchpoint);
        reported.append(std::nullopt);
        patchpoint->setGenerator([this, index](CCallHelpers&, const B3::StackmapGenerationParams& params) {
            reported[index] = params.usedRegisters();
        });
        switch (operand) {
        case PatchOperand::None:
            break;
        case PatchOperand::LateUse:
            patchpoint->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::SomeLateRegister);
            break;
        case PatchOperand::EarlyDef:
            patchpoint->resultConstraints = { B3::ValueRep::SomeEarlyRegister };
            break;
        case PatchOperand::EarlyClobber:
            patchpoint->clobberEarly(RegisterSet { tmp.reg() });
            break;
        case PatchOperand::LateClobber:
            patchpoint->clobberLate(RegisterSet { tmp.reg() });
            break;
        }
        if (operand == PatchOperand::LateUse || operand == PatchOperand::EarlyDef)
            block->append(Patch, patchpoint, Arg::special(special), tmp);
        else
            block->append(Patch, patchpoint, Arg::special(special));
        return patchpoint;
    }

    bool isReported(unsigned patchIndex, unsigned argumentIndex) const
    {
        CHECK(reported[patchIndex]);
        return reported[patchIndex]->contains(GPRInfo::toArgumentRegister(argumentIndex), IgnoreVectors);
    }

    B3::Procedure& proc;
    B3::Air::Special* special;
    Vector<B3::Value*> patchpoints;
    Vector<std::optional<RegisterSet>> reported;
};

struct BlockRegisterLiveness {
    friend bool operator==(const BlockRegisterLiveness&, const BlockRegisterLiveness&) = default;

    RegisterSet atHead;
    RegisterSet atTail;
    Vector<RegisterSet> afterInsts;
};

template<typename LiveTmps>
static RegisterSet registersOf(const LiveTmps& liveTmps)
{
    RegisterSet result;
    for (Tmp tmp : liveTmps)
        result.add(tmp.reg(), IgnoreVectors);
    return result;
}

static Vector<BlockRegisterLiveness> computeReferenceRegisterLiveness(Code& code)
{
    padInterference(code);
    UnifiedTmpLiveness liveness(code);
    Vector<BlockRegisterLiveness> result(code.size());
    for (BasicBlock* block : code) {
        BlockRegisterLiveness& blockLiveness = result[block->index()];
        blockLiveness.atHead = registersOf(liveness.liveAtHead(block));
        blockLiveness.atTail = registersOf(liveness.liveAtTail(block));
        UnifiedTmpLiveness::LocalCalc localCalc(liveness, block);
        for (unsigned instIndex = block->size(); instIndex--;) {
            if (block->at(instIndex).kind.opcode != Padding)
                blockLiveness.afterInsts.append(registersOf(localCalc.live()));
            localCalc.execute(instIndex);
        }
        blockLiveness.afterInsts.reverse();
    }
    return result;
}

struct RegisterLivenessSummary {
    Vector<std::optional<RegisterSet>> liveAtPatches;
    Vector<Vector<Inst>> instsToKeep;
};

static bool isDeadAssignment(Inst& inst, const RegisterSet& liveAfterInst)
{
    if (inst.hasNonArgEffects())
        return false;
    bool canDelete = true;
    inst.forEachArg([&](Arg& arg, Arg::Role role, B3::Bank, Width) {
        if (Arg::isEarlyDef(role)) {
            canDelete = false;
            return;
        }
        if (!Arg::isLateDef(role))
            return;
        if (!arg.isReg() || liveAfterInst.contains(arg.reg(), IgnoreVectors))
            canDelete = false;
    });
    return canDelete;
}

static RegisterLivenessSummary summarizeRegisterLiveness(const UsedRegistersRecorder& recorder, const Vector<BlockRegisterLiveness>& reference)
{
    Code& code = recorder.proc.code();
    RegisterLivenessSummary result;
    result.liveAtPatches.grow(recorder.patchpoints.size());
    result.instsToKeep.grow(code.size());
    for (BasicBlock* block : code) {
        const Vector<RegisterSet>& liveAfterInsts = reference[block->index()].afterInsts;
        CHECK(liveAfterInsts.size() == block->size());
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);
            if (isDeadAssignment(inst, liveAfterInsts[instIndex]))
                continue;
            result.instsToKeep[block->index()].append(inst);
            if (inst.kind.opcode == Patch)
                result.liveAtPatches[recorder.patchpoints.find(inst.origin)] = liveAfterInsts[instIndex];
        }
    }
    return result;
}

static Vector<Tmp> randomGraphRegisters()
{
    return { argumentTmp(0), argumentTmp(1), argumentTmp(2), argumentTmp(3), argumentTmp(4), argumentTmp(5), returnTmp() };
}

static void buildRandomRegisterGraph(unsigned seed, UsedRegistersRecorder& recorder)
{
    constexpr std::array operands { PatchOperand::None, PatchOperand::LateUse, PatchOperand::EarlyDef };
    WeakRandom random(seed);
    Vector<Tmp> registers = randomGraphRegisters();
    buildRandomGraph(recorder.proc.code(), random, registers, RandomGraphUse::Compilation, [&](BasicBlock* block) {
        if (block->size() && random.getUint32(3))
            return;
        PatchOperand operand = operands[random.getUint32(operands.size())];
        // simplifyCFG() may append this block to its predecessor, whose last Patch would then report the early def.
        if (!block->size() && operand == PatchOperand::EarlyDef)
            operand = PatchOperand::None;
        recorder.appendPatch(block, operand, registers[random.getUint32(registers.size())]);
    });
}

static bool isSameInst(const Inst& a, const Inst& b)
{
    return a.kind == b.kind && a.origin == b.origin && std::ranges::equal(a.args(), b.args());
}

static void checkRandomGraphUsedRegisters(unsigned seed, bool usesSIMD)
{
    B3::Procedure referenceProc(usesSIMD);
    UsedRegistersRecorder referenceRecorder(referenceProc);
    buildRandomRegisterGraph(seed, referenceRecorder);
    Vector<BlockRegisterLiveness> reference = computeReferenceRegisterLiveness(referenceProc.code());

    B3::Procedure proc(usesSIMD);
    UsedRegistersRecorder recorder(proc);
    buildRandomRegisterGraph(seed, recorder);
    Code& code = proc.code();
    RegisterLivenessSummary expected = summarizeRegisterLiveness(recorder, reference);

    reportUsedRegisters(code);
    for (BasicBlock* block : code) {
        const Vector<Inst>& instsToKeep = expected.instsToKeep[block->index()];
        CHECK(block->size() == instsToKeep.size());
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex)
            CHECK(isSameInst(block->at(instIndex), instsToKeep[instIndex]));
    }

    B3::Procedure compiledProc(usesSIMD);
    UsedRegistersRecorder compiledRecorder(compiledProc);
    buildRandomRegisterGraph(seed, compiledRecorder);
    prepareForGeneration(compiledProc.code());
    generateCode(compiledProc);
    for (unsigned i = 0; i < compiledRecorder.reported.size(); ++i) {
        const std::optional<RegisterSet>& reported = compiledRecorder.reported[i];
        CHECK(reported.has_value() == expected.liveAtPatches[i].has_value());
        if (reported)
            CHECK(reported->toScalarRegisterSet() == expected.liveAtPatches[i]->toScalarRegisterSet());
    }
}

static void checkReportedPatchOperands(bool usesSIMD)
{
    B3::Procedure proc(usesSIMD);
    UsedRegistersRecorder recorder(proc);
    BasicBlock* root = proc.code().addBlock();
    recorder.appendPatch(root);
    recorder.appendPatch(root, PatchOperand::LateUse, argumentTmp(1));
    recorder.appendPatch(root, PatchOperand::EarlyDef, argumentTmp(1));
    recorder.appendPatch(root, PatchOperand::LateUse, argumentTmp(2))->effects.terminal = true;

    prepareForGeneration(proc.code());
    generateCode(proc);
    CHECK(recorder.isReported(0, 1));
    CHECK(recorder.isReported(0, 2));
    CHECK(!recorder.isReported(0, 3));
    CHECK(recorder.isReported(1, 1));
    CHECK(!recorder.isReported(2, 1));
    CHECK(recorder.isReported(3, 2));
}

static void checkLastUseBeforePatch(bool usesSIMD)
{
    B3::Procedure proc(usesSIMD);
    UsedRegistersRecorder recorder(proc);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    root->append(Move, nullptr, argumentTmp(1), argumentTmp(2));
    root->append(Add64, nullptr, argumentTmp(2), argumentTmp(3));
    recorder.appendPatch(root);
    root->append(Move, nullptr, argumentTmp(4), argumentTmp(5));
    root->append(Move, nullptr, argumentTmp(3), returnTmp());
    appendReturn(root);
    unsigned sizeWithDeadMove = root->size();

    prepareForGeneration(code);
    generateCode(proc);
    CHECK(root->size() == sizeWithDeadMove - 1);
    CHECK(recorder.isReported(0, 3));
    CHECK(recorder.isReported(0, 4));
    CHECK(!recorder.isReported(0, 2));
    CHECK(!recorder.isReported(0, 5));
}

void testUsedRegistersMatchReference()
{
    for (bool usesSIMD : { false, true }) {
        if (usesSIMD && !Options::useWasmSIMD())
            continue;
        for (unsigned seed = 1; seed <= 40; ++seed)
            checkRandomGraphUsedRegisters(seed, usesSIMD);
        checkReportedPatchOperands(usesSIMD);
        checkLastUseBeforePatch(usesSIMD);
    }
}

enum class FloatingPointUses : uint8_t { All, OnlyVectors };

static void buildRandomVectorGraph(unsigned seed, UsedRegistersRecorder& recorder, bool hasVectorInsts, FloatingPointUses uses)
{
    constexpr std::array operands { PatchOperand::None, PatchOperand::LateUse, PatchOperand::EarlyDef, PatchOperand::EarlyClobber, PatchOperand::LateClobber };
    constexpr unsigned floatingPointRegisterCount = 4;
    WeakRandom random(seed);
    Vector<Tmp> registers = randomGraphRegisters();
    auto appendFloatingPointMove = [&](BasicBlock* block) {
        Tmp source(FPRInfo::toRegister(random.getUint32(floatingPointRegisterCount)));
        Tmp destination(FPRInfo::toRegister(random.getUint32(floatingPointRegisterCount)));
        bool isVector = random.getUint32(2) && hasVectorInsts;
        if (isVector)
            block->append(MoveVector, nullptr, source, destination);
        else if (uses == FloatingPointUses::All)
            block->append(MoveDouble, nullptr, source, destination);
        else
            block->append(Move64ToDouble, nullptr, argumentTmp(0), destination);
    };
    buildRandomGraph(recorder.proc.code(), random, registers, RandomGraphUse::Analysis, [&](BasicBlock* block) {
        switch (random.getUint32(3)) {
        case 0: {
            PatchOperand operand = operands[random.getUint32(operands.size())];
            recorder.appendPatch(block, operand, registers[random.getUint32(registers.size())]);
            break;
        }
        case 1:
            appendFloatingPointMove(block);
            break;
        case 2:
            break;
        }
    });
}

static void addUpperHalves(Vector<BlockRegisterLiveness>& liveness, const Vector<BlockRegisterLiveness>& upperHalfLiveness)
{
    auto widen = [](RegisterSet& registers, const RegisterSet& registersWithLiveUpperHalf) {
        registersWithLiveUpperHalf.forEach([&](Reg reg) {
            if (reg.isFPR())
                registers.add(reg, conservativeWidth(reg));
        });
    };
    for (size_t blockIndex = 0; blockIndex < liveness.size(); ++blockIndex) {
        BlockRegisterLiveness& block = liveness[blockIndex];
        const BlockRegisterLiveness& upperHalves = upperHalfLiveness[blockIndex];
        widen(block.atHead, upperHalves.atHead);
        widen(block.atTail, upperHalves.atTail);
        for (size_t instIndex = 0; instIndex < block.afterInsts.size(); ++instIndex)
            widen(block.afterInsts[instIndex], upperHalves.afterInsts[instIndex]);
    }
}

static Vector<BlockRegisterLiveness> collectRegLiveness(Code& code)
{
    padInterference(code);
    RegLiveness liveness(code);
    Vector<BlockRegisterLiveness> result(code.size());
    for (BasicBlock* block : code) {
        BlockRegisterLiveness& blockLiveness = result[block->index()];
        blockLiveness.atHead = liveness.liveAtHead(block);
        blockLiveness.atTail = liveness.liveAtTail(block);
        RegLiveness::LocalCalc localCalc(liveness, block);
        for (unsigned instIndex = block->size(); instIndex--;) {
            if (block->at(instIndex).kind.opcode != Padding)
                blockLiveness.afterInsts.append(localCalc.live());
            for (Reg reg : Reg::all())
                CHECK(localCalc.isLive(reg) == localCalc.live().contains(reg, IgnoreVectors));
            localCalc.execute(instIndex);
        }
        blockLiveness.afterInsts.reverse();
    }
    return result;
}

static void checkRandomGraphRegLiveness(unsigned seed, bool usesSIMD, bool hasVectorInsts)
{
    auto computeReference = [&](FloatingPointUses uses) {
        B3::Procedure proc(usesSIMD);
        UsedRegistersRecorder recorder(proc);
        buildRandomVectorGraph(seed, recorder, hasVectorInsts, uses);
        return computeReferenceRegisterLiveness(proc.code());
    };
    Vector<BlockRegisterLiveness> expected = computeReference(FloatingPointUses::All);
    addUpperHalves(expected, computeReference(FloatingPointUses::OnlyVectors));

    B3::Procedure proc(usesSIMD);
    UsedRegistersRecorder recorder(proc);
    buildRandomVectorGraph(seed, recorder, hasVectorInsts, FloatingPointUses::All);
    CHECK(collectRegLiveness(proc.code()) == expected);
}

static void checkUpperHalfLiveness(bool usesSIMD)
{
    B3::Procedure proc(usesSIMD);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* next = code.addBlock();
    Reg vector = FPRInfo::fpRegT0;
    Reg scalar = FPRInfo::fpRegT1;
    Arg memory = Arg::addr(argumentTmp(0));

    root->append(MoveVector, nullptr, memory, Tmp(vector));
    root->append(MoveDouble, nullptr, memory, Tmp(scalar));
    appendJump(root, next);
    next->append(MoveDouble, nullptr, Tmp(vector), memory);
    next->append(MoveVector, nullptr, Tmp(vector), memory);
    next->append(MoveDouble, nullptr, Tmp(vector), memory);
    next->append(MoveDouble, nullptr, Tmp(scalar), memory);
    appendReturn(next, 0);
    code.resetReachability();

    Vector<BlockRegisterLiveness> liveness = collectRegLiveness(code);
    const BlockRegisterLiveness& inRoot = liveness[root->index()];
    const BlockRegisterLiveness& inNext = liveness[next->index()];
    CHECK(!inRoot.atHead.contains(vector, IgnoreVectors));
    CHECK(inRoot.afterInsts[0].contains(vector, Width128));
    CHECK(inRoot.atTail.contains(vector, Width128));
    CHECK(inNext.atHead.contains(vector, Width128));
    CHECK(inNext.afterInsts[0].contains(vector, Width128));
    CHECK(inNext.afterInsts[1].contains(vector, IgnoreVectors));
    CHECK(!inNext.afterInsts[1].contains(vector, Width128));
    CHECK(!inNext.afterInsts[2].contains(vector, IgnoreVectors));
    CHECK(inNext.atHead.contains(scalar, IgnoreVectors));
    CHECK(!inNext.atHead.contains(scalar, Width128));
}

void testRegLivenessMatchesTmpLiveness()
{
    for (bool usesSIMD : { false, true }) {
        if (usesSIMD && !Options::useWasmSIMD())
            continue;
        for (bool hasVectorInsts : { false, true }) {
            for (unsigned seed = 1; seed <= 25; ++seed)
                checkRandomGraphRegLiveness(seed, usesSIMD, hasVectorInsts);
        }
        checkUpperHalfLiveness(usesSIMD);
    }
}
