/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramFile.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRParser.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <bit>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr ASCIILiteral repeatLineStart = "# REPEAT:"_s;
constexpr ASCIILiteral repeatLineEnd = "# END REPEAT"_s;
constexpr ASCIILiteral programLineStart = "# PROGRAM-OF:"_s;
constexpr ASCIILiteral functionLineStart = "function f"_s;
constexpr size_t maxWrittenOutLines = 1 << 20;

struct RepeatedName {
    StringView name;
    Vector<StringView> words;
    int64_t first { 0 };
    size_t count { 0 };
    size_t step { 0 };
};

class RepeatWriter {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(RepeatWriter);
public:
    RepeatWriter(StringView text, const String& folder)
        : m_folder(folder)
    {
        for (StringView line : text.splitAllowingEmptyEntries('\n'))
            m_lines.append(line);
    }

    static StringView withoutCarriageReturn(StringView line)
    {
        return line.endsWith('\r') ? line.left(line.length() - 1) : line;
    }

    WrittenOutText write()
    {
        writeLines(0, m_lines.size());
        if (!m_result.error.isNull())
            return WTF::move(m_result);
        m_result.text = m_text.toString();
        return WTF::move(m_result);
    }

private:
    template<typename... Parts>
    bool fail(size_t index, const Parts&... parts)
    {
        m_result.errorLine = static_cast<unsigned>(index + 1);
        m_result.error = makeString(parts...);
        return false;
    }

    bool writeLines(size_t begin, size_t end)
    {
        for (size_t index = begin; index < end; ++index) {
            if (withoutCarriageReturn(m_lines[index]) == repeatLineEnd)
                return fail(index, "END REPEAT without a REPEAT"_s);
            if (m_lines[index].startsWith(programLineStart)) {
                if (!writeProgramOf(index))
                    return false;
                continue;
            }
            if (!m_lines[index].startsWith(repeatLineStart)) {
                if (!writeLine(index))
                    return false;
                continue;
            }
            size_t last = endOfRepeat(index, end);
            if (last == end)
                return fail(index, "REPEAT without an END REPEAT"_s);
            if (!writeRepeat(index, last))
                return false;
            index = last;
        }
        return true;
    }

    size_t endOfRepeat(size_t first, size_t end) const
    {
        unsigned depth = 0;
        for (size_t index = first; index < end; ++index) {
            if (m_lines[index].startsWith(repeatLineStart))
                ++depth;
            else if (withoutCarriageReturn(m_lines[index]) == repeatLineEnd && !--depth)
                return index;
        }
        return end;
    }

    bool writeRepeat(size_t first, size_t last)
    {
        size_t outerNames = m_names.size();
        for (StringView part : withoutCarriageReturn(m_lines[first]).substring(repeatLineStart.length()).split(';')) {
            size_t equals = part.find('=');
            if (equals == notFound)
                return fail(first, "expected <name> = <first>..<last> or <name> = <word>.."_s);
            RepeatedName name { part.left(equals).trim(isTabOrSpace<char16_t>) };
            StringView value = part.substring(equals + 1).trim(isTabOrSpace<char16_t>);
            size_t dots = value.find(".."_s);
            auto firstNumber = dots == notFound ? std::nullopt : parseInteger<int64_t>(value.left(dots));
            auto lastNumber = dots == notFound ? std::nullopt : parseInteger<int64_t>(value.substring(dots + 2));
            if (firstNumber && lastNumber && *lastNumber >= *firstNumber) {
                name.first = *firstNumber;
                name.count = static_cast<size_t>(*lastNumber - *firstNumber) + 1;
            } else {
                for (StringView word : value.split(' '))
                    name.words.append(word);
                name.count = name.words.size();
            }
            if (name.name.isEmpty() || !name.count || name.count > maxWrittenOutLines || (m_names.size() > outerNames && name.count != m_names.last().count))
                return fail(first, "the names of a REPEAT take as many values as one another, and at least one"_s);
            m_names.append(WTF::move(name));
        }
        if (m_names.size() == outerNames)
            return fail(first, "REPEAT names nothing"_s);

        size_t count = m_names.last().count;
        for (size_t step = 0; step < count; ++step) {
            for (size_t index = outerNames; index < m_names.size(); ++index)
                m_names[index].step = step;
            if (!writeLines(first + 1, last))
                return false;
        }
        m_names.shrink(outerNames);
        return true;
    }

