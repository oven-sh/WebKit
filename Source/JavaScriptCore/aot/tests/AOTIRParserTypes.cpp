/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRParserInternal.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned maxTypeNestingInText = 32;

static constexpr uint32_t hasIntegersFlag = static_cast<uint32_t>(TypeFlags::HasIntegers);
static constexpr uint32_t anyFunctionFlag = static_cast<uint32_t>(TypeFlags::AnyFunction);
static constexpr uint32_t anyObjectFlags = static_cast<uint32_t>(TypeFlags::AnyObject) | anyFunctionFlag;

struct PrimitiveTypeWord {
    ASCIILiteral word;
    uint16_t primitives;
};

static constexpr PrimitiveTypeWord primitiveTypeWords[] = {
    { "undefined"_s, Primitive::Undefined },
    { "null"_s, Primitive::Null },
    { "boolean"_s, Primitive::Boolean },
    { "true"_s, Primitive::True },
    { "false"_s, Primitive::False },
    { "number"_s, Primitive::Number },
    { "double"_s, Primitive::Double },
    { "whole"_s, Primitive::WholeNumber },
    { "fractional"_s, Primitive::FractionalNumber },
    { "string"_s, Primitive::String },
    { "symbol"_s, Primitive::Symbol },
    { "bigint"_s, Primitive::BigInt },
    { "absent"_s, Primitive::Absent },
    { "unspecified"_s, Primitive::Unspecified },
};

struct TextParser::TypeParts {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TypeParts);

    struct EngineClassPart {
        bool contentsAreProven { false };
        ArrayStorage arrayStorage { ArrayStorage::Other };
        OptionSet<PristineAspects> provenPristine;
    };

    static bool isAtSymbolKey(const TextParser&);
    bool hasMixedInternalValue(const Wire::Type&) const;

    Vector<uint32_t> strings;
    Vector<uint32_t> layouts;
    Vector<uint32_t> builtins;
    Vector<uint32_t> functions;
    Vector<Wire::ClassPart> classes;

    uint16_t numberBits { 0 };
    Vector<uint32_t> symbolKeyWords;
    Vector<EngineClassPart> engineClassParts;
    Vector<RegExpLiteralInType> regExpLiterals;
    bool layoutsAreExact { false };
    bool hasEngineParts { false };
};

TextParser::TextParser(std::span<const uint8_t> text, IRForm form, BuildContext& build, const VerifierInputs& verifierInputs)
    : m_text(text)
    , m_cursor { Lexer(text), { }, { } }
    , m_form(form)
    , m_verifierInputs(verifierInputs)
    , m_build(build)
    , m_builtins(BuiltinTable::singleton())
    , m_builder(m_builtins)
{
}

TextParser::~TextParser() = default;

static uint32_t wirePrimitivesOf(uint16_t primitives)
{
    return static_cast<uint32_t>(toWire(primitives));
}

static Wire::Type anythingOnWire()
{
    Wire::Type wire;
    wire.primitives = wirePrimitivesOf(Primitive::All);
    wire.flags = anyObjectFlags;
    return wire;
}

static Wire::Span appendSortedWords(WireProgramBuilder& builder, Vector<uint32_t>& words)
{
    if (words.isEmpty())
        return { };
    AOT::sort(words);
    removeRepeatedElements(words);
    return builder.appendWords(words.span());
}

static uint32_t parseStringWordOrMinus(TextParser& parser)
{
    return parser.accept(TokenKind::Minus) ? Wire::none : parser.parseStringWord();
}

static uint32_t parseIDWordOrMinus(TextParser& parser, IDPrefix prefix)
{
    return parser.accept(TokenKind::Minus) ? Wire::none : parser.parseIDWord(prefix);
}

static uint32_t parseIDWordUnderLimit(TextParser& parser, IDPrefix prefix, uint32_t limit)
{
    if (parser.atID(prefix) && parser.token().number >= limit) {
        parser.fail(InternalErrorKind::IdOutOfRange);
        return 0;
    }
    return parser.parseIDWord(prefix);
}

