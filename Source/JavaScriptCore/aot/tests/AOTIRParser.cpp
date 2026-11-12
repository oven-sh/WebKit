/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRParserInternal.h"

#if ENABLE(AOT)

#include "AOTReaderInternal.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "LineColumn.h"
#include "PureNaN.h"
#include <limits>
#include <wtf/text/CString.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

std::unique_ptr<Program> parseProgram(StringView text, BuildContext& build)
{
    if (text.is8Bit() && text.containsOnlyASCII())
        return TextParser(byteCast<uint8_t>(text.span8()), build).run();
    CString utf8 = text.utf8();
    return TextParser(byteCast<uint8_t>(utf8.span()), build).run();
}

static bool endsLineOfIRText(const Token& token)
{
    return token.kind == TokenKind::EndOfLine || token.kind == TokenKind::EndOfFile;
}

static std::optional<ProgramList> findProgramList(const Token& keyword)
{
    for (ProgramList list : allMembersOf<ProgramList>) {
        std::optional<ASCIILiteral> candidate = keywordOf(list);
        if (candidate && keyword.isWord(*candidate))
            return list;
    }
    return std::nullopt;
}

static std::optional<ModuleKind> scanModuleKind(std::span<const uint8_t> text, Lexer::State lineStart)
{
    Lexer lexer(text);
    lexer.restore(lineStart);
    unsigned nesting = 0;
    Token name;
    Token separator;
    for (Token current = lexer.next(); !endsLineOfIRText(current); current = lexer.next()) {
        if (!nesting && name.isWord("kind"_s) && separator.kind == TokenKind::Equal)
            return current.kind == TokenKind::Word ? parseEnum<ModuleKind>(current.view()) : std::nullopt;
        if (current.kind == TokenKind::LeftBrace || current.kind == TokenKind::LeftBracket)
            ++nesting;
        else if (nesting && (current.kind == TokenKind::RightBrace || current.kind == TokenKind::RightBracket))
            --nesting;
        name = separator;
        separator = current;
    }
    return ModuleKind { };
}

static bool hasOnlyByteEscapes(std::span<const uint8_t> stringText)
{
    for (size_t index = 0; index < stringText.size(); ++index) {
        if (stringText[index] > 0x7e)
            return false;
        if (stringText[index] != '\\')
            continue;
        ++index;
        if (index == stringText.size() || stringText[index] != 'x')
            return false;
    }
    return true;
}

template<typename WireRecord, typename EngineRecord, typename Record>
static void appendProgramRecord(TextParser& parser, const WireRecord& wire, EngineRecord engineFields, Vector<Record> Program::* records)
{
    Vector<WireRecord>& wireRecords = parser.builder().sections().template listOf<WireRecord>();
    parser.addPatch([records, index = wireRecords.size(), engineFields = WTF::move(engineFields)](PatchContext& context) {
        applyEngineFields(context, engineFields, (context.program.*records)[index]);
    });
    wireRecords.append(wire);
}

template<typename WireRecord, typename EngineRecord, typename Record>
static void parseProgramRecord(TextParser& parser, Vector<Record> Program::* records)
{
    WireRecord wire;
    if constexpr (requires { parseFields(parser, wire); }) {
        parseFields(parser, wire);
        parser.builder().sections().template listOf<WireRecord>().append(wire);
    } else {
        EngineRecord engineFields;
        parseFields(parser, wire, engineFields);
        appendProgramRecord(parser, wire, WTF::move(engineFields), records);
    }
}

TextParser::~TextParser() = default;

std::unique_ptr<Program> TextParser::run()
{
    if (!scanItems())
        return nullptr;

    m_program = makeUnique<Program>(m_builtins);
    AOT::stableSort(m_items, { }, &TextItem::list);
    for (const TextItem& item : m_items) {
        parseItem(item);
        if (hasFailed())
            return nullptr;
    }

    typeWordOfAnything();

    m_program->adoptBuffer(m_builder.finish().releaseBuffer());
    Reader reader(*m_program, m_build);
    if (!reader.run())
        return nullptr;

    applyPatches(*m_program, m_program->types.typesOfWireIndexes());
    return WTF::move(m_program);
}

Token TextParser::nextScannedToken(Lexer& lexer)
{
    Token scanned = lexer.next();
    if (scanned.kind == TokenKind::Invalid)
        fail(InternalErrorKind::IRTextError, scanned.position);
    return scanned;
}

