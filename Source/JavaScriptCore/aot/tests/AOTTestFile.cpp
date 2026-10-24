/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestFile.h"

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTCompileOptions.h"
#include "AOTProgramFile.h"
#include "AOTRuntimeTable.h"
#include "AOTVector.h"
#include <algorithm>
#include <expected>
#include <wtf/ASCIICType.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class DirectiveKind : uint8_t { Run, Requires, Form, Options, Diagnostic, Statistic, Loss, Library, Todo };

template<typename T>
struct NamedValue {
    ASCIILiteral name;
    T value;
};

struct DirectiveLine {
    StringView name;
    std::optional<StringView> parenthesized;
    StringView value;
    unsigned line;
};

class TestFileParser {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(TestFileParser);
public:
    TestFileParser(String&& text, Architecture architecture)
        : m_architecture(architecture)
        , m_writtenOut(writeOutRepeats(WTF::move(text)))
    {
        m_file.text = WTF::move(m_writtenOut.text);
        if (!m_writtenOut.error.isNull())
            addError(m_writtenOut.errorLine, m_writtenOut.error);
    }

    TestFile parse();

private:
    struct PendingCheckGroup {
        String prefix;
        Vector<CheckLine> lines;
        unsigned firstRunLine;
        bool requiresLines;
        bool hasMalformedLine { false };
        std::optional<Architecture> requiredArchitecture { };
        bool hasRequiredArchitecture { false };
        bool requiresUnsanitizedCode { false };
        bool requiresOtherThanWindows { false };
        bool requiresFaultingLoopPolls { false };
        bool requiresCodeOutsideImage { false };
    };

    void collectDirectiveLines();
    void parseRunDirectives();
    void parseOtherDirective(const DirectiveLine&);

    void parseRun(const DirectiveLine&);
    void parseRequires(const DirectiveLine&);
    void parseCheckLine(const DirectiveLine&);
    void parseForm(const DirectiveLine&);
    void parseOptions(const DirectiveLine&);
    void parseDiagnostic(const DirectiveLine&);
    void parseStatistic(const DirectiveLine&);
    void parseLoss(const DirectiveLine&);
    void parseTodo(const DirectiveLine&);
    bool parseLossWord(const DirectiveLine&, StringView word, ExpectedLoss&);

    PendingCheckGroup* findCheckGroup(StringView prefix);
    void validateCheckGroup(const PendingCheckGroup&);
    void finishCheckGroups();

    template<typename... Parts>
    void addError(unsigned line, const Parts&... parts)
    {
        m_file.errors.append(TestFileError { line, makeString(parts...) });
    }

    const Architecture m_architecture;
    WrittenOutText m_writtenOut;
    TestFile m_file;
    Vector<DirectiveLine> m_directives;
    Vector<PendingCheckGroup> m_checkGroups;
};

} // anonymous namespace

static constexpr NamedValue<DirectiveKind> directiveKinds[] = {
    { "RUN"_s, DirectiveKind::Run },
    { "REQUIRES"_s, DirectiveKind::Requires },
    { "FORM"_s, DirectiveKind::Form },
    { "OPTIONS"_s, DirectiveKind::Options },
    { "DIAGNOSTIC"_s, DirectiveKind::Diagnostic },
    { "STATISTIC"_s, DirectiveKind::Statistic },
    { "LOSS"_s, DirectiveKind::Loss },
    { "LIBRARY"_s, DirectiveKind::Library },
    { "TODO"_s, DirectiveKind::Todo },
};

static constexpr NamedValue<Architecture> targetArchitectures[] = {
    { "x86_64"_s, Architecture::X86_64 },
    { "arm64"_s, Architecture::ARM64 },
};

static constexpr NamedValue<CheckKind> checkSuffixes[] = {
    { "NEXT"_s, CheckKind::Next },
    { "NOT"_s, CheckKind::Not },
    { "LABEL"_s, CheckKind::Label },
};

