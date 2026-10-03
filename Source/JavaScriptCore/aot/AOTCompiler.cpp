/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiler.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLowering.h"
#include "AOTProgram.h"
#include "AOTStubs.h"
#include "AOTTypeTable.h"
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
#include <wtf/Lock.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringHash.h>

namespace JSC { namespace AOT {

static ScopeChain unknownScopeChain()
{
    ScopeChain chain;
    ScopeChainEntry entry;
    entry.kind = ScopeChainEntry::Unknown;
    chain.append(entry);
    return chain;
}

#if CPU(ARM64) || CPU(X86_64)

static void estimateFrequencies(B3::Procedure& proc)
{
    proc.resetReachability();
    auto& loops = proc.naturalLoops();

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
        constexpr unsigned significantDepth = 6;
        double frequency = pow(10.0, std::min(loops.loopDepth(block), significantDepth));
        block->setFrequency(likely.contains(block) ? frequency : frequency * Lowering::coldFrequency);
    }
}

static bool mayStartCold(UnlinkedCodeBlock* unlinkedCodeBlock)
{
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->instructions().size() > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return false;
    for (const auto& instruction : unlinkedCodeBlock->instructions()) {
        if (instruction->opcodeID() == op_loop_hint)
            return false;
    }
    return true;
}

static bool isGetByValOnThis(UnlinkedCodeBlock* unlinkedCodeBlock)
{
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->numParameters() != 2)
        return false;
    VirtualRegister thisRegister = virtualRegisterForArgumentIncludingThis(0);
    std::optional<VirtualRegister> result;
    for (const auto& instruction : unlinkedCodeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_enter:
            break;
        case op_to_this:
            if (result || instruction->as<OpToThis>().m_srcDst != thisRegister)
                return false;
            break;
        case op_get_by_val: {
            auto bytecode = instruction->as<OpGetByVal>();
            if (result || bytecode.m_base != thisRegister || bytecode.m_property != virtualRegisterForArgumentIncludingThis(1))
                return false;
            result = bytecode.m_dst;
            break;
        }
        case op_ret:
            return result && instruction->as<OpRet>().m_value == *result;
        default:
            return false;
        }
    }
    return false;
}

static bool mayReturnScopeVariable(UnlinkedCodeBlock* unlinkedCodeBlock)
{
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->numParameters() != 1)
        return false;
    VirtualRegister scope = unlinkedCodeBlock->scopeRegister();
    std::optional<VirtualRegister> result;
    bool hasResolvedScope = false;
    for (const auto& instruction : unlinkedCodeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_enter:
            break;
        case op_get_scope:
            if (hasResolvedScope || result || instruction->as<OpGetScope>().m_dst != scope)
                return false;
            break;
        case op_resolve_scope:
            if (hasResolvedScope || result)
                return false;
            hasResolvedScope = true;
            scope = instruction->as<OpResolveScope>().m_dst;
            break;
        case op_get_from_scope: {
            auto bytecode = instruction->as<OpGetFromScope>();
            if (result || bytecode.m_scope != scope)
                return false;
            result = bytecode.m_dst;
            break;
        }
        case op_check_tdz:
            if (!result || instruction->as<OpCheckTdz>().m_targetVirtualRegister != *result)
                return false;
            break;
        case op_ret:
            return result && instruction->as<OpRet>().m_value == *result;
        default:
            return false;
        }
    }
    return false;
}

static void usePinnedRegistersDirectly(B3::Air::Code& code)
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

#if CPU(X86_64)
static void keepDataInRegister(const Graph& graph, B3::Air::Code& code)
{
    code.setKeepsFastTmpsFastWhenCoalesced();
    for (B3::Air::BasicBlock* block : code) {
        for (B3::Air::Inst& inst : *block) {
            if (inst.kind.opcode != B3::Air::Patch || !inst.origin || !graph.patchpointsTakingData.contains(inst.origin))
                continue;
            B3::Air::Arg& data = inst.args()[inst.args().size() - 1];
            if (data.isTmp() && !data.tmp().isReg())
                code.addFastTmp(data.tmp());
        }
    }
}
#endif

