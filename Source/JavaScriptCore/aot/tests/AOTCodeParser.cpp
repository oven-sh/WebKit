/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRLexer.h"
#include "AOTIRPrinter.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include "AOTTypeText.h"
#include "PureNaN.h"
#include <algorithm>
#include <limits>
#include <wtf/ASCIICType.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t absentInCodeText = UINT32_MAX;
static constexpr auto endOfFrameNumbersInCodeText = static_cast<uint32_t>(defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture()));

struct FrameInCodeText {
    uint32_t line { 0 };
    uint32_t number { 0 };
    InlineFrame frame { };
};

struct RecordInCodeText {
    Lexer::State start;
    uint32_t line { 0 };
    uint32_t id { 0 };
    uint32_t block { absentInCodeText };
    uint32_t parameter { absentInCodeText };
};

struct ParameterInCodeText {
    uint32_t id { 0 };
    TypeID type;
    Representation representation { Representation::Unchosen };
    Origin origin;
};

struct TargetInCodeText {
    uint32_t block { absentInCodeText };
    uint32_t firstArgument { 0 };
    uint32_t numberOfArguments { 0 };
    uint32_t line { 0 };
    StringView name;
};

struct BlockInCodeText {
    uint32_t line { 0 };
    BlockRole role { BlockRole::Ordinary };
    BlockFrequency frequency { BlockFrequency::Normal };
    bool isInLoop { false };
    bool isLoopHead { false };
    bool keepsSitesThoughRare { false };
    bool hasHandler { false };
    bool hasTerminator { false };
    bool hasOperations { false };
    uint32_t subject { absentInCodeText };
    uint32_t entry { absentInCodeText };
    uint32_t firstParameter { 0 };
    uint32_t numberOfParameters { 0 };
    uint32_t numberOfParametersToAppend { 0 };
    uint32_t lastOperation { 0 };
    TargetInCodeText handler;
    EdgeID handlerEdge;
    Lexer::State terminator;
};

struct FactLineInCodeText {
    Lexer::State start;
    uint32_t recordInFront { absentInCodeText };
};

struct TraitReadByCodeParser {
    ASCIILiteral name;
    bool (EffectTraits::*holds)() const;
};

static constexpr TraitReadByCodeParser traitsReadByCodeParser[] = {
    { "mayThrow"_s, &EffectTraits::mayThrow },
    { "mayThrowByRule"_s, &EffectTraits::mayThrowByRule },
    { "mayCollect"_s, &EffectTraits::mayCollect },
    { "mayCallUnknownCode"_s, &EffectTraits::mayCallUnknownCode },
    { "maySuspend"_s, &EffectTraits::maySuspend },
    { "mayBeTerminated"_s, &EffectTraits::mayBeTerminated },
    { "mayWriteObservableState"_s, &EffectTraits::mayWriteObservableState },
    { "mayTouchMemory"_s, &EffectTraits::mayTouchMemory },
    { "mayWriteOutsideObjects"_s, &EffectTraits::mayWriteOutsideObjects },
};

static uint16_t bitsOfTraitReadByCodeParser(const TraitReadByCodeParser& trait)
{
    uint16_t bits = 0;
    for (unsigned bit = 0; bit < EffectTraits::numberOfBits; ++bit) {
        auto single = static_cast<uint16_t>(1u << bit);
        if ((EffectTraits::fromBits(single).*trait.holds)())
            bits |= single;
    }
    return bits;
}

static ASCIILiteral spellingReadByCodeParser(FactKind kind)
{
    return nameOf(kind);
}

