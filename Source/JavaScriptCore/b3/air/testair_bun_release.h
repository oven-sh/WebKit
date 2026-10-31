/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Code as it is behind the allocation of registers and stack slots: one block, real registers, spill slots with a coloring written by hand.
struct ReleaseGraph {
    ReleaseGraph()
        : code(proc.code())
    {
    }

    StackSlot* addSlot(unsigned word, unsigned byteSize = 8, StackSlotKind kind = StackSlotKind::Spill, unsigned firstUnitInWord = 0)
    {
        StackSlot* slot = code.addStackSlot(byteSize, kind);
        coloring.unitsOfSlot.grow(slot->index() + 1);
        unsigned first = word * StackColoring::unitsPerWord + firstUnitInWord;
        unsigned last = first + byteSize / StackColoring::bytesPerUnit - 1;
        coloring.unitsOfSlot[slot->index()] = StackColoring::UnitRange { first, last };
        coloring.numberOfUnits = std::max(coloring.numberOfUnits, last + 1);
        return slot;
    }

    // `place`: where the value that the program lets go of is at the hint.
    void appendHint(Arg place)
    {
        hint = addPatchpoint(proc);
        hint->setReleasesDeadValues(true);
        hint->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::ColdAny);
        block->append(Patch, hint, special, place);
    }

    // A call with these arguments. Its callee lets go of the first.
    template<typename... Arguments>
    void appendCallWhoseCalleeLetsGoOfFirst(Arguments... arguments)
    {
        call = callRunsWithoutFrame ? addPatchpointWithoutFrame(proc) : addPatchpoint(proc);
        for (unsigned index = 0; index < sizeof...(arguments); ++index)
            call->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::ColdAny);
        BitVector children;
        children.set(0);
        call->setChildrenLetGoByCallee(WTF::move(children));
        block->append(Patch, call, special, arguments...);
    }

    void appendPatchpoint(bool runsWithoutFrame = false)
    {
        block->append(Patch, runsWithoutFrame ? addPatchpointWithoutFrame(proc) : addPatchpoint(proc), special);
    }

    void appendCCall()
    {
        B3::BasicBlock* root = proc.size() ? proc.at(0) : proc.addBlock();
        B3::Value* callee = root->appendNew<B3::Const64Value>(proc, B3::Origin(), 0x1000);
        B3::CCallValue* call = root->appendNew<B3::CCallValue>(proc, B3::Void, B3::Origin(), callee);
        block->append(buildCCall(code, call, { argumentTmp(1) }));
    }

    void run()
    {
        appendReturn(block, 0);
        code.resetReachability();
        releaseDeadValues(code, coloring);
    }

    static bool storesZero(const Inst& inst) { return inst.kind.opcode == Move && inst.args().size() == 2 && !inst.args()[1].isTmp() && (inst.args()[0] == Arg::imm(0) || inst.args()[0].isZeroReg()); }

    unsigned numberOfClears() const
    {
        unsigned result = 0;
        for (const Inst& inst : *block)
            result += storesZero(inst);
        return result;
    }

    bool clears(const Arg& place) const
    {
        for (const Inst& inst : *block) {
            if (storesZero(inst) && inst.args()[1] == place)
                return true;
        }
        return false;
    }

    bool clearsInFrontOfCall(const Arg& place) const
    {
        for (const Inst& inst : *block) {
            if (inst.kind.opcode == Patch && inst.origin == call)
                return false;
            if (storesZero(inst) && inst.args()[1] == place)
                return true;
        }
        return false;
    }

    bool hintIsThere() const
    {
        for (const Inst& inst : *block) {
            if (inst.kind.opcode == Patch && inst.origin == hint)
                return true;
        }
        return false;
    }

    B3::Procedure proc;
    Code& code;
    BasicBlock* block { code.addBlock() };
    Arg special { Arg::special(patchpointSpecialOf(code)) };
    StackColoring coloring;
    B3::PatchpointValue* hint { nullptr };
    B3::PatchpointValue* call { nullptr };
    bool callRunsWithoutFrame { false };
};

static Tmp someSavedRegister()
{
    Reg result;
    RegisterSet::calleeSaveRegisters().forEach([&](Reg reg) {
        if (reg.isGPR() && reg.gpr() != MacroAssembler::framePointerRegister && !result)
            result = reg;
    });
    return Tmp(result);
}

void testReleaseClearsDeadSpillWord()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.appendHint(Arg::stack(slot));
    graph.appendPatchpoint();
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clears(Arg::stack(slot)));
    CHECK(!graph.hintIsThere());
}

