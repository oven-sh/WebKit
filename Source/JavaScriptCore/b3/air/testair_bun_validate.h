/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_VALIDATE_TESTS() do { \
        RUN(testTmpsAreDefinedAtUses()); \
        RUN(testBlockEntryClobberPosition()); \
    } while (false)

enum class TmpGraph : uint8_t {
    SameBlock,
    Predecessor,
    BothPredecessorsDefine,
    DefinedBeforeLoop,
    Scratch,
    RegistersOnly,
    BothEntrypointsDefine,
    NoDefinition,
    UseDefWithoutDefinition,
    ColdUseWithoutDefinition,
    OnePredecessorDefines,
    DefinedAfterUseInLoop,
    TerminalResultOnOneEdge,
    OneEntrypointDefines,
};

static void appendDefinition(BasicBlock* block, Tmp tmp)
{
    appendChecked(block, Move, Arg::imm(1), tmp);
}

static void appendUseAndReturn(BasicBlock* block, Tmp tmp)
{
    appendChecked(block, Move, tmp, returnTmp());
    appendReturn(block);
}

static void buildTmpDiamond(Code& code, Tmp tmp, bool bothSidesDefine)
{
    BasicBlock* root = code.addBlock();
    BasicBlock* left = code.addBlock();
    BasicBlock* right = code.addBlock();
    BasicBlock* join = code.addBlock();
    appendBranchTest(root, argumentTmp(0), left, right);
    appendDefinition(left, tmp);
    appendJump(left, join);
    if (bothSidesDefine)
        appendDefinition(right, tmp);
    appendJump(right, join);
    appendUseAndReturn(join, tmp);
}

static void buildTmpLoop(Code& code, Tmp tmp, bool isDefinedBeforeLoop)
{
    BasicBlock* root = code.addBlock();
    BasicBlock* loop = code.addBlock();
    BasicBlock* exit = code.addBlock();
    if (isDefinedBeforeLoop)
        appendDefinition(root, tmp);
    appendJump(root, loop);
    appendChecked(loop, Move, tmp, returnTmp());
    appendDefinition(loop, tmp);
    appendChecked(loop, Sub64, Arg::imm(1), argumentTmp(1));
    appendBranchTest(loop, argumentTmp(1), loop, exit);
    appendReturn(exit);
}

static void buildTmpUseAfterTwoEntrypoints(B3::Procedure& proc, Tmp tmp, bool bothEntrypointsDefine)
{
    proc.setNumEntrypoints(2);
    Code& code = proc.code();
    BasicBlock* first = code.addBlock();
    BasicBlock* second = code.addBlock();
    BasicBlock* user = code.addBlock();
    appendDefinition(first, tmp);
    appendJump(first, user);
    if (bothEntrypointsDefine)
        appendDefinition(second, tmp);
    appendJump(second, user);
    appendUseAndReturn(user, tmp);
    code.setEntrypoints(Vector<FrequentedBlock> { first, second });
}

