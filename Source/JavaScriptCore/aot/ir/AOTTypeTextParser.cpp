/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeText.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIDPrefix.h"
#include "AOTIRProgram.h"
#include "AOTStringTable.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <span>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <wtf/ASCIICType.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeTextParser {
    WTF_MAKE_NONCOPYABLE(TypeTextParser);
public:
    TypeTextParser(const TypeArena& arena, StringView text, unsigned offset)
        : m_arena(arena)
        , m_text(text)
        , m_offset(offset)
    {
    }

    std::optional<TypeID> parseType(TypeArena&);
    std::optional<ObjectID> parseObject();
    std::optional<LayoutSetID> parseLayoutSet(TypeArena&);

    template<typename Result>
    std::optional<Result> finish(std::optional<Result> result, unsigned& offset, String& error) const
    {
        if (hasFailed()) {
            offset = m_offsetOfError;
            error = m_error;
            return std::nullopt;
        }
        offset = m_offset;
        return result;
    }

private:
    static constexpr unsigned maxTypeNestingDepth = 32;
    static constexpr unsigned maxCodePointDigits = 6;
    static constexpr uint32_t largestNumber = UINT32_MAX - 1;
    static constexpr uint32_t largestResultOrdinal = 1;

    using Bytes = Vector<uint8_t, 64>;

    struct PrimitiveWord {
        ASCIILiteral word;
        uint16_t primitives;
    };

    struct NamedObjectWord {
        ASCIILiteral word;
        ObjectKind kind;
    };

    struct ParsedParts {
        TypeParts parts;
        Vector<StringID, 8> strings;
        Vector<PropertyKeyID, 4> symbols;
        Vector<ObjectID, 8> objects;
        Vector<LayoutID, 8> layouts;
        Vector<ClassPart, 4> classes;
        bool holdsFractionsWithoutBounds { false };
    };

    static constexpr PrimitiveWord primitiveWords[] = {
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

    static constexpr NamedObjectWord namedObjectWords[] = {
        { "builtin"_s, ObjectKind::Builtin },
        { "result"_s, ObjectKind::ResultOfBuiltin },
        { "host"_s, ObjectKind::HostObject },
        { "escapedFunctions"_s, ObjectKind::EscapedFunctionsOfHolder },
        { "escaped"_s, ObjectKind::EscapedOfClass },
        { "escapedWithoutShape"_s, ObjectKind::EscapedWithoutShape },
        { "coroutine"_s, ObjectKind::CoroutineResult },
        { "rest"_s, ObjectKind::RestArray },
        { "prototypeOf"_s, ObjectKind::FunctionPrototype },
        { "constructed"_s, ObjectKind::ConstructedObject },
        { "namespace"_s, ObjectKind::Namespace },
        { "instance"_s, ObjectKind::Instance },
        { "prototype"_s, ObjectKind::Prototype },
    };

    static std::optional<UnknownSeed> seedNamed(StringView);
    static uint64_t valueOfDigits(StringView digits);
    static std::optional<uint64_t> numberInID(StringView name, IDPrefix);
    static void addIntegers(TypeParts&, Range);

    template<typename Sets, typename Member, size_t inlineCapacity>
    static auto setOf(Sets& sets, Vector<Member, inlineCapacity>& members) -> decltype(Sets::of(Member()))
    {
        std::ranges::sort(members);
        removeRepeatedElements(members);
        if (members.isEmpty())
            return { };
        if (members.size() == 1)
            return Sets::of(members[0]);
        return sets.ofSorted(members.span());
    }

    bool hasFailed() const { return !m_error.isNull(); }
    void fail(unsigned offset, String&& message);
    void fail(unsigned offset, ASCIILiteral message) { fail(offset, String(message)); }

    char16_t peek(unsigned distance = 0) const;
    StringView word() const;
    bool accept(char);
    bool acceptWord(ASCIILiteral);
    void expect(char);
    void skipBlanks();
    void expectOpening(char bracket);
    bool acceptComma();
    void expectComma();
    void expectClosing(char bracket);

    int64_t parseInteger();
    Range parseIntegerBounds();
    uint32_t parseNumber(uint32_t maximum = largestNumber);
    std::optional<uint32_t> parseID(IDPrefix, uint64_t count, ASCIILiteral kind);
    FunctionID parseFunction();
    ModuleID parseModule();
    bool isAtLayout() const;
    LayoutID parseLayout();
    BuiltinID parseBuiltin();
    BuiltinClassID parseBuiltinClass();

    bool parseEscape(Bytes&);
    bool parseQuoted(Bytes&);
    StringID parseString();
    StringID parseOptionalString() { return accept('-') ? StringID() : parseString(); }
    bool isAtSymbolKey() const;
    std::optional<WellKnownSymbol> parseWellKnownSymbol();
    PropertyKeyID parseSymbolKey();

    uint32_t numberOf(ObjectKind) const;
    ObjectID objectAt(ObjectKind, uint32_t number) const;
    ObjectID objectOfFunction(ObjectKind, FunctionID) const;
    ObjectID objectOfLayout(ObjectKind, LayoutID) const;
    ObjectID parseObjectOfKind(ObjectKind);
    ObjectID parseNamedObject();

    void parseSeeds(UnknownSeeds&);
    void parseClassPart(TypeArena&, ParsedParts&);
    bool parsePart(TypeArena&, ParsedParts&);

    LayoutID findLayout(NOESCAPE const Invocable<bool(const Layout&)> auto& matches) const
    {
        const Program* program = m_arena.program();
        for (size_t index = 0; program && index < program->layouts.size(); ++index) {
            if (matches(program->layouts[index]))
                return LayoutID(index);
        }
        return { };
    }

    const TypeArena& m_arena;
    StringView m_text;
    unsigned m_offset;
    unsigned m_offsetOfError { 0 };
    unsigned m_nesting { 0 };
    String m_error;
};

