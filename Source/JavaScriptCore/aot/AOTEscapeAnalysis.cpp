/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(AOT)

#include "AOTBuiltins.h"
#include "AOTTypeTable.h"
#include "BytecodeStructs.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "RegExp.h"
#include "SymbolTable.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace AOT {

ASCIILiteral nameOf(Escape escape)
{
    switch (escape) {
    case Escape::NotAnalyzed:
        return "not looked at"_s;
    case Escape::StaysHere:
        return "STAYS: nothing but this code sees it"_s;
    case Escape::IsOnlyBorrowed:
        return "STAYS: lent to what gives it back"_s;
    case Escape::Returned:
        return "returned"_s;
    case Escape::Thrown:
        return "thrown"_s;
    case Escape::StoredInProperty:
        return "stored in a property or an element"_s;
    case Escape::StoredInLiteral:
        return "part of an object or an array that is made"_s;
    case Escape::StoredInVariable:
        return "stored in a variable of an environment"_s;
    case Escape::Merged:
        return "one of several things a variable may hold (phi)"_s;
    case Escape::Homed:
        return "in a variable that a handler reads"_s;
    case Escape::PassedToMethod:
        return "passed to what was read from a property"_s;
    case Escape::PassedAsThisToMethod:
        return "receiver of a call of what was read from it or another"_s;
    case Escape::PassedToBuiltin:
        return "passed to one of the language's own functions"_s;
    case Escape::PassedToLocalClosure:
        return "passed to a closure that is made here"_s;
    case Escape::PassedToParameter:
        return "passed to a function that was itself passed in"_s;
    case Escape::PassedToVariable:
        return "passed to what was read from a variable, not proven"_s;
    case Escape::PassedToUnknown:
        return "passed to something else"_s;
    case Escape::PassedToRetainingCallee:
        return "passed to a known function that lets it out"_s;
    case Escape::PassedInList:
        return "passed in a call that takes a list, or constructs"_s;
    case Escape::PassedInTailCall:
        return "passed in a tail call"_s;
    case Escape::Constructed:
        return "constructed with"_s;
    case Escape::StoredInEscapingObject:
        return "scope of a closure or environment that escapes"_s;
    case Escape::ClosureLetsScopeOut:
        return "scope of a closure whose code may let its scope out"_s;
    case Escape::Iterated:
        return "iterated or spread"_s;
    case Escape::Converted:
        return "converted, compared loosely or tested with instanceof"_s;
    case Escape::Suspended:
        return "yielded, awaited or kept by a generator"_s;
    case Escape::Other:
        return "used by something not reckoned with"_s;
    case Escape::NumberOfThem:
        break;
    }
    return "?"_s;
}

std::optional<AllocationKind> allocationKind(const Node* node)
{
    if (node->kind != NodeKind::Bytecode)
        return std::nullopt;
    switch (node->opcode) {
    case op_new_object:
        return AllocationKind::Object;
    case op_new_array:
    case op_new_array_with_size:
    case op_new_array_buffer:
    case op_new_array_with_spread:
    case op_create_rest:
        return AllocationKind::Array;
    case op_new_func:
    case op_new_func_exp:
        return AllocationKind::Closure;
    case op_create_lexical_environment:
        return AllocationKind::Environment;
    default:
        return std::nullopt;
    }
}

static bool doesNotLeakScope(UnlinkedCodeBlock* code)
{
    if (code->codeType() != FunctionCode)
        return false;
    if (code->parseMode() != SourceParseMode::NormalFunctionMode && code->parseMode() != SourceParseMode::ArrowFunctionMode && code->parseMode() != SourceParseMode::MethodMode)
        return false;
    for (const auto& instruction : code->instructions()) {
        switch (instruction->opcodeID()) {
        case op_new_func:
        case op_new_func_exp:
        case op_new_generator_func:
        case op_new_generator_func_exp:
        case op_new_async_func:
        case op_new_async_func_exp:
        case op_new_async_generator_func:
        case op_new_async_generator_func_exp:
        case op_call_direct_eval:
        case op_push_with_scope:
        case op_create_scoped_arguments:
        case op_create_generator_frame_environment:
            return false;
        default:
            break;
        }
    }
    return true;
}

bool mayReferenceItself(UnlinkedCodeBlock* code)
{
    if (readsCallee(code))
        return true;
    for (const auto& instruction : code->instructions()) {
        switch (instruction->opcodeID()) {
        case op_call_direct_eval:
        case op_create_direct_arguments:
        case op_create_scoped_arguments:
            return true;
        default:
            break;
        }
    }
    return false;
}

class EscapeAnalysis {
public:
    EscapeAnalysis(Graph& graph, Vector<const KnownFunction*>* calleesRead)
        : m_graph(graph)
        , m_calleesConsulted(calleesRead)
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis)
                noteUsesBy(phi);
            for (Node* node : block->nodes)
                noteUsesBy(node);
        }
    }

    void analyzeAllocations()
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (allocationKind(node))
                    node->escape = escapeOf(node, false);
            }
        }
    }

    uint32_t escapingParameters()
    {
        UnlinkedCodeBlock* code = m_graph.codeBlock();
        if (code->codeType() != FunctionCode || (code->parseMode() != SourceParseMode::NormalFunctionMode && code->parseMode() != SourceParseMode::ArrowFunctionMode && code->parseMode() != SourceParseMode::MethodMode))
            return std::numeric_limits<uint32_t>::max();
        uint32_t result = 0;
        for (const auto& instruction : code->instructions()) {
            switch (instruction->opcodeID()) {
            case op_call_direct_eval:
            case op_create_direct_arguments:
            case op_create_scoped_arguments:
            case op_create_cloned_arguments:
                return std::numeric_limits<uint32_t>::max();
            case op_create_rest:
                result |= FunctionSummary::extraArgumentsEscape;
                break;
            default:
                break;
            }
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                bool readsArgument = node->isBytecode(op_get_argument);
                if ((node->kind != NodeKind::Argument && !readsArgument) || node->graph != &m_graph)
                    continue;
                unsigned index = readsArgument ? node->as<OpGetArgument>().m_index : node->reg.toArgument();
                if (index >= FunctionSummary::maxTrackedEscapingParameters) {
                    result |= FunctionSummary::extraArgumentsEscape;
                    continue;
                }
                if (!stays(escapeOf(node, true)))
                    result |= 1u << index;
            }
        }
        return result;
    }

