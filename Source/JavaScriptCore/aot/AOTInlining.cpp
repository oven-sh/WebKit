/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

#include "AOTCompiler.h"
#include "AOTProgram.h"
#include "BytecodeStructs.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

// A call of a function that is proven to be the callee is replaced by what the function does. See Graph::adopt().
//
// The block the call is in ends where the call was, and goes on to where the function begins. What came after the call is a block of its
// own, which is where the function's returns go. A value of the caller's that is used after the call was made before it, so it is still
// made before it is used: nothing has to be done about those. What the function calls its parameters are the values the call passed.
class Inliner {
public:
    Inliner(Graph& graph, const CodeOfProgram& program)
        : m_graph(graph)
        , m_program(program)
    {
    }

    void run()
    {
        // (What is taken over comes after what there is, so what it calls gets its turn.)
        for (unsigned i = 0; i < m_graph.blocks.size(); ++i) {
            BasicBlock* block = m_graph.blocks[i].get();
            if (!block->isReachable || block->isGeneric)
                continue;
            for (unsigned index = 0; index < block->nodes.size(); ++index) {
                // (The rest of the block is another block from then on, which comes later.)
                if (tryInline(block, index))
                    break;
            }
        }
        if (!m_didInline)
            return;

        UncheckedKeyHashSet<Node*> used;
        UncheckedKeyHashMap<Node*, Node*> onlyUser; // Null: more than one.
        for (auto& block : m_graph.blocks) {
            auto resolveUsesOf = [&](Node* node) {
                for (auto& use : node->uses) {
                    use.node = resolve(use.node);
                    used.add(use.node);
                    if (use.node->isBytecode(op_new_func_exp)) {
                        auto result = onlyUser.add(use.node, node);
                        if (!result.isNewEntry && result.iterator->value != node)
                            result.iterator->value = nullptr;
                    }
                }
                if (node->target)
                    node->target = resolve(node->target);
            };
            for (Node* phi : block->phis)
                resolveUsesOf(phi);
            for (Node* node : block->nodes)
                resolveUsesOf(node);
            for (auto& value : block->valuesAtTail) {
                if (value)
                    value = resolve(value);
            }
        }
        // What was read, or made, only to be called. (Graph::elideReadsOfCalleesNotPassed() goes by who uses a read, and nobody does.)
        for (Node* read : m_calleesRead) {
            if (!used.contains(read))
                read->isElided = true;
        }
        // A closure that nothing wants but a call that is seldom made is made when that is. (Making one does nothing that anybody can see.)
        for (Node* call : m_callsAfterAll) {
            for (auto& use : call->uses) {
                Node* closure = use.node;
                if (!closure->isBytecode(op_new_func_exp) || onlyUser.get(closure) != call || closure->block == call->block)
                    continue;
                closure->block->nodes.removeFirst(closure);
                closure->block = call->block;
                call->block->nodes.insert(0, closure);
            }
        }
        m_graph.computeOrderOfBlocks();
    }

    static bool isWorthIt(UnlinkedCodeBlock* callee, const ProgramFacts* facts)
    {
        unsigned size = callee->instructionsSize();
        // There is going to be no other copy of it.
        if (facts && facts->isClosed && facts->directCalls.load(std::memory_order_relaxed) == 1)
            return size <= Options::aotInlinesOnlyCallUpTo();
        return size <= Options::aotInlinesUpTo();
    }

    // Plain from its bytecode.
    static bool canBePartOfAnother(UnlinkedCodeBlock* callee)
    {
        if (callee->codeType() != FunctionCode || callee->isConstructor() || callee->numberOfExceptionHandlers())
            return false;
        if (callee->parseMode() != SourceParseMode::NormalFunctionMode && callee->parseMode() != SourceParseMode::ArrowFunctionMode && callee->parseMode() != SourceParseMode::MethodMode)
            return false;
        // What it says by a number has to mean the same wherever it is said.
        if (!numbersOfIdentifiersOfProgram() || !numbersOfConstantsOfProgramFor(callee))
            return false;
        for (const auto& instruction : callee->instructions()) {
            switch (instruction->opcodeID()) {
            // These go by the frame.
            case op_call_direct_eval:
            case op_push_with_scope:
            case op_catch:
            case op_super_construct:
            case op_super_construct_varargs:
            case op_create_direct_arguments:
            case op_create_scoped_arguments:
            case op_create_cloned_arguments:
            case op_create_rest:
            case op_argument_count:
                return false;
            default:
                break;
            }
        }
        return true;
    }

private:
    static constexpr unsigned deepest = 4;