std::optional<UnknownSeed> TypeTextParser::seedNamed(StringView name)
{
    for (auto bit = static_cast<unsigned>(UnknownSeed::ResultOfUnknownCallee); bit <= static_cast<unsigned>(UnknownSeed::LostByWalk); bit <<= 1) {
        auto seed = static_cast<UnknownSeed>(bit);
        if (name == nameOf(seed))
            return seed;
    }
    return std::nullopt;
}

uint64_t TypeTextParser::valueOfDigits(StringView digits)
{
    uint64_t number = 0;
    for (char16_t digit : digits.codeUnits())
        number = std::min<uint64_t>(number * 10 + (digit - '0'), UINT32_MAX);
    return number;
}

std::optional<uint64_t> TypeTextParser::numberInID(StringView name, IDPrefix prefix)
{
    if (name.length() < 2 || name[0] != letterOf(prefix))
        return std::nullopt;
    StringView digits = name.substring(1);
    if (!digits.containsOnly<isASCIIDigit>())
        return std::nullopt;
    return valueOfDigits(digits);
}

void TypeTextParser::addIntegers(TypeParts& parts, Range range)
{
    parts.primitives |= Primitive::Integer;
    parts.integers = parts.integers.unionWith(range);
}

void TypeTextParser::fail(unsigned offset, String&& message)
{
    if (hasFailed())
        return;
    m_error = WTF::move(message);
    m_offsetOfError = offset;
}

char16_t TypeTextParser::peek(unsigned distance) const
{
    uint64_t index = static_cast<uint64_t>(m_offset) + distance;
    return index < m_text.length() ? m_text[static_cast<unsigned>(index)] : 0;
}

StringView TypeTextParser::word() const
{
    if (!isASCIIAlpha(peek()) && peek() != '_')
        return { };
    unsigned length = 1;
    while (isASCIIAlphanumeric(peek(length)) || peek(length) == '_')
        ++length;
    return m_text.substring(m_offset, length);
}

bool TypeTextParser::accept(char character)
{
    if (peek() != character)
        return false;
    ++m_offset;
    return true;
}

bool TypeTextParser::acceptWord(ASCIILiteral wanted)
{
    if (word() != wanted)
        return false;
    m_offset += wanted.length();
    return true;
}

void TypeTextParser::expect(char character)
{
    if (!accept(character))
        fail(m_offset, makeString("expected `"_s, character, '`'));
}

