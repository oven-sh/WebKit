/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(AOT)

#include "AOTCompiler.h"
#include "AOTProgram.h"
#include "BuiltinExecutables.h"
#include "BytecodeStructs.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

class Inliner {
public:
    Inliner(Graph& graph, const ProgramCode& program)
        : m_graph(graph)
        , m_program(program)
    {
    }

    void run()
    {
        for (unsigned i = 0; i < m_graph.blocks.size(); ++i) {
            BasicBlock* block = m_graph.blocks[i].get();
            if (!block->isReachable || block->isGeneric || block->isRarelyExecuted)
                continue;
            for (unsigned index = 0; index < block->nodes.size(); ++index) {
                if (elideTypeCheckOfClosure(block->nodes[index])) {
                    block->nodes.removeAt(index--);
                    m_didInline = true;
                    continue;
                }
                if (tryInline(block, index))
                    break;
            }
        }
        if (!m_didInline)
            return;

        UncheckedKeyHashSet<Node*> used;
        UncheckedKeyHashMap<Node*, Node*> soleUser;
        for (auto& block : m_graph.blocks) {
            auto resolveUsesOf = [&](Node* node) {
                for (auto& use : node->uses) {
                    use.node = resolve(use.node);
                    used.add(use.node);
                    if (use.node->isBytecode(op_new_func_exp)) {
                        auto result = soleUser.add(use.node, node);
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
        for (Node* read : m_calleesRead) {
            if (used.contains(read))
                continue;
            if (Graph::closedMethodReadBy(read))
                read->isReadOnlyForCall = true;
            else
                read->isElided = true;
        }
        for (Node* call : m_fallbackCalls) {
            for (auto& use : call->uses) {
                Node* closure = use.node;
                if (!closure->isBytecode(op_new_func_exp) || soleUser.get(closure) != call || closure->block == call->block)
                    continue;
                closure->block->nodes.removeFirst(closure);
                closure->block = call->block;
                call->block->nodes.insert(0, closure);
            }
        }
        m_graph.computeBlockOrder();
    }

    static bool isProfitable(UnlinkedCodeBlock* callee, const FunctionSummary* summary, bool isCalledInLoop = true)
    {
        if (summary && !summary->isReached())
            return false;
        unsigned size = callee->instructionsSize();
        if (summary && summary->isNonEscaping && summary->directCalls.load(std::memory_order_relaxed) == 1)
            return size <= Options::maximumAOTInlineCandidateBytecodeCostForSingleCallSite();
        return size <= (isCalledInLoop ? Options::maximumAOTInlineCandidateBytecodeCostInLoop() : std::min(Options::maximumAOTInlineCandidateBytecodeCost(), Options::maximumAOTInlineCandidateBytecodeCostInLoop()));
    }

    static bool canBeInlinedIntoCaller(UnlinkedCodeBlock* callee)
    {
        if (callee->codeType() != FunctionCode || callee->numberOfExceptionHandlers())
            return false;
        if (callee->isConstructor() && callee->constructorKind() == ConstructorKind::Extends)
            return false;
        if (callee->parseMode() != SourceParseMode::NormalFunctionMode && callee->parseMode() != SourceParseMode::ArrowFunctionMode && callee->parseMode() != SourceParseMode::MethodMode)
            return false;
        if (!programIdentifierIndices() || !programConstantIndicesFor(callee))
            return false;
        for (const auto& instruction : callee->instructions()) {
            switch (instruction->opcodeID()) {
            case op_call_direct_eval:
            case op_push_with_scope:
            case op_catch:
            case op_super_construct:
            case op_super_construct_varargs:
            case op_create_direct_arguments:
            case op_create_scoped_arguments:
            case op_create_cloned_arguments:
            case op_create_rest:
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

    static std::optional<bool> areStrictlyEqual(Node* left, Node* right)
    {
        auto isAlwaysCell = [](Node* node) {
            return node->kind == NodeKind::ConstantCell || node->isBytecode(op_new_object) || node->isBytecode(op_new_array) || node->isBytecode(op_new_array_buffer) || node->isBytecode(op_new_func_exp);
        };
        auto isNonCellConstant = [](Node* node) { return node->kind == NodeKind::Constant && node->constant; };
        if ((isAlwaysCell(left) && isNonCellConstant(right)) || (isNonCellConstant(left) && isAlwaysCell(right)))
            return false;
        if (!isNonCellConstant(left) || !isNonCellConstant(right))
            return std::nullopt;
        if (left->constant.isNumber() && right->constant.isNumber())
            return left->constant.asNumber() == right->constant.asNumber();
        return left->constant == right->constant;
    }

    static void removeEdge(BasicBlock* from, BasicBlock* to)
    {
        size_t index = to->predecessors.find(from);
        RELEASE_ASSERT(index != notFound);
        to->predecessors.removeAt(index);
        for (Node* phi : to->phis)
            phi->uses.removeAt(index);
    }

    void foldComparisonsOfKnownValues(Graph& inlinee)
    {
        UncheckedKeyHashSet<BasicBlock*> dropped;
        for (BasicBlock* block : inlinee.m_rpo) {
            Node* terminal = block->terminal();
            if (dropped.contains(block) || !terminal || block->successors.size() != 2 || block->successors[0] == block->successors[1])
                continue;
            std::optional<bool> isTaken;
            if (terminal->isBytecode(op_jnstricteq)) {
                auto bytecode = terminal->as<OpJnstricteq>();
                if (auto equal = areStrictlyEqual(resolve(terminal->use(bytecode.m_lhs)), resolve(terminal->use(bytecode.m_rhs))))
                    isTaken = !*equal;
            } else if (terminal->isBytecode(op_jstricteq)) {
                auto bytecode = terminal->as<OpJstricteq>();
                isTaken = areStrictlyEqual(resolve(terminal->use(bytecode.m_lhs)), resolve(terminal->use(bytecode.m_rhs)));
            }
            if (!isTaken)
                continue;
            BasicBlock* live = block->successors[*isTaken ? 0 : 1];
            BasicBlock* dead = block->successors[*isTaken ? 1 : 0];

            UncheckedKeyHashSet<BasicBlock*> reached;
            Vector<BasicBlock*, 16> worklist { inlinee.root };
            reached.add(inlinee.root);
            while (!worklist.isEmpty()) {
                BasicBlock* at = worklist.takeLast();
                for (BasicBlock* successor : at->successors) {
                    if ((at == block && successor == dead) || !reached.add(successor).isNewEntry)
                        continue;
                    worklist.append(successor);
                }
            }
            Vector<BasicBlock*, 4> unreached;
            bool dropsReturn = false;
            for (BasicBlock* other : inlinee.m_rpo) {
                if (reached.contains(other) || dropped.contains(other))
                    continue;
                unreached.append(other);
                dropsReturn |= other->terminal() && other->terminal()->isBytecode(op_ret);
            }
            if (dropsReturn)
                continue;

            block->nodes.removeLast();
            block->successors = { live };
            removeEdge(block, dead);
            for (BasicBlock* other : unreached) {
                dropped.add(other);
                for (BasicBlock* successor : other->successors) {
                    if (reached.contains(successor) && successor->predecessors.contains(other))
                        removeEdge(other, successor);
                }
            }
            m_graph.remark("folded-comparison-of-argument"_s);
        }
    }

    bool elideTypeCheckOfClosure(Node* node)
    {
        if (!node->isBytecode(op_check_type) || node->guard || node->guarded)
            return false;
        auto bytecode = node->as<OpCheckType>();
        Node* value = resolve(node->use(bytecode.m_value));
        if (bytecode.m_mask > SoundTypeAll || !(bytecode.m_mask & SoundTypeFunction) || !value->isBytecode(op_new_func_exp))
            return false;
        m_graph.remark("elided-type-check-of-closure"_s);
        node->replacement = value;
        return true;
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
        case op_tail_call: {
            auto bytecode = call->as<OpTailCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
            break;
        }
        case op_construct: {
            auto bytecode = call->as<OpConstruct>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
            break;
        }
        default:
            return false;
        }

        bool isConstruct = call->opcode == op_construct;
        bool checksCalleeIsInitialized = false;
        Graph& caller = *call->graph;
        Node* calleeNode = resolve(call->use(calleeRegister));
        UnlinkedFunctionCodeBlock* callee = nullptr;
        UnlinkedFunctionExecutable* calleeExecutable = nullptr;
        Node* closureScope = nullptr;
        Node* closureFunction = nullptr;
        unsigned guardedIntrinsic = 0;
        bool calleeIsProvenIntrinsic = false;
        bool isArraySpecialization = false;
        auto declineToInline = [&](ASCIILiteral why) {
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: a builtin is not made part of its caller at bc#", call->bytecodeIndex.offset(), ": ", why);
            return false;
        };
        if (isConstruct) {
            bool isExact = false;
            const KnownFunction* known = caller.knownCallee(call, &isExact);
            if (!known || !isExact || !known->forConstruct || !calleeNode->isBytecode(op_get_from_scope) || m_graph.codeBlock()->codeType() != FunctionCode)
                return false;
            if (resolve(call->use(VirtualRegister(-static_cast<int>(argv) + CallFrame::thisArgumentOffset()))) != calleeNode)
                return false;
            callee = known->forConstruct;
            calleeExecutable = known->executable;
            closureFunction = calleeNode;
            checksCalleeIsInitialized = !known->isDeclaration;
        } else if (calleeNode->isBytecode(op_new_func_exp)) {
            auto bytecode = calleeNode->as<OpNewFuncExp>();
            calleeExecutable = calleeNode->graph->codeBlock()->functionExpr(bytecode.m_functionDecl);
            callee = calleeExecutable->codeBlockIfExists(CodeSpecializationKind::CodeForCall);
            if (!callee || readsCallee(callee))
                return false;
            closureScope = resolve(calleeNode->use(bytecode.m_scope));
        } else if (unsigned intrinsic = likelyArrayMethod(call, calleeNode, argc, argv)) {
            callee = m_program.codeForBuiltin(ImmutableIntrinsics::shared()->at(intrinsic).builtinCode - 1);
            if (!callee || readsCallee(callee))
                return declineToInline(callee ? "it reads its callee"_s : "there is no code for it"_s);
            guardedIntrinsic = ImmutableIntrinsics::shared()->at(intrinsic).canonical;
            if (auto lean = arraySpecializationOf(calleeNode->graph->codeBlock()->identifier(calleeNode->as<OpGetById>().m_property).impl(), argc, call->opcode != op_call_ignore_result)) {
                if (UnlinkedFunctionCodeBlock* code = m_program.codeForBuiltin(static_cast<unsigned>(*lean)); code && !readsCallee(code)) {
                    callee = code;
                    isArraySpecialization = true;
                }
            }
            calleeIsProvenIntrinsic = Options::useAOTTypedFields() && TypeTable::typedFieldsAreEnforced() && TypeTable::shared()->isArray(Graph::typeTagOf(calleeNode));
        } else {
            bool isExact = false;
            const KnownFunction* known = caller.knownCallee(call, &isExact);
            if (!known || !isExact || !known->forCall)
                return false;
            if (!known->isDeclaration && !Graph::closedMethodReadBy(calleeNode)) {
                if (!calleeNode->isBytecode(op_get_from_scope) || call->opcode == op_tail_call)
                    return false;
                checksCalleeIsInitialized = true;
            }
            if (!caller.passesNoFunctionObject(call))
                closureFunction = calleeNode;
            callee = known->forCall;
            calleeExecutable = known->executable;
        }
        auto about = m_program.about(callee);
        if (guardedIntrinsic && (!about || !canBeInlinedIntoCaller(callee)))
            return declineToInline(about ? "of what is in its bytecode"_s : "nothing is known about its code"_s);
        if (!about || !canBeInlinedIntoCaller(callee) || !(closureScope || guardedIntrinsic ? callee->instructionsSize() <= Options::maximumAOTInlineCandidateBytecodeCostForSingleCallSite() : isProfitable(callee, about->summary, block->isInLoop || m_graph.isCalledRepeatedly())))
            return guardedIntrinsic ? declineToInline("it is too big"_s) : false;
        if (m_inlinedBytecodeSize + callee->instructionsSize() > Options::maximumAOTInliningCallerBytecodeCost() || m_graph.inlineFrames.size() > PackedSite::maxInlineFrames)
            return guardedIntrinsic ? declineToInline("the caller has taken over enough"_s) : false;
        unsigned depth = 0;
        for (unsigned frame = caller.inlineFrame(); frame; frame = m_graph.inlineFrames[frame].parent)
            ++depth;
        if (depth >= deepest || m_graph.codeBlock() == callee)
            return guardedIntrinsic ? declineToInline("it is too deep"_s) : false;
        if (!guardedIntrinsic) {
            for (Graph* graph : m_chain(caller)) {
                if (graph->codeBlock() == callee)
                    return false;
            }
        }

        auto inlinee = makeUniqueWithoutFastMallocCheck<Graph>(m_graph.vm(), callee, unknownScopeChain());
        inlinee->setCalleeHints(about->hints);
        inlinee->setVariableSummaries(m_graph.variableSummaries());
        inlinee->setLinkage(about->linkage, declaredNamesFor(callee));
        inlinee->loopSplittingIsDisabled = !!guardedIntrinsic || m_graph.loopSplittingIsDisabled;
        inlinee->isInlinedBuiltin = !!guardedIntrinsic;
        if (!closureScope && !closureFunction && !guardedIntrinsic && (inlinee->needsFunctionObject() || !inlinee->scopeIsModuleEnvironment()))
            return false;
        if (!parseBytecode(*inlinee) || !inlinee->catchEntrypoints.isEmpty() || inlinee->hasFrameRegisters())
            return guardedIntrinsic ? declineToInline("its bytecode is not parsed, or it catches, or it has registers with homes"_s) : false;

        Vector<std::pair<BasicBlock*, Node*>, 4> returns;
        for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
            Node* terminal = inlineeBlock->terminal();
            if (!terminal || terminal->kind != NodeKind::Bytecode)
                continue;
            if (terminal->opcode == op_ret)
                returns.append({ inlineeBlock, terminal->use(terminal->as<OpRet>().m_value) });
        }
        if (returns.isEmpty())
            return guardedIntrinsic ? declineToInline("it never returns"_s) : false;
        if (guardedIntrinsic) {
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                for (Node* node : inlineeBlock->nodes) {
                    for (auto& use : node->uses) {
                        if (use.node->isBytecode(op_get_scope))
                            return declineToInline("it uses its scope"_s);
                    }
                }
            }
        }

        dataLogLnIf(Options::verboseAOTCompilation() && guardedIntrinsic, "AOT: a builtin is made part of its caller at bc#", call->bytecodeIndex.offset());
        if (calleeExecutable)
            m_graph.remark(isConstruct ? "inlined-construct"_s : closureScope ? "inlined-closure"_s : "inlined-call"_s, calleeExecutable->ecmaName().string());
        else
            m_graph.remark("inlined-builtin"_s);
        m_didInline = true;
        m_inlinedBytecodeSize += callee->instructionsSize();
        m_parents.add(inlinee.get(), &caller);
        inlinee->isInTailPosition = call->opcode == op_tail_call && caller.isInTailPosition;

        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        BasicBlock* entry = inlinee->root;
        Node* arraySpecializationReceiver = nullptr;
        if (isArraySpecialization) {
            inlinee->readsElementsOrEmpty = true;
            arraySpecializationReceiver = m_graph.addNode(NodeKind::Narrow);
            arraySpecializationReceiver->narrowedTo = TArray;
            arraySpecializationReceiver->uses.append({ VirtualRegister(), call->use(VirtualRegister(firstArgument)) });
            if (calleeIsProvenIntrinsic) {
                arraySpecializationReceiver->checksNarrowedType = true;
                arraySpecializationReceiver->graph = call->graph;
                arraySpecializationReceiver->opcode = call->opcode;
                arraySpecializationReceiver->instruction = call->instruction;
                arraySpecializationReceiver->bytecodeIndex = call->bytecodeIndex;
            } else {
                arraySpecializationReceiver->graph = inlinee.get();
                arraySpecializationReceiver->block = entry;
            }
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                Node* terminal = inlineeBlock->terminal();
                if (!terminal || !terminal->isBytecode(op_jtrue) || inlineeBlock->successors.size() != 2)
                    continue;
                Node* test = terminal->use(terminal->as<OpJtrue>().m_condition);
                BasicBlock* isThere = inlineeBlock->successors[1];
                if (!test->isBytecode(op_is_empty) || isThere->predecessors.size() != 1)
                    continue;
                Node* element = test->use(test->as<OpIsEmpty>().m_operand);
                Node* known = m_graph.addNode(NodeKind::Narrow);
                known->graph = inlinee.get();
                known->block = isThere;
                known->narrowedTo = TAll & ~TEmpty;
                for (BasicBlock* other : inlinee->m_rpo) {
                    for (Node* node : other->nodes) {
                        if (node == test)
                            continue;
                        for (auto& use : node->uses) {
                            if (use.node == element)
                                use.node = known;
                        }
                    }
                }
                known->uses.append({ VirtualRegister(), element });
                isThere->nodes.insert(0, known);
            }
        }
        auto isCalleeRead = [&](Node* node) {
            if (node->kind != NodeKind::Argument || node->reg != VirtualRegister(CallFrameSlot::callee))
                return false;
            node->replacement = calleeNode;
            return true;
        };
        entry->nodes.removeAllMatching([&](Node* node) {
            if (node->kind != NodeKind::Argument)
                return false;
            if (isCalleeRead(node))
                return true;
            RELEASE_ASSERT(node->reg.isArgument());
            unsigned argument = node->reg.toArgument();
            node->replacement = !argument && arraySpecializationReceiver ? arraySpecializationReceiver : argument < argc ? call->use(VirtualRegister(firstArgument + static_cast<int>(argument))) : m_graph.constant(jsUndefined());
            return true;
        });
        if (arraySpecializationReceiver && !calleeIsProvenIntrinsic)
            entry->nodes.insert(0, arraySpecializationReceiver);
        for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
            inlineeBlock->nodes.removeAllMatching([&](Node* node) {
                if (isCalleeRead(node))
                    return true;
                if (guardedIntrinsic && node->isBytecode(op_get_scope))
                    return true;
                if (node->isBytecode(op_is_callable) && resolve(node->use(node->as<OpIsCallable>().m_operand))->isBytecode(op_new_func_exp)) {
                    node->replacement = m_graph.constant(jsBoolean(true));
                    return true;
                }
                if (elideTypeCheckOfClosure(node))
                    return true;
                if (node->isBytecode(op_argument_count)) {
                    node->replacement = m_graph.constant(jsNumber(argc - 1));
                    return true;
                }
                if (!node->isBytecode(op_get_argument))
                    return false;
                unsigned argument = node->as<OpGetArgument>().m_index;
                node->replacement = argument < argc ? call->use(VirtualRegister(firstArgument + static_cast<int>(argument))) : m_graph.constant(jsUndefined());
                return true;
            });
        }
        foldComparisonsOfKnownValues(*inlinee);

        if (closureFunction) {
            inlinee->closureFunction = closureFunction;
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                for (Node* node : inlineeBlock->nodes) {
                    if (node->isBytecode(op_get_scope))
                        node->uses.append({ VirtualRegister(), closureFunction });
                }
            }
        }
        if (closureScope) {
            inlinee->closureScope = closureScope;
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                for (Node* node : inlineeBlock->nodes) {
                    if (node->isBytecode(op_get_scope))
                        node->uses.append({ VirtualRegister(), closureScope });
                }
            }
        }

        BasicBlock* continuation = m_graph.addBlock();
        continuation->graph = block->graph;
        continuation->bytecodeBegin = call->bytecodeIndex.offset() + call->instruction->size();
        continuation->bytecodeEnd = block->bytecodeEnd;
        continuation->isReachable = true;
        continuation->isInLoop = block->isInLoop;
        continuation->isInProfitableLoop = block->isInProfitableLoop;
        continuation->endsWithGuard = std::exchange(block->endsWithGuard, false);
        continuation->valuesAtTail = std::exchange(block->valuesAtTail, { });
        for (unsigned i = index + 1; i < block->nodes.size(); ++i) {
            block->nodes[i]->block = continuation;
            continuation->nodes.append(block->nodes[i]);
        }
        block->nodes.shrink(index);
        block->bytecodeEnd = call->bytecodeIndex.offset();
        if (arraySpecializationReceiver && calleeIsProvenIntrinsic) {
            arraySpecializationReceiver->block = block;
            block->nodes.append(arraySpecializationReceiver);
        }
        continuation->successors = std::exchange(block->successors, { });
        for (BasicBlock* successor : continuation->successors) {
            for (auto& predecessor : successor->predecessors) {
                if (predecessor == block)
                    predecessor = continuation;
            }
        }
        block->successors.append(entry);
        entry->predecessors.append(block);
        Node* fallbackCall = nullptr;
        if ((guardedIntrinsic && !calleeIsProvenIntrinsic) || checksCalleeIsInitialized) {
            Node* guard = m_graph.addNode(NodeKind::Guard);
            guard->graph = block->graph;
            guard->guardKind = checksCalleeIsInitialized ? GuardKind::KnownCallee : isArraySpecialization ? GuardKind::IsArrayIntrinsic : GuardKind::IsIntrinsic;
            guard->intrinsic = guardedIntrinsic;
            guard->opcode = call->opcode;
            guard->instruction = call->instruction;
            guard->bytecodeIndex = call->bytecodeIndex;
            guard->block = block;
            guard->uses.append({ checksCalleeIsInitialized ? calleeRegister : VirtualRegister(), calleeNode });
            if (isArraySpecialization)
                guard->uses.append({ VirtualRegister(), call->use(VirtualRegister(firstArgument)) });
            block->nodes.append(guard);
            block->endsWithGuard = true;

            unsigned index = m_graph.addNode(NodeKind::Bytecode)->index;
            fallbackCall = m_graph.lastNode();
            *fallbackCall = *call;
            fallbackCall->index = index;
            BasicBlock* otherwise = m_graph.addBlock();
            otherwise->graph = block->graph;
            otherwise->bytecodeBegin = call->bytecodeIndex.offset();
            otherwise->bytecodeEnd = continuation->bytecodeBegin;
            otherwise->isReachable = true;
            otherwise->isInLoop = block->isInLoop;
            otherwise->isInProfitableLoop = block->isInProfitableLoop;
            otherwise->isRarelyExecuted = true;
            fallbackCall->block = otherwise;
            otherwise->nodes.append(fallbackCall);
            block->successors.append(otherwise);
            otherwise->predecessors.append(block);
            if (checksCalleeIsInitialized) {
                Node* unreachable = m_graph.addNode(NodeKind::Bytecode);
                unreachable->graph = block->graph;
                unreachable->opcode = op_unreachable;
                unreachable->instruction = call->instruction;
                unreachable->bytecodeIndex = call->bytecodeIndex;
                unreachable->block = otherwise;
                otherwise->nodes.append(unreachable);
            } else {
                otherwise->successors.append(continuation);
                continuation->predecessors.append(otherwise);
            }
            m_fallbackCalls.append(fallbackCall);
        }

        for (auto& [inlineeBlock, value] : returns) {
            inlineeBlock->nodes.removeLast();
            RELEASE_ASSERT(inlineeBlock->successors.isEmpty());
            inlineeBlock->successors.append(continuation);
            continuation->predecessors.append(inlineeBlock);
        }
        if (call->opcode == op_call_ignore_result) {
        } else if (returns.size() == 1 && (!fallbackCall || checksCalleeIsInitialized))
            call->replacement = returns[0].second;
        else {
            Node* phi = m_graph.addNode(NodeKind::Phi);
            phi->graph = block->graph;
            phi->block = continuation;
            phi->range = IntegerRange::unknown();
            if (fallbackCall && !checksCalleeIsInitialized)
                phi->uses.append({ VirtualRegister(), fallbackCall });
            for (auto& [inlineeBlock, value] : returns)
                phi->uses.append({ VirtualRegister(), value });
            continuation->phis.append(phi);
            call->replacement = phi;
        }
        if (block->isInLoop) {
            bool isForBuiltin = block->isInBuiltinLoopOnly || (caller.isInlinedBuiltin && !caller.wasCalledInLoop);
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                if (isForBuiltin && !inlineeBlock->isInLoop)
                    inlineeBlock->isInBuiltinLoopOnly = true;
                inlineeBlock->isInLoop = true;
                inlineeBlock->isInProfitableLoop |= block->isInProfitableLoop;
            }
        }
        inlinee->wasCalledInLoop = block->isInLoop && !block->isInBuiltinLoopOnly;
        if (!fallbackCall)
            m_calleesRead.append(calleeNode);
        if (caller.isOutermost())
            m_graph.callSites.append(call->bytecodeIndex.offset());

