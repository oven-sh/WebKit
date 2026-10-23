/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTIRLexer.h"
#include "AOTIRParser.h"
#include "AOTVector.h"
#include "AOTWireSections.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StackCheck.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class WireProgramBuilder {
    WTF_MAKE_NONCOPYABLE(WireProgramBuilder);
public:
    explicit WireProgramBuilder(const BuiltinTable&);

    Wire::SectionVectors& sections() { return m_sections; }
    const Wire::SectionVectors& sections() const { return m_sections; }

    uint32_t internString(std::span<const uint8_t> bytes);
    uint32_t internPropertyKey(const Wire::PropertyKey&);
    uint32_t internType(const Wire::Type&);

    Wire::Span appendWords(std::span<const uint32_t>);
    Wire::Span appendBytes(std::span<const uint8_t>);
    Wire::Span appendCode(std::span<const uint32_t>);
    template<typename WireRecord> Wire::Span appendRecords(std::span<const WireRecord>);

    Vector<uint8_t> finish();

private:
    // The key is wider than the 32-bit hash it holds, so no hash is the table's empty or deleted key.
    using PositionsByHash = UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    template<typename WireRecord> uint32_t internRecord(Vector<WireRecord>&, PositionsByHash&, const WireRecord&);

    const BuiltinTable& m_builtins;
    Wire::SectionVectors m_sections;
    PositionsByHash m_strings;
    PositionsByHash m_propertyKeys;
    PositionsByHash m_types;
};

struct PatchContext {
    Program& program;
    std::span<const TypeID> types;

    TypeID type(uint32_t wireWord) const { return wireWord == Wire::none ? TypeID() : types[wireWord]; }
};

using ProgramPatch = WTF::Function<void(PatchContext&)>;

struct TextItem {
    ProgramList list;
    bool hasBody { false };
    Lexer::State start;
    Lexer::State body;
};

struct BodyScan {
    unsigned numberOfBlocks { 0 };
    uint32_t numberOfValues { 0 };
};

struct ValueEngineFields {
    FunctionID function;
    ValueID value;
    uint32_t typeWord { Wire::none };
    uint32_t expectedWord { Wire::none };
    std::optional<Representation> representation;
};

class TextParser {
    WTF_MAKE_NONCOPYABLE(TextParser);
public:
    TextParser(std::span<const uint8_t> text, IRForm, BuildContext&, const VerifierInputs&);
    ~TextParser();

    std::unique_ptr<Program> run();

    IRForm form() const { return m_form; }
    const BuiltinTable& builtins() const { return m_builtins; }
    WireProgramBuilder& builder() { return m_builder; }
    void addPatch(ProgramPatch&& patch) { m_patches.append(WTF::move(patch)); }

    const Token& token() const { return m_cursor.token; }
    const Token& nextToken() const { return m_cursor.nextToken; }
    void advance();
    void seek(Lexer::State);

    bool at(TokenKind kind) const { return token().kind == kind; }
    bool atID(IDPrefix prefix) const { return at(TokenKind::ID) && token().prefix == prefix; }
    bool atWord(ASCIILiteral word) const { return token().isWord(word); }
    bool atField() const { return at(TokenKind::Word) && nextToken().kind == TokenKind::Equal; }
    bool accept(TokenKind);
    bool acceptWord(ASCIILiteral);
    bool acceptField(ASCIILiteral name);

    void expect(TokenKind);
    void expectWord(ASCIILiteral);
    void expectEndOfLine();

    void fail(InternalErrorKind = InternalErrorKind::IRTextError);
    void fail(InternalErrorKind, TextPosition);
    bool hasFailed() const { return m_hasFailed; }

    uint32_t parseUInt32(uint32_t maximum = UINT32_MAX);
    uint64_t parseUInt64();
    int64_t parseInt64();
    double parseDouble();
    bool parseBool();
    uint32_t parseIDWord(IDPrefix);
    uint32_t parseStringWord();
    uint32_t parsePropertyKeyWord();
    uint32_t parseTypeWord();
    uint32_t typeWordOfAnything();
    Wire::Span parseByteString();
    template<typename E> E parseEnumerator();
    template<typename F> uint32_t parseFlagWord();
    void parseList(TokenKind open, TokenKind close, NOESCAPE const Invocable<void()> auto& element);
    void parseRecord(NOESCAPE const Invocable<void()> auto& fields);

    LayoutSetID parseLayoutSet();
    CalleeSetID parseCalleeSet();

    BuiltinID parseBuiltin();
    BuiltinClassID parseBuiltinClass();
    BuiltinName parseBuiltinName();
    BuiltinLayoutID parseBuiltinLayout();
    BuiltinSlotID parseBuiltinSlot();
    BuiltinOverloadID parseBuiltinOverload();
    RuntimeModuleID parseRuntimeModule();
    RuntimeFunctionID parseRuntimeFunction();

