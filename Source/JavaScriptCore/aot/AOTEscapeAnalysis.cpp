/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

#include "AOTTypeTable.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "SymbolTable.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace AOT {

ASCIILiteral nameOf(Escape escape)
{
    switch (escape) {
    case Escape::NotLookedAt:
        return "not looked at"_s;
    case Escape::StaysHere:
        return "STAYS: nothing but this code sees it"_s;
    case Escape::IsOnlyLent:
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
    case Escape::PassedToClosureMadeHere:
        return "passed to a closure that is made here"_s;
    case Escape::PassedToParameter:
        return "passed to a function that was itself passed in"_s;
    case Escape::PassedToVariable:
        return "passed to what was read from a variable, not proven"_s;
    case Escape::PassedToUnknown:
        return "passed to something else"_s;
    case Escape::PassedToKnownThatKeepsIt:
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

std::optional<AllocationKind> kindOfAllocation(const Node* node)
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

// Nothing that the code does hands on the scope it was closed over: it makes no closure of its own, which would have that scope for
// its own or further out, and nothing else gets at a scope as a value. Plain from the bytecode.
static bool keepsItsScopeToItself(UnlinkedCodeBlock* code)
{
    if (code->codeType() != FunctionCode)
        return false;
    // (What a generator or an async function has in hand when it stops is kept for it, its scope for one.)
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

bool mayGetHoldOfItself(UnlinkedCodeBlock* code)
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
    EscapeAnalysis(Graph& graph, Vector<const KnownFunction*>* calleesConsulted)
        : m_graph(graph)
        , m_calleesConsulted(calleesConsulted)
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis)
                noteUsesBy(phi);
            for (Node* node : block->nodes)
                noteUsesBy(node);
        }
    }

    void lookAtWhatIsMade()
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (kindOfAllocation(node))
                    node->escape = fateOf(node, false);
            }
        }
    }

    // A bit for each, `this` being the first; ProgramFacts::whatIsPassedBeyondParametersEscapes for the rest.
    uint32_t parametersThatEscape()
    {
        UnlinkedCodeBlock* code = m_graph.codeBlock();
        if (code->codeType() != FunctionCode || (code->parseMode() != SourceParseMode::NormalFunctionMode && code->parseMode() != SourceParseMode::ArrowFunctionMode && code->parseMode() != SourceParseMode::MethodMode))
            return std::numeric_limits<uint32_t>::max();
        uint32_t result = 0;
        for (const auto& instruction : code->instructions()) {
            switch (instruction->opcodeID()) {
            // These get at what was passed without saying which.
            case op_call_direct_eval:
            case op_create_direct_arguments:
            case op_create_scoped_arguments:
            case op_create_cloned_arguments:
            case op_get_argument:
                return std::numeric_limits<uint32_t>::max();
            case op_create_rest:
                result |= ProgramFacts::whatIsPassedBeyondParametersEscapes;
                break;
            default:
                break;
            }
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->kind != NodeKind::Argument || node->graph != &m_graph)
                    continue;
                unsigned index = node->reg.toArgument();
                if (index >= ProgramFacts::mostParametersToldOfEscaping) {
                    result |= ProgramFacts::whatIsPassedBeyondParametersEscapes;
                    continue;
                }
                if (!stays(fateOf(node, true)))
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
        enum Kind : uint8_t { Harmless, IsTheSame, Lent, FieldType, Escapes } kind { Harmless };
        Escape why { Escape::Other };
    };
    static Verdict escapes(Escape why) { return { Verdict::Escapes, why }; }

    Escape fateOf(Node* value, bool isParameter)
    {
        if (!isParameter) {
            if (auto it = m_fates.find(value); it != m_fates.end())
                return it->value;
            // (Whatever holds it was made after it, so this comes to an end. In case it does not.)
            m_fates.add(value, Escape::Other);
        }
        Escape result = Escape::StaysHere;
        if (!isParameter && kindOfAllocation(value) == AllocationKind::Closure) {
            UnlinkedFunctionExecutable* executable = value->opcode == op_new_func ? value->graph->codeBlock()->functionDecl(value->as<OpNewFunc>().m_functionDecl) : value->graph->codeBlock()->functionExpr(value->as<OpNewFuncExp>().m_functionDecl);
            UnlinkedFunctionCodeBlock* code = executable->codeBlockIfThereIsOne(CodeSpecializationKind::CodeForCall);
            if (!code || mayGetHoldOfItself(code))
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
                Verdict verdict = whatBecomesOfItIn(user.node, user.reg, value, isParameter);
                switch (verdict.kind) {
                case Verdict::Harmless:
                    continue;
                case Verdict::IsTheSame:
                    if (seen.add(user.node).isNewEntry)
                        worklist.append(user.node);
                    continue;
                case Verdict::Lent:
                    result = Escape::IsOnlyLent;
                    continue;
                case Verdict::FieldType: {
                    Escape ofHolder = fateOf(user.node, false);
                    if (!stays(ofHolder)) {
                        result = Escape::StoredInEscapingObject;
                        break;
                    }
                    if (ofHolder == Escape::IsOnlyLent)
                        result = Escape::IsOnlyLent;
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
            m_fates.set(value, result);
        return result;
    }

    Verdict whatBecomesOfItInCall(Node* call, VirtualRegister reg, VirtualRegister calleeRegister, unsigned argc, unsigned argv, bool isTailCall, bool isParameter)
    {
        // Calling something is not a way of getting hold of it, for anybody but itself (and what is made here that could name itself
        // does so by way of a variable).
        if (reg == calleeRegister)
            return { };
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        int index = reg.offset() - firstArgument;
        if (index < 0 || static_cast<unsigned>(index) >= argc)
            return escapes(Escape::Other);
        // (What this function makes is gone with its frame. What it was passed is somebody else's.)
        if (isTailCall && !isParameter)
            return escapes(Escape::PassedInTailCall);

        bool isProven = false;
        const KnownFunction* known = call->graph->knownCallee(call, &isProven);
        if (known && isProven && known->forCall && known->facts) {
            if (m_calleesConsulted && !m_calleesConsulted->contains(known))
                m_calleesConsulted->append(known);
            uint32_t mask = known->facts->parametersThatEscape.load(std::memory_order_relaxed);
            unsigned parameters = std::min<unsigned>(known->forCall->numParameters(), ProgramFacts::mostParametersToldOfEscaping);
            bool letsItOut = static_cast<unsigned>(index) < parameters ? mask >> index & 1 : mask & ProgramFacts::whatIsPassedBeyondParametersEscapes;
            if (letsItOut)
                return escapes(Escape::PassedToKnownThatKeepsIt);
            return { Verdict::Lent };
        }
        Node* callee = call->use(calleeRegister);
        if (callee->kind == NodeKind::Intrinsic)
            return escapes(Escape::PassedToBuiltin);
        if (callee->isBytecode(op_get_by_id) || callee->isBytecode(op_get_by_val))
            return escapes(index ? Escape::PassedToMethod : Escape::PassedAsThisToMethod);
        if (callee->isBytecode(op_new_func_exp) || callee->isBytecode(op_new_func))
            return escapes(Escape::PassedToClosureMadeHere);
        if (callee->kind == NodeKind::Argument)
            return escapes(Escape::PassedToParameter);
        if (callee->isBytecode(op_get_from_scope))
            return escapes(Escape::PassedToVariable);
        return escapes(Escape::PassedToUnknown);
    }

    // value: what is asked about. It, or what is the same thing, is what `user` reads from `reg`.
    Verdict whatBecomesOfItIn(Node* user, VirtualRegister reg, Node* value, bool isParameter)
    {
        switch (user->kind) {
        case NodeKind::Phi:
        case NodeKind::Narrow:
            return escapes(Escape::Merged);
        case NodeKind::SetStack:
            return escapes(Escape::Homed);
        // (It looks. What it stands in front of is a user in its own right.)
        case NodeKind::Guard:
            return { };
        case NodeKind::Bytecode:
            break;
        default:
            return escapes(Escape::Other);
        }

        bool isArrayMadeHere = !isParameter && kindOfAllocation(value) == AllocationKind::Array;
        switch (user->opcode) {
        // ---- Looking at it. Whatever is found in it is another matter.
        // (For something that was passed in: as long as reading and writing its properties runs nobody's code. That is for whoever
        // passes it to know, and goes for whatever is made where it can be seen what it is made with.)
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

        // ---- The same thing by another name.
        case op_check_type:
        case op_type_tag:
        case op_mov:
        case op_to_this:
        case op_to_object:
        case op_identity_with_profile:
        // (One of the scopes from that one out: which may be that one.)
        case op_resolve_scope:
            return { Verdict::IsTheSame };

        // ---- Putting something in it, or it in something.
        case op_put_by_id: {
            auto bytecode = user->as<OpPutById>();
            if (reg == bytecode.m_value)
                return escapes(Escape::StoredInProperty);
            // (There is one setter that everything has.)
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

        // ---- What has it for its scope has it for as long as it is there itself.
        case op_create_lexical_environment:
            return reg == user->as<OpCreateLexicalEnvironment>().m_scope ? Verdict { Verdict::FieldType } : escapes(Escape::Other);
        case op_new_func_exp:
        case op_new_func: {
            UnlinkedFunctionExecutable* executable = user->opcode == op_new_func ? user->graph->codeBlock()->functionDecl(user->as<OpNewFunc>().m_functionDecl) : user->graph->codeBlock()->functionExpr(user->as<OpNewFuncExp>().m_functionDecl);
            UnlinkedFunctionCodeBlock* code = executable->codeBlockIfThereIsOne(CodeSpecializationKind::CodeForCall);
            if (!code || !keepsItsScopeToItself(code))
                return escapes(Escape::ClosureLetsScopeOut);
            return { Verdict::FieldType };
        }
        case op_new_generator_func:
        case op_new_generator_func_exp:
        case op_new_async_func:
        case op_new_async_func_exp:
        case op_new_async_generator_func:
        case op_new_async_generator_func_exp:
            return escapes(Escape::ClosureLetsScopeOut);

        // ---- Handing it on.
        case op_call: {
            auto bytecode = user->as<OpCall>();
            return whatBecomesOfItInCall(user, reg, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, isParameter);
        }
        case op_call_ignore_result: {
            auto bytecode = user->as<OpCallIgnoreResult>();
            return whatBecomesOfItInCall(user, reg, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, isParameter);
        }
        case op_tail_call: {
            auto bytecode = user->as<OpTailCall>();
            // (Made part of something that goes on after it, it is a call like any other.)
            return whatBecomesOfItInCall(user, reg, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, user->graph->isInTailPosition, isParameter);
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

        // ---- What is in an array that was made here, one after the other, if iterating over arrays is what it was to begin with.
        case op_spread:
        case op_iterator_open:
        case op_iterator_next:
        case op_iterator_close_check:
        case op_new_array_with_spread:
            return isArrayMadeHere && Options::useImmutableIntrinsics() && user->opcode == op_spread ? Verdict { } : escapes(Escape::Iterated);

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
    UncheckedKeyHashMap<Node*, Escape> m_fates;
};

// ---- Environments that are no objects.
//
// A function whose inner functions use its variables keeps those in an environment record, on the heap, for the closures to find them in. Once the closures have all become part of the
// function's own code (inlineCalls()) nothing is left that has to find anything: the variables are the function's own, like any that no closure uses.
namespace {

class Promoter {
public:
    Promoter(Graph& graph)
        : m_graph(graph)
    {
    }

    // Which scope a value is: `hops` out from `base`. An environment that is made here is its own base.
    struct Where {
        Node* base { nullptr };
        unsigned hops { 0 };
        bool isEnvironmentMadeHere() const { return !hops && base->isBytecode(op_create_lexical_environment); }
    };

    static Where out(Where from, unsigned hops, unsigned depth)
    {
        while (hops && from.isEnvironmentMadeHere()) {
            from = whereIs(from.base->use(from.base->as<OpCreateLexicalEnvironment>().m_scope), depth + 1);
            --hops;
        }
        from.hops += hops;
        return from;
    }

    // As the lowering has it (lowerResolveScope() and the rest).
    static Where whereIs(Node* scope, unsigned depth = 0)
    {
        if (depth > 24 || scope->kind != NodeKind::Bytecode)
            return { scope, 0 };
        switch (scope->opcode) {
        case op_get_scope:
            if (Node* closedOver = scope->graph->scopeOfClosure)
                return whereIs(closedOver, depth + 1);
            return { scope, 0 };
        case op_get_parent_scope:
            return out(whereIs(scope->use(scope->as<OpGetParentScope>().m_scope), depth + 1), 1, depth);
        case op_resolve_scope: {
            auto bytecode = scope->as<OpResolveScope>();
            // (It is where it is, and the scope it is told to start from is not looked at.)
            if (scope->graph->distanceOfEnvironmentResolvedTo(scope))
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
            return out(whereIs(scope->use(bytecode.m_scope), depth + 1), hops, depth);
        }
        default:
            return { scope, 0 };
        }
    }

    // Where in the environment that its scope is the instruction's variable is, if that is plain.
    static std::optional<unsigned> offsetAccessedBy(Node* node)
    {
        if (node->graph->distanceOfEnvironmentAccessed(node))
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

    // Whether the node, which is handed the environment itself, makes do without there being one.
    bool makesDoWithout(Node* user, const Use& use)
    {
        if (user->kind != NodeKind::Bytecode || user->guard || user->guarded)
            return false;
        switch (user->opcode) {
        case op_get_scope: // (Of what has become part of this code: another name for it.)
        case op_get_parent_scope:
            return true;
        case op_resolve_scope:
            // (What is not found until the code runs is in none of the environments that the code says it makes: it starts looking further out.)
            return user->as<OpResolveScope>().m_resolveType != Dynamic;
        case op_get_from_scope:
            return use.reg == user->as<OpGetFromScope>().m_scope && (user->graph->distanceOfEnvironmentAccessed(user) || offsetAccessedBy(user));
        case op_put_to_scope:
            return use.reg == user->as<OpPutToScope>().m_scope && (user->graph->distanceOfEnvironmentAccessed(user) || offsetAccessedBy(user));
        case op_create_lexical_environment:
            return use.reg == user->as<OpCreateLexicalEnvironment>().m_scope && m_candidates.contains(user);
        default:
            return false;
        }
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

    void run()
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isBytecode(op_create_lexical_environment) || node->isElided || block->isGeneric)
                    continue;
                JSValue table = node->graph->codeBlock()->getConstant(node->as<OpCreateLexicalEnvironment>().m_symbolTable);
                if (table && table.isCell() && uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize() <= 32)
                    m_candidates.add(node);
            }
        }
        if (m_candidates.isEmpty())
            return;
        for (bool changed = true; changed && !m_candidates.isEmpty();) {
            changed = false;
            forEachUser([&](Node* user) {
                for (auto& use : user->uses) {
                    Where where = whereIs(use.node);
                    if (!where.isEnvironmentMadeHere() || !m_candidates.contains(where.base) || makesDoWithout(user, use))
                        continue;
                    m_candidates.remove(where.base);
                    changed = true;
                }
            });
        }
        if (m_candidates.isEmpty())
            return;
        for (Node* environment : m_candidates)
            environment->isPromoted = true;
        forEachUser([&](Node* user) {
            if (user->kind != NodeKind::Bytecode)
                return;
            switch (user->opcode) {
            case op_get_from_scope:
            case op_put_to_scope: {
                if (user->graph->distanceOfEnvironmentAccessed(user))
                    return;
                Where where = whereIs(user->use(user->opcode == op_get_from_scope ? user->as<OpGetFromScope>().m_scope : user->as<OpPutToScope>().m_scope));
                if (!where.isEnvironmentMadeHere() || !where.base->isPromoted)
                    return;
                user->promotedEnvironment = where.base;
                user->offsetInEnvironment = *offsetAccessedBy(user);
                return;
            }
            case op_get_scope:
            case op_get_parent_scope:
            case op_resolve_scope: {
                Where where = whereIs(user);
                if (where.base == user) {
                    if (user->opcode != op_resolve_scope || user->graph->distanceOfEnvironmentResolvedTo(user))
                        return;
                    Where start = whereIs(user->use(user->as<OpResolveScope>().m_scope));
                    unsigned passedOver = 0;
                    for (; start.isEnvironmentMadeHere() && start.base->isPromoted; ++passedOver)
                        start = out(start, 1, 0);
                    if (!passedOver)
                        return;
                    user->scopeToStartFrom = start.base;
                    user->hopsFromThere = start.hops;
                    user->environmentsPassedOver = passedOver;
                    user->uses.append({ VirtualRegister(), start.base });
                    return;
                }
                // Another name for an environment that is not there: whoever uses it makes do without.
                if (where.isEnvironmentMadeHere() && where.base->isPromoted) {
                    user->isElided = true;
                    return;
                }
                if (user->opcode != op_get_scope) {
                    user->scopeToStartFrom = where.base;
                    user->hopsFromThere = where.hops;
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
};

} // anonymous namespace

void promoteEnvironments(Graph& graph)
{
    // What a handler reads has to be in memory, and what a generator has in hand when it stops has to be kept for it.
    UnlinkedCodeBlock* code = graph.codeBlock();
    if (!graph.catchEntrypoints.isEmpty() || graph.hasHomedRegisters() || code->codeType() != FunctionCode)
        return;
    switch (code->parseMode()) {
    case SourceParseMode::NormalFunctionMode:
    case SourceParseMode::ArrowFunctionMode:
    case SourceParseMode::MethodMode:
    case SourceParseMode::GetterMode:
    case SourceParseMode::SetterMode:
        break;
    default:
        return;
    }
    Promoter(graph).run();
}

UsersOfNodes::UsersOfNodes(Graph& graph)
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

std::optional<UsersOfNodes::OnlyRead> UsersOfNodes::isOnlyRead(Node* object, std::span<UniquedStringImpl* const> names, uint16_t layoutID) const
{
    OnlyRead result;
    result.handedOn.append(object);
    for (unsigned i = 0; i < result.handedOn.size(); ++i) {
        Node* alias = result.handedOn[i];
        for (Node* user : of(alias)) {
            if (user->kind == NodeKind::Narrow) {
                result.handedOn.append(user);
                continue;
            }
            if (user->kind != NodeKind::Bytecode || user->guard)
                return std::nullopt;
            switch (user->opcode) {
            case op_check_tdz:
                result.handedOn.append(user);
                break;
            case op_check_type: {
                // (It gets past, or there would be something to throw.)
                unsigned mask = user->as<OpCheckType>().m_mask;
                if (mask > SoundTypeAll || !(mask & SoundTypeOtherObject))
                    return std::nullopt;
                result.handedOn.append(user);
                break;
            }
            case op_type_tag:
                // (Likewise. Where the slots are verified it asks nothing.)
                if (!user->firstLayout || !TypeTable::shared() || (user->firstLayout != layoutID && !user->isTrusted && !TypeTable::shared()->usesFieldIDs(user->firstLayout)))
                    return std::nullopt;
                result.handedOn.append(user);
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
                // (What it has no property of its own for is a matter of what it inherits.)
                if (index == names.size())
                    return std::nullopt;
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

// None: it has one of them twice.
static std::optional<ThingsReturnedByFunctions::Names> namesOfLiteral(Node* node)
{
    auto& instructions = node->graph->codeBlock()->instructions();
    auto stores = Graph::storesOfLiteral(instructions, node->bytecodeIndex.offset());
    RELEASE_ASSERT(stores.size() >= node->numberOfLiteralProperties);
    ThingsReturnedByFunctions::Names names;
    for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i) {
        UniquedStringImpl* name = node->graph->codeBlock()->identifier(instructions.at(stores[i])->as<OpPutById>().m_property).impl();
        if (names.contains(name))
            return std::nullopt;
        names.append(name);
    }
    return names;
}

void noteThingsReturned(Graph& graph)
{
    ThingsReturnedByFunctions* all = thingsReturnedByFunctions();
    UnlinkedCodeBlock* code = graph.codeBlock();
    if (!all || code->codeType() != FunctionCode || code->isConstructor())
        return;
    // (What one that can be suspended returns goes to whatever resumes it.)
    if (code->parseMode() != SourceParseMode::NormalFunctionMode && code->parseMode() != SourceParseMode::ArrowFunctionMode && code->parseMode() != SourceParseMode::MethodMode)
        return;
    std::optional<UsersOfNodes> users;
    std::optional<ThingsReturnedByFunctions::Names> names;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode)
                continue;
            // (What something else returns.)
            if (node->opcode == op_tail_call || node->opcode == op_tail_call_varargs)
                return;
            if (node->opcode != op_ret)
                continue;
            Node* object = node->use(node->as<OpRet>().m_value);
            if (!object->isBytecode(op_new_object) || !object->numberOfLiteralProperties || object->numberOfLiteralProperties > ProgramFacts::mostThingsReturned)
                return;
            if (!users)
                users.emplace(graph);
            if (users->of(object).size() != 1)
                return;
            auto itsNames = namesOfLiteral(object);
            if (!itsNames || (names && !(*names == *itsNames)))
                return;
            names = WTF::move(itsNames);
        }
    }
    if (names)
        all->note(code, WTF::move(*names));
}


void findThingsReturnedInRegisters(Graph& graph)
{
    if (!thingsReturnedByFunctions())
        return;
    if (auto* names = thingsReturnedInRegistersBy(graph.codeBlock(), graph.facts())) {
        for (BasicBlock* block : graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isBytecode(op_ret) || node->graph != &graph)
                    continue;
                Node* object = node->use(node->as<OpRet>().m_value);
                RELEASE_ASSERT(object->isBytecode(op_new_object) && object->numberOfLiteralProperties == names->size());
                object->isElided = true;
            }
        }
        graph.numberOfThingsReturnedInRegisters = names->size();
    }
    std::optional<UsersOfNodes> users;
    for (BasicBlock* block : graph.m_rpo) {
        for (Node* node : block->nodes) {
            if (!node->isBytecode(op_call) || node->isElided)
                continue;
            bool isProven = false;
            const KnownFunction* known = graph.knownCallee(node, &isProven);
            if (!known || !isProven || !known->forCall)
                continue;
            auto* names = thingsReturnedInRegistersBy(known->forCall, known->facts);
            if (!names)
                continue;
            if (!users)
                users.emplace(graph);
            auto onlyRead = users->isOnlyRead(node, names->span(), 0);
            // (Or it would have said that the object is wanted.)
            RELEASE_ASSERT(onlyRead);
            node->numberOfThingsReturned = names->size();
            auto& reads = graph.readsOfThingsReturned.add(node, Vector<Node*, 8> { }).iterator->value;
            for (auto [read, index] : onlyRead->reads) {
                read->kind = NodeKind::Proj;
                read->uses.shrink(0);
                read->uses.append({ VirtualRegister(), node });
                read->whichThing = index;
                reads.append(read);
            }
            for (Node* test : onlyRead->tests) {
                for (auto& use : test->uses) {
                    if (onlyRead->handedOn.contains(use.node))
                        use.node = graph.constant(jsBoolean(true));
                }
            }
            for (Node* alias : onlyRead->handedOn) {
                if (alias != node)
                    alias->isElided = true;
            }
        }
    }
}


void doWithoutObjectsThatAreOnlyRead(Graph& graph)
{
    auto resolve = [](Node* node) {
        while (node->replacement)
            node = node->replacement;
        return node;
    };
    // (Doing without one may be what it takes to do without another that was in it.)
    for (bool changed = true; changed;) {
        changed = false;
        std::optional<UsersOfNodes> users;
        for (BasicBlock* block : graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (!node->isBytecode(op_new_object) || !node->numberOfLiteralProperties || node->isElided)
                    continue;
                auto& instructions = node->graph->codeBlock()->instructions();
                auto stores = Graph::storesOfLiteral(instructions, node->bytecodeIndex.offset());
                RELEASE_ASSERT(stores.size() >= node->numberOfLiteralProperties);
                Vector<UniquedStringImpl*, 8> names;
                bool hasOneTwice = false;
                for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i) {
                    UniquedStringImpl* name = node->graph->codeBlock()->identifier(instructions.at(stores[i])->as<OpPutById>().m_property).impl();
                    hasOneTwice |= names.contains(name);
                    names.append(name);
                }
                if (hasOneTwice)
                    continue;
                if (!users)
                    users.emplace(graph);
                auto onlyRead = users->isOnlyRead(node, names.span(), Graph::layoutIDOfNewObject(node));
                if (!onlyRead)
                    continue;
                for (auto [read, index] : onlyRead->reads) {
                    read->replacement = node->use(NewObjectPlan::registerOf(index));
                    read->isElided = true;
                }
                for (Node* test : onlyRead->tests) {
                    for (auto& use : test->uses) {
                        if (onlyRead->handedOn.contains(use.node))
                            use.node = graph.constant(jsBoolean(true));
                    }
                }
                for (Node* alias : onlyRead->handedOn)
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

void analyzeEscapes(Graph& graph)
{
    EscapeAnalysis analysis(graph, nullptr);
    analysis.lookAtWhatIsMade();
}

uint32_t parametersThatEscape(Graph& graph, Vector<const KnownFunction*>* calleesConsulted)
{
    return EscapeAnalysis(graph, calleesConsulted).parametersThatEscape();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
