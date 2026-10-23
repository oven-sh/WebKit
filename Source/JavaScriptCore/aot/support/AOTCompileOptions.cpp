/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileOptions.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSelectionRules.h"
#include "B3Policy.h"
#include "CPUFeatureSet.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using WireOptions = Wire::CompileOptions;
using PassSet = decltype(RecordedSwitches::disabledPasses);

static constexpr unsigned bitsPerWordOfSwitches = 64;
static constexpr uint64_t fnvOffsetBasis = 0xcbf29ce484222325ull;
static constexpr uint64_t fnvPrime = 0x100000001b3ull;

static_assert(sizeof(WireOptions) == 264);
static_assert(sizeof(LimitID) == 4);
static_assert(std::is_same_v<decltype(WireOptions::sizeClassProgression), decltype(CompileOptions::sizeClassProgression)>);
static_assert(std::is_same_v<decltype(Wire::BisectionSwitches::disabledPasses), PassSet>);
static_assert(std::is_same_v<decltype(WireOptions::executableIdentity), decltype(CompileOptions::executableIdentity)>);
static_assert(std::is_same_v<decltype(WireOptions::removedFeatures), decltype(CompileOptions::removedFeatures)>);

static_assert(numberOfPasses <= bitsPerWordOfSwitches * std::tuple_size_v<PassSet>);
static_assert(numberOfEnumerators<FlowResultKind> <= bitsPerWordOfSwitches);
static_assert(numberOfEnumerators<SelectionRuleID> <= bitsPerWordOfSwitches);
static_assert(B3::numberOfSwitches <= bitsPerWordOfSwitches);

static_assert(numberOfEnumerators<Typed::Opcode> <= UINT16_MAX);
static_assert(numberOfEnumerators<TestingSwitch> <= 8 * sizeof(RecordedOptions::testing));
static_assert(numberOfEnumerators<HeatOverride> <= 256);
static_assert(numberOfEnumerators<Architecture> <= 256);
static_assert(numberOfEnumerators<ReferenceWidth> <= 256);
static_assert(numberOfEnumerators<MismatchPolicy> <= 256);
static_assert(numberOfEnumerators<Typed::IRForm> <= 8);

static_assert(CompileAction { } == CompileAction::Compile);
static_assert(ReferenceWidth { } == ReferenceWidth::Wide);
static_assert(MismatchPolicy { } == MismatchPolicy::Silent);
static_assert(HeatOverride { } == HeatOverride::None);
static_assert(LimitID { } == LimitID::None);

static constexpr bool isBitSet(uint64_t word, unsigned bit)
{
    return (word >> bit) & 1;
}

static constexpr uint64_t lowBits(unsigned count)
{
    return count < bitsPerWordOfSwitches ? (1ull << count) - 1 : ~0ull;
}

template<typename Flags>
static constexpr Flags withoutFlag(Flags set, Flags flag)
{
    return static_cast<Flags>(static_cast<uint32_t>(set) & ~static_cast<uint32_t>(flag));
}

static constexpr bool hasOnlyKnownFeatures(uint64_t features)
{
    return !(features & ~lowBits(numberOfCPUFeatures));
}

static std::optional<uint32_t> switchIDOf(Typed::FunctionID function)
{
    RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(function));
    if (!function)
        return std::nullopt;
    return function.index();
}

std::optional<DroppedGuard> RecordedOptions::droppedGuard() const
{
    if (!has(TestingSwitch::DropGuard))
        return std::nullopt;
    return DroppedGuard { static_cast<Typed::Opcode>(droppedGuardOpcode), droppedGuardOrdinal };
}

bool RecordedOptions::isInSwitchRange(std::optional<uint32_t> id) const
{
    // A program pass as a whole has no id, so only the range of every id covers it.
    if (!id)
        return !switches.firstID && !switches.idCount;
    if (*id < switches.firstID)
        return false;
    return !switches.idCount || static_cast<uint64_t>(*id) < static_cast<uint64_t>(switches.firstID) + switches.idCount;
}

