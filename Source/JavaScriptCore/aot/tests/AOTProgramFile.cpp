/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramFile.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRParser.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <bit>
#include <wtf/FileSystem.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

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
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions())
        text.append("runtimefunction id="_s, function.id.index(), " function=f"_s, libraryFunction, '\n');
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
            text.append(line);
        }
        text.append('\n');
    }
    if (!numberOfFunctions)
        return programText;

    Vector<uint32_t> standInIDs;
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions()) {
        uint32_t id = function.id.index();
        if (!ownLines.containsIf([&](const OwnLine& own) { return own.id == id; }))
            standInIDs.append(id);
    }
    std::ranges::sort(standInIDs);
    std::ranges::stable_sort(ownLines, { }, &OwnLine::id);

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