Token TextParser::scanToEndOfLine(Lexer& lexer)
{
    Token last;
    for (Token current = nextScannedToken(lexer); !hasFailed() && !endsLineOfIRText(current); current = nextScannedToken(lexer))
        last = current;
    return last;
}

bool TextParser::scanItems()
{
    Lexer lexer(m_text);
    while (!hasFailed()) {
        Token keyword = nextScannedToken(lexer);
        if (keyword.kind == TokenKind::EndOfFile)
            break;
        scanItem(lexer, keyword);
    }

    for (const TextItem& item : m_items) {
        if (item.list != ProgramList::Modules)
            continue;
        std::optional<ModuleKind> kind = scanModuleKind(m_text, item.start);
        m_scan.namespaceLayoutRanks.append(kind && hasNamespaceLayout(*kind) ? std::optional(m_scan.numberOfNamespaceLayouts++) : std::nullopt);
    }
    return !hasFailed();
}

void TextParser::scanItem(Lexer& lexer, const Token& keyword)
{
    std::optional<ProgramList> list = findProgramList(keyword);
    if (!list) {
        fail(InternalErrorKind::IRTextError, keyword.position);
        return;
    }

    TextItem item { *list, false, lexer.state(), { } };
    if (std::optional<char> letter = prefixLetterOf(*list)) {
        Token id = nextScannedToken(lexer);
        if (id.kind != TokenKind::ID || letterOf(id.prefix) != *letter || id.number != countOf(*list))
            fail(InternalErrorKind::IRTextError, id.position);
    }

    Token last = scanToEndOfLine(lexer);
    if (*list == ProgramList::Functions && last.kind == TokenKind::LeftBrace) {
        item.hasBody = true;
        item.body = lexer.state();
        scanFunctionEnd(lexer);
    }
    ++countOf(*list);
    m_items.append(item);
}

void TextParser::scanFunctionEnd(Lexer& lexer)
{
    while (!hasFailed()) {
        Token first = nextScannedToken(lexer);
        if (first.kind == TokenKind::EndOfFile) {
            fail(InternalErrorKind::IRTextError, first.position);
            return;
        }
        if (first.kind == TokenKind::RightBrace) {
            Token after = nextScannedToken(lexer);
            if (after.kind != TokenKind::EndOfLine)
                fail(InternalErrorKind::IRTextError, after.position);
            return;
        }
        scanToEndOfLine(lexer);
    }
}

void TextParser::scanBlockHead(Lexer& lexer, BodyTextScan& scan)
{
    unsigned nesting = 0;
    bool isInParameterList = true;
    for (Token current = lexer.next(); !endsLineOfIRText(current); current = lexer.next()) {
        if (!isInParameterList)
            continue;
        if (current.kind == TokenKind::LeftParenthesis)
            ++nesting;
        else if (current.kind == TokenKind::RightParenthesis) {
            if (nesting)
                --nesting;
            isInParameterList = nesting;
        } else if (current.kind == TokenKind::ID && current.prefix == IDPrefix::Value)
            ++scan.numberOfValues;
    }
}

bool TextParser::scanBody(Lexer::State start, BodyTextScan& scan)
{
    scan = { };
    Lexer lexer(m_text);
    lexer.restore(start);
    Token first = lexer.next();
    scan.leavesOutEntryHead = first.kind != TokenKind::RightBrace && first.kind != TokenKind::EndOfFile && !(first.kind == TokenKind::ID && first.prefix == IDPrefix::Block);
    scan.numberOfBlocks = scan.leavesOutEntryHead;
    for (; !hasFailed() && first.kind != TokenKind::RightBrace && first.kind != TokenKind::EndOfFile; first = lexer.next()) {
        if (first.kind == TokenKind::ID && first.prefix == IDPrefix::Block) {
            ++scan.numberOfBlocks;
            scanBlockHead(lexer, scan);
            continue;
        }
        bool isTerminator = first.kind == TokenKind::Word && parseEnum<TerminatorKind>(first.view());
        if (!isTerminator)
            ++scan.numberOfValues;
        scanToEndOfLine(lexer);
    }
    if (!scan.numberOfBlocks)
        fail(InternalErrorKind::IRTextError, first.position);
    return !hasFailed();
}

