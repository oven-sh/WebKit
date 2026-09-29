/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiler.h"

#if ENABLE(FTL_JIT)

#include "AOTImage.h"
#include "AOTLowering.h"
#include "AOTProgram.h"
#include "AOTStubs.h"
#include "AirCode.h"
#include "AirGenerate.h"
#include "AirInstInlines.h"
#include "B3BasicBlockInlines.h"
#include "B3Generate.h"
#include "B3NaturalLoops.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "CodeBlock.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "JSModuleEnvironment.h"
#include "JSWithScope.h"
#include "LinkBuffer.h"
#include "AOTTypeTable.h"
#include <wtf/Lock.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringHash.h>

namespace JSC { namespace AOT {

namespace {

struct Statistics {
    Lock lock;
    unsigned compiled { 0 };
    unsigned declined { 0 };
    size_t codeBytes { 0 };
    size_t bytecodeBytes { 0 };
    Seconds time;
    UncheckedKeyHashMap<String, unsigned> reasons;
    // By origin (see Lowering::lowerBlock): how many nodes, and how many bytes of code.
    Vector<std::pair<uint64_t, uint64_t>> byOrigin;
    uint64_t scopeReads[5] { }; // op_get_from_scope by name, by Graph::StaticVariable::Kind.
    uint64_t scopeReadsInGlobalScopes { 0 };
    uint64_t slots { 0 };
    uint64_t intrinsicReads { 0 };
    UncheckedKeyHashMap<String, uint64_t> provability; // TEMPORARY-PROVABILITY-STATS
};

Statistics& statistics()
{
    static NeverDestroyed<Statistics> stats;
    return stats;
}

} // anonymous namespace

static ScopeChain unknownScopeChain()
{
    ScopeChain chain;
    ScopeChainEntry entry;
    entry.kind = ScopeChainEntry::Unknown;
    chain.append(entry);
    return chain;
}

// How often each block runs, next to the others. It is what the register allocator goes by when not everything can have a register,
// or two moves cannot both be done away with; and with no profile it has to come from the shape of the code. The lowering has said
// which blocks it made for what seldom happens.
static void estimateFrequencies(B3::Procedure& proc)
{
    proc.resetReachability();
    auto& loops = proc.naturalLoops();

    // Got to without taking a branch that is rarely taken.
    IndexSet<B3::BasicBlock*> likely;
    Vector<B3::BasicBlock*, 32> worklist;
    auto visit = [&](B3::BasicBlock* block) {
        if (block->frequency() >= 1 && likely.add(block))
            worklist.append(block);
    };
    visit(proc[0]);
    while (!worklist.isEmpty()) {
        for (auto& successor : worklist.takeLast()->successors()) {
            if (successor.frequency() != B3::FrequencyClass::Rare)
                visit(successor.block());
        }
    }

    for (B3::BasicBlock* block : proc) {
        constexpr unsigned depthThatCounts = 6;
        double frequency = pow(10.0, std::min(loops.loopDepth(block), depthThatCounts));
        block->setFrequency(likely.contains(block) ? frequency : frequency * Lowering::coldFrequency);
    }
}

// TEMPORARY-PROVABILITY-STATS: how much there is to know about the program without running it.
static thread_local ASCIILiteral t_originForStatistics = ""_s;
void setOriginForStatistics(ASCIILiteral origin) { t_originForStatistics = origin; }

static Node* withoutWhatOnlyLooks(Node* node)
{
    for (;;) {
        if (node->kind == NodeKind::Narrow)
            node = node->uses[0].node;
        else if (node->isBytecode(op_check_type))
            node = node->use(node->as<OpCheckType>().m_value);
        else if (node->isBytecode(op_to_this))
            node = node->use(node->as<OpToThis>().m_srcDst);
        else if (node->isBytecode(op_check_tdz))
            node = node->use(node->as<OpCheckTdz>().m_targetVirtualRegister);
        else
            return node;
    }
}

static String whereItIsFrom(Node* node)
{
    node = withoutWhatOnlyLooks(node);
    switch (node->kind) {
    case NodeKind::Constant:
    case NodeKind::ConstantCell:
    case NodeKind::LinkTimeConstant:
        return "a constant"_s;
    case NodeKind::Intrinsic:
        return "an intrinsic"_s;
    case NodeKind::Argument:
        if (node->reg == virtualRegisterForArgumentIncludingThis(0))
            return "this"_s;
        if (node->reg == VirtualRegister(CallFrameSlot::callee))
            return "the callee"_s;
        return "a parameter"_s;
    case NodeKind::Phi:
        return "a phi"_s;
    case NodeKind::Proj:
        return "a proj"_s;
    case NodeKind::GetStack:
        return "a variable in memory"_s;
    case NodeKind::Bytecode:
        break;
    default:
        return "other"_s;
    }
    switch (node->opcode) {
    case op_get_by_id:
        return "a property"_s;
    case op_get_by_val:
        return "an element"_s;
    case op_get_from_scope: {
        auto bytecode = node->as<OpGetFromScope>();
        ResolveType type = bytecode.m_getPutInfo.resolveType();
        if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar)
            return "a closure variable"_s;
        auto variable = node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
        if (variable.isInGlobalScopes)
            return "a global"_s;
        switch (variable.kind) {
        case Graph::StaticVariable::Closure: return "a closure variable"_s;
        case Graph::StaticVariable::Import: return "a linked import"_s;
        case Graph::StaticVariable::ModuleImport: return "another import"_s;
        default: return "an unresolved variable"_s;
        }
    }
    case op_call:
    case op_call_ignore_result:
    case op_tail_call:
    case op_call_varargs:
        return "the result of a call"_s;
    case op_construct:
        return "the result of new"_s;
    case op_new_object:
    case op_create_this:
        return "an object made here"_s;
    case op_new_array:
    case op_new_array_buffer:
    case op_new_array_with_size:
    case op_new_array_with_spread:
        return "an array made here"_s;
    case op_new_func:
    case op_new_func_exp:
    case op_new_async_func:
    case op_new_async_func_exp:
    case op_new_generator_func:
    case op_new_generator_func_exp:
        return "a function made here"_s;
    default:
        return makeString("op "_s, opcodeNames[node->opcode]);
    }
}

// Options::aotWritesMap(): of each place in the bytecode that code may call a stub for, what was known there. For finding out, of the places a profile says are
// slow, what the compiler had to go on and what it made of it.
static CString describeSite(Node* node, bool isCompact, bool withName)
{
    BasicBlock* block = node->block;
    StringPrintStream out;
    out.print(opcodeNames[node->opcode], "\t", isCompact ? "compact" : "full", block->isGeneric ? " generic" : "", node->guard ? " guarded" : "", node->graph->inlineFrame() ? " inlined" : "");
    auto operand = [&](VirtualRegister reg) {
        Node* value = node->use(reg);
        out.print("\t", whereItIsFrom(value), "\t", TypeDump(value->type));
    };
    auto property = [&](VirtualRegister base, unsigned identifier) {
        operand(base);
        UniquedStringImpl* name = node->graph->codeBlock()->identifier(identifier).impl();
        uint32_t tag = Graph::typeTagOf(node);
        if (!withName)
            out.print("\t", !tag ? "no tag" : !TypeTable::shared() ? "tag" : !TypeTable::shared()->fieldOf(tag, name) ? "tag, no field" : TypeTable::shared()->fieldOf(tag, name)->holds.saysSomething() ? "tag, field, holds known" : "tag, field");
        else {
            out.print("\ttag ", tag);
            if (tag && TypeTable::shared()) {
                if (auto field = TypeTable::shared()->fieldOf(tag, name))
                    out.print(" slot ", field->slot, " layouts ", field->first, "-", field->last, " holds ", field->holds.kinds, " ", field->holds.first, "-", field->holds.last);
                else
                    out.print(" nofield reason ", TypeTable::shared()->reasonOf(tag));
            }
            out.print("\t", StringView(name));
        }
    };
    switch (node->opcode) {
    case op_get_by_id: { auto bytecode = node->as<OpGetById>(); property(bytecode.m_base, bytecode.m_property); break; }
    case op_put_by_id: { auto bytecode = node->as<OpPutById>(); property(bytecode.m_base, bytecode.m_property); break; }
    case op_in_by_id: { auto bytecode = node->as<OpInById>(); property(bytecode.m_base, bytecode.m_property); break; }
    case op_get_length: operand(node->as<OpGetLength>().m_base); break;
    case op_get_by_val: { auto bytecode = node->as<OpGetByVal>(); operand(bytecode.m_base); operand(bytecode.m_property); break; }
    case op_put_by_val: { auto bytecode = node->as<OpPutByVal>(); operand(bytecode.m_base); operand(bytecode.m_property); break; }
    case op_in_by_val: { auto bytecode = node->as<OpInByVal>(); operand(bytecode.m_base); operand(bytecode.m_property); break; }
    case op_stricteq: { auto bytecode = node->as<OpStricteq>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_nstricteq: { auto bytecode = node->as<OpNstricteq>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_jstricteq: { auto bytecode = node->as<OpJstricteq>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_jnstricteq: { auto bytecode = node->as<OpJnstricteq>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_eq: { auto bytecode = node->as<OpEq>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_add: { auto bytecode = node->as<OpAdd>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_less: { auto bytecode = node->as<OpLess>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_jless: { auto bytecode = node->as<OpJless>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_jnless: { auto bytecode = node->as<OpJnless>(); operand(bytecode.m_lhs); operand(bytecode.m_rhs); break; }
    case op_call: operand(node->as<OpCall>().m_callee); break;
    case op_call_ignore_result: operand(node->as<OpCallIgnoreResult>().m_callee); break;
    case op_tail_call: operand(node->as<OpTailCall>().m_callee); break;
    case op_construct: operand(node->as<OpConstruct>().m_callee); break;
    case op_iterator_open: operand(node->as<OpIteratorOpen>().m_iterable); break;
    case op_iterator_next: operand(node->as<OpIteratorNext>().m_iterable); break;
    case op_jtrue: operand(node->as<OpJtrue>().m_condition); break;
    case op_jfalse: operand(node->as<OpJfalse>().m_condition); break;
    case op_not: operand(node->as<OpNot>().m_operand); break;
    case op_jundefined_or_null: operand(node->as<OpJundefinedOrNull>().m_value); break;
    case op_jnundefined_or_null: operand(node->as<OpJnundefinedOrNull>().m_value); break;
    case op_typeof: operand(node->as<OpTypeof>().m_value); break;
    case op_to_string: operand(node->as<OpToString>().m_operand); break;
    case op_check_type: operand(node->as<OpCheckType>().m_value); break;
    default: break;
    }
    return out.toCString();
}

// Options::aotWritesMap(): of each place in the bytecode that code may call a stub for, what was known there.
static void describeSites(Graph& graph, Vector<std::pair<uint32_t, CString>>& notes)
{
    for (BasicBlock* block : graph.m_rpo) {
        bool isCompact = (!block->isInLoop && !graph.callsItself) || block->isGeneric;
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode || !node->instruction)
                continue;
            uint32_t site = CallSiteIndex(node->bytecodeIndex).bits();
            if (!graph.inlineFrames.isEmpty())
                site = PackedSite::pack(node->graph->inlineFrame(), site);
            notes.append({ site, describeSite(node, isCompact, true) });
        }
    }
}

// TEMPORARY-SITE-COUNTS (Options::aotCountsAllocations()): how often each kind of thing is done, by what was known where it is done. The kinds are numbered as they turn up.
static Lock s_kindsOfSitesLock;
static UncheckedKeyHashMap<CString, unsigned>& kindsOfSites()
{
    static NeverDestroyed<UncheckedKeyHashMap<CString, unsigned>> kinds;
    return kinds;
}
unsigned kindOfSite(Node* node, bool isCompact)
{
    CString description = describeSite(node, isCompact, false);
    Locker locker { s_kindsOfSitesLock };
    auto& kinds = kindsOfSites();
    if (auto it = kinds.find(description); it != kinds.end())
        return it->value;
    if (kinds.size() + 1 >= Instance::numberOfCountsOfSites)
        return 0;
    unsigned number = kinds.size() + 1;
    kinds.add(description, number);
    return number;
}
void dumpKindsOfSites()
{
    Locker locker { s_kindsOfSitesLock };
    for (auto& entry : kindsOfSites())
        dataLogLn("SITEKIND\t", entry.value, "\t", entry.key);
}

static void countWhatIsKnown(Graph& graph, UncheckedKeyHashMap<String, uint64_t>& counters)
{
    UnlinkedCodeBlock* codeBlock = graph.codeBlock();
    auto count = [&](const String& what) {
        counters.add(what, 0).iterator->value++;
        counters.add(makeString("BY "_s, t_originForStatistics, ' ', what), 0).iterator->value++;
    };
    count("FUNCTIONS"_s);
    auto strip = [&](Node* node) { return withoutWhatOnlyLooks(node); };
    auto origin = [&](Node* node) { return whereItIsFrom(node); };
    auto typeName = [&](Type type) -> ASCIILiteral {
        if (!type) return "nothing"_s;
        if (type & ~(TOther | TEmpty))
            type &= ~(TOther | TEmpty);
        if (isSubtype(type, TInt32)) return "int32"_s;
        if (isSubtype(type, TNumber)) return "number"_s;
        if (isSubtype(type, TString)) return "string"_s;
        if (isSubtype(type, TBoolean)) return "boolean"_s;
        if (isSubtype(type, TFunction)) return "function"_s;
        if (isSubtype(type, TArray)) return "array"_s;
        if (isSubtype(type, TTypedArray)) return "typed array"_s;
        if (isSubtype(type, TAnyObject)) return "some object"_s;
        if (isSubtype(type, TPrimitive)) return "some primitive"_s;
        return "anything"_s;
    };

    // Who uses what.
    UncheckedKeyHashMap<Node*, Vector<std::pair<Node*, VirtualRegister>, 4>> users;
    for (BasicBlock* block : graph.m_rpo) {
        if (block->isGeneric)
            continue;
        for (Node* node : block->nodes) {
            for (auto& use : node->uses)
                users.add(strip(use.node), Vector<std::pair<Node*, VirtualRegister>, 4>()).iterator->value.append({ node, use.reg });
        }
        for (Node* phi : block->phis) {
            for (auto& use : phi->uses)
                users.add(strip(use.node), Vector<std::pair<Node*, VirtualRegister>, 4>()).iterator->value.append({ phi, use.reg });
        }
    }
    auto calleeRegisterOf = [&](Node* node) -> std::optional<VirtualRegister> {
        switch (node->kind == NodeKind::Bytecode ? node->opcode : op_nop) {
        case op_call: return node->as<OpCall>().m_callee;
        case op_call_ignore_result: return node->as<OpCallIgnoreResult>().m_callee;
        case op_tail_call: return node->as<OpTailCall>().m_callee;
        case op_construct: return node->as<OpConstruct>().m_callee;
        default: return std::nullopt;
        }
    };
    // Where a value that is made here goes.
    auto fate = [&](Node* made) -> String {
        bool isArgument = false, isStored = false, isReturned = false, isMerged = false, isOther = false, isCaptured = false;
        String argumentOf;
        auto it = users.find(made);
        if (it == users.end())
            return "is not used"_s;
        for (auto [user, reg] : it->value) {
            if (user->kind == NodeKind::Guard || user->kind == NodeKind::Narrow)
                continue;
            if (user->kind == NodeKind::Phi) { isMerged = true; continue; }
            if (user->kind == NodeKind::SetStack) { isCaptured = true; continue; }
            if (user->kind != NodeKind::Bytecode) { isOther = true; continue; }
            if (auto calleeRegister = calleeRegisterOf(user)) {
                if (reg == *calleeRegister)
                    continue; // Called.
                isArgument = true;
                Node* callee = strip(user->use(*calleeRegister));
                if (graph.knownCallee(user))
                    argumentOf = "a known function"_s;
                else if (callee->isBytecode(op_get_by_id))
                    argumentOf = makeString("method "_s, StringView(callee->graph->codeBlock()->identifier(callee->as<OpGetById>().m_property).impl()));
                else
                    argumentOf = origin(callee);
                continue;
            }
            switch (user->opcode) {
            case op_get_by_id:
            case op_get_by_val:
            case op_get_length:
            case op_check_type:
            case op_type_tag:
            case op_check_tdz:
            case op_to_this:
            case op_jtrue: case op_jfalse: case op_typeof: case op_is_object: case op_instanceof: case op_in_by_id:
                break; // Looked at.
            case op_put_by_id:
                if (reg == user->as<OpPutById>().m_value) isStored = true;
                break;
            case op_put_by_val:
                if (reg == user->as<OpPutByVal>().m_value) isStored = true;
                break;
            case op_put_to_scope:
                isCaptured = true;
                break;
            case op_ret:
                isReturned = true;
                break;
            case op_new_object: case op_create_this: case op_new_array:
                isStored = true; // Part of a literal.
                break;
            default:
                isOther = true;
            }
        }
        if (isOther) return "goes somewhere else"_s;
        if (isCaptured) return "goes in a variable of a scope"_s;
        if (isStored) return "is stored in an object"_s;
        if (isReturned) return "is returned"_s;
        if (isMerged) return "is merged"_s;
        if (isArgument) return makeString("is only passed to "_s, argumentOf);
        return "stays here"_s;
    };

    for (BasicBlock* block : graph.m_rpo) {
        if (block->isGeneric)
            continue;
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode)
                continue;
            if (node->isBytecode(op_get_from_scope)) {
                auto bytecode = node->as<OpGetFromScope>();
                ResolveType type = bytecode.m_getPutInfo.resolveType();
                if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar && node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type).isGlobal)
                    count(makeString("GLOBALREAD "_s, StringView(node->graph->codeBlock()->identifier(bytecode.m_var).impl())));
            }
            for (auto& use : node->uses) {
                if (use.node->kind == NodeKind::Intrinsic && use.node->intrinsic) {
                    auto& entry = ImmutableIntrinsics::shared()->at(use.node->intrinsic);
                    count(makeString("INTRINSICUSE "_s, ImmutableIntrinsics::shared()->at(entry.holder).name, '.', entry.name));
                }
            }
            if (auto calleeRegister = calleeRegisterOf(node)) {
                Node* callee = strip(node->use(*calleeRegister));
                if (graph.knownCallee(node)) {
                    count("CALL known callee"_s);
                    count(graph.calleeIsProven(node) ? "PROOF proven"_s : "PROOF only a hint"_s);
                    if (graph.calleeIsProven(node))
                        count(!graph.passesNoFunctionObject(node) ? "PROOF proven, passes the function"_s : node->use(*calleeRegister)->isElided ? "PROOF proven, passes nothing, reads nothing"_s : "PROOF proven, passes nothing"_s);
                    count(makeString("KNOWNCALLRESULT "_s, typeName(node->type)));
                }
                else if (callee->isBytecode(op_get_by_id)) {
                    count("CALL method"_s);
                    count(makeString("METHODBASE "_s, origin(callee->use(callee->as<OpGetById>().m_base)), ", "_s, typeName(strip(callee->use(callee->as<OpGetById>().m_base))->type)));
                    count(makeString("METHODNAME "_s, StringView(callee->graph->codeBlock()->identifier(callee->as<OpGetById>().m_property).impl())));
                    count(makeString("METHODON "_s, typeName(strip(callee->use(callee->as<OpGetById>().m_base))->type)));
                    count(makeString("METHODOF "_s, typeName(strip(callee->use(callee->as<OpGetById>().m_base))->type), " . "_s, StringView(callee->graph->codeBlock()->identifier(callee->as<OpGetById>().m_property).impl())));
                } else {
                    count(makeString("CALL "_s, origin(callee)));
                    if (callee->kind == NodeKind::LinkTimeConstant)
                        count(makeString("CALLCONST cell link time constant "_s, callee->intrinsic));
                    else if (callee->kind == NodeKind::ConstantCell) {
                        bool isLinkTime = false;
                        JSValue value = callee->graph->codeBlock()->getConstant(callee->reg);
                        count(makeString("CALLCONST cell "_s, isLinkTime ? makeString("link time constant "_s, value.asInt32AsAnyInt()) : value.isCell() ? String::fromLatin1(value.asCell()->classInfo()->className.characters()) : "?"_s, codeBlock->isBuiltinFunction() ? " in a builtin"_s : ""_s));
                    } else if (callee->kind == NodeKind::Constant)
                        count(makeString("CALLCONST value "_s, callee->constant.isUndefined() ? "undefined"_s : callee->constant.isEmpty() ? "empty"_s : "other"_s, " after "_s, node->use(*calleeRegister)->kind == NodeKind::Bytecode ? opcodeNames[node->use(*calleeRegister)->opcode] : "nothing"_s));
                }
                continue;
            }
            switch (node->opcode) {
            case op_check_type:
                count(makeString("CHECK of "_s, origin(node->use(node->as<OpCheckType>().m_value)), " gives "_s, typeName(node->type)));
                break;
            case op_get_by_id:
                count(makeString("GETRESULT "_s, typeName(node->type)));
                count(makeString("GET base is "_s, origin(node->use(node->as<OpGetById>().m_base)), ", "_s, typeName(node->use(node->as<OpGetById>().m_base)->type)));
                break;
            case op_put_by_id:
                count(makeString("PUT base is "_s, origin(node->use(node->as<OpPutById>().m_base))));
                break;
            case op_add: case op_sub: case op_mul: case op_div: case op_mod:
            case op_less: case op_lesseq: case op_greater: case op_greatereq:
            case op_jless: case op_jlesseq: case op_jgreater: case op_jgreatereq: case op_jnless: case op_jnlesseq: case op_jngreater: case op_jngreatereq:
            case op_bitand: case op_bitor: case op_bitxor: case op_lshift: case op_rshift: case op_urshift:
            case op_inc: case op_dec: case op_negate: {
                bool numbers = true, strings = true;
                for (auto& use : node->uses) {
                    numbers &= isSubtype(use.node->type, TNumber);
                    strings &= isSubtype(use.node->type, TString);
                }
                count(makeString("ARITH "_s, node->opcode == op_add ? "add "_s : "other "_s, numbers ? "numbers"_s : strings ? "strings"_s : "not proven"_s));
                break;
            }
            case op_eq: case op_neq: case op_stricteq: case op_nstricteq: case op_jeq: case op_jneq: case op_jstricteq: case op_jnstricteq: {
                bool cheap = false;
                for (auto& use : node->uses)
                    cheap |= isSubtype(use.node->type, TInt32 | TBoolean | TOther | TAnyObject | TSymbol);
                count(makeString("EQUALITY "_s, cheap ? "one side settles it"_s : "not proven"_s));
                break;
            }
            case op_new_func_exp: case op_new_async_func_exp: case op_new_generator_func_exp:
                count(makeString("FUNCTION "_s, codeBlock->codeType() == ModuleCode ? "(module) "_s : ""_s, fate(node)));
                break;
            case op_new_object:
                count(makeString("OBJECT "_s, codeBlock->codeType() == ModuleCode ? "(module) "_s : ""_s, fate(node)));
                break;
            case op_new_array: case op_new_array_buffer: case op_new_array_with_spread:
                count(makeString("ARRAY "_s, codeBlock->codeType() == ModuleCode ? "(module) "_s : ""_s, fate(node)));
                break;
            case op_create_lexical_environment:
                count("SCOPE made"_s);
                break;
            default:
                break;
            }
        }
    }
}

// See CompiledFunctionInfo::startsCold.
static bool mayStartCold(UnlinkedCodeBlock* unlinkedCodeBlock)
{
    // (Nothing takes as many slots as it takes bytes.)
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->instructions().size() > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::mostSlotsSaid) || !constantsAreOfNoRealm(unlinkedCodeBlock, SymbolTablesWillDo::Yes))
        return false;
    for (const auto& instruction : unlinkedCodeBlock->instructions()) {
        if (instruction->opcodeID() == op_loop_hint)
            return false;
    }
    return true;
}

// What holds the same thing all the way through is used where it is: as a value it would be copied to another register on the way in, which
// would then have to be saved.
static void usePinnedRegistersWhereTheyAre(B3::Air::Code& code)
{
    using namespace B3::Air;
    Vector<std::pair<Tmp, Tmp>, 4> copies;
    for (Inst& inst : *code[0]) {
        if (inst.kind.opcode != Move || inst.args().size() != 2 || !inst.args()[0].isTmp() || !inst.args()[1].isTmp())
            continue;
        Tmp from = inst.args()[0].tmp();
        Tmp to = inst.args()[1].tmp();
        if (!from.isReg() || !code.isPinned(from.reg()) || from.reg() == Reg(MacroAssembler::framePointerRegister) || to.isReg())
            continue;
        copies.append({ to, from });
        inst = Inst();
    }
    if (copies.isEmpty())
        return;
    code[0]->insts().removeAllMatching([](const Inst& inst) { return !inst; });
    for (B3::Air::BasicBlock* block : code) {
        for (Inst& inst : *block) {
            inst.forEachTmpFast([&](Tmp& tmp) {
                for (auto& [copy, reg] : copies) {
                    if (tmp == copy)
                        tmp = reg;
                }
            });
        }
    }
}

// A function that calls nothing, keeps nothing on the stack and saves nothing has no frame.
bool hasNoFrame(const Graph& graph, B3::Air::Code& code)
{
    // (What is caught is caught in a frame.)
    if (code.frameSize() || code.calleeSaveRegisterAtOffsetList().registerCount() || graph.emitsCallsWhateverIsLeft || !graph.catchEntrypoints.isEmpty())
        return false;
    if (!graph.emitsCalls)
        return true;
    // What was written with a call in it may have turned out never to be reached. Whatever calls says that it overwrites the link register.
    if (!graph.callsAreLeft) {
        bool found = false;
        for (B3::Air::BasicBlock* block : code) {
            for (B3::Air::Inst& inst : *block) {
                if (inst.kind.opcode == B3::Air::Patch)
                    found |= !inst.origin || inst.origin->opcode() != B3::Patchpoint || inst.origin->as<B3::PatchpointValue>()->lateClobbered().contains(ARM64Registers::lr, IgnoreVectors);
                else
                    found |= inst.kind.opcode == B3::Air::ColdCCall;
            }
        }
        graph.callsAreLeft = found;
    }
    return !*graph.callsAreLeft;
}

void emitEpilogueBeforeLeaving(CCallHelpers& jit, const Graph& graph, B3::Air::Code& code)
{
    if (hasNoFrame(graph, code))
        return;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
    jit.emitFunctionEpilogue();
}

void emitRestoreBeforeLeaving(CCallHelpers& jit, const Graph& graph, B3::Air::Code& code)
{
    RELEASE_ASSERT(!hasNoFrame(graph, code));
    AllowMacroScratchRegisterUsage allowScratch(jit);
    jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
}

static bool compile(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, CompiledCode& result, ASCIILiteral& reason, OpcodeID& reasonOpcode, const ProgramFacts* facts, VariableFacts* variableFacts, const CodeOfProgram* program)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setFacts(facts);
    graph.setVariableFacts(variableFacts);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    graph.startsCold = mayStartCold(unlinkedCodeBlock);
    auto declined = [&] {
        reason = graph.failureReason();
        reasonOpcode = graph.failureOpcode();
        return false;
    };