bool hasNoFrame(const Graph& graph, B3::Air::Code& code)
{
    if (code.frameSize() || code.calleeSaveRegisterAtOffsetList().registerCount() || graph.alwaysEmitsCalls || !graph.catchEntrypoints.isEmpty())
        return false;
    if (!graph.emitsCalls)
        return true;
    if (!graph.hasRemainingCalls) {
        bool found = false;
        for (B3::Air::BasicBlock* block : code) {
            for (B3::Air::Inst& inst : *block) {
                if (inst.kind.opcode == B3::Air::Patch)
                    found |= !inst.origin || inst.origin->opcode() != B3::Patchpoint || inst.origin->as<B3::PatchpointValue>()->lateClobbered().contains(callMarkerGPR, IgnoreVectors);
                else
                    found |= inst.kind.opcode == B3::Air::ColdCCall;
            }
        }
        graph.hasRemainingCalls = found;
    }
    return !*graph.hasRemainingCalls;
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

static bool canSkipLoopSplitting(Graph& graph)
{
    bool hasLoop = false;
    for (BasicBlock* block : graph.m_rpo) {
        if (!block->isInLoop)
            continue;
        hasLoop = true;
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode || !node->instruction)
                continue;
            switch (node->opcode) {
            case op_get_by_val:
                if (!node->use(node->as<OpGetByVal>().m_base)->type || !isSubtype(node->use(node->as<OpGetByVal>().m_base)->type, TArray) || !isSubtype(node->use(node->as<OpGetByVal>().m_property)->type, TNumber))
                    return false;
                break;
            case op_get_length:
                if (!isSubtype(node->use(node->as<OpGetLength>().m_base)->type, TArray | TString))
                    return false;
                break;
            case op_get_by_id:
            case op_put_by_id:
                if (!Graph::typedFieldAccessedBy(node))
                    return false;
                break;
            case op_put_by_val:
            case op_iterator_open:
            case op_iterator_next:
            case op_call:
            case op_call_ignore_result:
                return false;
            case op_resolve_scope:
                if (!isStaticClosureVarResolveType(node->as<OpResolveScope>().m_resolveType))
                    return false;
                break;
            case op_get_from_scope:
                if (node->as<OpGetFromScope>().m_getPutInfo.resolveType() != ResolvedClosureVar)
                    return false;
                break;
            default:
                break;
            }
        }
    }
    return hasLoop;
}

static bool compile(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, CompiledCode& result, ASCIILiteral& reason, OpcodeID& reasonOpcode, const FunctionSummary* summary, VariableSummaries* variableSummaries, const ProgramCode* program, bool triesUnsplitLoops = true)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    triesUnsplitLoops &= Options::preferUnsplitAOTLoops() && !Options::useAOTFunctionSplitting();
    graph.loopSplittingIsDisabled = triesUnsplitLoops;
    graph.setCalleeHints(hints);
    graph.setSummary(summary);
    graph.setVariableSummaries(variableSummaries);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    graph.startsCold = mayStartCold(unlinkedCodeBlock);
    graph.isGetByValOnThis = isGetByValOnThis(unlinkedCodeBlock);
    graph.mayReturnScopeVariable = mayReturnScopeVariable(unlinkedCodeBlock);
    if (graph.isGetByValOnThis)
        graph.remark("get-by-val-on-this"_s);
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
    scalarReplaceReadOnlyObjects(graph);
    inferTypes(graph);
    planMultiValueReturns(graph);
    if (triesUnsplitLoops && Options::useAOTLoopSplitting() && Options::aotLoopSplittingPolicy() && !canSkipLoopSplitting(graph))
        return compile(vm, unlinkedCodeBlock, hints, linkage, result, reason, reasonOpcode, summary, variableSummaries, program, false);
    inferRanges(graph);
    optimizeLoops(graph);
    graph.elideUnpassedCalleeReads();
    graph.elideArrayIteratorMethodReads();
    graph.sinkIteratorMethodReads();
    graph.findBuiltinsCalled();
    graph.findArgumentLists();
    promoteEnvironments(graph);
    analyzeEscapes(graph);
    if (Options::dumpAOTGraph()) [[unlikely]] {
        static Lock lock;
        Locker locker { lock };
        dataLogLn("AOT graph:");
        graph.dump(WTF::dataFile());
    }

    B3::Procedure proc(/* usesSIMD = */ false);
    proc.setPositionIndependent();
    proc.pinRegister(instanceGPR);
    proc.pinRegister(GPRInfo::numberTagRegister);
    proc.pinRegister(GPRInfo::notCellMaskRegister);
#if CPU(X86_64)
    proc.code().setUnsavedCalleeSaves(RegisterSet { X86Registers::r12 });
