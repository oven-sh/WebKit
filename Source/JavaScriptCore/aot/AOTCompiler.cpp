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
#include <wtf/Lock.h>
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

void loadInstance(CCallHelpers& jit, CCallHelpers::Address codeBlockSlot, GPRReg instanceGPR)
{
    jit.loadPtr(codeBlockSlot, instanceGPR);
}

void jumpToThunk(CCallHelpers& jit, GPRReg dataGPR, Entry entry)
{
    jit.loadPtr(CCallHelpers::Address(dataGPR, Instance::offsetOfRuntimeTable()), dataGPR);
    jit.loadPtr(CCallHelpers::Address(dataGPR, static_cast<unsigned>(entry) * sizeof(void*)), dataGPR);
    jit.farJump(dataGPR, JITThunkPtrTag);
}

} // anonymous namespace

ScopeChain scopeChainFor(JSScope* scope)
{
    ScopeChain chain;
    for (; scope; scope = scope->next()) {
        ScopeChainEntry entry;
        if (scope->isJSLexicalEnvironment()) {
            entry.kind = ScopeChainEntry::Lexical;
            entry.symbolTable = uncheckedDowncast<JSLexicalEnvironment>(scope)->symbolTable();
            entry.isModule = scope->type() == ModuleEnvironmentType;
        } else if (scope->isGlobalLexicalEnvironment())
            entry.kind = ScopeChainEntry::GlobalLexical;
        else if (scope->isGlobalObject())
            entry.kind = ScopeChainEntry::Global;
        chain.append(entry);
        if (entry.kind != ScopeChainEntry::Lexical)
            break;
    }
    return chain;
}

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

static void countWhatIsKnown(Graph& graph, UncheckedKeyHashMap<String, uint64_t>& counters)
{
    UnlinkedCodeBlock* codeBlock = graph.codeBlock();
    auto count = [&](const String& what) {
        counters.add(what, 0).iterator->value++;
        counters.add(makeString("BY "_s, t_originForStatistics, ' ', what), 0).iterator->value++;
    };
    count("FUNCTIONS"_s);
    auto strip = [&](Node* node) {
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
    };
    auto origin = [&](Node* node) -> String {
        node = strip(node);
        switch (node->kind) {
        case NodeKind::Constant:
        case NodeKind::ConstantCell:
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
            auto variable = graph.resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
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
    };
    auto typeName = [&](Type type) -> ASCIILiteral {
        if (!type) return "nothing"_s;
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
                    argumentOf = makeString("method "_s, StringView(codeBlock->identifier(callee->as<OpGetById>().m_property).impl()));
                else
                    argumentOf = origin(callee);
                continue;
            }
            switch (user->opcode) {
            case op_get_by_id:
            case op_get_by_val:
            case op_get_length:
            case op_check_type:
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
                if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar && graph.resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type).isGlobal)
                    count(makeString("GLOBALREAD "_s, StringView(codeBlock->identifier(bytecode.m_var).impl())));
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
                    count(makeString("METHODNAME "_s, StringView(codeBlock->identifier(callee->as<OpGetById>().m_property).impl())));
                } else {
                    count(makeString("CALL "_s, origin(callee)));
                    if (callee->kind == NodeKind::ConstantCell) {
                        bool isLinkTime = codeBlock->constantSourceCodeRepresentation(callee->reg) == SourceCodeRepresentation::LinkTimeConstant;
                        JSValue value = codeBlock->getConstant(callee->reg);
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
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->instructions().size() > SharedData::maxSlots || !constantsAreOfNoRealm(unlinkedCodeBlock, SymbolTablesWillDo::Yes))
        return false;
    for (const auto& instruction : unlinkedCodeBlock->instructions()) {
        if (instruction->opcodeID() == op_loop_hint)
            return false;
    }
    return true;
}