private:
    struct User {
        Node* node;
        VirtualRegister reg;
    };

    void noteUsesBy(Node* user)
    {
        for (auto& use : user->uses)
            m_users.add(use.node, Vector<User, 4> { }).iterator->value.append({ user, use.reg });
    }

    struct Verdict {
        enum Kind : uint8_t { Harmless, Aliases, Lent, RetainedByUser, Escapes } kind { Harmless };
        Escape why { Escape::Other };
    };
    static Verdict escapes(Escape why) { return { Verdict::Escapes, why }; }

    Escape escapeOf(Node* value, bool isParameter)
    {
        if (!isParameter) {
            if (auto it = m_escapes.find(value); it != m_escapes.end())
                return it->value;
            m_escapes.add(value, Escape::Other);
        }
        Escape result = Escape::StaysHere;
        if (!isParameter && allocationKind(value) == AllocationKind::Closure) {
            UnlinkedFunctionExecutable* executable = value->opcode == op_new_func ? value->graph->codeBlock()->functionDecl(value->as<OpNewFunc>().m_functionDecl) : value->graph->codeBlock()->functionExpr(value->as<OpNewFuncExp>().m_functionDecl);
            UnlinkedFunctionCodeBlock* code = executable->codeBlockIfExists(CodeSpecializationKind::CodeForCall);
            if (!code || mayReferenceItself(code))
                result = Escape::Other;
        }
        Vector<Node*, 8> worklist { value };
        UncheckedKeyHashSet<Node*> seen { value };
        while (!worklist.isEmpty() && stays(result)) {
            Node* same = worklist.takeLast();
            auto it = m_users.find(same);
            if (it == m_users.end())
                continue;
            for (auto& user : it->value) {
                Verdict verdict = escapeThroughUse(user.node, user.reg, value, isParameter);
                switch (verdict.kind) {
                case Verdict::Harmless:
                    continue;
                case Verdict::Aliases:
                    if (seen.add(user.node).isNewEntry)
                        worklist.append(user.node);
                    continue;
                case Verdict::Lent:
                    result = Escape::IsOnlyBorrowed;
                    continue;
                case Verdict::RetainedByUser: {
                    Escape holderEscape = escapeOf(user.node, false);
                    if (!stays(holderEscape)) {
                        result = Escape::StoredInEscapingObject;
                        break;
                    }
                    if (holderEscape == Escape::IsOnlyBorrowed)
                        result = Escape::IsOnlyBorrowed;
                    continue;
                }
                case Verdict::Escapes:
                    result = verdict.why;
                    break;
                }
                break;
            }
        }
        if (!isParameter)
            m_escapes.set(value, result);
        return result;
    }

    Verdict escapeThroughCall(Node* call, VirtualRegister reg, VirtualRegister calleeRegister, unsigned argc, unsigned argv, bool isTailCall, bool isParameter)
    {
        if (reg == calleeRegister)
            return { };
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        int index = reg.offset() - firstArgument;
        if (index < 0 || static_cast<unsigned>(index) >= argc)
            return escapes(Escape::Other);
        if (isTailCall && !isParameter)
            return escapes(Escape::PassedInTailCall);

        bool isExact = false;
        const KnownFunction* known = call->graph->knownCallee(call, &isExact);
        if (known && isExact && known->forCall && known->summary) {
            if (m_calleesConsulted && !m_calleesConsulted->contains(known))
                m_calleesConsulted->append(known);
            uint32_t mask = known->summary->escapingParameters.load(std::memory_order_relaxed);
            bool isTracked = static_cast<unsigned>(index) < FunctionSummary::maxTrackedEscapingParameters;
            bool isDeclared = static_cast<unsigned>(index) < known->forCall->numParameters();
            bool leaksValue = (isTracked && (mask >> index & 1)) || ((!isTracked || !isDeclared) && (mask & FunctionSummary::extraArgumentsEscape));
            if (leaksValue)
                return escapes(Escape::PassedToRetainingCallee);
            return { Verdict::Lent };
        }
        Node* callee = call->use(calleeRegister);
        if (callee->kind == NodeKind::Intrinsic)
            return escapes(Escape::PassedToBuiltin);
        if (callee->isBytecode(op_get_by_id) || callee->isBytecode(op_get_by_val))
            return escapes(index ? Escape::PassedToMethod : Escape::PassedAsThisToMethod);
        if (callee->isBytecode(op_new_func_exp) || callee->isBytecode(op_new_func))
            return escapes(Escape::PassedToLocalClosure);
        if (callee->kind == NodeKind::Argument)
            return escapes(Escape::PassedToParameter);
        if (callee->isBytecode(op_get_from_scope))
            return escapes(Escape::PassedToVariable);
        return escapes(Escape::PassedToUnknown);
    }

    Verdict escapeThroughUse(Node* user, VirtualRegister reg, Node* value, bool isParameter)
    {
        switch (user->kind) {
        case NodeKind::Phi:
        case NodeKind::Narrow:
            return escapes(Escape::Merged);
        case NodeKind::SetStack:
            return escapes(Escape::Homed);
        case NodeKind::Guard:
            return { };
        case NodeKind::Bytecode:
            break;
        default:
            return escapes(Escape::Other);
        }

        bool isLocallyAllocatedArray = !isParameter && allocationKind(value) == AllocationKind::Array;
        switch (user->opcode) {
        case op_get_by_id:
            return reg == user->as<OpGetById>().m_base ? Verdict { } : escapes(Escape::Other);
        case op_get_by_id_direct:
        case op_get_length:
        case op_in_by_id:
        case op_del_by_id:
        case op_get_prototype_of:
        case op_typeof:
        case op_typeof_is_undefined:
        case op_typeof_is_object:
        case op_typeof_is_function:
        case op_is_empty:
        case op_is_undefined_or_null:
        case op_is_boolean:
        case op_is_number:
        case op_is_big_int:
        case op_is_object:
        case op_is_callable:
        case op_is_constructor:
        case op_is_cell_with_type:
        case op_has_structure_with_flags:
        case op_eq_null:
        case op_neq_null:
        case op_not:
        case op_stricteq:
        case op_nstricteq:
        case op_jstricteq:
        case op_jnstricteq:
        case op_jtrue:
        case op_jfalse:
        case op_jeq_null:
        case op_jneq_null:
        case op_jundefined_or_null:
        case op_jnundefined_or_null:
        case op_jeq_ptr:
        case op_jneq_ptr:
        case op_check_tdz:
        case op_get_parent_scope:
        case op_get_from_scope:
        case op_set_function_name:
            return { };
        case op_get_by_val:
            return reg == user->as<OpGetByVal>().m_base ? Verdict { } : escapes(Escape::Converted);
        case op_in_by_val:
            return reg == user->as<OpInByVal>().m_base ? Verdict { } : escapes(Escape::Converted);
        case op_del_by_val:
            return reg == user->as<OpDelByVal>().m_base ? Verdict { } : escapes(Escape::Converted);

        case op_check_type:
        case op_type_tag:
        case op_mov:
        case op_to_this:
        case op_to_object:
        case op_identity_with_profile:
        case op_resolve_scope:
            return { Verdict::Aliases };

        case op_put_by_id: {
            auto bytecode = user->as<OpPutById>();
            if (reg == bytecode.m_value)
                return escapes(Escape::StoredInProperty);
            return user->graph->codeBlock()->identifier(bytecode.m_property) == m_graph.vm().propertyNames->underscoreProto ? escapes(Escape::Other) : Verdict { };
        }
        case op_put_by_val:
            return reg == user->as<OpPutByVal>().m_base ? Verdict { } : reg == user->as<OpPutByVal>().m_value ? escapes(Escape::StoredInProperty) : escapes(Escape::Converted);
        case op_put_by_val_direct:
            return reg == user->as<OpPutByValDirect>().m_base ? Verdict { } : reg == user->as<OpPutByValDirect>().m_value ? escapes(Escape::StoredInProperty) : escapes(Escape::Converted);
        case op_put_to_scope:
            return reg == user->as<OpPutToScope>().m_scope ? Verdict { } : escapes(Escape::StoredInVariable);
        case op_new_object:
        case op_new_array:
        case op_create_this:
            return escapes(Escape::StoredInLiteral);
        case op_put_getter_by_id:
        case op_put_setter_by_id:
        case op_put_getter_setter_by_id:
        case op_put_getter_by_val:
        case op_put_setter_by_val:
        case op_define_data_property:
        case op_define_accessor_property:
            return escapes(Escape::StoredInProperty);

        case op_create_lexical_environment:
            return reg == user->as<OpCreateLexicalEnvironment>().m_scope ? Verdict { Verdict::RetainedByUser } : escapes(Escape::Other);
        case op_new_func_exp:
        case op_new_func: {
            UnlinkedFunctionExecutable* executable = user->opcode == op_new_func ? user->graph->codeBlock()->functionDecl(user->as<OpNewFunc>().m_functionDecl) : user->graph->codeBlock()->functionExpr(user->as<OpNewFuncExp>().m_functionDecl);
            UnlinkedFunctionCodeBlock* code = executable->codeBlockIfExists(CodeSpecializationKind::CodeForCall);
            if (!code || !doesNotLeakScope(code))
                return escapes(Escape::ClosureLetsScopeOut);
            return { Verdict::RetainedByUser };
        }
        case op_new_generator_func:
        case op_new_generator_func_exp:
        case op_new_async_func:
        case op_new_async_func_exp:
        case op_new_async_generator_func:
        case op_new_async_generator_func_exp:
            return escapes(Escape::ClosureLetsScopeOut);

        case op_call: {
            auto bytecode = user->as<OpCall>();
            return escapeThroughCall(user, reg, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, isParameter);
        }
        case op_call_ignore_result: {
            auto bytecode = user->as<OpCallIgnoreResult>();
            return escapeThroughCall(user, reg, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, isParameter);
        }
        case op_tail_call: {
            auto bytecode = user->as<OpTailCall>();
            return escapeThroughCall(user, reg, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, user->graph->isInTailPosition, isParameter);
        }
        case op_construct:
            return escapes(reg == user->as<OpConstruct>().m_callee ? Escape::Constructed : Escape::PassedInList);
        case op_call_varargs:
        case op_tail_call_varargs:
        case op_construct_varargs:
        case op_super_construct:
        case op_super_construct_varargs:
        case op_call_direct_eval:
            return escapes(Escape::PassedInList);
        case op_ret:
            return escapes(Escape::Returned);
        case op_throw:
            return escapes(Escape::Thrown);
        case op_yield:
        case op_put_internal_field:
        case op_create_generator_frame_environment:
            return escapes(Escape::Suspended);

        case op_spread:
        case op_iterator_open:
        case op_iterator_next:
        case op_iterator_close_check:
        case op_new_array_with_spread:
            return isLocallyAllocatedArray && user->opcode == op_spread ? Verdict { } : escapes(Escape::Iterated);

        case op_eq:
        case op_neq:
        case op_jeq:
        case op_jneq:
        case op_instanceof:
        case op_to_primitive:
        case op_to_string:
        case op_to_number:
        case op_to_numeric:
        case op_to_property_key:
        case op_to_property_key_or_number:
        case op_strcat:
        case op_add:
            return escapes(Escape::Converted);
        default:
            return escapes(Escape::Other);
        }
    }

    Graph& m_graph;
    Vector<const KnownFunction*>* m_calleesConsulted;
    UncheckedKeyHashMap<Node*, Vector<User, 4>> m_users;
    UncheckedKeyHashMap<Node*, Escape> m_escapes;
};