void TypeTextParser::skipBlanks()
{
    while (peek() == ' ')
        ++m_offset;
}

void TypeTextParser::expectOpening(char bracket)
{
    expect(bracket);
    skipBlanks();
}

bool TypeTextParser::acceptComma()
{
    skipBlanks();
    if (!accept(','))
        return false;
    skipBlanks();
    return true;
}

void TypeTextParser::expectComma()
{
    if (!acceptComma())
        fail(m_offset, "expected `,`"_s);
}

void TypeTextParser::expectClosing(char bracket)
{
    skipBlanks();
    expect(bracket);
}

int64_t TypeTextParser::parseInteger()
{
    unsigned start = m_offset;
    bool isNegative = accept('-');
    if (!isASCIIDigit(peek())) {
        fail(start, "expected an integer"_s);
        return 0;
    }
    CheckedInt64 integer = 0;
    for (; isASCIIDigit(peek()); ++m_offset) {
        int64_t digit = peek() - '0';
        integer *= 10;
        integer += isNegative ? -digit : digit;
    }
    if (integer.hasOverflowed() || !Range::full().contains(integer.value())) {
        fail(start, "the integer is outside the range that types track"_s);
        return 0;
    }
    if (isNegative && !integer.value()) {
        fail(start, "-0 is not an integer"_s);
        return 0;
    }
    return integer.value();
}

Range TypeTextParser::parseIntegerBounds()
{
    unsigned start = m_offset;
    if (!accept('['))
        return Range::full();
    skipBlanks();
    Range range;
    range.minimum = parseInteger();
    expectComma();
    range.maximum = parseInteger();
    expectClosing(']');
    if (range.isEmpty())
        fail(start, "the range is empty"_s);
    return range;
}

uint32_t TypeTextParser::parseNumber(uint32_t maximum)
{
    unsigned length = 0;
    while (isASCIIDigit(peek(length)))
        ++length;
    uint64_t number = valueOfDigits(m_text.substring(m_offset, length));
    if (!length || number > maximum) {
        fail(m_offset, makeString("expected a number from 0 to "_s, maximum));
        return 0;
    }
    m_offset += length;
    return static_cast<uint32_t>(number);
}

std::optional<uint32_t> TypeTextParser::parseID(IDPrefix prefix, uint64_t count, ASCIILiteral kind)
{
    StringView name = word();
    std::optional<uint64_t> number = numberInID(name, prefix);
    if (!number) {
        fail(m_offset, makeString("expected a "_s, kind));
        return std::nullopt;
    }
    if (*number >= count) {
        fail(m_offset, makeString("the program has no "_s, kind, ' ', name));
        return std::nullopt;
    }
    m_offset += name.length();
    return static_cast<uint32_t>(*number);
}

FunctionID TypeTextParser::parseFunction()
{
    std::optional<uint32_t> number = parseID(IDPrefix::Function, numberOf(ObjectKind::Closure), "function"_s);
    return number ? FunctionID(*number) : FunctionID();
}

ModuleID TypeTextParser::parseModule()
{
    const Program* program = m_arena.program();
    std::optional<uint32_t> number = parseID(IDPrefix::Module, program ? program->modules.size() : 0, "module"_s);
    return number ? ModuleID(*number) : ModuleID();
}

bool TypeTextParser::isAtLayout() const
{
    StringView name = word();
    return numberInID(name, IDPrefix::Layout) || (name == "L"_s && peek(1) == '(');
}

