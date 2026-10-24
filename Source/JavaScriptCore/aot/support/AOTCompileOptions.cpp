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
#include "AOTPassID.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaEnums.h"
#include "AOTSelectionRules.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "B3Policy.h"
#include "CPUFeatureSet.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <wtf/ASCIICType.h>
#include <wtf/BitSet.h>
#include <wtf/DataLog.h>
#include <wtf/NumberOfCores.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
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

LimitValues::LimitValues(Architecture architecture)
{
    for (LimitID limit : allMembersOf<LimitID>)
        m_values[static_cast<size_t>(limit)] = defaultLimitValue(limit, architecture);
}

void LimitValues::lower(LimitID lowered, uint64_t value)
{
    RELEASE_ASSERT(canBeLowered(lowered));
    RELEASE_ASSERT(value <= (*this)[lowered]);
    for (LimitID limit : allMembersOf<LimitID>)
        m_values[static_cast<size_t>(limit)] = valueWhenLowered(limit, (*this)[limit], lowered, value);
}

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

    if (Options::dumpAOTIR())
        dataLogLn("dumpAOTIR: only testaot prints IR. `bun build --compile --aot --aot-out=<file>` writes the program, and `testaot print <file> --until=<pass>` prints it");
    for (StringView point : StringView::fromLatin1(byteCast<char>(Options::dumpAOTIR())).split(',')) {
        if (auto pass = passNamed(point))
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

    CompileOptions read()
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

private:
    void reportInvalid(size_t offset)
    {
        m_diagnostics.add(InternalError { .kind = InternalErrorKind::InvalidCompileOptions, .count = static_cast<uint32_t>(offset) });
    }

    template<typename Member>
    size_t offsetOf(const Member& member) const
    {
        size_t offset = std::bit_cast<uintptr_t>(&member) - std::bit_cast<uintptr_t>(&m_wire);
        RELEASE_ASSERT(offset < sizeof(WireOptions));
        return offset;
    }

    template<typename Enum>
    Enum checkedEnum(const Enum& member)
    {
        static_assert(!isFlagWord<Enum>);
        if (static_cast<uint32_t>(member) < numberOfEnumerators<Enum>)
            return member;
        reportInvalid(offsetOf(member));
        return { };
    }

    template<typename Flags>
    Flags checkedFlags(const Flags& member)
    {
        static_assert(isFlagWord<Flags> && numberOfEnumerators<Flags> <= 32);
        if (!(static_cast<uint64_t>(member) >> numberOfEnumerators<Flags>))
            return member;
        reportInvalid(offsetOf(member));
        return { };
    }

    uint64_t checkedSwitches(const uint64_t& member, uint64_t optionalSwitches)
    {
        if (!(member & ~optionalSwitches))
            return member;
        reportInvalid(offsetOf(member));
        return 0;
    }

    void readTarget(CompileOptions& options)
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

    void readSwitches(RecordedSwitches& switches)
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

    void readLoweredLimit(CompileOptions& options)
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

    void readTestingSwitches(RecordedOptions& recorded)
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

    void readChecks(CompileOptions& options)
    {
        CheckSet checks = checkedFlags(m_wire.checks);
        options.verifiesAfterEveryPass = hasFlag(checks, CheckSet::VerifyEachPass);
        options.verifiesAfterEveryStage = options.verifiesAfterEveryPass || hasFlag(checks, CheckSet::VerifyEachStage);
        options.recorded.validates = hasFlag(checks, CheckSet::Validate);
        options.verifiesProgram = options.verifiesAfterEveryStage || options.recorded.validates || Options::verifyAOTProgram();
        options.validatesBackEnd = hasFlag(checks, CheckSet::ValidateBackEnd);
        options.checksClaimsToBackEnd = hasFlag(checks, CheckSet::ClaimsToBackEnd);
        options.checksMovedLoads = hasFlag(checks, CheckSet::MovedLoads);
        options.checksFlowFixpoint = hasFlag(checks, CheckSet::FlowFixpoint);
        options.checksThatFunctionsGetPastEntry = hasFlag(checks, CheckSet::FunctionsGetPastEntry);
        options.checksThatUsedValuesAreListed = ASSERT_ENABLED || hasFlag(checks, CheckSet::UsedValuesAreListed) || hasFlag(static_cast<TestingSwitch>(options.recorded.testing), TestingSwitch::CheckProofsAtRunTime);
        options.validatesReferenceGuards = hasFlag(checks, CheckSet::ValidateReferenceGuards);
        options.validatesReferences = options.validatesReferenceGuards || hasFlag(checks, CheckSet::ValidateReferences);
    }

    void readThreadCount(CompileOptions& options)
    {
        if (!m_wire.threadCount) {
            reportInvalid(offsetOf(m_wire.threadCount));
            return;
        }
        uint64_t maxWorkers = std::max<uint64_t>(options.limits[LimitID::MaxWorkers], 1);
        options.threadCount = static_cast<unsigned>(std::min<uint64_t>(m_wire.threadCount, maxWorkers));
    }

    void readSizeClassProgression(CompileOptions& options)
    {
        double progression = m_wire.sizeClassProgression;
        if (!std::isfinite(progression) || progression <= 1) {
            reportInvalid(offsetOf(m_wire.sizeClassProgression));
            return;
        }
        options.sizeClassProgression = progression;
    }

    const WireOptions& m_wire;
    Diagnostics& m_diagnostics;
};

} // anonymous namespace