static uint32_t parseWellKnownSymbolWord(TextParser& parser)
{
    if (parser.accept(TokenKind::Minus))
        return Wire::none;
    if (parser.at(TokenKind::Word)) {
        std::span<const uint8_t> word = parser.token().text;
        for (WellKnownSymbol symbol : allMembersOf<WellKnownSymbol>) {
            std::span<const char> name = nameOf(symbol).span();
            if (word.size() != name.size() || word[0] != toASCIILower(name[0]) || !equalSpans(word.subspan(1), name.subspan(1)))
                continue;
            parser.advance();
            return static_cast<uint32_t>(symbol);
        }
    }
    parser.fail();
    return 0;
}

uint32_t TextParser::parseStringWord()
{
    if (!at(TokenKind::String)) {
        fail();
        return 0;
    }
    uint32_t word = m_builder.internString(decodeStringToken(token()).span());
    advance();
    return word;
}

uint32_t TextParser::parsePropertyKeyWord()
{
    Wire::PropertyKey key = parsePropertyKeyValue();
    if (hasFailed())
        return 0;
    return m_builder.internPropertyKey(key);
}

Wire::PropertyKey TextParser::parsePropertyKeyValue()
{
    Wire::PropertyKey key;
    if (at(TokenKind::String) || at(TokenKind::Minus)) {
        key.kind = static_cast<uint32_t>(PropertyKeyKind::Text);
        key.text = parseStringWordOrMinus(*this);
        return key;
    }
    if (accept(TokenKind::At)) {
        key.kind = static_cast<uint32_t>(accept(TokenKind::NumberSign) ? PropertyKeyKind::ModulePrivateName : PropertyKeyKind::EnginePrivateName);
        key.text = parseStringWordOrMinus(*this);
        return key;
    }
    if (accept(TokenKind::NumberSign)) {
        key.kind = static_cast<uint32_t>(PropertyKeyKind::PrivateName);
        key.text = parseStringWordOrMinus(*this);
        expect(TokenKind::At);
        key.owner = parseIDWordOrMinus(*this, IDPrefix::Layout);
        return key;
    }
    if (accept(TokenKind::AtAt)) {
        key.kind = static_cast<uint32_t>(PropertyKeyKind::WellKnownSymbol);
        key.symbol = parseWellKnownSymbolWord(*this);
        return key;
    }
    if (acceptWord("symbol"_s)) {
        key.kind = static_cast<uint32_t>(PropertyKeyKind::CreatedSymbol);
        if (accept(TokenKind::LeftParenthesis)) {
            key.text = parseStringWord();
            expect(TokenKind::RightParenthesis);
        }
        expect(TokenKind::At);
        key.module = parseIDWordOrMinus(*this, IDPrefix::Module);
        expect(TokenKind::Colon);
        key.callNumber = accept(TokenKind::Minus) ? Wire::none : parseUInt32(Wire::none - 1);
        return key;
    }
    if (acceptWord("Symbol"_s)) {
        key.kind = static_cast<uint32_t>(PropertyKeyKind::RegisteredSymbol);
        expect(TokenKind::Dot);
        expectWord("for"_s);
        expect(TokenKind::LeftParenthesis);
        key.text = parseStringWordOrMinus(*this);
        expect(TokenKind::RightParenthesis);
        return key;
    }
    fail();
    return key;
}

bool TextParser::TypeParts::isAtSymbolKey(const TextParser& parser)
{
    if (parser.at(TokenKind::AtAt))
        return true;
    if (parser.atWord("Symbol"_s))
        return parser.nextToken().kind == TokenKind::Dot;
    if (!parser.atWord("symbol"_s))
        return false;
    if (parser.nextToken().kind == TokenKind::LeftParenthesis)
        return true;
    if (parser.nextToken().kind != TokenKind::At)
        return false;

    // "symbol @12" and "symbol @JSValue" are the primitive followed by a position or a representation.
    Lexer lexer = parser.m_cursor.lexer;
    Token afterAt = lexer.next();
    return afterAt.kind == TokenKind::Minus || (afterAt.kind == TokenKind::ID && afterAt.prefix == IDPrefix::Module);
}