namespace {

class Promoter {
public:
    Promoter(Graph& graph)
        : m_graph(graph)
    {
    }

    struct Where {
        Node* base { nullptr };
        unsigned hops { 0 };
        bool isLocalEnvironment() const { return base && !hops && base->isBytecode(op_create_lexical_environment); }
    };

    using PhisBeingLocated = Vector<Node*, 8>;

    static Where lexicallyOut(Where from, unsigned hops, unsigned depth, PhisBeingLocated* beingLocated)
    {
        while (hops && from.isLocalEnvironment()) {
            from = lexicalLocationOf(from.base->use(from.base->as<OpCreateLexicalEnvironment>().m_scope), depth + 1, beingLocated);
            --hops;
        }
        from.hops += hops;
        return from;
    }

    static Where out(Where from, unsigned hops, unsigned depth)
    {
        while (hops && from.isLocalEnvironment()) {
            from = locationOf(from.base->use(from.base->as<OpCreateLexicalEnvironment>().m_scope), depth + 1);
            --hops;
        }
        from.hops += hops;
        return from;
    }

    static Where locationOf(Node* scope, unsigned depth = 0) { return lexicalLocationOf(scope, depth, nullptr); }

    // Without a base: where the phis that are being located are.
    static Where lexicalLocationOf(Node* scope, unsigned depth, PhisBeingLocated* beingLocated)
    {
        if (depth <= 24 && (scope->kind == NodeKind::Phi || scope->kind == NodeKind::Narrow)) {
            if (beingLocated && beingLocated->contains(scope))
                return { nullptr, 0 };
            PhisBeingLocated own;
            PhisBeingLocated& phis = beingLocated ? *beingLocated : own;
            size_t numberBefore = phis.size();
            Vector<Node*, 8> worklist { scope };
            std::optional<Where> common;
            auto fail = [&] {
                phis.shrink(numberBefore);
                return Where { scope, 0 };
            };
            while (!worklist.isEmpty()) {
                Node* node = worklist.takeLast();
                if (node->kind == NodeKind::Phi || node->kind == NodeKind::Narrow) {
                    if (phis.contains(node))
                        continue;
                    phis.append(node);
                    for (auto& use : node->uses)
                        worklist.append(use.node);
                    continue;
                }
                Where where = lexicalLocationOf(node, depth + 1, &phis);
                if (!where.base) {
                    if (where.hops)
                        return fail();
                    continue;
                }
                if (common && (common->base != where.base || common->hops != where.hops))
                    return fail();
                common = where;
            }
            if (!common)
                return beingLocated ? Where { nullptr, 0 } : fail();
            if (common->base->kind != NodeKind::Bytecode)
                return fail();
            return *common;
        }
        if (depth > 24 || scope->kind != NodeKind::Bytecode)
            return { scope, 0 };
        switch (scope->opcode) {
        case op_get_scope:
            if (Node* closedOver = scope->graph->closureScope)
                return lexicalLocationOf(closedOver, depth + 1, beingLocated);
            return { scope, 0 };
        case op_get_parent_scope:
            return lexicallyOut(lexicalLocationOf(scope->use(scope->as<OpGetParentScope>().m_scope), depth + 1, beingLocated), 1, depth, beingLocated);
        case op_resolve_scope: {
            auto bytecode = scope->as<OpResolveScope>();
            if (scope->graph->resolvedEnvironmentDepth(scope))
                return { scope, 0 };
            unsigned hops;
            if (isStaticClosureVarResolveType(bytecode.m_resolveType))
                hops = bytecode.m_localScopeDepth + staticClosureVarHops(bytecode.m_resolveType);
            else {
                if (bytecode.m_resolveType == Dynamic)
                    return { scope, 0 };
                auto variable = scope->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
                if (!variable.isAtStaticDepth())
                    return { scope, 0 };
                hops = variable.depth;
            }
            return lexicallyOut(lexicalLocationOf(scope->use(bytecode.m_scope), depth + 1, beingLocated), hops, depth, beingLocated);
        }
        default:
            return { scope, 0 };
        }
    }

    static std::optional<unsigned> offsetAccessedBy(Node* node)
    {
        if (node->graph->accessedEnvironmentDepth(node))
            return std::nullopt;
        if (node->opcode == op_get_from_scope) {
            auto bytecode = node->as<OpGetFromScope>();
            ResolveType type = bytecode.m_getPutInfo.resolveType();
            if (type == ResolvedClosureVar)
                return bytecode.m_offset;
            if (type == ResolvedLazyClosureVar || type == Dynamic)
                return std::nullopt;
            auto variable = node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
            if (variable.kind == Graph::StaticVariable::Closure && !variable.inModule)
                return variable.offset.offset();
            return std::nullopt;
        }
        auto bytecode = node->as<OpPutToScope>();
        ResolveType type = bytecode.m_getPutInfo.resolveType();
        if (type == ResolvedClosureVar)
            return bytecode.m_offset;
        if (type == Dynamic)
            return std::nullopt;
        auto variable = node->graph->resolveStatically(bytecode.m_var, bytecode.m_symbolTableOrScopeDepth.scopeDepth(), type);
        if (variable.kind == Graph::StaticVariable::Closure && (!variable.isReadOnly || isInitialization(bytecode.m_getPutInfo.initializationMode())))
            return variable.offset.offset();
        return std::nullopt;
    }

    static std::optional<VirtualRegister> scopeOfNewFunction(Node* node)
    {
        if (node->kind != NodeKind::Bytecode)
            return std::nullopt;
        switch (node->opcode) {
        case op_new_func:
            return node->as<OpNewFunc>().m_scope;
        case op_new_func_exp:
            return node->as<OpNewFuncExp>().m_scope;
        case op_new_generator_func:
            return node->as<OpNewGeneratorFunc>().m_scope;
        case op_new_generator_func_exp:
            return node->as<OpNewGeneratorFuncExp>().m_scope;
        case op_new_async_func:
            return node->as<OpNewAsyncFunc>().m_scope;
        case op_new_async_func_exp:
            return node->as<OpNewAsyncFuncExp>().m_scope;
        case op_new_async_generator_func:
            return node->as<OpNewAsyncGeneratorFunc>().m_scope;
        case op_new_async_generator_func_exp:
            return node->as<OpNewAsyncGeneratorFuncExp>().m_scope;
        default:
            return std::nullopt;
        }
    }

    bool isDissolved(Node* environment)
    {
        VariableSummaries* summaries = m_graph.variableSummaries();
        return summaries && summaries->isDissolved(environment->graph->scopeIdentity(environment));
    }

    std::optional<VirtualRegister> scopeThatMustBeThere(Node* user)
    {
        if (auto scope = scopeOfNewFunction(user))
            return scope;
        if (user->isBytecode(op_create_lexical_environment) && !m_candidates.contains(user))
            return user->as<OpCreateLexicalEnvironment>().m_scope;
        return std::nullopt;
    }

    static bool isThisOfCall(const Node* user, const Use& use)
    {
        if (user->kind != NodeKind::Bytecode || (user->opcode != op_call && user->opcode != op_call_ignore_result && user->opcode != op_tail_call))
            return false;
        return use.reg == Graph::callOperands(user->instruction).argument(0);
    }

