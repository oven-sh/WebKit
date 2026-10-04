/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(AOT)

#include "AOTBuiltins.h"
#include "AOTOpcodeTraits.h"
#include "AOTTypeTable.h"
#include "BytecodeStructs.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

constexpr Type TNumberLike = TNumber | TBoolean | TOther;
constexpr Type TMayBeFalsy = TAll & ~(TSymbol | (TAnyObject & ~(TOtherObject | TFunction)));
constexpr Type TBuiltinsThatMayBeExtended = TArray | TPromise | TRegExp | TMap | TSet;

Type builtinConstructedBy(const Node* constructor)
{
    auto made = constructingIntrinsicResult(constructor->intrinsic);
    if (!made)
        return TNone;
    for (Type builtin : { TArray, TPromise, TRegExp, TMap, TSet }) {
        if (isSubtype(*made, builtin))
            return builtin;
    }
    return TNone;
}

Type builtinsBehind(const Node* constructor)
{
    if (constructor->kind == NodeKind::Intrinsic)
        return builtinConstructedBy(constructor);
    if (constructor->kind != NodeKind::Bytecode)
        return TBuiltinsThatMayBeExtended;
    switch (constructor->opcode) {
    case op_new_func:
    case op_new_func_exp:
        return TNone;
    case op_get_from_scope: {
        bool isExact = false;
        return constructor->graph->knownFunctionReadBy(constructor, &isExact) && isExact ? TNone : TBuiltinsThatMayBeExtended;
    }
    default:
        return TBuiltinsThatMayBeExtended;
    }
}

Type builtinsExtendedBy(const Node* user)
{
    if (user->isBytecode(op_call) || user->isBytecode(op_call_ignore_result) || user->isBytecode(op_tail_call)) {
        auto operands = Graph::callOperands(user->instruction);
        Node* callee = user->use(operands.callee);
        if (operands.argc == 2 && Graph::linkTimeConstantOf(callee) == LinkTimeConstant::setPrototypeDirectOrThrow)
            return user->use(operands.argument(0))->isBytecode(op_new_func_exp) ? builtinsBehind(user->use(operands.argument(1))) : TNone;
        if (callee->kind == NodeKind::Intrinsic && isReflectConstruct(callee->intrinsic)) {
            for (auto& use : user->uses) {
                if (use.node == callee && use.reg != operands.callee)
                    return TBuiltinsThatMayBeExtended;
            }
            if (operands.argc < 4 || user->use(operands.argument(1)) == user->use(operands.argument(3)))
                return TNone;
            return builtinsBehind(user->use(operands.argument(1)));
        }
    }
    if (user->isBytecode(op_construct_varargs)) {
        auto bytecode = user->as<OpConstructVarargs>();
        Node* callee = user->use(bytecode.m_callee);
        if (bytecode.m_thisValue.isValid() && user->use(bytecode.m_thisValue) != callee)
            return builtinsBehind(callee);
    }
    if (user->kind == NodeKind::Guard || user->isBytecode(op_jneq_ptr) || user->isBytecode(op_jeq_ptr))
        return TNone;
    for (auto& use : user->uses) {
        if (use.node->kind == NodeKind::Intrinsic && isReflectConstruct(use.node->intrinsic))
            return TBuiltinsThatMayBeExtended;
    }
    return TNone;
}

template<typename Functor>
void forEachBuiltinExtendedIn(Graph& graph, const Functor& functor)
{
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* phi : block->phis) {
            if (Type builtins = builtinsExtendedBy(phi))
                functor(builtins);
        }
        for (Node* node : block->nodes) {
            if (Type builtins = builtinsExtendedBy(node))
                functor(builtins);
        }
    }
}

class TypeInference {
public:
    TypeInference(Graph& graph)
        : m_graph(graph)
    {
    }

    static bool isNeverGivenEmptyValues(const Node* user)
    {
        if (user->kind != NodeKind::Bytecode)
            return false;
        switch (user->opcode) {
        case op_add:
        case op_sub:
        case op_mul:
        case op_div:
        case op_mod:
        case op_pow:
        case op_bitand:
        case op_bitor:
        case op_bitxor:
        case op_bitnot:
        case op_lshift:
        case op_rshift:
        case op_urshift:
        case op_inc:
        case op_dec:
        case op_negate:
        case op_to_number:
        case op_to_numeric:
        case op_less:
        case op_lesseq:
        case op_greater:
        case op_greatereq:
        case op_jless:
        case op_jlesseq:
        case op_jgreater:
        case op_jgreatereq:
        case op_jnless:
        case op_jnlesseq:
        case op_jngreater:
        case op_jngreatereq:
        case op_stricteq:
        case op_nstricteq:
        case op_jstricteq:
        case op_jnstricteq:
        case op_eq:
        case op_neq:
        case op_jeq:
        case op_jneq:
        case op_not:
        case op_jtrue:
        case op_jfalse:
        case op_typeof:
        case op_get_by_id:
        case op_get_by_val:
        case op_put_by_id:
        case op_put_by_val:
        case op_call:
        case op_call_ignore_result:
        case op_tail_call:
        case op_construct:
            return true;
        default:
            return false;
        }
    }

