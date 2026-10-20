/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun.h"

#if ENABLE(B3_JIT)

struct Diamond {
    explicit Diamond(Graph& graph)
        : graph(graph)
        , root(graph.proc.addBlock())
        , left(graph.proc.addBlock())
        , right(graph.proc.addBlock())
        , join(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t>(graph.proc, root);
        condition = arguments[0];
        operand = arguments[1];
        graph.branch(root, condition, left, right);
        graph.jump(left, join);
        graph.jump(right, join);
    }

    Value* appendSum() { return graph.make(join, Add, operand, graph.int64(join, 1)); }

    void returnSumFromJoin()
    {
        graph.ret(join, appendSum());
        resetAndValidate(graph.proc);
    }

    void returnProductAfterJoin()
    {
        BasicBlock* after = graph.proc.addBlock();
        Value* sum = appendSum();
        graph.jump(join, after);
        graph.ret(after, graph.make(after, Mul, sum, graph.int64(after, 2)));
        resetAndValidate(graph.proc);
    }

    void checkResult(int64_t expected)
    {
        fixSSA(graph.proc);
        validate(graph.proc);
        auto code = compileProc(graph.proc);
        for (int64_t conditionValue : { 1, 0 })
            CHECK_EQ(invoke<int64_t>(*code, conditionValue, static_cast<int64_t>(5)), expected);
    }

    Graph& graph;
    BasicBlock* root;
    BasicBlock* left;
    BasicBlock* right;
    BasicBlock* join;
    Value* condition;
    Value* operand;
};

#endif // ENABLE(B3_JIT)