static ASCIILiteral spellingReadByCodeParser(FateOfPoll fate)
{
    switch (fate) {
    case FateOfPoll::LeavesHandler:
        return "LeavesHandler"_s;
    case FateOfPoll::IsDropped:
        return "IsDropped"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral spellingReadByCodeParser(FactSubject::Kind kind)
{
    switch (kind) {
    case FactSubject::Kind::Program:
        return "Program"_s;
    case FactSubject::Kind::Function:
        return "Function"_s;
    case FactSubject::Kind::EntryParameter:
        return "EntryParameter"_s;
    case FactSubject::Kind::Result:
        return "Result"_s;
    case FactSubject::Kind::Thrown:
        return "Thrown"_s;
    case FactSubject::Kind::Variable:
        return "Variable"_s;
    case FactSubject::Kind::Field:
        return "Field"_s;
    case FactSubject::Kind::Layout:
        return "Layout"_s;
    case FactSubject::Kind::Object:
        return "Object"_s;
    case FactSubject::Kind::Cell:
        return "Cell"_s;
    case FactSubject::Kind::Builtin:
        return "Builtin"_s;
    case FactSubject::Kind::Module:
        return "Module"_s;
    case FactSubject::Kind::InputOfOperation:
        return "InputOfOperation"_s;
    case FactSubject::Kind::Operation:
        return "Operation"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isIDOfBuiltinTableInCodeText(PayloadExternal external)
{
    switch (external) {
    case PayloadExternal::Builtin:
    case PayloadExternal::BuiltinClass:
    case PayloadExternal::BuiltinName:
    case PayloadExternal::RuntimeModule:
    case PayloadExternal::RuntimeFunction:
    case PayloadExternal::BuiltinOverload:
    case PayloadExternal::BuiltinLayout:
    case PayloadExternal::BuiltinSlot:
        return true;
    case PayloadExternal::None:
    case PayloadExternal::CheckedJSType:
    case PayloadExternal::LinkTimeConstant:
    case PayloadExternal::Feature:
    case PayloadExternal::TypedArrayType:
    case PayloadExternal::HandlerType:
    case PayloadExternal::IterationKind:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void fillWordsOfDefaultValueInCodeText(const PayloadFieldInfo& info, PayloadKind kind, std::span<uint32_t> words)
{
    switch (kind) {
    case PayloadKind::Record:
    case PayloadKind::Origin:
        for (const PayloadFieldInfo& part : info.parts())
            fillWordsOfDefaultValueInCodeText(part, part.kind, words.subspan(part.firstWord, part.numberOfWords));
        return;
    case PayloadKind::Variant:
    case PayloadKind::List:
    case PayloadKind::Int64:
    case PayloadKind::Double:
        std::ranges::fill(words, 0u);
        return;
    case PayloadKind::String:
    case PayloadKind::PropertyKey:
    case PayloadKind::Function:
    case PayloadKind::Layout:
    case PayloadKind::Variable:
    case PayloadKind::Module:
    case PayloadKind::ModuleFailure:
    case PayloadKind::Type:
    case PayloadKind::TypeSite:
    case PayloadKind::EvalScope:
    case PayloadKind::Data:
    case PayloadKind::ExpectedResult:
    case PayloadKind::Builtin:
    case PayloadKind::LayoutSet:
    case PayloadKind::CalleeSet:
        std::ranges::fill(words, absentPayloadWord);
        return;
    case PayloadKind::External:
        std::ranges::fill(words, info.isOptional || isIDOfBuiltinTableInCodeText(info.external) ? absentPayloadWord : 0u);
        return;
    case PayloadKind::Field:
    case PayloadKind::Enum:
    case PayloadKind::Flags:
    case PayloadKind::Bool:
    case PayloadKind::UInt32:
    case PayloadKind::OrderWord:
    case PayloadKind::Opcode:
        std::ranges::fill(words, info.isOptional ? absentPayloadWord : 0u);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const PayloadFieldInfo* findFieldInCodeText(std::span<const PayloadFieldInfo> fields, StringView name)
{
    for (const PayloadFieldInfo& field : fields) {
        if (name == field.name)
            return &field;
    }
    return nullptr;
}

class CodeTextParser {
    WTF_MAKE_NONCOPYABLE(CodeTextParser);
public:
    CodeTextParser(const CodeTextContext&, std::span<const uint8_t> text, Code&);

    String run();

private:
    bool hasFailed() const { return !m_error.isNull(); }
    void failAtLine(uint32_t line, String&& message);
    void fail(String&& message) { failAtLine(m_token.position.line, WTF::move(message)); }
    void fail(ASCIILiteral message) { fail(String(message)); }
    void failExpecting(ASCIILiteral what);
    String describeToken() const;

    void advance();
    void seek(const Lexer::State&);
    void skipLine();
    size_t offsetOfToken() const;
    void continueOnLineAt(size_t offset);

    bool at(TokenKind kind) const { return m_token.kind == kind; }
    bool atID(IDPrefix prefix) const { return at(TokenKind::ID) && m_token.prefix == prefix; }
    bool atWord(ASCIILiteral word) const { return m_token.isWord(word); }
    bool atName() const { return at(TokenKind::Word) || at(TokenKind::ID); }
    TokenKind kindOfNextToken() const;
    bool atField() const { return atName() && kindOfNextToken() == TokenKind::Equal; }
    bool accept(TokenKind);
    bool acceptWord(ASCIILiteral);
    bool acceptField(ASCIILiteral name);
    void expect(TokenKind, ASCIILiteral what);
    void expectEndOfLine() { expect(TokenKind::EndOfLine, "the end of the line"_s); }

    uint32_t parseUInt32(ASCIILiteral what, uint32_t maximum = UINT32_MAX);
    bool parseBool();
    template<typename E> E parseEnumerator(ASCIILiteral what);
    template<typename E> E parseSpelledEnumerator(E last, ASCIILiteral what);
    uint32_t parseNameAmong(std::span<const ASCIILiteral> names, ASCIILiteral what);
    uint32_t parseNamesAsBits(std::span<const ASCIILiteral> names, ASCIILiteral what);
    uint16_t parseTraits();
    Origin parseOrigin();
    TargetInCodeText parseTarget();

    TypeID parseTypeInText();
    LayoutSetID parseLayoutSetInText();
    StringID parseStringInText();
    PropertyKeyID parsePropertyKeyInText();
    std::optional<WellKnownSymbol> parseWellKnownSymbolInText();
    LayoutID parseLayoutInText();
    uint32_t parseProgramID(IDPrefix, size_t sizeOfTable, ASCIILiteral what);
    template<typename IDType> uint32_t parseTokenOfBuiltinTable(IDType (BuiltinTable::*fromToken)(StringView) const, ASCIILiteral what);
    uint32_t parseBuiltinOverloadInText();
    uint32_t parseRuntimeFunctionInText();
    uint32_t parseExternalInText(PayloadExternal);

    ValueID parseValueUse();
    Reference referenceTo(ValueID) const;
    void checkValue(uint32_t id, uint32_t line);
    void checkTarget(const TargetInCodeText&);
    void checkFrameOf(Origin, uint32_t line);
    void checkNumberGivenByCode(ValueID, const RecordInCodeText&);
    std::span<const ValueID> argumentsOf(const TargetInCodeText& target) const { return m_arguments.span().subspan(target.firstArgument, target.numberOfArguments); }
    ArgumentsOfEdge argumentsAsWrittenOf(const TargetInCodeText& target) const
    {
        ArgumentsOfEdge written;
        written.m_records.append(argumentsOf(target));
        return written;
    }

    template<typename Entry> void parseFieldsOf(DraftWords&, Entry, uint16_t* traitBits);
    template<typename Entry> void parsePositionalOperands(DraftWords&, Entry);
    void parsePayloadField(DraftWords&, const PayloadFieldInfo&);
    void parsePayloadValue(const PayloadFieldInfo&, PayloadKind, std::span<uint32_t> words);
    void parseRecordInText(std::span<const PayloadFieldInfo> parts, std::span<uint32_t> words);

    void scan();
    void scanUnit();
    void scanLine();
    void scanFrame();
    void scanHead();
    void scanParameter(uint32_t block);
    void scanTerminator();
    void scanRecord();
    void noteRecord(uint32_t id, uint32_t block, uint32_t parameter);
    void indexRecords();

    void build();
    bool namesFunctionOfUnit(const InlineFrame& frame) const { return !frame.calledBuiltin && (!frame.callee || frame.callee == m_unit.function); }
    void addFramesAndBlocks();
    void addFrame(const FrameInCodeText&);
    void addBlocks();
    void appendRecords();
    void appendParameter(const RecordInCodeText&);
    void appendOperation(const RecordInCodeText&);
    void endBlock(uint32_t block);
    void addEntries();
    void addFactUses();
    void addFatesOfPolls();

    const Program& m_program;
    TypeArena& m_types;
    Code& m_code;
    std::span<const uint8_t> m_text;
    Lexer m_lexer;
    Token m_token;
    Lexer::State m_stateBeforeToken;
    String m_error;

    CodeUnit m_unit;
    uint32_t m_numberOfFrameWords { 0 };
    bool m_holdsCallThatPlanHasNotCounted { false };
    bool m_callsOfFunctionNeverCollect { false };
    bool m_isAmongConstants { true };
    uint32_t m_lastRecord { absentInCodeText };
    Vector<RecordInCodeText> m_records;
    Vector<uint32_t> m_recordOfID;
    Vector<ParameterInCodeText> m_parameters;
    Vector<BlockInCodeText> m_blocks;
    Vector<ValueID> m_arguments;
    Vector<FrameInCodeText> m_frames;
    Vector<Lexer::State> m_pollLines;
    Vector<FactLineInCodeText> m_factLines;
};

CodeTextParser::CodeTextParser(const CodeTextContext& context, std::span<const uint8_t> text, Code& code)
    : m_program(context.program)
    , m_types(context.types)
    , m_code(code)
    , m_text(text)
    , m_lexer(text)
{
}

String CodeTextParser::run()
{
    if (m_text.size() >= std::numeric_limits<uint32_t>::max())
        return "line 1: the text is too long"_s;
    advance();
    scan();
    if (!hasFailed())
        indexRecords();
    if (!hasFailed())
        build();
    return m_error;
}

void CodeTextParser::failAtLine(uint32_t line, String&& message)
{
    if (hasFailed())
        return;
    m_error = makeString("line "_s, line, ": "_s, message);
    m_token = { };
}

String CodeTextParser::describeToken() const
{
    if (at(TokenKind::EndOfFile))
        return "the end of the text"_s;
    if (at(TokenKind::EndOfLine))
        return "the end of the line"_s;
    if (at(TokenKind::String))
        return makeString('"', m_token.view(), '"');
    return makeString('`', m_token.view(), '`');
}

void CodeTextParser::failExpecting(ASCIILiteral what)
{
    if (!hasFailed())
        fail(makeString("expected "_s, what, ", found "_s, describeToken()));
}

void CodeTextParser::advance()
{
    if (hasFailed())
        return;
    m_stateBeforeToken = m_lexer.state();
    m_token = m_lexer.next();
}

void CodeTextParser::seek(const Lexer::State& state)
{
    m_lexer.restore(state);
    advance();
}

void CodeTextParser::skipLine()
{
    while (!hasFailed() && !at(TokenKind::EndOfLine) && !at(TokenKind::EndOfFile))
        advance();
    accept(TokenKind::EndOfLine);
}

size_t CodeTextParser::offsetOfToken() const
{
    size_t offset = m_token.text.data() - m_text.data();
    return at(TokenKind::String) ? offset - 1 : offset;
}

void CodeTextParser::continueOnLineAt(size_t offset)
{
    Lexer::State state;
    state.offset = offset;
    state.position = m_token.position;
    state.position.column += static_cast<uint32_t>(offset - offsetOfToken());
    state.hasTokenOnLine = true;
    seek(state);
}

TokenKind CodeTextParser::kindOfNextToken() const
{
    Lexer lookahead = m_lexer;
    return lookahead.next().kind;
}

bool CodeTextParser::accept(TokenKind kind)
{
    if (hasFailed() || !at(kind))
        return false;
    advance();
    return true;
}

bool CodeTextParser::acceptWord(ASCIILiteral word)
{
    if (hasFailed() || !atWord(word))
        return false;
    advance();
    return true;
}

bool CodeTextParser::acceptField(ASCIILiteral name)
{
    if (hasFailed() || !atWord(name) || kindOfNextToken() != TokenKind::Equal)
        return false;
    advance();
    advance();
    return true;
}

void CodeTextParser::expect(TokenKind kind, ASCIILiteral what)
{
    if (!accept(kind))
        failExpecting(what);
}

uint32_t CodeTextParser::parseUInt32(ASCIILiteral what, uint32_t maximum)
{
    if (!at(TokenKind::Integer) || m_token.integer() < 0 || m_token.number > maximum) {
        failExpecting(what);
        return 0;
    }
    auto value = static_cast<uint32_t>(m_token.number);
    advance();
    return value;
}

bool CodeTextParser::parseBool()
{
    if (acceptWord("true"_s))
        return true;
    if (!acceptWord("false"_s))
        failExpecting("`true` or `false`"_s);
    return false;
}

template<typename E>
E CodeTextParser::parseEnumerator(ASCIILiteral what)
{
    std::optional<E> enumerator = atName() ? parseEnum<E>(m_token.view()) : std::nullopt;
    if (!enumerator) {
        failExpecting(what);
        return { };
    }
    advance();
    return *enumerator;
}

template<typename E>
E CodeTextParser::parseSpelledEnumerator(E last, ASCIILiteral what)
{
    if (atName()) {
        for (unsigned number = 0; number <= static_cast<unsigned>(last); ++number) {
            auto enumerator = static_cast<E>(number);
            if (m_token.view() == spellingReadByCodeParser(enumerator)) {
                advance();
                return enumerator;
            }
        }
    }
    failExpecting(what);
    return { };
}

uint32_t CodeTextParser::parseNameAmong(std::span<const ASCIILiteral> names, ASCIILiteral what)
{
    if (atName()) {
        for (uint32_t index = 0; index < names.size(); ++index) {
            if (m_token.view() == names[index]) {
                advance();
                return index;
            }
        }
    }
    failExpecting(what);
    return 0;
}

uint32_t CodeTextParser::parseNamesAsBits(std::span<const ASCIILiteral> names, ASCIILiteral what)
{
    uint32_t bits = 0;
    expect(TokenKind::LeftBracket, "`[`"_s);
    if (!at(TokenKind::RightBracket)) {
        do {
            bits |= 1u << parseNameAmong(names.first(std::min<size_t>(names.size(), 32)), what);
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RightBracket, "`]`"_s);
    return bits;
}

uint16_t CodeTextParser::parseTraits()
{
    uint16_t bits = 0;
    expect(TokenKind::LeftBracket, "`[`"_s);
    if (!at(TokenKind::RightBracket)) {
        do {
            const TraitReadByCodeParser* found = nullptr;
            for (const TraitReadByCodeParser& trait : traitsReadByCodeParser) {
                if (atWord(trait.name))
                    found = &trait;
            }
            if (!found) {
                failExpecting("a trait of effects"_s);
                return 0;
            }
            bits |= bitsOfTraitReadByCodeParser(*found);
            advance();
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RightBracket, "`]`"_s);
    return bits;
}

Origin CodeTextParser::parseOrigin()
{
    Origin origin;
    origin.position = Instruction::noPosition;
    origin.inlineFrame = 0;
    origin.isTailCall = false;
    origin.isPartOfConstruction = false;
    if (!accept(TokenKind::At))
        return origin;
    origin.position = parseUInt32("a position"_s);
    if (accept(TokenKind::Colon))
        origin.inlineFrame = parseUInt32("the number of a frame"_s);
    origin.isTailCall = acceptWord("tail"_s);
    origin.isPartOfConstruction = acceptWord("constructs"_s);
    return origin;
}

TargetInCodeText CodeTextParser::parseTarget()
{
    TargetInCodeText target;
    target.line = m_token.position.line;
    target.firstArgument = m_arguments.size();
    if (at(TokenKind::Word)) {
        target.name = m_token.view();
        advance();
    }
    if (accept(TokenKind::Minus)) {
        if (accept(TokenKind::LeftParenthesis))
            expect(TokenKind::RightParenthesis, "`)`"_s);
        return target;
    }
    if (!atID(IDPrefix::Block)) {
        failExpecting("a block"_s);
        return target;
    }
    target.block = static_cast<uint32_t>(m_token.number);
    advance();
    expect(TokenKind::LeftParenthesis, "`(`"_s);
    if (!at(TokenKind::RightParenthesis)) {
        do {
            if (!atID(IDPrefix::Value)) {
                failExpecting("a value"_s);
                return target;
            }
            m_arguments.append(ValueID(static_cast<uint32_t>(m_token.number)));
            advance();
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RightParenthesis, "`)`"_s);
    target.numberOfArguments = m_arguments.size() - target.firstArgument;
    return target;
}

TypeID CodeTextParser::parseTypeInText()
{
    if (hasFailed() || accept(TokenKind::Minus))
        return { };
    auto offset = static_cast<unsigned>(offsetOfToken());
    String error;
    std::optional<TypeID> type = parseType(m_types, StringView(byteCast<Latin1Character>(m_text)), offset, error);
    if (!type) {
        fail(makeString("expected a type: "_s, error));
        return { };
    }
    continueOnLineAt(offset);
    return *type;
}

LayoutSetID CodeTextParser::parseLayoutSetInText()
{
    if (hasFailed())
        return { };
    auto offset = static_cast<unsigned>(offsetOfToken());
    String error;
    std::optional<LayoutSetID> set = parseLayoutSet(m_types, StringView(byteCast<Latin1Character>(m_text)), offset, error);
    if (!set) {
        fail(makeString("expected a set of layouts: "_s, error));
        return { };
    }
    continueOnLineAt(offset);
    return *set;
}

StringID CodeTextParser::parseStringInText()
{
    if (accept(TokenKind::Minus))
        return { };
    bool isMade = acceptWord("made"_s);
    if (!at(TokenKind::String)) {
        failExpecting("a string"_s);
        return { };
    }
    Vector<uint8_t> bytes = decodeStringToken(m_token);
    if (!isCanonicalProgramString(bytes.span()))
        bytes = encodeProgramString(decodeProgramString(bytes.span()).span());
    if (isMade) {
        if (bytes.isEmpty()) {
            fail("no text that the walk makes is empty"_s);
            return { };
        }
        advance();
        return m_types.internText(bytes.span());
    }
    StringID id = m_program.strings.find(bytes.span());
    if (!id) {
        fail(makeString("the program has no string "_s, describeToken()));
        return { };
    }
    advance();
    return id;
}

std::optional<WellKnownSymbol> CodeTextParser::parseWellKnownSymbolInText()
{
    if (accept(TokenKind::Minus))
        return std::nullopt;
    if (at(TokenKind::Word)) {
        StringView word = m_token.view();
        for (WellKnownSymbol symbol : allMembersOf<WellKnownSymbol>) {
            StringView name = nameOf(symbol);
            if (word.length() != name.length() || word[0] != toASCIILower(name[0]) || word.substring(1) != name.substring(1))
                continue;
            advance();
            return symbol;
        }
    }
    failExpecting("a well-known symbol"_s);
    return std::nullopt;
}

PropertyKeyID CodeTextParser::parsePropertyKeyInText()
{
    if (accept(TokenKind::Minus))
        return { };

    PropertyKey key;
    if (at(TokenKind::String)) {
        key.kind = PropertyKeyKind::Text;
        key.text = parseStringInText();
    } else if (accept(TokenKind::At)) {
        key.kind = accept(TokenKind::NumberSign) ? PropertyKeyKind::ModulePrivateName : PropertyKeyKind::EnginePrivateName;
        key.text = parseStringInText();
    } else if (accept(TokenKind::NumberSign)) {
        key.kind = PropertyKeyKind::PrivateName;
        key.text = parseStringInText();
        expect(TokenKind::At, "`@`"_s);
        key.owner = parseLayoutInText();
    } else if (accept(TokenKind::AtAt)) {
        key.kind = PropertyKeyKind::WellKnownSymbol;
        key.symbol = parseWellKnownSymbolInText();
    } else if (acceptWord("symbol"_s)) {
        key.kind = PropertyKeyKind::CreatedSymbol;
        if (accept(TokenKind::LeftParenthesis)) {
            key.text = parseStringInText();
            expect(TokenKind::RightParenthesis, "`)`"_s);
        }
        expect(TokenKind::At, "`@`"_s);
        key.module = ModuleID(parseProgramID(IDPrefix::Module, m_program.modules.size(), "a module"_s));
        expect(TokenKind::Colon, "`:`"_s);
        if (!accept(TokenKind::Minus))
            key.callNumber = parseUInt32("the number of a call"_s, UINT32_MAX - 1);
    } else if (acceptWord("Symbol"_s)) {
        key.kind = PropertyKeyKind::RegisteredSymbol;
        expect(TokenKind::Dot, "`.`"_s);
        if (!acceptWord("for"_s))
            failExpecting("`for`"_s);
        expect(TokenKind::LeftParenthesis, "`(`"_s);
        key.text = parseStringInText();
        expect(TokenKind::RightParenthesis, "`)`"_s);
    } else
        failExpecting("a property key"_s);

    if (hasFailed())
        return { };
    return m_types.intern(key);
}

LayoutID CodeTextParser::parseLayoutInText()
{
    if (accept(TokenKind::Minus))
        return { };

    if (atID(IDPrefix::Layout)) {
        LayoutID numbered = m_program.layoutNumberedInText(static_cast<uint32_t>(m_token.number));
        if (!numbered) {
            fail(makeString("the program has no layout "_s, describeToken()));
            return { };
        }
        advance();
        return numbered;
    }

    if (!acceptWord("L"_s)) {
        failExpecting("a layout"_s);
        return { };
    }
    expect(TokenKind::LeftParenthesis, "`(`"_s);
    LayoutID layout;
    if (atID(IDPrefix::Module)) {
        ModuleID module(static_cast<uint32_t>(m_token.number));
        for (uint32_t index = 0; index < m_program.layouts.size() && !layout; ++index) {
            const Layout& candidate = m_program.layouts[index];
            if (candidate.kind == LayoutKind::ModuleNamespace && candidate.namespaceOf == module)
                layout = LayoutID(index);
        }
        if (!layout) {
            fail(makeString("the program has no layout of the namespace of "_s, describeToken()));
            return { };
        }
        advance();
    } else {
        uint32_t builtinLayout = parseTokenOfBuiltinTable(&BuiltinTable::layoutFromToken, "a layout of the table of built-ins"_s);
        if (hasFailed() || builtinLayout == absentPayloadWord) {
            failExpecting("a layout of the table of built-ins"_s);
            return { };
        }
        layout = m_program.layoutOf(BuiltinLayoutID(builtinLayout));
    }
    expect(TokenKind::RightParenthesis, "`)`"_s);
    return layout;
}

uint32_t CodeTextParser::parseProgramID(IDPrefix prefix, size_t sizeOfTable, ASCIILiteral what)
{
    if (accept(TokenKind::Minus))
        return absentPayloadWord;
    if (!atID(prefix)) {
        failExpecting(what);
        return absentPayloadWord;
    }
    if (m_token.number >= sizeOfTable) {
        fail(makeString("the program has no "_s, describeToken(), ": it has "_s, sizeOfTable, " of the kind"_s));
        return absentPayloadWord;
    }
    auto index = static_cast<uint32_t>(m_token.number);
    advance();
    return index;
}

template<typename IDType>
uint32_t CodeTextParser::parseTokenOfBuiltinTable(IDType (BuiltinTable::*fromToken)(StringView) const, ASCIILiteral what)
{
    if (accept(TokenKind::Minus))
        return absentPayloadWord;
    IDType id = atName() ? (m_program.builtins().*fromToken)(m_token.view()) : IDType();
    if (!id) {
        failExpecting(what);
        return absentPayloadWord;
    }
    advance();
    return static_cast<uint32_t>(id.index());
}

uint32_t CodeTextParser::parseBuiltinOverloadInText()
{
    if (accept(TokenKind::Minus))
        return absentPayloadWord;
    if (!atName()) {
        failExpecting("an overload of a built-in"_s);
        return absentPayloadWord;
    }
    uint32_t line = m_token.position.line;
    StringView builtin = m_token.view();
    advance();
    expect(TokenKind::Slash, "`/`"_s);
    uint32_t ordinal = parseUInt32("the number of an overload"_s);
    if (hasFailed())
        return absentPayloadWord;
    BuiltinOverloadID id = m_program.builtins().overloadFromToken(makeString(builtin, '/', ordinal));
    if (!id) {
        failAtLine(line, makeString("the table of built-ins has no overload `"_s, builtin, '/', ordinal, '`'));
        return absentPayloadWord;
    }
    return static_cast<uint32_t>(id.index());
}

uint32_t CodeTextParser::parseRuntimeFunctionInText()
{
    if (accept(TokenKind::Minus))
        return absentPayloadWord;
    RuntimeFunctionID id = atName() ? runtimeFunctionFromToken(m_program.builtins(), m_token.view()) : RuntimeFunctionID();
    if (!id) {
        failExpecting("a function of the run time"_s);
        return absentPayloadWord;
    }
    advance();
    return static_cast<uint32_t>(id.index());
}

uint32_t CodeTextParser::parseExternalInText(PayloadExternal external)
{
    switch (external) {
    case PayloadExternal::Builtin:
        return parseTokenOfBuiltinTable(&BuiltinTable::builtinFromToken, "a built-in"_s);
    case PayloadExternal::BuiltinClass:
        return parseTokenOfBuiltinTable(&BuiltinTable::classFromToken, "a class of built-ins"_s);
    case PayloadExternal::BuiltinLayout:
        return parseTokenOfBuiltinTable(&BuiltinTable::layoutFromToken, "a layout of the table of built-ins"_s);
    case PayloadExternal::BuiltinSlot:
        return parseTokenOfBuiltinTable(&BuiltinTable::slotFromToken, "a slot of a built-in"_s);
    case PayloadExternal::RuntimeModule:
        return parseTokenOfBuiltinTable(&BuiltinTable::moduleFromToken, "a module of the run time"_s);
    case PayloadExternal::RuntimeFunction:
        return parseRuntimeFunctionInText();
    case PayloadExternal::BuiltinOverload:
        return parseBuiltinOverloadInText();
    case PayloadExternal::BuiltinName:
        if (accept(TokenKind::Minus))
            return absentPayloadWord;
        return parseUInt32("the number of a name"_s);
    case PayloadExternal::CheckedJSType:
    case PayloadExternal::LinkTimeConstant:
    case PayloadExternal::Feature:
    case PayloadExternal::TypedArrayType:
    case PayloadExternal::HandlerType:
    case PayloadExternal::IterationKind:
        return parseUInt32("a number"_s);
    case PayloadExternal::None:
        fail("the tables of the schema do not say which external the field holds"_s);
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID CodeTextParser::parseValueUse()
{
    if (!atID(IDPrefix::Value)) {
        failExpecting("a value"_s);
        return { };
    }
    auto id = static_cast<uint32_t>(m_token.number);
    checkValue(id, m_token.position.line);
    if (id >= m_code.numberOfValues())
        fail(makeString('v', id, " is used before it is appended: an operand has a smaller number than its user"_s));
    if (hasFailed())
        return { };
    advance();
    return ValueID(id);
}

Reference CodeTextParser::referenceTo(ValueID id) const
{
    if (!id)
        return { };
    bool isConstant = m_records[m_recordOfID[id.index()]].block == absentInCodeText;
    return isConstant ? Reference::constant(id) : Reference::value(id);
}

void CodeTextParser::checkValue(uint32_t id, uint32_t line)
{
    if (id >= m_records.size())
        failAtLine(line, makeString('v', id, " is defined nowhere"_s));
}

void CodeTextParser::checkTarget(const TargetInCodeText& target)
{
    if (target.block != absentInCodeText && target.block >= m_blocks.size())
        failAtLine(target.line, makeString('b', target.block, " is defined nowhere"_s));
    for (ValueID argument : argumentsOf(target))
        checkValue(argument.index(), target.line);
}

void CodeTextParser::checkFrameOf(Origin origin, uint32_t line)
{
    if (origin.inlineFrame && origin.inlineFrame >= m_code.inlineFrames().size())
        failAtLine(line, makeString("frame "_s, origin.inlineFrame, " is defined nowhere"_s));
}

void CodeTextParser::checkNumberGivenByCode(ValueID given, const RecordInCodeText& record)
{
    if (given.index() != record.id)
        failAtLine(record.line, makeString('v', record.id, " became v"_s, given.index(), ": Code held records before the first was appended"_s));
}

template<typename Entry>
void CodeTextParser::parsePositionalOperands(DraftWords& draft, Entry entry)
{
    if (!atID(IDPrefix::Value) && !at(TokenKind::Minus))
        return;

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(entry);
    uint32_t operandIndex = 0;
    do {
        Reference reference;
        if (!accept(TokenKind::Minus))
            reference = referenceTo(parseValueUse());
        if (hasFailed())
            return;
        while (operandIndex < numberOfNamedOperands && operandInfoOf(entry, operandIndex).isEngineOnly)
            ++operandIndex;
        if (operandIndex < numberOfNamedOperands)
            draft.setOperand(operandIndex++, reference);
        else if (hasVariadicOperand(entry))
            draft.appendVariadicOperand(reference);
        else {
            fail(makeString(nameOf(entry), " takes fewer operands"_s));
            return;
        }
    } while (accept(TokenKind::Comma));
}

template<typename Entry>
void CodeTextParser::parseFieldsOf(DraftWords& draft, Entry entry, uint16_t* traitBits)
{
    parsePositionalOperands(draft, entry);

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(entry);
    std::span<const PayloadFieldInfo> fields = payloadFieldsOf(entry);
    uint64_t fieldsRead = 0;
    while (!hasFailed()) {
        if (at(TokenKind::LeftBracket)) {
            draft.flags = static_cast<uint8_t>(draft.flags | parseNamesAsBits(flagNamesOf(entry), "a flag of the entry"_s));
            continue;
        }
        if (!atField())
            break;

        StringView name = m_token.view();
        if (traitBits && name == "traits"_s) {
            advance();
            advance();
            *traitBits |= parseTraits();
            continue;
        }
        if (const PayloadFieldInfo* field = findFieldInCodeText(fields, name)) {
            uint64_t bit = uint64_t { 1 } << ((field - fields.data()) & 63);
            if (fieldsRead & bit) {
                fail(makeString("the field `"_s, name, "` is written twice"_s));
                return;
            }
            fieldsRead |= bit;
            advance();
            advance();
            parsePayloadField(draft, *field);
            continue;
        }

        uint32_t operandIndex = 0;
        while (operandIndex < numberOfNamedOperands && !(operandInfoOf(entry, operandIndex).isEngineOnly && name == operandInfoOf(entry, operandIndex).name))
            ++operandIndex;
        if (operandIndex == numberOfNamedOperands) {
            fail(makeString(nameOf(entry), " has no field `"_s, name, '`'));
            return;
        }
        if (draft.operand(operandIndex)) {
            fail(makeString("the operand `"_s, name, "` is written twice"_s));
            return;
        }
        advance();
        advance();
        draft.setOperand(operandIndex, referenceTo(parseValueUse()));
    }

    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperands && !hasFailed(); ++operandIndex) {
        const OperandInfo& info = operandInfoOf(entry, operandIndex);
        if (!info.isOptional && !draft.operand(operandIndex))
            fail(makeString(nameOf(entry), " lacks its operand `"_s, info.name, '`'));
    }
}

void CodeTextParser::parsePayloadField(DraftWords& draft, const PayloadFieldInfo& info)
{
    uint32_t numberOfWords = info.numberOfWordsOfValue;
    if (!numberOfWords) {
        fail(makeString("the tables of the schema do not say how many words a value of `"_s, info.name, "` has"_s));
        return;
    }

    Vector<uint32_t, 16> words;
    if (info.kind != PayloadKind::List) {
        for (uint32_t word = 0; word < numberOfWords; ++word)
            words.append(draft.payloadWordAt(info.firstWord + word));
        parsePayloadValue(info, info.kind, words.mutableSpan());
        if (hasFailed())
            return;
        for (uint32_t word = 0; word < numberOfWords; ++word)
            draft.setPayloadWordAt(info.firstWord + word, words[word]);
        return;
    }

    expect(TokenKind::LeftBracket, "`[`"_s);
    if (!at(TokenKind::RightBracket)) {
        do {
            size_t first = words.size();
            for (uint32_t word = 0; word < numberOfWords; ++word)
                words.append(0u);
            std::span<uint32_t> wordsOfElement = words.mutableSpan().subspan(first);
            fillWordsOfDefaultValueInCodeText(info, info.elementKind, wordsOfElement);
            parsePayloadValue(info, info.elementKind, wordsOfElement);
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RightBracket, "`]`"_s);
    if (!hasFailed())
        draft.setPayloadListFrom(info.firstWord, words.span());
}

void CodeTextParser::parseRecordInText(std::span<const PayloadFieldInfo> parts, std::span<uint32_t> words)
{
    expect(TokenKind::LeftBrace, "`{`"_s);
    while (!hasFailed() && !at(TokenKind::RightBrace)) {
        const PayloadFieldInfo* part = atField() ? findFieldInCodeText(parts, m_token.view()) : nullptr;
        if (!part) {
            failExpecting("a field of the record or `}`"_s);
            return;
        }
        advance();
        advance();
        parsePayloadValue(*part, part->kind, words.subspan(part->firstWord, part->numberOfWords));
    }
    expect(TokenKind::RightBrace, "`}`"_s);
}

void CodeTextParser::parsePayloadValue(const PayloadFieldInfo& info, PayloadKind kind, std::span<uint32_t> words)
{
    if (hasFailed())
        return;

    auto setWordPair = [&](uint64_t bits) {
        words[0] = static_cast<uint32_t>(bits);
        words[1] = static_cast<uint32_t>(bits >> 32);
    };

    switch (kind) {
    case PayloadKind::String:
        words[0] = parseStringInText().index();
        return;
    case PayloadKind::PropertyKey:
        words[0] = parsePropertyKeyInText().index();
        return;
    case PayloadKind::Function:
        words[0] = parseProgramID(IDPrefix::Function, m_program.functions.size(), "a function"_s);
        return;
    case PayloadKind::Layout:
        words[0] = parseLayoutInText().index();
        return;
    case PayloadKind::Variable:
        words[0] = parseProgramID(IDPrefix::Variable, m_program.variables.size(), "a variable"_s);
        return;
    case PayloadKind::Module:
        words[0] = parseProgramID(IDPrefix::Module, m_program.modules.size(), "a module"_s);
        return;
    case PayloadKind::ModuleFailure:
        words[0] = parseProgramID(IDPrefix::ModuleFailure, m_program.moduleFailures.size(), "a failure of a module"_s);
        return;
    case PayloadKind::Type:
        words[0] = parseTypeInText().index();
        return;
    case PayloadKind::TypeSite:
        words[0] = parseProgramID(IDPrefix::TypeSite, m_program.typeSites.size(), "a site of a type"_s);
        return;
    case PayloadKind::EvalScope:
        words[0] = parseProgramID(IDPrefix::EvalScope, m_program.evalScopes.size(), "a scope of an eval"_s);
        return;
    case PayloadKind::Data:
        words[0] = parseProgramID(IDPrefix::Data, m_program.data.size(), "data"_s);
        return;
    case PayloadKind::ExpectedResult:
        words[0] = parseProgramID(IDPrefix::ExpectedResult, m_program.expectedResults.size(), "an expected result"_s);
        return;
    case PayloadKind::Builtin:
        words[0] = parseExternalInText(PayloadExternal::Builtin);
        return;
    case PayloadKind::External:
        words[0] = parseExternalInText(info.external);
        return;
    case PayloadKind::LayoutSet:
        words[0] = parseLayoutSetInText().index();
        return;
    case PayloadKind::Enum:
        words[0] = parseNameAmong(info.memberNames, "an enumerator"_s);
        return;
    case PayloadKind::Flags:
        words[0] = parseNamesAsBits(info.memberNames, "a flag"_s);
        return;
    case PayloadKind::Opcode:
        words[0] = static_cast<uint32_t>(parseEnumerator<Opcode>("an opcode"_s));
        return;
    case PayloadKind::Bool:
        words[0] = parseBool();
        return;
    case PayloadKind::CalleeSet:
        words[0] = accept(TokenKind::Minus) ? absentPayloadWord : parseUInt32("a number"_s);
        return;
    case PayloadKind::Field:
    case PayloadKind::UInt32:
    case PayloadKind::OrderWord:
        words[0] = parseUInt32("a number"_s);
        return;
    case PayloadKind::Int64:
        if (!at(TokenKind::Integer)) {
            failExpecting("an integer"_s);
            return;
        }
        setWordPair(m_token.number);
        advance();
        return;
    case PayloadKind::Double: {
        double value = 0;
        if (at(TokenKind::Integer))
            value = static_cast<double>(m_token.integer());
        else if (at(TokenKind::Decimal))
            value = m_token.decimal();
        else if (atWord("NaN"_s))
            value = PNaN;
        else if (atWord("Infinity"_s))
            value = std::numeric_limits<double>::infinity();
        else {
            failExpecting("a number"_s);
            return;
        }
        setWordPair(std::bit_cast<uint64_t>(value));
        advance();
        return;
    }
    case PayloadKind::Record:
    case PayloadKind::Origin:
        parseRecordInText(info.parts(), words);
        return;
    case PayloadKind::Variant: {
        const PayloadFieldInfo* chosen = atName() ? findFieldInCodeText(info.parts(), m_token.view()) : nullptr;
        if (!chosen) {
            failExpecting("a case of the variant"_s);
            return;
        }
        advance();
        std::ranges::fill(words, 0u);
        words[0] = static_cast<uint32_t>(chosen - info.firstPart);
        std::span<uint32_t> wordsOfCase = words.subspan(chosen->firstWord, chosen->numberOfWords);
        fillWordsOfDefaultValueInCodeText(*chosen, PayloadKind::Record, wordsOfCase);
        if (chosen->numberOfParts)
            parseRecordInText(chosen->parts(), wordsOfCase);
        return;
    }
    case PayloadKind::List:
        fail("a list stands inside a value"_s);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CodeTextParser::scan()
{
    if (!acceptWord("code"_s))
        failExpecting("`code`"_s);
    scanUnit();
    if (acceptField("frameWords"_s))
        m_numberOfFrameWords = parseUInt32("a number of words"_s);
    if (acceptField("holdsCallThatPlanHasNotCounted"_s))
        m_holdsCallThatPlanHasNotCounted = parseBool();
    if (acceptField("callsOfFunctionNeverCollect"_s))
        m_callsOfFunctionNeverCollect = parseBool();
    expect(TokenKind::LeftBrace, "`{`"_s);
    expectEndOfLine();

    while (!hasFailed() && !at(TokenKind::RightBrace))
        scanLine();
    expect(TokenKind::RightBrace, "`}`"_s);
    expectEndOfLine();
    if (!at(TokenKind::EndOfFile))
        failExpecting("the end of the text"_s);
}

void CodeTextParser::scanUnit()
{
    uint32_t function = parseProgramID(IDPrefix::Function, m_program.functions.size(), "a function"_s);
    if (!hasFailed() && function == absentPayloadWord)
        fail("the unit names no function"_s);
    CodeRole role = CodeRole::Body;
    if (accept(TokenKind::Colon))
        role = parseEnumerator<CodeRole>("a role of code"_s);
    if (!hasFailed())
        m_unit = CodeUnit(FunctionID(function), role);
}

void CodeTextParser::scanLine()
{
    if (atWord("frame"_s)) {
        scanFrame();
        return;
    }
    if (atWord("fact"_s)) {
        m_factLines.append({ m_stateBeforeToken, m_lastRecord });
        skipLine();
        return;
    }
    if (atWord("poll"_s)) {
        m_pollLines.append(m_stateBeforeToken);
        skipLine();
        return;
    }
    if (acceptWord("constants"_s)) {
        expect(TokenKind::Colon, "`:`"_s);
        expectEndOfLine();
        m_isAmongConstants = true;
        return;
    }
    if (atID(IDPrefix::Block)) {
        scanHead();
        return;
    }
    if (at(TokenKind::Word) && parseEnum<TerminatorKind>(m_token.view())) {
        scanTerminator();
        return;
    }
    scanRecord();
}

void CodeTextParser::scanFrame()
{
    FrameInCodeText scanned;
    scanned.line = m_token.position.line;
    advance();
    scanned.number = parseUInt32("the number of a frame"_s, endOfFrameNumbersInCodeText - 1);

    InlineFrame& frame = scanned.frame;
    while (!hasFailed() && !at(TokenKind::EndOfLine)) {
        if (acceptField("callee"_s))
            frame.callee = FunctionID(parseProgramID(IDPrefix::Function, m_program.functions.size(), "a function"_s));
        else if (acceptField("callPosition"_s))
            frame.callPosition = parseUInt32("a position"_s);
        else if (acceptField("parent"_s))
            frame.parent = parseUInt32("the number of a frame"_s);
        else if (acceptField("isTailCall"_s))
            frame.isTailCall = parseBool();
        else if (acceptField("isConstruction"_s))
            frame.isConstruction = parseBool();
        else if (acceptField("callIsPartOfConstruction"_s))
            frame.callIsPartOfConstruction = parseBool();
        else if (acceptField("module"_s))
            frame.module = ModuleID(parseProgramID(IDPrefix::Module, m_program.modules.size(), "a module"_s));
        else if (acceptField("calledBuiltin"_s))
            frame.calledBuiltin = BuiltinID(parseExternalInText(PayloadExternal::Builtin));
        else
            failExpecting("a member of a frame"_s);
    }
    expectEndOfLine();
    m_frames.append(scanned);
}

void CodeTextParser::scanHead()
{
    auto number = static_cast<uint32_t>(m_token.number);
    if (number != m_blocks.size()) {
        fail(makeString('b', number, " stands where b"_s, m_blocks.size(), " is expected: blocks stand in the order of their numbers"_s));
        return;
    }
    if (number >= Operation::noBlock) {
        fail("Code has no room for so many blocks"_s);
        return;
    }

    BlockInCodeText block;
    block.line = m_token.position.line;
    block.firstParameter = m_parameters.size();
    advance();
    expect(TokenKind::LeftParenthesis, "`(`"_s);
    if (!at(TokenKind::RightParenthesis)) {
        do {
            scanParameter(number);
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RightParenthesis, "`)`"_s);
    block.numberOfParameters = m_parameters.size() - block.firstParameter;
    block.numberOfParametersToAppend = block.numberOfParameters;

    while (!hasFailed() && !at(TokenKind::Colon)) {
        if (acceptField("role"_s))
            block.role = parseEnumerator<BlockRole>("a role of blocks"_s);
        else if (acceptField("subject"_s)) {
            if (!atID(IDPrefix::Value)) {
                failExpecting("a value"_s);
                return;
            }
            block.subject = static_cast<uint32_t>(m_token.number);
            advance();
        } else if (acceptField("frequency"_s))
            block.frequency = parseEnumerator<BlockFrequency>("a frequency of blocks"_s);
        else if (acceptField("isInLoop"_s))
            block.isInLoop = parseBool();
        else if (acceptField("isLoopHead"_s))
            block.isLoopHead = parseBool();
        else if (acceptField("keepsSitesThoughRare"_s))
            block.keepsSitesThoughRare = parseBool();
        else if (acceptField("entry"_s)) {
            if (block.entry != absentInCodeText)
                fail(makeString('b', number, " is two entries, and Code takes a block for one at the most"_s));
            block.entry = parseUInt32("the number of an entry"_s, UINT16_MAX);
        } else if (acceptField("handler"_s)) {
            block.hasHandler = true;
            block.handler = parseTarget();
        } else
            failExpecting("a field of a block or `:`"_s);
    }
    expect(TokenKind::Colon, "`:`"_s);
    expectEndOfLine();
    m_blocks.append(block);
    m_isAmongConstants = false;
}

void CodeTextParser::scanParameter(uint32_t block)
{
    if (!atID(IDPrefix::Value)) {
        failExpecting("a parameter"_s);
        return;
    }
    ParameterInCodeText parameter;
    parameter.id = static_cast<uint32_t>(m_token.number);
    noteRecord(parameter.id, block, m_parameters.size());
    advance();
    expect(TokenKind::Colon, "`:` and the type of the parameter"_s);
    parameter.type = parseTypeInText();
    if (!at(TokenKind::At) || kindOfNextToken() == TokenKind::Integer) {
        failExpecting("`@` and the representation of the parameter"_s);
        return;
    }
    advance();
    parameter.representation = parseEnumerator<Representation>("a representation"_s);
    parameter.origin = parseOrigin();
    m_parameters.append(parameter);
}

void CodeTextParser::scanTerminator()
{
    if (m_isAmongConstants) {
        fail("a terminator stands in no block"_s);
        return;
    }
    BlockInCodeText& block = m_blocks.last();
    if (block.hasTerminator) {
        fail(makeString('b', m_blocks.size() - 1, " has ended already"_s));
        return;
    }
    block.hasTerminator = true;
    block.terminator = m_stateBeforeToken;
    skipLine();
}

void CodeTextParser::scanRecord()
{
    if (!atID(IDPrefix::Value)) {
        failExpecting("a line of Code or `}`"_s);
        return;
    }
    auto id = static_cast<uint32_t>(m_token.number);

    uint32_t blockIndex = absentInCodeText;
    if (!m_isAmongConstants) {
        blockIndex = m_blocks.size() - 1;
        BlockInCodeText& block = m_blocks.last();
        if (block.hasTerminator) {
            fail(makeString('v', id, " stands behind the terminator of b"_s, blockIndex));
            return;
        }
        if (block.hasOperations && id <= block.lastOperation) {
            fail(makeString('v', id, " stands behind v"_s, block.lastOperation, ": the numbers of a block ascend"_s));
            return;
        }
        block.hasOperations = true;
        block.lastOperation = id;
    }
    noteRecord(id, blockIndex, absentInCodeText);
    skipLine();
}

void CodeTextParser::noteRecord(uint32_t id, uint32_t block, uint32_t parameter)
{
    m_records.append({ m_stateBeforeToken, m_token.position.line, id, block, parameter });
    m_lastRecord = id;
}

void CodeTextParser::indexRecords()
{
    m_recordOfID.fill(absentInCodeText, m_records.size());
    for (uint32_t index = 0; index < m_records.size(); ++index) {
        const RecordInCodeText& record = m_records[index];
        if (record.id >= m_records.size()) {
            failAtLine(record.line, makeString('v', record.id, " is out of range: the text has "_s, m_records.size(), " records, which are numbered from v0 without a gap"_s));
            return;
        }
        if (m_recordOfID[record.id] != absentInCodeText) {
            failAtLine(record.line, makeString('v', record.id, " is defined twice"_s));
            return;
        }
        m_recordOfID[record.id] = index;
    }

    for (const BlockInCodeText& block : m_blocks) {
        if (block.subject != absentInCodeText)
            checkValue(block.subject, block.line);
        if (block.hasHandler)
            checkTarget(block.handler);
    }
}

void CodeTextParser::build()
{
    m_code.reset(m_unit);
    m_code.reserveFrameWords(m_numberOfFrameWords);
    if (m_holdsCallThatPlanHasNotCounted)
        m_code.noteCallThatPlanHasNotCounted();
    if (m_callsOfFunctionNeverCollect)
        m_code.noteThatCallsOfFunctionNeverCollect();
    addFramesAndBlocks();
    appendRecords();
    for (uint32_t block = 0; block < m_blocks.size() && !hasFailed(); ++block) {
        if (m_blocks[block].hasTerminator)
            endBlock(block);
    }
    if (hasFailed())
        return;

    for (uint32_t index = 0; index < m_blocks.size(); ++index) {
        const BlockInCodeText& block = m_blocks[index];
        if (block.hasHandler && block.handler.block != absentInCodeText)
            m_code.closeEdge(block.handlerEdge, BlockID(block.handler.block), argumentsAsWrittenOf(block.handler));
        if (block.subject != absentInCodeText)
            m_code.setSubject(BlockID(index), ValueID(block.subject));
    }
    addEntries();
    addFactUses();
    addFatesOfPolls();
}

void CodeTextParser::addFramesAndBlocks()
{
    std::span<const FrameInCodeText> frames = m_frames.span();
    if (!frames.empty() && !frames.front().number) {
        const FrameInCodeText& first = frames.front();
        if (!namesFunctionOfUnit(first.frame) || first.frame.parent)
            failAtLine(first.line, "frame 0 is the function of the unit itself: it has no other callee, no built-in and no parent"_s);
        addFrame(first);
        frames = frames.subspan(1);
    }
    if (hasFailed())
        return;

    addBlocks();
    for (const FrameInCodeText& scanned : frames) {
        if (hasFailed())
            return;
        uint32_t number = scanned.number;
        if (number != m_code.inlineFrames().size())
            failAtLine(scanned.line, makeString("frame "_s, number, " stands where frame "_s, m_code.inlineFrames().size(), " is expected"_s));
        else if (scanned.frame.parent >= number)
            failAtLine(scanned.line, makeString("frame "_s, number, " lies in frame "_s, scanned.frame.parent, ", which does not stand in front of it"_s));
        else if (namesFunctionOfUnit(scanned.frame) && walksInputBody(m_unit.role))
            failAtLine(scanned.line, makeString("frame "_s, number, " names the function of the unit, which is never walked at a call in its own body"_s));
        addFrame(scanned);
    }
}

void CodeTextParser::addFrame(const FrameInCodeText& scanned)
{
    if (hasFailed())
        return;
    uint32_t given = m_code.addInlineFrame(scanned.frame);
    if (given != scanned.number)
        failAtLine(scanned.line, makeString("frame "_s, scanned.number, " became frame "_s, given, ": it names the function of the unit, and the text has no block"_s));
}

void CodeTextParser::addBlocks()
{
    for (uint32_t index = 0; index < m_blocks.size(); ++index) {
        BlockInCodeText& block = m_blocks[index];
        BlockID id = m_code.addBlock(block.role, block.frequency);
        if (id.index() != index) {
            failAtLine(block.line, makeString('b', index, " became b"_s, id.index(), ": Code held blocks before the first was added"_s));
            return;
        }
        if (block.isInLoop || block.isLoopHead)
            m_code.setLoopMarks(id, block.isInLoop, block.isLoopHead);
        if (block.keepsSitesThoughRare)
            m_code.markRareBlockThatKeepsSites(id);
        if (block.hasHandler)
            block.handlerEdge = m_code.addHandlerEdge(id);
    }
}

void CodeTextParser::appendRecords()
{
    for (uint32_t id = 0; id < m_records.size() && !hasFailed(); ++id) {
        const RecordInCodeText& record = m_records[m_recordOfID[id]];
        if (record.parameter != absentInCodeText)
            appendParameter(record);
        else
            appendOperation(record);
    }
}

void CodeTextParser::appendParameter(const RecordInCodeText& record)
{
    const ParameterInCodeText& parameter = m_parameters[record.parameter];
    BlockInCodeText& block = m_blocks[record.block];
    if (!parameter.type)
        failAtLine(record.line, makeString('v', record.id, " has no type"_s));
    if (parameter.representation == Representation::Unchosen)
        failAtLine(record.line, makeString('v', record.id, " has no representation"_s));
    checkFrameOf(parameter.origin, record.line);
    if (hasFailed())
        return;

    checkNumberGivenByCode(m_code.appendParameter(BlockID(record.block), parameter.type, parameter.representation, parameter.origin), record);
    if (hasFailed() || --block.numberOfParametersToAppend)
        return;

    Vector<ValueID, 8> parameters;
    for (const ParameterInCodeText& parameterOfBlock : m_parameters.span().subspan(block.firstParameter, block.numberOfParameters))
        parameters.append(ValueID(parameterOfBlock.id));
    m_code.setParameters(BlockID(record.block), parameters.span());
}

void CodeTextParser::appendOperation(const RecordInCodeText& record)
{
    seek(record.start);
    advance();

    TypeID type;
    Representation representation = Representation::Unchosen;
    if (accept(TokenKind::Colon))
        type = parseTypeInText();
    if (accept(TokenKind::At))
        representation = parseEnumerator<Representation>("a representation"_s);
    expect(TokenKind::Equal, "`=`"_s);
    Opcode opcode = parseEnumerator<Opcode>("an opcode"_s);
    if (hasFailed())
        return;

    if (opcode == Opcode::Parameter)
        failAtLine(record.line, "a parameter stands in the head of its block"_s);
    else if (opcodesThatStateTypes.contains(opcode))
        failAtLine(record.line, makeString(nameOf(opcode), " states a type, and Code holds no such operation"_s));
    else if (opcodesWithoutEmitter().contains(opcode))
        failAtLine(record.line, makeString(nameOf(opcode), " has no emitter, and Code holds no such operation"_s));
    else if (representation == Representation::Unchosen)
        failAtLine(record.line, makeString('v', record.id, " has no representation"_s));
    if (hasFailed())
        return;

    bool isConstant = record.block == absentInCodeText;
    Draft draft(opcode);
    uint16_t traitBits = 0;
    parseFieldsOf(draft, opcode, isConstant ? nullptr : &traitBits);
    draft.setOrigin(parseOrigin());
    expectEndOfLine();
    checkFrameOf(draft.origin(), record.line);
    if (hasFailed())
        return;
    draft.representation = representation;

    if (isConstant) {
        for (Reference operand : draft.operands()) {
            if (operand && !operand.isConstant()) {
                failAtLine(record.line, makeString("the constant v"_s, record.id, " has the operand v"_s, operand.id().index(), ", which is no constant"_s));
                return;
            }
        }
        checkNumberGivenByCode(m_code.addConstant(draft, type), record);
        return;
    }
    if (m_blocks[record.block].numberOfParametersToAppend) {
        failAtLine(record.line, makeString('v', record.id, " has a smaller number than a parameter of its block b"_s, record.block));
        return;
    }
    checkNumberGivenByCode(m_code.append(BlockID(record.block), draft, type, EffectTraits::fromBits(traitBits)), record);
}

void CodeTextParser::endBlock(uint32_t blockIndex)
{
    seek(m_blocks[blockIndex].terminator);
    uint32_t line = m_token.position.line;
    TerminatorKind kind = parseEnumerator<TerminatorKind>("a terminator"_s);
    if (hasFailed())
        return;
    if (!mayStandInCode(kind)) {
        failAtLine(line, makeString("the schema keeps "_s, nameOf(kind), " out of Code"_s));
        return;
    }

    TerminatorDraft draft(kind);
    parseFieldsOf(draft, kind, nullptr);

    size_t numberOfArgumentsInFront = m_arguments.size();
    Vector<TargetInCodeText, 4> targets;
    if (accept(TokenKind::Arrow)) {
        do {
            targets.append(parseTarget());
        } while (accept(TokenKind::Comma));
    }
    draft.setOrigin(parseOrigin());
    expectEndOfLine();
    checkFrameOf(draft.origin(), line);
    if (hasFailed())
        return;

    uint32_t numberOfTargets = targets.size();
    std::optional<uint32_t> numberOfFixedTargets = numberOfFixedTargetsOf(kind);
    if (numberOfFixedTargets ? *numberOfFixedTargets != numberOfTargets : !numberOfTargets) {
        failAtLine(line, makeString(nameOf(kind), " does not have "_s, numberOfTargets, " targets"_s));
        return;
    }
    for (uint32_t index = 0; index < numberOfTargets; ++index) {
        const TargetInCodeText& target = targets[index];
        const TargetInfo* info = infoOfTarget(kind, index, numberOfTargets);
        if (!info) {
            failAtLine(target.line, makeString(nameOf(kind), " has no target "_s, index));
            return;
        }
        if (!target.name.isNull() && target.name != info->name) {
            failAtLine(target.line, makeString("target "_s, index, " of "_s, nameOf(kind), " is `"_s, info->name, "`, not `"_s, target.name, '`'));
            return;
        }
        checkTarget(target);
    }
    if (hasFailed())
        return;

    BlockID block(blockIndex);
    m_code.end(block, draft, numberOfTargets);
    const Terminator& terminator = m_code.at(block).terminator;
    for (uint32_t index = 0; index < numberOfTargets; ++index) {
        if (targets[index].block != absentInCodeText)
            m_code.closeEdge(terminator.edge(index), BlockID(targets[index].block), argumentsAsWrittenOf(targets[index]));
    }
    m_arguments.shrink(numberOfArgumentsInFront);
}

void CodeTextParser::addEntries()
{
    uint32_t numberOfEntries = 0;
    for (const BlockInCodeText& block : m_blocks)
        numberOfEntries += block.entry != absentInCodeText;

    Vector<uint32_t, 4> blockOfEntry;
    blockOfEntry.fill(absentInCodeText, numberOfEntries);
    for (uint32_t index = 0; index < m_blocks.size(); ++index) {
        const BlockInCodeText& block = m_blocks[index];
        if (block.entry == absentInCodeText)
            continue;
        if (block.entry >= numberOfEntries) {
            failAtLine(block.line, makeString("entry "_s, block.entry, " is out of range: the text has "_s, numberOfEntries, " entries, which are numbered from 0 without a gap"_s));
            return;
        }
        if (blockOfEntry[block.entry] != absentInCodeText) {
            failAtLine(block.line, makeString("entry "_s, block.entry, " is defined twice"_s));
            return;
        }
        blockOfEntry[block.entry] = index;
    }
    for (uint32_t block : blockOfEntry)
        m_code.addEntry(BlockID(block));
}

void CodeTextParser::addFactUses()
{
    if (hasFailed())
        return;

    for (const FactLineInCodeText& line : m_factLines) {
        seek(line.start);
        advance();

        FactUse use;
        if (atID(IDPrefix::Value))
            use.record = parseValueUse();
        else if (line.recordInFront != absentInCodeText)
            use.record = ValueID(line.recordInFront);
        else
            fail("the fact names no record and stands behind none"_s);

        bool hasKind = false;
        while (!hasFailed() && !at(TokenKind::EndOfLine)) {
            if (acceptField("kind"_s)) {
                use.kind = parseSpelledEnumerator(FactKind::HeapWords, "a kind of facts"_s);
                hasKind = true;
            } else if (acceptField("subjectKind"_s))
                use.subjectKind = parseSpelledEnumerator(FactSubject::Kind::Operation, "a kind of subjects of facts"_s);
            else if (acceptField("subject"_s))
                use.subject = parseUInt32("a number"_s);
            else if (acceptField("index"_s))
                use.index = parseUInt32("a number"_s);
            else if (acceptField("type"_s))
                use.type = parseTypeInText();
            else if (acceptField("key"_s))
                use.key = parsePropertyKeyInText();
            else
                failExpecting("a member of a use of a fact"_s);
        }
        if (!hasKind)
            fail("the fact lacks `kind=`"_s);
        if (hasFailed())
            return;
        m_code.addFactUse(use);
    }
}

void CodeTextParser::addFatesOfPolls()
{
    if (hasFailed())
        return;

    for (const Lexer::State& line : m_pollLines) {
        seek(line);
        advance();

        ValueID poll = parseValueUse();
        if (!acceptField("fate"_s))
            failExpecting("`fate=`"_s);
        FateOfPoll fate = parseSpelledEnumerator(FateOfPoll::IsDropped, "a fate of a poll"_s);
        ValueID unlessKept = acceptField("unlessKept"_s) ? parseValueUse() : ValueID();
        if (!hasFailed() && m_code.at(poll).opcode != Opcode::LoopHint)
            fail("only a LoopHint has a fate"_s);
        if (!hasFailed() && unlessKept && (fate != FateOfPoll::LeavesHandler || m_code.at(unlessKept).opcode == Opcode::LoopHint))
            fail("only a poll that leaves its handler does so unless a record that is no poll is kept"_s);
        if (hasFailed())
            return;
        m_code.addFateOfPoll(poll, fate, unlessKept);
    }
}

String parseCode(const CodeTextContext& context, StringView text, Code& code)
{
    if (text.is8Bit())
        return CodeTextParser(context, byteCast<uint8_t>(text.span8()), code).run();
    CString utf8 = text.utf8();
    return CodeTextParser(context, byteCast<uint8_t>(utf8.span()), code).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