CompileOptions CompileOptions::fromWire(const Wire::CompileOptions& wire, Diagnostics& diagnostics)
{
    return WireOptionsReader(wire, diagnostics).read();
}

namespace {

struct OptionWord {
    ASCIILiteral key;
    ASCIILiteral valueSyntax;
    ASCIILiteral noun;
    std::optional<String> (*apply)(Wire::CompileOptions&, const OptionWord&, StringView value);
    void (*listValues)(PrintStream&);
};

struct SplitText {
    StringView head;
    std::optional<StringView> tail;
};

template<typename E>
struct NamedValue {
    ASCIILiteral name;
    E value;
};

enum class SwitchTable : uint8_t { FlowResults, Passes, SelectionRules, BackEndSwitches };

struct Switch {
    SwitchTable table;
    ASCIILiteral name;
    unsigned bit;
    bool isRequired;
};

} // anonymous namespace

static_assert(!canBeLowered(LimitID::None));

static constexpr SwitchTable switchTables[] = {
    SwitchTable::FlowResults,
    SwitchTable::Passes,
    SwitchTable::SelectionRules,
    SwitchTable::BackEndSwitches,
};

static constexpr ASCIILiteral allFlowResultsName = "flow-results"_s;

static constexpr NamedValue<Wire::ReferenceWidth> referenceWidths[] = {
    { "narrow"_s, Wire::ReferenceWidth::Narrow },
    { "wide"_s, Wire::ReferenceWidth::Wide },
};

static constexpr NamedValue<Wire::ImageForm> imageForms[] = {
    { "absolute"_s, Wire::ImageForm::Absolute },
    { "position-independent"_s, Wire::ImageForm::PositionIndependent },
};

static constexpr NamedValue<Wire::MismatchPolicy> mismatchPolicies[] = {
    { "silent"_s, Wire::MismatchPolicy::Silent },
    { "log"_s, Wire::MismatchPolicy::Report },
};

static String unknownNameSentence(ASCIILiteral noun, StringView name)
{
    return makeString("unknown "_s, noun, ' ', name);
}

static String notANumberSentence(StringView text)
{
    if (text.isEmpty())
        return "a number is missing"_s;
    return makeString(text, " is not a number"_s);
}

static SplitText splitAtFirst(StringView text, char separator)
{
    size_t position = text.find(separator);
    if (position == notFound)
        return { text, std::nullopt };
    return { text.left(position), text.substring(position + 1) };
}

