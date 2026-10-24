/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun_validate.h"

#if ENABLE(B3_JIT)

#include "B3FixSSA.h"

struct TerminalResultTestGraph : ValidationTestGraph {
    TerminalResultTestGraph()
    {
        result->effects.terminal = true;
    }

    void appendReturn(BasicBlock* block, Value* value = nullptr)
    {
        block->appendNew<Value>(proc, Return, Origin(), value ? value : block->appendNew<Const32Value>(proc, Origin(), 0));
    }

    void appendJump(BasicBlock* block, BasicBlock* target)
    {
        block->appendNew<Value>(proc, Jump, Origin());
        block->setSuccessors(FrequentedBlock(target));
    }

    std::optional<ValidationRule> validate()
    {
        proc.resetReachability();
        return findViolatedRule(proc);
    }

    PatchpointValue* result { appendPatchpoint(Int32) };
    BasicBlock* first { proc.addBlock() };
    BasicBlock* second { proc.addBlock() };
};

static void testValidateTerminalResultOnFirstEdge()
{
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendReturn(graph.first, graph.result);
        graph.appendReturn(graph.second);
        CHECK(graph.validate() == std::nullopt);
    }
    {
        TerminalResultTestGraph graph;
        BasicBlock* dominated = graph.proc.addBlock();
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendJump(graph.first, dominated);
        graph.appendReturn(dominated, graph.result);
        graph.appendReturn(graph.second);
        CHECK(graph.validate() == std::nullopt);
    }
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.first->appendNew<Value>(graph.proc, Branch, Origin(), graph.result);
        graph.first->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendReturn(graph.second);
        CHECK(graph.validate() == std::nullopt);
    }
}

static void testValidateUnusedTerminalResult()
{
    {
        TerminalResultTestGraph graph;
        CHECK(graph.validate() == std::nullopt);
    }
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendReturn(graph.first);
        graph.appendJump(graph.second, graph.first);
        CHECK(graph.validate() == std::nullopt);
    }
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.first));
        graph.appendReturn(graph.first);
        CHECK(graph.validate() == std::nullopt);
    }
}

static void testValidateTerminalResultOffFirstEdge()
{
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendReturn(graph.first, graph.result);
        graph.appendJump(graph.second, graph.first);
        CHECK(graph.validate() == ValidationRule::TerminalResult);
    }
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendReturn(graph.first);
        graph.appendReturn(graph.second, graph.result);
        CHECK(graph.validate() == ValidationRule::TerminalResult);
    }
    {
        TerminalResultTestGraph graph;
        BasicBlock* join = graph.proc.addBlock();
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
        graph.appendJump(graph.first, join);
        graph.appendJump(graph.second, join);
        graph.appendReturn(join, graph.result);
        CHECK(graph.validate() == ValidationRule::TerminalResult);
    }
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.first));
        graph.appendReturn(graph.first, graph.result);
        CHECK(graph.validate() == ValidationRule::TerminalResult);
    }
    {
        TerminalResultTestGraph graph;
        graph.root->setSuccessors(FrequentedBlock(graph.root), FrequentedBlock(graph.first));
        graph.appendReturn(graph.first, graph.result);
        CHECK(graph.validate() == ValidationRule::TerminalResult);
    }
}

static void testDemoteTerminalResult()
{
    TerminalResultTestGraph graph;
    graph.root->setSuccessors(FrequentedBlock(graph.first), FrequentedBlock(graph.second));
    graph.appendReturn(graph.first, graph.result);
    graph.appendReturn(graph.second);
    graph.proc.resetReachability();

    IndexSet<Value*> valuesToDemote;
    valuesToDemote.add(graph.result);
    demoteValues(graph.proc, valuesToDemote);

    CHECK(graph.validate() == std::nullopt);
    CHECK(graph.first->at(0)->opcode() == B3::Set);
    CHECK(graph.first->at(0)->child(0) == graph.result);
    for (Value* value : *graph.second)
        CHECK(value->opcode() != B3::Set);
}

inline void addBunValidateTerminalResultTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testValidateTerminalResultOnFirstEdge());
    RUN(testValidateUnusedTerminalResult());
    RUN(testValidateTerminalResultOffFirstEdge());
    RUN(testDemoteTerminalResult());
}

#endif // ENABLE(B3_JIT)
