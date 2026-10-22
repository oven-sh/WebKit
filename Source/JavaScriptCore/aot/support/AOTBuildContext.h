/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTDiagnostics.h"
#include "AOTLimits.h"
#include "AOTLosses.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTWire.h"
#include "AssemblerPolicy.h"
#include "CPUFeatureSet.h"
#include <memory>
#include <optional>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

namespace B3 {
class CompilationScratch;
}

namespace AOT {

class IdentifierNumbering;
class RegExpSummaries;

namespace Typed {
class Program;
class ScratchRealms;
}

class BuildContext {
    WTF_MAKE_TZONE_ALLOCATED(BuildContext);
    WTF_MAKE_NONCOPYABLE(BuildContext);
    WTF_MAKE_NONMOVABLE(BuildContext);
public:
    explicit BuildContext(const Wire::CompileOptions&, const HostThreadPool* = nullptr);
    ~BuildContext();

    const CompileOptions& options() const { return m_options; }
    const RecordedOptions& recordedOptions() const { return m_options.recorded; }
    bool validates() const { return m_options.recorded.validates; }

    bool isEnabled(FlowResultKind kind, uint32_t subject) const { return m_options.recorded.isEnabled(kind, subject); }
    bool isEnabledForProgram(FlowResultKind kind) const { return m_options.recorded.isEnabledForProgram(kind); }
    bool isEnabledEverywhere(FlowResultKind kind) const { return m_options.recorded.isEnabledEverywhere(kind); }
    bool isEnabled(PassID pass, Typed::FunctionID function) const { return m_options.recorded.isEnabled(pass, function); }
    bool isEnabled(SelectionRuleID rule, Typed::FunctionID function) const { return m_options.recorded.isEnabled(rule, function); }
    bool isEnabled(B3::PolicySwitch policySwitch, Typed::FunctionID function) const { return m_options.recorded.isEnabled(policySwitch, function); }

    const ParallelLoops& parallelLoops() const { return m_parallelLoops; }
    unsigned threadCount() const { return m_parallelLoops.width(); }

    CPUFeatureSet targetFeatures() const { return m_assemblerPolicy.features; }
    const AssemblerPolicy& assemblerPolicy() const { return m_assemblerPolicy; }

    Diagnostics& diagnostics() { return m_diagnostics; }
    Statistics& statistics() { return m_statistics; }

    const LimitValues& limits() const { return m_options.limits; }
    uint64_t limit(LimitID limit) const { return m_options.limits[limit]; }
    bool exceeds(LimitID, uint64_t amount, const Subject& = { });
    GaveUp gaveUp(LimitID, const Subject& = { });
    void reportLimitExceeded(LimitID, uint64_t needed, const Subject& = { });
    // A check that repeats work hits the same limits again. Those hits are no losses of the build.
    void setRecordsLimitHits(bool recordsLimitHits) { m_recordsLimitHits = recordsLimitHits; }

    void addDecline(const Wire::DeclineRecord&);
    void addAllocationRemark(const AllocationRemark&);

    const IdentifierNumbering& identifierNumbering() const;
    RegExpSummaries& regExpSummaries() { return *m_regExpSummaries; }
    Typed::ScratchRealms& scratchRealms() { return *m_scratchRealms; }
    B3::CompilationScratch& backEndScratch(unsigned worker);
    void destroyBackEndScratches();
    bool shouldDump(Typed::FunctionID) const;

    template<typename T>
    void destroyLater(T&& value)
    {
        static_assert(!std::is_lvalue_reference_v<T>);
        enqueueDeferredValue(makeUniqueWithoutFastMallocCheck<DeferredValueOf<T>>(WTF::move(value)));
    }

    void finishDeferredDestruction();

    void resolveFunctionToDump(const Typed::Program&);
    void setIdentifierNumbering(IdentifierNumbering&&);
    Diagnostics takeDiagnostics();
    Statistics takeStatistics();
    Vector<Typed::FunctionID> functionsWithUnresolvedWitnesses() { return m_losses.functionsWithUnresolvedWitnesses(); }
    Vector<Wire::LossRecord> takeLosses(const Typed::Program*);
    Vector<Wire::DeclineRecord> takeDeclines(const Typed::Program*);
    Vector<AllocationRemark> takeAllocationRemarks();

private:
    class DeferredValue {
    public:
        virtual ~DeferredValue() = default;
    };

    template<typename T>
    class DeferredValueOf final : public DeferredValue {
    public:
        explicit DeferredValueOf(T&& value)
            : m_value(WTF::move(value))
        {
        }

    private:
        T m_value;
    };

    class DeferredDestroyer;

    void enqueueDeferredValue(std::unique_ptr<DeferredValue>);

    Diagnostics m_diagnostics;
    Statistics m_statistics;
    const CompileOptions m_options;
    const AssemblerPolicy m_assemblerPolicy;
    Losses m_losses;
    bool m_recordsLimitHits { true };
    ParallelLoops m_parallelLoops;
    Vector<std::unique_ptr<B3::CompilationScratch>> m_backEndScratches;
    std::unique_ptr<RegExpSummaries> m_regExpSummaries;
    std::unique_ptr<Typed::ScratchRealms> m_scratchRealms;
    std::optional<BitVector> m_functionsToDump;
    std::unique_ptr<const IdentifierNumbering> m_identifierNumbering;
    std::unique_ptr<DeferredDestroyer> m_deferredDestroyer;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