bool TextParser::TypeParts::hasMixedInternalValue(const Wire::Type& wire) const
{
    bool hasOpaque = wire.opaque != Wire::none;
    bool hasEnvironment = wire.environment != Wire::none;
    if (!hasOpaque && !hasEnvironment)
        return false;
    if (hasOpaque && hasEnvironment)
        return true;
    if (wire.primitives & ~wirePrimitivesOf(Primitive::Absent) || wire.flags || numberBits & ~Primitive::Unspecified)
        return true;
    return !strings.isEmpty() || !layouts.isEmpty() || !builtins.isEmpty() || !classes.isEmpty() || !functions.isEmpty() || !symbolKeyWords.isEmpty();
}

static void addIntegers(Wire::Type& wire, uint16_t& numberBits, Range range)
{
    if (wire.flags & hasIntegersFlag)
        range = range.unionWith({ wire.minimum, wire.maximum });
    wire.flags |= hasIntegersFlag;
    wire.minimum = range.minimum;
    wire.maximum = range.maximum;
    numberBits |= Primitive::Integer;
}

static bool parseIntegerPart(TextParser& parser, Wire::Type& wire, uint16_t& numberBits)
{
    if (parser.at(TokenKind::Integer)) {
        addIntegers(wire, numberBits, Range::one(parser.parseInt64()));
        return true;
    }
    if (parser.acceptWord("i32"_s)) {
        addIntegers(wire, numberBits, Range::int32());
        return true;
    }
    if (!parser.acceptWord("int"_s))
        return false;

    Range range = Range::full();
    if (parser.at(TokenKind::LeftBracket) && parser.nextToken().kind == TokenKind::Integer) {
        parser.advance();
        range.minimum = parser.parseInt64();
        parser.expect(TokenKind::Comma);
        range.maximum = parser.parseInt64();
        parser.expect(TokenKind::RightBracket);
    }
    addIntegers(wire, numberBits, range);
    return true;
}

static bool parsePrimitivePart(TextParser& parser, Wire::Type& wire, uint16_t& numberBits)
{
    for (const PrimitiveTypeWord& entry : primitiveTypeWords) {
        if (!parser.acceptWord(entry.word))
            continue;
        wire.primitives |= wirePrimitivesOf(entry.primitives);
        numberBits |= entry.primitives & (Primitive::Number | Primitive::Unspecified);
        return true;
    }
    if (!parser.acceptWord("anything"_s))
        return false;
    Wire::Type anything = anythingOnWire();
    wire.primitives |= anything.primitives;
    wire.flags |= anything.flags;
    numberBits |= Primitive::Number;
    return true;
}

static void parseFunctionPart(TextParser& parser, Wire::Type& wire)
{
    parser.expectWord("function"_s);
    wire.flags |= anyFunctionFlag;
}

static void parseEnvironmentPart(TextParser& parser, Wire::Type& wire)
{
    TextPosition position = parser.token().position;
    parser.expectWord("env"_s);
    if (wire.environment != Wire::none) {
        parser.fail(InternalErrorKind::IRTextError, position);
        return;
    }
    wire.environment = parser.parseIDWord(IDPrefix::Layout);
}

static bool parseObjectWordPart(TextParser& parser, Wire::Type& wire)
{
    if (parser.acceptWord("object"_s)) {
        wire.flags |= anyObjectFlags;
        return true;
    }
    if (parser.atWord("function"_s)) {
        parseFunctionPart(parser, wire);
        return true;
    }
    if (parser.atWord("env"_s)) {
        parseEnvironmentPart(parser, wire);
        return true;
    }
    return false;
}