void testReleaseCountsCCallAsCollection()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.appendHint(Arg::stack(slot));
    graph.appendCCall();
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clears(Arg::stack(slot)));
}

void testReleaseNeedsCollectionBehind()
{
    for (bool hasPatchpointWithoutFrame : { false, true }) {
        ReleaseGraph graph;
        StackSlot* slot = graph.addSlot(0);
        graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
        graph.appendHint(Arg::stack(slot));
        if (hasPatchpointWithoutFrame)
            graph.appendPatchpoint(true);
        graph.run();
        CHECK(!graph.numberOfClears());
        CHECK(!graph.hintIsThere());
    }
}

void testReleaseKeepsWordReadAgain()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.appendHint(Arg::stack(slot));
    graph.appendPatchpoint();
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(1));
    graph.run();
    CHECK(!graph.numberOfClears());
}

void testReleaseKeepsWordWrittenBeforeCollection()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.appendHint(Arg::stack(slot));
    graph.block->append(Move, nullptr, argumentTmp(1), Arg::stack(slot));
    graph.appendPatchpoint();
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(1));
    graph.run();
    CHECK(!graph.numberOfClears());
}

void testReleaseLeavesOtherDeadValues()
{
    ReleaseGraph graph;
    StackSlot* released = graph.addSlot(0);
    StackSlot* other = graph.addSlot(1);
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(released));
    graph.block->append(Move, nullptr, argumentTmp(1), Arg::stack(other));
    graph.block->append(Move, nullptr, argumentTmp(2), saved);
    graph.appendHint(Arg::stack(released));
    graph.appendPatchpoint();
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clears(Arg::stack(released)));
    CHECK(!graph.hintIsThere());
}

void testReleaseFollowsCopies()
{
    ReleaseGraph graph;
    StackSlot* spilled = graph.addSlot(0);
    StackSlot* sibling = graph.addSlot(1);
    Tmp saved = someSavedRegister();
    // argument 0 > the saved register > a slot > argument 2, where the hint finds it. The slot has a copy of its own.
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.block->append(Move, nullptr, saved, Arg::stack(spilled));
    graph.block->append(Move, nullptr, Arg::stack(spilled), argumentTmp(3));
    graph.block->append(Move, nullptr, argumentTmp(3), Arg::stack(sibling));
    graph.block->append(Move, nullptr, Arg::stack(spilled), argumentTmp(2));
    graph.code.requestCallArgAreaSizeInBytes(16);
    graph.block->append(Move, nullptr, argumentTmp(2), Arg::callArg(0));
    graph.appendHint(argumentTmp(2));
    graph.appendPatchpoint();
    graph.run();
    CHECK(graph.numberOfClears() == 3);
    CHECK(graph.clears(Arg::stack(spilled)));
    CHECK(graph.clears(Arg::stack(sibling)));
    CHECK(graph.clears(Arg::callArg(0)));
    CHECK(graph.hintIsThere());
    CHECK(graph.hint->registersToRelease() == RegisterSet(saved.reg()));
}

void testReleaseFollowsCopiesOutOfPlaceWrittenSince()
{
    ReleaseGraph graph;
    StackSlot* spilled = graph.addSlot(0);
    Tmp saved = someSavedRegister();
    // argument 0 > a slot, where the hint finds it. Then argument 0 > the saved register, and argument 0 takes something else.
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(spilled));
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.block->append(Move, nullptr, argumentTmp(1), argumentTmp(0));
    graph.appendHint(Arg::stack(spilled));
    graph.appendPatchpoint();
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clears(Arg::stack(spilled)));
    CHECK(graph.hintIsThere());
    CHECK(graph.hint->registersToRelease() == RegisterSet(saved.reg()));
}

void testReleaseLeavesCopyOutOfPlaceWrittenInBetween()
{
    ReleaseGraph graph;
    StackSlot* spilled = graph.addSlot(0);
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(spilled));
    graph.block->append(Move, nullptr, argumentTmp(1), argumentTmp(0));
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.appendHint(Arg::stack(spilled));
    graph.appendPatchpoint();
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clears(Arg::stack(spilled)));
    CHECK(!graph.hintIsThere());
}

void testReleaseLeavesCopyWrittenSince()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(2));
    graph.block->append(Move, nullptr, argumentTmp(1), Arg::stack(slot));
    graph.appendHint(argumentTmp(2));
    graph.appendPatchpoint();
    graph.run();
    CHECK(!graph.numberOfClears());
}