    void findVariablesThatAreNotEmptyWhenRead()
    {
        UncheckedKeyHashSet<Node*> mayBeEmpty;
        auto visit = [&](Node* user) {
            if (isNeverGivenEmptyValues(user))
                return;
            for (auto& use : user->uses) {
                if (use.node->isBytecode(op_get_from_scope))
                    mayBeEmpty.add(use.node);
            }
        };
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis)
                visit(phi);
            for (Node* node : block->nodes)
                visit(node);
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->isBytecode(op_get_from_scope))
                    node->isNeverEmpty = !mayBeEmpty.contains(node);
            }
        }
    }

    void run()
    {
        remarkOnEscape();
        if (!calleesWithWidenedInputs && (Options::aotRemarksPath() || Options::aotTypeCoveragePath())) [[unlikely]] {
            forEachBuiltinExtendedIn(m_graph, [&](Type builtins) {
                m_graph.remark("extends-builtin"_s, builtins == TBuiltinsThatMayBeExtended ? "unknown"_s : builtins == TArray ? "Array"_s : builtins == TPromise ? "Promise"_s : builtins == TRegExp ? "RegExp"_s : builtins == TMap ? "Map"_s : "Set"_s);
            });
        }
        findVariablesThatAreNotEmptyWhenRead();
        iterateToFixpoint();
        constexpr unsigned maxRounds = 4;
        for (unsigned round = 0; round < maxRounds && narrowTestedValues(); ++round) {
            forgetTypes();
            iterateToFixpoint();
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->kind != NodeKind::Bytecode)
                    continue;
                if (!block->isExecutable) {
                    if (calleesWithWidenedInputs && isReached())
                        recordWhetherReturnObjectIsNeededBy(node);
                    continue;
                }
                switch (node->opcode) {
                case op_ret:
                    m_returnType |= node->use(node->as<OpRet>().m_value)->type;
                    recordReturnValueTypesOf(node);
                    break;
                case op_tail_call:
                    m_returnType |= callResult(node);
                    noteTailCall(node);
                    break;
                case op_tail_call_varargs:
                    m_returnType |= TTop;
                    break;
                default:
                    RELEASE_ASSERT(!(traitsOf(node->opcode) & OpcodeTraits::Returns));
                    break;
                }
                if (calleesWithWidenedInputs && isReached()) {
                    noteArgumentsOf(node);
                    noteStoresToVariables(node);
                    recordWhetherReturnObjectIsNeededBy(node);
                } else if (!calleesWithWidenedInputs && Options::validateAOTInferredTypes()) [[unlikely]]
                    verifyAgainstSummaries(node);
            }
        }
        if (calleesWithWidenedInputs && m_graph.summary())
            m_graph.summary()->knownTailCallees = WTF::move(m_knownTailCallees);
        if (programFunctions() && isReached()) {
            for (BasicBlock* block : m_graph.m_rpo) {
                if (!block->isExecutable)
                    continue;
                for (Node* phi : block->phis)
                    noteEscapesThrough(phi);
                for (Node* node : block->nodes)
                    noteEscapesThrough(node);
            }
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis) {
                if (!phi->type)
                    phi->type = TAll;
            }
            for (Node* node : block->nodes) {
                if (!node->type) {
                    node->type = TAll;
                    node->wasInferredUnreachable = true;
                }
            }
        }
    }

    static bool mayGoTo(const BasicBlock* from, unsigned successorIndex)
    {
        if (from->isGeneric || from->endsWithGuard || from->isReentry || from->isPreHeader || from->successors.size() != 2 || from->successors[0] == from->successors[1])
            return true;
        Node* terminal = from->terminal();
        if (!terminal || terminal->kind != NodeKind::Bytecode || terminal->guard || terminal->guarded)
            return true;
        TestedValue tested = valueTestedBy(terminal);
        if (!tested.value || tested.value->isElided)
            return true;
        return mayBe(tested.value->type, successorIndex ? tested.ifFalse : tested.ifTrue);
    }

    static bool mayComeFrom(const BasicBlock* to, const BasicBlock* from)
    {
        if (!from->isExecutable)
            return false;
        for (unsigned i = 0; i < from->successors.size(); ++i) {
            if (from->successors[i] == to && mayGoTo(from, i))
                return true;
        }
        return false;
    }

    void iterateToFixpoint()
    {
        m_graph.root->isExecutable = true;
        for (BasicBlock* entrypoint : m_graph.catchEntrypoints)
            entrypoint->isExecutable = true;
        bool changed = true;
        while (changed) {
            changed = false;
            for (BasicBlock* block : m_graph.m_rpo) {
                if (!block->isExecutable)
                    continue;
                for (Node* phi : block->phis)
                    changed |= update(phi);
                for (Node* node : block->nodes)
                    changed |= update(node);
                for (unsigned i = 0; i < block->successors.size(); ++i) {
                    BasicBlock* successor = block->successors[i];
                    if (successor->isExecutable || !mayGoTo(block, i))
                        continue;
                    successor->isExecutable = true;
                    changed = true;
                }
            }
            changed |= std::exchange(m_elementTypesChanged, false);
            if (!changed && !std::exchange(m_treatsEmptyArraysAsUntyped, true))
                changed = true;
        }
    }

    void forgetTypes()
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            block->isExecutable = false;
            for (Node* phi : block->phis)
                phi->type = TNone;
            for (Node* node : block->nodes) {
                switch (node->kind) {
                case NodeKind::Constant:
                case NodeKind::ConstantCell:
                case NodeKind::Intrinsic:
                case NodeKind::LinkTimeConstant:
                case NodeKind::Argument:
                    break;
                default:
                    node->type = TNone;
                    break;
                }
            }
        }
        m_graph.frameRegisterTypes.fill(TNone);
        m_elementTypes.clear();
        m_elementTypesChanged = false;
        m_treatsEmptyArraysAsUntyped = false;
        m_users.reset();
        m_aliasesOfObjectsKeptToThemselves.clear();
    }

    struct TestedValue {
        Node* value { nullptr };
        Type ifTrue { TAll };
        Type ifFalse { TAll };
        TestedValue inverted() const { return { value, ifFalse, ifTrue }; }
    };

    static TestedValue valueTestedByCondition(Node* condition, unsigned depth = 0)
    {
        if (condition->kind == NodeKind::Intrinsic && isSubtype(condition->type, TAnyObject))
            return { condition, TAll, TNone };
        if (condition->kind != NodeKind::Bytecode || condition->guard || condition->guarded)
            return { condition, TAll & ~TOther, TMayBeFalsy };
        constexpr Type notObject = TPrimitive | TEmpty;
        constexpr Type neverCallable = TAnyObject & ~(TFunction | TOtherObject);
        switch (condition->opcode) {
        case op_not:
            if (depth < 4)
                return valueTestedByCondition(condition->use(condition->as<OpNot>().m_operand), depth + 1).inverted();
            return { };
        case op_is_undefined_or_null:
            return { condition->use(condition->as<OpIsUndefinedOrNull>().m_operand), TOther, TAll & ~TOther };
        case op_is_number:
            return { condition->use(condition->as<OpIsNumber>().m_operand), TNumber, TAll & ~TNumber };
        case op_is_boolean:
            return { condition->use(condition->as<OpIsBoolean>().m_operand), TBoolean, TAll & ~TBoolean };
        case op_is_object:
            return { condition->use(condition->as<OpIsObject>().m_operand), TAnyObject, TAll & ~TAnyObject };
        case op_is_cell_with_type: {
            auto bytecode = condition->as<OpIsCellWithType>();
            Type type = bytecode.m_type == StringType ? TString : bytecode.m_type == SymbolType ? TSymbol : TNone;
            if (!type)
                return { };
            return { condition->use(bytecode.m_operand), type, TAll & ~type };
        }
        case op_typeof_is_undefined: {
            Node* operand = condition->use(condition->as<OpTypeofIsUndefined>().m_operand);
            return { operand, operand->kind == NodeKind::Intrinsic ? TNone : TAll, TAll & ~TUndefined };
        }
        case op_typeof_is_object:
            return { condition->use(condition->as<OpTypeofIsObject>().m_operand), TAll & ~(notObject & ~TNull), TAll & ~(TNull | neverCallable) };
        case op_typeof_is_function:
            return { condition->use(condition->as<OpTypeofIsFunction>().m_operand), TAll & ~(notObject | neverCallable), TAll & ~TFunction };
        case op_call: {
            auto bytecode = condition->as<OpCall>();
            Node* callee = condition->use(bytecode.m_callee);
            unsigned called = callee->kind == NodeKind::Intrinsic ? callee->intrinsic : intrinsicFunctionOf(callee->type);
            if (called && bytecode.m_argc >= 2) {
                Node* argument = condition->use(VirtualRegister(-static_cast<int>(bytecode.m_argv) + CallFrame::thisArgumentOffset() + 1));
                switch (builtinAtIndex(called)) {
                case Builtin::ArrayIsArray:
                    return { argument, TArray | TOtherObject, TAll & ~TArray };
                case Builtin::NumberIsInteger:
                case Builtin::NumberIsSafeInteger:
                case Builtin::NumberIsFinite:
                    return { argument, TNumber, TAll & ~TInt32 };
                case Builtin::NumberIsNaN:
                    return { argument, TDouble, TAll };
                default:
                    break;
                }
            }
            return { condition, TAll & ~TOther, TMayBeFalsy };
        }
        default:
            return { condition, TAll & ~TOther, TMayBeFalsy };
        }
    }

    static TestedValue valueTestedBy(Node* terminal)
    {
        constexpr Type mayEqualNull = TOther | TOtherObject | TFunction;
        auto comparedWithConstant = [&](VirtualRegister left, VirtualRegister right) -> TestedValue {
            Node* operands[2] = { terminal->use(left), terminal->use(right) };
            for (unsigned i = 0; i < 2; ++i) {
                Node* constant = operands[i];
                if (constant->kind != NodeKind::Constant || !constant->constant || !constant->constant.isUndefinedOrNull())
                    continue;
                Type type = constant->constant.isUndefined() ? TUndefined : TNull;
                return { operands[1 - i], type, TAll & ~type };
            }
            return { };
        };
        switch (terminal->opcode) {
        case op_jstricteq:
            return comparedWithConstant(terminal->as<OpJstricteq>().m_lhs, terminal->as<OpJstricteq>().m_rhs);
        case op_jnstricteq:
            return comparedWithConstant(terminal->as<OpJnstricteq>().m_lhs, terminal->as<OpJnstricteq>().m_rhs).inverted();
        case op_jundefined_or_null:
            return { terminal->use(terminal->as<OpJundefinedOrNull>().m_value), TOther, TAll & ~TOther };
        case op_jnundefined_or_null:
            return { terminal->use(terminal->as<OpJnundefinedOrNull>().m_value), TAll & ~TOther, TOther };
        case op_jeq_null: {
            Node* value = terminal->use(terminal->as<OpJeqNull>().m_value);
            return { value, value->kind == NodeKind::Intrinsic ? TNone : mayEqualNull, TAll & ~TOther };
        }
        case op_jneq_null: {
            Node* value = terminal->use(terminal->as<OpJneqNull>().m_value);
            return { value, TAll & ~TOther, value->kind == NodeKind::Intrinsic ? TNone : mayEqualNull };
        }
        case op_jneq_ptr: {
            auto bytecode = terminal->as<OpJneqPtr>();
            Node* value = terminal->use(bytecode.m_value);
            auto pointer = Graph::linkTimeConstantOf(terminal->use(bytecode.m_specialPointer));
            if (!pointer || (*pointer != LinkTimeConstant::callFunction && *pointer != LinkTimeConstant::applyFunction))
                return { };
            bool isThatFunction = isSubtype(value->type, TFunction) && intrinsicFunctionOf(value->type) && intrinsicFunctionOf(value->type) == intrinsicBehindCallOrApplyFunction(*pointer == LinkTimeConstant::applyFunction);
            return { value, isThatFunction ? TNone : TAll, TAll };
        }
        case op_jtrue:
            return valueTestedByCondition(terminal->use(terminal->as<OpJtrue>().m_condition));
        case op_jfalse:
            return valueTestedByCondition(terminal->use(terminal->as<OpJfalse>().m_condition)).inverted();
        default:
            return { };
        }
    }

    static bool isWorthNarrowing(Type before, Type after)
    {
        if (after == before)
            return false;
        if (!after)
            return true;
        if (repForType(after) != repForType(before) || (isSubtype(after, TCell) && !isSubtype(before, TCell)) || !mayBe(after, TCell))
            return true;
        return mayBe(after, TOther) && isSubtype(before & ~after, TOther) && isWorthNarrowing(before, before & ~TOther);
    }

    bool narrowOnEdge(BasicBlock* from, unsigned successorIndex, Node* value, Type narrowedTo, bool& addedBlock)
    {
        BasicBlock* to = from->successors[successorIndex];
        if (to->isGeneric || to->isCatchEntrypoint || to->isReentry || to->isPreHeader)
            return false;
        Node* narrow = nullptr;
        auto narrowed = [&] {
            if (!narrow) {
                narrow = m_graph.addNode(NodeKind::Narrow);
                narrow->graph = from->graph;
                narrow->narrowedTo = narrowedTo;
                narrow->bytecodeIndex = from->terminal()->bytecodeIndex;
                narrow->uses.append({ VirtualRegister(), value });
            }
            return narrow;
        };
        if (to->predecessors.size() == 1) {
            for (BasicBlock* block : m_graph.m_rpo) {
                for (Node* phi : block->phis) {
                    for (unsigned i = 0; i < phi->uses.size(); ++i) {
                        if (phi->uses[i].node == value && to->dominates(block->predecessors[i]))
                            phi->uses[i].node = narrowed();
                    }
                }
                if (!to->dominates(block))
                    continue;
                for (Node* node : block->nodes) {
                    if (node->kind == NodeKind::Narrow && node->narrowedTo == narrowedTo)
                        continue;
                    for (auto& use : node->uses) {
                        if (use.node == value)
                            use.node = narrowed();
                    }
                }
            }
            if (!narrow)
                return false;
            narrow->block = to;
            to->nodes.insert(0, narrow);
            return true;
        }

        if (to->isLoopHeader)
            return false;
        size_t index = to->predecessors.find(from);
        RELEASE_ASSERT(index != notFound);
        for (Node* phi : to->phis) {
            if (phi->uses[index].node == value)
                phi->uses[index].node = narrowed();
        }
        if (!narrow)
            return false;
        BasicBlock* edge = m_graph.addBlock();
        edge->graph = from->graph;
        edge->bytecodeBegin = to->bytecodeBegin;
        edge->bytecodeEnd = to->bytecodeBegin;
        edge->isReachable = true;
        edge->isInLoop = from->isInLoop && to->isInLoop;
        edge->isInProfitableLoop = from->isInProfitableLoop && to->isInProfitableLoop;
        edge->isInBuiltinLoopOnly = from->isInBuiltinLoopOnly && to->isInBuiltinLoopOnly;
        edge->isRarelyExecuted = from->isRarelyExecuted || to->isRarelyExecuted;
        narrow->block = edge;
        edge->nodes.append(narrow);
        edge->predecessors.append(from);
        edge->successors.append(to);
        from->successors[successorIndex] = edge;
        to->predecessors[index] = edge;
        addedBlock = true;
        return true;
    }

    static bool isCalleeOf(const Node* user, VirtualRegister reg)
    {
        if ((user->kind != NodeKind::Bytecode && user->kind != NodeKind::Guard) || !user->instruction)
            return false;
        switch (user->opcode) {
        case op_call:
            return reg == user->as<OpCall>().m_callee;
        case op_call_ignore_result:
            return reg == user->as<OpCallIgnoreResult>().m_callee;
        case op_tail_call:
            return reg == user->as<OpTailCall>().m_callee;
        case op_construct:
            return reg == user->as<OpConstruct>().m_callee;
        default:
            return false;
        }
    }

    bool narrowAfterCheck(BasicBlock* block, unsigned index, Node* value)
    {
        Node* check = block->nodes[index];
        Node* narrow = nullptr;
        auto narrowed = [&] {
            if (!narrow) {
                narrow = m_graph.addNode(NodeKind::Narrow);
                narrow->graph = check->graph;
                narrow->narrowedTo = TAll & ~TEmpty;
                narrow->bytecodeIndex = check->bytecodeIndex;
                narrow->uses.append({ VirtualRegister(), value });
            }
            return narrow;
        };
        auto narrowUsesOf = [&](Node* user) {
            if (user->kind == NodeKind::Narrow && user->narrowedTo && !mayBe(user->narrowedTo, TEmpty))
                return;
            for (auto& use : user->uses) {
                if (use.node == value && !isCalleeOf(user, use.reg))
                    use.node = narrowed();
            }
        };
        for (BasicBlock* other : m_graph.m_rpo) {
            for (Node* phi : other->phis) {
                for (unsigned i = 0; i < phi->uses.size(); ++i) {
                    if (phi->uses[i].node == value && block->dominates(other->predecessors[i]))
                        phi->uses[i].node = narrowed();
                }
            }
            if (other == block || !block->dominates(other))
                continue;
            for (Node* node : other->nodes)
                narrowUsesOf(node);
        }
        for (unsigned i = index + 1; i < block->nodes.size(); ++i)
            narrowUsesOf(block->nodes[i]);
        if (!narrow)
            return false;
        narrow->block = block;
        block->nodes.insert(index + 1, narrow);
        return true;
    }

    bool narrowTestedValues()
    {
        bool hasDominators = false;
        bool addedBlock = false;
        bool changed = false;
        Vector<BasicBlock*> blocks = m_graph.m_rpo;
        for (BasicBlock* block : blocks) {
            if (block->isGeneric || block->isReentry || block->isPreHeader || !block->isExecutable)
                continue;
            for (unsigned index = 0; index < block->nodes.size(); ++index) {
                Node* check = block->nodes[index];
                if (!check->isBytecode(op_check_tdz) || check->guard || check->guarded || check->isElided)
                    continue;
                Node* value = check->uses[0].node;
                if (value->isElided || !isWorthNarrowing(value->type, value->type & ~TEmpty))
                    continue;
                if (!std::exchange(hasDominators, true))
                    m_graph.computeDominators();
                if (narrowAfterCheck(block, index, value)) {
                    m_graph.remark("narrowed-after-tdz-check"_s);
                    changed = true;
                }
            }
        }
        for (BasicBlock* block : blocks) {
            if (block->isGeneric || block->endsWithGuard || block->isReentry || block->isPreHeader || !block->isExecutable || block->successors.size() != 2 || block->successors[0] == block->successors[1])
                continue;
            Node* terminal = block->terminal();
            if (!terminal || terminal->kind != NodeKind::Bytecode || terminal->guard || terminal->guarded)
                continue;
            TestedValue tested = valueTestedBy(terminal);
            if (!tested.value || tested.value->isElided)
                continue;
            for (unsigned i = 0; i < 2; ++i) {
                Type narrowedTo = i ? tested.ifFalse : tested.ifTrue;
                if (!mayBe(tested.value->type, narrowedTo) || !isWorthNarrowing(tested.value->type, tested.value->type & narrowedTo))
                    continue;
                if (!std::exchange(hasDominators, true))
                    m_graph.computeDominators();
                if (narrowOnEdge(block, i, tested.value, narrowedTo, addedBlock)) {
                    m_graph.remark(narrowedTo == TMayBeFalsy ? "narrowed-falsy-value"_s : "narrowed-tested-value"_s);
                    changed = true;
                }
            }
        }
        if (addedBlock)
            m_graph.computeBlockOrder();
        return changed;
    }

    bool isReached() const { return !m_graph.summary() || m_graph.summary()->isReached(); }
    Type returnType() const { return m_returnType; }
    Vector<const KnownFunction*>* calleesRead { nullptr };
    Vector<const KnownFunction*>* calleesWithWidenedInputs { nullptr };
    std::optional<NodeUsers> m_users;