void TextParser::parseItem(const TextItem& item)
{
    seek(item.start);
    if (prefixLetterOf(item.list))
        advance();

    switch (item.list) {
    case ProgramList::TypeSites:
        parseProgramRecord<Wire::TypeSite, TypeSite>(*this, &Program::typeSites);
        break;
    case ProgramList::ExpectedResults:
        parseProgramRecord<Wire::ExpectedResult, ExpectedResult>(*this, &Program::expectedResults);
        break;
    case ProgramList::Layouts:
        parseProgramRecord<Wire::Layout, LayoutRecord>(*this, &Program::layouts);
        break;
    case ProgramList::Variables:
        parseProgramRecord<Wire::Variable, Variable>(*this, &Program::variables);
        break;
    case ProgramList::Modules:
        parseProgramRecord<Wire::Module, Module>(*this, &Program::modules);
        break;
    case ProgramList::ModuleInstances:
        parseProgramRecord<Wire::ModuleInstance, ModuleInstance>(*this, &Program::moduleInstances);
        break;
    case ProgramList::ModuleLinks:
        parseProgramRecord<Wire::ModuleLink, ModuleLink>(*this, &Program::moduleLinks);
        break;
    case ProgramList::ModuleFailures:
        parseProgramRecord<Wire::ModuleFailure, ModuleFailure>(*this, &Program::moduleFailures);
        break;
    case ProgramList::AmbiguousExports:
        parseProgramRecord<Wire::AmbiguousExport, AmbiguousExport>(*this, &Program::ambiguousExports);
        break;
    case ProgramList::SourceStamps:
        parseProgramRecord<Wire::SourceStamp, SourceStamp>(*this, &Program::sourceStamps);
        break;
    case ProgramList::DeclaredRecords:
        parseProgramRecord<Wire::DeclaredRecord, DeclaredRecord>(*this, &Program::declaredRecords);
        break;
    case ProgramList::EvalScopes:
        parseProgramRecord<Wire::EvalScope, EvalScope>(*this, &Program::evalScopes);
        break;
    case ProgramList::Functions: {
        Wire::Function wire;
        wire.thisType.type = typeWordOfAnything();
        FunctionRecord engineFields;
        parseFields(*this, wire, engineFields);
        wire.body = parseBody(item, wire);
        appendProgramRecord(*this, wire, WTF::move(engineFields), &Program::functions);
        break;
    }
    case ProgramList::Data:
        parseProgramRecord<Wire::Data, Data>(*this, &Program::data);
        break;
    case ProgramList::Entries:
        parseProgramRecord<Wire::Entry, Entry>(*this, &Program::entries);
        break;
    case ProgramList::RuntimeFunctions:
        parseProgramRecord<Wire::RuntimeFunctionEntry, RuntimeFunctionEntry>(*this, &Program::runtimeFunctions);
        break;
    case ProgramList::WebAssemblyModules:
        parseProgramRecord<Wire::WebAssemblyModule, WebAssemblyModule>(*this, &Program::webAssemblyModules);
        break;
    case ProgramList::ReplaceableBuiltins:
        parseProgramRecord<Wire::ReplaceableBuiltin, ReplaceableBuiltin>(*this, &Program::replaceableBuiltins);
        break;
    case ProgramList::OpenedNames:
        parseProgramRecord<Wire::OpenedName, OpenedName>(*this, &Program::openedNames);
        break;
    case ProgramList::BuiltinsUsedAtStart:
        parseProgramRecord<Wire::BuiltinUsedAtStart, BuiltinUsedAtStart>(*this, &Program::builtinsUsedAtStart);
        break;
    case ProgramList::Bytes:
    case ProgramList::Words:
    case ProgramList::Strings:
    case ProgramList::PropertyKeys:
    case ProgramList::Types:
    case ProgramList::ClassParts:
    case ProgramList::DeclaredTypes:
    case ProgramList::DeclaredCaptures:
    case ProgramList::Fields:
    case ProgramList::StaticFields:
    case ProgramList::ModuleRequests:
    case ProgramList::ModuleExports:
    case ProgramList::SyntheticExports:
    case ProgramList::RequiredNames:
    case ProgramList::ColumnAdjustments:
    case ProgramList::QuotedSources:
    case ProgramList::EvalBindings:
    case ProgramList::Code:
    case ProgramList::DataNodes:
    case ProgramList::DataMembers:
    case ProgramList::HostReads:
        RELEASE_ASSERT_NOT_REACHED();
    }
    expectEndOfLine();
}

void TextParser::advance()
{
    if (hasFailed())
        return;
    m_cursor.token = m_cursor.nextToken;
    m_cursor.nextToken = m_cursor.lexer.next();
}

void TextParser::seek(Lexer::State state)
{
    if (hasFailed())
        return;
    m_cursor.lexer.restore(state);
    m_cursor.token = m_cursor.lexer.next();
    m_cursor.nextToken = m_cursor.lexer.next();
}