    FunctionID currentFunction() const { return m_body.function; }
    ValueID currentValue() const { return m_body.value; }
    BlockID currentBlock() const { return m_body.block; }
    Vector<uint32_t>& code() { return m_body.code; }
    uint32_t parseValueUseWord();
    uint32_t parseOptionalValueUseWord();
    bool atOperand() const;

private:
    struct Cursor {
        Lexer lexer;
        Token token;
        Token nextToken;
    };

    struct ProgramScan {
        std::array<uint32_t, numberOfEnumerators<ProgramList>> counts { };
        Vector<std::optional<uint32_t>> namespaceLayoutRanks;
        uint32_t numberOfNamespaceLayouts { 0 };
    };

    struct BodyState {
        BodyScan scan;
        FunctionID function;
        BlockID block;
        ValueID value;
        Vector<uint32_t> code;
    };

    struct TypeParts;

    uint32_t& countOf(ProgramList list) { return m_scan.counts[static_cast<unsigned>(list)]; }
    uint32_t layoutWordOf(uint32_t numberInText);

    bool scanItems();
    void scanItem(Lexer&, const Token& keyword);
    Token nextScannedToken(Lexer&);
    Token scanToEndOfLine(Lexer&);
    void scanFunctionEnd(Lexer&);
    bool scanBody(Lexer::State, BodyScan&);
    void scanBlockHead(Lexer&, BodyScan&);

    void parseItem(const TextItem&);
    Wire::Body parseBody(const TextItem&);
    void parseBlock(uint32_t blockIndex);
    void parseBlockParameter();
    void parseValue();
    void parseEngineValueFields(ValueEngineFields&);
    void parseTerminator();
    void parseTarget();
    uint32_t parseBlockUseWord();

    Wire::Type parseTypeValue(TypeParts&);
    void parseTypePart(Wire::Type&, TypeParts&);
    void parseClassPart(TypeParts&, BuiltinClassID);
    Wire::PropertyKey parsePropertyKeyValue();
    void completeType(uint32_t wireIndex, Type&) const;

    void applyPatches(Program&, std::span<const TypeID>);
    void applyValueEngineFields(const PatchContext&) const;

    std::span<const uint8_t> m_text;
    Cursor m_cursor;
    IRForm m_form;
    VerifierInputs m_verifierInputs;
    BuildContext& m_build;
    const BuiltinTable& m_builtins;
    WireProgramBuilder m_builder;
    std::unique_ptr<Program> m_program;
    ProgramScan m_scan;
    Vector<TextItem> m_items;
    Vector<ProgramPatch> m_patches;
    Vector<ValueEngineFields> m_valueEngineFields;
    BodyState m_body;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<TypeParts>, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_engineTypeParts;
    bool m_layoutsAreExact { false };
    unsigned m_typeNesting { 0 };
    WTF::StackCheck m_stackCheck;
    bool m_hasFailed { false };
};

template<typename WireRecord>
Wire::Span WireProgramBuilder::appendRecords(std::span<const WireRecord> records)
{
    Vector<WireRecord>& list = m_sections.template listOf<WireRecord>();
    Wire::Span span { static_cast<uint32_t>(list.size()), static_cast<uint32_t>(records.size()) };
    list.append(records);
    return span;
}

template<typename E>
E TextParser::parseEnumerator()
{
    std::optional<E> enumerator = at(TokenKind::Word) ? parseEnum<E>(token().view()) : std::nullopt;
    if (!enumerator) {
        fail();
        return { };
    }
    advance();
    return *enumerator;
}

template<typename F>
uint32_t TextParser::parseFlagWord()
{
    uint32_t word = 0;
    parseList(TokenKind::LeftBracket, TokenKind::RightBracket, [&] {
        word |= static_cast<uint32_t>(parseEnumerator<F>());
    });
    return hasFailed() ? 0 : word;
}

void TextParser::parseList(TokenKind open, TokenKind close, NOESCAPE const Invocable<void()> auto& element)
{
    expect(open);
    while (!hasFailed() && !at(close)) {
        element();
        if (!accept(TokenKind::Comma))
            break;
    }
    expect(close);
}

void TextParser::parseRecord(NOESCAPE const Invocable<void()> auto& fields)
{
    if (!m_stackCheck.isSafeToRecurse()) {
        fail();
        return;
    }
    expect(TokenKind::LeftBrace);
    fields();
    expect(TokenKind::RightBrace);
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaParser.h"

#endif // ENABLE(AOT)