#endif
    Lowering lowering(graph, proc);
    if (!lowering.run())
        return declined();
    estimateFrequencies(proc);
    if (Options::dumpAOTB3Graph()) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);

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
        constexpr unsigned maxFrameSizeWithoutStackCheck = 256;
        if (graph.makesCalls || code.frameSize() > maxFrameSizeWithoutStackCheck) {
#if CPU(X86_64)
            constexpr GPRReg newStackPointer = CCallHelpers::s_scratchRegister;
            constexpr GPRReg vm = stubTemporaryGPRs[2];
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, newStackPointer);
            jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfVM()), vm);
            CCallHelpers::Jump fits = jit.branchPtr(CCallHelpers::BelowOrEqual, CCallHelpers::Address(vm, VM::offsetOfSoftStackLimit()), newStackPointer);
            stubCalls.tailCall(jit, Stub::ThrowStackOverflowAtPrologue);
            fits.link(&jit);
            jit.move(newStackPointer, CCallHelpers::stackPointerRegister);
#else
            stubCalls.call(jit, Stub::Prologue, code.frameSize(), CallSite { });
#endif
        } else if (code.frameSize())
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
        jit.emitSave(code.calleeSaveRegisterAtOffsetList());
    }));
    proc.code().setEpilogueGenerator(createSharedTask<B3::Air::PrologueGeneratorFunction>([&graph](CCallHelpers& jit, B3::Air::Code& code) {
        emitEpilogueBeforeLeaving(jit, graph, code);
        jit.ret();
    }));
    for (unsigned i = 0; i < graph.catchEntrypoints.size(); ++i) {
        proc.code().setPrologueForEntrypoint(i + 1, createSharedTask<B3::Air::PrologueGeneratorFunction>([](CCallHelpers& jit, B3::Air::Code& code) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        }));
    }

    B3::generateToAir(proc);
    usePinnedRegistersDirectly(proc.code());
#if CPU(X86_64)
    keepDataInRegister(graph, proc.code());
#endif
    B3::Air::prepareForGeneration(proc.code());
    CCallHelpers jit;
    CCallHelpers::Jump jumpToMainEntrypoint;
    if (!graph.catchEntrypoints.isEmpty())
        jumpToMainEntrypoint = jit.jump();
    CCallHelpers::Label codeStart = jit.label();
    jit.setOopsEmitsBreakpointOnly();
    B3::generate(proc, jit);
    if (jumpToMainEntrypoint.isSet())
        jumpToMainEntrypoint.linkTo(proc.code().entrypointLabel(0), &jit);
    else
        RELEASE_ASSERT(!CCallHelpers::differenceBetween(codeStart, proc.code().entrypointLabel(0)));
    jit.breakpoint();
    jit.padBeforePatch();
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::FTL);

    CompiledFunctionInfo info;
    info.stubCalls = stubCalls.link(linkBuffer);
    info.indexReferences = graph.indexReferences.link(linkBuffer);
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    uint32_t sizeOfJump = static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(codeStart).untaggedPtr()) - static_cast<uint8_t*>(start);
    if (jumpToMainEntrypoint.isSet() && static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(proc.code().entrypointLabel(0)).untaggedPtr()) - static_cast<uint8_t*>(start) == sizeOfJump) {
        start = static_cast<uint8_t*>(start) + sizeOfJump;
        info.codeSize -= sizeOfJump;
        for (auto& call : info.stubCalls)
            call.offset -= sizeOfJump;
        for (auto& reference : info.indexReferences)
            reference.offset -= sizeOfJump;
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
    if (program && mayBeAbsorbed(unlinkedCodeBlock, summary))
        recordAllSitesOf(graph);
    info.quotableSites = WTF::move(graph.quotableSites);
    std::ranges::sort(info.quotableSites);
    info.quotableSites.shrink(std::ranges::unique(info.quotableSites).begin() - info.quotableSites.begin());
    info.isOnlyCalledDirectly = summary && summary->isNonEscaping;
    if (program) {
        auto noteKeysOf = [&](UnlinkedFunctionExecutable* executable, Vector<ImageKey>& keys) {
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (UnlinkedFunctionCodeBlock* code = executable->codeBlockIfExists(kind)) {
                    if (auto about = program->about(code))
                        keys.append(about->key);
                }
            }
        };
        for (unsigned i = 0; i < unlinkedCodeBlock->numberOfFunctionExprs(); ++i)
            noteKeysOf(unlinkedCodeBlock->functionExpr(i), info.functionExpressionsInCode);
        for (UnlinkedFunctionExecutable* executable : graph.functionsCreated)
            noteKeysOf(executable, info.functionsCreated);
    }
    info.numberOfFunction = summary ? summary->number : 0;
    for (auto& frame : graph.inlineFrames)
        info.inlineFrames.append({ frame.parent, frame.callSite, frame.knownCallee, frame.isTailCall });
    info.spreadSites = WTF::move(graph.spreadSites);
    info.callSites = WTF::move(graph.callSites);
    std::ranges::sort(info.callSites);
    info.callSites.shrink(std::ranges::unique(info.callSites).begin() - info.callSites.begin());
    while (info.siteConstants.size() < info.numSlots)
        info.siteConstants.append(0);
    info.selectors = WTF::move(graph.selectors);
    info.shapes = WTF::move(graph.shapes);
    info.usesStaticImports = graph.usesStaticImports;
    info.startsCold = graph.startsCold;
    info.isGetByValOnThis = graph.isGetByValOnThis;
    info.returnedVariable = graph.returnedVariable;
    RELEASE_ASSERT(!info.startsCold || info.numSlots <= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots));
    info.calleeSaveRegisters = proc.calleeSaveRegisterAtOffsetList();
    for (unsigned i = 0; i < graph.catchEntrypoints.size(); ++i)
        info.catchEntrypoints.append({ graph.catchEntrypoints[i]->bytecodeBegin, offsetOf(proc.code().entrypointLabel(i + 1)) });

    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::dumpAOTDisassembly(), linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