    bool writeProgramOf(size_t index)
    {
        StringView name = withoutCarriageReturn(m_lines[index]).substring(programLineStart.length()).trim(isTabOrSpace<char16_t>);
        std::optional<Vector<uint8_t>> bytes;
        if (!m_folder.isNull() && !name.isEmpty())
            bytes = FileSystem::readEntireFile(FileSystem::pathByAppendingComponent(m_folder, name));
        if (!bytes)
            return fail(index, "cannot read the file beside this one that PROGRAM-OF names"_s);

        StringBuilder program;
        String text = String::fromUTF8(bytes->span());
        for (StringView line : StringView(text).split('\n')) {
            bool isComment = line.trim(isTabOrSpace<char16_t>).startsWith('#');
            if (!isComment || line.startsWith(repeatLineStart) || withoutCarriageReturn(line) == repeatLineEnd)
                program.append(line, '\n');
        }
        WrittenOutText writtenOut = RepeatWriter(program.toString(), { }).write();
        if (!writtenOut.error.isNull())
            return fail(index, name, ':', ' ', writtenOut.error);
        for (StringView line : StringView(writtenOut.text).split('\n')) {
            if (!startLine(index))
                return false;
            m_text.append(line);
        }
        return true;
    }

    bool startLine(size_t index)
    {
        if (m_result.sourceLines.size() == maxWrittenOutLines)
            return fail(index, "more than "_s, maxWrittenOutLines, " lines"_s);
        if (!m_result.sourceLines.isEmpty())
            m_text.append('\n');
        m_result.sourceLines.append(static_cast<unsigned>(index + 1));
        return true;
    }

    bool writeLine(size_t index)
    {
        if (!startLine(index))
            return false;

        StringView rest = m_lines[index];
        for (size_t start = rest.find("%("_s); start != notFound; start = rest.find("%("_s)) {
            size_t end = rest.find(')', start);
            if (end == notFound || !writeUse(rest.substring(start + 2, end - start - 2), rest.left(start)))
                return fail(index, "expected %(<name>), with *<factor>, +<number> or -<number>, :<digits> behind the name"_s);
            rest = rest.substring(end + 1);
        }
        m_text.append(rest);
        return true;
    }

    bool writeUse(StringView use, StringView textBefore)
    {
        auto takeNumberBehind = [&](char16_t sign) -> std::optional<int64_t> {
            size_t position = use.reverseFind(sign);
            if (position == notFound || !position || use[position - 1] == '*')
                return std::nullopt;
            auto number = parseInteger<int64_t>(use.substring(position + 1));
            if (number)
                use = use.left(position);
            return number;
        };
        std::optional<int64_t> digits = takeNumberBehind(':');
        std::optional<int64_t> added = takeNumberBehind('+');
        std::optional<int64_t> subtracted = added ? std::nullopt : takeNumberBehind('-');
        std::optional<int64_t> factor;
        if (size_t position = use.find('*'); position != notFound) {
            factor = parseInteger<int64_t>(use.substring(position + 1));
            if (!factor)
                return false;
            use = use.left(position);
        }

        size_t index = m_names.reverseFindIf([&](const RepeatedName& name) {
            return name.name == use;
        });
        if (index == notFound)
            return false;
        const RepeatedName& name = m_names[index];
        m_text.append(textBefore);
        bool isPlain = !digits && !added && !subtracted && !factor;
        if (isPlain && !name.words.isEmpty()) {
            m_text.append(name.words[name.step]);
            return true;
        }

        std::optional<int64_t> value = name.words.isEmpty() ? std::optional<int64_t>(name.first + static_cast<int64_t>(name.step)) : parseInteger<int64_t>(name.words[name.step]);
        if (!value)
            return false;
        String number = String::number(*value * factor.value_or(1) + added.value_or(0) - subtracted.value_or(0));
        for (int64_t length = number.length(); length < digits.value_or(0); ++length)
            m_text.append('0');
        m_text.append(number);
        return true;
    }

    const String m_folder;
    Vector<StringView> m_lines;
    Vector<RepeatedName> m_names;
    StringBuilder m_text;
    WrittenOutText m_result;
};

struct ImpliedLabel {
    size_t line;
    StringView prefix;
    StringView function;
};

} // anonymous namespace

static StringView labelOfFunctionLine(StringView line)
{
    size_t nameStart = line.find(" name=\""_s);
    size_t idEnd = line.find(' ', functionLineStart.length());
    size_t nameEnd = nameStart == notFound || nameStart != idEnd ? notFound : line.find('"', nameStart + 7);
    return nameEnd == notFound ? StringView() : line.left(nameEnd + 1);
}