    if (unlinkedCodeBlock->wasCompiledWithDebuggingOpcodes() || unlinkedCodeBlock->wasCompiledWithTypeProfilerOpcodes() || unlinkedCodeBlock->wasCompiledWithControlFlowProfilerOpcodes()) {
        graph.fail("compiled for the debugger or a profiler"_s);
        return declined();
    }
    if (!parseBytecode(graph))
        return declined();
    if (program)
        inlineCalls(graph, *program);
    inferTypes(graph);
    inferRanges(graph);
    optimizeLoops(graph);
    graph.elideReadsOfCalleesNotPassed();
    graph.findDirectMethods();
    graph.findListsOfArguments();
    analyzeEscapes(graph);
    if (Options::aotDumpGraph()) [[unlikely]] {
        dataLogLn("AOT graph:");
        graph.dump(WTF::dataFile());
    }

    B3::Procedure proc(/* usesSIMD = */ false);
    proc.setOptLevel(Options::aotB3OptLevel());
    proc.setPositionIndependent();
    proc.pinRegister(instanceGPR);
    proc.pinRegister(GPRInfo::numberTagRegister);
    proc.pinRegister(GPRInfo::notCellMaskRegister);
    if (Options::aotReportStats()) [[unlikely]]
        proc.setNeedsPCToOriginMap();
    Lowering lowering(graph, proc);
    if (!lowering.run())
        return declined();
    estimateFrequencies(proc);
    if (Options::aotDumpB3()) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);

    // The code is going to run in another process, where nothing is where it is here. No lowering should have put an address in
    // it; running the code only shows that for the paths that are taken, so look. (No JSValue that is not a cell is in this range.)
    for (B3::Value* value : proc.values()) {
        if (!value->hasInt64())
            continue;
        uint64_t bits = value->asInt64();
        if (bits >= 4 * GB && bits < (1ULL << 47) && !graph.wideIntegerConstants.contains(static_cast<int64_t>(bits))) {
            graph.fail("an address in the code"_s);
            return declined();
        }
    }

    StubCalls& stubCalls = graph.stubCalls;
    proc.code().setPrologueForEntrypoint(0, createSharedTask<B3::Air::PrologueGeneratorFunction>([&stubCalls, &graph](CCallHelpers& jit, B3::Air::Code& code) {
        if (hasNoFrame(graph, code))
            return;
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.emitFunctionPrologue();
        // The limit leaves room for the runtime to do what it has to when the stack is used up, which is a great deal more than this.
        // However deep the calls go, whatever made the last of them has checked.
        constexpr unsigned frameSizeThatNeedsNoCheck = 256;
        if (graph.makesCalls || code.frameSize() > frameSizeThatNeedsNoCheck)
            stubCalls.call(jit, Stub::Prologue, code.frameSize(), CallSite { });
        else if (code.frameSize())
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
        jit.emitSave(code.calleeSaveRegisterAtOffsetList());
    }));
    proc.code().setEpilogueGenerator(createSharedTask<B3::Air::PrologueGeneratorFunction>([&graph](CCallHelpers& jit, B3::Air::Code& code) {
        emitEpilogueBeforeLeaving(jit, graph, code);
        jit.ret();
    }));
    for (unsigned i = 0; i < graph.catchEntrypoints.size(); ++i) {
        // From catchThunk(): the frame pointer is this frame's again, and what this function saved on entry is still saved.
        proc.code().setPrologueForEntrypoint(i + 1, createSharedTask<B3::Air::PrologueGeneratorFunction>([](CCallHelpers& jit, B3::Air::Code& code) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        }));
    }

    B3::generateToAir(proc);
    usePinnedRegistersWhereTheyAre(proc.code());
    B3::Air::prepareForGeneration(proc.code());
    CCallHelpers jit;
    // The way in is what comes first.
    CCallHelpers::Jump toTheWayIn;
    if (!graph.catchEntrypoints.isEmpty())
        toTheWayIn = jit.jump();
    CCallHelpers::Label startOfCode = jit.label();
    jit.setOopsIsJustABreakpoint();
    B3::generate(proc, jit);
    if (toTheWayIn.isSet())
        toTheWayIn.linkTo(proc.code().entrypointLabel(0), &jit);
    else
        RELEASE_ASSERT(!CCallHelpers::differenceBetween(startOfCode, proc.code().entrypointLabel(0)));
    LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::FTL, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate()) {
        graph.fail("out of executable memory"_s);
        return declined();
    }

    CompiledFunctionInfo info;
    info.stubCalls = stubCalls.link(linkBuffer);
    info.indexReferences = graph.indexReferences.link(linkBuffer);
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    // (It turned out to be what comes first anyway.)
    if (toTheWayIn.isSet() && static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(proc.code().entrypointLabel(0)).untaggedPtr()) - static_cast<uint8_t*>(start) == sizeof(uint32_t)) {
        start = static_cast<uint8_t*>(start) + sizeof(uint32_t);
        info.codeSize -= sizeof(uint32_t);
        for (auto& call : info.stubCalls)
            call.offset -= sizeof(uint32_t);
        for (auto& reference : info.indexReferences)
            reference.offset -= sizeof(uint32_t);
    }
    auto offsetOf = [&](CCallHelpers::Label label) {
        return static_cast<unsigned>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(label).untaggedPtr()) - static_cast<uint8_t*>(start));
    };
    info.convention = graph.convention();
    info.frameSizeInBytes = proc.frameSize();
    info.numSlots = graph.numICSlots;
    info.sites = WTF::move(graph.sites);
    while (info.sites.size() < info.numSlots)
        info.sites.append(Site { });
    info.knownCallees = WTF::move(graph.knownCallees);
    info.siteConstants = WTF::move(graph.siteConstants);
    info.plans = WTF::move(graph.plans);
    if (program && mayBecomePartOfAnother(unlinkedCodeBlock, facts))
        noteEverySiteOf(graph);
    info.quotableSites = WTF::move(graph.quotableSites);
    std::ranges::sort(info.quotableSites);
    info.quotableSites.shrink(std::ranges::unique(info.quotableSites).begin() - info.quotableSites.begin());
    info.isOnlyCalledDirectly = facts && facts->isClosed;
    info.numberOfFunction = facts ? facts->number : 0;
    for (auto& frame : graph.inlineFrames)
        info.inlineFrames.append({ frame.parent, frame.callSite, frame.knownCallee, frame.isTailCall });
    info.sitesOfSpreads = WTF::move(graph.sitesOfSpreads);
    if (Options::aotWritesMap()) [[unlikely]]
        describeSites(graph, info.notesOfSites);
    info.callSites = WTF::move(graph.callSites);
    std::ranges::sort(info.callSites);
    info.callSites.shrink(std::ranges::unique(info.callSites).begin() - info.callSites.begin());
    while (info.siteConstants.size() < info.numSlots)
        info.siteConstants.append(0);
    info.selectors = WTF::move(graph.selectors);
    info.shapes = WTF::move(graph.shapes);
    info.usesStaticImports = graph.usesStaticImports;
    info.startsCold = graph.startsCold;
    RELEASE_ASSERT(!info.startsCold || info.numSlots <= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::mostSlotsSaid));
    info.calleeSaveRegisters = proc.calleeSaveRegisterAtOffsetList();
    for (unsigned i = 0; i < graph.catchEntrypoints.size(); ++i)
        info.catchEntrypoints.append({ graph.catchEntrypoints[i]->bytecodeBegin, offsetOf(proc.code().entrypointLabel(i + 1)) });

    if (Options::aotReportStats()) [[unlikely]] {
        B3::PCToOriginMap originMap = proc.releasePCToOriginMap();
        auto& ranges = originMap.ranges();
        auto& stats = statistics();
        Locker locker { stats.lock };
        if (stats.byOrigin.isEmpty())
            stats.byOrigin.fill({ 0, 0 }, numOpcodeIDs + 16);
        unsigned accounted = 0;
        for (unsigned i = 0; i < ranges.size(); ++i) {
            unsigned begin = offsetOf(ranges[i].label);
            unsigned end = i + 1 < ranges.size() ? offsetOf(ranges[i + 1].label) : info.codeSize;
            uintptr_t tag = std::bit_cast<uintptr_t>(ranges[i].origin.dfgOrigin()) >> 4;
            stats.byOrigin[tag].second += end - begin;
            accounted += end - begin;
        }
        stats.byOrigin[0].second += info.codeSize - accounted;
        for (BasicBlock* block : graph.m_rpo) {
            for (Node* node : block->nodes) {
                stats.byOrigin[node->kind == NodeKind::Bytecode ? node->opcode + 1 : numOpcodeIDs + 1 + static_cast<unsigned>(node->kind)].first++;
                if (node->isBytecode(op_get_from_scope) && !block->isGeneric) {
                    auto bytecode = node->as<OpGetFromScope>();
                    ResolveType type = bytecode.m_getPutInfo.resolveType();
                    if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar) {
                        auto variable = node->graph->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
                        stats.scopeReads[variable.kind]++;
                        stats.scopeReadsInGlobalScopes += variable.isInGlobalScopes;
                    }
                }
            }
        }
        stats.slots += info.numSlots;
        stats.intrinsicReads += graph.numberOfIntrinsicReads;
        stats.byOrigin[0].first++;
        countWhatIsKnown(graph, stats.provability);
    }

    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::aotDumpDisassembly(), linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