static constexpr NamedValue<StatisticComparison> statisticComparisons[] = {
    { "="_s, StatisticComparison::Equal },
    { ">="_s, StatisticComparison::AtLeast },
    { "<="_s, StatisticComparison::AtMost },
};

static constexpr ASCIILiteral defaultPrefix = "CHECK"_s;
static constexpr ASCIILiteral countSuffix = "COUNT-"_s;
static constexpr unsigned maxCheckCount = 1000;

template<typename T, size_t size>
static std::optional<T> findNamedValue(const NamedValue<T> (&values)[size], StringView name)
{
    for (const NamedValue<T>& candidate : values) {
        if (name == candidate.name)
            return candidate.value;
    }
    return std::nullopt;
}

template<typename T>
static std::optional<T> parseDecimal(StringView text)
{
    if (!text.containsOnly<isASCIIDigit>())
        return std::nullopt;
    return parseInteger<T>(text, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

static bool isWordCharacter(char16_t character)
{
    return !isTabOrSpace(character);
}

static bool isPrefixCharacter(char16_t character)
{
    return isASCIIUpper(character) || isASCIIDigit(character);
}

static bool isNameCharacter(char16_t character)
{
    return isPrefixCharacter(character) || character == '-';
}

static bool isParenthesizedCharacter(char16_t character)
{
    return character != ')';
}

static unsigned skipCharacters(StringView text, unsigned position, bool (*shouldSkip)(char16_t))
{
    while (position < text.length() && shouldSkip(text[position]))
        ++position;
    return position;
}

static bool hasCharacterAt(StringView text, unsigned position, char16_t character)
{
    return position < text.length() && text[position] == character;
}

static Vector<StringView, 4> splitWords(StringView text)
{
    Vector<StringView, 4> words;
    unsigned start = skipCharacters(text, 0, isTabOrSpace<char16_t>);
    while (start < text.length()) {
        unsigned end = skipCharacters(text, start, isWordCharacter);
        words.append(text.substring(start, end - start));
        start = skipCharacters(text, end, isTabOrSpace<char16_t>);
    }
    return words;
}

static std::optional<DirectiveLine> splitDirective(StringView line, unsigned number)
{
    unsigned position = skipCharacters(line, 0, isTabOrSpace<char16_t>);
    if (!hasCharacterAt(line, position, '#'))
        return std::nullopt;

    unsigned nameStart = skipCharacters(line, position + 1, isTabOrSpace<char16_t>);
    if (nameStart == line.length() || !isASCIIUpper(line[nameStart]))
        return std::nullopt;
    position = skipCharacters(line, nameStart, isNameCharacter);
    StringView name = line.substring(nameStart, position - nameStart);

    std::optional<StringView> parenthesized;
    if (hasCharacterAt(line, position, '(')) {
        unsigned parenthesizedStart = position + 1;
        position = skipCharacters(line, parenthesizedStart, isParenthesizedCharacter);
        if (position == line.length())
            return std::nullopt;
        parenthesized = line.substring(parenthesizedStart, position - parenthesizedStart);
        ++position;
    }

    if (!hasCharacterAt(line, position, ':'))
        return std::nullopt;
    return DirectiveLine { name, parenthesized, line.substring(position + 1).trim(isTabOrSpace<char16_t>), number };
}

static bool canBePrefix(StringView text)
{
    if (text.isEmpty() || !isASCIIUpper(text[0]) || !text.containsOnly<isPrefixCharacter>())
        return false;
    return text != defaultPrefix && !findNamedValue(directiveKinds, text);
}

static String unknownDirectiveSentence(const DirectiveLine& directive)
{
    return makeString("unknown directive "_s, directive.name, directive.parenthesized ? "(..)"_s : ""_s);
}

static std::expected<CheckLine, String> createCheckLine(const DirectiveLine& directive, std::optional<StringView> suffix)
{
    CheckLine checkLine;
    checkLine.line = directive.line;
    checkLine.pattern = directive.value.toString();

    if (suffix) {
        if (suffix->startsWith(countSuffix)) {
            auto count = parseDecimal<unsigned>(suffix->substring(countSuffix.length()));
            if (!count || !*count || *count > maxCheckCount)
                return makeUnexpected(makeString("the count must be 1 to "_s, maxCheckCount));
            checkLine.kind = CheckKind::Count;
            checkLine.count = *count;
        } else if (auto kind = findNamedValue(checkSuffixes, *suffix))
            checkLine.kind = *kind;
        else
            return makeUnexpected(unknownDirectiveSentence(directive));
    }

    if (auto sentence = parseCheckPattern(directive.value, checkLine.elements))
        return makeUnexpected(WTF::move(*sentence));
    return checkLine;
}

TestFile TestFileParser::parse()
{
    if (!m_file.errors.isEmpty())
        return WTF::move(m_file);

    collectDirectiveLines();
    parseRunDirectives();
    for (const DirectiveLine& directive : m_directives)
        parseOtherDirective(directive);
    finishCheckGroups();

    std::stable_sort(m_file.errors.begin(), m_file.errors.end(), [](const TestFileError& a, const TestFileError& b) {
        return a.line < b.line;
    });
    return WTF::move(m_file);
}

void TestFileParser::collectDirectiveLines()
{
    StringView text = m_file.text;
    unsigned start = 0;
    for (unsigned number = 1; start < text.length(); ++number) {
        size_t newline = text.find('\n', start);
        bool hasNewline = newline != notFound;
        unsigned end = hasNewline ? static_cast<unsigned>(newline) : text.length();
        StringView line = text.substring(start, end - start);
        if (hasNewline && line.endsWith('\r'))
            line = line.left(line.length() - 1);
        if (auto directive = splitDirective(line, m_writtenOut.sourceLineOf(number)))
            m_directives.append(WTF::move(*directive));
        start = end + 1;
    }
}

void TestFileParser::parseRunDirectives()
{
    bool hasRunLine = false;
    for (const DirectiveLine& directive : m_directives) {
        if (findNamedValue(directiveKinds, directive.name) != DirectiveKind::Run)
            continue;
        hasRunLine = true;
        parseRun(directive);
    }
    if (!hasRunLine)
        addError(1, "the file has no RUN line"_s);
}

void TestFileParser::parseOtherDirective(const DirectiveLine& directive)
{
    auto kind = findNamedValue(directiveKinds, directive.name);
    bool takesPrefix = kind == DirectiveKind::Run || kind == DirectiveKind::Requires;
    if (directive.parenthesized && !takesPrefix) {
        addError(directive.line, unknownDirectiveSentence(directive));
        return;
    }
    if (!kind) {
        parseCheckLine(directive);
        return;
    }

    switch (*kind) {
    case DirectiveKind::Run:
        return;
    case DirectiveKind::Requires:
        parseRequires(directive);
        return;
    case DirectiveKind::Form:
        parseForm(directive);
        return;
    case DirectiveKind::Options:
        parseOptions(directive);
        return;
    case DirectiveKind::Diagnostic:
        parseDiagnostic(directive);
        return;
    case DirectiveKind::Statistic:
        parseStatistic(directive);
        return;
    case DirectiveKind::Loss:
        parseLoss(directive);
        return;
    case DirectiveKind::Library:
        if (directive.value != "own"_s)
            addError(directive.line, "LIBRARY is written '"_s, ownLibraryLine, '\'');
        return;
    case DirectiveKind::Todo:
        parseTodo(directive);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestFileParser::parseTodo(const DirectiveLine& directive)
{
    if (m_file.todo) {
        addError(directive.line, "the file has a second TODO line"_s);
        return;
    }
    size_t colon = directive.value.find(':');
    bool namesOwnerAndWhatIsLost = colon != notFound && colon && !directive.value.substring(colon + 1).trim(isTabOrSpace<char16_t>).isEmpty();
    if (!namesOwnerAndWhatIsLost) {
        addError(directive.line, "TODO is written 'TODO: <owner>: <what is lost>'"_s);
        return;
    }
    m_file.todo = TestTodo { directive.value.toString(), directive.line };
}

void TestFileParser::parseRun(const DirectiveLine& directive)
{
    StringView prefix = defaultPrefix;
    if (directive.parenthesized) {
        prefix = *directive.parenthesized;
        if (!canBePrefix(prefix)) {
            addError(directive.line, prefix, " cannot be a prefix"_s);
            return;
        }
    }
    if (!findCheckGroup(prefix)) {
        m_checkGroups.append(PendingCheckGroup {
            .prefix = prefix.toString(),
            .lines = { },
            .firstRunLine = directive.line,
            .requiresLines = directive.parenthesized.has_value(),
        });
    }

    auto words = splitWords(directive.value);
    if (words.isEmpty()) {
        addError(directive.line, "RUN needs a command"_s);
        return;
    }

    Vector<String> arguments;
    for (StringView argument : words.span().subspan(1))
        arguments.append(argument.toString());
    m_file.runs.append(RunDirective { prefix.toString(), words[0].toString(), WTF::move(arguments), directive.line });
}

// Under AddressSanitizer compiled code asks the shadow memory in front of an access: a run that expects sizes or faults finds other code there.
static constexpr ASCIILiteral unsanitizedCode = "unsanitized-code"_s;
// For Windows the code of x86-64 follows another calling convention.
static constexpr ASCIILiteral notWindows = "not-windows"_s;
// A loop poll is a load that faults where the target has that, and a comparison with a call on its slow path elsewhere.
static constexpr ASCIILiteral faultingLoopPolls = "faulting-loop-polls"_s;
// A hardened engine runs only the code of the image, whatever the options say.
static constexpr ASCIILiteral codeOutsideImage = "code-outside-image"_s;
#if OS(WINDOWS)
static constexpr bool isForWindows = true;
#else
static constexpr bool isForWindows = false;
#endif

void TestFileParser::parseRequires(const DirectiveLine& directive)
{
    StringView prefix = directive.parenthesized.value_or(defaultPrefix);
    PendingCheckGroup* group = findCheckGroup(prefix);
    if (!group) {
        addError(directive.line, "no RUN line has the prefix "_s, prefix);
        return;
    }

    auto words = splitWords(directive.value);
    if (words.isEmpty()) {
        addError(directive.line, "REQUIRES needs an architecture or "_s, unsanitizedCode);
        return;
    }
    for (StringView word : words) {
        bool& isGiven = word == unsanitizedCode ? group->requiresUnsanitizedCode : word == notWindows ? group->requiresOtherThanWindows : word == faultingLoopPolls ? group->requiresFaultingLoopPolls : word == codeOutsideImage ? group->requiresCodeOutsideImage : group->hasRequiredArchitecture;
        if (std::exchange(isGiven, true)) {
            addError(directive.line, "REQUIRES is given twice for "_s, prefix);
            return;
        }
        if (word == unsanitizedCode || word == notWindows || word == faultingLoopPolls || word == codeOutsideImage)
            continue;
        group->requiredArchitecture = findNamedValue(targetArchitectures, word);
        if (!group->requiredArchitecture) {
            addError(directive.line, "unknown architecture "_s, word);
            return;
        }
    }
}

void TestFileParser::parseCheckLine(const DirectiveLine& directive)
{
    StringView prefix = directive.name;
    std::optional<StringView> suffix;
    unsigned prefixEnd = skipCharacters(directive.name, 0, isPrefixCharacter);
    if (prefixEnd < directive.name.length()) {
        prefix = directive.name.left(prefixEnd);
        suffix = directive.name.substring(prefixEnd + 1);
    }

    PendingCheckGroup* group = findCheckGroup(prefix);
    if (!group) {
        addError(directive.line, unknownDirectiveSentence(directive));
        return;
    }

    auto checkLine = createCheckLine(directive, suffix);
    if (!checkLine) {
        group->hasMalformedLine = true;
        addError(directive.line, checkLine.error());
        return;
    }
    group->lines.append(WTF::move(*checkLine));
}

void TestFileParser::parseForm(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    if (words.isEmpty() || words.size() > 2) {
        addError(directive.line, "FORM is written <IRForm> [ optimizing ]"_s);
        return;
    }
    auto form = parseEnum<IRForm>(words[0]);
    if (!form) {
        addError(directive.line, "unknown form "_s, words[0]);
        return;
    }
    if (words.size() == 2 && (words[1] != "optimizing"_s || *form != IRForm::Prepared)) {
        addError(directive.line, "'optimizing' goes with Prepared"_s);
        return;
    }
    if (m_file.form) {
        addError(directive.line, "FORM is given twice"_s);
        return;
    }
    m_file.form = form;
    m_file.isOptimizing = words.size() == 2;
    m_file.formLine = directive.line;
}

void TestFileParser::parseOptions(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    if (words.isEmpty()) {
        addError(directive.line, "OPTIONS needs a word"_s);
        return;
    }
    for (StringView word : words)
        m_file.optionWords.append(OptionWord { word.toString(), directive.line });
}

void TestFileParser::parseDiagnostic(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    if (words.isEmpty() || words.size() > 2) {
        addError(directive.line, "DIAGNOSTIC is written <DiagnosticKind> [ <count> ]"_s);
        return;
    }

    auto kind = parseDiagnosticName(words[0]);
    if (!kind) {
        addError(directive.line, "unknown diagnostic kind '"_s, words[0], '\'');
        return;
    }

    std::optional<unsigned> count;
    if (words.size() == 2) {
        count = parseDecimal<unsigned>(words[1]);
        if (!count || !*count) {
            addError(directive.line, "the count must be above 0"_s);
            return;
        }
    }

    bool isListed = m_file.diagnostics.containsIf([&](const ExpectedDiagnostic& expected) {
        return expected.kind == *kind;
    });
    if (isListed) {
        addError(directive.line, words[0], " is listed twice"_s);
        return;
    }
    m_file.diagnostics.append(ExpectedDiagnostic { *kind, count, directive.line });
}

void TestFileParser::parseStatistic(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    std::optional<StatisticComparison> comparison;
    std::optional<uint64_t> value;
    if (words.size() == 3) {
        comparison = findNamedValue(statisticComparisons, words[1]);
        value = parseDecimal<uint64_t>(words[2]);
    }
    if (!comparison || !value) {
        addError(directive.line, "STATISTIC is written <StatisticId> = | >= | <= <n>"_s);
        return;
    }

    auto statistic = parseEnum<StatisticID>(words[0]);
    if (!statistic || *statistic == StatisticID::None) {
        addError(directive.line, "unknown statistic "_s, words[0]);
        return;
    }
    m_file.statistics.append(ExpectedStatistic { *statistic, *comparison, *value, directive.line });
}

bool TestFileParser::parseLossWord(const DirectiveLine& directive, StringView word, ExpectedLoss& loss)
{
    std::optional<uint32_t> number = word.length() > 1 ? parseDecimal<uint32_t>(word.substring(1)) : std::nullopt;
    char16_t letter = word[0];
    if (!number || (letter != 'x' && letter != 'f' && letter != 'v' && letter != 'L')) {
        addError(directive.line, "LOSS takes x<count>, f<n>, v<n> or L<n>, not '"_s, word, '\'');
        return false;
    }

    bool isGivenTwice = false;
    auto assign = [&](auto& member, auto value) {
        isGivenTwice = member.has_value();
        member = value;
    };
    switch (letter) {
    case 'x':
        assign(loss.count, *number);
        break;
    case 'f':
        assign(loss.function, FunctionID(*number));
        break;
    case 'v':
        assign(loss.value, ValueID(*number));
        break;
    default:
        assign(loss.layout, LayoutID(*number));
        break;
    }
    if (isGivenTwice)
        addError(directive.line, '\'', letter, "' is given twice"_s);
    return !isGivenTwice;
}

void TestFileParser::parseLoss(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    if (words.isEmpty()) {
        addError(directive.line, "LOSS needs a limit"_s);
        return;
    }

    auto limit = parseEnum<LimitID>(words[0]);
    if (!limit || *limit == LimitID::None) {
        addError(directive.line, "unknown limit '"_s, words[0], '\'');
        return;
    }

    ExpectedLoss loss { *limit, std::nullopt, std::nullopt, std::nullopt, std::nullopt, directive.line };
    for (StringView word : words.span().subspan(1)) {
        if (!parseLossWord(directive, word, loss))
            return;
    }
    if (loss.count == 0u && (loss.function || loss.value || loss.layout)) {
        addError(directive.line, "a limit that is not hit has no subject"_s);
        return;
    }

    bool isListed = m_file.losses.containsIf([&](const ExpectedLoss& expected) {
        return expected.limit == *limit;
    });
    if (isListed) {
        addError(directive.line, words[0], " is listed twice"_s);
        return;
    }
    m_file.losses.append(loss);
}

auto TestFileParser::findCheckGroup(StringView prefix) -> PendingCheckGroup*
{
    for (PendingCheckGroup& group : m_checkGroups) {
        if (group.prefix == prefix)
            return &group;
    }
    return nullptr;
}

void TestFileParser::validateCheckGroup(const PendingCheckGroup& group)
{
    if (group.requiresLines && group.lines.isEmpty()) {
        addError(group.firstRunLine, "no line has the prefix "_s, group.prefix);
        return;
    }
    if (auto error = validateCheckLines(group.lines.span()))
        addError(error->line, error->message);
}

void TestFileParser::finishCheckGroups()
{
    for (PendingCheckGroup& group : m_checkGroups) {
        if (!group.hasMalformedLine)
            validateCheckGroup(group);
        if (group.requiredArchitecture.value_or(m_architecture) != m_architecture || (group.requiresUnsanitizedCode && AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE) || (group.requiresOtherThanWindows && isForWindows) || (group.requiresFaultingLoopPolls && !hasFaultingLoopPolls) || (group.requiresCodeOutsideImage && runsOnlyCodeOfImage)) {
            m_file.runs.removeAllMatching([&](const RunDirective& run) {
                return run.prefix == group.prefix;
            });
        }
        m_file.checkGroups.append(CheckGroup { WTF::move(group.prefix), WTF::move(group.lines) });
    }
}

std::optional<DiagnosticName> parseDiagnosticName(StringView name)
{
    if (auto kind = parseEnum<UserDiagnosticKind>(name); kind && *kind != UserDiagnosticKind { })
        return DiagnosticName { *kind };
    if (auto kind = parseEnum<InternalErrorKind>(name); kind && *kind != InternalErrorKind { })
        return DiagnosticName { *kind };
    return std::nullopt;
}

ASCIILiteral nameOf(const DiagnosticName& name)
{
    return WTF::switchOn(name, [](auto kind) {
        return nameOf(kind);
    });
}

std::span<const CheckLine> TestFile::checkLinesFor(StringView prefix) const
{
    for (const CheckGroup& group : checkGroups) {
        if (group.prefix == prefix)
            return group.lines.span();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TestFile parseTestFile(String&& text, Architecture architecture)
{
    return TestFileParser(WTF::move(text), architecture).parse();
}

bool holds(StatisticComparison comparison, uint64_t actual, uint64_t expected)
{
    switch (comparison) {
    case StatisticComparison::Equal:
        return actual == expected;
    case StatisticComparison::AtLeast:
        return actual >= expected;
    case StatisticComparison::AtMost:
        return actual <= expected;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