bool RecordedOptions::isEnabled(FlowResultKind kind, uint32_t subject) const
{
    return !(isBitSet(switches.disabledFlowResults, static_cast<unsigned>(kind)) && isInSwitchRange(subject));
}

bool RecordedOptions::isEnabledForProgram(FlowResultKind kind) const
{
    return !(isBitSet(switches.disabledFlowResults, static_cast<unsigned>(kind)) && isInSwitchRange(std::nullopt));
}

bool RecordedOptions::isEnabledEverywhere(FlowResultKind kind) const
{
    return !isBitSet(switches.disabledFlowResults, static_cast<unsigned>(kind));
}

bool RecordedOptions::isEnabled(PassID pass, Typed::FunctionID function) const
{
    RELEASE_ASSERT(pass != PassID::None);
    std::optional<uint32_t> id = switchIDOf(function);
    unsigned bit = static_cast<unsigned>(pass);
    return !(isBitSet(switches.disabledPasses[bit / bitsPerWordOfSwitches], bit % bitsPerWordOfSwitches) && isInSwitchRange(id));
}

bool RecordedOptions::isEnabled(SelectionRuleID rule, Typed::FunctionID function) const
{
    RELEASE_ASSERT(rule != SelectionRuleID::None);
    std::optional<uint32_t> id = switchIDOf(function);
    return !(isBitSet(switches.disabledSelectionRules, static_cast<unsigned>(rule)) && isInSwitchRange(id));
}

bool RecordedOptions::isEnabled(B3::PolicySwitch policySwitch, Typed::FunctionID function) const
{
    std::optional<uint32_t> id = switchIDOf(function);
    return !(isBitSet(switches.disabledBackEndSwitches, static_cast<unsigned>(policySwitch)) && isInSwitchRange(id));
}

uint64_t RecordedOptions::hash() const
{
    uint64_t result = fnvOffsetBasis;
    for (uint8_t byte : std::bit_cast<std::array<uint8_t, sizeof(RecordedOptions)>>(*this)) {
        result ^= byte;
        result *= fnvPrime;
    }
    return result;
}

bool RecordedOptions::isCompatibleWithProcess() const
{
    if (architecture != static_cast<uint8_t>(hostArchitecture()) || form() != imageFormOfThisRuntime())
        return false;
    if (!hasOnlyKnownFeatures(targetFeatures))
        return false;
    // The operating system may report fewer features than the CPU has, and this binary already executes its build target's instructions.
    if (!CPUFeatureSet::fromBits(static_cast<uint32_t>(targetFeatures)).isSubsetOf(CPUFeatureSet::host().unionWith(CPUFeatureSet::buildTarget())))
        return false;
    return useWideReferences() || MarkedBlockCage::isEnabled();
}

LoggingOptions LoggingOptions::fromJSCOptions()
{
    LoggingOptions logging;
    logging.logsPhaseTimes = Options::logAOTPhaseTimes();
    logging.dumpsB3Graph = Options::dumpAOTB3Graph();
    logging.dumpsDisassembly = Options::dumpAOTDisassembly();
    logging.functionToDump = UTF8CString { Options::aotFunctionToDump() };
    logging.mapFilePath = UTF8CString { Options::aotMapFilePath() };

#if !ASSERT_ENABLED
    if (Options::dumpAOTIR())
        dataLogLn("dumpAOTIR: a build without assertions prints no IR. `bun build --compile --aot --aot-out=<file>` writes the program, and `testaot print <file> --until=<pass>` prints it");
#endif
    for (StringView point : StringView::fromLatin1(byteCast<char>(Options::dumpAOTIR())).split(',')) {
        if (auto form = parseEnum<Typed::IRForm>(point))
            logging.dumpedForms |= 1u << static_cast<unsigned>(*form);
        else if (auto pass = passNamed(point))
            logging.dumpedPass = pass;
        else
            dataLogLn("dumpAOTIR: unknown point ", point);
    }
    return logging;
}

