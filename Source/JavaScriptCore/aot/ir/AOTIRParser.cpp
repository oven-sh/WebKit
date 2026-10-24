/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRParserInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTReaderInternal.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "AOTVerifierInternal.h"
#include "LineColumn.h"
#include "PureNaN.h"
#include <algorithm>
#include <climits>
#include <limits>
#include <wtf/text/CString.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr PlacedTableSizes unboundedTableSizes { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };

std::unique_ptr<Program> parseProgram(StringView text, IRForm form, BuildContext& build, const VerifierInputs& inputs)
{
    // ASCII is its own UTF-8. The text of a large program is over the length that the conversion takes.
    if (text.is8Bit() && text.containsOnlyASCII())
        return TextParser(byteCast<uint8_t>(text.span8()), form, build, inputs).run();
    CString utf8 = text.utf8();
    return TextParser(byteCast<uint8_t>(utf8.span()), form, build, inputs).run();
}

static bool endsLine(const Token& token)
{
    return token.kind == TokenKind::EndOfLine || token.kind == TokenKind::EndOfFile;
}

static std::optional<ProgramList> findList(const Token& keyword)
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
    for (Token current = lexer.next(); !endsLine(current); current = lexer.next()) {
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

static bool canHaveExpectedTypes(IRForm form)
{
    switch (form) {
    case IRForm::AsRead:
    case IRForm::Prepared:
        return true;
    case IRForm::Selected:
    case IRForm::Placed:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A pass that moves the body of a function into an appended function leaves one of these under the id of the function.
static bool canBeRoleOfFunctionAsRead(FunctionRole role)
{
    switch (role) {
    case FunctionRole::CoroutineRamp:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::StubBody:
        return true;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWorker:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
        return static_cast<unsigned>(role) < numberOfWireEnumerators<FunctionRole>;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Passes append the functions they derive, so the functions as read end at the first function that only a pass can have appended.
static unsigned countFunctionsAsRead(const Program& program)
{
    auto firstDerivedFunction = std::ranges::find_if(program.functions, [](const Function& function) {
        return !canBeRoleOfFunctionAsRead(function.role) || function.derivation != Derivation { };
    });
    return static_cast<unsigned>(firstDerivedFunction - program.functions.begin());
}

// Typing follows operands and runs before dominance is verified. Only in form AsRead does the order of the blocks rule a cycle of operands out.
static ValueID findValueInOperandCycle(const Function& function)
{
    enum class State : uint8_t { Unvisited, OnPath, Finished };
    Vector<State> states(FillWith { }, function.values.size(), State::Unvisited);
    Vector<ValueID> worklist;
    for (size_t index = 0; index < function.values.size(); ++index) {
        worklist.append(ValueID(index));
        while (!worklist.isEmpty()) {
            ValueID value = worklist.last();
            State& state = states[value.index()];
            if (state != State::Unvisited) {
                state = State::Finished;
                worklist.removeLast();
                continue;
            }

            state = State::OnPath;
            bool closesCycle = false;
            function.at(value).forEachChild([&](ValueID operand) {
                State operandState = states[operand.index()];
                closesCycle |= operandState == State::OnPath;
                if (operandState == State::Unvisited)
                    worklist.append(operand);
            });
            if (closesCycle)
                return value;
        }
    }
    return { };
}

// The reader has not seen the engine's fields, and typing follows the ids in them.
static bool verifyPatchedReferences(const Program& program, IRForm form, BuildContext& build, const VerifierInputs& inputs)
{
    if (!ProgramVerifier(program, form, build, inputs).runIDs())
        return false;

    bool isValid = true;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        FunctionVerifier verifier(program, id, function, form, build, inputs);
        if (!verifier.runStructure()) {
            build.diagnostics().append(verifier.takeErrors());
            isValid = false;
            continue;
        }
        if (ValueID value = findValueInOperandCycle(function)) {
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::UseNotDominated, .function = id, .value = value });
            isValid = false;
        }
    }
    return isValid;
}

static TypePropagationOutcome typeFunction(const TypePropagationInputs& inputs, IRForm form)
{
    if (canHaveExpectedTypes(form))
        return typeValuesFromNever(inputs);

    TypePropagation propagation(inputs);
    propagation.restartAllValues();
    propagation.solveTypes();
    return propagation.finish();
}

static bool typeAllFunctions(Program& program, IRForm form, BuildContext& build)
{
    bool hasCutFunction = false;
    TypeArena types(program.types);
    for (size_t index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
        if (!typeFunction({ program, id, function, analyses, types, build }, form).didChangeControlFlow())
            continue;
        // Cutting a block renumbers the values, which the text names by number.
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::ValueAfterNever, .function = id });
        hasCutFunction = true;
    }
    return !hasCutFunction;
}