static bool compile(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const ScopeChain& scopeChain, const CalleeHints* hints, const ModuleLinkage* linkage, void* ownerForLinkBuffer, RefPtr<JITCode>& result, ASCIILiteral& reason, OpcodeID& reasonOpcode, bool hasDirectEntry = false)
{
    Graph graph(vm, unlinkedCodeBlock, scopeChain);
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    // (It is code in an image that gets to.)
    graph.startsCold = (!ownerForLinkBuffer || Options::aotWriteImage()) && mayStartCold(unlinkedCodeBlock);
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
    inferTypes(graph);
    inferRanges(graph);
    optimizeLoops(graph);
    graph.elideReadsOfCalleesNotPassed();
    graph.findListsOfArguments();
    if (Options::aotDumpGraph()) [[unlikely]] {
        dataLogLn("AOT graph:");
        graph.dump(WTF::dataFile());
    }

    B3::Procedure proc(/* usesSIMD = */ false);
    proc.setOptLevel(Options::aotB3OptLevel());
    proc.setPositionIndependent();
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
    bool makesCalls = graph.makesCalls;
    proc.code().setPrologueForEntrypoint(0, createSharedTask<B3::Air::PrologueGeneratorFunction>([&stubCalls, &graph, makesCalls](CCallHelpers& jit, B3::Air::Code& code) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.emitFunctionPrologue();
        // The frame has the Instance where a CodeBlock would be, and the object that was called for a callee. Once there is known to
        // be room for the frame that is put aside, and what says which function this is takes its place. Until then nobody looks,
        // and if there is no room, whoever says so wants to know which function (generateThrowStackOverflowAtPrologue()).
        constexpr unsigned frameSizeThatNeedsNoCheck = 256;
        if (usesStubs && (makesCalls || code.frameSize() > frameSizeThatNeedsNoCheck)) {
            // The stub does all that.
            if (graph.calleeSlot)
                jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), GPRInfo::regT11);
            graph.headerReferences.moveTaggedHeader(jit, boxedHeaderGPR);
            stubCalls.call(jit, Stub::Prologue, code.frameSize());
            if (graph.calleeSlot)
                jit.store64(GPRInfo::regT11, CCallHelpers::Address(GPRInfo::callFrameRegister, graph.calleeSlot->offsetFromFP()));
            jit.emitSave(code.calleeSaveRegisterAtOffsetList());
            return;
        }
        graph.headerReferences.moveBoxedHeader(jit, boxedHeaderGPR);
        auto becomeFrameOfThisFunction = makeScopeExit([&] {
            if (!graph.calleeSlot) {
                jit.store64(boxedHeaderGPR, CCallHelpers::addressFor(CallFrameSlot::callee));
                return;
            }
            jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), GPRInfo::regT9);
            jit.store64(boxedHeaderGPR, CCallHelpers::addressFor(CallFrameSlot::callee));
            jit.store64(GPRInfo::regT9, CCallHelpers::Address(GPRInfo::callFrameRegister, graph.calleeSlot->offsetFromFP()));
        });
        // The limit leaves room for the runtime to do what it has to when the stack is used up, which is a great deal more than this.
        // However deep the calls go, whatever made the last of them has checked.
        if constexpr (usesStubs) {
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
            jit.emitSave(code.calleeSaveRegisterAtOffsetList());
            return;
        }
        loadInstance(jit, CCallHelpers::addressFor(CallFrameSlot::codeBlock), GPRInfo::regT0);
        jit.loadPtr(CCallHelpers::Address(GPRInfo::regT0, Instance::offsetOfVM()), GPRInfo::regT1);
        jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, GPRInfo::regT2);
        auto ok = jit.branchPtr(CCallHelpers::BelowOrEqual, CCallHelpers::Address(GPRInfo::regT1, VM::offsetOfSoftStackLimit()), GPRInfo::regT2);
        jumpToThunk(jit, GPRInfo::regT0, Entry::ThrowStackOverflowAtPrologue);
        ok.link(&jit);
        jit.move(GPRInfo::regT2, CCallHelpers::stackPointerRegister);
        jit.emitSave(code.calleeSaveRegisterAtOffsetList());
    }));
    for (unsigned i = 0; i < graph.catchEntrypoints.size(); ++i) {
        // From catchThunk(): the frame pointer is this frame's again, and what this function saved on entry is still saved.
        proc.code().setPrologueForEntrypoint(i + 1, createSharedTask<B3::Air::PrologueGeneratorFunction>([](CCallHelpers& jit, B3::Air::Code& code) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        }));
    }

    B3::prepareForGeneration(proc);
    CCallHelpers jit;
    // The CodeHeader. What is in it is filled in when it is known.
    CCallHelpers::Label header = jit.label();
    for (unsigned i = 0; i < sizeof(CodeHeader) / sizeof(uint32_t); ++i)
        jit.m_assembler.buffer().putInt(0);
    unsigned numParameters = unlinkedCodeBlock->numParameters();
    bool checksArity = unlinkedCodeBlock->codeType() == FunctionCode && numParameters != 1;
    CCallHelpers::Label arityCheckWithStub;
    CCallHelpers::Jump arityChecked;
    CCallHelpers::Jump arityFixed;
    CCallHelpers::Label tooFewArguments;
    if (usesStubs && checksArity) {
        // What is done about too few comes first: it refers to the header, which has to be within reach.
        tooFewArguments = jit.label();
        graph.headerReferences.moveTaggedHeader(jit, boxedHeaderGPR); // In case there is no room for more.
        jit.move(CCallHelpers::linkRegister, GPRInfo::regT10);
        stubCalls.call(jit, Stub::ArityCheck, numParameters);
        arityFixed = jit.jump();
    }
    CCallHelpers::Label directEntry;
    CCallHelpers::Jump directlyEntered;
    if (usesStubs && hasDirectEntry) {
        // The frame is made, with the Instance in it, and the callee is where every call has it. If the function has nothing of the
        // realm yet, it is called the way a function nobody knows anything about is, which sees to that.
        // (One that may have been called as no object at all is found by its number, which is at hand.)
        CCallHelpers::Label noData = jit.label();
        if (graph.needsFunctionObject())
            stubCalls.tailCall(jit, unlinkedCodeBlock->isConstructor() ? Stub::ConstructFarFunction : Stub::CallFarFunction);
        else {
            jit.move(CCallHelpers::TrustedImm32(graph.distanceOfEnvironmentOfModule()), GPRInfo::regT10);
            stubCalls.tailCall(jit, Stub::LinkFunction);
        }
        directEntry = jit.label();
        graph.headerReferences.loadIndex(jit, GPRInfo::regT9);
        jit.loadPtr(CCallHelpers::calleeFrameSlot(CallFrameSlot::codeBlock).withOffset(sizeof(CallerFrameAndPC) - prologueStackPointerDelta()), GPRInfo::regT10);
        jit.addLeftShift64(GPRInfo::regT10, GPRInfo::regT9, CCallHelpers::TrustedImm32(3), GPRInfo::regT10);
        jit.loadPtr(CCallHelpers::Address(GPRInfo::regT10, Instance::offsetOfData()), GPRInfo::regT10);
        jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::regT10).linkTo(noData, &jit);
        if (!checksArity && !graph.catchEntrypoints.isEmpty())
            directlyEntered = jit.jump();
    }
    if (usesStubs && checksArity) {
        // Nothing has been pushed: the frame's slots are found from the stack pointer.
        arityCheckWithStub = jit.label();
        jit.load32(CCallHelpers::calleeFrameSlot(CallFrameSlot::argumentCountIncludingThis).withOffset(sizeof(CallerFrameAndPC) - prologueStackPointerDelta() + LowWordOffset), GPRInfo::regT9);
        jit.branch32(CCallHelpers::Below, GPRInfo::regT9, CCallHelpers::TrustedImm32(numParameters)).linkTo(tooFewArguments, &jit);
        // The only way in is what comes first.
        if (!graph.catchEntrypoints.isEmpty())
            arityChecked = jit.jump();
    }
    CCallHelpers::Label startOfCode = jit.label();
    jit.setOopsIsJustABreakpoint();
    B3::generate(proc, jit);

    CCallHelpers::Label entryLabel = proc.code().entrypointLabel(0);
    CCallHelpers::Label arityCheckLabel = entryLabel;
    if (directlyEntered.isSet())
        directlyEntered.linkTo(entryLabel, &jit);
    if (usesStubs && checksArity) {
        if (arityChecked.isSet())
            arityChecked.linkTo(entryLabel, &jit);
        else
            RELEASE_ASSERT(!CCallHelpers::differenceBetween(startOfCode, entryLabel));
        arityFixed.linkTo(entryLabel, &jit);
        arityCheckLabel = arityCheckWithStub;
    } else if (checksArity) {
        // What FTL::compile() emits, with the VM and the thunk found from the callee frame's CodeBlock.
        arityCheckLabel = jit.label();
        auto slotBeforePrologue = [](CallFrameSlot slot) {
            return CCallHelpers::calleeFrameSlot(slot).withOffset(sizeof(CallerFrameAndPC) - prologueStackPointerDelta());
        };
        jit.load32(slotBeforePrologue(CallFrameSlot::argumentCountIncludingThis).withOffset(LowWordOffset), GPRInfo::argumentGPR2);
        jit.branch32(CCallHelpers::AboveOrEqual, GPRInfo::argumentGPR2, CCallHelpers::TrustedImm32(numParameters)).linkTo(entryLabel, &jit);

        graph.headerReferences.moveBoxedHeader(jit, boxedHeaderGPR);
        loadInstance(jit, slotBeforePrologue(CallFrameSlot::codeBlock), GPRInfo::regT5);
        static_assert(stackAlignmentRegisters() == 2);
        unsigned aligned = WTF::roundUpToMultipleOf(stackAlignmentRegisters(), numParameters + CallFrame::headerSizeInRegisters) == numParameters + CallFrame::headerSizeInRegisters ? numParameters : numParameters + 1;
        jit.move(CCallHelpers::TrustedImm32(aligned), GPRInfo::argumentGPR0);
        jit.sub32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR2, GPRInfo::argumentGPR0);
        jit.add32(CCallHelpers::TrustedImm32(1), GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
        jit.and32(CCallHelpers::TrustedImm32(~1U), GPRInfo::argumentGPR1);
        jit.lshiftPtr(CCallHelpers::TrustedImm32(3), GPRInfo::argumentGPR1);
        jit.subPtr(CCallHelpers::stackPointerRegister, GPRInfo::argumentGPR1, GPRInfo::argumentGPR3);
        jit.loadPtr(CCallHelpers::Address(GPRInfo::regT5, Instance::offsetOfVM()), GPRInfo::argumentGPR1);
        auto stackOverflow = jit.branchPtr(CCallHelpers::Above, CCallHelpers::Address(GPRInfo::argumentGPR1, VM::offsetOfSoftStackLimit()), GPRInfo::argumentGPR3);

        jit.loadPtr(CCallHelpers::Address(GPRInfo::regT5, Instance::offsetOfRuntimeTable()), GPRInfo::regT5);
        jit.loadPtr(CCallHelpers::Address(GPRInfo::regT5, static_cast<unsigned>(Entry::ArityFixup) * sizeof(void*)), GPRInfo::regT5);
        jit.tagPtr(NoPtrTag, CCallHelpers::linkRegister);
        jit.move(CCallHelpers::linkRegister, GPRInfo::argumentGPR1);
        jit.call(GPRInfo::regT5, JITThunkPtrTag);
        jit.move(GPRInfo::argumentGPR1, CCallHelpers::linkRegister);
        jit.untagPtr(NoPtrTag, CCallHelpers::linkRegister);
        jit.jump().linkTo(entryLabel, &jit);

        stackOverflow.link(&jit);
        jit.emitFunctionPrologue();
        jumpToThunk(jit, GPRInfo::regT5, Entry::ThrowStackOverflowAtPrologue);
    }

    LinkBuffer linkBuffer(jit, ownerForLinkBuffer, LinkBuffer::Profile::FTL, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate()) {
        graph.fail("out of executable memory"_s);
        return declined();
    }

    graph.headerReferences.link(linkBuffer, header);
    {
        CodeHeader contents;
        contents.visibility = unlinkedCodeBlock->codeType() == FunctionCode && unlinkedCodeBlock->isBuiltinFunction() ? ImplementationVisibility::Private : ImplementationVisibility::Public;
        contents.calleeSlot = graph.calleeSlot ? safeCast<int16_t>(graph.calleeSlot->offsetFromFP() / static_cast<int>(sizeof(Register))) : 0;
        // (Whoever puts it in an image gives it another.)
        contents.index = ownerForLinkBuffer ? allocateFunctionIndex() : 0;
        performJITMemcpy<jitMemcpyRepatch>(linkBuffer.locationOf<JSEntryPtrTag>(header).untaggedPtr(), &contents, sizeof(contents));
    }

    CompiledFunctionInfo info;
    info.stubCalls = stubCalls.link(linkBuffer);
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    auto offsetOf = [&](CCallHelpers::Label label) {
        return static_cast<unsigned>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(label).untaggedPtr()) - static_cast<uint8_t*>(start));
    };
    info.entryOffset = offsetOf(entryLabel);
    info.arityCheckOffset = offsetOf(arityCheckLabel);
    if (directEntry.isSet())
        info.directEntryOffset = offsetOf(directEntry);
    info.frameSizeInBytes = proc.frameSize();
    info.numSlots = graph.numICSlots;
    info.sites = WTF::move(graph.sites);
    while (info.sites.size() < info.numSlots)
        info.sites.append(Site { });
    info.knownCallees = WTF::move(graph.knownCallees);
    info.siteConstants = WTF::move(graph.siteConstants);
    info.plans = WTF::move(graph.plans);
    info.quotableSites = WTF::move(graph.quotableSites);
    std::ranges::sort(info.quotableSites);
    info.quotableSites.shrink(std::ranges::unique(info.quotableSites).begin() - info.quotableSites.begin());
    info.callSites = WTF::move(graph.callSites);
    info.callSites.append(0); // generateThrowStackOverflowAtPrologue()
    std::ranges::sort(info.callSites);
    info.callSites.shrink(std::ranges::unique(info.callSites).begin() - info.callSites.begin());
    while (info.siteConstants.size() < info.numSlots)
        info.siteConstants.append(0);
    info.selectors = WTF::move(graph.selectors);
    info.shapes = WTF::move(graph.shapes);
    info.bytecodeHash = hashOfBytecode(unlinkedCodeBlock);
    info.usesStaticImports = graph.usesStaticImports;
    info.startsCold = graph.startsCold;
    RELEASE_ASSERT(!info.startsCold || info.numSlots <= SharedData::maxSlots);
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
                        auto variable = graph.resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
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
    result = adoptRef(*new JITCode(start, codeRef.executableMemory(), WTF::move(info), JITCode::wayInto(unlinkedCodeBlock)));
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