static StringView checkPrefixOfComment(StringView line)
{
    if (!line.startsWith("# "_s))
        return { };
    unsigned end = 2;
    while (end < line.length() && (isASCIIUpper(line[end]) || (end > 2 && isASCIIDigit(line[end]))))
        ++end;
    bool isDirective = end > 2 && end < line.length() && (line[end] == ':' || line[end] == '-');
    return isDirective ? line.substring(2, end - 2) : StringView();
}

static void writeOutLabels(WrittenOutText& writtenOut)
{
    Vector<StringView> lines;
    for (StringView line : StringView(writtenOut.text).splitAllowingEmptyEntries('\n'))
        lines.append(line);

    Vector<ImpliedLabel> labels;
    Vector<StringView, 4> prefixesInFunction;
    bool isInFunction = false;
    StringView function;
    for (size_t index = 0; index < lines.size(); ++index) {
        StringView line = lines[index].endsWith('\r') ? lines[index].left(lines[index].length() - 1) : lines[index];
        if (line.startsWith(functionLineStart) && line.endsWith('{')) {
            isInFunction = true;
            function = labelOfFunctionLine(line);
            prefixesInFunction.shrink(0);
        } else if (line == "}"_s)
            isInFunction = false;
        else if (StringView prefix = isInFunction ? checkPrefixOfComment(line) : StringView(); !prefix.isEmpty() && !prefixesInFunction.contains(prefix)) {
            if (function.isNull()) {
                writtenOut.errorLine = writtenOut.sourceLineOf(static_cast<unsigned>(index + 1));
                writtenOut.error = "check lines stand inside a function that has no name"_s;
                return;
            }
            prefixesInFunction.append(prefix);
            labels.append({ index, prefix, function });
        }
    }
    if (labels.isEmpty())
        return;

    StringBuilder text;
    Vector<unsigned> sourceLines;
    size_t nextLabel = 0;
    for (size_t index = 0; index < lines.size(); ++index) {
        unsigned sourceLine = writtenOut.sourceLineOf(static_cast<unsigned>(index + 1));
        if (nextLabel < labels.size() && labels[nextLabel].line == index) {
            text.append("# "_s, labels[nextLabel].prefix, "-LABEL: "_s, labels[nextLabel].function, '\n');
            sourceLines.append(sourceLine);
            ++nextLabel;
        }
        text.append(lines[index]);
        if (index + 1 < lines.size())
            text.append('\n');
        sourceLines.append(sourceLine);
    }
    writtenOut.text = text.toString();
    writtenOut.sourceLines = WTF::move(sourceLines);
}

WrittenOutText writeOut(String&& text, const String& folder)
{
    WrittenOutText writtenOut { WTF::move(text), { }, 0, { } };
    if (writtenOut.text.contains(repeatLineStart) || writtenOut.text.contains(programLineStart))
        writtenOut = RepeatWriter(String(writtenOut.text), folder).write();
    if (writtenOut.error.isNull())
        writeOutLabels(writtenOut);
    return writtenOut;
}

ProgramFile::Kind programKindOf(std::span<const uint8_t> bytes)
{
    static constexpr auto magicBytes = std::bit_cast<std::array<uint8_t, sizeof(Wire::Magic)>>(Wire::Magic);
    return spanHasPrefix(bytes, std::span { magicBytes }) ? ProgramFile::Kind::Bytes : ProgramFile::Kind::Text;
}

std::optional<ProgramFile> readProgramFile(const String& path, PrintStream& errors)
{
    std::optional<Vector<uint8_t>> bytes = FileSystem::readEntireFile(path);
    if (!bytes) {
        errors.println("cannot read ", path);
        return std::nullopt;
    }

    ProgramFile file;
    file.bytes = WTF::move(*bytes);
    file.kind = programKindOf(file.bytes.span());
    if (file.kind == ProgramFile::Kind::Bytes)
        return file;

    if (!StringImpl::isValidLength<char16_t>(file.bytes.size())) {
        errors.println(path, " is too large for IR text");
        return std::nullopt;
    }
    file.text = String::fromUTF8(file.bytes.span());
    if (file.text.isNull()) {
        errors.println(path, " is neither a program nor UTF-8 text");
        return std::nullopt;
    }
    WrittenOutText writtenOut = writeOut(WTF::move(file.text), FileSystem::parentPath(path));
    if (!writtenOut.error.isNull()) {
        errors.println(path, ":", writtenOut.errorLine, ": ", writtenOut.error);
        return std::nullopt;
    }
    file.text = WTF::move(writtenOut.text);
    return file;
}