template<typename WireRecord, typename EngineRecord, typename Record>
static void appendLine(TextParser& parser, const WireRecord& wire, EngineRecord engineFields, Vector<Record> Program::* records)
{
    Vector<WireRecord>& wireRecords = parser.builder().sections().template listOf<WireRecord>();
    parser.addPatch([records, index = wireRecords.size(), engineFields = WTF::move(engineFields)](PatchContext& context) {
        applyEngineFields(context, engineFields, (context.program.*records)[index]);
    });
    wireRecords.append(wire);
}

template<typename WireRecord, typename EngineRecord, typename Record>
static void parseLine(TextParser& parser, Vector<Record> Program::* records)
{
    WireRecord wire;
    if constexpr (requires { parseFields(parser, wire); }) {
        parseFields(parser, wire);
        parser.builder().sections().template listOf<WireRecord>().append(wire);
    } else {
        EngineRecord engineFields;
        parseFields(parser, wire, engineFields);
        appendLine(parser, wire, WTF::move(engineFields), records);
    }
}

std::unique_ptr<Program> TextParser::run()
{
    if (!scanItems())
        return nullptr;

    m_program = makeUnique<Program>(m_builtins);
    for (const TextItem& item : m_items) {
        parseItem(item);
        if (hasFailed())
            return nullptr;
    }

    if (m_form == IRForm::Placed && !m_verifierInputs.placed)
        m_verifierInputs.placed = &unboundedTableSizes;
    m_verifierInputs.hasUncheckedRecords = m_form != IRForm::AsRead;

    m_program->adoptBuffer(m_builder.finish());
    Reader reader(*m_program, m_form, m_build);
    reader.setTypeCompletion([&](uint32_t wireIndex, Type& type) {
        completeType(wireIndex, type);
    });
    VerifierInputs inputsOfReader = m_verifierInputs;
    inputsOfReader.lacksEngineFields = true;
    reader.setVerifierInputs(inputsOfReader);
    if (!reader.run())
        return nullptr;

    applyPatches(*m_program, reader.typesAsRead());
    if (m_form == IRForm::AsRead)
        return WTF::move(m_program);

    m_program->setNumberOfFunctionsAsRead(countFunctionsAsRead(*m_program));
    if (!verifyPatchedReferences(*m_program, m_form, m_build, m_verifierInputs))
        return nullptr;
    m_program->indexClasses();
    m_program->indexNames();
    if (!typeAllFunctions(*m_program, m_form, m_build))
        return nullptr;
    applyValueEngineFields({ *m_program, reader.typesAsRead() });
    if (!verifyProgram(*m_program, m_form, m_build, m_verifierInputs))
        return nullptr;
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
    for (Token current = nextScannedToken(lexer); !hasFailed() && !endsLine(current); current = nextScannedToken(lexer))
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
        bool hasNamespaceLayout = scanModuleKind(m_text, item.start) == ModuleKind::SourceText;
        m_scan.namespaceLayoutRanks.append(hasNamespaceLayout ? std::optional(m_scan.numberOfNamespaceLayouts++) : std::nullopt);
    }
    return !hasFailed();
}