LayoutID TypeTextParser::parseLayout()
{
    const Program* program = m_arena.program();
    if (word() != "L"_s || peek(1) != '(') {
        unsigned start = m_offset;
        StringView name = word();
        std::optional<uint32_t> number = parseID(IDPrefix::Layout, program ? program->layouts.size() : 0, "layout"_s);
        if (!number)
            return { };
        LayoutID layout = program->layoutNumberedInText(*number);
        if (!layout)
            fail(start, makeString("the program has no layout "_s, name));
        return layout;
    }

    unsigned start = m_offset++;
    expectOpening('(');
    LayoutID layout;
    if (numberInID(word(), IDPrefix::Module)) {
        ModuleID module = parseModule();
        layout = findLayout([&](const Layout& candidate) {
            return candidate.kind == LayoutKind::ModuleNamespace && candidate.namespaceOf == module;
        });
    } else {
        StringView name = word();
        BuiltinLayoutID builtinLayout = m_arena.builtins().layoutFromToken(name);
        if (!builtinLayout)
            fail(m_offset, makeString("the table of built-ins has no layout `"_s, name, '`'));
        m_offset += name.length();
        layout = findLayout([&](const Layout& candidate) {
            return candidate.kind == LayoutKind::BuiltinResult && candidate.builtinLayout == builtinLayout;
        });
    }
    expectClosing(')');
    if (!layout)
        fail(start, "the program has no such layout"_s);
    return hasFailed() ? LayoutID() : layout;
}

BuiltinID TypeTextParser::parseBuiltin()
{
    const BuiltinTable& builtins = m_arena.builtins();
    StringView name = word();
    BuiltinID builtin = builtins.builtinFromToken(name);
    if (!builtin) {
        fail(m_offset, makeString("the table of built-ins has no built-in `"_s, name, '`'));
        return { };
    }
    m_offset += name.length();
    return builtins.canonicalID(builtin);
}

BuiltinClassID TypeTextParser::parseBuiltinClass()
{
    StringView name = word();
    BuiltinClassID builtinClass = m_arena.builtins().classFromToken(name);
    if (!builtinClass) {
        fail(m_offset, makeString("the table of built-ins has no class `"_s, name, '`'));
        return { };
    }
    m_offset += name.length();
    return builtinClass;
}

bool TypeTextParser::parseEscape(Bytes& bytes)
{
    unsigned start = m_offset++;
    char16_t escaped = peek();
    if (escaped == '"' || escaped == '\\' || escaped == 'n') {
        bytes.append(static_cast<uint8_t>(escaped == 'n' ? '\n' : escaped));
        ++m_offset;
        return true;
    }
    if (escaped == 'x' && isASCIIHexDigit(peek(1)) && isASCIIHexDigit(peek(2))) {
        bytes.append(toASCIIHexValue(peek(1), peek(2)));
        m_offset += 3;
        return true;
    }
    if (escaped == 'u' && peek(1) == '{') {
        m_offset += 2;
        char32_t codePoint = 0;
        unsigned numberOfDigits = 0;
        for (; numberOfDigits < maxCodePointDigits && isASCIIHexDigit(peek()); ++numberOfDigits, ++m_offset)
            codePoint = codePoint << 4 | toASCIIHexValue(peek());
        if (numberOfDigits && codePoint <= UCHAR_MAX_VALUE && accept('}')) {
            std::array<uint8_t, U8_MAX_LENGTH> encoded;
            size_t length = 0;
            U8_APPEND_UNSAFE(encoded, length, codePoint);
            bytes.append(std::span<const uint8_t> { encoded }.first(length));
            return true;
        }
    }
    fail(start, "unknown escape"_s);
    return false;
}

bool TypeTextParser::parseQuoted(Bytes& bytes)
{
    if (!accept('"')) {
        fail(m_offset, "expected a string"_s);
        return false;
    }
    while (!accept('"')) {
        char16_t character = peek();
        if (character < 0x20 || character > 0xff) {
            fail(m_offset, character < 0x20 ? "the string ends without a quote"_s : "a string may contain only bytes and escapes"_s);
            return false;
        }
        if (character == '\\') {
            if (!parseEscape(bytes))
                return false;
            continue;
        }
        bytes.append(static_cast<uint8_t>(character));
        ++m_offset;
    }
    return true;
}

StringID TypeTextParser::parseString()
{
    unsigned start = m_offset;
    Bytes bytes;
    if (!parseQuoted(bytes))
        return { };

    std::span<const uint8_t> canonicalBytes = bytes.span();
    Vector<uint8_t> reencoded;
    if (!isCanonicalProgramString(canonicalBytes)) {
        reencoded = encodeProgramString(decodeProgramString(canonicalBytes).span());
        canonicalBytes = reencoded.span();
    }
    const Program* program = m_arena.program();
    StringID string = program ? program->strings.find(canonicalBytes) : StringID();
    if (!string)
        fail(start, makeString("the program has no string "_s, m_text.substring(start, m_offset - start)));
    return string;
}