    static Node* resolve(Node* node)
    {
        while (node->replacement)
            node = node->replacement;
        return node;
    }

    bool tryInline(BasicBlock* block, unsigned index)
    {
        Node* call = block->nodes[index];
        if (call->kind != NodeKind::Bytecode || call->guard || call->guarded)
            return false;
        VirtualRegister calleeRegister;
        unsigned argc;
        unsigned argv;
        switch (call->opcode) {
        case op_call: {
            auto bytecode = call->as<OpCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
            break;
        }
        case op_call_ignore_result: {
            auto bytecode = call->as<OpCallIgnoreResult>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
            break;
        }
        // (What comes after it returns what it returned.)
        case op_tail_call: {
            auto bytecode = call->as<OpTailCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
            break;
        }
        default:
            return false;
        }

        Graph& caller = *call->graph;
        Node* calleeNode = resolve(call->use(calleeRegister));
        UnlinkedFunctionCodeBlock* callee = nullptr;
        Node* scopeOfClosure = nullptr;
        unsigned intrinsicToCheckFor = 0;
        if (calleeNode->isBytecode(op_new_func_exp)) {
            // The closure is made here, so what it closes over is at hand.
            auto bytecode = calleeNode->as<OpNewFuncExp>();
            callee = calleeNode->graph->codeBlock()->functionExpr(bytecode.m_functionDecl)->codeBlockIfThereIsOne(CodeSpecializationKind::CodeForCall);
            if (!callee || readsCallee(callee))
                return false;
            scopeOfClosure = resolve(calleeNode->use(bytecode.m_scope));
        } else if (unsigned intrinsic = methodOfArraysThatMayWellBeCalled(call, calleeNode, argc, argv)) {
            callee = m_program.codeOfBuiltin(ImmutableIntrinsics::shared()->at(intrinsic).builtinCode - 1);
            if (!callee || readsCallee(callee))
                return false;
            intrinsicToCheckFor = ImmutableIntrinsics::shared()->at(intrinsic).canonical;
        } else {
            bool isProven = false;
            const KnownFunction* known = caller.knownCallee(call, &isProven);
            if (!known || !isProven || !known->forCall || !known->isDeclaration || !caller.passesNoFunctionObject(call))
                return false;
            callee = known->forCall;
        }
        auto about = m_program.about(callee);
        // (A closure that is called where it is made is as good as called from one place.)
        if (!about || !canBePartOfAnother(callee) || !(scopeOfClosure || intrinsicToCheckFor ? callee->instructionsSize() <= Options::aotInlinesOnlyCallUpTo() : isWorthIt(callee, about->facts)))
            return false;
        if (m_sizeTakenOver + callee->instructionsSize() > Options::aotInlinesAtMost() || m_graph.inlineFrames.size() > PackedSite::mostInlineFrames)
            return false;
        unsigned depth = 0;
        for (unsigned frame = caller.inlineFrame(); frame; frame = m_graph.inlineFrames[frame].parent)
            ++depth;
        if (depth >= deepest || m_graph.codeBlock() == callee)
            return false;
        for (Graph* graph : m_chain(caller)) {
            if (graph->codeBlock() == callee)
                return false;
        }

        auto inlinee = makeUniqueWithoutFastMallocCheck<Graph>(m_graph.vm(), callee, unknownScopeChain());
        inlinee->setCalleeHints(about->hints);
        inlinee->setVariableFacts(m_graph.variableFacts());
        inlinee->setLinkage(about->linkage, declaredNamesFor(callee));
        inlinee->loopsAreNotSplit = !!intrinsicToCheckFor;
        if (!scopeOfClosure && !intrinsicToCheckFor && (inlinee->needsFunctionObject() || !inlinee->scopeIsEnvironmentOfModule()))
            return false;
        if (!parseBytecode(*inlinee) || !inlinee->catchEntrypoints.isEmpty() || inlinee->hasHomedRegisters())
            return false;

        // Where it returns, and what.
        Vector<std::pair<BasicBlock*, Node*>, 4> returns;
        for (BasicBlock* itsBlock : inlinee->m_rpo) {
            Node* terminal = itsBlock->terminal();
            if (!terminal || terminal->kind != NodeKind::Bytecode)
                continue;
            // (What comes after one of these returns what it returned. It is a call like any other now.)
            if (terminal->opcode == op_ret)
                returns.append({ itsBlock, terminal->use(terminal->as<OpRet>().m_value) });
        }
        if (returns.isEmpty())
            return false;
        // (One of the engine's own has no use for the scope it was made in: there is nothing there.)
        if (intrinsicToCheckFor) {
            for (BasicBlock* itsBlock : inlinee->m_rpo) {
                for (Node* node : itsBlock->nodes) {
                    for (auto& use : node->uses) {
                        if (use.node->isBytecode(op_get_scope))
                            return false;
                    }
                }
            }
        }

        // ---- Nothing is in the way.
        m_didInline = true;
        m_sizeTakenOver += callee->instructionsSize();
        m_parents.add(inlinee.get(), &caller);
        inlinee->isInTailPosition = call->opcode == op_tail_call && caller.isInTailPosition;

        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        BasicBlock* entry = inlinee->root;
        entry->nodes.removeAllMatching([&](Node* node) {
            if (node->kind != NodeKind::Argument)
                return false;
            RELEASE_ASSERT(node->reg.isArgument());
            unsigned argument = node->reg.toArgument();
            node->replacement = argument < argc ? call->use(VirtualRegister(firstArgument + static_cast<int>(argument))) : m_graph.constant(jsUndefined());
            return true;
        });
        for (BasicBlock* itsBlock : inlinee->m_rpo) {
            itsBlock->nodes.removeAllMatching([&](Node* node) {
                if (intrinsicToCheckFor && node->isBytecode(op_get_scope))
                    return true;
                // (So that nothing is left that wants the closure but what calls it.)
                if (node->isBytecode(op_is_callable) && resolve(node->use(node->as<OpIsCallable>().m_operand))->isBytecode(op_new_func_exp)) {
                    node->replacement = m_graph.constant(jsBoolean(true));
                    return true;
                }
                if (!node->isBytecode(op_get_argument))
                    return false;
                // (It counts `this`.)
                unsigned argument = node->as<OpGetArgument>().m_index;
                node->replacement = argument < argc ? call->use(VirtualRegister(firstArgument + static_cast<int>(argument))) : m_graph.constant(jsUndefined());
                return true;
            });
        }

        if (scopeOfClosure) {
            inlinee->scopeOfClosure = scopeOfClosure;
            for (BasicBlock* itsBlock : inlinee->m_rpo) {
                for (Node* node : itsBlock->nodes) {
                    if (node->isBytecode(op_get_scope))
                        node->uses.append({ VirtualRegister(), scopeOfClosure });
                }
            }
        }

        BasicBlock* continuation = m_graph.addBlock();
        continuation->graph = block->graph;
        continuation->bytecodeBegin = call->bytecodeIndex.offset() + call->instruction->size();
        continuation->bytecodeEnd = block->bytecodeEnd;
        continuation->isReachable = true;
        continuation->isInLoop = block->isInLoop;
        continuation->endsWithGuard = std::exchange(block->endsWithGuard, false);
        continuation->valuesAtTail = std::exchange(block->valuesAtTail, { });
        for (unsigned i = index + 1; i < block->nodes.size(); ++i) {
            block->nodes[i]->block = continuation;
            continuation->nodes.append(block->nodes[i]);
        }
        block->nodes.shrink(index);
        block->bytecodeEnd = call->bytecodeIndex.offset();
        continuation->successors = std::exchange(block->successors, { });
        for (BasicBlock* successor : continuation->successors) {
            for (auto& predecessor : successor->predecessors) {
                if (predecessor == block)
                    predecessor = continuation;
            }
        }
        block->successors.append(entry);
        entry->predecessors.append(block);
        // If it is not the function it was taken for, the call is made after all.
        Node* callAfterAll = nullptr;
        if (intrinsicToCheckFor) {
            Node* guard = m_graph.addNode(NodeKind::Guard);
            guard->graph = block->graph;
            guard->guardKind = GuardKind::IsIntrinsic;
            guard->intrinsic = intrinsicToCheckFor;
            guard->opcode = call->opcode;
            guard->instruction = call->instruction;
            guard->bytecodeIndex = call->bytecodeIndex;
            guard->block = block;
            guard->uses.append({ VirtualRegister(), calleeNode });
            block->nodes.append(guard);
            block->endsWithGuard = true;

            unsigned index = m_graph.addNode(NodeKind::Bytecode)->index;
            callAfterAll = m_graph.lastNode();
            *callAfterAll = *call;
            callAfterAll->index = index;
            BasicBlock* otherwise = m_graph.addBlock();
            otherwise->graph = block->graph;
            otherwise->bytecodeBegin = call->bytecodeIndex.offset();
            otherwise->bytecodeEnd = continuation->bytecodeBegin;
            otherwise->isReachable = true;
            otherwise->isInLoop = block->isInLoop;
            otherwise->isSeldomReached = true;
            callAfterAll->block = otherwise;
            otherwise->nodes.append(callAfterAll);
            block->successors.append(otherwise);
            otherwise->predecessors.append(block);
            otherwise->successors.append(continuation);
            continuation->predecessors.append(otherwise);
            m_callsAfterAll.append(callAfterAll);
        }

        for (auto& [itsBlock, value] : returns) {
            itsBlock->nodes.removeLast();
            RELEASE_ASSERT(itsBlock->successors.isEmpty());
            itsBlock->successors.append(continuation);
            continuation->predecessors.append(itsBlock);
        }
        if (returns.size() == 1 && !callAfterAll)
            call->replacement = returns[0].second;
        else {
            Node* phi = m_graph.addNode(NodeKind::Phi);
            phi->graph = block->graph;
            phi->block = continuation;
            phi->range = IntegerRange::unknown();
            // (In the order of the predecessors.)
            if (callAfterAll)
                phi->uses.append({ VirtualRegister(), callAfterAll });
            for (auto& [itsBlock, value] : returns)
                phi->uses.append({ VirtualRegister(), value });
            continuation->phis.append(phi);
            call->replacement = phi;
        }
        if (block->isInLoop) {
            for (BasicBlock* itsBlock : inlinee->m_rpo)
                itsBlock->isInLoop = true;
        }
        if (!callAfterAll)
            m_calleesRead.append(calleeNode);
        // (One that is part of another keeps what there is to say about all of its sites.)
        if (caller.isOutermost())
            m_graph.callSites.append(call->bytecodeIndex.offset());

        Graph::InlineFrame frame { caller.inlineFrame(), CallSiteIndex(call->bytecodeIndex).bits(), m_graph.indexOfKnownCallee(about->key), call->opcode == op_tail_call };
        m_graph.adopt(WTF::move(inlinee), frame);
        return true;
    }

