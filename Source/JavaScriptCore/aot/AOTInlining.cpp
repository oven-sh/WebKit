/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

#include "AOTCompiler.h"
#include "AOTProgram.h"
#include "BuiltinExecutables.h"
#include "BytecodeStructs.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

// Replaces a call whose callee is proven with the body of the callee. See Graph::adoptInlinee().
//
// The block that contains the call is split at the call, and the first part jumps to the callee's entry. The rest becomes a new
// block, which the callee's returns jump to. A value of the caller that is used after the call was defined before it, so it still
// dominates its uses and needs no fixing up. The callee's parameters are replaced by the values that the call passed.
class Inliner {
public:
    Inliner(Graph& graph, const CodeOfProgram& program)
        : m_graph(graph)
        , m_program(program)
    {
    }

    void run()
    {
        // (The blocks of an inlinee are appended to the list, so the calls in them are visited too.)
        for (unsigned i = 0; i < m_graph.blocks.size(); ++i) {
            BasicBlock* block = m_graph.blocks[i].get();
            // (Calls on fallback and rarely executed paths are not inlined.)
            if (!block->isReachable || block->isGeneric || block->isRarelyExecuted)
                continue;
            for (unsigned index = 0; index < block->nodes.size(); ++index) {
                // (The rest of the block is now a separate block, which is visited later.)
                if (tryInline(block, index))
                    break;
            }
        }
        if (!m_didInline)
            return;

        UncheckedKeyHashSet<Node*> used;
        UncheckedKeyHashMap<Node*, Node*> onlyUser; // Null: more than one user.
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
        // Values that were read or created only to be called. (Graph::elideReadsOfCalleesNotPassed() looks at the users of a read,
        // and these have none left.)
        for (Node* read : m_calleesRead) {
            if (used.contains(read))
                continue;
            if (Graph::closedMethodReadBy(read))
                read->isReadOnlyToBeCalled = true;
            else
                read->isElided = true;
        }
        // A closure whose only user is a fallback call is created next to that call. (Creating a closure has no observable effect,
        // so it can be moved.)
        for (Node* call : m_fallbackCalls) {
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

    // isCalledInLoop: true if the call is in a loop, or may be.
    static bool isProfitable(UnlinkedCodeBlock* callee, const FunctionSummary* summary, bool isCalledInLoop = true)
    {
        unsigned size = callee->instructionsSize();
        // With a single call site, inlining does not duplicate the code.
        if (summary && summary->isNonEscaping && summary->directCalls.load(std::memory_order_relaxed) == 1)
            return size <= Options::maximumAOTInlineCandidateBytecodeCostForSingleCallSite();
        // Each additional copy grows the code, unless the body is no larger than the call. That is worth it where the call may run
        // repeatedly, which is the only available signal. (In one large program: 5.4 MB for bodies of 30 to 60 bytes of bytecode,
        // and 0.07 MB for bodies of up to 18.)
        return size <= (isCalledInLoop ? Options::maximumAOTInlineCandidateBytecodeCostInLoop() : std::min(Options::maximumAOTInlineCandidateBytecodeCost(), Options::maximumAOTInlineCandidateBytecodeCostInLoop()));
    }

    // Whether the callee can be inlined, judging by its bytecode alone.
    static bool canBeInlinedIntoCaller(UnlinkedCodeBlock* callee)
    {
        if (callee->codeType() != FunctionCode || callee->isConstructor() || callee->numberOfExceptionHandlers())
            return false;
        if (callee->parseMode() != SourceParseMode::NormalFunctionMode && callee->parseMode() != SourceParseMode::ArrowFunctionMode && callee->parseMode() != SourceParseMode::MethodMode)
            return false;
        // Identifier and constant numbers have to mean the same thing in the caller as in the callee.
        if (!numbersOfIdentifiersOfProgram() || !numbersOfConstantsOfProgramFor(callee))
            return false;
        for (const auto& instruction : callee->instructions()) {
            switch (instruction->opcodeID()) {
            // These depend on the callee having its own frame.
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
        // (The code after a tail call returns the call's result.)
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
        // The receiver's type says it is an array, and Array.prototype is immutable, so the callee does not have to be checked.
        bool calleeIsProvenIntrinsic = false;
        // The callee is a specialized version of the method that only works for arrays.
        bool isArraySpecialization = false;
        // With Options::verboseAOTCompilation(): logs why a builtin is not inlined.
        auto declineToInline = [&](ASCIILiteral why) {
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: a builtin is not made part of its caller at bc#", call->bytecodeIndex.offset(), ": ", why);
            return false;
        };
        if (calleeNode->isBytecode(op_new_func_exp)) {
            // The closure is created in this function, so its scope is available.
            auto bytecode = calleeNode->as<OpNewFuncExp>();
            callee = calleeNode->graph->codeBlock()->functionExpr(bytecode.m_functionDecl)->codeBlockIfExists(CodeSpecializationKind::CodeForCall);
            if (!callee || readsCallee(callee))
                return false;
            scopeOfClosure = resolve(calleeNode->use(bytecode.m_scope));
        } else if (unsigned intrinsic = likelyArrayMethod(call, calleeNode, argc, argv)) {
            callee = m_program.codeOfBuiltin(ImmutableIntrinsics::shared()->at(intrinsic).builtinCode - 1);
            if (!callee || readsCallee(callee))
                return declineToInline(callee ? "it reads its callee"_s : "there is no code for it"_s);
            intrinsicToCheckFor = ImmutableIntrinsics::shared()->at(intrinsic).canonical;
            if (auto lean = arraySpecializationOf(calleeNode->graph->codeBlock()->identifier(calleeNode->as<OpGetById>().m_property).impl(), argc, call->opcode != op_call_ignore_result)) {
                if (UnlinkedFunctionCodeBlock* code = m_program.codeOfBuiltin(static_cast<unsigned>(*lean)); code && !readsCallee(code)) {
                    callee = code;
                    isArraySpecialization = true;
                }
            }
            calleeIsProvenIntrinsic = Options::useAOTTypedFields() && TypeTable::typedFieldsAreEnforced() && TypeTable::shared()->isArray(Graph::typeTagOf(calleeNode));
        } else {
            bool isExact = false;
            const KnownFunction* known = caller.knownCallee(call, &isExact);
            if (!known || !isExact || !known->forCall || !(known->isDeclaration || Graph::closedMethodReadBy(calleeNode)) || !caller.passesNoFunctionObject(call))
                return false;
            callee = known->forCall;
        }
        auto about = m_program.about(callee);
        // (A closure that is called where it is created effectively has a single call site.)
        if (intrinsicToCheckFor && (!about || !canBeInlinedIntoCaller(callee)))
            return declineToInline(about ? "of what is in its bytecode"_s : "nothing is known about its code"_s);
        if (!about || !canBeInlinedIntoCaller(callee) || !(scopeOfClosure || intrinsicToCheckFor ? callee->instructionsSize() <= Options::maximumAOTInlineCandidateBytecodeCostForSingleCallSite() : isProfitable(callee, about->summary, block->isInLoop || m_graph.isCalledRepeatedly())))
            return intrinsicToCheckFor ? declineToInline("it is too big"_s) : false;
        if (m_inlinedBytecodeSize + callee->instructionsSize() > Options::maximumAOTInliningCallerBytecodeCost() || m_graph.inlineFrames.size() > PackedSite::mostInlineFrames)
            return intrinsicToCheckFor ? declineToInline("the caller has taken over enough"_s) : false;
        unsigned depth = 0;
        for (unsigned frame = caller.inlineFrame(); frame; frame = m_graph.inlineFrames[frame].parent)
            ++depth;
        if (depth >= deepest || m_graph.codeBlock() == callee)
            return intrinsicToCheckFor ? declineToInline("it is too deep"_s) : false;
        // No recursion. (An array method that is used in its own callback, as in a.some(x => x.b.some(...)), is not recursion: the depth
        // limit above bounds it.)
        if (!intrinsicToCheckFor) {
            for (Graph* graph : m_chain(caller)) {
                if (graph->codeBlock() == callee)
                    return false;
            }
        }

        auto inlinee = makeUniqueWithoutFastMallocCheck<Graph>(m_graph.vm(), callee, unknownScopeChain());
        inlinee->setCalleeHints(about->hints);
        inlinee->setVariableSummaries(m_graph.variableSummaries());
        inlinee->setLinkage(about->linkage, declaredNamesFor(callee));
        inlinee->loopsAreNotSplit = !!intrinsicToCheckFor || m_graph.loopsAreNotSplit;
        inlinee->isInlinedBuiltin = !!intrinsicToCheckFor;
        if (!scopeOfClosure && !intrinsicToCheckFor && (inlinee->needsFunctionObject() || !inlinee->scopeIsEnvironmentOfModule()))
            return false;
        if (!parseBytecode(*inlinee) || !inlinee->catchEntrypoints.isEmpty() || inlinee->hasFrameRegisters())
            return intrinsicToCheckFor ? declineToInline("its bytecode is not parsed, or it catches, or it has registers with homes"_s) : false;

        // The blocks that return, and the values they return.
        Vector<std::pair<BasicBlock*, Node*>, 4> returns;
        for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
            Node* terminal = inlineeBlock->terminal();
            if (!terminal || terminal->kind != NodeKind::Bytecode)
                continue;
            // (The code after a tail call returns the call's result, so in an inlinee it is an ordinary call.)
            if (terminal->opcode == op_ret)
                returns.append({ inlineeBlock, terminal->use(terminal->as<OpRet>().m_value) });
        }
        if (returns.isEmpty())
            return intrinsicToCheckFor ? declineToInline("it never returns"_s) : false;
        // (A builtin must not use the scope it was created in, because there is none here.)
        if (intrinsicToCheckFor) {
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                for (Node* node : inlineeBlock->nodes) {
                    for (auto& use : node->uses) {
                        if (use.node->isBytecode(op_get_scope))
                            return declineToInline("it uses its scope"_s);
                    }
                }
            }
        }

        // ---- From here on, inlining cannot fail.
        dataLogLnIf(Options::verboseAOTCompilation() && intrinsicToCheckFor, "AOT: a builtin is made part of its caller at bc#", call->bytecodeIndex.offset());
        m_didInline = true;
        m_inlinedBytecodeSize += callee->instructionsSize();
        m_parents.add(inlinee.get(), &caller);
        inlinee->isInTailPosition = call->opcode == op_tail_call && caller.isInTailPosition;

        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        BasicBlock* entry = inlinee->root;
        // The receiver, narrowed to an array. Either a guard has checked that or, if the type says it is an array and there is no
        // guard, the Narrow checks it.
        Node* receiverOfArraySpecialization = nullptr;
        if (isArraySpecialization) {
            inlinee->readsElementsOrEmpty = true;
            receiverOfArraySpecialization = m_graph.addNode(NodeKind::Narrow);
            receiverOfArraySpecialization->narrowedTo = TArray;
            receiverOfArraySpecialization->uses.append({ VirtualRegister(), call->use(VirtualRegister(firstArgument)) });
            if (calleeIsProvenIntrinsic) {
                receiverOfArraySpecialization->checksNarrowedType = true;
                receiverOfArraySpecialization->graph = call->graph;
                receiverOfArraySpecialization->opcode = call->opcode;
                receiverOfArraySpecialization->instruction = call->instruction;
                receiverOfArraySpecialization->bytecodeIndex = call->bytecodeIndex;
            } else {
                receiverOfArraySpecialization->graph = inlinee.get();
                receiverOfArraySpecialization->block = entry;
            }
            // After a test that an element is not empty, the element is known not to be empty.
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
        // (A read of the callee is replaced by the callee node. If there is none, nothing uses the read: it is only the copy bound
        // to the function's own name.)
        auto isReadOfCallee = [&](Node* node) {
            if (node->kind != NodeKind::Argument || node->reg != VirtualRegister(CallFrameSlot::callee))
                return false;
            node->replacement = calleeNode;
            return true;
        };
        entry->nodes.removeAllMatching([&](Node* node) {
            if (node->kind != NodeKind::Argument)
                return false;
            if (isReadOfCallee(node))
                return true;
            RELEASE_ASSERT(node->reg.isArgument());
            unsigned argument = node->reg.toArgument();
            node->replacement = !argument && receiverOfArraySpecialization ? receiverOfArraySpecialization : argument < argc ? call->use(VirtualRegister(firstArgument + static_cast<int>(argument))) : m_graph.constant(jsUndefined());
            return true;
        });
        if (receiverOfArraySpecialization && !calleeIsProvenIntrinsic)
            entry->nodes.insert(0, receiverOfArraySpecialization);
        for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
            inlineeBlock->nodes.removeAllMatching([&](Node* node) {
                if (isReadOfCallee(node))
                    return true;
                if (intrinsicToCheckFor && node->isBytecode(op_get_scope))
                    return true;
                // (So that the only remaining user of the closure is the call.)
                if (node->isBytecode(op_is_callable) && resolve(node->use(node->as<OpIsCallable>().m_operand))->isBytecode(op_new_func_exp)) {
                    node->replacement = m_graph.constant(jsBoolean(true));
                    return true;
                }
                // (op_argument_count does not count `this`.)
                if (node->isBytecode(op_argument_count)) {
                    node->replacement = m_graph.constant(jsNumber(argc - 1));
                    return true;
                }
                if (!node->isBytecode(op_get_argument))
                    return false;
                // (op_get_argument counts `this`.)
                unsigned argument = node->as<OpGetArgument>().m_index;
                node->replacement = argument < argc ? call->use(VirtualRegister(firstArgument + static_cast<int>(argument))) : m_graph.constant(jsUndefined());
                return true;
            });
        }

        if (scopeOfClosure) {
            inlinee->scopeOfClosure = scopeOfClosure;
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                for (Node* node : inlineeBlock->nodes) {
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
        if (receiverOfArraySpecialization && calleeIsProvenIntrinsic) {
            receiverOfArraySpecialization->block = block;
            block->nodes.append(receiverOfArraySpecialization);
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
        // If the callee turns out not to be the expected function, the call is made as a fallback.
        Node* fallbackCall = nullptr;
        if (intrinsicToCheckFor && !calleeIsProvenIntrinsic) {
            Node* guard = m_graph.addNode(NodeKind::Guard);
            guard->graph = block->graph;
            guard->guardKind = isArraySpecialization ? GuardKind::IsIntrinsicOfArray : GuardKind::IsIntrinsic;
            guard->intrinsic = intrinsicToCheckFor;
            guard->opcode = call->opcode;
            guard->instruction = call->instruction;
            guard->bytecodeIndex = call->bytecodeIndex;
            guard->block = block;
            guard->uses.append({ VirtualRegister(), calleeNode });
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
            otherwise->isRarelyExecuted = true;
            fallbackCall->block = otherwise;
            otherwise->nodes.append(fallbackCall);
            block->successors.append(otherwise);
            otherwise->predecessors.append(block);
            otherwise->successors.append(continuation);
            continuation->predecessors.append(otherwise);
            m_fallbackCalls.append(fallbackCall);
        }

        for (auto& [inlineeBlock, value] : returns) {
            inlineeBlock->nodes.removeLast();
            RELEASE_ASSERT(inlineeBlock->successors.isEmpty());
            inlineeBlock->successors.append(continuation);
            continuation->predecessors.append(inlineeBlock);
        }
        if (call->opcode == op_call_ignore_result) {
            // The result is unused, and the fallback call produces none.
        } else if (returns.size() == 1 && !fallbackCall)
            call->replacement = returns[0].second;
        else {
            Node* phi = m_graph.addNode(NodeKind::Phi);
            phi->graph = block->graph;
            phi->block = continuation;
            phi->range = IntegerRange::unknown();
            // (In the order of the predecessors.)
            if (fallbackCall)
                phi->uses.append({ VirtualRegister(), fallbackCall });
            for (auto& [inlineeBlock, value] : returns)
                phi->uses.append({ VirtualRegister(), value });
            continuation->phis.append(phi);
            call->replacement = phi;
        }
        if (block->isInLoop) {
            // (The loop belongs to an inlined builtin that was not itself called in a loop, or the caller is in a loop for the same
            // reason.)
            bool isForBuiltin = block->isOnlyInLoopOfBuiltin || (caller.isInlinedBuiltin && !caller.wasCalledInLoop);
            for (BasicBlock* inlineeBlock : inlinee->m_rpo) {
                if (isForBuiltin && !inlineeBlock->isInLoop)
                    inlineeBlock->isOnlyInLoopOfBuiltin = true;
                inlineeBlock->isInLoop = true;
            }
        }
        inlinee->wasCalledInLoop = block->isInLoop && !block->isOnlyInLoopOfBuiltin;
        if (!fallbackCall)
            m_calleesRead.append(calleeNode);
        // (A function that has been inlined records all of its sites.)
        if (caller.isOutermost())
            m_graph.callSites.append(call->bytecodeIndex.offset());

        Graph::InlineFrame frame { caller.inlineFrame(), CallSiteIndex(call->bytecodeIndex).bits(), m_graph.indexOfKnownCallee(about->key), call->opcode == op_tail_call };
        m_graph.adoptInlinee(WTF::move(inlinee), frame);
        return true;
    }

    // The name that a method is called by only suggests which code to inline. Whether the callee really is that method is checked
    // when the call runs. Inlining pays off when the argument is a closure created for the call, which then does not have to be
    // allocated.
    // Returns the version of an array method that is specialized for arrays with the realm's original structure and a single
    // function argument (builtins/ArrayPrototype.js). argc counts `this`.
    static std::optional<BuiltinCodeIndex> arraySpecializationOf(UniquedStringImpl* name, unsigned argc, bool usesResult)
    {
        StringView method { name };
        if (argc == 3)
            return method == "reduce"_s ? std::optional { BuiltinCodeIndex::arrayPrototypeReduceOfArrayCode } : std::nullopt;
        if (argc != 2)
            return std::nullopt;
        if (method == "forEach"_s)
            return BuiltinCodeIndex::arrayPrototypeForEachOfArrayCode;
        // (If the result is unused, the array is not created.)
        if (method == "map"_s)
            return usesResult ? BuiltinCodeIndex::arrayPrototypeMapOfArrayCode : BuiltinCodeIndex::arrayPrototypeMapOfArrayForEffectCode;
        if (method == "filter"_s)
            return usesResult ? BuiltinCodeIndex::arrayPrototypeFilterOfArrayCode : BuiltinCodeIndex::arrayPrototypeFilterOfArrayForEffectCode;
        if (method == "some"_s)
            return BuiltinCodeIndex::arrayPrototypeSomeOfArrayCode;
        if (method == "every"_s)
            return BuiltinCodeIndex::arrayPrototypeEveryOfArrayCode;
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
        // The argument is a closure created for the call or a known function. In both cases the method's callback is known.
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

    // The graph and its chain of callers, excluding the outermost graph.
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
    Vector<Node*, 4> m_fallbackCalls;
    unsigned m_inlinedBytecodeSize { 0 };
    bool m_didInline { false };
};

} // anonymous namespace

void inlineCalls(Graph& graph, const CodeOfProgram& program)
{
    // (A site must have room to record which inlined call it belongs to.)
    if (!Options::useAOTInlining() || !PackedSite::fits(CallSiteIndex(BytecodeIndex(graph.codeBlock()->instructionsSize())).bits()))
        return;
    Inliner(graph, program).run();
}

bool mayBecomePartOfAnother(UnlinkedCodeBlock* codeBlock, const FunctionSummary* summary)
{
    return Options::useAOTInlining() && Inliner::isProfitable(codeBlock, summary) && Inliner::canBeInlinedIntoCaller(codeBlock);
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