    bool worksWithoutEnvironment(Node* user, const Use& use)
    {
        if (user->kind != NodeKind::Bytecode || user->guard || user->guarded)
            return false;
        if (auto scope = scopeThatMustBeThere(user))
            return use.reg == *scope && isDissolved(locationOf(use.node).base);
        if (isThisOfCall(user, use))
            return true;
        switch (user->opcode) {
        case op_get_scope:
        case op_get_parent_scope:
            return true;
        case op_resolve_scope:
            return user->as<OpResolveScope>().m_resolveType != Dynamic;
        case op_get_from_scope:
            return use.reg == user->as<OpGetFromScope>().m_scope && (user->graph->accessedEnvironmentDepth(user) || offsetAccessedBy(user));
        case op_put_to_scope:
            return use.reg == user->as<OpPutToScope>().m_scope && (user->graph->accessedEnvironmentDepth(user) || offsetAccessedBy(user));
        case op_create_lexical_environment:
            return use.reg == user->as<OpCreateLexicalEnvironment>().m_scope && m_candidates.contains(user);
        default:
            return false;
        }
    }

    bool keepSearchedEnvironments(Node* resolve)
    {
        if (!resolve->isBytecode(op_resolve_scope) || locationOf(resolve).base != resolve || resolve->graph->resolvedEnvironmentDepth(resolve))
            return false;
        auto bytecode = resolve->as<OpResolveScope>();
        UniquedStringImpl* name = resolve->graph->codeBlock()->identifier(bytecode.m_var).impl();
        bool changed = false;
        for (Where where = locationOf(resolve->use(bytecode.m_scope)); where.isLocalEnvironment(); where = out(where, 1, 0)) {
            Node* environment = where.base;
            if (!m_candidates.contains(environment))
                continue;
            JSValue table = environment->graph->codeBlock()->getConstant(environment->as<OpCreateLexicalEnvironment>().m_symbolTable);
            if (uncheckedDowncast<SymbolTable>(table.asCell())->contains(name)) {
                m_candidates.remove(environment);
                changed = true;
            }
        }
        return changed;
    }

    template<typename Functor> void forEachUser(const Functor& functor)
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis)
                functor(phi);
            for (Node* node : block->nodes) {
                if (!node->isElided)
                    functor(node);
            }
        }
    }

    void run(bool mayPromote)
    {
        findCandidates(mayPromote);
        rewrite();
    }

    void findCandidates(bool mayPromote)
    {
        bool& hasDissolvedScopes = m_hasDissolvedScopes;
        hasDissolvedScopes = false;
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isBytecode(op_create_lexical_environment) || node->isElided)
                    continue;
                bool isDissolved = this->isDissolved(node);
                hasDissolvedScopes |= isDissolved;
                if (!mayPromote || (block->isGeneric && !isDissolved))
                    continue;
                JSValue table = node->graph->codeBlock()->getConstant(node->as<OpCreateLexicalEnvironment>().m_symbolTable);
                if (table && table.isCell() && uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize() <= 32)
                    m_candidates.add(node);
            }
        }
        if (m_candidates.isEmpty() && !hasDissolvedScopes)
            return;
        for (bool changed = true; changed && !m_candidates.isEmpty();) {
            changed = false;
            forEachUser([&](Node* user) {
                for (auto& use : user->uses) {
                    Where where = locationOf(use.node);
                    if (!where.isLocalEnvironment() || !m_candidates.contains(where.base) || worksWithoutEnvironment(user, use))
                        continue;
                    m_candidates.remove(where.base);
                    changed = true;
                }
                changed |= keepSearchedEnvironments(user);
                if (auto scope = scopeThatMustBeThere(user)) {
                    for (Where where = locationOf(user->use(*scope)); where.isLocalEnvironment(); where = out(where, 1, 0)) {
                        if (!isDissolved(where.base))
                            changed |= m_candidates.remove(where.base);
                    }
                }
            });
        }
    }

    void rewrite()
    {
        if (m_candidates.isEmpty() && !m_hasDissolvedScopes)
            return;
        for (Node* environment : m_candidates)
            environment->isPromoted = true;
        auto isInNoChain = [&](Node* environment) { return environment->isPromoted || isDissolved(environment); };
        forEachUser([&](Node* user) {
            if (user->kind != NodeKind::Bytecode)
                return;
            if (auto scope = user->isBytecode(op_create_lexical_environment) && user->isPromoted ? std::nullopt : scopeThatMustBeThere(user)) {
                Where start = locationOf(user->use(*scope));
                const KnownFunction* made = Graph::functionMadeBy(user);
                if (made && made->summary && !made->summary->captures.isEmpty()) {
                    auto& captures = m_graph.outermost().capturesOfClosures.add(user, Vector<std::pair<Node*, unsigned>, 4> { }).iterator->value;
                    for (auto [capturedScope, offset] : made->summary->captures) {
                        Node* environment = nullptr;
                        for (Where where = start; where.isLocalEnvironment() && !environment; where = out(where, 1, 0)) {
                            if (where.base->graph->scopeIdentity(where.base) == capturedScope)
                                environment = where.base;
                        }
                        captures.append({ environment, offset });
                    }
                }
                if (start.base->kind == NodeKind::Phi) {
                    for (auto& use : start.base->uses) {
                        Where where = locationOf(use.node);
                        RELEASE_ASSERT_WITH_MESSAGE(!where.isLocalEnvironment() || !isDissolved(where.base), "A scope that is in no chain reaches a closure through a phi");
                    }
                }
                if (!start.isLocalEnvironment() || !isInNoChain(start.base))
                    return;
                while (start.isLocalEnvironment() && isInNoChain(start.base))
                    start = out(start, 1, 0);
                user->scopeToStartFrom = start.base;
                user->remainingHops = start.hops;
                user->uses.append({ VirtualRegister(), start.base });
                return;
            }
            for (auto& use : user->uses) {
                if (!isThisOfCall(user, use))
                    continue;
                if (Where where = locationOf(use.node); where.isLocalEnvironment() && isInNoChain(where.base))
                    use.node = m_graph.constant(jsUndefined());
            }
            switch (user->opcode) {
            case op_get_from_scope:
            case op_put_to_scope: {
                if (user->graph->accessedEnvironmentDepth(user))
                    return;
                Where where = locationOf(user->use(user->opcode == op_get_from_scope ? user->as<OpGetFromScope>().m_scope : user->as<OpPutToScope>().m_scope));
                user->accessesLocalEnvironment = where.isLocalEnvironment();
                if (!where.isLocalEnvironment() || !where.base->isPromoted)
                    return;
                user->promotedEnvironment = where.base;
                user->offsetInEnvironment = *offsetAccessedBy(user);
                return;
            }
            case op_get_scope:
            case op_get_parent_scope:
            case op_resolve_scope: {
                Where where = locationOf(user);
                if (where.base == user) {
                    if (user->opcode != op_resolve_scope || user->graph->resolvedEnvironmentDepth(user))
                        return;
                    Where start = locationOf(user->use(user->as<OpResolveScope>().m_scope));
                    unsigned skippedScopes = 0;
                    for (; start.isLocalEnvironment() && start.base->isPromoted; ++skippedScopes)
                        start = out(start, 1, 0);
                    if (!skippedScopes)
                        return;
                    user->scopeToStartFrom = start.base;
                    user->remainingHops = start.hops;
                    user->skippedEnvironments = skippedScopes;
                    user->uses.append({ VirtualRegister(), start.base });
                    return;
                }
                if (where.isLocalEnvironment() && where.base->isPromoted) {
                    user->isElided = true;
                    return;
                }
                if (user->opcode != op_get_scope) {
                    user->scopeToStartFrom = where.base;
                    user->remainingHops = where.hops;
                    user->uses.append({ VirtualRegister(), where.base });
                }
                return;
            }
            default:
                return;
            }
        });
    }

private:
    Graph& m_graph;
    UncheckedKeyHashSet<Node*> m_candidates;
    bool m_hasDissolvedScopes { false };
};

} // anonymous namespace