        Graph::InlineFrame frame { caller.inlineFrame(), CallSiteIndex(call->bytecodeIndex).bits(), m_graph.knownCalleeIndex(about->key), call->opcode == op_tail_call };
        m_graph.adoptInlinee(WTF::move(inlinee), frame);
        return true;
    }

    static std::optional<BuiltinCodeIndex> arraySpecializationOf(UniquedStringImpl* name, unsigned argc, bool usesResult)
    {
        StringView method { name };
        if (argc == 3)
            return method == "reduce"_s ? std::optional { BuiltinCodeIndex::arrayPrototypeReduceKnownArrayCode } : std::nullopt;
        if (argc != 2)
            return std::nullopt;
        if (method == "forEach"_s)
            return BuiltinCodeIndex::arrayPrototypeForEachKnownArrayCode;
        if (method == "map"_s)
            return usesResult ? BuiltinCodeIndex::arrayPrototypeMapKnownArrayCode : BuiltinCodeIndex::arrayPrototypeMapKnownArrayForEffectCode;
        if (method == "filter"_s)
            return usesResult ? BuiltinCodeIndex::arrayPrototypeFilterKnownArrayCode : BuiltinCodeIndex::arrayPrototypeFilterKnownArrayForEffectCode;
        if (method == "some"_s)
            return BuiltinCodeIndex::arrayPrototypeSomeKnownArrayCode;
        if (method == "every"_s)
            return BuiltinCodeIndex::arrayPrototypeEveryKnownArrayCode;
        return std::nullopt;
    }

    unsigned likelyArrayMethod(Node* call, Node* calleeNode, unsigned argc, unsigned argv)
    {
        if (!calleeNode->isBytecode(op_get_by_id) || argc < 2)
            return 0;
        const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
        if (!intrinsics) {
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: no builtin is made part of anything: the intrinsics are not known");
            return 0;
        }
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        auto bytecode = calleeNode->as<OpGetById>();
        if (resolve(call->use(VirtualRegister(firstArgument))) != resolve(calleeNode->use(bytecode.m_base)))
            return 0;
        if (Node* passed = resolve(call->use(VirtualRegister(firstArgument + 1))); !passed->isBytecode(op_new_func_exp)) {
            bool isExact = false;
            if (!passed->isBytecode(op_get_from_scope) || !passed->graph->knownFunctionReadBy(passed, &isExact) || !isExact)
                return 0;
        }
        static const unsigned prototype = [&] {
            unsigned array = intrinsics->find(ImmutableIntrinsics::globalObject, *String("Array"_s).impl());
            return array ? intrinsics->find(intrinsics->at(array).canonical, *String("prototype"_s).impl()) : 0;
        }();
        if (!prototype)
            return 0;
        unsigned method = intrinsics->find(intrinsics->at(prototype).canonical, *calleeNode->graph->codeBlock()->identifier(bytecode.m_property).impl());
        return method && intrinsics->at(method).builtinCode ? method : 0;
    }

    Vector<Graph*, 4> m_chain(Graph& graph)
    {
        Vector<Graph*, 4> result;
        for (Graph* at = &graph; at && at != &m_graph; at = m_parents.get(at))
            result.append(at);
        return result;
    }

    Graph& m_graph;
    const ProgramCode& m_program;
    UncheckedKeyHashMap<Graph*, Graph*> m_parents;
    Vector<Node*, 8> m_calleesRead;
    Vector<Node*, 4> m_fallbackCalls;
    unsigned m_inlinedBytecodeSize { 0 };
    bool m_didInline { false };
};

} // anonymous namespace

void inlineCalls(Graph& graph, const ProgramCode& program)
{
    if (!Options::useAOTInlining() || !PackedSite::fits(CallSiteIndex(BytecodeIndex(graph.codeBlock()->instructionsSize())).bits()))
        return;
    Inliner(graph, program).run();
}

bool mayBeAbsorbed(UnlinkedCodeBlock* codeBlock, const FunctionSummary* summary)
{
    return Options::useAOTInlining() && Inliner::isProfitable(codeBlock, summary) && Inliner::canBeInlinedIntoCaller(codeBlock);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
