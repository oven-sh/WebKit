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

static bool compile(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const ScopeChain& scopeChain, const CalleeHints* hints, const ModuleLinkage* linkage, void* ownerForLinkBuffer, RefPtr<JITCode>& result, ASCIILiteral& reason, OpcodeID& reasonOpcode)
{
    Graph graph(vm, unlinkedCodeBlock, scopeChain);
    graph.setCalleeHints(hints);
    graph.setLinkage(linkage, declaredNamesFor(unlinkedCodeBlock));
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
        graph.headerReferences.moveBoxedHeader(jit, boxedHeaderGPR);
        auto becomeFrameOfThisFunction = makeScopeExit([&] {
            jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), GPRInfo::regT9);
            jit.store64(boxedHeaderGPR, CCallHelpers::addressFor(CallFrameSlot::callee));
            jit.store64(GPRInfo::regT9, CCallHelpers::Address(GPRInfo::callFrameRegister, graph.calleeSlot->offsetFromFP()));
        });
        // The limit leaves room for the runtime to do what it has to when the stack is used up, which is a great deal more than this.
        // However deep the calls go, whatever made the last of them has checked.
        constexpr unsigned frameSizeThatNeedsNoCheck = 256;
        if (usesStubs && !makesCalls && code.frameSize() <= frameSizeThatNeedsNoCheck) {
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
            jit.emitSave(code.calleeSaveRegisterAtOffsetList());
            return;
        }
        if constexpr (usesStubs) {
            jit.move(CCallHelpers::TrustedImm32(code.frameSize()), GPRInfo::regT9);
            stubCalls.call(jit, Stub::Prologue);
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
    if (usesStubs && checksArity) {
        // What is done about too few comes first: it refers to the header, which has to be within reach.
        CCallHelpers::Label tooFewArguments = jit.label();
        graph.headerReferences.moveBoxedHeader(jit, boxedHeaderGPR); // In case there is no room for more.
        jit.move(CCallHelpers::linkRegister, GPRInfo::regT10);
        jit.move(CCallHelpers::TrustedImm32(numParameters), GPRInfo::regT9);
        stubCalls.call(jit, Stub::ArityCheck);
        arityFixed = jit.jump();

        // Nothing has been pushed: the frame's slots are found from the stack pointer.
        arityCheckWithStub = jit.label();
        jit.load32(CCallHelpers::calleeFrameSlot(CallFrameSlot::argumentCountIncludingThis).withOffset(sizeof(CallerFrameAndPC) - prologueStackPointerDelta() + LowWordOffset), GPRInfo::regT9);
        jit.branch32(CCallHelpers::Below, GPRInfo::regT9, CCallHelpers::TrustedImm32(numParameters)).linkTo(tooFewArguments, &jit);
        // The only way in is what comes first.
        if (!graph.catchEntrypoints.isEmpty())
            arityChecked = jit.jump();
    }
    CCallHelpers::Label startOfCode = jit.label();
    B3::generate(proc, jit);

    CCallHelpers::Label entryLabel = proc.code().entrypointLabel(0);
    CCallHelpers::Label arityCheckLabel = entryLabel;
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
        contents.calleeSlot = safeCast<int16_t>(graph.calleeSlot->offsetFromFP() / static_cast<int>(sizeof(Register)));
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
    info.frameSizeInBytes = proc.frameSize();
    info.numSlots = graph.numICSlots;
    info.sites = WTF::move(graph.sites);
    while (info.sites.size() < info.numSlots)
        info.sites.append(Site { });
    info.knownCallees = WTF::move(graph.knownCallees);
    info.bytecodeHash = hashOfBytecode(unlinkedCodeBlock);
    info.usesStaticImports = graph.usesStaticImports;
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
        stats.byOrigin[0].first++;
    }

    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::aotDumpDisassembly(), linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
    result = adoptRef(*new JITCode(start, codeRef.executableMemory(), WTF::move(info)));
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

bool compileForImage(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, CompiledCode& result, const CalleeHints* hints, const ModuleLinkage* linkage)
{
    MonotonicTime before = MonotonicTime::now();
    RefPtr<JITCode> jitCode;
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    bool ok = compile(vm, unlinkedCodeBlock, unknownScopeChain(), hints, linkage, nullptr, jitCode, reason, reasonOpcode);
    if (Options::aotReportStats()) [[unlikely]]
        recordStatistics(ok, ok ? jitCode->size() : 0, unlinkedCodeBlock->instructionsSize(), MonotonicTime::now() - before, reason, reasonOpcode);
    if (!ok)
        return false;
    // Out of the JIT's memory, which goes back to the JIT.
    result.info = jitCode->info();
    result.bytes.append(std::span { static_cast<const uint8_t*>(jitCode->dataAddressAtOffset(0)), jitCode->size() });
    return true;
}