#if CPU(ARM64)
    // What the JIT's memory is handed out in multiples of is made up with these. One stays if what is before it is a call: where that
    // would come back to says whose frame it is, and what comes after the function is another function.
    {
        constexpr uint32_t breakpoint = 0xd4200000;
        unsigned atLeast = sizeof(uint32_t);
        for (auto& call : info.stubCalls) {
            if (!call.isTailCall)
                atLeast = std::max<unsigned>(atLeast, call.offset + 2 * sizeof(uint32_t));
        }
        while (info.codeSize > atLeast && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == breakpoint)
            info.codeSize -= sizeof(uint32_t);
    }
#endif
    // Out of the JIT's memory, which goes back to the JIT.
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    result.info = WTF::move(info);
    return true;
}

static void recordStatistics(bool ok, size_t codeBytes, size_t bytecodeBytes, Seconds time, ASCIILiteral reason, OpcodeID reasonOpcode)
{
    static std::once_flag once;
    std::call_once(once, [] {
        atexit(reportStatistics);
    });
    auto& stats = statistics();
    Locker locker { stats.lock };
    stats.time += time;
    if (ok) {
        stats.compiled++;
        stats.codeBytes += codeBytes;
        stats.bytecodeBytes += bytecodeBytes;
    } else {
        stats.declined++;
        stats.reasons.add(makeString(reason, ' ', reasonOpcode != op_nop ? opcodeNames[reasonOpcode] : ""_s), 0).iterator->value++;
    }
}

bool noteUsesOfProvenFunctionsForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, const FactsOfExecutables& factsOfExecutables, VariableFacts* variableFacts)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    if (!parseBytecode(graph))
        return false;
    graph.noteUsesOfProvenFunctions(factsOfExecutables);
    if (variableFacts)
        graph.noteWhatCannotBeToldOfVariables(*variableFacts);
    return true;
}

Type inferReturnTypeForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, const ProgramFacts* facts, VariableFacts* variableFacts, unsigned readerOfFacts, Vector<const KnownFunction*>& calleesConsulted, Vector<const KnownFunction*>& calleesGivenMore, uint32_t& parametersThatEscape, const String& nameForLog)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    graph.setFacts(facts);
    graph.setVariableFacts(variableFacts, readerOfFacts);
    graph.setNameForLog(nameForLog);
    if (!parseBytecode(graph)) {
        parametersThatEscape = std::numeric_limits<uint32_t>::max();
        return TTop;
    }
    Type result = inferTypes(graph, &calleesConsulted, &calleesGivenMore) & TTop;
    parametersThatEscape = facts ? AOT::parametersThatEscape(graph, &calleesConsulted) : std::numeric_limits<uint32_t>::max();
    return result;
}

bool compileForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, CompiledCode& result, const CalleeHints* hints, const ModuleLinkage* linkage, const ProgramFacts* facts, VariableFacts* variableFacts, const CodeOfProgram* program)
{
    MonotonicTime before = MonotonicTime::now();
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    bool ok = compile(vm, unlinkedCodeBlock, hints, linkage, result, reason, reasonOpcode, facts, variableFacts, program);
    if (Options::aotReportStats()) [[unlikely]]
        recordStatistics(ok, ok ? result.bytes.size() : 0, unlinkedCodeBlock->instructionsSize(), MonotonicTime::now() - before, reason, reasonOpcode);
    return ok;
}

