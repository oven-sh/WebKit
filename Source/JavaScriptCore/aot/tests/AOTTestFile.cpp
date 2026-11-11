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
#include "AOTVector.h"
#include <algorithm>
#include <expected>
#include <wtf/ASCIICType.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class DirectiveKind : uint8_t { Run, Requires, Options, JSCOptions, Diagnostic, Statistic, Library, Todo, HangsOnOrder, Types, AsUsersBuild };

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
    bool isWellFormed { true };
    bool hasColon { true };
};

struct GeneratedMarker {
    unsigned line;
    Vector<StringView, 4> functions;
    Vector<StringView> linesBelow;
};

class TestFileParser {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(TestFileParser);
public:
    TestFileParser(String&& text, Architecture architecture, const String& folder)
        : m_architecture(architecture)
        , m_writtenOut(writeOut(WTF::move(text), folder))
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
        bool requiresWindows { false };
        bool requiresFaultingLoopPolls { false };
        bool requiresCallingLoopPolls { false };
        bool requiresCodeOutsideImage { false };
        bool requiresTargetWithoutJSCVT { false };
        bool requiresOptionsAsWritten { false };
    };

    struct OpenSection {
        ASCIILiteral name;
        unsigned markerLine;
        unsigned firstLine;
        bool isFirstOfItsName;
    };

    void collectDirectiveLines();
    bool parseMarker(StringView line, unsigned number);
    void openSection(ASCIILiteral name, unsigned number);
    void appendToOpenSection(StringView line, unsigned number);
    void closeSection();
    void reportSectionWithoutEnd();
    void checkGeneratedMarker();
    bool isKnownName(StringView) const;
    std::optional<String> findNameWithin2Edits(StringView) const;
    void reportIfMeantAsDirective(const DirectiveLine&);
    void parseRunDirectives();
    void parseOtherDirective(const DirectiveLine&);

    void parseRun(const DirectiveLine&);
    void parseRequires(const DirectiveLine&);
    void parseCheckLine(const DirectiveLine&);
    void parseOptions(const DirectiveLine&);
    void parseJSCOptions(const DirectiveLine&);
    void parseDiagnostic(const DirectiveLine&);
    void parseStatistic(const DirectiveLine&);
    void parseTodo(const DirectiveLine&);
    void parseTypes(const DirectiveLine&);

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
    std::optional<unsigned> m_openSourceBlock;
    bool m_hasSourceBlock { false };
    std::optional<OpenSection> m_openSection;
    StringBuilder m_textOfOpenSection;
    std::optional<GeneratedMarker> m_generated;
};

} // anonymous namespace

static constexpr NamedValue<DirectiveKind> directiveKinds[] = {
    { "RUN"_s, DirectiveKind::Run },
    { "REQUIRES"_s, DirectiveKind::Requires },
    { "OPTIONS"_s, DirectiveKind::Options },
    { "JSC-OPTIONS"_s, DirectiveKind::JSCOptions },
    { "DIAGNOSTIC"_s, DirectiveKind::Diagnostic },
    { "STATISTIC"_s, DirectiveKind::Statistic },
    { "LIBRARY"_s, DirectiveKind::Library },
    { "TODO"_s, DirectiveKind::Todo },
    { "HANGS-ON-ORDER"_s, DirectiveKind::HangsOnOrder },
    { "TYPES"_s, DirectiveKind::Types },
    { "AS-USERS-BUILD"_s, DirectiveKind::AsUsersBuild },
};

static constexpr ASCIILiteral namesOfTestSections[] = { "code"_s };
static constexpr ASCIILiteral startOfSectionMarker = "# -- "_s;
static constexpr ASCIILiteral startOfSectionEndMarker = "# -- end of "_s;

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

static std::optional<ASCIILiteral> testSectionNameAfter(ASCIILiteral prefix, StringView line)
{
    if (!line.startsWith(prefix))
        return std::nullopt;
    StringView name = line.substring(prefix.length());
    for (ASCIILiteral candidate : namesOfTestSections) {
        if (name == candidate)
            return candidate;
    }
    return std::nullopt;
}

static bool isWordOfCommentCharacter(char16_t character)
{
    return isASCIIAlphanumeric(character) || character == '-';
}

