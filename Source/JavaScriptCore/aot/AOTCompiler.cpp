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

static ScopeChain unknownScopeChain()
{
    ScopeChain chain;
    ScopeChainEntry entry;
    entry.kind = ScopeChainEntry::Unknown;
    chain.append(entry);
    return chain;
}

// Estimates the relative execution frequency of each block. The register allocator uses it to decide what to spill and which moves
// to coalesce. Without a profile, it has to be derived from the shape of the code. The lowering has already marked the blocks that
// it created for rare cases.
static void estimateFrequencies(B3::Procedure& proc)
{
    proc.resetReachability();
    auto& loops = proc.naturalLoops();

    // The blocks that are reachable without taking a branch that is rarely taken.
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

// See CompiledFunctionInfo::startsCold.
static bool mayStartCold(UnlinkedCodeBlock* unlinkedCodeBlock)
{
    // (The number of slots is always less than the size of the bytecode.)
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->instructions().size() > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots) || !hasOnlyRealmIndependentConstants(unlinkedCodeBlock, SymbolTablesAreShared::Yes))
        return false;
    for (const auto& instruction : unlinkedCodeBlock->instructions()) {
        if (instruction->opcodeID() == op_loop_hint)
            return false;
    }
    return true;
}

// A register that holds the same value throughout the function is used directly. As a B3 value it would be copied to another
// register on entry, and that register would then have to be saved.
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

