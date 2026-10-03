/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(AOT)

#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

class LoopOptimizer {
public:
    LoopOptimizer(Graph& graph)
        : m_graph(graph)
    {
    }

    void run()
    {
        m_graph.computeDominators();
        for (BasicBlock* block : m_graph.m_rpo) {
            if (block->isPreHeader)
                optimize(block);
        }
    }

    void hoistArrayStorageLoads()
    {
        m_graph.computeDominators();
        for (BasicBlock* header : m_graph.m_rpo) {
            if (!header->graph->loopSplittingIsDisabled || header->isGeneric || header->isCatchEntrypoint)
                continue;
            Loop loop;
            loop.header = header;
            for (BasicBlock* predecessor : header->predecessors) {
                if (header->dominates(predecessor))
                    loop.latches.append(predecessor);
            }
            if (loop.latches.isEmpty() || loop.latches.size() == header->predecessors.size())
                continue;
            loop.body.ensureSize(m_graph.blocks.size());
            loop.body.set(header->index);
            Vector<BasicBlock*, 16> worklist;
            for (BasicBlock* latch : loop.latches)
                worklist.append(latch);
            bool isProper = true;
            while (!worklist.isEmpty()) {
                BasicBlock* block = worklist.takeLast();
                if (loop.body.get(block->index))
                    continue;
                isProper &= header->dominates(block) && !block->isGeneric && !block->endsWithGuard && !block->isCatchEntrypoint;
                loop.body.set(block->index);
                for (BasicBlock* predecessor : block->predecessors)
                    worklist.append(predecessor);
            }
            if (!isProper)
                continue;
            m_isUnsplit = true;
            bool isKnown = true;
            for (BasicBlock* block : m_graph.m_rpo) {
                if (!loop.body.get(block->index))
                    continue;
                loop.blocks.append(block);
                for (Node* node : block->nodes)
                    isKnown = isKnown && noteEffects(loop, node);
            }
            m_isUnsplit = false;
            if (!isKnown || loop.writesIndexed)
                continue;
            for (BasicBlock* block : loop.blocks) {
                for (Node* node : block->nodes) {
                    if (node->kind != NodeKind::Bytecode || node->viewedAheadOf || node->guard)
                        continue;
                    Node* base = nullptr;
                    if (node->opcode == op_get_by_val && node->use(node->as<OpGetByVal>().m_property)->isInteger())
                        base = node->use(node->as<OpGetByVal>().m_base);
                    else if (node->opcode == op_get_length)
                        base = node->use(node->as<OpGetLength>().m_base);
                    if (!base || !base->type || !isSubtype(base->type, TArray))
                        continue;
                    base = invariantValueOf(loop, base);
                    if (!base || !base->type || !isSubtype(base->type, TArray))
                        continue;
                    node->viewedAheadOf = header;
                    node->arrayViewed = base;
                    if (!header->arraysViewed.contains(base))
                        header->arraysViewed.append(base);
                }
            }
            if (!header->arraysViewed.isEmpty())
                header->loopBody = loop.body;
        }
    }

private:
    struct Loop {
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        Vector<BasicBlock*, 2> latches;
        BitVector body;
        Vector<BasicBlock*> blocks;

        bool writesIndexed { false };
        bool changesStructures { false };
        bool writesVariables { false };
        Vector<unsigned, 8> propertiesWritten;
    };

    bool findLoop(BasicBlock* preHeader, Loop& loop)
    {
        loop.preHeader = preHeader;
        loop.header = preHeader->successors[0];
        for (BasicBlock* predecessor : loop.header->predecessors) {
            if (predecessor == preHeader)
                continue;
            if (!loop.header->dominates(predecessor))
                return false;
            loop.latches.append(predecessor);
        }
        if (loop.latches.isEmpty())
            return false;
        loop.body.ensureSize(m_graph.blocks.size());
        loop.body.set(loop.header->index);
        Vector<BasicBlock*, 16> worklist;
        for (BasicBlock* latch : loop.latches)
            worklist.append(latch);
        while (!worklist.isEmpty()) {
            BasicBlock* block = worklist.takeLast();
            if (loop.body.get(block->index))
                continue;
            loop.body.set(block->index);
            for (BasicBlock* predecessor : block->predecessors)
                worklist.append(predecessor);
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            if (loop.body.get(block->index))
                loop.blocks.append(block);
        }
        return true;
    }

