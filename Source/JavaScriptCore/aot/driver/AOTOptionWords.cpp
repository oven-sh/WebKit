/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOptionWords.h"

#if ENABLE(AOT)

#include "AOTMechanismWords.h"
#include "AOTSchemaEnums.h"
#include "CPUFeatureSet.h"
#include <array>
#include <wtf/ASCIICType.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

namespace {

enum class OptionWordReader : bool { FrontEnd, Engine };

// A row of AOTSchemaOptionWords.inc.
struct OptionWord {
    ASCIILiteral key;
    // Empty: the word takes no value.
    ASCIILiteral valueSyntax;
    ASCIILiteral noun;
    // Null: the front end reads the word itself.
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

// WalksNothingAtCalls may be written walks-nothing-at-calls.
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
    static_assert(isFlagWord<F> && sizeof(F) == sizeof(uint32_t));
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

template<typename... Parts>
static void listValue(PrintStream& out, const Parts&... parts)
{
    out.println("  "_s, parts...);
}

// A switch that no code reads is refused like a name that nobody knows: a run that asks for it would pass without having tested anything.
static bool engineHonorsTestingSwitch(Wire::TestingSwitch testingSwitch)
{
    switch (testingSwitch) {
    // Wrong rows on purpose, for the red control of the test of what native code answers: tests/answers/native-result-outside-its-row.ir. The reader of programs takes it.
    case Wire::TestingSwitch::NativeCodeAnswersNullish:
    // Walker::checkProof().
    case Wire::TestingSwitch::CheckProofsAtRunTime:
    // A false proof on purpose, for the red controls of that: tests/answers/false-proof-*.ir. Link takes it: CallInLinkCalls::callUnseen().
    case Wire::TestingSwitch::UnseenCodeAnswersUndefined:
    // CompileWorker::compileStraight().
    case Wire::TestingSwitch::WritesBodiesStraight:
    // The worklist of Link's flow in another order: scattered, and on lanes from the first tell, however small the program | oldest first. factsRequestForPreparedProgram().
    case Wire::TestingSwitch::DelayFlowRules:
    case Wire::TestingSwitch::DelayOtherFlowRules:
        return true;
    case Wire::TestingSwitch::NobodyLooksAtNewInstances:
    case Wire::TestingSwitch::WalksNothingAtCalls:
    case Wire::TestingSwitch::IgnoreBranchHints:
    case Wire::TestingSwitch::ForceGCSlowPaths:
    case Wire::TestingSwitch::MayEncounterMasqueradesAsUndefined:
    case Wire::TestingSwitch::DropGuard:
    case Wire::TestingSwitch::IgnoreExpectedTypes:
    case Wire::TestingSwitch::DropReferenceGuards:
    case Wire::TestingSwitch::ProveEverything:
    case Wire::TestingSwitch::NoCallConvertsCells:
    case Wire::TestingSwitch::NoLayoutIsMadeSeldom:
    case Wire::TestingSwitch::FoldLoadsPastCollections:
    case Wire::TestingSwitch::OverwriteDeadValuesAtCollections:
    case Wire::TestingSwitch::ShuffleExpectedTypes:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<String> applyTesting(Wire::CompileOptions& options, const OptionWord& word, StringView value)
{
    return forEachListedName(word.key, value, [&](StringView written) -> std::optional<String> {
        auto [name, numbers] = splitOptionText(written, ':');
        auto testingSwitch = findMember<Wire::TestingSwitch>(name);
        if (!testingSwitch || !engineHonorsTestingSwitch(*testingSwitch))
            return unknownNameSentence(word.noun, name);
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
}

// All but Validate are read in debug and ASAN builds alone. Every build takes them: testaot puts two of them in front of the options of each test.
static bool engineHonorsCheck(Wire::CheckSet check)
{
    switch (check) {
    case Wire::CheckSet::Validate:
    case Wire::CheckSet::ValidateReferences:
    case Wire::CheckSet::VerifyEachStage:
    case Wire::CheckSet::ValidateReferenceGuards:
        return true;
    case Wire::CheckSet::KeepGoing:
    case Wire::CheckSet::VerifyEachPass:
    case Wire::CheckSet::ValidateBackEnd:
    case Wire::CheckSet::FlowFixpoint:
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

// An image is compiled for the CPU that the engine itself is compiled for. On ARM64 that is the oldest one on Linux and the M1 on macOS, which has instructions
// that change what is emitted for the commonest operations. With this word a Linux machine that has them compiles and runs what a Mac does.
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
        // Bun tells a refusal from a word that was applied by the length of the sentence.
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
    case Typed::Mechanism::Deferral:
        return "Deferral"_s;
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

// The list begins with the name and loses it.
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
