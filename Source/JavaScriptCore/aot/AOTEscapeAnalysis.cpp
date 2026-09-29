/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

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
    case Escape::HeldByWhatEscapes:
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

// Roughly.
unsigned bytesOfAllocation(const Node* node)
{
    switch (node->opcode) {
    case op_new_object:
        return roundUpToMultipleOf<16>(16 + std::max<unsigned>(node->as<OpNewObject>().m_inlineCapacity, node->numberOfLiteralProperties) * 8);
    case op_new_array:
        return 16 + roundUpToMultipleOf<16>(8 + std::max<unsigned>(node->as<OpNewArray>().m_argc, 3) * 8);
    case op_new_array_buffer:
        return 16;
    case op_new_array_with_size:
    case op_new_array_with_spread:
    case op_create_rest:
        return 16 + 48;
    case op_new_func:
    case op_new_func_exp:
        return 32;
    case op_create_lexical_environment: {
        JSValue table = node->graph->codeBlock()->getConstant(node->as<OpCreateLexicalEnvironment>().m_symbolTable);
        unsigned variables = table.isCell() ? uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize() : 1;
        return roundUpToMultipleOf<16>(24 + variables * 8);
    }
    default:
        return 0;
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
        enum Kind : uint8_t { Harmless, IsTheSame, Lent, Held, Escapes } kind { Harmless };
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
                case Verdict::Held: {
                    Escape ofHolder = fateOf(user.node, false);
                    if (!stays(ofHolder)) {
                        result = Escape::HeldByWhatEscapes;
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
            return reg == user->as<OpCreateLexicalEnvironment>().m_scope ? Verdict { Verdict::Held } : escapes(Escape::Other);
        case op_new_func_exp:
        case op_new_func: {
            UnlinkedFunctionExecutable* executable = user->opcode == op_new_func ? user->graph->codeBlock()->functionDecl(user->as<OpNewFunc>().m_functionDecl) : user->graph->codeBlock()->functionExpr(user->as<OpNewFuncExp>().m_functionDecl);
            UnlinkedFunctionCodeBlock* code = executable->codeBlockIfThereIsOne(CodeSpecializationKind::CodeForCall);
            if (!code || !keepsItsScopeToItself(code))
                return escapes(Escape::ClosureLetsScopeOut);
            return { Verdict::Held };
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

// TEMPORARY-ESCAPE-STATS
static std::atomic<uint64_t> s_sites[numberOfAllocationKinds][static_cast<unsigned>(Escape::NumberOfThem)];

void analyzeEscapes(Graph& graph)
{
    EscapeAnalysis analysis(graph, nullptr);
    analysis.lookAtWhatIsMade();
    if (Options::aotReportStats()) [[unlikely]] {
        for (BasicBlock* block : graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (auto kind = kindOfAllocation(node); kind && !node->isElided)
                    s_sites[static_cast<unsigned>(*kind)][static_cast<unsigned>(node->escape)].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

uint32_t parametersThatEscape(Graph& graph, Vector<const KnownFunction*>* calleesConsulted)
{
    return EscapeAnalysis(graph, calleesConsulted).parametersThatEscape();
}

void reportEscapeStatistics()
{
    for (unsigned kind = 0; kind < numberOfAllocationKinds; ++kind) {
        for (unsigned escape = 0; escape < static_cast<unsigned>(Escape::NumberOfThem); ++escape) {
            if (uint64_t count = s_sites[kind][escape].load())
                dataLogLn("  ESCAPESITES ", nameOf(static_cast<AllocationKind>(kind)), " ", count, " ", nameOf(static_cast<Escape>(escape)));
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