uint32_t inferReturnTypeForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, Vector<const KnownFunction*>& calleesConsulted)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    if (!parseBytecode(graph))
        return TTop;
    return inferTypes(graph, &calleesConsulted) & TTop;
}

bool compileForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, CompiledCode& result, const CalleeHints* hints, const ModuleLinkage* linkage, bool hasDirectEntry)
{
    MonotonicTime before = MonotonicTime::now();
    RefPtr<JITCode> jitCode;
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    bool ok = compile(vm, unlinkedCodeBlock, unknownScopeChain(), hints, linkage, nullptr, jitCode, reason, reasonOpcode, hasDirectEntry);
    if (Options::aotReportStats()) [[unlikely]]
        recordStatistics(ok, ok ? jitCode->size() : 0, unlinkedCodeBlock->instructionsSize(), MonotonicTime::now() - before, reason, reasonOpcode);
    if (!ok)
        return false;
    // Out of the JIT's memory, which goes back to the JIT.
    result.info = jitCode->info();
    result.bytes.append(std::span { static_cast<const uint8_t*>(jitCode->dataAddressAtOffset(0)), jitCode->size() });
#if CPU(ARM64)
    // What the JIT's allocator rounds a size up with.
    constexpr uint32_t breakpoint = 0xd4200000;
    while (result.bytes.size() > sizeof(CodeHeader) + sizeof(uint32_t)) {
        uint32_t last;
        memcpy(&last, result.bytes.span().data() + result.bytes.size() - sizeof(last), sizeof(last));
        if (last != breakpoint)
            break;
        result.bytes.shrink(result.bytes.size() - sizeof(last));
    }
    result.info.codeSize = result.bytes.size();
#endif
    return true;
}