#if CPU(ARM64)
    {
        constexpr uint32_t breakpoint = 0xd4200000;
        constexpr uint32_t nop = 0xd503201f;
        while (info.codeSize > sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == nop)
            info.codeSize -= sizeof(uint32_t);
        unsigned minimumSize = sizeof(uint32_t);
        for (auto& call : info.stubCalls) {
            if (!call.isTailCall)
                minimumSize = std::max<unsigned>(minimumSize, call.offset + 2 * sizeof(uint32_t));
        }
        while (info.codeSize > minimumSize && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == breakpoint)
            info.codeSize -= sizeof(uint32_t);
    }
#endif
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    result.info = WTF::move(info);
    result.remarks = WTF::move(graph.remarks);
    return true;
}

#endif // CPU(ARM64) || CPU(X86_64)

bool recordKnownFunctionUsesForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, const FunctionSummaryMap& summariesByExecutable, const FunctionSummary* summary, VariableSummaries* variableSummaries)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    if (!parseBytecode(graph))
        return false;
    graph.recordKnownFunctionUses(summariesByExecutable, summary);
    graph.noteFieldsComparedWithStrings();
    recordReturnedLiterals(graph);
    graph.noteClassesDefined();
    if (variableSummaries)
        graph.recordUntrackableVariableAccesses(*variableSummaries);
    return true;
}

Type inferReturnTypeForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, const FunctionSummary* summary, VariableSummaries* variableSummaries, unsigned summaryReader, Vector<const KnownFunction*>& calleesRead, Vector<const KnownFunction*>& calleesWithWidenedInputs, uint32_t& escapingParameters, const String& nameForLog)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    graph.setSummary(summary);
    graph.setVariableSummaries(variableSummaries, summaryReader);
    graph.setNameForLog(nameForLog);
    if (!parseBytecode(graph)) {
        escapingParameters = std::numeric_limits<uint32_t>::max();
        return TTop;
    }
    scalarReplaceReadOnlyObjects(graph);
    Type result = inferTypes(graph, &calleesRead, &calleesWithWidenedInputs) & TTop;
    escapingParameters = summary ? AOT::escapingParameters(graph, &calleesRead) : std::numeric_limits<uint32_t>::max();
    return result;
}

bool compileForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, CompiledCode& result, const CalleeHints* hints, const ModuleLinkage* linkage, const FunctionSummary* summary, VariableSummaries* variableSummaries, const ProgramCode* program)
{
#if CPU(ARM64) || CPU(X86_64)
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    bool ok = compile(vm, unlinkedCodeBlock, hints, linkage, result, reason, reasonOpcode, summary, variableSummaries, program);
    if (!ok && Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: declined: ", reason, " ", reasonOpcode != op_nop ? opcodeNames[reasonOpcode] : ""_s);
    return ok;
#else
    UNUSED_PARAM(vm);
    UNUSED_PARAM(unlinkedCodeBlock);
    UNUSED_PARAM(result);
    UNUSED_PARAM(hints);
    UNUSED_PARAM(linkage);
    UNUSED_PARAM(summary);
    UNUSED_PARAM(variableSummaries);
    UNUSED_PARAM(program);
    return false;
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