void testReleaseLeavesWordThatAnotherSlotTook()
{
    ReleaseGraph graph;
    StackSlot* first = graph.addSlot(0);
    StackSlot* second = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(first));
    graph.block->append(Move, nullptr, Arg::stack(first), argumentTmp(2));
    graph.block->append(Move, nullptr, argumentTmp(1), Arg::stack(second));
    graph.appendHint(argumentTmp(2));
    graph.appendPatchpoint();
    graph.block->append(Move, nullptr, Arg::stack(second), argumentTmp(1));
    graph.run();
    CHECK(!graph.numberOfClears());
}

void testReleaseLeavesWordThatHalfSlotTook()
{
    ReleaseGraph graph;
    StackSlot* whole = graph.addSlot(0);
    StackSlot* half = graph.addSlot(0, 4, StackSlotKind::Spill, 1);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(whole));
    graph.block->append(Move, nullptr, Arg::stack(whole), argumentTmp(2));
    graph.block->append(Move32, nullptr, argumentTmp(1), Arg::stack(half));
    graph.appendHint(argumentTmp(2));
    graph.appendPatchpoint();
    graph.block->append(Move32, nullptr, Arg::stack(half), argumentTmp(1));
    graph.run();
    CHECK(!graph.numberOfClears());
}

void testReleaseLeavesSlotThatIsNoSpill()
{
    ReleaseGraph graph;
    StackSlot* locked = graph.addSlot(0, 8, StackSlotKind::Locked);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(locked));
    graph.appendHint(argumentTmp(0));
    graph.appendPatchpoint();
    graph.run();
    CHECK(!graph.numberOfClears());
}

void testReleaseOfConstantClearsNothing()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.appendHint(Arg::bigImm(7));
    graph.appendPatchpoint();
    graph.run();
    CHECK(!graph.numberOfClears());
    CHECK(!graph.hintIsThere());
}

void testReleaseClearsSavedRegisterInPatchpoint()
{
    ReleaseGraph graph;
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.appendHint(saved);
    graph.appendPatchpoint();
    graph.run();
    CHECK(!graph.numberOfClears());
    CHECK(graph.hintIsThere());
    CHECK(graph.hint->registersToRelease() == RegisterSet(saved.reg()));
    CHECK(graph.hint->lateClobbered().contains(saved.reg(), IgnoreVectors));
}

void testReleaseKeepsSavedRegisterReadAgain()
{
    ReleaseGraph graph;
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.appendHint(saved);
    graph.appendPatchpoint();
    graph.block->append(Move, nullptr, saved, argumentTmp(1));
    graph.run();
    CHECK(!graph.hintIsThere());
}

void testReleaseFollowsCopiesIntoOtherBlocks()
{
    ReleaseGraph graph;
    StackSlot* always = graph.addSlot(0);
    StackSlot* onOnePath = graph.addSlot(1);
    StackSlot* other = graph.addSlot(2);
    BasicBlock* entry = graph.block;
    BasicBlock* copies = graph.code.addBlock();
    BasicBlock* join = graph.code.addBlock();
    entry->append(Move, nullptr, argumentTmp(0), Arg::stack(always));
    entry->append(Move, nullptr, argumentTmp(1), Arg::stack(other));
    appendBranchTest(entry, argumentTmp(1), copies, join);
    copies->append(Move, nullptr, Arg::stack(always), argumentTmp(2));
    copies->append(Move, nullptr, argumentTmp(2), Arg::stack(onOnePath));
    appendJump(copies, join);
    graph.block = join;
    graph.appendHint(Arg::stack(always));
    graph.appendPatchpoint();
    graph.run();
    CHECK(graph.numberOfClears() == 2);
    CHECK(graph.clears(Arg::stack(always)));
    CHECK(graph.clears(Arg::stack(onOnePath)));
}

void testReleaseClearsCopiesOfCallerInFrontOfCall()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.block->append(Move, nullptr, saved, Arg::stack(slot));
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(2));
    graph.appendCallWhoseCalleeLetsGoOfFirst(argumentTmp(2));
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clearsInFrontOfCall(Arg::stack(slot)));
    CHECK(graph.call->registersToRelease() == RegisterSet(saved.reg()));
    CHECK(graph.call->lateClobbered().contains(saved.reg(), IgnoreVectors));
}