bool tryCompileAndInstall(VM& vm, CodeBlock* codeBlock, JSScope* scope)
{
    if (codeBlock->codeType() == EvalCode)
        return false;
    if (const char* filter = Options::aotFilter()) {
        if (!strstr(codeBlock->inferredName().data(), filter))
            return false;
    }

    if (unsigned skip = Options::aotSkip()) {
        static std::atomic<unsigned> count;
        if (++count == skip)
            return false;
    }
    if (unsigned limit = Options::aotLimit()) {
        // For finding the function that is miscompiled: bisect on the limit, and the last one compiled is it.
        static std::atomic<unsigned> count;
        unsigned index = ++count;
        if (index > limit)
            return false;
        if (index == limit) {
            codeBlock->dumpBytecode();
            Options::aotDumpGraph() = true;
        }
        if (index == limit)
            dataLogLn("AOT: function #", index, " is ",codeBlock->inferredName(), " ", String::fromUTF8(codeBlock->sourceCodeForTools().span()).left(400));
    }

    MonotonicTime before = MonotonicTime::now();
    RefPtr<JITCode> jitCode;
    ASCIILiteral reason;
    OpcodeID reasonOpcode = op_nop;
    UnlinkedCodeBlock* unlinkedCodeBlock = codeBlock->unlinkedCodeBlock();
    // By default, with no more to go by than there is when a program is compiled before it is run.
    std::unique_ptr<LiveHints> hints;
    if (Options::aotUseLiveCalleeHints())
        hints = makeUnique<LiveHints>(codeBlock->globalObject());
    bool ok = compile(vm, unlinkedCodeBlock, Options::aotUseLiveScopes() ? scopeChainFor(scope) : unknownScopeChain(), hints.get(), nullptr, codeBlock, jitCode, reason, reasonOpcode);

    if (Options::aotVerbose()) [[unlikely]] {
        if (ok) {
            dataLogLn("AOT: compiled ", codeBlock->inferredName(), ": ", jitCode->size(), " bytes for ", unlinkedCodeBlock->instructionsSize(), " of bytecode, frame ", jitCode->info().frameSizeInBytes);
            if (auto key = imageKeyFor(codeBlock->ownerExecutable(), codeBlock->specializationKind()))
                dataLogLn("AOT: ", codeBlock->inferredName(), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") is at ", RawPointer(jitCode->dataAddressAtOffset(0)), " size ", jitCode->size(), " hash ", hashOfCode({ static_cast<const uint8_t*>(jitCode->dataAddressAtOffset(0)), jitCode->size() }));
        }
        else
            dataLogLn("AOT: declined ", codeBlock->inferredName(), ": ", reason, reasonOpcode != op_nop ? " " : "", reasonOpcode != op_nop ? opcodeNames[reasonOpcode] : ""_s);
    }
    if (Options::aotReportStats()) [[unlikely]]
        recordStatistics(ok, ok ? jitCode->size() : 0, unlinkedCodeBlock->instructionsSize(), MonotonicTime::now() - before, reason, reasonOpcode);
    if (!ok)
        return false;

    if (Options::aotWriteImage()) [[unlikely]]
        addToImageBeingWritten(codeBlock, *jitCode);
    codeBlock->installAOTCode(jitCode.releaseNonNull());
    return true;
}

void reportStatistics()
{
    auto& stats = statistics();
    Locker locker { stats.lock };
    dataLogLn("AOT: compiled ", stats.compiled, " functions (", stats.codeBytes, " bytes of code for ", stats.bytecodeBytes, " of bytecode) in ", stats.time.milliseconds(), " ms; declined ", stats.declined);
    dataLogLn("AOT: reads of variables by name: ", stats.scopeReads[Graph::StaticVariable::Closure], " closure, ", stats.scopeReads[Graph::StaticVariable::Import], " linked imports, ", stats.scopeReads[Graph::StaticVariable::ModuleImport], " other imports, ", stats.scopeReadsInGlobalScopes, " globals, ", stats.scopeReads[Graph::StaticVariable::Unresolved] - stats.scopeReadsInGlobalScopes, " unresolved, ", stats.scopeReads[Graph::StaticVariable::Dynamic], " dynamic; ", stats.slots, " slots");
    Vector<std::pair<unsigned, String>> sorted;
    for (auto& entry : stats.reasons)
        sorted.append({ entry.value, entry.key });
    std::ranges::sort(sorted, [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& entry : sorted)
        dataLogLn("    ", entry.first, "  ", entry.second);
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