void saveRegistersAtDefinitions(Graph& graph)
{
    if (!isGeneratorOrAsyncFunctionBodyParseMode(graph.codeBlock()->parseMode()))
        return;
    VirtualRegister frameRegister = virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Frame));
    if (!graph.isTracked(frameRegister))
        return;
    auto slotOf = [&](Node* node) -> std::optional<unsigned> {
        if (node->graph != &graph || node->isElided)
            return std::nullopt;
        Variable variable = graph.variableAccessedBy(node);
        if (!variable || !graph.isGeneratorFrame(variable.scope))
            return std::nullopt;
        return variable.offset;
    };
    UncheckedKeyHashMap<unsigned, Vector<Node*, 4>, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> savesBySlot;
    Node* frame = nullptr;
    bool hasSeveralFrames = false;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (node->graph != &graph)
                continue;
            if (node->isBytecode(op_create_lexical_environment) && graph.isGeneratorFrame(graph.scopeIdentity(node))) {
                hasSeveralFrames |= !!frame;
                frame = node;
            }
            if (!node->isBytecode(op_put_to_scope))
                continue;
            if (auto slot = slotOf(node))
                savesBySlot.add(*slot, Vector<Node*, 4> { }).iterator->value.append(node);
        }
    }
    if (!frame || hasSeveralFrames || frame->block->isInLoop || savesBySlot.isEmpty())
        return;
    graph.computeDominators();
    unsigned frameIndex = graph.registerIndex(frameRegister);
    unsigned numberOfSavesElided = 0;
    unsigned numberOfStoresAdded = 0;
    for (auto& [slot, saves] : savesBySlot) {
        Vector<Node*, 4> definitions;
        bool works = true;
        UncheckedKeyHashSet<Node*> seen;
        Vector<Node*, 8> worklist;
        for (Node* save : saves)
            worklist.append(save->use(save->as<OpPutToScope>().m_value));
        while (works && !worklist.isEmpty()) {
            Node* value = worklist.takeLast();
            if (!seen.add(value).isNewEntry)
                continue;
            switch (value->kind) {
            case NodeKind::Phi:
            case NodeKind::Narrow:
                for (auto& use : value->uses)
                    worklist.append(use.node);
                break;
            case NodeKind::Constant:
            case NodeKind::ConstantCell:
                definitions.append(value);
                break;
            case NodeKind::Bytecode:
                if (value->isBytecode(op_get_from_scope) && slotOf(value) == std::optional<unsigned> { slot })
                    break;
                works = value->block && !value->block->isInLoop && !value->isElided && !value->guard && !value->guarded;
                definitions.append(value);
                break;
            default:
                works = false;
                break;
            }
        }
        if (!works || definitions.size() != 1)
            continue;
        if (Node* definition = definitions[0]; definition->kind == NodeKind::Bytecode && (definition->graph != &graph || frameIndex >= definition->block->valuesAtTail.size() || !definition->block->valuesAtTail[frameIndex]))
            continue;
        Node* like = saves[0];
        for (Node* definition : definitions) {
            bool comesBeforeFrame = !definition->block || definition->kind != NodeKind::Bytecode || (definition->block != frame->block && definition->block->dominates(frame->block))
                || (definition->block == frame->block && frame->block->nodes.find(definition) < frame->block->nodes.find(frame));
            BasicBlock* block = comesBeforeFrame ? frame->block : definition->block;
            Node* after = comesBeforeFrame ? frame : definition;
            Node* frameThere = comesBeforeFrame ? frame : block->valuesAtTail[frameIndex];
            RELEASE_ASSERT(frameThere);
            Node* store = graph.addNode(NodeKind::Bytecode);
            store->opcode = op_put_to_scope;
            store->instruction = like->instruction;
            store->bytecodeIndex = like->bytecodeIndex;
            store->block = block;
            store->uses.append({ like->as<OpPutToScope>().m_scope, frameThere });
            store->uses.append({ like->as<OpPutToScope>().m_value, definition });
            store->type = like->type;
            size_t index = block->nodes.find(after);
            RELEASE_ASSERT(index != notFound);
            while (index + 1 < block->nodes.size() && block->nodes[index + 1]->kind == NodeKind::Proj)
                ++index;
            block->nodes.insert(index + 1, store);
            ++numberOfStoresAdded;
        }
        for (Node* save : saves)
            save->isElided = true;
        numberOfSavesElided += saves.size();
    }
    if (numberOfSavesElided) {
        graph.remark("saves-at-definition"_s, String::number(numberOfStoresAdded));
        graph.remark("does-not-save-at-suspension"_s, String::number(numberOfSavesElided));
    }
}

void clearDeadFrameSlots(Graph& graph)
{
    if (!isGeneratorOrAsyncFunctionBodyParseMode(graph.codeBlock()->parseMode()))
        return;
    auto slotOf = [&](Node* node) -> std::optional<unsigned> {
        if (node->graph != &graph)
            return std::nullopt;
        Variable variable = graph.variableAccessedBy(node);
        if (!variable || !graph.isGeneratorFrame(variable.scope) || variable.offset >= 1024)
            return std::nullopt;
        return variable.offset;
    };
    struct Suspension {
        BitVector saved;
        BitVector toClear;
        Node* lastSave { nullptr };
    };
    UncheckedKeyHashMap<BasicBlock*, Suspension> suspensions;
    UncheckedKeyHashMap<BasicBlock*, BitVector> restoredIn;
    UncheckedKeyHashMap<unsigned, Node*, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> saveOfSlot;
    BitVector mayHoldCell;
    for (BasicBlock* block : graph.m_rpo) {
        Node* terminal = block->nodes.isEmpty() ? nullptr : block->nodes.last();
        bool returns = terminal && terminal->graph == &graph && terminal->isBytecode(op_ret);
        for (Node* node : block->nodes) {
            if (node->isBytecode(op_put_to_scope) && returns) {
                if (auto slot = slotOf(node)) {
                    auto& suspension = suspensions.add(block, Suspension { }).iterator->value;
                    suspension.saved.set(*slot);
                    suspension.lastSave = node;
                    saveOfSlot.add(*slot, node);
                    if (mayBe(node->use(node->as<OpPutToScope>().m_value)->type, TCell))
                        mayHoldCell.set(*slot);
                }
            } else if (node->isBytecode(op_get_from_scope)) {
                if (auto slot = slotOf(node))
                    restoredIn.add(block, BitVector { }).iterator->value.set(*slot);
            }
        }
    }
    if (suspensions.isEmpty() || restoredIn.isEmpty())
        return;
    for (auto& [resume, restored] : restoredIn) {
        Vector<BasicBlock*, 8> worklist { resume };
        UncheckedKeyHashSet<BasicBlock*> seen;
        while (!worklist.isEmpty()) {
            BasicBlock* block = worklist.takeLast();
            if (!seen.add(block).isNewEntry)
                continue;
            if (auto it = suspensions.find(block); it != suspensions.end()) {
                for (unsigned slot : restored) {
                    if (!it->value.saved.get(slot) && mayHoldCell.get(slot))
                        it->value.toClear.set(slot);
                }
                continue;
            }
            for (BasicBlock* successor : block->successors)
                worklist.append(successor);
        }
    }
    unsigned numberCleared = 0;
    for (auto& [block, suspension] : suspensions) {
        if (suspension.toClear.isEmpty())
            continue;
        Node* frameThere = suspension.lastSave->use(suspension.lastSave->as<OpPutToScope>().m_scope);
        size_t index = block->nodes.find(suspension.lastSave);
        RELEASE_ASSERT(index != notFound);
        for (unsigned slot : suspension.toClear) {
            Node* like = saveOfSlot.get(slot);
            if (!like)
                continue;
            Node* store = graph.addNode(NodeKind::Bytecode);
            store->opcode = op_put_to_scope;
            store->instruction = like->instruction;
            store->bytecodeIndex = suspension.lastSave->bytecodeIndex;
            store->block = block;
            store->uses.append({ like->as<OpPutToScope>().m_scope, frameThere });
            store->uses.append({ like->as<OpPutToScope>().m_value, graph.constant(jsUndefined()) });
            store->type = like->type;
            block->nodes.insert(++index, store);
            ++numberCleared;
        }
    }
    if (numberCleared)
        graph.remark("clears-dead-frame-slots"_s, String::number(numberCleared));
}

bool mayPromoteEnvironmentsOf(Graph& graph)
{
    return graph.codeBlock()->codeType() == FunctionCode;
}

void promoteEnvironments(Graph& graph)
{
    Promoter(graph).run(mayPromoteEnvironmentsOf(graph));
}