bool TypeTextParser::isAtSymbolKey() const
{
    char16_t second = peek(1);
    if (peek() == '@')
        return second == '@' || second == '#' || second == '"' || second == '-';
    if (peek() == '#')
        return second == '"' || second == '-';

    StringView name = word();
    char16_t afterName = peek(name.length());
    if (name == "Symbol"_s)
        return afterName == '.';
    if (name != "symbol"_s)
        return false;
    if (afterName != '@')
        return afterName == '(';
    char16_t afterAt = peek(name.length() + 1);
    return afterAt == '-' || (afterAt == letterOf(IDPrefix::Module) && isASCIIDigit(peek(name.length() + 2)));
}

std::optional<WellKnownSymbol> TypeTextParser::parseWellKnownSymbol()
{
    StringView name = word();
    for (WellKnownSymbol symbol : allMembersOf<WellKnownSymbol>) {
        StringView enumerator = nameOf(symbol);
        if (name.length() != enumerator.length() || name[0] != toASCIILower(enumerator[0]) || name.substring(1) != enumerator.substring(1))
            continue;
        m_offset += name.length();
        return symbol;
    }
    fail(m_offset, "expected a well-known symbol"_s);
    return std::nullopt;
}

PropertyKeyID TypeTextParser::parseSymbolKey()
{
    unsigned start = m_offset;
    PropertyKey key;
    if (accept('@')) {
        if (accept('@')) {
            key.kind = PropertyKeyKind::WellKnownSymbol;
            key.symbol = parseWellKnownSymbol();
        } else {
            key.kind = accept('#') ? PropertyKeyKind::ModulePrivateName : PropertyKeyKind::EnginePrivateName;
            key.text = parseOptionalString();
        }
    } else if (accept('#')) {
        key.kind = PropertyKeyKind::PrivateName;
        key.text = parseOptionalString();
        expect('@');
        if (!accept('-'))
            key.owner = parseLayout();
    } else if (acceptWord("symbol"_s)) {
        key.kind = PropertyKeyKind::CreatedSymbol;
        if (accept('(')) {
            key.text = parseString();
            expect(')');
        }
        expect('@');
        if (!accept('-'))
            key.module = parseModule();
        expect(':');
        if (!accept('-'))
            key.callNumber = parseNumber();
    } else if (acceptWord("Symbol"_s)) {
        key.kind = PropertyKeyKind::RegisteredSymbol;
        expect('.');
        if (!acceptWord("for"_s))
            fail(m_offset, "expected `for`"_s);
        expect('(');
        key.text = parseOptionalString();
        expect(')');
    } else
        fail(start, "expected a symbol key"_s);
    if (hasFailed())
        return { };

    std::optional<PropertyKeyID> found = m_arena.table().find(key);
    if (!found) {
        fail(start, makeString("the program has no key "_s, m_text.substring(start, m_offset - start)));
        return { };
    }
    return *found;
}

uint32_t TypeTextParser::numberOf(ObjectKind kind) const
{
    const ObjectTable& objects = m_arena.objects();
    return objects.endOf(kind) > objects.firstOf(kind) ? objects.endOf(kind) - objects.firstOf(kind) : 0;
}

ObjectID TypeTextParser::objectAt(ObjectKind kind, uint32_t number) const
{
    return number < numberOf(kind) ? ObjectID(m_arena.objects().firstOf(kind) + number) : ObjectID();
}