private:
    Vector<const FunctionSummary*> m_knownTailCallees;

    static String describe(FunctionSummary::EscapeCause cause)
    {
        auto withName = [&](ASCIILiteral text) -> String {
            return cause.name ? makeString(text, ':', StringView(cause.name)) : String(text);
        };
        switch (cause.why & 0xff) {
        case FunctionSummary::ReportedByBundler:
            return "reported-by-bundler"_s;
        case FunctionSummary::CreationSiteUnknown:
            return "creation-site-unknown"_s;
        case FunctionSummary::ReferencesItself:
            return "references-itself"_s;
        case FunctionSummary::ExternalFunction:
            return "external-function"_s;
        case FunctionSummary::NotCallable:
            return "not-callable"_s;
        case FunctionSummary::FunctionNumberOverflow:
            return "function-number-overflow"_s;
        case FunctionSummary::UsedBy:
            if (unsigned user = cause.why >> 8; user < static_cast<unsigned>(numOpcodeIDs))
                return makeString("used-by:"_s, opcodeNames[user]);
            return "used-by-node"_s;
        case FunctionSummary::PassedToUnknownCallee:
            return "passed-to-unknown-call"_s;
        case FunctionSummary::PassedAsThis:
            return "passed-as-this"_s;
        case FunctionSummary::PassedAsExtraArgument:
            return "passed-as-extra-argument"_s;
        case FunctionSummary::CalledIndirectly:
            return "called-indirectly"_s;
        case FunctionSummary::ReturnedToUnknownCaller:
            return "returned"_s;
        case FunctionSummary::MergedInPhi:
            return "merged-in-phi"_s;
        case FunctionSummary::MergedInFrameRegister:
            return "merged-in-frame-register"_s;
        case FunctionSummary::MergedInVariable:
            return "merged-in-variable"_s;
        case FunctionSummary::MergedInParameter:
            return "merged-in-parameter"_s;
        case FunctionSummary::MergedInReturn:
            return "merged-in-return"_s;
        case FunctionSummary::LostThroughAlias:
            return "lost-through-alias"_s;
        case FunctionSummary::StoredInModuleVariable:
            return "stored-in-module-variable"_s;
        case FunctionSummary::StoredInUntrackedVariable:
            return "stored-in-untracked-variable"_s;
        case FunctionSummary::StoredToUnknownLocation:
            return "stored-to-unknown-location"_s;
        case FunctionSummary::StoredInDynamicallyReadVariable:
            return "stored-in-dynamically-read-variable"_s;
        case FunctionSummary::ReadInexactly:
            return "read-inexactly"_s;
        case FunctionSummary::PropertyRead:
            return withName("property-read"_s);
        case FunctionSummary::PropertyWritten:
            return withName("property-written"_s);
        case FunctionSummary::PrototypeRead:
            return "prototype"_s;
        case FunctionSummary::LeftOfInstanceof:
            return "instanceof"_s;
        case FunctionSummary::RightOfInstanceof:
            return "right-of-instanceof"_s;
        case FunctionSummary::StoredInProperty:
            return "stored-in-property"_s;
        case FunctionSummary::Constructed:
            return "constructed"_s;
        default:
            return "unknown"_s;
        }
    }

    void remarkOnEscape()
    {
        const FunctionSummary* body = m_graph.summary();
        if (calleesWithWidenedInputs || !body)
            return;
        const FunctionSummary* summary = &body->ofFunction();
        if (summary->isNonEscaping) {
            m_graph.remark("function-does-not-escape"_s);
            if (summary->takesScopeAsCallee)
                m_graph.remark("function-has-no-object"_s);
            else if (summary->objectIsNeededAfterAll)
                m_graph.remark("function-object-is-needed-after-all"_s);
            return;
        }
        if (body->function)
            m_graph.remark("is-general-body"_s);
        else if (body->hasOnlyKnownCallers) {
            m_graph.remark("is-typed-body"_s);
            return;
        }
        if (!Options::aotRemarksPath() && !Options::aotTypeCoveragePath()) [[likely]]
            return;
        Vector<FunctionSummary::EscapeCause, 4> causes;
        {
            Locker locker { summary->escapeCausesLock };
            causes.appendVector(summary->escapeCauses);
        }
        uint32_t first = summary->escapeReason.load(std::memory_order_relaxed);
        if (!causes.containsIf([&](auto& cause) { return cause.why == first; }))
            causes.append({ first, nullptr });
        Vector<String, 4> descriptions;
        bool isOnlyThroughProperties = true;
        for (auto& cause : causes) {
            isOnlyThroughProperties &= cause.why == FunctionSummary::PropertyRead || cause.why == FunctionSummary::PropertyWritten;
            if (String description = describe(cause); !descriptions.contains(description))
                descriptions.append(WTF::move(description));
        }
        std::ranges::sort(descriptions, [](const String& a, const String& b) { return codePointCompareLessThan(a, b); });
        for (auto& description : descriptions)
            m_graph.remark("function-escapes"_s, description);
        if (isOnlyThroughProperties)
            m_graph.remark("function-escapes-only-through-properties"_s);
        if (!summary->typesPassedByDirectCalls[0].load()) {
            m_graph.remark("open-function-has-no-direct-call"_s);
            return;
        }
        bool passesUnboxedValues = false;
        bool passesKnownKinds = false;
        for (unsigned i = 1; i < std::min<unsigned>(m_graph.codeBlock()->numParameters(), FunctionSummary::maxParameters); ++i) {
            Type type = summary->typesPassedByDirectCalls[i].load();
            if (!type)
                continue;
            passesUnboxedValues |= isSubtype(type, TNumber) || isSubtype(type, TBoolean);
            passesKnownKinds |= isSubtype(type, TString | TOther) || isSubtype(type, TAnyObject | TOther);
        }
        m_graph.remark(passesUnboxedValues ? "direct-calls-of-open-function-pass-unboxed-values"_s : passesKnownKinds ? "direct-calls-of-open-function-pass-known-kinds"_s : "direct-calls-of-open-function-pass-anything"_s);
    }

    void noteNeedsObject(Type type)
    {
        const KnownFunction* function = programFunctions()->function(functionNumberOf(type));
        if (!function || !function->summary)
            return;
        bool wasKnown = function->summary->needsObject.exchange(true, std::memory_order_relaxed);
        if (!wasKnown && function->summary->takesScopeAsCallee && Options::validateAOTInferredTypes()) [[unlikely]]
            dataLogLn("AOT: contradicts the analysis: the function object of `", function->executable ? function->executable->ecmaName().string() : String(), "` @", function->key.module, ":", function->key.start, " is needed after all");
    }

    bool markEscaping(Type type, uint32_t why, UniquedStringImpl* name = nullptr)
    {
        const KnownFunction* function = programFunctions()->function(functionNumberOf(type));
        if (!function || !function->summary)
            return false;
        if (!calleesWithWidenedInputs) {
            noteNeedsObject(type);
            return false;
        }
        if (Options::aotRemarksPath() || Options::aotTypeCoveragePath()) [[unlikely]]
            function->summary->noteEscapeCause({ why, name });
        Vector<Type, FunctionSummary::maxParameters + 1> wasPassed;
        if (!function->summary->markEscaping(why, [&](Type passed) { wasPassed.append(passed); }))
            return false;
        if (!calleesWithWidenedInputs->contains(function))
            calleesWithWidenedInputs->append(function);
        for (Type passed : wasPassed)
            markEscaping(passed, FunctionSummary::MergedInParameter);
        return true;
    }

    void noteTailCall(Node* node)
    {
        if (!calleesWithWidenedInputs || !programFunctions())
            return;
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isExact);
        if (!known || !isExact || !known->forCall || !known->summary)
            return;
        if (known->conventionForCall.signature == Signature::List)
            markEscaping(functionType(known->summary->number), FunctionSummary::CalledIndirectly);
        if (!m_knownTailCallees.contains(known->summary))
            m_knownTailCallees.append(known->summary);
    }

    void noteMergedInto(Type whole, Type part, uint32_t why)
    {
        if (uint32_t function = functionNumberOf(part); function && functionNumberOf(whole) != function)
            markEscaping(part, why);
    }
    void noteJoin(Type before, Type added, uint32_t why)
    {
        noteMergedInto(before | added, before, why);
        noteMergedInto(before | added, added, why);
    }
    static uint32_t usedBy(Node* user) { return FunctionSummary::UsedBy | (user->kind == NodeKind::Bytecode ? static_cast<uint32_t>(user->opcode) : 1000 + static_cast<uint32_t>(user->kind)) << 8; }

    void markOperandsEscaping(Node* user)
    {
        for (auto& use : user->uses)
            markEscaping(use.node->type, usedBy(user));
    }

    void noteEscapingArguments(Node* node, VirtualRegister calleeRegister, unsigned argc, unsigned argv)
    {
        if (!mayBe(node->use(calleeRegister)->type, TAnyObject))
            return;
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isExact);
        unsigned followed = known && isExact && known->forCall && known->summary && known->summary->hasOnlyKnownCallers ? std::min<unsigned>(known->forCall->numParameters(), FunctionSummary::maxParameters) : 0;
        for (auto& use : node->uses) {
            if (use.reg == calleeRegister && use.reg.offset() != firstArgument) {
                if (!followed)
                    recordIndirectCall(use.node->type);
                else if (!isSubtype(use.node->type & TCell, TFunction))
                    noteNeedsObject(use.node->type);
                continue;
            }
            int index = use.reg.offset() - firstArgument;
            if (index >= 1 && static_cast<unsigned>(index) < followed && static_cast<unsigned>(index) < argc)
                continue;
            markEscaping(use.node->type, !followed ? FunctionSummary::PassedToUnknownCallee : !index ? FunctionSummary::PassedAsThis : FunctionSummary::PassedAsExtraArgument);
        }
    }

    void recordIndirectCall(Type callee) { markEscaping(callee, FunctionSummary::CalledIndirectly); }

    static bool inheritsFromFunctionPrototypeOnly(const KnownFunction& function)
    {
        if (function.executable->isClass())
            return false;
        switch (function.executable->parseMode()) {
        case SourceParseMode::NormalFunctionMode:
        case SourceParseMode::ArrowFunctionMode:
        case SourceParseMode::MethodMode:
        case SourceParseMode::GetterMode:
        case SourceParseMode::SetterMode:
            return true;
        default:
            return false;
        }
    }

    enum class Access : uint8_t { Read, Write };
    bool accessRunsNoProgramCode(Type base, const Identifier& property, Access access)
    {
        const KnownFunction* function = programFunctions()->function(functionNumberOf(base));
        if (!function || !function->executable || property.isPrivateName())
            return true;
        const CommonIdentifiers& names = *m_graph.vm().propertyNames;
        if constexpr (ImmutableIntrinsics::functionPrototypesAreSealed) {
            if (ImmutableIntrinsics::shared() && !function->executable->isClass())
                return property != names.prototype && property != names.underscoreProto && property != names.caller && property != names.arguments;
        }
        if (access == Access::Write)
            return false;
        if (property == names.name || property == names.length)
            return true;
        return inheritsFromFunctionPrototypeOnly(*function) && isDataPropertyOfFunctionPrototype(*property.impl());
    }

    bool hasInstanceRunsNoProgramCode(Type constructor)
    {
        const KnownFunction* function = programFunctions()->function(functionNumberOf(constructor));
        if (!function || !function->executable)
            return true;
        if (function->executable->isClass())
            return false;
        if constexpr (ImmutableIntrinsics::functionPrototypesAreSealed) {
            if (ImmutableIntrinsics::shared())
                return true;
        }
        return function->executable->parseMode() == SourceParseMode::NormalFunctionMode;
    }

    void noteReadOfProperty(Type base, const Identifier& property)
    {
        if (!accessRunsNoProgramCode(base, property, Access::Read))
            markEscaping(base, property == m_graph.vm().propertyNames->prototype ? FunctionSummary::PrototypeRead : FunctionSummary::PropertyRead, property.impl());
    }

    void noteEscapesThrough(Node* user)
    {
        switch (user->kind) {
        case NodeKind::Phi:
        case NodeKind::Narrow:
            for (auto& use : user->uses)
                noteMergedInto(user->type, use.node->type, FunctionSummary::MergedInPhi);
            return;
        case NodeKind::SetStack:
            noteMergedInto(m_graph.frameRegisterTypes[m_graph.registerIndex(user->reg)], user->uses[0].node->type, FunctionSummary::MergedInFrameRegister);
            if (m_graph.isArrayOperandRegister(user->reg))
                markEscaping(user->uses[0].node->type, usedBy(user));
            return;
        case NodeKind::Guard:
        case NodeKind::Proj:
            return;
        case NodeKind::Bytecode:
            break;
        default:
            markOperandsEscaping(user);
            return;
        }
        auto markOperandsEscapingExcept = [&](VirtualRegister harmless) {
            for (auto& use : user->uses) {
                if (use.reg != harmless)
                    markEscaping(use.node->type, usedBy(user));
            }
        };
        switch (user->opcode) {
        case op_is_empty:
        case op_is_undefined_or_null:
        case op_eq_null:
        case op_neq_null:
        case op_not:
        case op_jtrue:
        case op_jfalse:
        case op_jeq_null:
        case op_jneq_null:
        case op_jundefined_or_null:
        case op_jnundefined_or_null:
        case op_get_scope:
        case op_get_parent_scope:
        case op_check_tdz:
            return;
        case op_get_length:
        case op_get_prototype_of:
        case op_typeof:
        case op_typeof_is_undefined:
        case op_typeof_is_object:
        case op_typeof_is_function:
        case op_is_boolean:
        case op_is_number:
        case op_is_big_int:
        case op_is_object:
        case op_is_callable:
        case op_is_constructor:
        case op_is_cell_with_type:
        case op_has_structure_with_flags:
        case op_stricteq:
        case op_nstricteq:
        case op_jstricteq:
        case op_jnstricteq:
        case op_jeq_ptr:
        case op_jneq_ptr:
        case op_set_function_name:
            for (auto& use : user->uses)
                noteNeedsObject(use.node->type);
            return;
        case op_get_by_id:
            noteNeedsObject(user->use(user->as<OpGetById>().m_base)->type);
            return noteReadOfProperty(user->use(user->as<OpGetById>().m_base)->type, user->graph->codeBlock()->identifier(user->as<OpGetById>().m_property));
        case op_get_by_id_direct:
            noteNeedsObject(user->use(user->as<OpGetByIdDirect>().m_base)->type);
            return noteReadOfProperty(user->use(user->as<OpGetByIdDirect>().m_base)->type, user->graph->codeBlock()->identifier(user->as<OpGetByIdDirect>().m_property));
        case op_instanceof: {
            auto bytecode = user->as<OpInstanceof>();
            bool constructorIsHarmless = bytecode.m_constructor != bytecode.m_value && hasInstanceRunsNoProgramCode(user->use(bytecode.m_constructor)->type);
            for (auto& use : user->uses) {
                if (use.reg == bytecode.m_value)
                    markEscaping(use.node->type, FunctionSummary::LeftOfInstanceof);
                else if (use.reg != bytecode.m_constructor)
                    markEscaping(use.node->type, usedBy(user));
                else if (!constructorIsHarmless)
                    markEscaping(use.node->type, FunctionSummary::RightOfInstanceof);
                else
                    noteNeedsObject(use.node->type);
            }
            return;
        }
        case op_put_by_id: {
            auto bytecode = user->as<OpPutById>();
            const Identifier& property = user->graph->codeBlock()->identifier(bytecode.m_property);
            Type base = user->use(bytecode.m_base)->type;
            noteNeedsObject(base);
            if (!bytecode.m_flags.isDirect() && !accessRunsNoProgramCode(base, property, Access::Write))
                markEscaping(base, FunctionSummary::PropertyWritten, property.impl());
            markEscaping(user->use(bytecode.m_value)->type, FunctionSummary::StoredInProperty);
            return;
        }
        case op_put_by_val:
            markEscaping(user->use(user->as<OpPutByVal>().m_value)->type, FunctionSummary::StoredInProperty);
            return markOperandsEscapingExcept(user->as<OpPutByVal>().m_value);
        case op_put_by_val_direct:
            markEscaping(user->use(user->as<OpPutByValDirect>().m_value)->type, FunctionSummary::StoredInProperty);
            return markOperandsEscapingExcept(user->as<OpPutByValDirect>().m_value);
        case op_construct: {
            auto bytecode = user->as<OpConstruct>();
            VirtualRegister newTarget(-static_cast<int>(bytecode.m_argv) + CallFrame::thisArgumentOffset());
            for (auto& use : user->uses)
                markEscaping(use.node->type, use.reg == bytecode.m_callee || use.reg == newTarget ? FunctionSummary::Constructed : FunctionSummary::PassedToUnknownCallee);
            return;
        }
        case op_get_from_scope: {
            bool isExact = false;
            if (const KnownFunction* known = m_graph.knownFunctionReadBy(user, &isExact); known && !isExact)
                markEscaping(closureTypeFor(known->executable), FunctionSummary::ReadInexactly);
            return;
        }
        case op_define_data_property:
            if (auto* classes = programClasses(); classes && classes->isNonEscapingMethod(functionNumberOf(user->use(user->as<OpDefineDataProperty>().m_value)->type))) {
                noteNeedsObject(user->use(user->as<OpDefineDataProperty>().m_value)->type);
                return markOperandsEscapingExcept(user->as<OpDefineDataProperty>().m_value);
            }
            markOperandsEscaping(user);
            return;

        case op_check_type:
        case op_type_tag:
        case op_to_this:
        case op_to_object:
        case op_identity_with_profile:
        case op_resolve_scope:
            for (auto& use : user->uses) {
                noteNeedsObject(use.node->type);
                noteMergedInto(user->type, use.node->type, FunctionSummary::LostThroughAlias | static_cast<uint32_t>(user->opcode) << 8);
            }
            return;

        case op_put_to_scope:
        case op_create_lexical_environment:
        case op_new_func:
        case op_new_func_exp:
        case op_new_generator_func:
        case op_new_generator_func_exp:
        case op_new_async_func:
        case op_new_async_func_exp:
        case op_new_async_generator_func:
        case op_new_async_generator_func_exp:
            return;

        case op_call:
            return noteEscapingArguments(user, user->as<OpCall>().m_callee, user->as<OpCall>().m_argc, user->as<OpCall>().m_argv);
        case op_call_ignore_result:
            return noteEscapingArguments(user, user->as<OpCallIgnoreResult>().m_callee, user->as<OpCallIgnoreResult>().m_argc, user->as<OpCallIgnoreResult>().m_argv);
        case op_tail_call: {
            noteEscapingArguments(user, user->as<OpTailCall>().m_callee, user->as<OpTailCall>().m_argc, user->as<OpTailCall>().m_argv);
            if (user->graph == &m_graph)
                recordReturnedValues(callResult(user));
            return;
        }
        case op_ret:
            if (user->graph == &m_graph)
                recordReturnedValues(user->use(user->as<OpRet>().m_value)->type);
            return;
        default:
            markOperandsEscaping(user);
            return;
        }
    }

    void recordReturnedValues(Type type)
    {
        const FunctionSummary* summary = m_graph.summary();
        if (!summary || !summary->hasOnlyKnownCallers || summary->hasUnknownCallers.load(std::memory_order_relaxed)) {
            markEscaping(type, FunctionSummary::ReturnedToUnknownCaller);
            return;
        }
        noteJoin(calleesWithWidenedInputs ? summary->returnType.join(type & TTop) : summary->returnType.load(), type, FunctionSummary::MergedInReturn);
    }

    Type closureTypeFor(UnlinkedFunctionExecutable* executable)
    {
        if (!programFunctions())
            return TFunction;
        uint32_t number = programFunctions()->numberOf(executable);
        return number ? functionType(number) : TFunction;
    }

    void noteStoresToVariables(Node* node)
    {
        VariableSummaries* summaries = m_graph.variableSummaries();
        if (!summaries)
            return;
        auto noteInitialValue = [&](VirtualRegister initialValue) {
            if (const void* scope = m_graph.scopeIdentity(node)) {
                if (node->graph->isGeneratorFrame(scope))
                    summaries->noteInitialValueIsNeverRead(scope);
                summaries->join({ scope, Variable::initialValue }, node->use(initialValue)->type);
            }
        };
        switch (node->opcode) {
        case op_put_to_scope:
            if (node->as<OpPutToScope>().m_var == UINT_MAX) {
                if (programFunctions())
                    markEscaping(node->use(node->as<OpPutToScope>().m_value)->type, FunctionSummary::StoredInUntrackedVariable);
                return;
            }
            if (Variable variable = m_graph.variableAccessedBy(node)) {
                Type put = node->use(node->as<OpPutToScope>().m_value)->type;
                Type before = summaries->join(variable, put);
                if (programFunctions()) {
                    UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl();
                    const CalleeHints* hints = m_graph.calleeHints();
                    const KnownFunction* known = hints && hints->variableScope() == variable.scope ? hints->find(name, variable.offset) : nullptr;
                    bool isAnyModuleScope = summaries->isModuleScope(variable.scope);
                    if (summaries->isUntracked(variable, name))
                        markEscaping(put, FunctionSummary::StoredInUntrackedVariable);
                    else if (summaries->isDynamicallyRead(name))
                        markEscaping(put, FunctionSummary::StoredInDynamicallyReadVariable);
                    else if (isAnyModuleScope && (!known || known->isExternallyVisible))
                        markEscaping(put, FunctionSummary::StoredInModuleVariable);
                    else
                        noteJoin(before, put, FunctionSummary::MergedInVariable);
                }
                if (!m_graph.nameForLog().isNull()) [[unlikely]]
                    dataLogLn("AOT inference: put `", node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl(), "` scope ", RawPointer(variable.scope), " offset ", variable.offset, " in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset(), ": ", TypeDump(node->use(node->as<OpPutToScope>().m_value)->type));
            } else if (programFunctions())
                markEscaping(node->use(node->as<OpPutToScope>().m_value)->type, FunctionSummary::StoredToUnknownLocation);
            if (!m_graph.variableAccessedBy(node) && !m_graph.nameForLog().isNull()) [[unlikely]]
                dataLogLn("AOT inference: put `", node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl(), "` at an unknown location in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset());
            return;
        case op_create_lexical_environment:
            noteInitialValue(node->as<OpCreateLexicalEnvironment>().m_initialValue);
            return;
        case op_create_generator_frame_environment:
            noteInitialValue(node->as<OpCreateGeneratorFrameEnvironment>().m_initialValue);
            return;
        default:
            RELEASE_ASSERT(!(traitsOf(node->opcode) & OpcodeTraits::StoresToVariables));
            return;
        }
    }

    void reportContradiction(Node* node, ASCIILiteral what, const String& name, Type found, Type recorded)
    {
        const KnownFunction* function = programFunctions() && m_graph.summary() ? programFunctions()->function(m_graph.summary()->number) : nullptr;
        dataLogLn("AOT: contradicts the analysis: ", what, " `", name, "` at bc#", node->bytecodeIndex.offset(), node->graph != &m_graph ? " (inlined)" : "", " in `", function && function->executable ? function->executable->ecmaName().string() : String(), "` @", function ? function->key.module : 0, ":", function ? function->key.start : 0, ": ", TypeDump(found), " is not in ", TypeDump(recorded));
    }

    bool isSavedAtDefinition(Variable slot)
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isElided || !node->isBytecode(op_put_to_scope))
                    continue;
                Variable other = m_graph.variableAccessedBy(node);
                if (other.scope == slot.scope && other.offset == slot.offset)
                    return true;
            }
        }
        return false;
    }

    void verifyAgainstSummaries(Node* node)
    {
        switch (node->opcode) {
        case op_put_to_scope: {
            VariableSummaries* summaries = m_graph.variableSummaries();
            Variable variable = summaries ? m_graph.variableAccessedBy(node) : Variable { };
            if (!variable || node->as<OpPutToScope>().m_var == UINT_MAX)
                return;
            if (node->graph->isGeneratorFrame(variable.scope)) {
                if (!node->isElided && isSavedAtDefinition(variable))
                    return;
            } else if (node->isElided)
                return;
            UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl();
            Node* value = node->use(node->as<OpPutToScope>().m_value);
            if (value->wasInferredUnreachable)
                return;
            Type put = value->type & TTop;
            Type recorded = summaries->read(variable, name, VariableSummaries::nobody);
            if (!isSubtype(put, recorded))
                reportContradiction(node, "the store to"_s, String(name), put, recorded);
            return;
        }
        case op_ret: {
            const KnownFunction* function = programFunctions() && m_graph.summary() ? programFunctions()->function(m_graph.summary()->number) : nullptr;
            if (node->isElided || !function || m_graph.summary()->function || !m_graph.summary()->isReached() || function->forCall != m_graph.codeBlock() || node->graph != &m_graph)
                return;
            if (node->use(node->as<OpRet>().m_value)->wasInferredUnreachable)
                return;
            Type returned = node->use(node->as<OpRet>().m_value)->type & TTop;
            Type recorded = function->returnType.load();
            if (!isSubtype(returned, recorded))
                reportContradiction(node, "the result of"_s, "return"_s, returned, recorded);
            return;
        }
        case op_call:
        case op_call_ignore_result:
        case op_tail_call: {
            auto operands = Graph::callOperands(node->instruction);
            bool isExact = false;
            const KnownFunction* known = node->isElided ? nullptr : m_graph.knownCallee(node, &isExact);
            if (!known || !isExact || !known->forCall || !known->summary || !known->summary->hasOnlyKnownCallers || !known->summary->isReached())
                return;
            int firstArgument = -static_cast<int>(operands.argv) + CallFrame::thisArgumentOffset();
            if (node->use(operands.callee)->wasInferredUnreachable || !mayBe(node->use(operands.callee)->type, TAnyObject))
                return;
            for (unsigned i = 0; i < operands.argc; ++i) {
                Node* argument = node->use(VirtualRegister(firstArgument + i));
                if (argument->wasInferredUnreachable || !argument->type)
                    return;
            }
            unsigned count = std::min<unsigned>(std::max<unsigned>(known->forCall->numParameters(), known->conventionForCall.numberOfParameters + 1), FunctionSummary::maxParameters);
            for (unsigned i = 1; i < count; ++i) {
                Type passed = i < operands.argc ? node->use(VirtualRegister(firstArgument + i))->type & TTop : TUndefined;
                Type recorded = known->summary->parameterTypes[i].load();
                if (!isSubtype(passed, recorded))
                    reportContradiction(node, "an argument of"_s, makeString(known->executable ? known->executable->ecmaName().string() : String(), " #"_s, i), passed, recorded);
            }
            Type passed = node->use(VirtualRegister(firstArgument))->type & TTop;
            Type recorded = known->summary->thisType.load();
            if (!isSubtype(passed, recorded))
                reportContradiction(node, "this of"_s, known->executable ? known->executable->ecmaName().string() : String(), passed, recorded);
            return;
        }
        default:
            return;
        }
    }

    void noteArgumentsOf(Node* node)
    {
        unsigned argc;
        unsigned argv;
        switch (node->opcode) {
        case op_call:
            argc = node->as<OpCall>().m_argc;
            argv = node->as<OpCall>().m_argv;
            break;
        case op_call_ignore_result:
            argc = node->as<OpCallIgnoreResult>().m_argc;
            argv = node->as<OpCallIgnoreResult>().m_argv;
            break;
        case op_tail_call:
            argc = node->as<OpTailCall>().m_argc;
            argv = node->as<OpTailCall>().m_argv;
            break;
        default:
            return;
        }
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isExact);
        if (!known || !isExact || !known->forCall || !known->summary || !known->summary->hasOnlyKnownCallers)
            return;
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        for (unsigned i = 0; i < argc; ++i) {
            if (!node->use(VirtualRegister(firstArgument + i))->type) {
                if (!m_graph.nameForLog().isNull()) [[unlikely]]
                    dataLogLn("AOT inference: call of `", known->executable->name().impl(), "` @", known->key.module, ":", known->key.start, " in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset(), " IS NOT REACHED: argument ", i, " is nothing");
                return;
            }
        }
        if (!m_graph.nameForLog().isNull()) [[unlikely]] {
            StringPrintStream out;
            for (unsigned i = 1; i < argc; ++i)
                out.print(" ", TypeDump(node->use(VirtualRegister(firstArgument + i))->type));
            dataLogLn("AOT inference: call of `", known->executable->name().impl(), "` @", known->key.module, ":", known->key.start, " in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset(), " passes", out.toString());
        }
        bool widensInputs = false;
        bool collectsRemarks = Options::aotRemarksPath() || Options::aotTypeCoveragePath();
        unsigned count = std::min<unsigned>(std::max<unsigned>(known->forCall->numParameters(), known->conventionForCall.numberOfParameters + 1), FunctionSummary::maxParameters);
        for (unsigned i = 1; i < count; ++i) {
            Type type = i < argc ? node->use(VirtualRegister(firstArgument + i))->type & TTop : TUndefined;
            if (collectsRemarks) [[unlikely]]
                known->summary->typesPassedByDirectCalls[i].join(type);
            Type before = known->summary->parameterTypes[i].join(type);
            widensInputs |= (before | type) != before;
            if (programFunctions())
                noteJoin(before, type, FunctionSummary::MergedInParameter);
        }
        {
            Type type = node->use(VirtualRegister(firstArgument))->type & TTop;
            Type before = known->summary->thisType.join(type);
            widensInputs |= (before | type) != before;
        }
        if (collectsRemarks) [[unlikely]]
            known->summary->typesPassedByDirectCalls[0].join(TTop);
        Type before = known->summary->parameterTypes[0].join(TTop);
        widensInputs |= before != TTop;
        if (widensInputs && !calleesWithWidenedInputs->contains(known))
            calleesWithWidenedInputs->append(known);
    }

    std::optional<Type> builtinCallResult(Node* node)
    {
        VirtualRegister calleeRegister;
        unsigned argc;
        unsigned argv;
        if (node->isBytecode(op_call)) {
            auto bytecode = node->as<OpCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
        } else if (node->isBytecode(op_tail_call)) {
            auto bytecode = node->as<OpTailCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
        } else
            return std::nullopt;
        Node* callee = node->use(calleeRegister);
        if (argc == 1 && node->opcode == op_call && Graph::linkTimeConstantOf(callee) == LinkTimeConstant::cloneObject) {
            uint16_t layoutID = Graph::newObjectLayoutID(node);
            return layoutID ? objectTypeForLayout(layoutID) : TFinalObject;
        }
        if (argc == 2 && Graph::linkTimeConstantOf(callee) == LinkTimeConstant::toLength) {
            Type argument = node->use(VirtualRegister(-static_cast<int>(argv) + CallFrame::thisArgumentOffset() + 1))->type;
            if (!argument)
                return TNone;
            return isSubtype(argument, TInt32) ? TInt32 : TNumber;
        }
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        unsigned number = 0;
        bool isMethodOfObject = false;
        bool receiverMayBeNullish = false;
        if (callee->kind == NodeKind::Intrinsic)
            number = callee->intrinsic;
        else if (uint32_t aliased = callee->isBytecode(op_get_by_id) ? 0 : intrinsicFunctionOf(callee->type))
            number = aliased;
        else if (callee->isBytecode(op_get_by_id)) {
            auto bytecode = callee->as<OpGetById>();
            Node* receiver = callee->use(bytecode.m_base);
            Type base = receiver->type & ~(TOther | TEmpty);
            if (!base)
                return TNone;
            number = intrinsicFoundOnPrimitive(base, *callee->graph->codeBlock()->identifier(bytecode.m_property).impl());
            if (!number && node->use(VirtualRegister(firstArgument)) == receiver) {
                number = builtinMethodReadFromObject(callee, node);
                isMethodOfObject = true;
                if (!number && isOnlyTestedAndBooleanUnlessShadowed(node, callee, base))
                    return TBoolean;
            }
            receiverMayBeNullish = mayBe(receiver->type, TOther);
        }
        if (!number)
            return std::nullopt;
        auto signature = intrinsicSignature(number);
        if (!signature)
            return std::nullopt;
        if (isMethodOfObject)
            m_graph.remark("typed-call-of-builtin-method"_s);
        if (receiverMayBeNullish)
            m_graph.remark("typed-builtin-call-unless-receiver-is-nullish"_s);
        switch (signature->condition) {
        case BuiltinSignature::Condition::Always:
            break;
        case BuiltinSignature::Condition::IfFirstArgumentIsNotObject: {
            if (argc < 2)
                break;
            Node* argument = node->use(VirtualRegister(firstArgument + 1));
            Type first = argument->type;
            if (!first)
                return TNone;
            if (isSubtype(first, TPrimitive))
                break;
            if (!argument->isBytecode(op_new_reg_exp) || !isUsedOnceAndOnlyBy(argument, node))
                return std::nullopt;
            m_graph.remark("typed-call-with-untouched-regexp-literal"_s);
            break;
        }
        case BuiltinSignature::Condition::IfThisIsHolder: {
            Node* thisValue = node->use(VirtualRegister(firstArgument));
            const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
            if (thisValue->kind != NodeKind::Intrinsic || thisValue->intrinsic != intrinsics->at(intrinsics->at(number).holder).canonical)
                return std::nullopt;
            break;
        }
        case BuiltinSignature::Condition::IsInt32IfArgumentsAre:
        case BuiltinSignature::Condition::IsInt32IfOnlyArgumentIs: {
            if (argc < 2 || (argc > 2 && signature->condition == BuiltinSignature::Condition::IsInt32IfOnlyArgumentIs))
                break;
            Type arguments = TNone;
            for (unsigned i = 1; i < argc; ++i) {
                Type argument = node->use(VirtualRegister(firstArgument + static_cast<int>(i)))->type;
                if (!argument)
                    return TNone;
                arguments |= argument;
            }
            if (!isSubtype(arguments, TInt32))
                break;
            m_graph.remark("int32-result-of-builtin-with-int32-arguments"_s);
            return TInt32;
        }
        case BuiltinSignature::Condition::IfReceiverIsOriginal:
            if (!isMethodOfObject)
                return std::nullopt;
            break;
        }
        return signature->result;
    }

    bool isUsedOnceAndOnlyBy(Node* value, Node* user)
    {
        unsigned numberOfUses = 0;
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis) {
                for (auto& use : phi->uses) {
                    if (use.node == value)
                        return false;
                }
            }
            for (Node* node : block->nodes) {
                if (node == user->guard)
                    continue;
                for (auto& use : node->uses) {
                    if (use.node != value)
                        continue;
                    if (node != user)
                        return false;
                    ++numberOfUses;
                }
            }
        }
        return numberOfUses == 1;
    }

    bool isOnlyTestedAndBooleanUnlessShadowed(Node* call, Node* read, Type base)
    {
        if (!call->isBytecode(op_call) || Graph::closedMethodReadBy(read) || m_graph.calleeKnownByFact(call))
            return false;
        Receiver kind = receiverWithType(base);
        if (kind == Receiver::None || kind == Receiver::String || kind == Receiver::Number)
            return false;
        unsigned method = intrinsicFoundOn(kind, *read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property).impl());
        if (!method)
            return false;
        auto signature = intrinsicSignature(method);
        if (!signature || signature->condition != BuiltinSignature::Condition::Always || signature->result != TBoolean)
            return false;
        auto usersOfResult = users().of(call);
        if (usersOfResult.empty())
            return false;
        for (Node* user : usersOfResult) {
            if (!user->isBytecode(op_jtrue) && !user->isBytecode(op_jfalse) && !user->isBytecode(op_not))
                return false;
        }
        m_graph.remark("boolean-result-of-method-that-is-only-tested"_s);
        return true;
    }

    static constexpr bool trustsThatArraysAreOriginal = false;

    static Receiver kindAllocatedBy(Node* node)
    {
        if (node->kind != NodeKind::Bytecode)
            return Receiver::None;
        switch (node->opcode) {
        case op_new_array:
        case op_new_array_buffer:
        case op_new_array_with_size:
        case op_new_array_with_spread:
        case op_create_rest:
            return Receiver::Array;
        case op_new_reg_exp:
            return Receiver::RegExp;
        case op_construct: {
            auto bytecode = node->as<OpConstruct>();
            Node* callee = node->use(bytecode.m_callee);
            if (callee->kind != NodeKind::Intrinsic || node->use(VirtualRegister(-static_cast<int>(bytecode.m_argv) + CallFrame::thisArgumentOffset())) != callee)
                return Receiver::None;
            auto made = constructingIntrinsicResult(callee->intrinsic);
            return made ? receiverWithType(*made) : Receiver::None;
        }
        default:
            return Receiver::None;
        }
    }

    static bool isCall(const Node* node) { return node->isBytecode(op_call) || node->isBytecode(op_call_ignore_result) || node->isBytecode(op_tail_call); }

    static Node* allocationBehind(Node* node)
    {
        for (unsigned depth = 0; depth < 8; ++depth) {
            if (kindAllocatedBy(node) != Receiver::None)
                return node;
            if (!node->isBytecode(op_call))
                return nullptr;
            auto operands = Graph::callOperands(node->instruction);
            Node* callee = node->use(operands.callee);
            if (!callee->isBytecode(op_get_by_id) || callee->use(callee->as<OpGetById>().m_base) != node->use(operands.argument(0)))
                return nullptr;
            node = node->use(operands.argument(0));
        }
        return nullptr;
    }

    static unsigned builtinMethodReadFrom(Node* read, Node* object, Receiver kind)
    {
        if (!read->isBytecode(op_get_by_id) || read->use(read->as<OpGetById>().m_base) != object)
            return 0;
        return intrinsicFoundOn(kind, *read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property).impl());
    }

    static bool readsSizeFrom(Node* read, Node* object, Receiver kind)
    {
        if ((kind != Receiver::Map && kind != Receiver::Set) || !read->isBytecode(op_get_by_id) || read->use(read->as<OpGetById>().m_base) != object)
            return false;
        return read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property) == read->graph->vm().propertyNames->size;
    }

    static bool onlyTests(const Node* user)
    {
        if (user->kind == NodeKind::Guard)
            return true;
        if (user->kind != NodeKind::Bytecode)
            return false;
        switch (user->opcode) {
        case op_is_empty:
        case op_is_undefined_or_null:
        case op_eq_null:
        case op_neq_null:
        case op_not:
        case op_jtrue:
        case op_jfalse:
        case op_jeq_null:
        case op_jneq_null:
        case op_jundefined_or_null:
        case op_jnundefined_or_null:
        case op_check_tdz:
        case op_typeof:
        case op_typeof_is_undefined:
        case op_typeof_is_object:
        case op_typeof_is_function:
        case op_is_object:
        case op_is_cell_with_type:
        case op_stricteq:
        case op_nstricteq:
        case op_jstricteq:
        case op_jnstricteq:
            return true;
        default:
            return false;
        }
    }

    bool isInsideInlineeOf(const Node* user, const Node* call)
    {
        uint32_t callSite = CallSiteIndex(call->bytecodeIndex).bits();
        for (unsigned frame = user->graph->inlineFrame(); frame; frame = m_graph.inlineFrames[frame].parent) {
            if (m_graph.inlineFrames[frame].parent == call->graph->inlineFrame() && m_graph.inlineFrames[frame].callSite == callSite)
                return true;
        }
        return false;
    }

    bool isUsedOnlyAsReceiverOf(Node* allocation, Node* read, Node* call)
    {
        auto beforeInlining = [](BasicBlock* block) { return block->splitFrom ? block->splitFrom : block; };
        if (beforeInlining(allocation->block) != beforeInlining(read->block))
            return false;
        for (Node* user : users().of(allocation)) {
            if (user != read && user != call && user->kind != NodeKind::Guard && !isInsideInlineeOf(user, call))
                return false;
        }
        unsigned numberOfUses = 0;
        for (auto& use : call->uses)
            numberOfUses += use.node == allocation;
        return numberOfUses == 1;
    }

    bool isUntouchedReceiverOfInlinedCall(Node* inside, Node* array)
    {
        if (!inside->graph->isInlinedBuiltin || kindAllocatedBy(array) != Receiver::Array)
            return false;
        for (Node* user : users().of(array)) {
            if (!isCall(user) || !isInsideInlineeOf(inside, user))
                continue;
            Node* read = user->use(Graph::callOperands(user->instruction).callee);
            return builtinMethodReadFrom(read, array, Receiver::Array) && isUsedOnlyAsReceiverOf(array, read, user);
        }
        return false;
    }

    const Vector<Node*, 4>& aliasesIfKeptToItself(Node* allocation, Receiver kind)
    {
        return m_aliasesOfObjectsKeptToThemselves.ensure(allocation, [&]() -> Vector<Node*, 4> {
            Vector<Node*, 4> aliases { allocation };
            for (unsigned i = 0; i < aliases.size(); ++i) {
                Node* object = aliases[i];
                for (Node* user : users().of(object)) {
                    if (onlyTests(user) || builtinMethodReadFrom(user, object, kind) || readsSizeFrom(user, object, kind))
                        continue;
                    if (!isCall(user))
                        return { };
                    auto operands = Graph::callOperands(user->instruction);
                    unsigned method = builtinMethodReadFrom(user->use(operands.callee), object, kind);
                    if (!method || !keepsReceiverToItself(method))
                        return { };
                    for (auto& use : user->uses) {
                        if ((use.node == object) != (use.reg == operands.argument(0)))
                            return { };
                    }
                    if (!returnsReceiver(method) || user->isBytecode(op_call_ignore_result))
                        continue;
                    if (user->isBytecode(op_tail_call))
                        return { };
                    if (!aliases.contains(user))
                        aliases.append(user);
                }
            }
            return aliases;
        }).iterator->value;
    }

    unsigned builtinMethodReadFromObject(Node* read, Node* call)
    {
        Node* receiver = read->use(read->as<OpGetById>().m_base);
        const StringImpl& name = *read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property).impl();
        if (Node* allocation = allocationBehind(receiver)) {
            Receiver kind = kindAllocatedBy(allocation);
            unsigned method = intrinsicFoundOn(kind, name);
            if (method && receiver == allocation && isUsedOnlyAsReceiverOf(allocation, read, call))
                return method;
            if (method && kind != Receiver::Array && aliasesIfKeptToItself(allocation, kind).contains(receiver))
                return method;
        }
        if constexpr (trustsThatArraysAreOriginal) {
            Type base = receiver->type & ~(TOther | TEmpty);
            if (base && isSubtype(base, TArray) && programClasses() && !programClasses()->mayBeExtended(TArray))
                return intrinsicFoundOn(Receiver::Array, name);
        }
        return 0;
    }

    bool readsSizeOfMapOrSet(Node* read)
    {
        Node* receiver = read->use(read->as<OpGetById>().m_base);
        Node* allocation = allocationBehind(receiver);
        if (!allocation)
            return false;
        Receiver kind = kindAllocatedBy(allocation);
        return readsSizeFrom(read, receiver, kind) && aliasesIfKeptToItself(allocation, kind).contains(receiver);
    }

    const NodeUsers& users()
    {
        if (!m_users)
            m_users.emplace(m_graph);
        return *m_users;
    }

    void recordReturnValueTypesOf(Node* node)
    {
        const FunctionSummary* summary = m_graph.summary();
        if (!calleesWithWidenedInputs || node->graph != &m_graph)
            return;
        auto* names = registerReturnValuesOf(m_graph.codeBlock(), summary);
        if (!names)
            return;
        Node* object = node->use(node->as<OpRet>().m_value);
        RELEASE_ASSERT(object->isBytecode(op_new_object) && object->numberOfLiteralProperties == names->size());
        for (unsigned i = 0; i < names->size(); ++i) {
            Type type = object->use(NewObjectPlan::registerOf(i))->type;
            if (auto fieldType = Graph::fieldTypeInLayout(Graph::newObjectLayoutID(object), names->at(i)))
                type = fieldType->typeOfStored(type);
            Type old = summary->returnValueTypes[i].join(type);
            if ((old | type) != old)
                summary->returnValueTypesChanged.store(true, std::memory_order_relaxed);
        }
    }

    void recordWhetherReturnObjectIsNeededBy(Node* node)
    {
        if (!multiValueReturnTable() || node->opcode != op_tail_call)
            return;
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isExact);
        if (!known || !isExact || !known->forCall || !known->summary)
            return;
        if (!multiValueReturnTable()->returnValueNamesOf(known->forCall))
            return;
        if (known->summary->needsReturnObject.exchange(true, std::memory_order_relaxed))
            return;
        known->summary->returnValueTypesChanged.store(true, std::memory_order_relaxed);
        if (!calleesWithWidenedInputs->contains(known))
            calleesWithWidenedInputs->append(known);
    }

    std::optional<Type> returnValueTypeReadBy(Node* read)
    {
        if (!multiValueReturnTable())
            return std::nullopt;
        Node* call = read->use(read->as<OpGetById>().m_base);
        while (call->kind == NodeKind::Narrow || call->isBytecode(op_check_type) || call->isBytecode(op_check_tdz) || call->isBytecode(op_type_tag))
            call = call->uses[0].node;
        if (!call->isBytecode(op_call))
            return std::nullopt;
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(call, &isExact);
        if (!known || !isExact || !known->forCall)
            return std::nullopt;
        auto* names = registerReturnValuesOf(known->forCall, known->summary);
        if (!names)
            return std::nullopt;
        size_t index = names->find(read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property).impl());
        if (index == notFound || !users().isOnlyRead(call, names->span(), 0))
            return std::nullopt;
        if (calleesRead && !calleesRead->contains(known))
            calleesRead->append(known);
        return known->summary->returnValueTypes[index].load();
    }

    Type callResult(Node* node)
    {
        if (auto result = builtinCallResult(node))
            return *result;
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isExact);
        if (!known || !isExact || !known->forCall)
            return mayBe(node->use(node->opcode == op_tail_call ? node->as<OpTailCall>().m_callee : node->as<OpCall>().m_callee)->type, TAnyObject) ? TTop : TNone;
        if (calleesRead && !calleesRead->contains(known))
            calleesRead->append(known);
        return known->returnType.load();
    }

    bool update(Node* node)
    {
        Type computed = compute(node);
        if (Options::validateAOTInferredTypes() && calleesWithWidenedInputs && (node->type & ~computed)) [[unlikely]] {
            StringPrintStream operands;
            for (auto& use : node->uses)
                operands.print(" ", TypeDump(use.node->type));
            dataLogLn("AOT: not monotone: kind ", static_cast<unsigned>(node->kind), node->kind == NodeKind::Bytecode ? " " : "", node->kind == NodeKind::Bytecode ? opcodeNames[node->opcode] : ""_s, node->speculatedType ? " speculated" : "", node->narrowedTo ? " narrowed" : "", node->target ? " target" : "", ": had ", TypeDump(node->type), ", now ", TypeDump(computed), ", from", operands.toString());
        }
        Type type = node->type | computed;
        if (type == node->type)
            return false;
        node->type = type;
        return true;
    }

    Type arithResult(Type left, Type right)
    {
        left &= ~TEmpty;
        right &= ~TEmpty;
        if (!left || !right)
            return TNone;
        if (isSubtype(left | right, TNumberLike | TString))
            return TNumber;
        if (isSubtype(left, TNumberLike | TString) || isSubtype(right, TNumberLike | TString)) {
            m_graph.remark("number-result-because-one-operand-is-no-bigint"_s);
            return TNumber;
        }
        return TNumber | TBigInt;
    }

    static Type bitResult(Type left, Type right)
    {
        left &= ~TEmpty;
        right &= ~TEmpty;
        if (!left || !right)
            return TNone;
        if (isSubtype(left, TNumberLike | TString) || isSubtype(right, TNumberLike | TString))
            return TInt32;
        return TInt32 | TBigInt;
    }

    static Type unaryArithResult(Type operand)
    {
        operand &= ~TEmpty;
        if (!operand)
            return TNone;
        if (isSubtype(operand, TNumberLike | TString))
            return TNumber;
        return TNumber | TBigInt;
    }

    Type compute(Node* node)
    {
        switch (node->kind) {
        case NodeKind::Constant:
        case NodeKind::ConstantCell:
        case NodeKind::Intrinsic:
        case NodeKind::LinkTimeConstant:
        case NodeKind::Argument:
            return node->type;
        case NodeKind::Phi: {
            Type type = TNone;
            for (unsigned i = 0; i < node->uses.size(); ++i) {
                if (mayComeFrom(node->block, node->block->predecessors[i]))
                    type |= node->uses[i].node->type;
            }
            return type;
        }
        case NodeKind::GetStack:
            return m_graph.frameRegisterTypes[m_graph.registerIndex(node->reg)];
        case NodeKind::SetStack: {
            Type& homed = m_graph.frameRegisterTypes[m_graph.registerIndex(node->reg)];
            homed |= node->uses[0].node->type;
            return homed;
        }
        case NodeKind::Guard:
            return TNone;
        case NodeKind::Narrow:
            if (node->fieldOrigin)
                return node->fieldType.typeOfStored(node->uses[0].node->type);
            if (node->speculatedType) {
                Type value = node->uses[0].node->type;
                if (calleesWithWidenedInputs)
                    return value;
                return mayBe(value, node->speculatedType) && !isSubtype(value, TNumber) ? value & node->speculatedType : value;
            }
            if (node->narrowedTo)
                return node->uses[0].node->type & node->narrowedTo;
            return node->target ? node->uses[0].node->type & node->target->type : node->uses[0].node->type;
        case NodeKind::Proj:
            return computeProj(node);
        case NodeKind::Bytecode:
            return computeBytecode(node);
        }
        RELEASE_ASSERT_NOT_REACHED();
        return TAll;
    }

    Type computeProj(Node* node)
    {
        Node* parent = node->uses[0].node;
        switch (parent->opcode) {
        case op_instanceof:
            if (node->reg == parent->as<OpInstanceof>().m_dst)
                return TBoolean;
            return TAll;
        case op_iterator_next: {
            auto bytecode = parent->as<OpIteratorNext>();
            if (node->reg == bytecode.m_done)
                return TBoolean;
            if (node->reg == bytecode.m_next) {
                Type next = parent->use(bytecode.m_next)->type;
                return next ? next | TNumber : TNone;
            }
            return TAll;
        }
        case op_iterator_open:
            if (node->reg == parent->as<OpIteratorOpen>().m_iterator)
                return TAnyObject | TCellOther;
            return TTop;
        case op_async_iterator_open:
            if (node->reg == parent->as<OpAsyncIteratorOpen>().m_iterator)
                return TAnyObject;
            return TTop;
        case op_enumerator_next: {
            auto bytecode = parent->as<OpEnumeratorNext>();
            if (node->reg == bytecode.m_propertyName)
                return TString;
            if (node->reg == bytecode.m_mode)
                return TInt32;
            return TNumber;
        }
        case op_catch:
            if (node->reg == parent->as<OpCatch>().m_exception)
                return TCellOther;
            return TAll;
        default:
            return TAll;
        }
    }

    Type computeBytecode(Node* node)
    {
        auto typeOf = [&](VirtualRegister reg) {
            return node->use(reg)->type;
        };

        if (auto [array, element] = m_graph.arrayAndElementStored(node); array && Graph::isLocallyAllocatedArray(array) && !node->block->isGeneric) {
            Type& elements = m_elementTypes.add(array, TNone).iterator->value;
            if (element->type & ~elements) {
                elements |= element->type;
                m_elementTypesChanged = true;
            }
        }

        switch (node->opcode) {
        case op_add: {
            auto bytecode = node->as<OpAdd>();
            Type left = typeOf(bytecode.m_lhs) & TTop;
            Type right = typeOf(bytecode.m_rhs) & TTop;
            if (!left || !right)
                return TNone;
            if (isSubtype(left | right, TNumberLike))
                return TNumber;
            if (isSubtype(left, TString) || isSubtype(right, TString))
                return TString;
            Type result = TNone;
            if (mayBe(left | right, TString | TAnyObject))
                result |= TString;
            if (mayBe(left, TBigInt | TAnyObject) && mayBe(right, TBigInt | TAnyObject))
                result |= TBigInt;
            if (mayBe(left, TNumberLike | TAnyObject) && mayBe(right, TNumberLike | TAnyObject))
                result |= TNumber;
            return result;
        }
        case op_sub: {
            auto bytecode = node->as<OpSub>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_mul: {
            auto bytecode = node->as<OpMul>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_div: {
            auto bytecode = node->as<OpDiv>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_mod: {
            auto bytecode = node->as<OpMod>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_pow: {
            auto bytecode = node->as<OpPow>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_bitand: {
            auto bytecode = node->as<OpBitand>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_bitor: {
            auto bytecode = node->as<OpBitor>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_bitxor: {
            auto bytecode = node->as<OpBitxor>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_lshift: {
            auto bytecode = node->as<OpLshift>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_rshift: {
            auto bytecode = node->as<OpRshift>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_check_type: {
            auto bytecode = node->as<OpCheckType>();
            return typeOf(bytecode.m_value) & typeAcceptedByMask(bytecode.m_mask);
        }
        case op_type_tag:
            if (Options::auditAOTTypedFields()) [[unlikely]]
                return node->uses[0].node->type;
            if (!node->isTrusted)
                return node->uses[0].node->type;
            if (node->isEdge)
                return node->uses[0].node->type & (~TAnyObject | objectTypeForLayout(node->firstLayout));
            return node->uses[0].node->type & objectTypeForLayout(node->firstLayout);
        case op_urshift:
            return TInt32;
        case op_unsigned:
            return TNumber;
        case op_bitnot: {
            Type operand = typeOf(node->as<OpBitnot>().m_operand);
            return bitResult(operand, operand);
        }
        case op_negate:
            return unaryArithResult(typeOf(node->as<OpNegate>().m_operand));
        case op_inc:
            return unaryArithResult(typeOf(node->as<OpInc>().m_srcDst));
        case op_dec:
            return unaryArithResult(typeOf(node->as<OpDec>().m_srcDst));
        case op_to_number: {
            Type operand = typeOf(node->as<OpToNumber>().m_operand);
            if (!operand)
                return TNone;
            if (isSubtype(operand, TNumber))
                return operand;
            return TNumber;
        }
        case op_to_numeric: {
            Type operand = typeOf(node->as<OpToNumeric>().m_operand);
            if (!operand)
                return TNone;
            if (isSubtype(operand, TNumber | TBigInt))
                return operand;
            return unaryArithResult(operand);
        }
        case op_typeof:
            return TAtomString;
        case op_to_string: {
            Type operand = typeOf(node->as<OpToString>().m_operand);
            if (!operand)
                return TNone;
            return isSubtype(operand, TString) ? operand : TString;
        }
        case op_strcat:
            return TString;
        case op_to_primitive: {
            Type operand = typeOf(node->as<OpToPrimitive>().m_src);
            if (!operand)
                return TNone;
            if (isSubtype(operand, TPrimitive))
                return operand;
            return TPrimitive;
        }
        case op_to_property_key:
            return TString | TSymbol;
        case op_to_property_key_or_number:
            return TString | TSymbol | TNumber;

        case op_eq:
        case op_neq:
        case op_stricteq:
        case op_nstricteq:
        case op_less:
        case op_lesseq:
        case op_greater:
        case op_greatereq:
        case op_below:
        case op_beloweq:
        case op_eq_null:
        case op_neq_null:
        case op_not:
        case op_is_empty:
        case op_typeof_is_undefined:
        case op_typeof_is_object:
        case op_typeof_is_function:
        case op_is_undefined_or_null:
        case op_is_boolean:
        case op_is_number:
        case op_is_big_int:
        case op_is_object:
        case op_is_callable:
        case op_is_constructor:
        case op_is_cell_with_type:
        case op_has_structure_with_flags:
        case op_in_by_id:
        case op_in_by_val:
        case op_has_private_name:
        case op_has_private_brand:
        case op_del_by_id:
        case op_del_by_val:
        case op_enumerator_in_by_val:
        case op_enumerator_has_own_property:
            return TBoolean;

        case op_new_object:
            if (uint16_t layoutID = Graph::newObjectLayoutID(node))
                return objectTypeForLayout(layoutID);
            if (Options::useAOTTypedFields() && node->numberOfLiteralProperties) {
                if (auto shape = m_graph.literalShape(node); shape && shape->number) {
                    if (TypeTable::hasTypedFields())
                        return shape->layoutID ? objectTypeForLayout(shape->layoutID) : TFinalObject;
                    return objectTypeForLayout(shape->number);
                }
            }
            return TFinalObject;
        case op_get_by_id:
            if (node->onlyChecksConstantObject || node->slotInConstantObjectPlusOne)
                return TAll;
            if (auto type = returnValueTypeReadBy(node))
                return *type;
            if (TypeTable::typedFieldsAreEnforced() && Options::useAOTTypedFields()) {
                if (uint32_t method = Graph::closedMethodReadBy(node))
                    return typeOf(node->as<OpGetById>().m_base) ? functionType(method) : TNone;
                if (auto field = Graph::typedFieldAccessedBy(node); field && field->fieldType.isConstrained())
                    return typeOf(node->as<OpGetById>().m_base) ? field->fieldType.type() | (field->isOptional ? TUndefined : TNone) : TNone;
            }
            if (Options::useAOTTypedFields() && !Options::auditAOTTypedFields() && !node->guard && TypeTable::shared()) {
                if (uint32_t tag = Graph::typeTagOf(node)) {
                    if (auto field = TypeTable::shared()->fieldOf(tag, node->graph->codeBlock()->identifier(node->as<OpGetById>().m_property).impl()); field && field->fieldType.isConstrained()) {
                        if (!typeOf(node->as<OpGetById>().m_base))
                            return TNone;
                        if (TypeTable::hasTypedFields())
                            return field->fieldType.type() | (field->isOptional ? TUndefined : TNone);
                        return field->fieldType.kindsOnly().type();
                    }
                }
            }
            if (readsSizeOfMapOrSet(node)) {
                m_graph.remark("typed-size-of-map-or-set"_s);
                return TNumber;
            }
            if (!node->guard) {
                Type base = typeOf(node->as<OpGetById>().m_base) & ~(TOther | TEmpty);
                if (!base)
                    return TNone;
                const StringImpl& name = *node->graph->codeBlock()->identifier(node->as<OpGetById>().m_property).impl();
                if (unsigned method = intrinsicFoundOnPrimitive(base, name); method && ImmutableIntrinsics::shared()->at(method).type == JSFunctionType)
                    return intrinsicFunctionType(method);
                if (uint32_t function = isSubtype(base, TFunction) ? intrinsicFunctionOf(base) : 0) {
                    if (unsigned member = intrinsicInheritedByFunction(function, name))
                        return intrinsicFunctionType(member);
                }
            }
            return typeOf(node->as<OpGetById>().m_base) ? TAll : TNone;
        case op_new_reg_exp:
        case op_new_reg_exp_shared:
            return TRegExp;
        case op_new_promise:
        case op_create_promise:
            return TPromise;
        case op_create_this:
            if (uint16_t layoutID = node->graph->thisLayoutID())
                return objectTypeForLayout(layoutID);
            m_graph.remark("create-this-is-final-object"_s);
            return TFinalObject;
        case op_create_direct_arguments:
        case op_create_scoped_arguments:
        case op_create_cloned_arguments:
        case op_new_generator:
        case op_new_async_function_generator:
        case op_create_generator:
        case op_create_async_generator:
        case op_get_scope:
        case op_get_parent_scope:
        case op_create_lexical_environment:
        case op_create_generator_frame_environment:
        case op_push_with_scope:
            return TObject;
        case op_resolve_scope: {
            auto bytecode = node->as<OpResolveScope>();
            if (isStaticClosureVarResolveType(bytecode.m_resolveType))
                return TObject;
            auto variable = node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
            bool mayPassWithScope = variable.kind == Graph::StaticVariable::Dynamic || (variable.kind == Graph::StaticVariable::Unresolved && !variable.isGlobal && node->graph->codeBlock()->codeType() == FunctionCode);
            return mayPassWithScope ? TAnyObject : TObject;
        }
        case op_construct:
            if (Node* callee = node->use(node->as<OpConstruct>().m_callee); callee->kind == NodeKind::Intrinsic) {
                if (auto result = constructingIntrinsicResult(callee->intrinsic))
                    return *result;
            }
            return TAnyObject;
        case op_to_object: {
            Type operand = typeOf(node->as<OpToObject>().m_operand);
            if (!operand)
                return TNone;
            return isSubtype(operand, TAnyObject) ? operand : TAnyObject;
        }
        case op_super_construct:
        case op_super_construct_varargs:
            if (uint16_t layoutID = node->graph->thisLayoutID())
                return objectTypeForLayout(layoutID);
            return TAnyObject;
        case op_construct_varargs:
            return TAnyObject;
        case op_new_array:
        case op_new_array_buffer:
        case op_new_array_with_size:
        case op_new_array_with_spread:
        case op_create_rest:
            return TArray;
        case op_new_array_with_species: {
            Node* array = node->use(node->as<OpNewArrayWithSpecies>().m_array);
            while (array->isBytecode(op_to_this) || array->isBytecode(op_to_object) || (array->kind == NodeKind::Narrow && array->graph == node->graph))
                array = array->uses[0].node;
            if (isUntouchedReceiverOfInlinedCall(node, array)) {
                m_graph.remark("plain-array-for-original-receiver"_s);
                return array->type ? TArray : TNone;
            }
            return TAnyObject;
        }
        case op_new_func:
            return closureTypeFor(node->graph->codeBlock()->functionDecl(node->as<OpNewFunc>().m_functionDecl));
        case op_new_func_exp:
            return closureTypeFor(node->graph->codeBlock()->functionExpr(node->as<OpNewFuncExp>().m_functionDecl));
        case op_new_generator_func:
            return closureTypeFor(node->graph->codeBlock()->functionDecl(node->as<OpNewGeneratorFunc>().m_functionDecl));
        case op_new_generator_func_exp:
            return closureTypeFor(node->graph->codeBlock()->functionExpr(node->as<OpNewGeneratorFuncExp>().m_functionDecl));
        case op_new_async_func:
            return closureTypeFor(node->graph->codeBlock()->functionDecl(node->as<OpNewAsyncFunc>().m_functionDecl));
        case op_new_async_func_exp:
            return closureTypeFor(node->graph->codeBlock()->functionExpr(node->as<OpNewAsyncFuncExp>().m_functionDecl));
        case op_new_async_generator_func:
            return closureTypeFor(node->graph->codeBlock()->functionDecl(node->as<OpNewAsyncGeneratorFunc>().m_functionDecl));
        case op_new_async_generator_func_exp:
            return closureTypeFor(node->graph->codeBlock()->functionExpr(node->as<OpNewAsyncGeneratorFuncExp>().m_functionDecl));
        case op_spread:
        case op_get_property_enumerator:
            return TCellOther;
        case op_argument_count:
            return TInt32;
        case op_get_by_val:
            {
                auto bytecode = node->as<OpGetByVal>();
                if (!typeOf(bytecode.m_base) || !typeOf(bytecode.m_property))
                    return TNone;
                if (auto type = Graph::typedArrayAccessed(node)) {
                    Type element = *type == Float32ArrayType || *type == Float64ArrayType || *type == Uint32ArrayType ? TNumber : TInt32;
                    if (node->guard)
                        return element;
                    if (isSubtype(typeOf(bytecode.m_property), TNumber))
                        return element | TUndefined;
                }
                if (node->guard && isSubtype(typeOf(bytecode.m_base), TArray)) {
                    Node* array = node->use(bytecode.m_base);
                    if (Graph::isLocallyAllocatedArray(array)) {
                        Type elements = m_elementTypes.get(array);
                        if (array->opcode == op_new_array) {
                            for (auto& use : array->uses)
                                elements |= use.node->type;
                        }
                        if (!elements && !m_treatsEmptyArraysAsUntyped)
                            return TNone;
                        if (elements && isSubtype(elements, TNumber))
                            return TNumber;
                    }
                    if (node->expectedMask && isSubtype(typeAcceptedByMask(node->expectedMask), TNumber | TOther) && (node->expectedMask & MaskNumber))
                        return TNumber;
                }
            }
            return node->graph->readsElementsOrEmpty ? TAll | TEmpty : TAll;
        case op_call:
            if (node->guard) {
                switch (m_graph.callIntrinsic(node)) {
                case CallIntrinsic::None:
                    break;
                case CallIntrinsic::MathIMul:
                case CallIntrinsic::StringCharCodeAt:
                case CallIntrinsic::ArrayPush:
                    return TInt32;
                default:
                    return TNumber;
                }
            }
            return callResult(node);
        case op_tail_call:
            return callResult(node);
        case op_get_length: {
            Type base = typeOf(node->as<OpGetLength>().m_base) & ~(TOther | TEmpty);
            if (!base)
                return TNone;
            if (isSubtype(base, TString) || node->guard)
                return TInt32;
            if (isSubtype(base, TString | TArray))
                return TNumber;
            return TTop;
        }
        case op_to_this: {
            auto bytecode = node->as<OpToThis>();
            Type operand = typeOf(bytecode.m_srcDst);
            if (!operand)
                return TNone;
            if (bytecode.m_ecmaMode.isStrict())
                return operand | (mayBe(operand, TOtherObject) ? TUndefined : TNone);
            return isSubtype(operand, TAnyObject & ~TOtherObject) ? operand : TAnyObject;
        }
        case op_iterator_close_check: {
            Type iterator = typeOf(node->as<OpIteratorCloseCheck>().m_iterator);
            return iterator ? iterator | TObject : TNone;
        }
        case op_resolve_scope_for_hoisting_func_decl_in_eval:
            return TObject | TUndefined;
        case op_get_argument: {
            unsigned index = node->as<OpGetArgument>().m_index;
            if (!node->graph->isOutermost() || node->graph->convention().signature != Signature::Registers || index > node->graph->convention().numberOfParameters)
                return TTop;
            return node->graph->argumentTypeOnEntry(index);
        }
        case op_get_from_scope: {
            bool isExact = false;
            if (const KnownFunction* known = m_graph.knownFunctionReadBy(node, &isExact); known && isExact)
                return known->isDeclaration ? closureTypeFor(known->executable) : closureTypeFor(known->executable) | TUndefined | TEmpty;
            if (VariableSummaries* summaries = m_graph.variableSummaries()) {
                if (Variable variable = m_graph.variableAccessedBy(node)) {
                    auto bytecode = node->as<OpGetFromScope>();
                    Type type = summaries->read(variable, node->graph->codeBlock()->identifier(bytecode.m_var).impl(), m_graph.summaryReader());
                    if (node->isNeverEmpty)
                        type &= ~TEmpty;
                    return bytecode.m_getPutInfo.resolveType() == ResolvedLazyClosureVar ? type | TFunction : type;
                }
            }
            return TAll;
        }
        default:
            return TAll;
        }
    }

    Graph& m_graph;
    Type m_returnType { TNone };
    UncheckedKeyHashMap<Node*, Type> m_elementTypes;
    UncheckedKeyHashMap<Node*, Vector<Node*, 4>> m_aliasesOfObjectsKeptToThemselves;
    bool m_elementTypesChanged { false };
    bool m_treatsEmptyArraysAsUntyped { false };
};

} // anonymous namespace

void noteBuiltinsExtended(Graph& graph)
{
    ProgramClasses* classes = programClasses();
    if (!classes)
        return;
    forEachBuiltinExtendedIn(graph, [&](Type builtins) {
        classes->noteBuiltinsExtended(builtins);
    });
}

Type inferTypes(Graph& graph, Vector<const KnownFunction*>* calleesRead, Vector<const KnownFunction*>* calleesWithWidenedInputs)
{
    TypeInference inference(graph);
    inference.calleesRead = calleesRead;
    inference.calleesWithWidenedInputs = calleesWithWidenedInputs;
    inference.run();
    return inference.returnType();
}

std::optional<bool> isBranchTakenAccordingToTypes(Node* branch)
{
    auto tested = TypeInference::valueTestedBy(branch);
    if (!tested.value || tested.value->isElided || tested.value->wasInferredUnreachable)
        return std::nullopt;
    bool mayBeTaken = mayBe(tested.value->type, tested.ifTrue);
    if (mayBeTaken == mayBe(tested.value->type, tested.ifFalse))
        return std::nullopt;
    return mayBeTaken;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