static std::optional<DirectiveLine> splitDirective(StringView line, unsigned number)
{
    unsigned position = skipCharacters(line, 0, isTabOrSpace<char16_t>);
    if (!hasCharacterAt(line, position, '#'))
        return std::nullopt;

    unsigned nameStart = skipCharacters(line, position + 1, isTabOrSpace<char16_t>);
    bool isWellFormed = nameStart == position + 2 && line[position + 1] == ' ';
    position = skipCharacters(line, nameStart, isWordOfCommentCharacter);
    StringView name = line.substring(nameStart, position - nameStart);
    if (name.isEmpty() || !isASCIIAlpha(name[0]))
        return std::nullopt;
    isWellFormed &= isASCIIUpper(name[0]) && name.containsOnly<isNameCharacter>();

    std::optional<StringView> parenthesized;
    if (hasCharacterAt(line, position, '(')) {
        unsigned parenthesizedStart = position + 1;
        unsigned parenthesizedEnd = skipCharacters(line, parenthesizedStart, isParenthesizedCharacter);
        if (parenthesizedEnd < line.length()) {
            parenthesized = line.substring(parenthesizedStart, parenthesizedEnd - parenthesizedStart);
            position = parenthesizedEnd + 1;
        }
    }

    unsigned colon = skipCharacters(line, position, isTabOrSpace<char16_t>);
    bool hasColon = hasCharacterAt(line, colon, ':');
    isWellFormed &= hasColon && colon == position;
    return DirectiveLine { name, parenthesized, hasColon ? line.substring(colon + 1).trim(isTabOrSpace<char16_t>) : StringView(), number, isWellFormed, hasColon };
}

static unsigned editDistance(StringView a, StringView b)
{
    Vector<unsigned> beforeLast;
    Vector<unsigned> last;
    Vector<unsigned> current;
    for (Vector<unsigned>* row : { &beforeLast, &last, &current })
        row->fill(0, b.length() + 1);
    for (unsigned j = 0; j <= b.length(); ++j)
        last[j] = j;
    for (unsigned i = 1; i <= a.length(); ++i) {
        current[0] = i;
        for (unsigned j = 1; j <= b.length(); ++j) {
            current[j] = std::min({ last[j] + 1, current[j - 1] + 1, last[j - 1] + (a[i - 1] != b[j - 1]) });
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                current[j] = std::min(current[j], beforeLast[j - 2] + 1);
        }
        std::swap(beforeLast, last);
        std::swap(last, current);
    }
    return last[b.length()];
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
    checkGeneratedMarker();

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
        unsigned sourceLine = m_writtenOut.sourceLineOf(number);
        if (parseMarker(line, sourceLine)) {
            start = end + 1;
            continue;
        }
        auto directive = splitDirective(line, sourceLine);
        if (m_generated) {
            m_generated->linesBelow.append(line);
            if (!line.isEmpty() && (!directive || !directive->isWellFormed))
                addError(sourceLine, "only check lines follow `# -- generated:`: the update tool writes everything below it anew"_s);
        }
        if (directive)
            m_directives.append(WTF::move(*directive));
        start = end + 1;
    }
    if (m_openSourceBlock)
        addError(*m_openSourceBlock, "`# -- source:` without `# -- end of source`"_s);
    if (m_openSection)
        reportSectionWithoutEnd();
}

bool TestFileParser::parseMarker(StringView line, unsigned number)
{
    constexpr ASCIILiteral marker = "# --"_s;
    constexpr ASCIILiteral source = "# -- source: "_s;
    constexpr ASCIILiteral generated = "# -- generated: "_s;
    if (!line.startsWith(marker)) {
        if (!m_openSourceBlock && !m_openSection)
            return false;
        if (line.startsWith('#')) {
            if (m_openSection)
                appendToOpenSection(line, number);
            return true;
        }
        if (m_openSection)
            reportSectionWithoutEnd();
        else {
            addError(*m_openSourceBlock, "`# -- source:` without `# -- end of source`"_s);
            m_openSourceBlock = std::nullopt;
        }
        return false;
    }

    if (m_openSection) {
        if (line.startsWith(startOfSectionEndMarker) && line.substring(startOfSectionEndMarker.length()) == m_openSection->name) {
            closeSection();
            return true;
        }
        reportSectionWithoutEnd();
    }

    if (std::optional<ASCIILiteral> name = testSectionNameAfter(startOfSectionMarker, line)) {
        openSection(*name, number);
        return true;
    }
    if (std::optional<ASCIILiteral> name = testSectionNameAfter(startOfSectionEndMarker, line)) {
        if (m_openSourceBlock)
            addError(number, '`', line, "` inside the sources"_s);
        else
            addError(number, '`', line, "` without `"_s, startOfSectionMarker, *name, '`');
        return true;
    }

    if (line.startsWith(source) && splitWords(line.substring(source.length())).size() == 1) {
        if (m_generated)
            addError(number, "the source comes in front of `# -- generated:`"_s);
        else if (m_hasSourceBlock && !m_openSourceBlock)
            addError(number, "the file has two blocks of sources"_s);
        if (!m_openSourceBlock)
            m_openSourceBlock = number;
        m_hasSourceBlock = true;
    } else if (line == "# -- end of source"_s) {
        if (!m_openSourceBlock)
            addError(number, "`# -- end of source` without `# -- source:`"_s);
        m_openSourceBlock = std::nullopt;
    } else if (line.startsWith(generated) && !splitWords(line.substring(generated.length())).isEmpty()) {
        if (m_openSourceBlock)
            addError(number, "`# -- generated:` inside the sources"_s);
        else if (m_generated)
            addError(number, "the file has two lines `# -- generated:`"_s);
        else
            m_generated = GeneratedMarker { number, splitWords(line.substring(generated.length())), { } };
    } else
        addError(number, "a marker is `# -- source: <file>`, `# -- generated: <function> ..`, `# -- code` or `# -- end of <source | code>`"_s);
    return true;
}