// A function that calls nothing, keeps nothing on the stack and saves nothing has no frame.
bool hasNoFrame(const Graph& graph, B3::Air::Code& code)
{
    // (An exception handler needs a frame.)
    if (code.frameSize() || code.calleeSaveRegisterAtOffsetList().registerCount() || graph.alwaysEmitsCalls || !graph.catchEntrypoints.isEmpty())
        return false;
    if (!graph.emitsCalls)
        return true;
    // Code that contained a call may have turned out to be unreachable. Every remaining call declares that it clobbers the link
    // register, so look for that.
    if (!graph.hasRemainingCalls) {
        bool found = false;
        for (B3::Air::BasicBlock* block : code) {
            for (B3::Air::Inst& inst : *block) {
                if (inst.kind.opcode == B3::Air::Patch)
                    found |= !inst.origin || inst.origin->opcode() != B3::Patchpoint || inst.origin->as<B3::PatchpointValue>()->lateClobbered().contains(ARM64Registers::lr, IgnoreVectors);
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

// For Options::preferUnsplitAOTLoops(). Takes a graph whose loops have not been split.
static bool loopsNeedNoSplitting(Graph& graph)
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

static bool compile(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, CompiledCode& result, ASCIILiteral& reason, OpcodeID& reasonOpcode, const FunctionSummary* summary, VariableSummaries* variableSummaries, const CodeOfProgram* program, bool triesUnsplitLoops = true)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    triesUnsplitLoops &= Options::preferUnsplitAOTLoops() && !Options::useAOTFunctionSplitting();
    graph.loopsAreNotSplit = triesUnsplitLoops;
    graph.setCalleeHints(hints);
    graph.setSummary(summary);
    graph.setVariableSummaries(variableSummaries);
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
    scalarReplaceReadOnlyObjects(graph);
    inferTypes(graph);
    planMultiValueReturns(graph);
    // (Without loop splitting there is no choice to make: LoopOptimizer::hoistArrayStorageLoads() looks at each loop separately.)
    if (triesUnsplitLoops && Options::useAOTLoopSplitting() && Options::aotLoopSplittingPolicy() && !loopsNeedNoSplitting(graph))
        return compile(vm, unlinkedCodeBlock, hints, linkage, result, reason, reasonOpcode, summary, variableSummaries, program, false);
    inferRanges(graph);
    optimizeLoops(graph);
    graph.elideReadsOfCalleesNotPassed();
    graph.elideReadsOfIteratorMethodsOfArrays();
    graph.findBuiltinsCalled();
    graph.findListsOfArguments();
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
    Lowering lowering(graph, proc);
    if (!lowering.run())
        return declined();
    estimateFrequencies(proc);
    if (Options::dumpAOTB3Graph()) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);

    // The code is going to run in another process, where objects are at other addresses. No lowering should have embedded an
    // address. Running the code only proves that for the paths that are taken, so check all constants. (No JSValue that is not a
    // cell falls in this range.)
    for (B3::Value* value : proc.values()) {
        if (!value->hasInt64())
            continue;
        uint64_t bits = value->asInt64();
        // (Except for the base address of structures, which is the same in every process: structureIDBaseOfImages.)
        if (bits >= 4 * GB && bits < (1ULL << 47) && bits != structureIDBaseOfImages && !graph.wideIntegerConstants.contains(static_cast<int64_t>(bits))) {
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
        // The stack limit leaves room for the runtime to handle a stack overflow, which is far more than this. However deep the
        // calls go, the function that made the last call has checked the limit.
        constexpr unsigned maxFrameSizeWithoutStackCheck = 256;
        if (graph.makesCalls || code.frameSize() > maxFrameSizeWithoutStackCheck)
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
        // Entered from catchThunk(): the frame pointer is this frame's again, and the registers that this function saved on entry
        // are still saved.
        proc.code().setPrologueForEntrypoint(i + 1, createSharedTask<B3::Air::PrologueGeneratorFunction>([](CCallHelpers& jit, B3::Air::Code& code) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        }));
    }

    B3::generateToAir(proc);
    usePinnedRegistersDirectly(proc.code());
    B3::Air::prepareForGeneration(proc.code());
    CCallHelpers jit;
    // The main entry point has to be at the start of the code.
    CCallHelpers::Jump jumpToMainEntrypoint;
    if (!graph.catchEntrypoints.isEmpty())
        jumpToMainEntrypoint = jit.jump();
    CCallHelpers::Label startOfCode = jit.label();
    jit.setOopsIsJustABreakpoint();
    B3::generate(proc, jit);
    if (jumpToMainEntrypoint.isSet())
        jumpToMainEntrypoint.linkTo(proc.code().entrypointLabel(0), &jit);
    else
        RELEASE_ASSERT(!CCallHelpers::differenceBetween(startOfCode, proc.code().entrypointLabel(0)));
    // The code is linked in ordinary memory. It is only copied into the image and never runs from here, so compiling needs no JIT memory.
    jit.breakpoint();
    jit.padBeforePatch();
    Vector<uint32_t> storage(jit.m_assembler.codeSize() / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::FTL);

    CompiledFunctionInfo info;
    info.stubCalls = stubCalls.link(linkBuffer);
    info.indexReferences = graph.indexReferences.link(linkBuffer);
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    // (The main entry point was first after all, so the jump is removed.)
    if (jumpToMainEntrypoint.isSet() && static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(proc.code().entrypointLabel(0)).untaggedPtr()) - static_cast<uint8_t*>(start) == sizeof(uint32_t)) {
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
    if (program && mayBecomePartOfAnother(unlinkedCodeBlock, summary))
        noteEverySiteOf(graph);
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
    info.sitesOfSpreads = WTF::move(graph.sitesOfSpreads);
    info.callSites = WTF::move(graph.callSites);
    std::ranges::sort(info.callSites);
    info.callSites.shrink(std::ranges::unique(info.callSites).begin() - info.callSites.begin());
    while (info.siteConstants.size() < info.numSlots)
        info.siteConstants.append(0);
    info.selectors = WTF::move(graph.selectors);
    info.shapes = WTF::move(graph.shapes);
    info.usesStaticImports = graph.usesStaticImports;
    info.startsCold = graph.startsCold;
    RELEASE_ASSERT(!info.startsCold || info.numSlots <= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots));
    info.calleeSaveRegisters = proc.calleeSaveRegisterAtOffsetList();
    for (unsigned i = 0; i < graph.catchEntrypoints.size(); ++i)
        info.catchEntrypoints.append({ graph.catchEntrypoints[i]->bytecodeBegin, offsetOf(proc.code().entrypointLabel(i + 1)) });


    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::dumpAOTDisassembly(), linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
#if CPU(ARM64)
    // Branch compaction leaves nops at the end, after the breakpoint emitted above. The breakpoint stays only if it follows a call: the
    // call's return address identifies the frame, so it must not be the start of the next function.
    {
        constexpr uint32_t breakpoint = 0xd4200000;
        constexpr uint32_t nop = 0xd503201f;
        while (info.codeSize > sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == nop)
            info.codeSize -= sizeof(uint32_t);
        unsigned atLeast = sizeof(uint32_t);
        for (auto& call : info.stubCalls) {
            if (!call.isTailCall)
                atLeast = std::max<unsigned>(atLeast, call.offset + 2 * sizeof(uint32_t));
        }
        while (info.codeSize > atLeast && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == breakpoint)
            info.codeSize -= sizeof(uint32_t);
    }
#endif
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    result.info = WTF::move(info);
    return true;
}

bool recordUsesOfKnownFunctionsForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, const FunctionSummaryMap& summariesByExecutable, VariableSummaries* variableSummaries)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
    if (!parseBytecode(graph))
        return false;
    graph.recordUsesOfKnownFunctions(summariesByExecutable);
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

bool compileForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, CompiledCode& result, const CalleeHints* hints, const ModuleLinkage* linkage, const FunctionSummary* summary, VariableSummaries* variableSummaries, const CodeOfProgram* program)
{
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    bool ok = compile(vm, unlinkedCodeBlock, hints, linkage, result, reason, reasonOpcode, summary, variableSummaries, program);
    if (!ok && Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: declined: ", reason, " ", reasonOpcode != op_nop ? opcodeNames[reasonOpcode] : ""_s);
    return ok;
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