std::unique_ptr<Program> createProgram(const ProgramFile& file, BuildContext& build)
{
    switch (file.kind) {
    case ProgramFile::Kind::Bytes:
        return Program::read(file.bytes.span(), build);
    case ProgramFile::Kind::Text:
        return parseProgram(file.text, IRForm::AsRead, build);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printDiagnostics(const Diagnostics& diagnostics, PrintStream& out)
{
    for (size_t index = 0; index < diagnostics.userDiagnostics().size(); ++index)
        out.println(nameOf(diagnostics.userDiagnostics()[index].kind), ": ", diagnostics.messageOfUserDiagnostic(index));
    for (size_t index = 0; index < diagnostics.internalErrors().size(); ++index)
        out.println(nameOf(diagnostics.internalErrors()[index].kind), ": ", diagnostics.messageOfInternalError(index));
}

Vector<uint8_t> encodeProgramText(StringView text, BuildContext& build)
{
    std::unique_ptr<Program> program = parseProgram(text, IRForm::AsRead, build);
    if (!program)
        return { };
    return Vector<uint8_t>(program->buffer());
}

String standInLibraryText(unsigned libraryFunction)
{
    StringBuilder text;
    text.append("function f"_s, libraryFunction, " name=\"library\" role=Library results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions()) {
        if (function.platforms.contains(targetOfThisBuild().platform))
            text.append("runtimefunction id="_s, function.id.index(), " function=f"_s, libraryFunction, '\n');
    }
    return text.toString();
}

static std::optional<uint32_t> runtimeFunctionNumberOf(StringView line)
{
    constexpr ASCIILiteral prefix = "runtimefunction id="_s;
    if (!line.startsWith(prefix))
        return std::nullopt;
    StringView rest = line.substring(prefix.length());
    size_t end = rest.find(' ');
    return parseInteger<uint32_t>(end == notFound ? rest : rest.left(end), 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

String withStandInLibrary(const String& programText)
{
    struct OwnLine {
        uint32_t id;
        StringView line;
    };

    unsigned numberOfFunctions = 0;
    unsigned numberOfModules = 0;
    Vector<OwnLine> ownLines;
    StringBuilder text;
    for (StringView line : StringView(programText).splitAllowingEmptyEntries('\n')) {
        if (line == ownLibraryLine)
            return programText;
        if (std::optional<uint32_t> id = runtimeFunctionNumberOf(line))
            ownLines.append({ *id, line });
        else {
            if (line.startsWith("function f"_s))
                ++numberOfFunctions;
            if (line.startsWith("module m"_s))
                ++numberOfModules;
            text.append(line);
        }
        text.append('\n');
    }
    if (!numberOfFunctions)
        return programText;

    Vector<uint32_t> standInIDs;
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions()) {
        uint32_t id = function.id.index();
        if (function.platforms.contains(targetOfThisBuild().platform) && !ownLines.containsIf([&](const OwnLine& own) { return own.id == id; }))
            standInIDs.append(id);
    }
    std::ranges::sort(standInIDs);
    std::ranges::stable_sort(ownLines, { }, &OwnLine::id);

    // As a front end sends it: a registered function lies in a module of the library, which has no body.
    if (numberOfModules) {
        text.append("module m"_s, numberOfModules, " path=\"library\" displayPath=\"library\" flags=[IsLibrary]\n"_s);
        text.append("function f"_s, numberOfFunctions, " name=\"library\" module=m"_s, numberOfModules, " role=Library results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    } else
        text.append("function f"_s, numberOfFunctions, " name=\"library\" role=Library results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    size_t nextOwnLine = 0;
    auto appendOwnLinesBelow = [&](uint64_t id) {
        for (; nextOwnLine < ownLines.size() && ownLines[nextOwnLine].id < id; ++nextOwnLine)
            text.append(ownLines[nextOwnLine].line, '\n');
    };
    for (uint32_t id : standInIDs) {
        appendOwnLinesBelow(id);
        text.append("runtimefunction id="_s, id, " function=f"_s, numberOfFunctions, '\n');
    }
    appendOwnLinesBelow(uint64_t { 1 } << 32);
    return text.toString();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
