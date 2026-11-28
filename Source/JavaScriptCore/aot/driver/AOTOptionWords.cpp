/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOptionWords.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsNet.h"
#include "AOTFactFaults.h"
#include "AOTMechanismWords.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaEnums.h"
#include "AOTStubDescriptor.h"
#include "CPUFeatureSet.h"
#include <array>
#include <wtf/ASCIICType.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

namespace {

enum class OptionWordReader : bool { FrontEnd, Engine };

struct OptionWord {
    ASCIILiteral key;
    ASCIILiteral valueSyntax;
    ASCIILiteral noun;
    std::optional<String> (*apply)(Wire::CompileOptions&, const OptionWord&, StringView value);
    void (*listValues)(PrintStream&);

    OptionWordReader reader() const { return apply ? OptionWordReader::Engine : OptionWordReader::FrontEnd; }
};

struct SplitOptionText {
    StringView head;
    std::optional<StringView> tail;
};

struct NamedCPU {
    ASCIILiteral name;
    CPUFeatureSet features;
};

} // anonymous namespace

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

static SplitOptionText splitOptionText(StringView text, char separator)
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

template<typename F>
static void addFlag(F& set, F flag)
{
    static_assert(isFlagWord<F>);
    using Word = std::underlying_type_t<F>;
    set = static_cast<F>(static_cast<Word>(set) | static_cast<Word>(flag));
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

template<typename... Parts>
static void listValue(PrintStream& out, const Parts&... parts)
{
    out.println("  "_s, parts...);
}

static bool engineHonorsTestingSwitch(Wire::TestingSwitch testingSwitch)
{
    switch (testingSwitch) {
    // Wrong rows on purpose, for the red control of the test of what native code answers: tests/answers/native-result-outside-its-row.ir. The reader of programs takes it.
    case Wire::TestingSwitch::AssumeNullishHostResults:
    case Wire::TestingSwitch::CheckProofsAtRunTime:
    case Wire::TestingSwitch::CheckProofsAtEntries:
    case Wire::TestingSwitch::CheckProofsAtEntriesAndFields:
    // A false proof on purpose, for the red controls of that: tests/answers/false-proof-*.ir. Link takes it: CallInLinkCalls::callUnseen().
    case Wire::TestingSwitch::AssumeUnknownCallsReturnUndefined:
    case Wire::TestingSwitch::ForceStraightCompilation:
    case Wire::TestingSwitch::DelayFlowRules:
    case Wire::TestingSwitch::DelayOtherFlowRules:
    case Wire::TestingSwitch::DisableFrameCells:
    case Wire::TestingSwitch::CheckBoundsAtRunTime:
    case Wire::TestingSwitch::CheckBoundsInEmittersOnly:
    case Wire::TestingSwitch::SkipUnrollEntryChecks:
    case Wire::TestingSwitch::SkipUnrollTripCountCheck:
    case Wire::TestingSwitch::SkipUnrolledShapeChecks:
    case Wire::TestingSwitch::CheckHoistedCallsAtRunTime:
    case Wire::TestingSwitch::AssumeInlineBuiltinsPure:
    case Wire::TestingSwitch::AssumeNoNaNStores:
    case Wire::TestingSwitch::UseGenericOwnKeyReads:
    case Wire::TestingSwitch::MisidentifyHoistedReadOperands:
    // A false bound on purpose, for the red control of that: isKnownToLieInsideList().
    case Wire::TestingSwitch::AssumeIndexesInBounds:
    case Wire::TestingSwitch::DropLengthCheckedReads:
    case Wire::TestingSwitch::AssumeInvariantLoopBounds:
    case Wire::TestingSwitch::AssumeInvariantArrayStorage:
    case Wire::TestingSwitch::MisplaceUncheckedAccesses:
    case Wire::TestingSwitch::MisplaceUncheckedReads:
    case Wire::TestingSwitch::AssumeGuardsAndFacts:
    case Wire::TestingSwitch::IgnoreAlternativeOverloads:
    case Wire::TestingSwitch::IgnoreFreshArrayEscapes:
    case Wire::TestingSwitch::SkipFencesAfterLateStores:
    case Wire::TestingSwitch::AssumeUInt32Dividends:
    case Wire::TestingSwitch::MisroundLoopBounds:
    case Wire::TestingSwitch::TreatEqualIntegersAsLess:
    case Wire::TestingSwitch::IgnoreMarksInShapeChecks:
    case Wire::TestingSwitch::SkipInt32Reboxing:
    case Wire::TestingSwitch::MisfoldBranchInSweep:
    case Wire::TestingSwitch::DropEdgeInSweep:
    case Wire::TestingSwitch::AssumeCalleesRetainNothing:
    case Wire::TestingSwitch::AssumeArgumentsNeverEscape:
    case Wire::TestingSwitch::CheckEffectsAtRunTime:
    case Wire::TestingSwitch::CheckWalkAtRunTime:
    case Wire::TestingSwitch::CheckResultFreshnessAtRunTime:
    case Wire::TestingSwitch::UnderstateEffects:
    case Wire::TestingSwitch::DisableLaterUnitSpeculation:
    case Wire::TestingSwitch::IgnoreBranchHints:
    case Wire::TestingSwitch::ShuffleBlockFrequencies:
    case Wire::TestingSwitch::IgnoreExpectedTypes:
    case Wire::TestingSwitch::ShuffleExpectedTypes:
    case Wire::TestingSwitch::FailSpeculations:
    case Wire::TestingSwitch::WidenFlowNodes:
    case Wire::TestingSwitch::ShuffleChoices:
    case Wire::TestingSwitch::ForceFallbackBlockOrder:
    case Wire::TestingSwitch::DropPatchOnlySaves:
    case Wire::TestingSwitch::AssumeSpillSlotsDead:
    case Wire::TestingSwitch::ForceWalkMode:
    case Wire::TestingSwitch::AbortLoopVisits:
    case Wire::TestingSwitch::IgnoreDormantMemberUpdates:
    case Wire::TestingSwitch::TreatMemberCalleesAsUnknown:
    case Wire::TestingSwitch::DisableFamilyReadShortcut:
    case Wire::TestingSwitch::IgnoreEscapeAfterUnknownCall:
    case Wire::TestingSwitch::TreatLiteralFieldsAsDefined:
    case Wire::TestingSwitch::OverstateTypedArrayLength:
    case Wire::TestingSwitch::CheckDictionaryReadProofs:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool drawsBySeed(Wire::TestingSwitch testingSwitch)
{
    return testingSwitch == Wire::TestingSwitch::ShuffleBlockFrequencies || testingSwitch == Wire::TestingSwitch::ShuffleExpectedTypes
        || testingSwitch == Wire::TestingSwitch::WidenFlowNodes || testingSwitch == Wire::TestingSwitch::ShuffleChoices;
}

static std::optional<String> parseShuffleSeed(Wire::CompileOptions& options, StringView written)
{
    auto seed = parseNumber<uint32_t>(written);
    if (!seed)
        return notANumberSentence(written);
    if (!*seed)
        return String("a seed must be at least 1"_s);
    if (options.shuffleSeed && options.shuffleSeed != *seed)
        return String("a build takes only one seed"_s);
    options.shuffleSeed = *seed;
    return std::nullopt;
}

static std::optional<String> parseSpeculationFailurePeriod(Wire::CompileOptions& options, StringView written)
{
    auto [period, seed] = splitOptionText(written, ':');
    auto turns = parseNumber<uint32_t>(period);
    if (!turns)
        return notANumberSentence(period);
    if (!*turns)
        return String("a guess fails one time in n, and n is at least 1"_s);
    options.speculationFailurePeriod = *turns;
    return seed ? parseShuffleSeed(options, *seed) : std::nullopt;
}

static std::optional<uint32_t> parseUnderstatedEffects(StringView written)
{
    auto [kind, rest] = splitOptionText(written, '/');
    if (!rest)
        return std::nullopt;
    auto [row, part] = splitOptionText(*rest, '/');
    if (!part)
        return std::nullopt;
    Typed::UnderstatedEffects found;
    if (matchesName(*part, "RunsOtherCode"_s))
        found.part = Typed::PartOfEffects::RunsOtherCode;
    else if (matchesName(*part, "Collects"_s))
        found.part = Typed::PartOfEffects::Collects;
    else if (matchesName(*part, "Throws"_s))
        found.part = Typed::PartOfEffects::Throws;
    else if (matchesName(*part, "ThrowsByRule"_s))
        found.part = Typed::PartOfEffects::ThrowsByRule;
    else if (matchesName(*part, "RangeOfResult"_s))
        found.part = Typed::PartOfEffects::RangeOfResult;
    else if (matchesName(*part, "KindOfResult"_s))
        found.part = Typed::PartOfEffects::KindOfResult;
    else if (matchesName(*part, "ContentsOfResult"_s))
        found.part = Typed::PartOfEffects::ContentsOfResult;
    else if (matchesName(*part, "FreshnessOfResult"_s))
        found.part = Typed::PartOfEffects::FreshnessOfResult;
    else if (matchesName(*part, "TestsOfGuard"_s))
        found.part = Typed::PartOfEffects::TestsOfGuard;
    else if (matchesName(*part, "ShapesKnown"_s))
        found.part = Typed::PartOfEffects::ShapesKnown;
    else if (matchesName(*part, "ShapesOfField"_s))
        found.part = Typed::PartOfEffects::ShapesOfField;
    else if (matchesName(*part, "MiddleShapesOfField"_s))
        found.part = Typed::PartOfEffects::MiddleShapesOfField;
    else if (matchesName(*part, "WhatIsSeen"_s))
        found.part = Typed::PartOfEffects::WritesObservableState;
    else if (auto heap = findMember<Typed::AbstractHeap>(*part)) {
        found.part = Typed::PartOfEffects::Writes;
        found.heap = *heap;
    } else
        return std::nullopt;
    if (matchesName(kind, "Operation"_s)) {
        auto opcode = findMember<Typed::Opcode>(row);
        if (!opcode)
            return std::nullopt;
        found.kind = Typed::KindOfEffectsRow::Operation;
        found.row = static_cast<uint16_t>(*opcode);
    } else if (matchesName(kind, "RuntimeOperation"_s)) {
        String name = makeString("operationAOT"_s, row);
        unsigned entry = 0;
        while (entry < numberOfEntries && name != nameOf(static_cast<Entry>(entry)))
            ++entry;
        if (entry == numberOfEntries)
            return std::nullopt;
        found.kind = Typed::KindOfEffectsRow::RuntimeOperation;
        found.row = static_cast<uint16_t>(entry);
    } else if (matchesName(kind, "Stub"_s)) {
        unsigned family = 0;
        while (family < numberOfStubFamilies && row != descriptorOf(static_cast<StubFamily>(family)).name)
            ++family;
        if (family == numberOfStubFamilies)
            return std::nullopt;
        found.kind = Typed::KindOfEffectsRow::Stub;
        found.row = static_cast<uint16_t>(family);
    } else if (matchesName(kind, "Callee"_s) || matchesName(kind, "Summary"_s)) {
        if (!row.startsWith('f'))
            return std::nullopt;
        auto function = parseNumber<uint16_t>(row.substring(1));
        if (!function)
            return std::nullopt;
        found.kind = matchesName(kind, "Callee"_s) ? Typed::KindOfEffectsRow::Callee : Typed::KindOfEffectsRow::Summary;
        found.row = *function;
    } else if (matchesName(kind, "Builtin"_s)) {
        const BuiltinTable& table = BuiltinTable::singleton();
        unsigned builtin = 1;
        while (builtin < table.builtinCount() && row != table.textOf(BuiltinID(builtin)))
            ++builtin;
        if (builtin == table.builtinCount())
            return std::nullopt;
        found.kind = Typed::KindOfEffectsRow::Builtin;
        found.row = static_cast<uint16_t>(builtin);
    } else if (matchesName(kind, "RestartMark"_s) && row == "all"_s)
        found.kind = Typed::KindOfEffectsRow::RestartMark;
    else if (matchesName(kind, "RowOfLoop"_s) && row == "all"_s)
        found.kind = Typed::KindOfEffectsRow::RowOfLoop;
    else
        return std::nullopt;
    bool describesResult = found.part == Typed::PartOfEffects::RangeOfResult || found.part == Typed::PartOfEffects::KindOfResult || found.part == Typed::PartOfEffects::ContentsOfResult || found.part == Typed::PartOfEffects::FreshnessOfResult;
    bool describesOperationRule = found.part == Typed::PartOfEffects::KindOfResult && found.kind == Typed::KindOfEffectsRow::Operation;
    if (describesResult && found.kind != Typed::KindOfEffectsRow::Builtin && !describesOperationRule)
        return std::nullopt;
    if (found.part == Typed::PartOfEffects::TestsOfGuard && found.kind != Typed::KindOfEffectsRow::Callee)
        return std::nullopt;
    return found.encode();
}

static std::optional<String> parseForcedWalkMode(Wire::CompileOptions& options, StringView written)
{
    auto [way, numbers] = splitOptionText(written, ':');
    options.forcedWalkMode = way == "NothingCarried"_s ? 1 : way == "Closed"_s ? 2 : 0;
    if (!options.forcedWalkMode)
        return String("ForceWalkMode takes NothingCarried or Closed, then :<n>:<seed> for one unit in n"_s);
    auto [period, seed] = splitOptionText(numbers.value_or("1"_s), ':');
    auto units = parseNumber<uint32_t>(period);
    if (!units)
        return notANumberSentence(period);
    if (!*units)
        return String("the walk mode is forced on one unit in n, and n must be at least 1"_s);
    options.forcedWalkModePeriod = *units;
    return seed ? parseShuffleSeed(options, *seed) : std::nullopt;
}

static std::optional<String> takeHearingsThatCutWalks(Wire::CompileOptions& options, StringView written)
{
    auto [period, phase] = splitOptionText(written, ':');
    auto hearings = parseNumber<uint32_t>(period);
    if (!hearings)
        return notANumberSentence(period);
    auto first = parseNumber<uint32_t>(phase.value_or("0"_s));
    if (!first)
        return notANumberSentence(*phase);
    if (*first >= *hearings)
        return String("AbortLoopVisits takes <n>:<k>: the k-th of every n operations aborts the visit, and k must be less than n"_s);
    options.loopVisitAbortPeriod = *hearings;
    options.loopVisitAbortPhase = *first;
    return std::nullopt;
}

static std::optional<String> applyTesting(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    return forEachListedName(word.key, value, [&](StringView written) -> std::optional<String> {
        auto [name, numbers] = splitOptionText(written, ':');
        if (auto injectedFault = findMember<Wire::FaultInjection>(name)) {
            if (*injectedFault == Wire::FaultInjection::MisstateFact) {
                auto [fact, seed] = splitOptionText(numbers.value_or(""_s), ':');
                auto number = Typed::numberOfMisstatedFact(fact);
                if (!number)
                    return makeString(name, " takes <fact> or <fact>:<seed>"_s);
                options.misstatedFact = *number;
                if (auto sentence = seed ? parseShuffleSeed(options, *seed) : std::nullopt)
                    return sentence;
                numbers = std::nullopt;
            }
            if (numbers)
                return makeString(name, " takes no number"_s);
            addFlag(options.injectedFaults, *injectedFault);
            return std::nullopt;
        }
        auto testingSwitch = findMember<Wire::TestingSwitch>(name);
        if (!testingSwitch || !engineHonorsTestingSwitch(*testingSwitch))
            return unknownNameSentence(word.noun, name);
        if (*testingSwitch == Wire::TestingSwitch::UnderstateEffects) {
            std::optional<uint32_t> row = numbers ? parseUnderstatedEffects(*numbers) : std::nullopt;
            if (!row)
                return makeString(name, " takes <kind>/<row>/<part>"_s);
            options.understatedEffects = *row;
            numbers = std::nullopt;
        }
        if (drawsBySeed(*testingSwitch)) {
            if (!numbers)
                return makeString(name, " takes a seed"_s);
            if (auto sentence = parseShuffleSeed(options, *numbers))
                return sentence;
            numbers = std::nullopt;
        }
        if (*testingSwitch == Wire::TestingSwitch::FailSpeculations) {
            if (auto sentence = parseSpeculationFailurePeriod(options, numbers.value_or("1"_s)))
                return sentence;
            numbers = std::nullopt;
        }
        if (*testingSwitch == Wire::TestingSwitch::ForceWalkMode) {
            if (auto sentence = parseForcedWalkMode(options, numbers.value_or(""_s)))
                return sentence;
            numbers = std::nullopt;
        }
        if (*testingSwitch == Wire::TestingSwitch::AbortLoopVisits) {
            if (auto sentence = takeHearingsThatCutWalks(options, numbers.value_or("1"_s)))
                return sentence;
            numbers = std::nullopt;
        }
        if (numbers)
            return makeString(name, " takes no number"_s);
        addFlag(options.testing, *testingSwitch);
        return std::nullopt;
    });
}

static void listTestingSwitches(PrintStream& out)
{
    for (Wire::TestingSwitch testingSwitch : allMembersOf<Wire::TestingSwitch>) {
        if (engineHonorsTestingSwitch(testingSwitch))
            listValue(out, nameOf(testingSwitch));
    }
    for (Wire::FaultInjection injectedFault : allMembersOf<Wire::FaultInjection>)
        listValue(out, nameOf(injectedFault));
    Typed::listMisstatedFacts(out);
}

static bool engineHonorsCheck(Wire::CheckSet check)
{
    switch (check) {
    case Wire::CheckSet::Validate:
    case Wire::CheckSet::VerifyEachStage:
    case Wire::CheckSet::FlowFixpoint:
    case Wire::CheckSet::ReplayFinalLoopVisits:
    case Wire::CheckSet::RepeatFittingLoopVisits:
    case Wire::CheckSet::FlowMonotonicity:
        return true;
    case Wire::CheckSet::KeepGoing:
    case Wire::CheckSet::VerifyEachPass:
    case Wire::CheckSet::ValidateBackEnd:
    case Wire::CheckSet::ValidateReferences:
    case Wire::CheckSet::ValidateReferenceGuards:
    case Wire::CheckSet::UsedValuesAreListed:
    case Wire::CheckSet::ClaimsToBackEnd:
    case Wire::CheckSet::MovedLoads:
    case Wire::CheckSet::DecidedComparisons:
    case Wire::CheckSet::SpillSlots:
    case Wire::CheckSet::AllocatedRegisters:
    case Wire::CheckSet::ClaimsOfFlow:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<String> applyCheck(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    return forEachListedName(word.key, value, [&](StringView name) -> std::optional<String> {
        auto check = findMember<Wire::CheckSet>(name);
        if (!check || !engineHonorsCheck(*check))
            return unknownNameSentence(word.noun, name);
        addFlag(options.checks, *check);
        return std::nullopt;
    });
}

static void listChecks(PrintStream& out)
{
    for (Wire::CheckSet check : allMembersOf<Wire::CheckSet>) {
        if (engineHonorsCheck(check))
            listValue(out, nameOf(check));
    }
}

#if CPU(ARM64)
static constexpr NamedCPU namedCPUs[] = {
    { "apple-m1"_s, CPUFeatureSet().with(CPUFeature::LSE).with(CPUFeature::JSCVT).with(CPUFeature::Float16).with(CPUFeature::FRINT).with(CPUFeature::SHA3).with(CPUFeature::DotProd) },
};
#else
static constexpr std::array<NamedCPU, 0> namedCPUs { };
#endif

static std::optional<String> applyCPU(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    for (const NamedCPU& cpu : namedCPUs) {
        if (value != cpu.name)
            continue;
        options.target.features = cpu.features.unionWith(CPUFeatureSet::buildTarget()).bits();
        return std::nullopt;
    }
    return unknownNameSentence(word.noun, value);
}

static void listCPUs(PrintStream& out)
{
    for (const NamedCPU& cpu : namedCPUs)
        listValue(out, cpu.name);
}

#include "AOTSchemaOptionWords.inc"

static const OptionWord* findOptionWord(StringView key)
{
    for (const OptionWord& word : optionWords) {
        if (key == word.key)
            return &word;
    }
    return nullptr;
}

static String unknownOptionWordSentence(StringView key)
{
    StringBuilder sentence;
    sentence.append(unknownNameSentence("option"_s, key), ": the options are "_s);
    ASCIILiteral separator = ""_s;
    for (const OptionWord& word : optionWords) {
        sentence.append(separator, word.key);
        separator = ", "_s;
    }
    return sentence.toString();
}

static void printOptionWords(PrintStream& out, OptionWordReader reader)
{
    for (const OptionWord& word : optionWords) {
        if (word.reader() != reader)
            continue;
        if (word.valueSyntax.isEmpty())
            out.println(word.key);
        else
            out.println(word.key, '=', word.valueSyntax);
        if (word.listValues)
            word.listValues(out);
    }
}

std::optional<String> applyOptionWord(Wire::CompileOptions& options, StringView word)
{
    auto [key, value] = splitOptionText(word, '=');
    const OptionWord* optionWord = findOptionWord(key);
    if (!optionWord)
        return unknownOptionWordSentence(key);

    if (optionWord->valueSyntax.isEmpty()) {
        if (value)
            return makeString(key, " takes no value"_s);
    } else if (!value || value->isEmpty())
        return makeString(key, " needs a value: "_s, optionWord->key, '=', optionWord->valueSyntax);
    if (optionWord->reader() == OptionWordReader::FrontEnd)
        return makeString(key, " is an option of the front end"_s);

    Wire::CompileOptions changedOptions = options;
    if (auto sentence = optionWord->apply(changedOptions, *optionWord, value.value_or(StringView { }))) {
        RELEASE_ASSERT(!sentence->isEmpty());
        return sentence;
    }
    options = changedOptions;
    return std::nullopt;
}

void listOptionWords(PrintStream& out)
{
    printOptionWords(out, OptionWordReader::Engine);
    out.println("front end only:"_s);
    printOptionWords(out, OptionWordReader::FrontEnd);
}

static constexpr ASCIILiteral keyOfMechanismsWord = "mechanisms"_s;
static constexpr ASCIILiteral syntaxOfMechanismsWord = "[none|all|<name>{,<name>}]{+<name>|-<name>}"_s;

ASCIILiteral nameOfMechanism(Typed::Mechanism mechanism)
{
    switch (mechanism) {
    case Typed::Mechanism::TypedOperations:
        return "TypedOperations"_s;
    case Typed::Mechanism::Guesses:
        return "Guesses"_s;
    case Typed::Mechanism::Arrivals:
        return "Arrivals"_s;
    case Typed::Mechanism::DirectCalls:
        return "DirectCalls"_s;
    case Typed::Mechanism::Inlining:
        return "Inlining"_s;
    case Typed::Mechanism::UnboxedNumbers:
        return "UnboxedNumbers"_s;
    case Typed::Mechanism::EscapeAnalysis:
        return "EscapeAnalysis"_s;
    case Typed::Mechanism::LoopInvariants:
        return "LoopInvariants"_s;
    case Typed::Mechanism::Folding:
        return "Folding"_s;
    case Typed::Mechanism::TypedSlots:
        return "TypedSlots"_s;
    case Typed::Mechanism::Handlers:
        return "Handlers"_s;
    case Typed::Mechanism::BuiltinCalls:
        return "BuiltinCalls"_s;
    case Typed::Mechanism::ParameterGuards:
        return "ParameterGuards"_s;
    case Typed::Mechanism::CellsInFrames:
        return "CellsInFrames"_s;
    case Typed::Mechanism::Deferral:
        return "Deferral"_s;
    case Typed::Mechanism::PromotedEnvironments:
        return "PromotedEnvironments"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Typed::Mechanism> findMechanismOfWord(StringView written)
{
    for (Typed::Mechanism mechanism : Typed::allMechanisms) {
        if (written == nameOfMechanism(mechanism))
            return mechanism;
    }
    return std::nullopt;
}

static size_t lengthOfNameInMechanismsWord(StringView list)
{
    size_t length = 0;
    while (length < list.length() && list[length] != '+' && list[length] != '-' && list[length] != ',')
        ++length;
    return length;
}

static std::optional<String> addMechanismOfWord(Typed::Mechanisms& mechanisms, Typed::Mechanism added)
{
    for (Typed::Mechanism dependency : Typed::dependenciesOf(added)) {
        if (!mechanisms.contains(dependency))
            return makeString(nameOfMechanism(added), " leans on "_s, nameOfMechanism(dependency), ", which is withheld"_s);
    }
    mechanisms.add(added);
    return std::nullopt;
}

static std::optional<String> applyNameInMechanismsWord(Typed::Mechanisms& mechanisms, StringView& list, char16_t sign, StringView word)
{
    StringView name = list.left(lengthOfNameInMechanismsWord(list));
    list = list.substring(name.length());

    std::optional<Typed::Mechanism> mechanism = findMechanismOfWord(name);
    if (!mechanism)
        return name.isEmpty() ? makeString("empty name in "_s, word) : unknownNameSentence("mechanism"_s, name);
    if (sign == '+')
        return addMechanismOfWord(mechanisms, *mechanism);
    mechanisms.remove(*mechanism);
    mechanisms = Typed::withoutMechanismsMissingDependencies(mechanisms);
    return std::nullopt;
}

bool isMechanismsWord(StringView word)
{
    return splitOptionText(word, '=').head == keyOfMechanismsWord;
}

std::optional<String> applyMechanismsWord(Typed::Mechanisms& mechanisms, StringView word)
{
    auto [key, value] = splitOptionText(word, '=');
    if (key != keyOfMechanismsWord)
        return unknownNameSentence("option"_s, key);
    if (!value || value->isEmpty())
        return makeString(key, " needs a value: "_s, keyOfMechanismsWord, '=', syntaxOfMechanismsWord);

    Typed::Mechanisms changedMechanisms = Typed::withoutMechanismsMissingDependencies(mechanisms);
    StringView list = *value;
    StringView head = list.left(lengthOfNameInMechanismsWord(list));
    if (head == "none"_s || head == "all"_s) {
        changedMechanisms = head == "all"_s ? Typed::allMechanisms : Typed::Mechanisms { };
        list = list.substring(head.length());
    } else if (!head.isEmpty()) {
        changedMechanisms = { };
        for (;;) {
            if (auto sentence = applyNameInMechanismsWord(changedMechanisms, list, '+', word))
                return sentence;
            if (list.isEmpty() || list[0] != ',')
                break;
            list = list.substring(1);
        }
    }

    while (!list.isEmpty()) {
        char16_t sign = list[0];
        if (sign != '+' && sign != '-')
            return makeString("a + or a - is missing in front of "_s, list);
        list = list.substring(1);
        if (auto sentence = applyNameInMechanismsWord(changedMechanisms, list, sign, word))
            return sentence;
    }
    mechanisms = changedMechanisms;
    return std::nullopt;
}

void listMechanismsWord(PrintStream& out)
{
    out.println(keyOfMechanismsWord, '=', syntaxOfMechanismsWord);
    for (Typed::Mechanism mechanism : Typed::allMechanisms) {
        StringBuilder line;
        line.append(nameOfMechanism(mechanism));
        ASCIILiteral separator = ", leans on "_s;
        for (Typed::Mechanism dependency : Typed::dependenciesOf(mechanism)) {
            line.append(separator, nameOfMechanism(dependency));
            separator = " and "_s;
        }
        listValue(out, line.toString());
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
