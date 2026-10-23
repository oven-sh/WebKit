/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileOptionWords.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTLimits.h"
#include "AOTPassID.h"
#include "AOTSchemaEnums.h"
#include "AOTSelectionRules.h"
#include "AOTVector.h"
#include "B3Policy.h"
#include "CPUFeatureSet.h"
#include "Options.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/BitSet.h>
#include <wtf/NumberOfCores.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

namespace {

enum class ValueForm : uint8_t { None, Required };
enum class WordOwner : uint8_t { Engine, FrontEnd };

struct OptionWord {
    ASCIILiteral key;
    ValueForm form;
    ASCIILiteral valueSyntax;
    WordOwner owner;
    std::optional<String> (*apply)(Wire::CompileOptions&, StringView value);
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

static constexpr unsigned bitsPerSwitchWord = 64;

static_assert(numberOfEnumerators<Wire::FlowResultKind> <= bitsPerSwitchWord);
static_assert(numberOfPasses <= 8 * sizeof(Wire::BisectionSwitches::disabledPasses));
static_assert(numberOfEnumerators<Wire::SelectionRuleID> <= bitsPerSwitchWord);
static_assert(B3::numberOfSwitches <= bitsPerSwitchWord);
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
    { "throw"_s, Wire::MismatchPolicy::Throw },
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

static String frontEndOptionSentence(ASCIILiteral key)
{
    return makeString(key, " is an option of the front end"_s);
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

template<typename E, size_t size>
static std::optional<E> findNamedValue(const NamedValue<E> (&values)[size], StringView written)
{
    for (const NamedValue<E>& candidate : values) {
        if (matchesName(written, candidate.name))
            return candidate.value;
    }
    return std::nullopt;
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

template<typename F>
static std::optional<String> addListedFlags(F& set, ASCIILiteral key, ASCIILiteral noun, StringView value)
{
    return forEachListedName(key, value, [&](StringView name) -> std::optional<String> {
        auto flag = findMember<F>(name);
        if (!flag)
            return unknownNameSentence(noun, name);
        addFlag(set, *flag);
        return std::nullopt;
    });
}

static std::optional<String> setNumber(uint32_t& member, StringView value)
{
    auto number = parseNumber<uint32_t>(value);
    if (!number)
        return notANumberSentence(value);
    member = *number;
    return std::nullopt;
}

template<typename... Parts>
static void listValue(PrintStream& out, const Parts&... parts)
{
    out.println("  "_s, parts...);
}

static void listNoValues(PrintStream&) { }

template<typename E>
static void listMembers(PrintStream& out)
{
    for (E member : allMembersOf<E>)
        listValue(out, nameOf(member));
}

template<typename E, size_t size>
static void listNamedValues(PrintStream& out, const NamedValue<E> (&values)[size])
{
    for (const NamedValue<E>& value : values)
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
    uint64_t mask = 1ull << (disabled.bit % bitsPerSwitchWord);
    switch (disabled.table) {
    case SwitchTable::FlowResults:
        options.bisection.disabledFlowResults |= mask;
        return;
    case SwitchTable::Passes:
        options.bisection.disabledPasses[disabled.bit / bitsPerSwitchWord] |= mask;
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

static std::optional<String> applyValidate(Wire::CompileOptions& options, StringView)
{
    addFlag(options.checks, Wire::CheckSet::Validate);
    return std::nullopt;
}

static std::optional<String> applyCheck(Wire::CompileOptions& options, StringView value)
{
    return addListedFlags(options.checks, "check"_s, "check"_s, value);
}

static std::optional<String> applyVerifyOnly(Wire::CompileOptions& options, StringView)
{
    options.action = Wire::CompileAction::VerifyOnly;
    return std::nullopt;
}

static std::optional<String> applyCheckOnly(Wire::CompileOptions& options, StringView)
{
    options.action = Wire::CompileAction::CheckOnly;
    return std::nullopt;
}

static std::optional<String> applyDisable(Wire::CompileOptions& options, StringView value)
{
    return forEachListedName("disable"_s, value, [&](StringView name) {
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

static std::optional<String> applyOnlyPasses(Wire::CompileOptions& options, StringView value)
{
    WTF::BitSet<numberOfPasses> enabledPasses;
    enabledPasses.set(static_cast<unsigned>(PassID::AnalyzeFlow));
    auto sentence = forEachListedName("only-passes"_s, value, [&](StringView name) -> std::optional<String> {
        auto pass = findMember<PassID>(name);
        if (!pass || *pass == PassID::None)
            return unknownNameSentence("pass"_s, name);
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

static std::optional<String> applyDisableIn(Wire::CompileOptions& options, StringView value)
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

static std::optional<String> applyLimit(Wire::CompileOptions& options, StringView value)
{
    auto [name, loweredValueText] = splitAtFirst(value, ':');
    if (!loweredValueText)
        return "limit takes <LimitID>:<n>"_s;
    auto limit = findMember<LimitID>(name);
    if (!limit || *limit == LimitID::None)
        return unknownNameSentence("limit"_s, name);
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

static std::optional<String> applyHeat(Wire::CompileOptions& options, StringView value)
{
    auto heatOverride = findMember<Wire::HeatOverride>(value);
    if (!heatOverride)
        return unknownNameSentence("heat override"_s, value);
    options.heatOverride = *heatOverride;
    return std::nullopt;
}

static std::optional<String> applySabotage(Wire::CompileOptions& options, StringView value)
{
    auto sabotage = findMember<Wire::Sabotage>(value);
    if (!sabotage)
        return unknownNameSentence("sabotage"_s, value);
    options.sabotage = *sabotage;
    return std::nullopt;
}

static std::optional<String> applyTesting(Wire::CompileOptions& options, StringView value)
{
    return forEachListedName("testing"_s, value, [&](StringView name) -> std::optional<String> {
        auto testingSwitch = findMember<Wire::TestingSwitch>(name);
        if (!testingSwitch)
            return unknownNameSentence("testing switch"_s, name);
        if (*testingSwitch == Wire::TestingSwitch::DropGuard)
            return "use drop-guard=<Opcode>:<n>"_s;
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

static std::optional<String> applyCountBranches(Wire::CompileOptions& options, StringView)
{
    addFlag(options.instrumentation, Wire::Instrumentation::CountBranches);
    return std::nullopt;
}

static std::optional<String> applyCountArrivals(Wire::CompileOptions& options, StringView)
{
    addFlag(options.instrumentation, Wire::Instrumentation::CountArrivals);
    return std::nullopt;
}

static std::optional<String> applyDropGuard(Wire::CompileOptions& options, StringView value)
{
    auto [name, ordinalText] = splitAtFirst(value, ':');
    if (!ordinalText)
        return "drop-guard takes <Opcode>:<n>"_s;
    auto opcode = findMember<Typed::Opcode>(name);
    if (!opcode)
        return unknownNameSentence("opcode"_s, name);
    auto ordinal = parseNumber<uint32_t>(*ordinalText);
    if (!ordinal)
        return notANumberSentence(*ordinalText);

    addFlag(options.testing, Wire::TestingSwitch::DropGuard);
    options.bisection.droppedGuard = { .opcode = static_cast<uint32_t>(*opcode), .ordinal = *ordinal };
    return std::nullopt;
}

static std::optional<String> applyReferences(Wire::CompileOptions& options, StringView value)
{
    auto referenceWidth = findNamedValue(referenceWidths, value);
    if (!referenceWidth)
        return unknownNameSentence("reference width"_s, value);
    options.target.referenceWidth = *referenceWidth;
    return std::nullopt;
}

static void listReferenceWidths(PrintStream& out)
{
    listNamedValues(out, referenceWidths);
}

static std::optional<String> applyImageForm(Wire::CompileOptions& options, StringView value)
{
    auto form = findNamedValue(imageForms, value);
    if (!form)
        return unknownNameSentence("image form"_s, value);
    options.target.imageForm = *form;
    return std::nullopt;
}

static void listImageForms(PrintStream& out)
{
    listNamedValues(out, imageForms);
}

static std::optional<String> applyTypeErrors(Wire::CompileOptions& options, StringView value)
{
    auto mismatchPolicy = findNamedValue(mismatchPolicies, value);
    if (!mismatchPolicy)
        return unknownNameSentence("type error policy"_s, value);
    options.mismatchPolicy = *mismatchPolicy;
    return std::nullopt;
}

static void listMismatchPolicies(PrintStream& out)
{
    listNamedValues(out, mismatchPolicies);
}

static std::optional<String> applyLayoutSeed(Wire::CompileOptions& options, StringView value)
{
    return setNumber(options.layoutSeed, value);
}

static std::optional<String> applyThreads(Wire::CompileOptions& options, StringView value)
{
    if (auto sentence = setNumber(options.threadCount, value))
        return sentence;
    if (!options.threadCount)
        return "the number of threads is at least 1"_s;
    return std::nullopt;
}

static std::optional<String> applyReport(Wire::CompileOptions& options, StringView value)
{
    return addListedFlags(options.reports, "report"_s, "report"_s, value);
}

static std::optional<String> applyAdmit(Wire::CompileOptions& options, StringView value)
{
    return addListedFlags(options.admittedCode, "admit"_s, "admitted code"_s, value);
}

static std::optional<String> applyExclude(Wire::CompileOptions& options, StringView value)
{
    return addListedFlags(options.excludedCode, "exclude"_s, "excluded code"_s, value);
}

static std::optional<String> applyObfuscate(Wire::CompileOptions& options, StringView)
{
    addFlag(options.output, Wire::OutputOptions::Obfuscate);
    return std::nullopt;
}

static std::optional<String> applyEraseTypes(Wire::CompileOptions&, StringView)
{
    return frontEndOptionSentence("erase-types"_s);
}

static std::optional<String> applyOut(Wire::CompileOptions&, StringView)
{
    return frontEndOptionSentence("out"_s);
}

static constexpr OptionWord optionWords[] = {
    { "validate"_s, ValueForm::None, { }, WordOwner::Engine, applyValidate, listNoValues },
    { "check"_s, ValueForm::Required, "<name>,.."_s, WordOwner::Engine, applyCheck, listMembers<Wire::CheckSet> },
    { "verify-only"_s, ValueForm::None, { }, WordOwner::Engine, applyVerifyOnly, listNoValues },
    { "check-only"_s, ValueForm::None, { }, WordOwner::Engine, applyCheckOnly, listNoValues },
    { "disable"_s, ValueForm::Required, "<switch>,.."_s, WordOwner::Engine, applyDisable, listSwitches },
    { "only-passes"_s, ValueForm::Required, "<PassID>,.."_s, WordOwner::Engine, applyOnlyPasses, listOptionalPasses },
    { "disable-in"_s, ValueForm::Required, "<first>[:<count>]"_s, WordOwner::Engine, applyDisableIn, listNoValues },
    { "limit"_s, ValueForm::Required, "<LimitID>:<n>"_s, WordOwner::Engine, applyLimit, listLimits },
    { "heat"_s, ValueForm::Required, "<HeatOverride>"_s, WordOwner::Engine, applyHeat, listMembers<Wire::HeatOverride> },
    { "testing"_s, ValueForm::Required, "<name>,.."_s, WordOwner::Engine, applyTesting, listTestingSwitches },
    { "sabotage"_s, ValueForm::Required, "<Sabotage>"_s, WordOwner::Engine, applySabotage, listMembers<Wire::Sabotage> },
    { "count-branches"_s, ValueForm::None, { }, WordOwner::Engine, applyCountBranches, listNoValues },
    { "count-arrivals"_s, ValueForm::None, { }, WordOwner::Engine, applyCountArrivals, listNoValues },
    { "drop-guard"_s, ValueForm::Required, "<Opcode>:<n>"_s, WordOwner::Engine, applyDropGuard, listNoValues },
    { "references"_s, ValueForm::Required, "narrow|wide"_s, WordOwner::Engine, applyReferences, listReferenceWidths },
    { "image-form"_s, ValueForm::Required, "absolute|position-independent"_s, WordOwner::Engine, applyImageForm, listImageForms },
    { "type-errors"_s, ValueForm::Required, "throw|log"_s, WordOwner::Engine, applyTypeErrors, listMismatchPolicies },
    { "layout-seed"_s, ValueForm::Required, "<n>"_s, WordOwner::Engine, applyLayoutSeed, listNoValues },
    { "threads"_s, ValueForm::Required, "<n>"_s, WordOwner::Engine, applyThreads, listNoValues },
    { "report"_s, ValueForm::Required, "<name>,.."_s, WordOwner::Engine, applyReport, listMembers<Wire::ReportSet> },
    { "admit"_s, ValueForm::Required, "<name>,.."_s, WordOwner::Engine, applyAdmit, listMembers<Wire::AdmittedCode> },
    { "exclude"_s, ValueForm::Required, "<name>,.."_s, WordOwner::Engine, applyExclude, listMembers<Wire::ExcludedCode> },
    { "obfuscate"_s, ValueForm::None, { }, WordOwner::Engine, applyObfuscate, listNoValues },
    { "erase-types"_s, ValueForm::None, { }, WordOwner::FrontEnd, applyEraseTypes, listNoValues },
    { "out"_s, ValueForm::Required, "<file>"_s, WordOwner::FrontEnd, applyOut, listNoValues },
};

static constexpr bool valueSyntaxesMatchForms()
{
    for (const OptionWord& word : optionWords) {
        if ((word.form == ValueForm::None) != word.valueSyntax.isEmpty())
            return false;
    }
    return true;
}

static_assert(valueSyntaxesMatchForms());

static const OptionWord* findOptionWord(StringView key)
{
    for (const OptionWord& word : optionWords) {
        if (key == word.key)
            return &word;
    }
    return nullptr;
}

static void listOptionWords(PrintStream& out, WordOwner owner)
{
    for (const OptionWord& word : optionWords) {
        if (word.owner != owner)
            continue;
        switch (word.form) {
        case ValueForm::None:
            out.println(word.key);
            break;
        case ValueForm::Required:
            out.println(word.key, '=', word.valueSyntax);
            break;
        }
        word.listValues(out);
    }
}

Wire::CompileOptions makeDefaultCompileOptions()
{
    Wire::CompileOptions options { };
    options.size = sizeof(Wire::CompileOptions);
    constexpr unsigned mostUsefulThreads = 16;
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

    switch (optionWord->form) {
    case ValueForm::None:
        if (value)
            return makeString(key, " takes no value"_s);
        break;
    case ValueForm::Required:
        if (!value || value->isEmpty())
            return makeString(key, " needs a value"_s);
        break;
    }

    Wire::CompileOptions changedOptions = options;
    if (auto sentence = optionWord->apply(changedOptions, value.value_or(StringView { })))
        return sentence;
    options = changedOptions;
    return std::nullopt;
}

void listOptionWords(PrintStream& out)
{
    listOptionWords(out, WordOwner::Engine);
    out.println("front end only:"_s);
    listOptionWords(out, WordOwner::FrontEnd);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