static uint64_t findOptionalSelectionRules()
{
    uint64_t rules = 0;
    for (const Typed::SelectionRule& rule : Typed::selectionRules()) {
        if (rule.id != SelectionRuleID::None && rule.kind == Typed::SelectionRuleKind::Optional)
            rules |= 1ull << static_cast<unsigned>(rule.id);
    }
    return rules;
}

static PassSet findOptionalPasses()
{
    PassSet passes { };
    for (PassID pass : allMembersOf<PassID>) {
        if (!isOptionalPass(pass))
            continue;
        unsigned bit = static_cast<unsigned>(pass);
        passes[bit / bitsPerWordOfSwitches] |= 1ull << (bit % bitsPerWordOfSwitches);
    }
    return passes;
}

namespace {

class WireOptionsReader {
public:
    WireOptionsReader(const WireOptions& wire, Diagnostics& diagnostics)
        : m_wire(wire)
        , m_diagnostics(diagnostics)
    {
    }

    CompileOptions read();

private:
    void reportInvalid(size_t offset);
    template<typename Member> size_t offsetOf(const Member&) const;
    template<typename Enum> Enum checkedEnum(const Enum&);
    template<typename Flags> Flags checkedFlags(const Flags&);
    uint64_t checkedSwitches(const uint64_t&, uint64_t optionalSwitches);

    void readTarget(CompileOptions&);
    void readSwitches(RecordedSwitches&);
    void readLoweredLimit(CompileOptions&);
    void readTestingSwitches(RecordedOptions&);
    void readChecks(CompileOptions&);
    void readThreadCount(CompileOptions&);
    void readSizeClassProgression(CompileOptions&);