static BuiltinID parseBuiltinPart(TextParser& parser)
{
    parser.expectWord("builtin"_s);
    parser.expect(TokenKind::LeftParenthesis);
    BuiltinID builtin = parser.parseBuiltin();
    parser.expect(TokenKind::RightParenthesis);
    return builtin;
}

static bool parseOpaquePart(TextParser& parser, Wire::Type& wire)
{
    if (!parser.at(TokenKind::Word))
        return false;
    if (std::ranges::any_of(parser.token().text, [](uint8_t byte) { return isASCIIUpper(byte); }))
        return false;
    StringView word = parser.token().view();
    for (OpaqueKind kind : allMembersOf<OpaqueKind>) {
        if (!equalIgnoringASCIICase(word, nameOf(kind)))
            continue;
        if (wire.opaque != Wire::none) {
            parser.fail();
            return true;
        }
        wire.opaque = static_cast<uint32_t>(kind);
        parser.advance();
        return true;
    }
    return false;
}

uint32_t TextParser::typeWordOfAnything()
{
    return m_builder.internType(anythingOnWire());
}

uint32_t TextParser::typeWordOfAnyFunction()
{
    Wire::Type wire;
    wire.flags = anyFunctionFlag;
    return m_builder.internType(wire);
}

uint32_t TextParser::parseTypeWord()
{
    SetForScope nesting(m_typeNesting, m_typeNesting + 1);
    if (m_typeNesting > maxTypeNestingInText) {
        fail();
        return 0;
    }

    auto parts = makeUnique<TypeParts>();
    Wire::Type wire = parseTypeValue(*parts);
    if (hasFailed())
        return 0;

    parts->layoutsAreExact = m_layoutsAreExact && !parts->layouts.isEmpty();
    if (!parts->hasEngineParts && !parts->layoutsAreExact)
        return m_builder.internType(wire);

    Vector<Wire::Type>& types = m_builder.sections().types;
    uint32_t index = static_cast<uint32_t>(types.size());
    types.append(wire);
    m_engineTypeParts.add(index, WTF::move(parts));
    return index;
}

Wire::Type TextParser::parseTypeValue(TypeParts& parts)
{
    if (acceptWord("never"_s)) {
        if (at(TokenKind::Bar))
            fail();
        return { };
    }

    TextPosition position = token().position;
    Wire::Type wire;
    do
        parseTypePart(wire, parts);
    while (!hasFailed() && accept(TokenKind::Bar));
    if (hasFailed())
        return { };
    if (parts.hasMixedInternalValue(wire)) {
        fail(InternalErrorKind::IRTextError, position);
        return { };
    }

    if ((parts.numberBits & Primitive::Double && !(wire.primitives & wirePrimitivesOf(Primitive::Number))) || parts.numberBits & Primitive::Unspecified)
        parts.hasEngineParts = true;
    wire.strings = appendSortedWords(m_builder, parts.strings);
    wire.layouts = appendSortedWords(m_builder, parts.layouts);
    wire.builtins = appendSortedWords(m_builder, parts.builtins);
    wire.functions = appendSortedWords(m_builder, parts.functions);
    if (!parts.classes.isEmpty())
        wire.classes = m_builder.appendRecords<Wire::ClassPart>(parts.classes.span());
    return wire;
}