    static uint32_t putFlags(const OpPutById& bytecode) { return (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0); }

    bool applies(Node* guard)
    {
        switch (guard->opcode) {
        case op_get_by_id:
            return Site::fits(guard->as<OpGetById>().m_property, 0);
        case op_put_by_id:
            return Site::fits(guard->as<OpPutById>().m_property, putFlags(guard->as<OpPutById>()));
        case op_get_by_val:
        case op_put_by_val:
        case op_get_length:
        case op_check_type:
        case op_check_traps:
            return true;
        case op_call:
        case op_call_ignore_result:
            return m_graph.callIntrinsic(guard) != CallIntrinsic::None;
        case op_resolve_scope: {
            auto bytecode = guard->as<OpResolveScope>();
            if (isStaticClosureVarResolveType(bytecode.m_resolveType))
                return true;
            auto variable = guard->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
            if (variable.kind == Graph::StaticVariable::Unresolved)
                return Site::fits(bytecode.m_var, guard->graph->resolveScopeExtra(bytecode));
            return variable.kind != Graph::StaticVariable::Dynamic;
        }
        case op_get_from_scope: {
            auto bytecode = guard->as<OpGetFromScope>();
            ResolveType type = bytecode.m_getPutInfo.resolveType();
            if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar)
                return true;
            return Site::fits(bytecode.m_var, 1) && guard->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type).kind != Graph::StaticVariable::Dynamic;
        }
        default:
            return false;
        }
    }

    static bool isNumber(Node* node) { return isSubtype(node->type, TNumber); }

    bool noteEffects(Loop& loop, Node* node)
    {
        switch (node->kind) {
        case NodeKind::Constant:
        case NodeKind::ConstantCell:
        case NodeKind::Intrinsic:
        case NodeKind::LinkTimeConstant:
        case NodeKind::Argument:
        case NodeKind::Phi:
        case NodeKind::Proj:
        case NodeKind::GetStack:
        case NodeKind::SetStack:
        case NodeKind::Narrow:
            return true;
        case NodeKind::Guard:
            if (node->guardKind != GuardKind::Whole)
                return true;
            if (!applies(node))
                return false;
            if (node->opcode == op_put_by_id)
                loop.propertiesWritten.append(node->as<OpPutById>().m_property);
            else if (node->opcode == op_put_by_val || m_graph.callIntrinsic(node) == CallIntrinsic::ArrayPush)
                loop.writesIndexed = true;
            return true;
        case NodeKind::Bytecode:
            break;
        }
        if (node->guard)
            return true;

        if (m_isUnsplit) {
            switch (node->opcode) {
            case op_get_by_val:
                if (node->use(node->as<OpGetByVal>().m_base)->type && isSubtype(node->use(node->as<OpGetByVal>().m_base)->type, TArray) && isNumber(node->use(node->as<OpGetByVal>().m_property)))
                    return true;
                break;
            case op_get_length:
                if (node->use(node->as<OpGetLength>().m_base)->type && isSubtype(node->use(node->as<OpGetLength>().m_base)->type, TArray | TString))
                    return true;
                break;
            case op_get_by_id:
                if (Graph::isArrayIteratorMethodRead(node) && !Graph::methodMayBeOverridden("Array"_s, node))
                    return true;
                break;
            case op_check_type:
            case op_check_traps:
            case op_stricteq:
            case op_nstricteq:
            case op_jstricteq:
            case op_jnstricteq:
                return true;
            default:
                break;
            }
        }

        auto bothNumbers = [&](VirtualRegister lhs, VirtualRegister rhs) { return isNumber(node->use(lhs)) && isNumber(node->use(rhs)); };
        auto bothAreBitOperands = [&](VirtualRegister lhs, VirtualRegister rhs) { return isSubtype(node->use(lhs)->type | node->use(rhs)->type, TNumber | TBoolean); };
        auto equalityIsSimple = [&](bool strict, VirtualRegister lhs, VirtualRegister rhs) {
            Type left = node->use(lhs)->type;
            Type right = node->use(rhs)->type;
            if (isSubtype(left | right, TNumber) || isSubtype(left | right, TBoolean))
                return true;
            constexpr Type byContent = TNumber | TString | TBigInt;
            if (strict)
                return !mayBe(left, byContent) || !mayBe(right, byContent) || !mayBe(left, TString | TBigInt) || !mayBe(right, TString | TBigInt);
            return isSubtype(left | right, TAnyObject | TSymbol);
        };
        if (node->opcode == op_type_tag) {
            loop.changesStructures |= !node->uses[0].node->hasLayoutInRange(node->firstLayout, node->lastLayout);
            return true;
        }
        if (auto field = Graph::typedFieldAccessedBy(node); field && !field->id && node->use(node->opcode == op_get_by_id ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base)->hasLayoutInRange(field->first, field->last)) {
            bool isRead = node->opcode == op_get_by_id;
            if (isRead)
                return true;
            loop.propertiesWritten.append(node->as<OpPutById>().m_property);
            loop.changesStructures = true;
            return true;
        }
        switch (node->opcode) {
        case op_loop_hint:
        case op_jmp:
        case op_check_tdz:
        case op_get_scope:
        case op_get_parent_scope:
        case op_argument_count:
        case op_get_argument:
        case op_unsigned:
        case op_is_empty:
        case op_is_undefined_or_null:
        case op_is_boolean:
        case op_is_number:
        case op_is_object:
        case op_is_big_int:
        case op_is_cell_with_type:
        case op_eq_null:
        case op_neq_null:
        case op_typeof_is_undefined:
        case op_jeq_null:
        case op_jneq_null:
        case op_jundefined_or_null:
        case op_jnundefined_or_null:
        case op_jeq_ptr:
        case op_jneq_ptr:
        case op_jbelow:
        case op_jbeloweq:
        case op_below:
        case op_beloweq:
        case op_get_internal_field:
        case op_ret:
            return true;

#define AOT_BINARY(Struct, opcodeName, test) \
        case opcodeName: { \
            auto bytecode = node->as<Struct>(); \
            return test(bytecode.m_lhs, bytecode.m_rhs); \
        }
        AOT_BINARY(OpAdd, op_add, bothNumbers)
        AOT_BINARY(OpSub, op_sub, bothNumbers)
        AOT_BINARY(OpMul, op_mul, bothNumbers)
        AOT_BINARY(OpDiv, op_div, bothNumbers)
        AOT_BINARY(OpMod, op_mod, bothNumbers)
        AOT_BINARY(OpPow, op_pow, bothNumbers)
        AOT_BINARY(OpBitand, op_bitand, bothAreBitOperands)
        AOT_BINARY(OpBitor, op_bitor, bothAreBitOperands)
        AOT_BINARY(OpBitxor, op_bitxor, bothAreBitOperands)
        AOT_BINARY(OpLshift, op_lshift, bothAreBitOperands)
        AOT_BINARY(OpRshift, op_rshift, bothAreBitOperands)
        AOT_BINARY(OpUrshift, op_urshift, bothAreBitOperands)
        AOT_BINARY(OpLess, op_less, bothNumbers)
        AOT_BINARY(OpLesseq, op_lesseq, bothNumbers)
        AOT_BINARY(OpGreater, op_greater, bothNumbers)
        AOT_BINARY(OpGreatereq, op_greatereq, bothNumbers)
        AOT_BINARY(OpJless, op_jless, bothNumbers)
        AOT_BINARY(OpJlesseq, op_jlesseq, bothNumbers)
        AOT_BINARY(OpJgreater, op_jgreater, bothNumbers)
        AOT_BINARY(OpJgreatereq, op_jgreatereq, bothNumbers)
        AOT_BINARY(OpJnless, op_jnless, bothNumbers)
        AOT_BINARY(OpJnlesseq, op_jnlesseq, bothNumbers)
        AOT_BINARY(OpJngreater, op_jngreater, bothNumbers)
        AOT_BINARY(OpJngreatereq, op_jngreatereq, bothNumbers)
#undef AOT_BINARY
#define AOT_EQUALITY(Struct, opcodeName, strict) \
        case opcodeName: { \
            auto bytecode = node->as<Struct>(); \
            return equalityIsSimple(strict, bytecode.m_lhs, bytecode.m_rhs); \
        }
        AOT_EQUALITY(OpEq, op_eq, false)
        AOT_EQUALITY(OpNeq, op_neq, false)
        AOT_EQUALITY(OpStricteq, op_stricteq, true)
        AOT_EQUALITY(OpNstricteq, op_nstricteq, true)
        AOT_EQUALITY(OpJeq, op_jeq, false)
        AOT_EQUALITY(OpJneq, op_jneq, false)
        AOT_EQUALITY(OpJstricteq, op_jstricteq, true)
        AOT_EQUALITY(OpJnstricteq, op_jnstricteq, true)
#undef AOT_EQUALITY

        case op_inc:
            return isNumber(node->use(node->as<OpInc>().m_srcDst));
        case op_dec:
            return isNumber(node->use(node->as<OpDec>().m_srcDst));
        case op_negate:
            return isNumber(node->use(node->as<OpNegate>().m_operand));
        case op_bitnot:
            return isNumber(node->use(node->as<OpBitnot>().m_operand));
        case op_to_number:
            return isNumber(node->use(node->as<OpToNumber>().m_operand));
        case op_to_numeric:
            return isNumber(node->use(node->as<OpToNumeric>().m_operand));
        case op_not:
        case op_jtrue:
        case op_jfalse:
            return true;

        case op_resolve_scope:
            return isStaticClosureVarResolveType(node->as<OpResolveScope>().m_resolveType);
        case op_get_from_scope:
            return node->as<OpGetFromScope>().m_getPutInfo.resolveType() == ResolvedClosureVar;
        case op_put_to_scope:
            loop.writesVariables = true;
            return node->as<OpPutToScope>().m_getPutInfo.resolveType() == ResolvedClosureVar;
        default:
            return false;
        }
    }

    bool isInvariant(const Loop& loop, Node* node)
    {
        if (!node->block)
            return true;
        return !loop.body.get(node->block->index);
    }

    static bool isOperandAlias(Node* node)
    {
        return node->kind == NodeKind::Narrow || node->isBytecode(op_type_tag) || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz);
    }

    Node* invariantValueOf(const Loop& loop, Node* node)
    {
        for (unsigned steps = 0; steps < 64; ++steps) {
            if (isInvariant(loop, node))
                return node;
            if (isOperandAlias(node)) {
                node = node->uses[0].node;
                continue;
            }
            if (node->kind != NodeKind::Phi || node->block != loop.header || node->uses.size() != loop.header->predecessors.size())
                return nullptr;
            Node* valueOnEntry = nullptr;
            for (unsigned i = 0; i < node->uses.size(); ++i) {
                Node* input = node->uses[i].node;
                if (!loop.body.get(loop.header->predecessors[i]->index)) {
                    if (valueOnEntry && valueOnEntry != input)
                        return nullptr;
                    valueOnEntry = input;
                    continue;
                }
                for (unsigned inner = 0; input != node; ++inner) {
                    if (inner == 64 || !isOperandAlias(input))
                        return nullptr;
                    input = input->uses[0].node;
                }
            }
            if (!valueOnEntry)
                return nullptr;
            node = valueOnEntry;
        }
        return nullptr;
    }

    bool inputsAreInvariant(const Loop& loop, Node* node)
    {
        for (auto& use : node->uses) {
            if (!isInvariant(loop, use.node))
                return false;
        }
        return true;
    }

    bool isInvariantGuard(const Loop& loop, Node* guard)
    {
        switch (guard->opcode) {
        case op_get_by_id:
            return !loop.propertiesWritten.contains(guard->as<OpGetById>().m_property) && !loop.changesStructures;
        case op_get_by_val:
        case op_get_length:
            return !loop.writesIndexed;
        case op_check_type:
        case op_resolve_scope:
            return true;
        case op_call:
            return hasNoEffects(m_graph.callIntrinsic(guard));
        case op_get_from_scope:
            return !loop.writesVariables && !loop.propertiesWritten.contains(guard->as<OpGetFromScope>().m_var);
        default:
            return false;
        }
    }

    bool executesOnEveryIteration(const Loop& loop, BasicBlock* block)
    {
        for (BasicBlock* latch : loop.latches) {
            if (!block->dominates(latch))
                return false;
        }
        return true;
    }

    void addToPreHeader(Loop& loop, Node* node)
    {
        auto& nodes = loop.preHeader->nodes;
        node->block = loop.preHeader;
        nodes.insert(nodes.size() - 1, node);
    }

    Node* addGuardToPreHeader(Loop& loop, GuardKind kind, Node* like)
    {
        Node* guard = m_graph.addNode(NodeKind::Guard);
        guard->guardKind = kind;
        guard->opcode = like->opcode;
        guard->instruction = like->instruction;
        guard->bytecodeIndex = like->bytecodeIndex;
        guard->type = TAll;
        addToPreHeader(loop, guard);
        return guard;
    }

    static Node* baseOf(Node* guard)
    {
        if (guard->opcode == op_get_by_id)
            return guard->use(guard->as<OpGetById>().m_base);
        return guard->use(guard->as<OpPutById>().m_base);
    }

    static Node* resolve(Node* node)
    {
        while (node->replacement)
            node = node->replacement;
        return node;
    }

    void eliminateRedundantGuards(Loop& loop)
    {
        struct Known {
            Node* guard;
            Node* instruction;
        };
        Vector<Known, 16> known;
        bool replacedAny = false;

        auto isAliasOf = [&](Node* guard, const Known& earlier) {
            if (guard->opcode != earlier.guard->opcode)
                return false;
            if (guard->opcode == op_get_by_id) {
                return guard->as<OpGetById>().m_property == earlier.guard->as<OpGetById>().m_property
                    && resolve(guard->use(guard->as<OpGetById>().m_base)) == resolve(earlier.guard->use(earlier.guard->as<OpGetById>().m_base));
            }
            if (guard->opcode == op_get_by_val) {
                return resolve(guard->use(guard->as<OpGetByVal>().m_base)) == resolve(earlier.guard->use(earlier.guard->as<OpGetByVal>().m_base))
                    && resolve(guard->use(guard->as<OpGetByVal>().m_property)) == resolve(earlier.guard->use(earlier.guard->as<OpGetByVal>().m_property));
            }
            return guard->as<OpCheckType>().m_mask == earlier.guard->as<OpCheckType>().m_mask
                && resolve(guard->use(guard->as<OpCheckType>().m_value)) == resolve(earlier.guard->use(earlier.guard->as<OpCheckType>().m_value));
        };
        auto visit = [&](Node* guard, Node* instruction) {
            if (guard->guardKind != GuardKind::Whole)
                return false;
            if (guard->opcode == op_get_by_id) {
                if (!applies(guard) || loop.propertiesWritten.contains(guard->as<OpGetById>().m_property))
                    return false;
            } else if (guard->opcode == op_get_by_val) {
                if (loop.writesIndexed)
                    return false;
            } else if (guard->opcode != op_check_type)
                return false;
            for (auto& earlier : known) {
                if (isAliasOf(guard, earlier) && earlier.guard->block->dominates(guard->block)) {
                    if (guard->opcode == op_get_by_val)
                        m_graph.remark("reuses-guarded-element-read"_s);
                    instruction->replacement = earlier.instruction;
                    replacedAny = true;
                    return true;
                }
            }
            known.append({ guard, instruction });
            return false;
        };

        auto& ahead = loop.preHeader->nodes;
        for (unsigned i = 0; i + 1 < ahead.size(); ++i) {
            Node* guard = ahead[i];
            Node* instruction = ahead[i + 1];
            if (guard->kind != NodeKind::Guard || instruction->guard != guard)
                continue;
            if (visit(guard, instruction)) {
                ahead.removeAt(i, 2);
                --i;
            }
        }
        for (BasicBlock* block : loop.blocks) {
            Node* guard = block->terminal();
            if (!guard || guard->kind != NodeKind::Guard)
                continue;
            BasicBlock* next = block->successors[0];
            Node* instruction = next->nodes.isEmpty() ? nullptr : next->nodes[0];
            if (!instruction || instruction->guard != guard)
                continue;
            if (visit(guard, instruction)) {
                guard->guardKind = GuardKind::Nothing;
                guard->uses.clear();
                next->nodes.removeAt(0);
            }
        }
        if (!replacedAny)
            return;
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis) {
                for (auto& use : phi->uses)
                    use.node = resolve(use.node);
            }
            for (Node* node : block->nodes) {
                for (auto& use : node->uses)
                    use.node = resolve(use.node);
            }
        }
    }

    void optimize(BasicBlock* preHeader)
    {
        Loop loop;
        if (!findLoop(preHeader, loop))
            return;
        for (BasicBlock* block : loop.blocks) {
            if (block->isGeneric)
                return;
            for (Node* node : block->nodes) {
                if (!noteEffects(loop, node))
                    return;
            }
        }

        for (BasicBlock* block : loop.blocks) {
            for (unsigned i = 0; i < block->nodes.size(); ++i) {
                Node* node = block->nodes[i];
                if (node->kind == NodeKind::Guard && node->guardKind == GuardKind::KnownCallee && isInvariant(loop, node->uses[0].node) && executesOnEveryIteration(loop, block)) {
                    block->nodes.removeAt(i--);
                    addToPreHeader(loop, node);
                }
            }

            Node* guard = block->terminal();
            if (!guard || guard->kind != NodeKind::Guard || guard->guardKind != GuardKind::Whole || !executesOnEveryIteration(loop, block))
                continue;
            BasicBlock* next = block->successors[0];
            Node* instruction = next->nodes.isEmpty() ? nullptr : next->nodes[0];
            if (!instruction || instruction->guard != guard)
                continue;
            if (inputsAreInvariant(loop, guard) && isInvariantGuard(loop, guard)) {
                Node* hoisted = addGuardToPreHeader(loop, GuardKind::Whole, guard);
                hoisted->uses = guard->uses;
                hoisted->guarded = instruction;
                guard->guardKind = GuardKind::Nothing;
                guard->uses.clear();
                instruction->guard = hoisted;
                next->nodes.removeAt(0);
                addToPreHeader(loop, instruction);
                continue;
            }
            if ((guard->opcode == op_call || guard->opcode == op_call_ignore_result) && isInvariant(loop, guard->use(Graph::callOperands(guard->instruction).callee))) {
                Node* hoisted = addGuardToPreHeader(loop, GuardKind::Callee, guard);
                hoisted->uses = guard->uses;
                hoisted->uses.removeAllMatching([&](Use& use) { return use.reg != Graph::callOperands(guard->instruction).callee; });
                guard->calleeIsChecked = true;
            }
        }

        eliminateRedundantGuards(loop);

        Vector<Node*, 4> storages;
        for (BasicBlock* block : loop.blocks) {
            Node* guard = block->terminal();
            if (!guard || guard->kind != NodeKind::Guard || guard->guardKind != GuardKind::Whole || !Graph::typedArrayAccessed(guard) || !executesOnEveryIteration(loop, block))
                continue;
            Node* base = guard->opcode == op_get_by_val ? guard->use(guard->as<OpGetByVal>().m_base) : guard->use(guard->as<OpPutByVal>().m_base);
            if (!isInvariant(loop, base))
                continue;
            for (Node* storage : storages) {
                if (storage->uses[0].node == base)
                    guard->storage = storage;
            }
            if (guard->storage)
                continue;
            Node* storage = addGuardToPreHeader(loop, GuardKind::TypedArrayStorage, guard);
            storage->uses.append({ VirtualRegister(), base });
            storages.append(storage);
            guard->storage = storage;
        }
        for (BasicBlock* block : loop.blocks) {
            Node* guard = block->terminal();
            if (!guard || guard->kind != NodeKind::Guard || guard->guardKind != GuardKind::Whole || guard->storage || !Graph::typedArrayAccessed(guard))
                continue;
            Node* base = guard->opcode == op_get_by_val ? guard->use(guard->as<OpGetByVal>().m_base) : guard->use(guard->as<OpPutByVal>().m_base);
            for (Node* storage : storages) {
                if (storage->uses[0].node == base)
                    guard->storage = storage;
            }
        }

        Vector<Node*, 4> optionalStorages;
        for (BasicBlock* block : loop.blocks) {
            Node* guard = block->terminal();
            if (!guard || guard->kind != NodeKind::Guard || guard->guardKind != GuardKind::Whole || guard->opcode != op_get_by_val || guard->storage || Graph::typedArrayAccessed(guard))
                continue;
            Node* base = guard->use(guard->as<OpGetByVal>().m_base);
            if (!isInvariant(loop, base) || !mayBe(base->type, typeForTypedArray(Uint8ArrayType)))
                continue;
            for (Node* storage : optionalStorages) {
                if (storage->uses[0].node == base)
                    guard->storage = storage;
            }
            if (guard->storage)
                continue;
            Node* storage = addGuardToPreHeader(loop, GuardKind::Uint8ArrayStorageIfAny, guard);
            storage->uses.append({ VirtualRegister(), base });
            optionalStorages.append(storage);
            guard->storage = storage;
        }

        Vector<std::pair<Node*, Vector<Node*, 4>>, 8> groups;
        for (BasicBlock* block : loop.blocks) {
            Node* guard = block->terminal();
            if (!guard || guard->kind != NodeKind::Guard || guard->guardKind != GuardKind::Whole || (guard->opcode != op_get_by_id && guard->opcode != op_put_by_id))
                continue;
            Node* base = baseOf(guard);
            bool found = false;
            for (auto& group : groups) {
                if (group.first == base) {
                    group.second.append(guard);
                    found = true;
                    break;
                }
            }
            if (!found)
                groups.append({ base, { guard } });
        }
        Vector<Node*, 16> slotChecks;
        auto addSlotCheck = [&](GuardKind kind, Node* site, Node* otherSite) {
            Node* check = m_graph.addNode(NodeKind::Guard);
            check->guardKind = kind;
            check->opcode = site->opcode;
            check->instruction = site->instruction;
            check->bytecodeIndex = site->bytecodeIndex;
            check->type = TAll;
            check->site = site;
            check->otherSite = otherSite;
            slotChecks.append(check);
        };
        for (auto& [base, guards] : groups) {
            for (Node* guard : guards) {
                if (executesOnEveryIteration(loop, guard->block)) {
                    addSlotCheck(GuardKind::SlotIsDirect, guard, nullptr);
                    guard->slotIsDirect = true;
                }
            }
            Node* leader = guards[0];
            bool checkedAhead = isInvariant(loop, base) && executesOnEveryIteration(loop, leader->block);
            if (checkedAhead) {
                Node* check = addGuardToPreHeader(loop, GuardKind::Structure, leader);
                check->uses.append({ VirtualRegister(), base });
                check->site = leader;
                leader->structureIsChecked = true;
            }
            for (unsigned i = 1; i < guards.size(); ++i) {
                Node* follower = guards[i];
                if (!checkedAhead && !leader->block->dominates(follower->block))
                    continue;
                addSlotCheck(GuardKind::SlotsAgree, leader, follower);
                follower->structureIsChecked = true;
            }
        }
        if (!slotChecks.isEmpty()) {
            addGuardToPreHeader(loop, GuardKind::BeginSlotChecks, slotChecks[0]);
            for (Node* check : slotChecks)
                addToPreHeader(loop, check);
            addGuardToPreHeader(loop, GuardKind::EndSlotChecks, slotChecks[0]);
        }
    }

    Graph& m_graph;
    bool m_isUnsplit { false };
};

} // anonymous namespace

void optimizeLoops(Graph& graph)
{
    LoopOptimizer optimizer(graph);
    if (graph.hasGuards())
        optimizer.run();
    optimizer.hoistArrayStorageLoads();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