void TextParser::scanItem(Lexer& lexer, const Token& keyword)
{
    std::optional<ProgramList> list = findList(keyword);
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

void TextParser::scanBlockHead(Lexer& lexer, BodyScan& scan)
{
    unsigned nesting = 0;
    bool isInParameterList = true;
    for (Token current = lexer.next(); !endsLine(current); current = lexer.next()) {
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

bool TextParser::scanBody(Lexer::State start, BodyScan& scan)
{
    scan = { };
    Lexer lexer(m_text);
    lexer.restore(start);
    Token first = lexer.next();
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
        parseLine<Wire::TypeSite, TypeSite>(*this, &Program::typeSites);
        break;
    case ProgramList::ExpectedResults:
        parseLine<Wire::ExpectedResult, ExpectedResult>(*this, &Program::expectedResults);
        break;
    case ProgramList::Layouts:
        parseLine<Wire::Layout, LayoutRecord>(*this, &Program::layouts);
        break;
    case ProgramList::Variables:
        parseLine<Wire::Variable, Variable>(*this, &Program::variables);
        break;
    case ProgramList::Modules:
        parseLine<Wire::Module, Module>(*this, &Program::modules);
        break;
    case ProgramList::ModuleFailures:
        parseLine<Wire::ModuleFailure, ModuleFailure>(*this, &Program::moduleFailures);
        break;
    case ProgramList::EvalScopes:
        parseLine<Wire::EvalScope, EvalScope>(*this, &Program::evalScopes);
        break;
    case ProgramList::Functions: {
        Wire::Function wire;
        // Wire type 0 is whichever type the text names first.
        wire.thisType.type = typeWordOfAnything();
        FunctionRecord engineFields;
        parseFields(*this, wire, engineFields);
        wire.body = parseBody(item);
        appendLine(*this, wire, WTF::move(engineFields), &Program::functions);
        break;
    }
    case ProgramList::Data:
        parseLine<Wire::Data, Data>(*this, &Program::data);
        break;
    case ProgramList::Entries:
        parseLine<Wire::Entry, Entry>(*this, &Program::entries);
        break;
    case ProgramList::RuntimeFunctions:
        parseLine<Wire::RuntimeFunctionEntry, RuntimeFunctionEntry>(*this, &Program::runtimeFunctions);
        break;
    case ProgramList::WebAssemblyModules:
        parseLine<Wire::WebAssemblyModule, WebAssemblyModule>(*this, &Program::webAssemblyModules);
        break;
    case ProgramList::Bytes:
    case ProgramList::Words:
    case ProgramList::Strings:
    case ProgramList::PropertyKeys:
    case ProgramList::Types:
    case ProgramList::ClassParts:
    case ProgramList::DeclaredTypes:
    case ProgramList::TemplateParts:
    case ProgramList::ValidationRules:
    case ProgramList::ValidationMembers:
    case ProgramList::ValidationNodes:
    case ProgramList::Fields:
    case ProgramList::StaticFields:
    case ProgramList::ModuleRequests:
    case ProgramList::ModuleExports:
    case ProgramList::SyntheticExports:
    case ProgramList::RequiredNames:
    case ProgramList::ColumnAdjustments:
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
    // A number above the int64 range is a Decimal token, whose double has lost digits.
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

// The reader of a program as read derives layouts from those of the text and places them after the layouts of built-ins and namespaces. The text numbers them after its own.
uint32_t TextParser::layoutWordOf(uint32_t numberInText)
{
    if (m_form != IRForm::AsRead || numberInText < countOf(ProgramList::Layouts))
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
    applyValueEngineFields(context);
}

void TextParser::applyValueEngineFields(const PatchContext& context) const
{
    for (const ValueEngineFields& fields : m_valueEngineFields) {
        Function& function = context.program.at(fields.function);
        Value& value = function.edit(fields.value);
        if (fields.typeWord != Wire::none)
            value.type = context.type(fields.typeWord);
        if (fields.expectedWord != Wire::none)
            value.setExpected(context.type(fields.expectedWord));
        if (fields.representation) {
            value.representation = *fields.representation;
            function.representationsAreChosen = true;
        }
    }

    // An operation without a result has no `vN` in the text that could state its representation.
    for (Function& function : context.program.functions) {
        if (!function.representationsAreChosen)
            continue;
        for (Value& value : function.editAllValues()) {
            if (resultRuleOf(value.opcode) == ResultRule::None)
                value.representation = Representation::Void;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