void recordScopes(Graph& graph, VariableSummaries& summaries, const FunctionSummaryMap& summariesByExecutable, const FunctionSummary* current)
{
    using Where = Promoter::Where;
    Vector<const void*, 4> made;
    using WhyMade = VariableSummaries::WhyMade;
    Vector<std::pair<const void*, WhyMade>, 4> mustExist;
    Vector<Variable, 8> readFromInside;
    Vector<VariableSummaries::ClosureMade> closures;
    UncheckedKeyHashMap<Node*, const void*> local;
    UncheckedKeyHashSet<const void*> localScopes;
    bool canDoWithout = current && mayPromoteEnvironmentsOf(graph);
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (!node->isBytecode(op_create_lexical_environment) && !node->isBytecode(op_create_generator_frame_environment))
                continue;
            const void* scope = graph.scopeIdentity(node);
            if (!scope)
                continue;
            if (!localScopes.add(scope).isNewEntry)
                mustExist.append({ scope, WhyMade::MadeTwice });
            local.add(node, scope);
            made.append(scope);
            if (graph.isGeneratorFrame(scope))
                mustExist.append({ scope, WhyMade::GeneratorFrame });
            JSValue table = node->isBytecode(op_create_lexical_environment) ? graph.codeBlock()->getConstant(node->as<OpCreateLexicalEnvironment>().m_symbolTable) : JSValue();
            if (!canDoWithout) {
                SourceParseMode mode = graph.codeBlock()->parseMode();
                mustExist.append({ scope, graph.codeBlock()->codeType() != FunctionCode ? WhyMade::MakerIsNotFunction
                    : !current ? WhyMade::MakerHasNoSummary
                    : isGeneratorOrAsyncFunctionBodyParseMode(mode) ? WhyMade::MakerIsBody
                    : isGeneratorOrAsyncFunctionWrapperParseMode(mode) ? WhyMade::MakerIsWrapper
                    : !graph.catchEntrypoints.isEmpty() || graph.hasFrameRegisters() ? WhyMade::MakerHasCatch : WhyMade::MakerIsOtherKind });
            }
            else if (!table || !table.isCell() || uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize() > 32)
                mustExist.append({ scope, WhyMade::TooLarge });
        }
    }
    auto everyScopeInSightMustExist = [&](WhyMade why) {
        for (const void* scope : local.values())
            mustExist.append({ scope, why });
        if (const DeclaredNamesLink* declaredNames = declaredNamesFor(graph.codeBlock()))
            declaredNames->forEachScope([&](const void* scope) { mustExist.append({ scope, why }); });
    };
    Vector<BasicBlock*, 8> blocksResumedIn;
    if (isGeneratorOrAsyncFunctionBodyParseMode(graph.codeBlock()->parseMode())) {
        for (BasicBlock* block : graph.m_rpo) {
            if (!block->bytecodeBegin && !block->nodes.isEmpty() && block->nodes.last()->isBytecode(op_switch_imm)) {
                for (BasicBlock* successor : block->successors)
                    blocksResumedIn.append(successor);
            }
        }
    }
    auto storedLater = [&](Node* environment, Node* closure) {
        uint64_t mask = 0;
        auto scan = [&](BasicBlock* block, unsigned from) {
            for (unsigned i = from; i < block->nodes.size(); ++i) {
                Node* node = block->nodes[i];
                if (node == environment)
                    return false;
                if (!node->isBytecode(op_put_to_scope))
                    continue;
                Where where = Promoter::locationOf(node->use(node->as<OpPutToScope>().m_scope));
                if (!where.isLocalEnvironment() || where.base != environment)
                    continue;
                auto offset = Promoter::offsetAccessedBy(node);
                mask |= offset && *offset < 64 ? 1ull << *offset : ~0ull;
            }
            return true;
        };
        Vector<BasicBlock*, 8> worklist;
        UncheckedKeyHashSet<BasicBlock*> seen;
        size_t index = closure->block->nodes.find(closure);
        RELEASE_ASSERT(index != notFound);
        auto continueAfter = [&](BasicBlock* block) {
            for (BasicBlock* successor : block->successors)
                worklist.append(successor);
            if (!block->nodes.isEmpty() && block->nodes.last()->isBytecode(op_ret)) {
                for (BasicBlock* resumed : blocksResumedIn) {
                    if (resumed->bytecodeBegin == block->bytecodeEnd)
                        worklist.append(resumed);
                }
            }
        };
        if (scan(closure->block, index + 1))
            continueAfter(closure->block);
        while (!worklist.isEmpty()) {
            BasicBlock* block = worklist.takeLast();
            if (!seen.add(block).isNewEntry)
                continue;
            if (scan(block, 0))
                continueAfter(block);
        }
        return mask;
    };
    auto visit = [&](Node* user) {
        for (auto& use : user->uses) {
            Where where = Promoter::locationOf(use.node);
            if (!where.isLocalEnvironment())
                continue;
            bool works = user->kind == NodeKind::Bytecode && !user->guard && !user->guarded;
            if (works && Promoter::isThisOfCall(user, use))
                continue;
            if (works) {
                switch (user->opcode) {
                case op_get_scope:
                case op_get_parent_scope:
                    break;
                case op_resolve_scope: {
                    auto bytecode = user->as<OpResolveScope>();
                    works = bytecode.m_resolveType != Dynamic;
                    if (works && Promoter::locationOf(user).base == user && !graph.resolvedEnvironmentDepth(user)) {
                        UniquedStringImpl* name = graph.codeBlock()->identifier(bytecode.m_var).impl();
                        for (Where at = where; at.isLocalEnvironment(); at = Promoter::out(at, 1, 0)) {
                            JSValue table = graph.codeBlock()->getConstant(at.base->as<OpCreateLexicalEnvironment>().m_symbolTable);
                            if (const void* scope = local.get(at.base); scope && (!table || !table.isCell() || uncheckedDowncast<SymbolTable>(table.asCell())->contains(name)))
                                mustExist.append({ scope, WhyMade::UnknownAccess });
                        }
                    }
                    break;
                }
                case op_get_from_scope:
                    works = use.reg == user->as<OpGetFromScope>().m_scope && (graph.accessedEnvironmentDepth(user) || Promoter::offsetAccessedBy(user));
                    break;
                case op_put_to_scope:
                    works = use.reg == user->as<OpPutToScope>().m_scope && (graph.accessedEnvironmentDepth(user) || Promoter::offsetAccessedBy(user));
                    break;
                case op_create_lexical_environment:
                    works = use.reg == user->as<OpCreateLexicalEnvironment>().m_scope;
                    break;
                case op_new_func:
                    works = use.reg == user->as<OpNewFunc>().m_scope;
                    break;
                case op_new_func_exp:
                    works = use.reg == user->as<OpNewFuncExp>().m_scope;
                    break;
                case op_new_async_func:
                    works = use.reg == user->as<OpNewAsyncFunc>().m_scope;
                    break;
                case op_new_async_func_exp:
                    works = use.reg == user->as<OpNewAsyncFuncExp>().m_scope;
                    break;
                default:
                    works = false;
                    break;
                }
            }
            if (works)
                continue;
            for (Where at = where; at.isLocalEnvironment(); at = Promoter::out(at, 1, 0)) {
                if (const void* scope = local.get(at.base))
                    mustExist.append({ scope, WhyMade::UsedOtherwise });
            }
        }
    };
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* phi : block->phis)
            visit(phi);
        for (Node* node : block->nodes) {
            visit(node);
            if (node->kind != NodeKind::Bytecode)
                continue;
            switch (node->opcode) {
            case op_get_from_scope:
            case op_put_to_scope: {
                if (graph.accessedEnvironmentDepth(node))
                    break;
                Variable variable = graph.variableAccessedBy(node);
                if (!variable) {
                    ResolveType type = node->opcode == op_get_from_scope ? node->as<OpGetFromScope>().m_getPutInfo.resolveType() : node->as<OpPutToScope>().m_getPutInfo.resolveType();
                    const DeclaredNamesLink* declaredNames = declaredNamesFor(graph.codeBlock());
                    unsigned identifier = node->opcode == op_get_from_scope ? node->as<OpGetFromScope>().m_var : node->as<OpPutToScope>().m_var;
                    auto kind = declaredNames ? declaredNames->resolve(graph.codeBlock()->identifier(identifier).impl()).kind : DeclaredNamesLink::Resolution::Dynamic;
                    if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar || type == Dynamic || (kind != DeclaredNamesLink::Resolution::Global && kind != DeclaredNamesLink::Resolution::Stable))
                        everyScopeInSightMustExist(WhyMade::UnknownAccess);
                    break;
                }
                if (localScopes.contains(variable.scope))
                    break;
                if (node->opcode == op_put_to_scope || !current)
                    mustExist.append({ variable.scope, node->opcode == op_put_to_scope ? WhyMade::WrittenFromInside : WhyMade::ReadByUnknownCode });
                else
                    readFromInside.append(variable);
                break;
            }
            case op_new_func:
            case op_new_func_exp:
            case op_new_async_func:
            case op_new_async_func_exp: {
                UnlinkedFunctionExecutable* executable = node->opcode == op_new_func ? graph.codeBlock()->functionDecl(node->as<OpNewFunc>().m_functionDecl)
                    : node->opcode == op_new_func_exp ? graph.codeBlock()->functionExpr(node->as<OpNewFuncExp>().m_functionDecl)
                    : node->opcode == op_new_async_func ? graph.codeBlock()->functionDecl(node->as<OpNewAsyncFunc>().m_functionDecl) : graph.codeBlock()->functionExpr(node->as<OpNewAsyncFuncExp>().m_functionDecl);
                VariableSummaries::ClosureMade closure { summariesByExecutable.get(executable), current, { } };
                for (Where at = Promoter::locationOf(node->use(*Promoter::scopeOfNewFunction(node))); at.isLocalEnvironment(); at = Promoter::out(at, 1, 0)) {
                    const void* scope = local.get(at.base);
                    if (!scope)
                        break;
                    if (closure.made)
                        closure.scopesAndWhatIsStoredLater.append({ scope, storedLater(at.base, node) });
                    else
                        mustExist.append({ scope, WhyMade::ClosureIsUnknown });
                }
                if (closure.made)
                    closures.append(WTF::move(closure));
                break;
            }
            case op_call_direct_eval:
            case op_push_with_scope:
            case op_create_scoped_arguments:
            case op_resolve_scope_for_hoisting_func_decl_in_eval:
                everyScopeInSightMustExist(WhyMade::Evaluated);
                break;
            default:
                break;
            }
        }
    }
    summaries.noteScopes(current, made.span(), mustExist.span(), readFromInside.span(), WTF::move(closures));
}