    const WireOptions& m_wire;
    Diagnostics& m_diagnostics;
};

void WireOptionsReader::reportInvalid(size_t offset)
{
    m_diagnostics.add(InternalError { .kind = InternalErrorKind::InvalidCompileOptions, .count = static_cast<uint32_t>(offset) });
}

template<typename Member>
size_t WireOptionsReader::offsetOf(const Member& member) const
{
    size_t offset = std::bit_cast<uintptr_t>(&member) - std::bit_cast<uintptr_t>(&m_wire);
    RELEASE_ASSERT(offset < sizeof(WireOptions));
    return offset;
}

template<typename Enum>
Enum WireOptionsReader::checkedEnum(const Enum& member)
{
    static_assert(!isFlagWord<Enum>);
    if (static_cast<uint32_t>(member) < numberOfEnumerators<Enum>)
        return member;
    reportInvalid(offsetOf(member));
    return { };
}

template<typename Flags>
Flags WireOptionsReader::checkedFlags(const Flags& member)
{
    static_assert(isFlagWord<Flags> && numberOfEnumerators<Flags> <= 32);
    if (!(static_cast<uint64_t>(member) >> numberOfEnumerators<Flags>))
        return member;
    reportInvalid(offsetOf(member));
    return { };
}

uint64_t WireOptionsReader::checkedSwitches(const uint64_t& member, uint64_t optionalSwitches)
{
    if (!(member & ~optionalSwitches))
        return member;
    reportInvalid(offsetOf(member));
    return 0;
}

void WireOptionsReader::readTarget(CompileOptions& options)
{
    const Wire::TargetOptions& target = m_wire.target;

    Architecture architecture = checkedEnum(target.architecture);
    if (architecture != hostArchitecture())
        reportInvalid(offsetOf(target.architecture));

    uint64_t features = target.features;
    if (!hasOnlyKnownFeatures(features)) {
        reportInvalid(offsetOf(target.features));
        features = 0;
    }

    options.recorded.architecture = static_cast<uint8_t>(architecture);
    options.recorded.targetFeatures = features;
    options.recorded.referenceWidth = static_cast<uint8_t>(checkedEnum(target.referenceWidth));
    options.recorded.imageForm = static_cast<uint8_t>(checkedEnum(target.imageForm));
    if (target.reserved)
        reportInvalid(offsetOf(target.reserved));
    options.limits = LimitValues(architecture);
}

void WireOptionsReader::readSwitches(RecordedSwitches& switches)
{
    const Wire::BisectionSwitches& bisection = m_wire.bisection;

    switches.disabledFlowResults = checkedSwitches(bisection.disabledFlowResults, lowBits(numberOfEnumerators<FlowResultKind>));
    switches.disabledSelectionRules = checkedSwitches(bisection.disabledSelectionRules, findOptionalSelectionRules());
    switches.disabledBackEndSwitches = checkedSwitches(bisection.disabledBackEndSwitches, lowBits(B3::numberOfSwitches));

    PassSet optionalPasses = findOptionalPasses();
    bool hasInvalidPass = false;
    for (size_t i = 0; i < optionalPasses.size(); ++i)
        hasInvalidPass |= !!(bisection.disabledPasses[i] & ~optionalPasses[i]);
    if (hasInvalidPass)
        reportInvalid(offsetOf(bisection.disabledPasses));
    else
        switches.disabledPasses = bisection.disabledPasses;

    switches.firstID = bisection.switchRange.firstID;
    switches.idCount = bisection.switchRange.count;
}

void WireOptionsReader::readLoweredLimit(CompileOptions& options)
{
    LimitID limit = checkedEnum(m_wire.loweredLimit);
    uint64_t value = m_wire.loweredLimitValue;
    if (limit == LimitID::None) {
        if (value)
            reportInvalid(offsetOf(m_wire.loweredLimit));
        return;
    }
    if (!canBeLowered(limit) || value > options.limits[limit]) {
        reportInvalid(offsetOf(m_wire.loweredLimit));
        return;
    }
    options.limits.lower(limit, value);
    options.recorded.loweredLimit = limit;
    options.recorded.loweredLimitValue = value;
}

void WireOptionsReader::readTestingSwitches(RecordedOptions& recorded)
{
    const Wire::DroppedGuardWords& droppedGuard = m_wire.bisection.droppedGuard;

    TestingSwitch testing = checkedFlags(m_wire.testing);
    if (hasFlag(testing, TestingSwitch::DropGuard)) {
        if (droppedGuard.opcode < numberOfEnumerators<Typed::Opcode>) {
            recorded.droppedGuardOpcode = static_cast<uint16_t>(droppedGuard.opcode);
            recorded.droppedGuardOrdinal = droppedGuard.ordinal;
        } else {
            reportInvalid(offsetOf(droppedGuard.opcode));
            testing = withoutFlag(testing, TestingSwitch::DropGuard);
        }
    }
    if (hasFlag(testing, TestingSwitch::FailPredictions)) {
        const Wire::PredictionFailureWords& failure = m_wire.bisection.predictionFailure;
        if (failure.period && failure.period <= UINT16_MAX && failure.seed <= UINT16_MAX) {
            recorded.predictionFailurePeriod = static_cast<uint16_t>(failure.period);
            recorded.predictionFailureSeed = static_cast<uint16_t>(failure.seed);
        } else {
            reportInvalid(offsetOf(failure.period));
            testing = withoutFlag(testing, TestingSwitch::FailPredictions);
        }
    }
    recorded.testing = static_cast<uint16_t>(testing);
}

void WireOptionsReader::readChecks(CompileOptions& options)
{
    CheckSet checks = checkedFlags(m_wire.checks);
    options.verifiesAfterEveryPass = hasFlag(checks, CheckSet::VerifyEachPass);
    options.verifiesAfterEveryStage = options.verifiesAfterEveryPass || hasFlag(checks, CheckSet::VerifyEachStage);
    options.recorded.validates = hasFlag(checks, CheckSet::Validate);
    options.verifiesProgram = options.verifiesAfterEveryStage || options.recorded.validates || Options::verifyAOTProgram();
    options.validatesBackEnd = hasFlag(checks, CheckSet::ValidateBackEnd);
    options.checksFlowFixpoint = hasFlag(checks, CheckSet::FlowFixpoint);
    options.checksThatFunctionsGetPastEntry = hasFlag(checks, CheckSet::FunctionsGetPastEntry);
    options.checksThatUsedValuesAreListed = ASSERT_ENABLED || hasFlag(checks, CheckSet::UsedValuesAreListed) || hasFlag(static_cast<TestingSwitch>(options.recorded.testing), TestingSwitch::CheckProofsAtRunTime);
    options.validatesReferenceGuards = hasFlag(checks, CheckSet::ValidateReferenceGuards);
    options.validatesReferences = options.validatesReferenceGuards || hasFlag(checks, CheckSet::ValidateReferences);
    options.countsChangesBehindSimplify = hasFlag(checks, CheckSet::CountChangesBehindSimplify);
}

void WireOptionsReader::readThreadCount(CompileOptions& options)
{
    if (!m_wire.threadCount) {
        reportInvalid(offsetOf(m_wire.threadCount));
        return;
    }
    uint64_t maxWorkers = std::max<uint64_t>(options.limits[LimitID::MaxWorkers], 1);
    options.threadCount = static_cast<unsigned>(std::min<uint64_t>(m_wire.threadCount, maxWorkers));
}

void WireOptionsReader::readSizeClassProgression(CompileOptions& options)
{
    double progression = m_wire.sizeClassProgression;
    if (!std::isfinite(progression) || progression <= 1) {
        reportInvalid(offsetOf(m_wire.sizeClassProgression));
        return;
    }
    options.sizeClassProgression = progression;
}

CompileOptions WireOptionsReader::read()
{
    if (m_wire.size != sizeof(WireOptions)) {
        reportInvalid(offsetOf(m_wire.size));
        return { };
    }

    CompileOptions options;
    RecordedOptions& recorded = options.recorded;

    readTarget(options);
    readSwitches(recorded.switches);
    readLoweredLimit(options);
    readTestingSwitches(recorded);
    readChecks(options);
    readThreadCount(options);
    readSizeClassProgression(options);

    recorded.mismatchPolicy = static_cast<uint8_t>(checkedEnum(m_wire.mismatchPolicy));
    recorded.heatOverride = static_cast<uint8_t>(checkedEnum(m_wire.heatOverride));
    recorded.sabotage = static_cast<uint8_t>(checkedEnum(m_wire.sabotage));
    Instrumentation instrumentation = checkedFlags(m_wire.instrumentation);
    recorded.countsBranches = hasFlag(instrumentation, Instrumentation::CountBranches);
    recorded.countsArrivals = hasFlag(instrumentation, Instrumentation::CountArrivals);
    recorded.layoutSeed = m_wire.layoutSeed;
    recorded.admittedCode = static_cast<uint8_t>(checkedFlags(m_wire.admittedCode));

    options.action = checkedEnum(m_wire.action);
    options.reports = checkedFlags(m_wire.reports);
    options.excludedCode = checkedFlags(m_wire.excludedCode);
    recorded.omitsFunctionSource = hasFlag(options.excludedCode, ExcludedCode::FunctionSource);
    options.output = checkedFlags(m_wire.output);
    options.executableIdentity = m_wire.executableIdentity;
    options.removedFeatures = m_wire.removedFeatures;
    options.logging = LoggingOptions::fromJSCOptions();
    return options;
}

} // anonymous namespace

CompileOptions CompileOptions::fromWire(const Wire::CompileOptions& wire, Diagnostics& diagnostics)
{
    return WireOptionsReader(wire, diagnostics).read();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