ObjectID TypeTextParser::objectOfFunction(ObjectKind kind, FunctionID function) const
{
    const ObjectTable& objects = m_arena.objects();
    if (!function || function.index() >= numberOf(kind))
        return { };
    switch (kind) {
    case ObjectKind::Closure:
        return objects.closureOf(function);
    case ObjectKind::CoroutineResult:
        return objects.coroutineResultOf(function);
    case ObjectKind::RestArray:
        return objects.restArrayOf(function);
    case ObjectKind::FunctionPrototype:
        return objects.prototypeObjectOf(function);
    case ObjectKind::ConstructedObject:
        return objects.constructedObjectOf(function);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

ObjectID TypeTextParser::objectOfLayout(ObjectKind kind, LayoutID layout) const
{
    const ObjectTable& objects = m_arena.objects();
    if (!layout || layout.index() >= numberOf(kind))
        return { };
    switch (kind) {
    case ObjectKind::Instance:
        return objects.instanceOf(layout);
    case ObjectKind::Prototype:
        return objects.prototypeOf(layout);
    case ObjectKind::EscapedOfLayout:
        return objects.escapedOf(layout);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

ObjectID TypeTextParser::parseObjectOfKind(ObjectKind kind)
{
    const ObjectTable& objects = m_arena.objects();
    switch (kind) {
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin: {
        BuiltinID builtin = parseBuiltin();
        if (!acceptComma())
            return builtin ? objects.objectOf(builtin) : ObjectID();
        ModuleID module = parseModule();
        if (hasFailed())
            return { };
        ObjectID object = objects.objectOf(builtin, module);
        return object && objects.kindOf(object) == ObjectKind::ModuleBuiltin ? object : ObjectID();
    }
    case ObjectKind::ResultOfBuiltin: {
        BuiltinID builtin = parseBuiltin();
        expectComma();
        uint32_t ordinal = parseNumber(largestResultOrdinal);
        return hasFailed() ? ObjectID() : objects.resultOf(builtin, ordinal);
    }
    case ObjectKind::HostObject:
    case ObjectKind::EscapedWithoutShape:
        return objectAt(kind, parseNumber());
    case ObjectKind::EscapedFunctionsOfHolder: {
        BuiltinID holder = parseBuiltin();
        return holder ? objects.escapedFunctionsOf(holder) : ObjectID();
    }
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedOfLayout:
        if (isAtLayout())
            return objectOfLayout(ObjectKind::EscapedOfLayout, parseLayout());
        return objectAt(ObjectKind::EscapedOfClass, parseBuiltinClass().index());
    case ObjectKind::Closure:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
        return objectOfFunction(kind, parseFunction());
    case ObjectKind::Namespace:
        return objectAt(kind, parseModule().index());
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
        return objectOfLayout(kind, parseLayout());
    case ObjectKind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ObjectID TypeTextParser::parseNamedObject()
{
    unsigned start = m_offset;
    StringView name = word();
    ObjectID object;
    if (numberInID(name, IDPrefix::Function))
        object = parseObjectOfKind(ObjectKind::Closure);
    else {
        const NamedObjectWord* form = nullptr;
        for (const NamedObjectWord& candidate : namedObjectWords) {
            if (name == candidate.word && peek(name.length()) == '(')
                form = &candidate;
        }
        if (!form)
            return { };
        m_offset += name.length();
        expectOpening('(');
        object = parseObjectOfKind(form->kind);
        expectClosing(')');
    }
    if (!object)
        fail(start, makeString("the build has no object "_s, m_text.substring(start, m_offset - start)));
    return hasFailed() ? ObjectID() : object;
}

void TypeTextParser::parseSeeds(UnknownSeeds& seeds)
{
    if (hasFailed() || !accept('?'))
        return;
    do {
        StringView name = word();
        std::optional<UnknownSeed> seed = seedNamed(name);
        if (!seed) {
            fail(m_offset, makeString("`unknown` has no seed `"_s, name, '`'));
            return;
        }
        seeds.add(*seed);
        m_offset += name.length();
    } while (accept('+'));
}

void TypeTextParser::parseClassPart(TypeArena& arena, ParsedParts& parsed)
{
    unsigned start = m_offset;
    ClassPart part { parseBuiltinClass(), { } };
    if (hasFailed())
        return;

    if (accept('<')) {
        skipBlanks();
        unsigned maxContents = 0;
        for (ContentsSlot slot : allMembersOf<ContentsSlot>) {
            if (std::optional<uint8_t> position = positionInTypeOf(m_arena.builtins(), part.builtinClass, slot))
                maxContents = std::max<unsigned>(maxContents, *position + 1);
        }
        maxContents = std::min(maxContents, mostContentsOfClass);
        unsigned numberOfContents = 0;
        do {
            if (numberOfContents == maxContents) {
                fail(m_offset, makeString("the class has "_s, maxContents, " contents"_s));
                return;
            }
            std::optional<TypeID> contents = parseType(arena);
            if (!contents)
                return;
            part.writtenContents[numberOfContents++] = *contents;
        } while (acceptComma());
        expectClosing('>');
        if (hasFailed())
            return;
    }

    auto place = std::ranges::lower_bound(parsed.classes, part.builtinClass, { }, &ClassPart::builtinClass);
    if (place != parsed.classes.end() && place->builtinClass == part.builtinClass) {
        fail(start, "the type names the class twice"_s);
        return;
    }
    parsed.classes.insert(place - parsed.classes.begin(), part);
}

bool TypeTextParser::parsePart(TypeArena& arena, ParsedParts& parsed)
{
    TypeParts& parts = parsed.parts;
    unsigned start = m_offset;

    if (peek() == '"') {
        if (StringID string = parseString())
            parsed.strings.append(string);
        return !hasFailed();
    }
    if (isASCIIDigit(peek()) || (peek() == '-' && isASCIIDigit(peek(1)))) {
        addIntegers(parts, Range::one(parseInteger()));
        return !hasFailed();
    }
    if (isAtSymbolKey()) {
        if (PropertyKeyID key = parseSymbolKey())
            parsed.symbols.append(key);
        return !hasFailed();
    }

    StringView name = word();
    if (name.isEmpty())
        return false;

    if (isAtLayout()) {
        if (LayoutID layout = parseLayout())
            parsed.layouts.append(layout);
        parseSeeds(parts.seeds);
        return !hasFailed();
    }
    if (ObjectID object = parseNamedObject()) {
        parsed.objects.append(object);
        parseSeeds(parts.seeds);
        return !hasFailed();
    }
    if (hasFailed())
        return false;

    if (acceptWord("int"_s)) {
        addIntegers(parts, parseIntegerBounds());
        return !hasFailed();
    }
    if (acceptWord("i32"_s)) {
        addIntegers(parts, Range::int32());
        return true;
    }
    if (name == "fractional"_s && peek(name.length()) == '[') {
        m_offset += name.length();
        parts.primitives |= Primitive::FractionalNumber;
        parts.boundsOfFractions = parts.boundsOfFractions.unionWith(parseIntegerBounds());
        return !hasFailed();
    }
    for (const PrimitiveWord& primitive : primitiveWords) {
        if (!acceptWord(primitive.word))
            continue;
        parts.primitives |= primitive.primitives;
        if (primitive.primitives & Primitive::Integer)
            parts.integers = Range::full();
        if (primitive.primitives & Primitive::FractionalNumber)
            parsed.holdsFractionsWithoutBounds = true;
        return true;
    }

    bool isAnything = acceptWord("anything"_s);
    if (isAnything || acceptWord("object"_s)) {
        if (isAnything) {
            parts.primitives |= Primitive::All;
            parts.integers = Range::full();
            parsed.holdsFractionsWithoutBounds = true;
        }
        parts.anyObject = true;
        parseSeeds(parts.seeds);
        return !hasFailed();
    }
    if (acceptWord("function"_s)) {
        parts.anyFunction = true;
        parseSeeds(parts.seeds);
        return !hasFailed();
    }

    if (acceptWord("env"_s)) {
        if (parts.environment)
            fail(start, "a type has at most one environment"_s);
        if (peek() != ' ')
            fail(m_offset, "expected a space"_s);
        skipBlanks();
        parts.environment = parseLayout();
        return !hasFailed();
    }
    if (name == "opaque"_s && peek(name.length()) == '(') {
        if (parts.opaque)
            fail(start, "a type has at most one opaque kind"_s);
        m_offset += name.length();
        expectOpening('(');
        StringView kindName = word();
        parts.opaque = parseEnum<OpaqueKind>(kindName);
        if (!parts.opaque)
            fail(m_offset, makeString("unknown opaque kind `"_s, kindName, '`'));
        m_offset += kindName.length();
        expectClosing(')');
        return !hasFailed();
    }

    parseClassPart(arena, parsed);
    parseSeeds(parts.seeds);
    return !hasFailed();
}

std::optional<TypeID> TypeTextParser::parseType(TypeArena& arena)
{
    unsigned start = m_offset;
    SetForScope nesting(m_nesting, m_nesting + 1);
    if (m_nesting > maxTypeNestingDepth) {
        fail(start, "types are nested too deeply"_s);
        return std::nullopt;
    }
    if (acceptWord("never"_s))
        return TypeTable::never;

    ParsedParts parsed;
    if (!parsePart(arena, parsed)) {
        fail(start, "expected a type"_s);
        return std::nullopt;
    }
    while (peek() == '|') {
        unsigned bar = m_offset++;
        if (parsePart(arena, parsed))
            continue;
        if (hasFailed())
            return std::nullopt;
        m_offset = bar;
        break;
    }

    TypeParts& parts = parsed.parts;
    bool hasUnknown = parts.anyObject || parts.anyFunction || !parsed.layouts.isEmpty() || !parsed.classes.isEmpty();
    bool hasJavaScriptValues = hasUnknown || parts.primitives & ~(Primitive::Absent | Primitive::Unspecified) || !parsed.strings.isEmpty() || !parsed.symbols.isEmpty() || !parsed.objects.isEmpty();
    if ((parts.opaque || parts.environment) && (hasJavaScriptValues || (parts.opaque && parts.environment))) {
        fail(start, "an environment or an opaque value can only be combined with absent and unspecified"_s);
        return std::nullopt;
    }
    if (!parts.seeds.isEmpty() && !hasUnknown) {
        fail(start, "a type without `unknown` has seeds"_s);
        return std::nullopt;
    }

    if (parsed.holdsFractionsWithoutBounds)
        parts.boundsOfFractions = Range::empty();
    parts.stringLiterals = setOf(arena.stringSets(), parsed.strings);
    parts.symbolLiterals = setOf(arena.keySets(), parsed.symbols);
    parts.objects = setOf(arena.objectSets(), parsed.objects);
    parts.unnamedLayouts = setOf(arena.layoutSets(), parsed.layouts);
    if (!parsed.classes.isEmpty())
        parts.unnamedClasses = arena.internClassParts(parsed.classes.span());
    return arena.intern(parts);
}

std::optional<ObjectID> TypeTextParser::parseObject()
{
    unsigned start = m_offset;
    if (acceptWord("unknown"_s))
        return ObjectTable::unknown();
    ObjectID object = parseNamedObject();
    if (!object) {
        fail(start, "expected an object"_s);
        return std::nullopt;
    }
    return object;
}

std::optional<LayoutSetID> TypeTextParser::parseLayoutSet(TypeArena& arena)
{
    if (accept('-'))
        return LayoutSetID();
    if (!accept('[')) {
        fail(m_offset, "expected a set of layouts"_s);
        return std::nullopt;
    }
    skipBlanks();

    Vector<LayoutID, 16> members;
    if (!accept(']')) {
        do
            members.append(parseLayout());
        while (!hasFailed() && acceptComma());
        expectClosing(']');
    }
    if (hasFailed())
        return std::nullopt;
    return setOf(arena.layoutSets(), members);
}

std::optional<TypeID> parseType(TypeArena& arena, StringView text, unsigned& offset, String& error)
{
    TypeTextParser parser(arena, text, offset);
    return parser.finish(parser.parseType(arena), offset, error);
}

std::optional<ObjectID> parseObject(const TypeArena& arena, StringView text, unsigned& offset, String& error)
{
    TypeTextParser parser(arena, text, offset);
    return parser.finish(parser.parseObject(), offset, error);
}

std::optional<LayoutSetID> parseLayoutSet(TypeArena& arena, StringView text, unsigned& offset, String& error)
{
    TypeTextParser parser(arena, text, offset);
    return parser.finish(parser.parseLayoutSet(arena), offset, error);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