bool TextParser::accept(TokenKind kind)
{
    if (!at(kind))
        return false;
    advance();
    return true;
}

bool TextParser::acceptWord(ASCIILiteral word)
{
    if (!atWord(word))
        return false;
    advance();
    return true;
}

bool TextParser::acceptField(ASCIILiteral name)
{
    if (!atWord(name) || !atField())
        return false;
    advance();
    advance();
    return true;
}

void TextParser::expect(TokenKind kind)
{
    if (!accept(kind))
        fail();
}

void TextParser::expectWord(ASCIILiteral word)
{
    if (!acceptWord(word))
        fail();
}

void TextParser::expectEndOfLine()
{
    expect(TokenKind::EndOfLine);
}

void TextParser::fail(InternalErrorKind kind)
{
    fail(kind, token().position);
}

void TextParser::fail(InternalErrorKind kind, TextPosition position)
{
    if (hasFailed())
        return;
    m_build.diagnostics().add(InternalError { .kind = kind, .textPosition = LineColumn { position.line, position.column } });
    m_hasFailed = true;
    m_cursor.token = { };
    m_cursor.nextToken = { };
}

uint32_t TextParser::parseUInt32(uint32_t maximum)
{
    if (!at(TokenKind::Integer) || token().integer() < 0 || token().number > maximum) {
        fail();
        return 0;
    }
    uint32_t value = static_cast<uint32_t>(token().number);
    advance();
    return value;
}

uint64_t TextParser::parseUInt64()
{
    std::optional<uint64_t> value;
    if (at(TokenKind::Integer) || at(TokenKind::Decimal))
        value = parseInteger<uint64_t>(token().view());
    if (!value) {
        fail();
        return 0;
    }
    advance();
    return *value;
}

int64_t TextParser::parseInt64()
{
    if (!at(TokenKind::Integer)) {
        fail();
        return 0;
    }
    int64_t value = token().integer();
    advance();
    return value;
}

double TextParser::parseDouble()
{
    double value = 0;
    if (at(TokenKind::Integer))
        value = static_cast<double>(token().integer());
    else if (at(TokenKind::Decimal))
        value = token().decimal();
    else if (atWord("NaN"_s))
        value = PNaN;
    else if (atWord("Infinity"_s))
        value = std::numeric_limits<double>::infinity();
    else {
        fail();
        return 0;
    }
    advance();
    return value;
}

bool TextParser::parseBool()
{
    if (acceptWord("true"_s))
        return true;
    if (!acceptWord("false"_s))
        fail();
    return false;
}

uint32_t TextParser::layoutWordOf(uint32_t numberInText)
{
    if (numberInText < countOf(ProgramList::Layouts))
        return numberInText;
    uint64_t word = static_cast<uint64_t>(numberInText) + m_builtins.layoutCount() + m_scan.numberOfNamespaceLayouts;
    if (word >= Wire::none) {
        fail(InternalErrorKind::IdOutOfRange);
        return 0;
    }
    return static_cast<uint32_t>(word);
}

uint32_t TextParser::parseIDWord(IDPrefix prefix)
{
    if (atID(prefix)) {
        uint32_t number = static_cast<uint32_t>(token().number);
        if (prefix == IDPrefix::Layout)
            number = layoutWordOf(number);
        advance();
        return number;
    }
    if (prefix != IDPrefix::Layout || !acceptWord("L"_s)) {
        fail();
        return 0;
    }

    uint32_t derivedLayoutIndex = 0;
    expect(TokenKind::LeftParenthesis);
    if (atID(IDPrefix::Module)) {
        const auto& ranks = m_scan.namespaceLayoutRanks;
        uint64_t module = token().number;
        if (module >= ranks.size() || !ranks[module]) {
            fail();
            return 0;
        }
        derivedLayoutIndex = m_builtins.layoutCount() + *ranks[module];
        advance();
    } else
        derivedLayoutIndex = parseBuiltinLayout().index();
    expect(TokenKind::RightParenthesis);
    return hasFailed() ? 0 : countOf(ProgramList::Layouts) + derivedLayoutIndex;
}

Wire::Span TextParser::parseByteString()
{
    if (!at(TokenKind::String) || !hasOnlyByteEscapes(token().text)) {
        fail();
        return { };
    }
    Wire::Span span = m_builder.appendBytes(decodeStringToken(token()).span());
    advance();
    return span;
}

void TextParser::applyPatches(Program& program, std::span<const TypeID> types)
{
    PatchContext context { program, types };
    for (ProgramPatch& patch : m_patches)
        patch(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