void TestFileParser::openSection(ASCIILiteral name, unsigned number)
{
    if (m_openSourceBlock) {
        addError(number, '`', startOfSectionMarker, name, "` inside the sources"_s);
        return;
    }
    bool isFirstOfItsName = !m_file.sections.containsIf([&](const TestSection& section) {
        return section.name == name;
    });
    if (m_generated)
        addError(number, "a section comes in front of `# -- generated:`"_s);
    else if (!isFirstOfItsName)
        addError(number, "the file has two sections `"_s, name, '`');
    m_openSection = OpenSection { name, number, number + 1, isFirstOfItsName };
    m_textOfOpenSection.clear();
}

void TestFileParser::appendToOpenSection(StringView line, unsigned number)
{
    if (m_textOfOpenSection.isEmpty())
        m_openSection->firstLine = number;
    m_textOfOpenSection.append(line.substring(hasCharacterAt(line, 1, ' ') ? 2 : 1), '\n');
}

void TestFileParser::closeSection()
{
    if (m_openSection->isFirstOfItsName)
        m_file.sections.append(TestSection { m_openSection->name, m_openSection->firstLine, m_textOfOpenSection.toString() });
    m_openSection = std::nullopt;
}

void TestFileParser::reportSectionWithoutEnd()
{
    addError(m_openSection->markerLine, '`', startOfSectionMarker, m_openSection->name, "` without `"_s, startOfSectionEndMarker, m_openSection->name, '`');
    m_openSection = std::nullopt;
}

void TestFileParser::checkGeneratedMarker()
{
    if (!m_generated)
        return;
    for (StringView function : m_generated->functions) {
        if (function == "*"_s)
            continue;
        String byName = makeString("name=\""_s, function, '"');
        String byID = makeString("function "_s, function, ' ');
        String labelOfCode = makeString("-LABEL: code "_s, function, ':');
        auto names = [&](StringView line) {
            return line.contains(byName) || line.contains(byID);
        };
        auto isLabel = [&](StringView line) {
            return (line.contains("-LABEL: function "_s) && names(line)) || line.contains(labelOfCode);
        };
        bool isInProgram = false;
        for (StringView line : StringView(m_file.text).splitAllowingEmptyEntries('\n'))
            isInProgram |= line.startsWith("function f"_s) && names(line);
        if (!isInProgram)
            addError(m_generated->line, "`# -- generated:` names "_s, function, ", and the program has no such function"_s);
        else if (!m_generated->linesBelow.containsIf(isLabel))
            addError(m_generated->line, "`# -- generated:` names "_s, function, ", and no label below it does"_s);
    }
}

bool TestFileParser::isKnownName(StringView name) const
{
    if (findNamedValue(directiveKinds, name))
        return true;
    unsigned prefixEnd = skipCharacters(name, 0, isPrefixCharacter);
    if (!m_checkGroups.containsIf([&](const PendingCheckGroup& group) { return group.prefix == name.left(prefixEnd); }))
        return false;
    if (prefixEnd == name.length())
        return true;
    StringView suffix = name.substring(prefixEnd + 1);
    return findNamedValue(checkSuffixes, suffix) || suffix.startsWith(countSuffix);
}

