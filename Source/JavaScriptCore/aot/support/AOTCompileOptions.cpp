/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileOptions.h"

#if ENABLE(AOT)

#include "AOTCompileOptionWords.h"
#include "AOTDiagnostics.h"
#include "AOTLimits.h"
#include "AOTSchemaEnums.h"
#include "AOTWireOptionMembers.h"
#include "CPUFeatureSet.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <wtf/NumberOfCores.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

using WireOptions = Wire::CompileOptions;

static_assert(std::is_same_v<decltype(WireOptions::sizeClassProgression), decltype(CompileOptions::sizeClassProgression)>);
static_assert(std::is_same_v<decltype(WireOptions::executableIdentity), decltype(CompileOptions::executableIdentity)>);
static_assert(std::is_same_v<decltype(WireOptions::removedFeatures), decltype(CompileOptions::removedFeatures)>);
static_assert(std::is_same_v<decltype(Wire::TargetOptions::featuresRetainedByName), decltype(CompileOptions::featuresRetainedByName)>);

static_assert(numberOfEnumerators<Architecture> <= 256);
static_assert(numberOfEnumerators<ReferenceWidth> <= 256);
static_assert(numberOfEnumerators<ImageForm> <= 256);

static_assert(CompileAction { } == CompileAction::Compile);
static_assert(ReferenceWidth { } == ReferenceWidth::Wide);

static_assert(Limits::maxWorkers >= 1);

static constexpr bool hasOnlyKnownFeatures(uint64_t features)
{
    static_assert(numberOfCPUFeatures < 64);
    return !(features >> numberOfCPUFeatures);
}

bool RecordedOptions::isCompatibleWithProcess() const
{
    if (architecture != static_cast<uint8_t>(hostArchitecture()) || form() != imageFormOfThisRuntime())
        return false;
    if (!hasOnlyKnownFeatures(targetFeatures))
        return false;
    if (!CPUFeatureSet::fromBits(static_cast<uint32_t>(targetFeatures)).isSubsetOf(CPUFeatureSet::host().unionWith(CPUFeatureSet::buildTarget())))
        return false;
    return useWideReferences() || MarkedBlockCage::isEnabled();
}

static LoggingOptions readLoggingOptions()
{
    LoggingOptions logging;
    logging.logsPhaseTimes = Options::logAOTPhaseTimes();
    logging.mapFilePath = UTF8CString { Options::aotMapFilePath() };
    return logging;
}

namespace {

class WireOptionsReader {
public:
    WireOptionsReader(const WireOptions& wire, Diagnostics& diagnostics)
        : m_wire(wire)
        , m_diagnostics(diagnostics)
    {
    }

    CompileOptions read()
    {
        if (m_wire.size != sizeof(WireOptions)) {
            reportInvalid(m_wire.size);
            return { };
        }
        Wire::forEachEnumeratedMember(m_wire, [&]<typename E>(E& member) {
            constexpr unsigned numberOfMembers = numberOfEnumerators<E>;
            static_assert(!isFlagWord<E> || numberOfMembers <= 8 * sizeof(E));
            constexpr uint64_t bitsOfMembers = numberOfMembers < 64 ? (1ull << numberOfMembers % 64) - 1 : ~0ull;
            uint64_t word = static_cast<std::underlying_type_t<E>>(member);
            if (isFlagWord<E> ? !(word & ~bitsOfMembers) : word < numberOfMembers)
                return;
            reportInvalid(member);
            member = { };
        });

        CompileOptions options;
        RecordedOptions& recorded = options.recorded;

        readTarget(options);
        readChecks(options);
        readThreadCount(options);
        readSizeClassProgression(options);
        readTesting(options);

        recorded.admittedCode = static_cast<uint8_t>(m_wire.admittedCode);

        options.action = m_wire.action;
        options.reports = m_wire.reports;
        options.output = m_wire.output;
        options.excludedCode = m_wire.excludedCode;
        recorded.omitsFunctionSource = hasFlag(options.excludedCode, ExcludedCode::FunctionSource);
        options.executableIdentity = m_wire.executableIdentity;
        options.removedFeatures = m_wire.removedFeatures;
        options.featuresRetainedByName = m_wire.target.featuresRetainedByName;
        options.logging = readLoggingOptions();
        return options;
    }

private:
    template<typename Member>
    void reportInvalid(const Member& member)
    {
        size_t offset = std::bit_cast<uintptr_t>(&member) - std::bit_cast<uintptr_t>(&m_wire);
        RELEASE_ASSERT(offset < sizeof(WireOptions));
        m_diagnostics.add(InternalError { .kind = InternalErrorKind::InvalidCompileOptions, .count = static_cast<uint32_t>(offset) });
    }

    void readTarget(CompileOptions& options)
    {
        const Wire::TargetOptions& target = m_wire.target;

        Architecture architecture = target.architecture;
        if (architecture != hostArchitecture())
            reportInvalid(target.architecture);

        uint64_t features = target.features;
        if (!hasOnlyKnownFeatures(features)) {
            reportInvalid(target.features);
            features = 0;
        }

        options.recorded.architecture = static_cast<uint8_t>(architecture);
        options.recorded.targetFeatures = features;
        options.recorded.referenceWidth = static_cast<uint8_t>(target.referenceWidth);
        options.recorded.imageForm = static_cast<uint8_t>(target.imageForm);
        if (target.reserved)
            reportInvalid(target.reserved);
    }