void TextParser::parseTypePart(Wire::Type& wire, TypeParts& parts)
{
    if (at(TokenKind::String)) {
        parts.strings.append(parseStringWord());
        return;
    }
    if (atID(IDPrefix::Layout) || (atWord("L"_s) && nextToken().kind == TokenKind::LeftParenthesis)) {
        parts.layouts.append(parseIDWord(IDPrefix::Layout));
        return;
    }
    if (atID(IDPrefix::Function)) {
        parts.functions.append(parseIDWordUnderLimit(*this, IDPrefix::Function, countOf(ProgramList::Functions)));
        return;
    }
    if (TypeParts::isAtSymbolKey(*this)) {
        parts.symbolKeyWords.append(parsePropertyKeyWord());
        parts.hasEngineParts = true;
        return;
    }
    if (parseIntegerPart(*this, wire, parts.numberBits) || parsePrimitivePart(*this, wire, parts.numberBits) || parseObjectWordPart(*this, wire))
        return;
    if (atWord("builtin"_s)) {
        parts.builtins.append(parseBuiltinPart(*this).index());
        return;
    }
    if (parseOpaquePart(*this, wire))
        return;

    BuiltinClassID builtinClass = parseBuiltinClass();
    if (!hasFailed())
        parseClassPart(parts, builtinClass);
}

void TextParser::parseClassPart(TypeParts& parts, BuiltinClassID builtinClass)
{
    TextPosition position = token().position;
    unsigned arity = m_builtins.numberOfSlotsInType(builtinClass);
    Vector<uint32_t, 2> contents;
    TypeParts::EngineClassPart enginePart;
    if (at(TokenKind::LeftAngle)) {
        parseList(TokenKind::LeftAngle, TokenKind::RightAngle, [&] {
            if (contents.size() < arity) {
                contents.append(parseTypeWord());
                return;
            }
            parts.hasEngineParts = true;
            if (builtinClass == BuiltinClasses::RegExp && at(TokenKind::String)) {
                StringID pattern(parseStringWord());
                parts.regExpLiterals.append({ pattern, parseFlagWord<RegExpFlags>() });
                return;
            }
            if (acceptWord("proven"_s)) {
                enginePart.contentsAreProven = true;
                return;
            }
            if (acceptWord("pristine"_s)) {
                uint32_t aspects = parseFlagWord<PristineAspects>();
                enginePart.provenPristine = OptionSet<PristineAspects>::fromRaw(static_cast<OptionSet<PristineAspects>::StorageType>(aspects));
                return;
            }
            enginePart.arrayStorage = parseEnumerator<ArrayStorage>();
        });
        if (contents.size() != arity)
            fail(InternalErrorKind::IRTextError, position);
    } else if (arity)
        contents.fill(m_builder.internType(anythingOnWire()), arity);
    if (hasFailed())
        return;

    size_t index = 0;
    while (index < parts.classes.size() && parts.classes[index].builtinClass < builtinClass.index())
        ++index;
    if (index < parts.classes.size() && parts.classes[index].builtinClass == builtinClass.index()) {
        fail(InternalErrorKind::IRTextError, position);
        return;
    }
    Wire::Span contentsSpan = contents.isEmpty() ? Wire::Span { } : m_builder.appendWords(contents.span());
    parts.classes.insert(index, Wire::ClassPart { builtinClass.index(), contentsSpan });
    parts.engineClassParts.insert(index, enginePart);
}

void TextParser::completeType(uint32_t wireIndex, Type& type) const
{
    auto iterator = m_engineTypeParts.find(wireIndex);
    if (iterator == m_engineTypeParts.end())
        return;
    const TypeParts& parts = *iterator->value;

    type.primitives = static_cast<uint16_t>((type.primitives & ~Primitive::Number) | parts.numberBits);
    for (uint32_t keyWord : parts.symbolKeyWords)
        type.symbolLiterals.add(PropertyKeyID(keyWord));

    RELEASE_ASSERT(type.classes.size() == parts.engineClassParts.size());
    for (size_t index = 0; index < type.classes.size(); ++index) {
        ClassPart& part = type.classes[index];
        const TypeParts::EngineClassPart& enginePart = parts.engineClassParts[index];
        part.contentsAreProven = enginePart.contentsAreProven;
        part.arrayStorage = enginePart.arrayStorage;
        part.provenPristine = enginePart.provenPristine;
    }
    type.regExpLiteralSet = SortedSet<RegExpLiteralInType>(parts.regExpLiterals.span());

    if (parts.layoutsAreExact) {
        type.layouts = m_program->types.intern(LayoutSet::complete(parts.layouts.map([](uint32_t word) { return LayoutID(word); })));
    }
}