std::optional<String> TestFileParser::findNameWithin2Edits(StringView name) const
{
    Vector<String> known;
    for (const auto& kind : directiveKinds)
        known.append(kind.name);
    for (const PendingCheckGroup& group : m_checkGroups) {
        known.append(group.prefix);
        for (const auto& suffix : checkSuffixes)
            known.append(makeString(group.prefix, '-', suffix.name));
    }
    for (const String& candidate : known) {
        if (candidate.length() >= 4 && editDistance(name, candidate) <= (candidate.length() == 4 ? 1u : 2u))
            return candidate;
    }
    return std::nullopt;
}

void TestFileParser::reportIfMeantAsDirective(const DirectiveLine& directive)
{
    bool isInCapitals = isASCIIUpper(directive.name[0]) && directive.name.containsOnly<isNameCharacter>();
    String inCapitals = directive.name.convertToASCIIUppercase();
    if (isInCapitals ? isKnownName(directive.name) : isKnownName(inCapitals) && (directive.hasColon || directive.name.contains('-'))) {
        addError(directive.line, "a directive is written `# "_s, inCapitals, ": ..`"_s);
        return;
    }
    if (!isInCapitals)
        return;
    if (!directive.hasColon && !directive.name.contains('-'))
        return;
    if (auto meant = findNameWithin2Edits(directive.name))
        addError(directive.line, directive.name, " is no directive: is it "_s, *meant, '?');
    else if (directive.hasColon)
        addError(directive.line, unknownDirectiveSentence(directive));
}

void TestFileParser::parseRunDirectives()
{
    bool hasRunLine = false;
    for (const DirectiveLine& directive : m_directives) {
        if (!directive.isWellFormed || findNamedValue(directiveKinds, directive.name) != DirectiveKind::Run)
            continue;
        hasRunLine = true;
        parseRun(directive);
    }
    if (!hasRunLine)
        addError(1, "the file has no RUN line"_s);
}