template<typename T>
static std::optional<T> parseNumber(StringView text)
{
    return parseInteger<T>(text, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

static bool matchesName(StringView written, ASCIILiteral name)
{
    if (written == name)
        return true;

    size_t matchedLength = 0;
    for (char16_t character : written.codeUnits()) {
        if (character == '-')
            continue;
        if (matchedLength == name.length() || toASCIILower(character) != toASCIILower(static_cast<char16_t>(name[matchedLength])))
            return false;
        ++matchedLength;
    }
    return matchedLength == name.length();
}

template<typename E>
static std::optional<E> findMember(StringView written)
{
    for (E member : allMembersOf<E>) {
        if (matchesName(written, nameOf(member)))
            return member;
    }
    return std::nullopt;
}

template<typename T>
static T& optionAt(Wire::CompileOptions& options, T Wire::CompileOptions::* member)
{
    return options.*member;
}

template<typename T>
static T& optionAt(Wire::CompileOptions& options, T Wire::TargetOptions::* member)
{
    return options.target.*member;
}

template<typename F>
static void addFlag(F& set, F flag)
{
    set = static_cast<F>(static_cast<uint32_t>(set) | static_cast<uint32_t>(flag));
}

template<typename Functor>
static std::optional<String> forEachListedName(ASCIILiteral key, StringView value, const Functor& functor)
{
    for (StringView name : value.splitAllowingEmptyEntries(',')) {
        if (name.isEmpty())
            return makeString("empty name in "_s, key, '=', value);
        if (auto sentence = functor(name))
            return sentence;
    }
    return std::nullopt;
}

template<auto member, auto flag>
static std::optional<String> applyFlag(Wire::CompileOptions& options, const OptionWord&, StringView)
{
    addFlag(options.*member, flag);
    return std::nullopt;
}

template<auto member>
static std::optional<String> applyListedFlags(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    return forEachListedName(word.key, value, [&](StringView name) -> std::optional<String> {
        auto flag = findMember<std::remove_reference_t<decltype(options.*member)>>(name);
        if (!flag)
            return unknownNameSentence(word.noun, name);
        addFlag(options.*member, *flag);
        return std::nullopt;
    });
}

template<auto member>
static std::optional<String> applyMember(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    auto found = findMember<std::remove_reference_t<decltype(options.*member)>>(value);
    if (!found)
        return unknownNameSentence(word.noun, value);
    options.*member = *found;
    return std::nullopt;
}

template<auto& values, auto member>
static std::optional<String> applyNamedValue(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    for (auto& candidate : values) {
        if (matchesName(value, candidate.name)) {
            optionAt(options, member) = candidate.value;
            return std::nullopt;
        }
    }
    return unknownNameSentence(word.noun, value);
}

template<auto member>
static std::optional<String> applyNumber(Wire::CompileOptions& options, const OptionWord&, StringView value)
{
    auto number = parseNumber<uint32_t>(value);
    if (!number)
        return notANumberSentence(value);
    options.*member = *number;
    return std::nullopt;
}

template<typename... Parts>
static void listValue(PrintStream& out, const Parts&... parts)
{
    out.println("  "_s, parts...);
}

template<typename E>
static void listMembers(PrintStream& out)
{
    for (E member : allMembersOf<E>)
        listValue(out, nameOf(member));
}

template<auto& values>
static void listNamedValues(PrintStream& out)
{
    for (auto& value : values)
        listValue(out, value.name);
}

static ASCIILiteral prefixOf(SwitchTable table)
{
    switch (table) {
    case SwitchTable::FlowResults:
        return "flow:"_s;
    case SwitchTable::Passes:
        return "pass:"_s;
    case SwitchTable::SelectionRules:
        return "rule:"_s;
    case SwitchTable::BackEndSwitches:
        return "b3:"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachSwitch(SwitchTable table, const Functor& functor)
{
    switch (table) {
    case SwitchTable::FlowResults:
        for (Wire::FlowResultKind kind : allMembersOf<Wire::FlowResultKind>)
            functor(Switch { table, nameOf(kind), static_cast<unsigned>(kind), false });
        return;
    case SwitchTable::Passes:
        for (PassID pass : allMembersOf<PassID>) {
            if (pass == PassID::None)
                continue;
            functor(Switch { table, nameOf(pass), static_cast<unsigned>(pass), !isOptionalPass(pass) });
        }
        return;
    case SwitchTable::SelectionRules:
        for (const Typed::SelectionRule& rule : Typed::selectionRules())
            functor(Switch { table, nameOf(rule.id), static_cast<unsigned>(rule.id), rule.kind == Typed::SelectionRuleKind::Required });
        return;
    case SwitchTable::BackEndSwitches:
        for (unsigned bit = 0; bit < B3::numberOfSwitches; ++bit)
            functor(Switch { table, nameOf(static_cast<B3::PolicySwitch>(bit)), bit, false });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<Switch, 4> findSwitches(StringView written)
{
    Vector<Switch, 4> matches;
    auto appendMatches = [&](SwitchTable table, StringView name) {
        forEachSwitch(table, [&](const Switch& candidate) {
            if (matchesName(name, candidate.name))
                matches.append(candidate);
        });
    };

    for (SwitchTable table : switchTables) {
        ASCIILiteral prefix = prefixOf(table);
        if (written.startsWith(prefix)) {
            appendMatches(table, written.substring(prefix.length()));
            return matches;
        }
    }
    for (SwitchTable table : switchTables)
        appendMatches(table, written);
    return matches;
}

static void disableSwitch(Wire::CompileOptions& options, const Switch& disabled)
{
    uint64_t mask = 1ull << (disabled.bit % bitsPerWordOfSwitches);
    switch (disabled.table) {
    case SwitchTable::FlowResults:
        options.bisection.disabledFlowResults |= mask;
        return;
    case SwitchTable::Passes:
        options.bisection.disabledPasses[disabled.bit / bitsPerWordOfSwitches] |= mask;
        return;
    case SwitchTable::SelectionRules:
        options.bisection.disabledSelectionRules |= mask;
        return;
    case SwitchTable::BackEndSwitches:
        options.bisection.disabledBackEndSwitches |= mask;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<String> disableSwitchNamed(Wire::CompileOptions& options, StringView written)
{
    if (matchesName(written, allFlowResultsName)) {
        forEachSwitch(SwitchTable::FlowResults, [&](const Switch& flowResult) {
            disableSwitch(options, flowResult);
        });
        // Nothing may use what the analysis finds, so it does not run either.
        disableSwitch(options, { SwitchTable::Passes, nameOf(PassID::AnalyzeFlow), static_cast<unsigned>(PassID::AnalyzeFlow), false });
        return std::nullopt;
    }

    auto matches = findSwitches(written);
    if (matches.isEmpty())
        return unknownNameSentence("switch"_s, written);
    if (matches.size() > 1)
        return makeString(written, " is both "_s, prefixOf(matches[0].table), matches[0].name, " and "_s, prefixOf(matches[1].table), matches[1].name);
    if (matches[0].isRequired)
        return makeString(written, " is required and cannot be disabled"_s);
    disableSwitch(options, matches[0]);
    return std::nullopt;
}

template<Wire::CompileAction action>
static std::optional<String> applyAction(Wire::CompileOptions& options, const OptionWord&, StringView)
{
    options.action = action;
    return std::nullopt;
}

static std::optional<String> applyDisable(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    return forEachListedName(word.key, value, [&](StringView name) {
        return disableSwitchNamed(options, name);
    });
}

static void listSwitches(PrintStream& out)
{
    listValue(out, allFlowResultsName);
    for (SwitchTable table : switchTables) {
        forEachSwitch(table, [&](const Switch& listed) {
            if (!listed.isRequired)
                listValue(out, prefixOf(table), listed.name);
        });
    }
}

static std::optional<String> applyOnlyPasses(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    WTF::BitSet<numberOfPasses> enabledPasses;
    enabledPasses.set(static_cast<unsigned>(PassID::AnalyzeFlow));
    auto sentence = forEachListedName(word.key, value, [&](StringView name) -> std::optional<String> {
        auto pass = findMember<PassID>(name);
        if (!pass || *pass == PassID::None)
            return unknownNameSentence(word.noun, name);
        enabledPasses.set(static_cast<unsigned>(*pass));
        return std::nullopt;
    });
    if (sentence)
        return sentence;

    forEachSwitch(SwitchTable::Passes, [&](const Switch& pass) {
        if (!pass.isRequired && !enabledPasses.get(pass.bit))
            disableSwitch(options, pass);
    });
    return std::nullopt;
}

static void listOptionalPasses(PrintStream& out)
{
    forEachSwitch(SwitchTable::Passes, [&](const Switch& pass) {
        if (!pass.isRequired)
            listValue(out, pass.name);
    });
}

static std::optional<String> applyDisableIn(Wire::CompileOptions& options, const OptionWord&, StringView value)
{
    auto [firstText, countText] = splitAtFirst(value, ':');
    auto first = parseNumber<uint32_t>(firstText);
    if (!first)
        return notANumberSentence(firstText);

    uint32_t count = 0;
    if (countText) {
        auto writtenCount = parseNumber<uint32_t>(*countText);
        if (!writtenCount)
            return notANumberSentence(*countText);
        if (!*writtenCount)
            return "the count must be above 0"_s;
        count = *writtenCount;
    }

    options.bisection.switchRange = { .firstID = *first, .count = count };
    return std::nullopt;
}

static std::optional<String> applyLimit(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    auto [name, loweredValueText] = splitAtFirst(value, ':');
    if (!loweredValueText)
        return "limit takes <LimitID>:<n>"_s;
    auto limit = findMember<LimitID>(name);
    if (!limit || *limit == LimitID::None)
        return unknownNameSentence(word.noun, name);
    if (!canBeLowered(*limit))
        return makeString("the limit "_s, name, " fails the build and cannot be lowered"_s);
    auto loweredValue = parseNumber<uint64_t>(*loweredValueText);
    if (!loweredValue)
        return notANumberSentence(*loweredValueText);
    uint64_t defaultValue = defaultLimitValue(*limit, options.target.architecture);
    if (*loweredValue > defaultValue)
        return makeString(name, " can only be lowered: it is "_s, defaultValue);
    if (options.loweredLimit != LimitID::None && options.loweredLimit != *limit)
        return "only one limit can be lowered in a build"_s;

    options.loweredLimit = *limit;
    options.loweredLimitValue = *loweredValue;
    return std::nullopt;
}

static void listLimits(PrintStream& out)
{
    for (LimitID limit : allMembersOf<LimitID>) {
        if (limit == LimitID::None)
            continue;
        ASCIILiteral mark = canBeLowered(limit) ? ""_s : " (cannot be lowered)"_s;
        listValue(out, nameOf(limit), ' ', defaultLimitValue(limit, hostArchitecture()), ' ', nameOf(stageOf(limit)), ' ', nameOf(protectsOf(limit)), ' ', nameOf(degradesToOf(limit)), mark);
    }
}

static std::optional<String> applyNumbersOfFailPredictions(Wire::CompileOptions& options, std::optional<StringView> numbers)
{
    auto [periodText, seedText] = splitAtFirst(numbers.value_or("1"_s), ':');
    auto period = parseNumber<uint16_t>(periodText);
    if (!period)
        return notANumberSentence(periodText);
    if (!*period)
        return "a prediction fails one time in n, and n is at least 1"_s;
    auto seed = parseNumber<uint16_t>(seedText.value_or("0"_s));
    if (!seed)
        return notANumberSentence(*seedText);
    options.bisection.predictionFailure = { .period = *period, .seed = *seed };
    return std::nullopt;
}

static std::optional<String> applyTesting(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    return forEachListedName(word.key, value, [&](StringView written) -> std::optional<String> {
        auto [name, numbers] = splitAtFirst(written, ':');
        auto testingSwitch = findMember<Wire::TestingSwitch>(name);
        if (!testingSwitch)
            return unknownNameSentence(word.noun, name);
        if (*testingSwitch == Wire::TestingSwitch::DropGuard)
            return "use drop-guard=<Opcode>:<n>"_s;
        if (*testingSwitch == Wire::TestingSwitch::FailPredictions) {
            if (auto sentence = applyNumbersOfFailPredictions(options, numbers))
                return sentence;
        } else if (numbers)
            return makeString(name, " takes no number"_s);
        addFlag(options.testing, *testingSwitch);
        return std::nullopt;
    });
}

static void listTestingSwitches(PrintStream& out)
{
    for (Wire::TestingSwitch testingSwitch : allMembersOf<Wire::TestingSwitch>) {
        if (testingSwitch != Wire::TestingSwitch::DropGuard)
            listValue(out, nameOf(testingSwitch));
    }
}

static std::optional<String> applyDropGuard(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    auto [name, ordinalText] = splitAtFirst(value, ':');
    if (!ordinalText)
        return "drop-guard takes <Opcode>:<n>"_s;
    auto opcode = findMember<Typed::Opcode>(name);
    if (!opcode)
        return unknownNameSentence(word.noun, name);
    auto ordinal = parseNumber<uint32_t>(*ordinalText);
    if (!ordinal)
        return notANumberSentence(*ordinalText);

    addFlag(options.testing, Wire::TestingSwitch::DropGuard);
    options.bisection.droppedGuard = { .opcode = static_cast<uint32_t>(*opcode), .ordinal = *ordinal };
    return std::nullopt;
}

static std::optional<String> applyThreads(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    if (auto sentence = applyNumber<&Wire::CompileOptions::threadCount>(options, word, value))
        return sentence;
    if (!options.threadCount)
        return "the number of threads is at least 1"_s;
    return std::nullopt;
}

static constexpr OptionWord optionWords[] = {
    { "validate"_s, { }, { }, applyFlag<&WireOptions::checks, Wire::CheckSet::Validate>, nullptr },
    { "check"_s, "<name>,.."_s, "check"_s, applyListedFlags<&WireOptions::checks>, listMembers<Wire::CheckSet> },
    { "verify-only"_s, { }, { }, applyAction<Wire::CompileAction::VerifyOnly>, nullptr },
    { "check-only"_s, { }, { }, applyAction<Wire::CompileAction::CheckOnly>, nullptr },
    { "disable"_s, "<switch>,.."_s, { }, applyDisable, listSwitches },
    { "only-passes"_s, "<PassID>,.."_s, "pass"_s, applyOnlyPasses, listOptionalPasses },
    { "disable-in"_s, "<first>[:<count>]"_s, { }, applyDisableIn, nullptr },
    { "limit"_s, "<LimitID>:<n>"_s, "limit"_s, applyLimit, listLimits },
    { "heat"_s, "<HeatOverride>"_s, "heat override"_s, applyMember<&WireOptions::heatOverride>, listMembers<Wire::HeatOverride> },
    { "testing"_s, "<name>,.."_s, "testing switch"_s, applyTesting, listTestingSwitches },
    { "sabotage"_s, "<Sabotage>"_s, "sabotage"_s, applyMember<&WireOptions::sabotage>, listMembers<Wire::Sabotage> },
    { "count-branches"_s, { }, { }, applyFlag<&WireOptions::instrumentation, Wire::Instrumentation::CountBranches>, nullptr },
    { "count-arrivals"_s, { }, { }, applyFlag<&WireOptions::instrumentation, Wire::Instrumentation::CountArrivals>, nullptr },
    { "drop-guard"_s, "<Opcode>:<n>"_s, "opcode"_s, applyDropGuard, nullptr },
    { "references"_s, "narrow|wide"_s, "reference width"_s, applyNamedValue<referenceWidths, &Wire::TargetOptions::referenceWidth>, listNamedValues<referenceWidths> },
    { "image-form"_s, "absolute|position-independent"_s, "image form"_s, applyNamedValue<imageForms, &Wire::TargetOptions::imageForm>, listNamedValues<imageForms> },
    { "type-errors"_s, "silent|log"_s, "type error policy"_s, applyNamedValue<mismatchPolicies, &WireOptions::mismatchPolicy>, listNamedValues<mismatchPolicies> },
    { "layout-seed"_s, "<n>"_s, { }, applyNumber<&WireOptions::layoutSeed>, nullptr },
    { "threads"_s, "<n>"_s, { }, applyThreads, nullptr },
    { "report"_s, "<name>,.."_s, "report"_s, applyListedFlags<&WireOptions::reports>, listMembers<Wire::ReportSet> },
    { "admit"_s, "<name>,.."_s, "admitted code"_s, applyListedFlags<&WireOptions::admittedCode>, listMembers<Wire::AdmittedCode> },
    { "exclude"_s, "<name>,.."_s, "excluded code"_s, applyListedFlags<&WireOptions::excludedCode>, listMembers<Wire::ExcludedCode> },
    { "obfuscate"_s, { }, { }, applyFlag<&WireOptions::output, Wire::OutputOptions::Obfuscate>, nullptr },
    { "erase-types"_s, { }, { }, nullptr, nullptr },
    { "out"_s, "<file>"_s, { }, nullptr, nullptr },
};

static const OptionWord* findOptionWord(StringView key)
{
    for (const OptionWord& word : optionWords) {
        if (key == word.key)
            return &word;
    }
    return nullptr;
}

static void listOptionWords(PrintStream& out, bool ofEngine)
{
    for (const OptionWord& word : optionWords) {
        if (!!word.apply != ofEngine)
            continue;
        if (word.valueSyntax.isEmpty())
            out.println(word.key);
        else
            out.println(word.key, '=', word.valueSyntax);
        if (word.listValues)
            word.listValues(out);
    }
}

Wire::CompileOptions makeDefaultCompileOptions()
{
    Wire::CompileOptions options { };
    options.size = sizeof(Wire::CompileOptions);
    AOT_THRESHOLD(unsigned, mostUsefulThreads, 16);
    options.threadCount = std::clamp<unsigned>(WTF::numberOfProcessorCores(), 1, std::min<unsigned>(Limits::maxWorkers, mostUsefulThreads));
    options.target.architecture = hostArchitecture();
    options.target.features = CPUFeatureSet::buildTarget().bits();
    options.target.imageForm = imageFormOfThisRuntime();
    options.sizeClassProgression = Options::sizeClassProgressionForCompiledCode;
    return options;
}

std::optional<String> applyOptionWord(Wire::CompileOptions& options, StringView word)
{
    auto [key, value] = splitAtFirst(word, '=');
    const OptionWord* optionWord = findOptionWord(key);
    if (!optionWord)
        return unknownNameSentence("option"_s, key);

    if (optionWord->valueSyntax.isEmpty()) {
        if (value)
            return makeString(key, " takes no value"_s);
    } else if (!value || value->isEmpty())
        return makeString(key, " needs a value"_s);
    if (!optionWord->apply)
        return makeString(key, " is an option of the front end"_s);

    Wire::CompileOptions changedOptions = options;
    if (auto sentence = optionWord->apply(changedOptions, *optionWord, value.value_or(StringView { })))
        return sentence;
    options = changedOptions;
    return std::nullopt;
}

void listOptionWords(PrintStream& out)
{
    listOptionWords(out, true);
    out.println("front end only:"_s);
    listOptionWords(out, false);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