static CString nameOf(ScriptExecutable* executable)
{
    if (auto* function = dynamicDowncast<FunctionExecutable>(executable))
        return function->ecmaName().utf8();
    return executable->type() == ModuleProgramExecutableType ? "<module>" : "<global>";
}

RefPtr<JITCode> tryCompile(VM& vm, ScriptExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSScope* scope)
{
    if (unlinkedCodeBlock->codeType() == EvalCode)
        return nullptr;
    if (const char* filter = Options::aotFilter()) {
        if (!strstr(nameOf(executable).data(), filter))
            return nullptr;
    }

    if (unsigned skip = Options::aotSkip()) {
        static std::atomic<unsigned> count;
        if (++count == skip)
            return nullptr;
    }
    if (unsigned limit = Options::aotLimit()) {
        // For finding the function that is miscompiled: bisect on the limit, and the last one compiled is it.
        static std::atomic<unsigned> count;
        unsigned index = ++count;
        if (index > limit)
            return nullptr;
        if (index == limit) {
            Options::aotDumpGraph() = true;
            dataLogLn("AOT: function #", index, " is ", nameOf(executable), " ", executable->source().view().left(400));
        }
    }

    MonotonicTime before = MonotonicTime::now();
    RefPtr<JITCode> jitCode;
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    // By default, with no more to go by than there is when a program is compiled before it is run.
    std::unique_ptr<LiveHints> hints;
    if (Options::aotUseLiveCalleeHints())
        hints = makeUnique<LiveHints>(scope->realm());
    bool ok = compile(vm, unlinkedCodeBlock, Options::aotUseLiveScopes() ? scopeChainFor(scope) : unknownScopeChain(), hints.get(), nullptr, executable, jitCode, reason, reasonOpcode);

    if (Options::aotVerbose()) [[unlikely]] {
        if (ok) {
            dataLogLn("AOT: compiled ", nameOf(executable), ": ", jitCode->size(), " bytes for ", unlinkedCodeBlock->instructionsSize(), " of bytecode, frame ", jitCode->info().frameSizeInBytes);
            if (auto key = imageKeyFor(executable, kind))
                dataLogLn("AOT: ", nameOf(executable), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") is at ", RawPointer(jitCode->dataAddressAtOffset(0)), " size ", jitCode->size(), " hash ", hashOfCode({ static_cast<const uint8_t*>(jitCode->dataAddressAtOffset(0)), jitCode->size() }));
        }
        else
            dataLogLn("AOT: declined ", nameOf(executable), ": ", reason, reasonOpcode != op_nop ? " " : "", reasonOpcode != op_nop ? opcodeNames[reasonOpcode] : ""_s);
    }
    if (Options::aotReportStats()) [[unlikely]]
        recordStatistics(ok, ok ? jitCode->size() : 0, unlinkedCodeBlock->instructionsSize(), MonotonicTime::now() - before, reason, reasonOpcode);
    if (!ok)
        return nullptr;

    if (Options::aotWriteImage()) [[unlikely]]
        addToImageBeingWritten(executable, kind, *jitCode);
    return jitCode;
}

void reportStatistics()
{
    auto& stats = statistics();
    Locker locker { stats.lock };
    dataLogLn("AOT: compiled ", stats.compiled, " functions (", stats.codeBytes, " bytes of code for ", stats.bytecodeBytes, " of bytecode) in ", stats.time.milliseconds(), " ms; declined ", stats.declined);
    dataLogLn("AOT: ", stats.intrinsicReads, " reads of immutable intrinsics; ", stats.slots, " slots");
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
        static constexpr ASCIILiteral kinds[] = { "Bytecode"_s, "Constant"_s, "ConstantCell"_s, "Argument"_s, "Phi"_s, "Proj"_s, "GetStack"_s, "SetStack"_s, "Guard"_s, "Narrow"_s };
        ASCIILiteral name = !tag ? "(function)"_s : tag <= numOpcodeIDs ? opcodeNames[tag - 1] : kinds[tag - numOpcodeIDs - 1];
        dataLogLn("  SIZE ", name, " ", count, " ", bytes);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
