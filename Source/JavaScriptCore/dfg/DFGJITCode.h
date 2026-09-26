/*
 * Copyright (C) 2013-2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#if ENABLE(DFG_JIT)

#include "CodeBlock.h"
#include "CompilationResult.h"
#include "DFGCommonData.h"
#include "DFGMinifiedGraph.h"
#include "DFGOSREntry.h"
#include "DFGOSRExit.h"
#include "VMLite.h"
#include "DFGVariableEventStream.h"
#include "ExecutionCounter.h"
#include "JITCode.h"
#include "JumpTable.h"
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/CompactPointerTuple.h>
#include <wtf/Lock.h>
#include <wtf/SegmentedVector.h>

namespace JSC {

class TrackedReferences;

struct SimpleJumpTable;
struct StringJumpTable;

namespace DFG {

class JITCode;
class JITCompiler;

struct UnlinkedPropertyInlineCache : JSC::UnlinkedPropertyInlineCache {
    CallSiteIndex callSiteIndex;
    CodeOrigin codeOrigin;
};

struct UnlinkedCallLinkInfo : JSC::UnlinkedCallLinkInfo {
    void setUpCall(CallLinkInfo::CallType callType)
    {
        this->callType = callType;
    }

    CodeOrigin codeOrigin;
    CallLinkInfo::CallType callType { CallLinkInfo::CallType::None };
};

class LinkerIR {
    WTF_MAKE_NONCOPYABLE(LinkerIR);
public:
    using Constant = unsigned;

    enum class Type : uint8_t {
        Invalid,
        CallLinkInfo,
        CellPointer,
        NonCellPointer,
        GlobalObject,

        // WatchpointSet.
        HavingABadTimeWatchpointSet,
        MasqueradesAsUndefinedWatchpointSet,
        ArrayBufferDetachWatchpointSet,
        ArrayIteratorProtocolWatchpointSet,
        SetIteratorProtocolWatchpointSet,
        NumberToStringWatchpointSet,
        StructureCacheClearedWatchpointSet,
        StringToStringWatchpointSet,
        StringValueOfWatchpointSet,
        StringSymbolMatchWatchpointSet,
        StringSymbolSearchWatchpointSet,
        StringSymbolReplaceWatchpointSet,
        StringSymbolSplitWatchpointSet,
        StringSymbolToPrimitiveWatchpointSet,
        RegExpPrimordialPropertiesWatchpointSet,
        RegExpSpeciesWatchpointSet,
        PromiseThenWatchpointSet,
        ArraySpeciesWatchpointSet,
        ArrayPrototypeChainIsSaneWatchpointSet,
        StringPrototypeChainIsSaneWatchpointSet,
        ObjectPrototypeChainIsSaneWatchpointSet,
        PromiseSpeciesWatchpointSet,
    };

    using Value = JITConstant<Type>;

    struct ValueHash {
        static unsigned hash(const Value& p)
        {
            return p.hash();
        }

        static bool equal(const Value& a, const Value& b)
        {
            return a == b;
        }

        static constexpr bool safeToCompareToEmptyOrDeleted = true;
    };

    struct ValueTraits : public WTF::GenericHashTraits<Value> {
        static constexpr bool emptyValueIsZero = true;
        static Value emptyValue() { return Value(); }
        static void constructDeletedValue(Value& slot) { slot = Value(reinterpret_cast<void*>(static_cast<uintptr_t>(0x1)), Type::Invalid); }
        static bool isDeletedValue(Value value)
        {
            return value == Value(reinterpret_cast<void*>(static_cast<uintptr_t>(0x1)), Type::Invalid);
        }
    };

    LinkerIR() = default;
    LinkerIR(LinkerIR&&) = default;
    LinkerIR& operator=(LinkerIR&&) = default;

    LinkerIR(Vector<Value>&& constants)
        : m_constants(WTF::move(constants))
    {
    }

    size_t size() const { return m_constants.size(); }
    Value at(size_t i) const { return m_constants[i]; }

private:
    FixedVector<Value> m_constants;
};

class JITData final : public ButterflyArray<JITData, HandlerPropertyInlineCache, void*> {
    friend class JSC::LLIntOffsetsExtractor;
public:
    using Base = ButterflyArray<JITData, HandlerPropertyInlineCache, void*>;
    using ExitJumpTable = FixedVector<CodePtr<OSRExitPtrTag>>;

    static constexpr ptrdiff_t offsetOfExitJumpTable() { return OBJECT_OFFSETOF(JITData, m_exitJumpTable); }
    static constexpr ptrdiff_t offsetOfIsInvalidated() { return OBJECT_OFFSETOF(JITData, m_isInvalidated); }

    static std::unique_ptr<JITData> tryCreate(VM&, CodeBlock*, const JITCode&, ExitJumpTable&&);

    void appendExitStub(unsigned exitIndex, MacroAssemblerCodeRef<OSRExitPtrTag> code)
    {
        ASSERT(!m_exitStubs.containsIf([&](const OSRExitStub& stub) { return stub.exitIndex == exitIndex; }));
        m_exitStubs.append({ exitIndex, WTF::move(code) });
    }
    void setExitJumpTableEntry(unsigned exitIndex, CodePtr<OSRExitPtrTag> code) { m_exitJumpTable[exitIndex] = code; }
    const OSRExitStubs& exitStubs() const LIFETIME_BOUND { return m_exitStubs; }

    // GIL off, generated code never has an exit entrance patched under it (no patching of
    // reachable code outside a stop): every DFG code block dispatches its exits through the
    // exit jump table, as unlinked code does (JITCompiler::linkOSRExits). The entry is the one
    // word the dispatch's far jump and the lock-free probe of operationCompileOSRExit read.
    // The writer holds the OSR exit generation lock and has appended the stub, which keeps the
    // ramp's memory alive, before it publishes. Bumping the stop generation before the store
    // makes "saw the pointer" imply "saw the bump", so a reader's generation compare
    // (jsThreadsSyncToStopGenerationBeforeJITEntry) stands in for a serializing instruction
    // per exit.
    void publishExitJumpTableEntryConcurrently(unsigned exitIndex, CodePtr<OSRExitPtrTag> code)
    {
        static_assert(sizeof(CodePtr<OSRExitPtrTag>) == sizeof(void*));
        jsThreadsBumpStopGeneration();
        WTF::storeStoreFence();
        WTF::atomicStore(std::bit_cast<void**>(&m_exitJumpTable[exitIndex]), code.taggedPtr(), std::memory_order_relaxed);
    }

    // Pairs with publishExitJumpTableEntryConcurrently(); the caller issues WTF::loadLoadFence()
    // after it sees a value that is not the generation thunk.
    void* exitJumpTableEntryConcurrently(unsigned exitIndex) const
    {
        return WTF::atomicLoad(std::bit_cast<void**>(const_cast<CodePtr<OSRExitPtrTag>*>(&m_exitJumpTable[exitIndex])), std::memory_order_relaxed);
    }

    bool isInvalidated() const { return !!m_isInvalidated; }

    void invalidate()
    {
        m_isInvalidated = 1;
    }

    auto propertyInlineCaches() -> decltype(leadingSpan())
    {
        return leadingSpan();
    }

    HandlerPropertyInlineCache& propertyCache(unsigned index)
    {
        auto span = propertyInlineCaches();
        return span[span.size() - index - 1];
    }

    FixedVector<OptimizingCallLinkInfo>& callLinkInfos() LIFETIME_BOUND { return m_callLinkInfos; }

    UpperTierExecutionCounter& tierUpCounter() LIFETIME_BOUND
    {
        if (processIsGILOff()) [[unlikely]]
            return gilOffFields().tierUpCounter;
        return m_tierUpCounter;
    }
    const UpperTierExecutionCounter& tierUpCounter() const LIFETIME_BOUND { return const_cast<JITData*>(this)->tierUpCounter(); }

    uint8_t neverExecutedEntry() const { return m_neverExecutedEntry; }

    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(JITData, m_globalObject); }
    static constexpr ptrdiff_t offsetOfStackOffset() { return OBJECT_OFFSETOF(JITData, m_stackOffset); }
    static constexpr ptrdiff_t offsetOfDummyArrayProfile() { return OBJECT_OFFSETOF(JITData, m_dummyArrayProfile); }
    // As BaselineJITData::GILOffFields: with the GIL off every thread running this DFG code bumps
    // the tier-up counter, and nothing read-mostly may share its line, so the counter is in the
    // first slots of the trailing pool, between two lines of padding. Otherwise it is
    // m_tierUpCounter and the object has the layout it has without the threads work.
    struct GILOffFields {
        char padBeforeCounter[64];
        UpperTierExecutionCounter tierUpCounter;
        char padAfterCounter[64];
    };
    static constexpr unsigned gilOffReservedPoolSlots = (sizeof(GILOffFields) + sizeof(void*) - 1) / sizeof(void*);
    static unsigned reservedPoolSlots() { return processIsGILOff() ? gilOffReservedPoolSlots : 0; }
    static ptrdiff_t offsetOfTierUpCounterObject()
    {
        if (processIsGILOff()) [[unlikely]]
            return offsetOfTrailingData() + OBJECT_OFFSETOF(GILOffFields, tierUpCounter);
        return OBJECT_OFFSETOF(JITData, m_tierUpCounter);
    }
    static ptrdiff_t offsetOfTierUpCounter() { return offsetOfTierUpCounterObject() + OBJECT_OFFSETOF(UpperTierExecutionCounter, m_counter); }
    static ptrdiff_t offsetOfTierUpActiveThreshold() { return offsetOfTierUpCounterObject() + OBJECT_OFFSETOF(UpperTierExecutionCounter, m_activeThreshold); }
    static ptrdiff_t offsetOfTierUpTotalCount() { return offsetOfTierUpCounterObject() + OBJECT_OFFSETOF(UpperTierExecutionCounter, m_totalCount); }
    static ptrdiff_t offsetOfConstant(unsigned index) { return offsetOfTrailingData() + (reservedPoolSlots() + index) * sizeof(void*); }
    std::span<void*> constants() LIFETIME_BOUND { return trailingSpan().subspan(reservedPoolSlots()); }
    GILOffFields& gilOffFields() LIFETIME_BOUND
    {
        ASSERT(processIsGILOff());
        return *std::bit_cast<GILOffFields*>(trailingSpan().data());
    }
    ~JITData();
    static constexpr ptrdiff_t offsetOfNeverExecutedEntry() { return OBJECT_OFFSETOF(JITData, m_neverExecutedEntry); }

    explicit JITData(unsigned propertyCacheSize, unsigned poolSize, const JITCode&, ExitJumpTable&&);

    void reconcileWeakReferencesAtGCEnd()
    {
        m_dummyArrayProfile.clear();
    }

private:

    bool tryInitialize(VM&, CodeBlock*, const JITCode&);

    JSGlobalObject* m_globalObject { nullptr }; // This is not marked since owner CodeBlock will mark JSGlobalObject.
    intptr_t m_stackOffset { 0 };
    ArrayProfile m_dummyArrayProfile { };
    UpperTierExecutionCounter m_tierUpCounter; // Not the counter with the GIL off: GILOffFields.
    FixedVector<OptimizingCallLinkInfo> m_callLinkInfos;
    FixedVector<CodeBlockJettisoningWatchpoint> m_watchpoints;
    ExitJumpTable m_exitJumpTable;
    OSRExitStubs m_exitStubs;
    uint8_t m_isInvalidated { 0 };
    uint8_t m_neverExecutedEntry { 1 };
};

#if CPU(X86_64)
static_assert(sizeof(JITData) == 104, "DFG::JITData has the size it has without the threads work");
#endif

class JITCode final : public DirectJITCode {
public:
    JITCode(bool isUnlinked);
    ~JITCode() final;
    
    CommonData* dfgCommon() final;
    const CommonData* dfgCommon() const final;
    JITCode* dfg() final;
    bool isUnlinked() const { return common.isUnlinked(); }
    
    OSREntryData* osrEntryDataForBytecodeIndex(BytecodeIndex bytecodeIndex)
    {
        return tryBinarySearch<OSREntryData, BytecodeIndex>(
            m_osrEntry, m_osrEntry.size(), bytecodeIndex,
            getOSREntryDataBytecodeIndex);
    }

    void finalizeOSREntrypoints(Vector<DFG::OSREntryData>&&);
    
    void reconstruct(
        CodeBlock*, CodeOrigin, unsigned streamIndex, Operands<ValueRecovery>& result);
    
    // This is only applicable if we're at a point where all values are spilled to the
    // stack. Currently, it also has the restriction that the values must be in their
    // bytecode-designated stack slots.
    void reconstruct(
        CallFrame*, CodeBlock*, CodeOrigin, unsigned streamIndex, Operands<std::optional<JSValue>>& result);

#if ENABLE(FTL_JIT)
    // NB. All of these methods take CodeBlock* because they may want to use
    // CodeBlock's logic about scaling thresholds. It should be a DFG CodeBlock.
    
    bool checkIfOptimizationThresholdReached(CodeBlock*);
    void optimizeNextInvocation(CodeBlock*);
    void dontOptimizeAnytimeSoon(CodeBlock*);
    void optimizeAfterWarmUp(CodeBlock*);
    void optimizeSoon(CodeBlock*);
    void forceOptimizationSlowPathConcurrently(CodeBlock*);
    void setOptimizationThresholdBasedOnCompilationResult(CodeBlock*, CompilationResult);
#endif // ENABLE(FTL_JIT)
    
    void validateReferences(const TrackedReferences&) final;
    
    void shrinkToFit() final;

    RegisterSet liveRegistersToPreserveAtExceptionHandlingCallSite(CodeBlock*, CallSiteIndex) final;
#if ENABLE(FTL_JIT)
    CodeBlock* osrEntryBlock() LIFETIME_BOUND { return m_osrEntryBlock.get(); }
    void setOSREntryBlock(VM&, const JSCell* owner, CodeBlock* osrEntryBlock);
    void clearOSREntryBlockAndResetThresholds(CodeBlock* dfgCodeBlock);
#endif

    static constexpr ptrdiff_t commonDataOffset() { return OBJECT_OFFSETOF(JITCode, common); }

    std::optional<CodeOrigin> findPC(CodeBlock*, void* pc) final;

    using DirectJITCode::initializeCodeRefForDFG;

    PCToCodeOriginMap* pcToCodeOriginMap() override { return common.m_pcToCodeOriginMap.get(); }
    
private:
    friend class JITCompiler; // Allow JITCompiler to call setCodeRef().

public:
    CommonData common;
    FixedVector<DFG::OSREntryData> m_osrEntry;
    DFG::OSRExitStream m_osrExits;
    CodeLocationLabel<JSInternalPtrTag> m_osrExitEntrances;

    CodeLocationLabel<JSInternalPtrTag> osrExitEntrance(unsigned exitIndex)
    {
        return m_osrExitEntrances.labelAtOffset(exitIndex * osrExitEntranceSize);
    }

    unsigned osrExitIndexForReturnPC(void* returnPC) const
    {
        size_t offset = reinterpret_cast<uintptr_t>(returnPC) - m_osrExitEntrances.dataLocation<uintptr_t>();
        ASSERT(offset && !(offset % osrExitEntranceSize));
        return offset / osrExitEntranceSize - 1;
    }
    FixedVector<DFG::SpeculationRecovery> m_speculationRecovery;
    FixedVector<SimpleJumpTable> m_switchJumpTables;
    FixedVector<StringJumpTable> m_stringSwitchJumpTables;
    FixedVector<UnlinkedPropertyInlineCache> m_unlinkedPropertyInlineCaches;
    FixedVector<UnlinkedCallLinkInfo> m_unlinkedCallLinkInfos;
    DFG::VariableEventStream variableEventStream;
    DFG::MinifiedGraph minifiedDFG;
    LinkerIR m_linkerIR;

#if ENABLE(FTL_JIT)
    // For osrEntrypoint that are in inner loop, this maps their bytecode to the bytecode
    // of the outerloop entry points in order (from innermost to outermost).
    //
    // The key may not always be a target for OSR Entry but the list in the value is guaranteed
    // to be usable for OSR Entry.
    UncheckedKeyHashMap<BytecodeIndex, FixedVector<BytecodeIndex>> tierUpInLoopHierarchy;

    // Map each bytecode of CheckTierUpAndOSREnter to its stream index.
    UncheckedKeyHashMap<BytecodeIndex, unsigned> bytecodeIndexToStreamIndex;

    enum class TriggerReason : uint8_t {
        DontTrigger,
        CompilationDone,
        StartCompilation,
    };

    // Map each bytecode of CheckTierUpAndOSREnter to its trigger forcing OSR Entry.
    // This can never be modified after it has been initialized since the addresses of the triggers
    // are used by the JIT.
    //
    // THREADS (DFG-1): GIL-off, N mutators reach the tier-up slow paths concurrently on the
    // same CodeBlock. m_tierUpTriggersLock guards every post-link C++ access (find/iterate/
    // value-write) to tierUpEntryTriggers and to tierUpInLoopHierarchy. JIT'd code keeps
    // reading the embedded TriggerReason bytes lock-free; that stays sound because keys are
    // never added or removed after link (no rehash => stable value addresses) and the payload
    // is a single byte. Post-link updates must go through locked find()+in-place value write —
    // never set()/add() — so structural mutation is impossible by construction.
    //
    // Post-link C++ writers that address triggers by BytecodeIndex must go through
    // setTierUpEntryTrigger() (locked find() + in-place value write; RELEASE_ASSERTs the key
    // exists, so structural mutation/rehash is impossible by construction). Releasing
    // m_tierUpTriggersLock also supplies the release edge ordering setOSREntryBlock before
    // the CompilationDone publication on weak memory. Carve-out: the compiler thread's raw
    // single-byte store through m_forcedOSREntryTrigger in
    // ToFTLForOSREntryDeferredCompilationCallback::compilationDidBecomeReadyAsynchronously is
    // permitted — stable post-link address, single-byte payload, same contract as the
    // JIT-embedded lock-free reads.
    //
    // The former KNOWN GAP is closed (DFG-1, this round): DFGToFTLForOSREntryDeferredCompilationCallback
    // ::compilationDidComplete was converted to setTierUpEntryTrigger() (locked find() +
    // in-place value write), so the locked-writer set is complete — no post-link structural
    // writer of tierUpEntryTriggers remains and the no-rehash guarantee holds.
    UncheckedKeyHashMap<BytecodeIndex, TriggerReason> tierUpEntryTriggers;
    void setTierUpEntryTrigger(BytecodeIndex, TriggerReason);
    Lock m_tierUpTriggersLock;

    WriteBarrier<CodeBlock> m_osrEntryBlock;
    std::atomic<unsigned> osrEntryRetry { 0 };
    std::atomic<bool> abandonOSREntry { false };
#endif // ENABLE(FTL_JIT)
};

inline std::unique_ptr<JITData> JITData::tryCreate(VM& vm, CodeBlock* codeBlock, const JITCode& jitCode, ExitJumpTable&& exitJumpTable)
{
    auto result = std::unique_ptr<JITData> { createImpl(jitCode.m_unlinkedPropertyInlineCaches.size(), jitCode.m_linkerIR.size() + reservedPoolSlots(), jitCode, WTF::move(exitJumpTable)) };
    if (result->tryInitialize(vm, codeBlock, jitCode))
        return result;
    return nullptr;
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