static void buildTmpGraph(B3::Procedure& proc, TmpGraph graph, Tmp tmp)
{
    Code& code = proc.code();
    switch (graph) {
    case TmpGraph::SameBlock: {
        BasicBlock* root = code.addBlock();
        appendDefinition(root, tmp);
        appendUseAndReturn(root, tmp);
        return;
    }
    case TmpGraph::Predecessor: {
        BasicBlock* root = code.addBlock();
        BasicBlock* user = code.addBlock();
        appendDefinition(root, tmp);
        appendJump(root, user);
        appendUseAndReturn(user, tmp);
        return;
    }
    case TmpGraph::BothPredecessorsDefine:
        buildTmpDiamond(code, tmp, true);
        return;
    case TmpGraph::DefinedBeforeLoop:
        buildTmpLoop(code, tmp, true);
        return;
    case TmpGraph::Scratch: {
        BasicBlock* root = code.addBlock();
        Arg slot = Arg::stack(code.addStackSlot(8, StackSlotKind::Locked));
        appendChecked(root, Move, slot, slot, tmp);
        appendReturn(root, 0);
        return;
    }
    case TmpGraph::RegistersOnly:
        appendUseAndReturn(code.addBlock(), argumentTmp(1));
        return;
    case TmpGraph::BothEntrypointsDefine:
        buildTmpUseAfterTwoEntrypoints(proc, tmp, true);
        return;
    case TmpGraph::NoDefinition:
        appendUseAndReturn(code.addBlock(), tmp);
        return;
    case TmpGraph::UseDefWithoutDefinition: {
        BasicBlock* root = code.addBlock();
        appendChecked(root, Add64, Arg::imm(1), tmp);
        appendReturn(root, 0);
        return;
    }
    case TmpGraph::ColdUseWithoutDefinition: {
        BasicBlock* root = code.addBlock();
        B3::PatchpointValue* patchpoint = addEmptyPatchpoint(proc);
        patchpoint->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::LateColdAny);
        appendCheckedPatch(root, patchpoint, patchpointSpecialOf(code), tmp);
        appendReturn(root, 0);
        return;
    }
    case TmpGraph::OnePredecessorDefines:
        buildTmpDiamond(code, tmp, false);
        return;
    case TmpGraph::DefinedAfterUseInLoop:
        buildTmpLoop(code, tmp, false);
        return;
    case TmpGraph::TerminalResultOnOneEdge: {
        BasicBlock* root = code.addBlock();
        BasicBlock* edge = code.addBlock();
        BasicBlock* other = code.addBlock();
        BasicBlock* user = code.addBlock();
        appendCheckedPatch(root, addTerminalPatchpoint(proc), patchpointSpecialOf(code), returnTmp());
        root->setSuccessors(edge, other);
        appendChecked(edge, Move, returnTmp(), tmp);
        appendJump(edge, user);
        appendJump(other, user);
        appendUseAndReturn(user, tmp);
        return;
    }
    case TmpGraph::OneEntrypointDefines:
        buildTmpUseAfterTwoEntrypoints(proc, tmp, false);
        return;
    }
}

void testTmpsAreDefinedAtUses()
{
    auto check = [](TmpGraph graph, bool isDefined) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        Tmp tmp = code.newTmp(GP);
        buildTmpGraph(proc, graph, tmp);
        code.resetReachability();
        if (!isDefined) {
            CHECK(findUndefinedTmp(code) == tmp);
            return;
        }
        CHECK(!findUndefinedTmp(code));
        validate(code);
    };

    for (TmpGraph graph : { TmpGraph::SameBlock, TmpGraph::Predecessor, TmpGraph::BothPredecessorsDefine, TmpGraph::DefinedBeforeLoop, TmpGraph::Scratch, TmpGraph::RegistersOnly,
        TmpGraph::BothEntrypointsDefine })
        check(graph, true);
    for (TmpGraph graph : { TmpGraph::NoDefinition, TmpGraph::UseDefWithoutDefinition, TmpGraph::ColdUseWithoutDefinition, TmpGraph::OnePredecessorDefines,
        TmpGraph::DefinedAfterUseInLoop, TmpGraph::TerminalResultOnOneEdge, TmpGraph::OneEntrypointDefines })
        check(graph, false);
}

void testBlockEntryClobberPosition()
{
    enum class Position : uint8_t { HeadOfSuccessor, SecondInSuccessor, HeadOfFirstBlock, HeadOfSecondEntrypoint };
    for (Position position : { Position::HeadOfSuccessor, Position::SecondInSuccessor, Position::HeadOfFirstBlock, Position::HeadOfSecondEntrypoint }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        Arg special = Arg::special(code.addSpecial(makeUnique<BlockEntryClobberSpecial>(callClobberedRegisters())));
        BasicBlock* root = code.addBlock();
        BasicBlock* successor = code.addBlock();
        BasicBlock* clobbered = position == Position::HeadOfFirstBlock ? root : successor;
        if (position == Position::SecondInSuccessor)
            appendChecked(successor, Move, Arg::imm(1), argumentTmp(1));
        unsigned patchIndex = clobbered->size();
        appendCheckedInst(clobbered, Inst(Patch, nullptr, special));
        if (position == Position::HeadOfSecondEntrypoint) {
            proc.setNumEntrypoints(2);
            appendReturn(root, 0);
            code.setEntrypoints(Vector<FrequentedBlock> { root, successor });
        } else
            appendJump(root, successor);
        appendReturn(successor, 0);
        code.resetReachability();

        Inst* misplaced = findMisplacedBlockEntryClobber(code);
        if (position != Position::HeadOfSuccessor) {
            CHECK(misplaced == &clobbered->at(patchIndex));
            continue;
        }
        CHECK(!misplaced);
        validate(code);
    }
}