    void readChecks(CompileOptions& options)
    {
        CheckSet checks = m_wire.checks;
        options.recorded.validates = hasFlag(checks, CheckSet::Validate);
        options.verifiesCode = options.recorded.validates || hasFlag(checks, CheckSet::VerifyEachStage);
        options.verifiesMachineCode = options.verifiesCode || Options::verifyAOTMachineCode();
        options.checksFlowFixpoint = hasFlag(checks, CheckSet::FlowFixpoint);
        options.replaysFinalLoopVisits = hasFlag(checks, CheckSet::ReplayFinalLoopVisits);
        options.repeatsFittingLoopVisits = hasFlag(checks, CheckSet::RepeatFittingLoopVisits);
        options.checksFlowMonotonicity = hasFlag(checks, CheckSet::FlowMonotonicity);
    }

    void readThreadCount(CompileOptions& options)
    {
        if (!m_wire.threadCount) {
            reportInvalid(m_wire.threadCount);
            return;
        }
        options.threadCount = static_cast<unsigned>(std::min<uint64_t>(m_wire.threadCount, Limits::maxWorkers));
    }

    void readSizeClassProgression(CompileOptions& options)
    {
        double progression = m_wire.sizeClassProgression;
        if (!std::isfinite(progression) || progression <= 1) {
            reportInvalid(m_wire.sizeClassProgression);
            return;
        }
        options.sizeClassProgression = progression;
    }

    void readTesting(CompileOptions& options)
    {
        options.testing = m_wire.testing;
        options.injectedFaults = m_wire.injectedFaults;
        options.recorded.verifiesFastPathAnswers = hasFlag(m_wire.testing, TestingSwitch::CheckWalkAtRunTime);
        if (m_wire.understatedEffects && !hasFlag(m_wire.testing, TestingSwitch::UnderstateEffects))
            reportInvalid(m_wire.understatedEffects);
        options.understatedEffects = m_wire.understatedEffects;

        bool shouldFailSpeculations = hasFlag(m_wire.testing, TestingSwitch::FailSpeculations);
        bool drawsBySeed = hasFlag(m_wire.testing, TestingSwitch::ShuffleBlockFrequencies) || hasFlag(m_wire.testing, TestingSwitch::ShuffleExpectedTypes)
            || hasFlag(m_wire.testing, TestingSwitch::WidenFlowNodes) || hasFlag(m_wire.testing, TestingSwitch::ShuffleChoices);
        bool forcesWay = hasFlag(m_wire.testing, TestingSwitch::ForceWalkMode);
        bool misstatesFact = hasFlag(m_wire.injectedFaults, FaultInjection::MisstateFact);
        if (drawsBySeed ? !m_wire.shuffleSeed : m_wire.shuffleSeed && !shouldFailSpeculations && !forcesWay && !misstatesFact)
            reportInvalid(m_wire.shuffleSeed);
        if (forcesWay != !!m_wire.forcedWalkModePeriod)
            reportInvalid(m_wire.forcedWalkModePeriod);
        if (forcesWay ? !m_wire.forcedWalkMode || m_wire.forcedWalkMode > 2 : !!m_wire.forcedWalkMode)
            reportInvalid(m_wire.forcedWalkMode);
        options.forcedWalkMode = m_wire.forcedWalkMode;
        options.forcedWalkModePeriod = m_wire.forcedWalkModePeriod;
        if (hasFlag(m_wire.testing, TestingSwitch::AbortLoopVisits) != !!m_wire.loopVisitAbortPeriod)
            reportInvalid(m_wire.loopVisitAbortPeriod);
        if (m_wire.loopVisitAbortPhase && m_wire.loopVisitAbortPhase >= m_wire.loopVisitAbortPeriod)
            reportInvalid(m_wire.loopVisitAbortPhase);
        options.loopVisitAbortPeriod = m_wire.loopVisitAbortPeriod;
        options.loopVisitAbortPhase = m_wire.loopVisitAbortPhase;
        if (shouldFailSpeculations != !!m_wire.speculationFailurePeriod)
            reportInvalid(m_wire.speculationFailurePeriod);
        if (misstatesFact != !!m_wire.misstatedFact)
            reportInvalid(m_wire.misstatedFact);
        options.misstatedFact = m_wire.misstatedFact;
        options.shuffleSeed = m_wire.shuffleSeed;
        options.speculationFailurePeriod = m_wire.speculationFailurePeriod;
    }

    WireOptions m_wire;
    Diagnostics& m_diagnostics;
};

} // anonymous namespace

CompileOptions CompileOptions::fromWire(const Wire::CompileOptions& wire, Diagnostics& diagnostics)
{
    return WireOptionsReader(wire, diagnostics).read();
}

Wire::CompileOptions makeDefaultCompileOptions()
{
    static constexpr unsigned mostUsefulThreads = 16;

    Wire::CompileOptions options { };
    options.size = sizeof(Wire::CompileOptions);
    options.threadCount = std::clamp<unsigned>(WTF::numberOfProcessorCores(), 1, std::min<unsigned>(Limits::maxWorkers, mostUsefulThreads));
    options.target.architecture = hostArchitecture();
    options.target.features = CPUFeatureSet::buildTarget().bits();
    options.target.imageForm = imageFormOfThisRuntime();
    options.sizeClassProgression = Options::sizeClassProgressionForCompiledCode;
    return options;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