LayoutSetID TextParser::parseLayoutSet()
{
    Vector<LayoutID> members;
    bool isComplete = true;
    parseList(TokenKind::LeftBracket, TokenKind::RightBracket, [&] {
        if (!isComplete) {
            fail();
            return;
        }
        if (accept(TokenKind::DotDot)) {
            isComplete = false;
            return;
        }
        members.append(LayoutID(parseIDWordUnderLimit(*this, IDPrefix::Layout, countOf(ProgramList::Layouts))));
    });
    if (hasFailed())
        return { };
    return m_program->types.intern(isComplete ? LayoutSet::complete(WTF::move(members)) : LayoutSet::open(WTF::move(members)));
}

CalleeSetID TextParser::parseCalleeSet()
{
    Vector<FunctionID> functions;
    Vector<BuiltinID> builtins;
    bool isComplete = true;
    parseList(TokenKind::LeftBracket, TokenKind::RightBracket, [&] {
        if (!isComplete) {
            fail();
            return;
        }
        if (accept(TokenKind::DotDot)) {
            isComplete = false;
            return;
        }
        if (atWord("builtin"_s)) {
            builtins.append(parseBuiltinPart(*this));
            return;
        }
        functions.append(FunctionID(parseIDWordUnderLimit(*this, IDPrefix::Function, countOf(ProgramList::Functions))));
    });
    if (hasFailed())
        return { };
    return m_program->types.intern(isComplete ? CalleeSet::complete(WTF::move(functions), WTF::move(builtins)) : CalleeSet::open(WTF::move(functions), WTF::move(builtins)));
}

template<typename IDType>
static IDType parseBuiltinToken(TextParser& parser, IDType (BuiltinTable::*fromToken)(StringView) const)
{
    if (!parser.at(TokenKind::Word)) {
        parser.fail();
        return { };
    }
    IDType id = (parser.builtins().*fromToken)(parser.token().view());
    if (!id) {
        parser.fail();
        return { };
    }
    parser.advance();
    return id;
}

template<typename IDType>
static IDType parseBuiltinNumber(TextParser& parser)
{
    TextPosition position = parser.token().position;
    IDType id(parser.parseUInt32());
    if (parser.hasFailed())
        return { };
    if (!parser.builtins().contains(id)) {
        parser.fail(InternalErrorKind::IRTextError, position);
        return { };
    }
    return id;
}

BuiltinID TextParser::parseBuiltin()
{
    return parseBuiltinToken(*this, &BuiltinTable::builtinFromToken);
}

BuiltinClassID TextParser::parseBuiltinClass()
{
    return parseBuiltinToken(*this, &BuiltinTable::classFromToken);
}

BuiltinLayoutID TextParser::parseBuiltinLayout()
{
    return parseBuiltinToken(*this, &BuiltinTable::layoutFromToken);
}

BuiltinOverloadID TextParser::parseBuiltinOverload()
{
    if (!at(TokenKind::Word)) {
        fail();
        return { };
    }
    Token word = token();
    advance();
    expect(TokenKind::Slash);
    uint32_t index = parseUInt32();
    if (hasFailed())
        return { };
    BuiltinOverloadID id = m_builtins.overloadFromToken(makeString(word.view(), '/', index));
    if (!id)
        fail(InternalErrorKind::IRTextError, word.position);
    return id;
}

RuntimeModuleID TextParser::parseRuntimeModule()
{
    return parseBuiltinToken(*this, &BuiltinTable::moduleFromToken);
}

RuntimeFunctionID TextParser::parseRuntimeFunction()
{
    return parseBuiltinNumber<RuntimeFunctionID>(*this);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
