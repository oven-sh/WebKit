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
static_assert(std::is_same_v<decltype(Wire::TargetOptions::featuresKeptOnlyWhenNamed), decltype(CompileOptions::featuresKeptOnlyWhenNamed)>);

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
            uint64_t end = numberOfEnumerators<E>;
            if constexpr (isFlagWord<E>) {
                static_assert(numberOfEnumerators<E> <= 32);
                end = 1ull << numberOfEnumerators<E>;
            }
            if (static_cast<uint32_t>(member) < end)
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
        readOutput(options);
        readTesting(options);

        recorded.admittedCode = static_cast<uint8_t>(m_wire.admittedCode);

        options.action = m_wire.action;
        options.reports = m_wire.reports;
        options.excludedCode = m_wire.excludedCode;
        recorded.omitsFunctionSource = hasFlag(options.excludedCode, ExcludedCode::FunctionSource);
        options.executableIdentity = m_wire.executableIdentity;
        options.removedFeatures = m_wire.removedFeatures;
        options.featuresKeptOnlyWhenNamed = m_wire.target.featuresKeptOnlyWhenNamed;
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
        options.verifiesMachineCode = options.recorded.validates || Options::verifyAOTMachineCode();
        options.asksWhetherFlowOfLinkIsClosed = hasFlag(checks, CheckSet::FlowFixpoint);
        options.walksLastWalksOfLoopsAlone = hasFlag(checks, CheckSet::LastWalksOfLoopsAlone);
        options.walksLoopsThatFitAgain = hasFlag(checks, CheckSet::WalksOfLoopsThatFitAgain);
        options.asksWhetherLinkFindsLessFromMore = hasFlag(checks, CheckSet::FlowFindsNoLessFromMore);
#if ASSERT_ENABLED || ASAN_ENABLED
        options.verifiesAfterEveryStage = hasFlag(checks, CheckSet::VerifyEachStage);
        options.verifiesProgram = options.verifiesAfterEveryStage || options.recorded.validates || Options::verifyAOTProgram();
        options.validatesReferenceGuards = hasFlag(checks, CheckSet::ValidateReferenceGuards);
        options.validatesReferences = options.validatesReferenceGuards || hasFlag(checks, CheckSet::ValidateReferences);
#endif
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

    void readOutput(CompileOptions& options)
    {
        if (hasFlag(m_wire.output, OutputOptions::SharesRealms))
            reportInvalid(m_wire.output);
        if (m_wire.target.imagesBelow.address || m_wire.target.imagesBelow.size)
            reportInvalid(m_wire.target.imagesBelow);
        options.output = m_wire.output;
    }

    void readTesting(CompileOptions& options)
    {
        options.testing = m_wire.testing;
        if (m_wire.rowThatSaysLess && !hasFlag(m_wire.testing, TestingSwitch::RowSaysLess))
            reportInvalid(m_wire.rowThatSaysLess);
        options.rowThatSaysLess = m_wire.rowThatSaysLess;

        bool failsGuesses = hasFlag(m_wire.testing, TestingSwitch::FailGuesses);
        bool drawsBySeed = hasFlag(m_wire.testing, TestingSwitch::ShuffleBlockFrequencies) || hasFlag(m_wire.testing, TestingSwitch::ShuffleExpectedTypes)
            || hasFlag(m_wire.testing, TestingSwitch::GiveLinkMore) || hasFlag(m_wire.testing, TestingSwitch::ShuffleChoices);
        if (drawsBySeed ? !m_wire.seedOfShuffles : m_wire.seedOfShuffles && !failsGuesses)
            reportInvalid(m_wire.seedOfShuffles);
        if (failsGuesses != !!m_wire.periodOfGuessesThatFail)
            reportInvalid(m_wire.periodOfGuessesThatFail);
        options.seedOfShuffles = m_wire.seedOfShuffles;
        options.periodOfGuessesThatFail = m_wire.periodOfGuessesThatFail;
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