// A tail call. The frame of the callee lies over the words of this one, and the saved registers have been restored when its generator runs.
void testReleaseClearsWordsInFrontOfCallWithoutFrame()
{
    ReleaseGraph graph;
    graph.callRunsWithoutFrame = true;
    StackSlot* slot = graph.addSlot(0);
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.block->append(Move, nullptr, saved, Arg::stack(slot));
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(2));
    graph.appendCallWhoseCalleeLetsGoOfFirst(argumentTmp(2));
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clearsInFrontOfCall(Arg::stack(slot)));
    CHECK(!graph.call->lateClobbered().contains(saved.reg(), IgnoreVectors));
}

void testReleaseKeepsCopiesOfCallerReadBehindCall()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    Tmp saved = someSavedRegister();
    graph.block->append(Move, nullptr, argumentTmp(0), saved);
    graph.block->append(Move, nullptr, saved, Arg::stack(slot));
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(2));
    graph.appendCallWhoseCalleeLetsGoOfFirst(argumentTmp(2));
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(1));
    graph.block->append(Move, nullptr, saved, argumentTmp(3));
    graph.run();
    CHECK(!graph.numberOfClears());
    CHECK(graph.call->registersToRelease().isEmpty());
}

void testReleaseKeepsWhatCallReads()
{
    ReleaseGraph graph;
    StackSlot* slot = graph.addSlot(0);
    graph.code.requestCallArgAreaSizeInBytes(16);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(slot));
    graph.block->append(Move, nullptr, Arg::stack(slot), argumentTmp(2));
    graph.block->append(Move, nullptr, argumentTmp(2), Arg::callArg(0));
    graph.block->append(Move, nullptr, argumentTmp(2), Arg::callArg(8));
    graph.appendCallWhoseCalleeLetsGoOfFirst(Arg::callArg(0));
    graph.run();
    CHECK(graph.numberOfClears() == 2);
    CHECK(graph.clearsInFrontOfCall(Arg::stack(slot)));
    CHECK(graph.clearsInFrontOfCall(Arg::callArg(8)));
    CHECK(!graph.clearsInFrontOfCall(Arg::callArg(0)));
}

void testReleaseLeavesCopiesOfArgumentsThatCalleeKeeps()
{
    ReleaseGraph graph;
    StackSlot* letGo = graph.addSlot(0);
    StackSlot* kept = graph.addSlot(1);
    graph.block->append(Move, nullptr, argumentTmp(0), Arg::stack(letGo));
    graph.block->append(Move, nullptr, argumentTmp(1), Arg::stack(kept));
    graph.block->append(Move, nullptr, Arg::stack(letGo), argumentTmp(2));
    graph.block->append(Move, nullptr, Arg::stack(kept), argumentTmp(3));
    graph.appendCallWhoseCalleeLetsGoOfFirst(argumentTmp(2), argumentTmp(3));
    graph.run();
    CHECK(graph.numberOfClears() == 1);
    CHECK(graph.clearsInFrontOfCall(Arg::stack(letGo)));
}

#define RUN_BUN_RELEASE_TESTS() do { \
        RUN(testReleaseClearsDeadSpillWord()); \
        RUN(testReleaseCountsCCallAsCollection()); \
        RUN(testReleaseNeedsCollectionBehind()); \
        RUN(testReleaseKeepsWordReadAgain()); \
        RUN(testReleaseKeepsWordWrittenBeforeCollection()); \
        RUN(testReleaseLeavesOtherDeadValues()); \
        RUN(testReleaseFollowsCopies()); \
        RUN(testReleaseFollowsCopiesOutOfPlaceWrittenSince()); \
        RUN(testReleaseLeavesCopyOutOfPlaceWrittenInBetween()); \
        RUN(testReleaseLeavesCopyWrittenSince()); \
        RUN(testReleaseLeavesWordThatAnotherSlotTook()); \
        RUN(testReleaseLeavesWordThatHalfSlotTook()); \
        RUN(testReleaseLeavesSlotThatIsNoSpill()); \
        RUN(testReleaseOfConstantClearsNothing()); \
        RUN(testReleaseClearsSavedRegisterInPatchpoint()); \
        RUN(testReleaseKeepsSavedRegisterReadAgain()); \
        RUN(testReleaseFollowsCopiesIntoOtherBlocks()); \
        RUN(testReleaseClearsCopiesOfCallerInFrontOfCall()); \
        RUN(testReleaseClearsWordsInFrontOfCallWithoutFrame()); \
        RUN(testReleaseKeepsCopiesOfCallerReadBehindCall()); \
        RUN(testReleaseKeepsWhatCallReads()); \
        RUN(testReleaseLeavesCopiesOfArgumentsThatCalleeKeeps()); \
    } while (false)