    // The name a method is called by is a reason to have the code for it, and no more: whether it is that one is looked at when the call
    // is made. It is worth it when what is passed is a closure made for the occasion, which then need not be made at all.
    unsigned methodOfArraysThatMayWellBeCalled(Node* call, Node* calleeNode, unsigned argc, unsigned argv)
    {
        if (!Options::aotInlinesBuiltins() || !Options::useImmutableIntrinsics() || !calleeNode->isBytecode(op_get_by_id) || argc < 2)
            return 0;
        const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
        if (!intrinsics)
            return 0;
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        auto bytecode = calleeNode->as<OpGetById>();
        if (resolve(call->use(VirtualRegister(firstArgument))) != resolve(calleeNode->use(bytecode.m_base)))
            return 0;
        if (!resolve(call->use(VirtualRegister(firstArgument + 1)))->isBytecode(op_new_func_exp))
            return 0;
        static const unsigned prototype = [&] {
            unsigned array = intrinsics->find(ImmutableIntrinsics::globalObject, *String("Array"_s).impl());
            return array ? intrinsics->find(intrinsics->at(array).canonical, *String("prototype"_s).impl()) : 0;
        }();
        if (!prototype)
            return 0;
        unsigned method = intrinsics->find(intrinsics->at(prototype).canonical, *calleeNode->graph->codeBlock()->identifier(bytecode.m_property).impl());
        return method && intrinsics->at(method).builtinCode ? method : 0;
    }

    // The graph and what it is part of, but for the outermost.
    Vector<Graph*, 4> m_chain(Graph& graph)
    {
        Vector<Graph*, 4> result;
        for (Graph* at = &graph; at && at != &m_graph; at = m_parents.get(at))
            result.append(at);
        return result;
    }

    Graph& m_graph;
    const CodeOfProgram& m_program;
    UncheckedKeyHashMap<Graph*, Graph*> m_parents;
    Vector<Node*, 8> m_calleesRead;
    Vector<Node*, 4> m_callsAfterAll;
    unsigned m_sizeTakenOver { 0 };
    bool m_didInline { false };
};

} // anonymous namespace

void inlineCalls(Graph& graph, const CodeOfProgram& program)
{
    // (A site has to have room to say which call it is in.)
    if (!Options::aotInlines() || !PackedSite::fits(CallSiteIndex(BytecodeIndex(graph.codeBlock()->instructionsSize())).bits()))
        return;
    Inliner(graph, program).run();
}

bool mayBecomePartOfAnother(UnlinkedCodeBlock* codeBlock, const ProgramFacts* facts)
{
    return Options::aotInlines() && Inliner::isWorthIt(codeBlock, facts) && Inliner::canBePartOfAnother(codeBlock);
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
