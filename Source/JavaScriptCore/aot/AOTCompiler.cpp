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
        constexpr unsigned significantDepth = 6;
        double frequency = pow(10.0, std::min(loops.loopDepth(block), significantDepth));
        block->setFrequency(likely.contains(block) ? frequency : frequency * Lowering::coldFrequency);
    }
}

// See CompiledFunctionInfo::startsCold.
static bool mayStartCold(UnlinkedCodeBlock* unlinkedCodeBlock)
{
    // (Nothing takes as many slots as it takes bytes.)
    if (unlinkedCodeBlock->codeType() != FunctionCode || unlinkedCodeBlock->instructions().size() > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots) || !constantsAreOfNoRealm(unlinkedCodeBlock, SymbolTablesAreShared::Yes))
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
    if (code.frameSize() || code.calleeSaveRegisterAtOffsetList().registerCount() || graph.alwaysEmitsCalls || !graph.catchEntrypoints.isEmpty())
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

// Options::preferUnsplitAOTLoops(). Of a graph whose loops have not been split.
static bool loopsWillDoWhole(Graph& graph)
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

static bool compile(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const CalleeHints* hints, const ModuleLinkage* linkage, CompiledCode& result, ASCIILiteral& reason, OpcodeID& reasonOpcode, const FunctionSummary* summary, VariableSummaries* variableSummaries, const CodeOfProgram* program, bool triesLoopsWhole = true)
{
    Graph graph(vm, unlinkedCodeBlock, unknownScopeChain());
    triesLoopsWhole &= Options::preferUnsplitAOTLoops() && Options::useImmutableIntrinsics() && !Options::useAOTFunctionSplitting();
    graph.loopsAreNotSplit = triesLoopsWhole;
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
    // (With no loop split there is no choice to make: LoopOptimizer::viewArrays() looks at each loop by itself.)
    if (triesLoopsWhole && Options::useAOTLoopSplitting() && Options::aotLoopSplittingPolicy() && !loopsWillDoWhole(graph))
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

    // The code is going to run in another process, where nothing is where it is here. No lowering should have put an address in
    // it; running the code only shows that for the paths that are taken, so look. (No JSValue that is not a cell is in this range.)
    for (B3::Value* value : proc.values()) {
        if (!value->hasInt64())
            continue;
        uint64_t bits = value->asInt64();
        // (But for where structures are, which is the same in every process: structureIDBaseOfImages.)
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
        // The limit leaves room for the runtime to do what it has to when the stack is used up, which is a great deal more than this.
        // However deep the calls go, whatever made the last of them has checked.
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
            noteKeysOf(unlinkedCodeBlock->functionExpr(i), info.functionExpressionsWritten);
        for (UnlinkedFunctionExecutable* executable : graph.functionsMade)
            noteKeysOf(executable, info.functionsMade);
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