void TestFileParser::parseOtherDirective(const DirectiveLine& directive)
{
    if (!directive.isWellFormed) {
        reportIfMeantAsDirective(directive);
        return;
    }
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
    case DirectiveKind::Options:
        parseOptions(directive);
        return;
    case DirectiveKind::JSCOptions:
        parseJSCOptions(directive);
        return;
    case DirectiveKind::Diagnostic:
        parseDiagnostic(directive);
        return;
    case DirectiveKind::Statistic:
        parseStatistic(directive);
        return;
    case DirectiveKind::Library:
        if (directive.value != "own"_s)
            addError(directive.line, "LIBRARY is written '"_s, ownLibraryLine, '\'');
        return;
    case DirectiveKind::Todo:
        parseTodo(directive);
        return;
    case DirectiveKind::HangsOnOrder:
        if (m_file.hangsOnOrder)
            addError(directive.line, "the file has a second HANGS-ON-ORDER line"_s);
        m_file.hangsOnOrder = TestTodo { directive.value.toString(), directive.line };
        return;
    case DirectiveKind::Types:
        parseTypes(directive);
        return;
    case DirectiveKind::AsUsersBuild:
        if (m_file.asUsersBuild)
            addError(directive.line, "the file has a second AS-USERS-BUILD line"_s);
        if (directive.value.trim(isTabOrSpace<char16_t>).isEmpty())
            addError(directive.line, "AS-USERS-BUILD is written 'AS-USERS-BUILD: <why>'"_s);
        m_file.asUsersBuild = TestTodo { directive.value.toString(), directive.line };
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

void TestFileParser::parseTypes(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    std::optional<uint32_t> number;
    if (words.size() == 1 && words[0].startsWith('f'))
        number = parseDecimal<uint32_t>(words[0].substring(1));
    if (!number || *number > FunctionID::largestIndex) {
        addError(directive.line, "TYPES is written f<n>"_s);
        return;
    }
    if (m_file.functionOfTypesLine) {
        addError(directive.line, "TYPES is given twice"_s);
        return;
    }
    m_file.functionOfTypesLine = FunctionID(*number);
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

static constexpr ASCIILiteral unsanitizedCode = "unsanitized-code"_s;
static constexpr ASCIILiteral notWindows = "not-windows"_s;
static constexpr ASCIILiteral windows = "windows"_s;
static constexpr ASCIILiteral faultingLoopPolls = "faulting-loop-polls"_s;
static constexpr ASCIILiteral callingLoopPolls = "calling-loop-polls"_s;
static constexpr ASCIILiteral codeOutsideImage = "code-outside-image"_s;
static constexpr ASCIILiteral noJSCVT = "no-jscvt"_s;
static constexpr ASCIILiteral asWritten = "as-written"_s;
#if ASAN_ENABLED
static constexpr bool isBuiltWithAddressSanitizer = true;
#else
static constexpr bool isBuiltWithAddressSanitizer = false;
#endif
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
        bool& isGiven = word == unsanitizedCode ? group->requiresUnsanitizedCode : word == notWindows ? group->requiresOtherThanWindows : word == windows ? group->requiresWindows : word == faultingLoopPolls ? group->requiresFaultingLoopPolls : word == callingLoopPolls ? group->requiresCallingLoopPolls : word == codeOutsideImage ? group->requiresCodeOutsideImage : word == noJSCVT ? group->requiresTargetWithoutJSCVT : word == asWritten ? group->requiresOptionsAsWritten : group->hasRequiredArchitecture;
        if (std::exchange(isGiven, true)) {
            addError(directive.line, "REQUIRES is given twice for "_s, prefix);
            return;
        }
        if (word == unsanitizedCode || word == notWindows || word == windows || word == faultingLoopPolls || word == callingLoopPolls || word == codeOutsideImage || word == noJSCVT || word == asWritten)
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
        if (auto meant = findNameWithin2Edits(directive.name))
            addError(directive.line, directive.name, " is no directive: is it "_s, *meant, '?');
        else
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

void TestFileParser::parseJSCOptions(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    bool isWellFormed = !words.isEmpty() && std::ranges::all_of(words, [](StringView word) {
        return word.startsWith("--"_s) && word.contains('=');
    });
    if (!isWellFormed) {
        addError(directive.line, "JSC-OPTIONS is written --<name>=<value> { --<name>=<value> }"_s);
        return;
    }
    for (StringView word : words)
        m_file.jscOptions.append(OptionWord { word.toString(), directive.line });
}

void TestFileParser::parseDiagnostic(const DirectiveLine& directive)
{
    auto words = splitWords(directive.value);
    if (words.isEmpty() || words.size() > 3) {
        addError(directive.line, "DIAGNOSTIC is written <DiagnosticKind> [ f<n> ] [ <count> ]"_s);
        return;
    }

    auto kind = parseDiagnosticName(words[0]);
    if (!kind) {
        addError(directive.line, "unknown diagnostic kind '"_s, words[0], '\'');
        return;
    }

    std::optional<FunctionID> function;
    size_t placeOfCount = 1;
    if (words.size() > 1 && words[1].startsWith('f')) {
        std::optional<uint32_t> number = parseDecimal<uint32_t>(words[1].substring(1));
        if (!number || *number > FunctionID::largestIndex) {
            addError(directive.line, "the function of a DIAGNOSTIC is written f<n>"_s);
            return;
        }
        function = FunctionID(*number);
        placeOfCount = 2;
    }
    if (words.size() > placeOfCount + 1) {
        addError(directive.line, "DIAGNOSTIC is written <DiagnosticKind> [ f<n> ] [ <count> ]"_s);
        return;
    }

    std::optional<unsigned> count;
    if (words.size() > placeOfCount) {
        count = parseDecimal<unsigned>(words[placeOfCount]);
        if (!count || !*count) {
            addError(directive.line, "the count must be above 0"_s);
            return;
        }
    }

    bool isListed = m_file.diagnostics.containsIf([&](const ExpectedDiagnostic& expected) {
        return expected.kind == *kind && expected.function == function;
    });
    if (isListed) {
        addError(directive.line, words[0], " is listed twice"_s);
        return;
    }
    m_file.diagnostics.append(ExpectedDiagnostic { *kind, function, count, directive.line });
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
        if (group.requiredArchitecture.value_or(m_architecture) != m_architecture || (group.requiresUnsanitizedCode && isBuiltWithAddressSanitizer) || (group.requiresOtherThanWindows && isForWindows) || (group.requiresWindows && !isForWindows) || (group.requiresFaultingLoopPolls && !hasFaultingLoopPolls) || (group.requiresCallingLoopPolls && hasFaultingLoopPolls) || (group.requiresCodeOutsideImage && isHardenedBuild) || (group.requiresTargetWithoutJSCVT && targetHasJSCVT)) {
            m_file.runs.removeAllMatching([&](const RunDirective& run) {
                return run.prefix == group.prefix;
            });
        }
        for (RunDirective& run : m_file.runs)
            run.isLeftOutUnderFurtherOptionWords |= group.requiresOptionsAsWritten && run.prefix == group.prefix;
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

TestFile parseTestFile(String&& text, Architecture architecture, const String& folder)
{
    return TestFileParser(WTF::move(text), architecture, folder).parse();
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