NodeUsers::NodeUsers(Graph& graph)
{
    auto note = [&](Node* user) {
        if (user->isElided)
            return;
        for (auto& use : user->uses)
            m_users.add(use.node, Vector<Node*, 2> { }).iterator->value.append(user);
    };
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* phi : block->phis)
            note(phi);
        for (Node* node : block->nodes)
            note(node);
    }
}

bool isAbsentFromObjectPrototype(UniquedStringImpl* name)
{
    if (!ImmutableIntrinsics::shared() || name->isSymbol())
        return false;
    static constexpr ASCIILiteral names[] = {
        "constructor"_s, "toString"_s, "toLocaleString"_s, "valueOf"_s, "hasOwnProperty"_s, "propertyIsEnumerable"_s, "isPrototypeOf"_s,
        "__defineGetter__"_s, "__defineSetter__"_s, "__lookupGetter__"_s, "__lookupSetter__"_s, "__proto__"_s,
    };
    for (auto candidate : names) {
        if (equal(name, candidate))
            return false;
    }
    return true;
}

std::optional<NodeUsers::OnlyRead> NodeUsers::isOnlyRead(Node* object, std::span<UniquedStringImpl* const> names, uint16_t layoutID, AbsentReads absentReads) const
{
    OnlyRead result;
    result.aliasingUsers.append(object);
    for (unsigned i = 0; i < result.aliasingUsers.size(); ++i) {
        Node* alias = result.aliasingUsers[i];
        for (Node* user : of(alias)) {
            if (user->kind == NodeKind::Narrow) {
                result.aliasingUsers.append(user);
                continue;
            }
            if (user->kind != NodeKind::Bytecode || user->guard)
                return std::nullopt;
            switch (user->opcode) {
            case op_check_tdz:
                result.aliasingUsers.append(user);
                break;
            case op_check_type: {
                unsigned mask = user->as<OpCheckType>().m_mask;
                if (mask > SoundTypeAll || !(mask & SoundTypeOtherObject))
                    return std::nullopt;
                result.aliasingUsers.append(user);
                break;
            }
            case op_type_tag:
                if (!user->firstLayout || !TypeTable::shared() || (user->firstLayout != layoutID && !user->isTrusted && !TypeTable::shared()->usesFieldIDs(user->firstLayout)))
                    return std::nullopt;
                result.aliasingUsers.append(user);
                break;
            case op_get_by_id: {
                auto bytecode = user->as<OpGetById>();
                if (user->use(bytecode.m_base) != alias)
                    return std::nullopt;
                UniquedStringImpl* name = user->graph->codeBlock()->identifier(bytecode.m_property).impl();
                size_t index = names.size();
                for (size_t candidate = 0; candidate < names.size(); ++candidate) {
                    if (names[candidate] == name)
                        index = candidate;
                }
                if (index == names.size()) {
                    if (absentReads == AbsentReads::Disallow || !isAbsentFromObjectPrototype(name))
                        return std::nullopt;
                    result.absentReads.append(user);
                    break;
                }
                result.reads.append({ user, static_cast<unsigned>(index) });
                break;
            }
            case op_jundefined_or_null:
            case op_jnundefined_or_null:
            case op_jeq_null:
            case op_jneq_null:
                result.tests.append(user);
                break;
            default:
                return std::nullopt;
            }
        }
    }
    return result;
}

static std::optional<MultiValueReturnTable::Names> literalNames(Node* node)
{
    auto& instructions = node->graph->codeBlock()->instructions();
    auto& stores = node->graph->literalStores(node->bytecodeIndex.offset());
    RELEASE_ASSERT(stores.size() >= node->numberOfLiteralProperties);
    MultiValueReturnTable::Names names;
    for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i) {
        UniquedStringImpl* name = node->graph->codeBlock()->identifier(instructions.at(stores[i])->as<OpPutById>().m_property).impl();
        if (names.contains(name))
            return std::nullopt;
        names.append(name);
    }
    return names;
}

void recordReturnedLiterals(Graph& graph)
{
    MultiValueReturnTable* all = multiValueReturnTable();
    UnlinkedCodeBlock* code = graph.codeBlock();
    if (!all || code->codeType() != FunctionCode || code->isConstructor())
        return;
    if (code->parseMode() != SourceParseMode::NormalFunctionMode && code->parseMode() != SourceParseMode::ArrowFunctionMode && code->parseMode() != SourceParseMode::MethodMode)
        return;
    std::optional<NodeUsers> users;
    std::optional<MultiValueReturnTable::Names> names;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode)
                continue;
            if (node->opcode == op_tail_call || node->opcode == op_tail_call_varargs)
                return;
            if (node->opcode != op_ret)
                continue;
            Node* object = node->use(node->as<OpRet>().m_value);
            if (!object->isBytecode(op_new_object) || !object->numberOfLiteralProperties || object->numberOfLiteralProperties > FunctionSummary::maxReturnValues)
                return;
            if (!users)
                users.emplace(graph);
            if (users->of(object).size() != 1)
                return;
            auto literalPropertyNames = literalNames(object);
            if (!literalPropertyNames || (names && !(*names == *literalPropertyNames)))
                return;
            names = WTF::move(literalPropertyNames);
        }
    }
    if (names)
        all->note(code, WTF::move(*names));
}

void planMultiValueReturns(Graph& graph)
{
    if (!multiValueReturnTable())
        return;
    if (auto* names = registerReturnValuesOf(graph.codeBlock(), graph.summary())) {
        for (BasicBlock* block : graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isBytecode(op_ret) || node->graph != &graph)
                    continue;
                Node* object = node->use(node->as<OpRet>().m_value);
                RELEASE_ASSERT(object->isBytecode(op_new_object) && object->numberOfLiteralProperties == names->size());
                object->isElided = true;
            }
        }
        graph.numberOfRegisterReturnValues = names->size();
    }
    std::optional<NodeUsers> users;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (!node->isBytecode(op_call) || node->isElided)
                continue;
            bool isExact = false;
            const KnownFunction* known = graph.knownCallee(node, &isExact);
            if (!known || !isExact || !known->forCall)
                continue;
            auto* names = registerReturnValuesOf(known->forCall, known->summary);
            if (!names)
                continue;
            if (!users)
                users.emplace(graph);
            auto onlyRead = users->isOnlyRead(node, names->span(), 0);
            RELEASE_ASSERT(onlyRead);
            graph.remark("reads-returned-object-from-registers"_s, known->executable ? known->executable->ecmaName().string() : String());
            node->numberOfReturnValues = names->size();
            auto& reads = graph.returnValueReads.add(node, Vector<Node*, 8> { }).iterator->value;
            for (auto [read, index] : onlyRead->reads) {
                read->kind = NodeKind::Proj;
                read->uses.shrink(0);
                read->uses.append({ VirtualRegister(), node });
                read->returnValueIndex = index;
                reads.append(read);
            }
            for (Node* test : onlyRead->tests) {
                for (auto& use : test->uses) {
                    if (onlyRead->aliasingUsers.contains(use.node))
                        use.node = graph.constant(jsBoolean(true));
                }
            }
            for (Node* alias : onlyRead->aliasingUsers) {
                if (alias != node)
                    alias->isElided = true;
            }
        }
    }
}