void reportStatistics()
{
    auto& stats = statistics();
    Locker locker { stats.lock };
    dataLogLn("AOT: compiled ", stats.compiled, " functions (", stats.codeBytes, " bytes of code for ", stats.bytecodeBytes, " of bytecode) in ", stats.time.milliseconds(), " ms; declined ", stats.declined);
    dataLogLn("AOT: ", stats.intrinsicReads, " reads of immutable intrinsics; ", stats.slots, " slots");
    reportEscapeStatistics();
    reportShapeStatistics();
    dataLogLn("AOT: reads of variables by name: ", stats.scopeReads[Graph::StaticVariable::Closure], " closure, ", stats.scopeReads[Graph::StaticVariable::Import], " linked imports, ", stats.scopeReads[Graph::StaticVariable::ModuleImport], " other imports, ", stats.scopeReadsInGlobalScopes, " globals, ", stats.scopeReads[Graph::StaticVariable::Unresolved] - stats.scopeReadsInGlobalScopes, " unresolved, ", stats.scopeReads[Graph::StaticVariable::Dynamic], " dynamic; ", stats.slots, " slots");
    Vector<std::pair<unsigned, String>> sorted;
    for (auto& entry : stats.reasons)
        sorted.append({ entry.value, entry.key });
    std::ranges::sort(sorted, [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& entry : sorted)
        dataLogLn("    ", entry.first, "  ", entry.second);
    {
        Vector<std::pair<uint64_t, String>> known;
        for (auto& entry : stats.provability)
            known.append({ entry.value, entry.key });
        std::ranges::sort(known, [](auto& a, auto& b) { return a.first > b.first; });
        for (auto& entry : known) {
            if (entry.first >= 200)
                dataLogLn("  KNOWN ", entry.first, " ", entry.second);
        }
    }
    for (unsigned tag = 0; tag < stats.byOrigin.size(); ++tag) {
        auto [count, bytes] = stats.byOrigin[tag];
        if (!count && !bytes)
            continue;
        static constexpr ASCIILiteral kinds[] = { "Bytecode"_s, "Constant"_s, "ConstantCell"_s, "Intrinsic"_s, "Argument"_s, "Phi"_s, "Proj"_s, "GetStack"_s, "SetStack"_s, "Guard"_s, "Narrow"_s };
        ASCIILiteral name = !tag ? "(function)"_s : tag <= numOpcodeIDs ? opcodeNames[tag - 1] : kinds[tag - numOpcodeIDs - 1];
        dataLogLn("  SIZE ", name, " ", count, " ", bytes);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