void scalarReplaceReadOnlyObjects(Graph& graph)
{
    auto resolve = [](Node* node) {
        while (node->replacement)
            node = node->replacement;
        return node;
    };
    for (bool changed = true; changed;) {
        changed = false;
        std::optional<NodeUsers> users;
        for (BasicBlock* block : graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isBytecode(op_new_object) || !node->numberOfLiteralProperties || node->isElided)
                    continue;
                auto& instructions = node->graph->codeBlock()->instructions();
                auto& stores = node->graph->literalStores(node->bytecodeIndex.offset());
                RELEASE_ASSERT(stores.size() >= node->numberOfLiteralProperties);
                Vector<UniquedStringImpl*, 8> names;
                bool hasDuplicateEntry = false;
                for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i) {
                    UniquedStringImpl* name = node->graph->codeBlock()->identifier(instructions.at(stores[i])->as<OpPutById>().m_property).impl();
                    hasDuplicateEntry |= names.contains(name);
                    names.append(name);
                }
                if (hasDuplicateEntry)
                    continue;
                if (!users)
                    users.emplace(graph);
                auto onlyRead = users->isOnlyRead(node, names.span(), Graph::newObjectLayoutID(node), NodeUsers::AbsentReads::Allow);
                if (!onlyRead)
                    continue;
                for (auto [read, index] : onlyRead->reads) {
                    read->replacement = node->use(NewObjectPlan::registerOf(index));
                    read->isElided = true;
                }
                graph.remark("scalar-replaced-object"_s);
                for (Node* read : onlyRead->absentReads) {
                    graph.remark("absent-property-is-undefined"_s, read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property).string());
                    read->replacement = graph.constant(jsUndefined());
                    read->isElided = true;
                }
                for (Node* test : onlyRead->tests) {
                    for (auto& use : test->uses) {
                        if (onlyRead->aliasingUsers.contains(use.node))
                            use.node = graph.constant(jsBoolean(true));
                    }
                }
                for (Node* alias : onlyRead->aliasingUsers)
                    alias->isElided = true;
                changed = true;
            }
        }
        if (!changed)
            break;
        for (BasicBlock* block : graph.m_rpo) {
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
}

void replaceReadsOfConstantObjects(Graph& graph)
{
    VariableSummaries* summaries = graph.variableSummaries();
    if (!summaries)
        return;
    bool hasReplacedReads = false;
    auto isAlias = [](const Node* node) {
        return node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag);
    };
    auto skipAliases = [&](Node* node) {
        while (isAlias(node))
            node = node->uses[0].node;
        return node;
    };
    UncheckedKeyHashSet<Node*> objectsNotAllocated;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (!node->isBytecode(op_put_to_scope))
                continue;
            Variable variable = graph.variableAccessedBy(node);
            auto* literal = variable ? summaries->constantObjectIn(variable) : nullptr;
            if (!literal || !literal->isNeverAllocated)
                continue;
            VirtualRegister value = node->as<OpPutToScope>().m_value;
            for (auto& use : node->uses) {
                if (use.reg != value)
                    continue;
                Node* object = skipAliases(use.node);
                RELEASE_ASSERT(object->isBytecode(op_new_object));
                graph.remark("does-not-allocate-constant-object"_s, StringView(literal->variableName));
                objectsNotAllocated.add(object);
                object->isElided = true;
                use.node = graph.constant(jsBoolean(true));
            }
        }
    }
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (!objectsNotAllocated.isEmpty() && !node->uses.isEmpty()) {
                if (isAlias(node) && objectsNotAllocated.contains(skipAliases(node))) {
                    node->isElided = true;
                    continue;
                }
                if (node->kind == NodeKind::SetStack && objectsNotAllocated.contains(skipAliases(node->uses[0].node))) {
                    node->uses[0].node = graph.constant(jsBoolean(true));
                    continue;
                }
            }
            if ((node->isBytecode(op_type_tag) || node->isBytecode(op_check_type)) && !node->uses.isEmpty()) {
                Node* source = skipAliases(node);
                Variable variable = source->isBytecode(op_get_from_scope) ? graph.variableAccessedBy(source) : Variable { };
                if (auto* literal = variable ? summaries->constantObjectIn(variable) : nullptr; literal && literal->isNeverAllocated) {
                    node->replacement = node->uses[0].node;
                    node->isElided = true;
                    hasReplacedReads = true;
                }
                continue;
            }
            if (!node->isBytecode(op_get_by_id) || node->guard || node->isElided || node->isReadOnlyForCall)
                continue;
            auto bytecode = node->as<OpGetById>();
            Node* source = node->use(bytecode.m_base);
            while (source->kind == NodeKind::Narrow || source->isBytecode(op_check_type) || source->isBytecode(op_check_tdz) || source->isBytecode(op_type_tag))
                source = source->uses[0].node;
            if (!source->isBytecode(op_get_from_scope))
                continue;
            Variable variable = graph.variableAccessedBy(source);
            auto* literal = variable ? summaries->constantObjectIn(variable) : nullptr;
            if (!literal)
                continue;
            UniquedStringImpl* name = node->graph->codeBlock()->identifier(bytecode.m_property).impl();
            size_t index = literal->names.find(name);
            if (index == notFound) {
                if (!isAbsentFromObjectPrototype(name))
                    continue;
                node->replacement = graph.constant(jsUndefined());
                node->propertyOfConstantObjectIsAbsent = true;
            } else if (auto& value = literal->values[index]; value.isKnown()) {
                node->replacement = value.immediate ? graph.constant(value.immediate) : graph.constantCellOf(literal->codeBlock, value.constantCell);
            } else {
                if (literal->inlineCapacity) {
                    node->slotInConstantObjectPlusOne = safeCast<uint16_t>(index + 1);
                    node->inlineCapacityOfConstantObject = *literal->inlineCapacity;
                }
                continue;
            }
            node->onlyChecksConstantObject = true;
            node->constantObjectIsNeverAllocated = literal->isNeverAllocated;
            hasReplacedReads = true;
        }
    }
    if (!hasReplacedReads)
        return;
    auto resolve = [](Node* node) {
        while (node->replacement)
            node = node->replacement;
        return node;
    };
    for (BasicBlock* block : graph.m_rpo) {
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

void shareRegExpLiterals(Graph& graph)
{
    if (!Options::useSharedRegExpLiteralObjects() || !ImmutableIntrinsics::shared())
        return;
    Vector<Node*, 4> literals;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode || (node->opcode != op_call && node->opcode != op_call_ignore_result && node->opcode != op_tail_call))
                continue;
            auto operands = Graph::callOperands(node->instruction);
            if (operands.argc < 2)
                continue;
            Node* literal = node->use(operands.argument(1));
            Type receiver = node->use(operands.argument(0))->type;
            if (!node->builtinCalled || !literal->isBytecode(op_new_reg_exp) || literal->isElided || !receiver || !isSubtype(receiver, TString))
                continue;
            switch (builtinAtIndex(node->builtinCalled)) {
            case Builtin::StringReplace:
            case Builtin::StringReplaceAll:
            case Builtin::StringMatch:
            case Builtin::StringSearch:
            case Builtin::StringSplit:
                break;
            default:
                continue;
            }
            if (uncheckedDowncast<RegExp>(literal->graph->codeBlock()->getConstant(literal->as<OpNewRegExp>().m_regexp).asCell())->sticky())
                continue;
            literals.append(literal);
        }
    }
    if (literals.isEmpty())
        return;
    UncheckedKeyHashMap<Node*, unsigned> numberOfUses;
    for (Node* literal : literals)
        numberOfUses.add(literal, 0);
    auto count = [&](Node* user) {
        for (auto& use : user->uses) {
            if (auto it = numberOfUses.find(use.node); it != numberOfUses.end())
                ++it->value;
        }
    };
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* phi : block->phis)
            count(phi);
        for (Node* node : block->nodes)
            count(node);
    }
    for (auto& [literal, uses] : numberOfUses)
        literal->isSharedRegExpLiteral = uses == 1;
}

void analyzeEscapes(Graph& graph)
{
    EscapeAnalysis analysis(graph, nullptr);
    analysis.analyzeAllocations();
}

uint32_t escapingParameters(Graph& graph, Vector<const KnownFunction*>* calleesRead)
{
    return EscapeAnalysis(graph, calleesRead).escapingParameters();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
