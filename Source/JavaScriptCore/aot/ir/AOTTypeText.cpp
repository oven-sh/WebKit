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
#include "AOTSort.h"
#include "AOTStringTable.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <climits>
#include <compare>
#include <functional>
#include <tuple>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/CommaPrinter.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeTextPrinter {
    WTF_MAKE_NONCOPYABLE(TypeTextPrinter);
public:
    TypeTextPrinter(PrintStream& out, TypeReader types, TypeTextOptions options)
        : m_out(out)
        , m_types(types)
        , m_options(options)
    {
    }

    void printType(TypeID);
    void printObject(ObjectID);
    void printLayoutSet(LayoutSetID);
    void printObjectSet(ObjectSetID);

private:
    using LayoutList = Vector<LayoutID, 16>;

    enum class UnknownPart : uint8_t { Word, Layouts, Classes };

    struct CodePoint {
        char32_t character;
        unsigned length;
    };

    static bool needsEscape(uint8_t byte) { return byte < 0x20 || byte >= 0x7f || byte == '"' || byte == '\\'; }
    static std::optional<CodePoint> decodeCodePoint(std::span<const uint8_t>);
    static bool isLayoutOfBuiltinOrNamespace(const Layout& layout) { return layout.kind == LayoutKind::BuiltinResult || layout.kind == LayoutKind::ModuleNamespace; }

    bool writesIRText() const { return m_options.namesFamiliesOfLayouts; }

    std::span<const uint8_t> bytesOf(StringID) const;
    bool isPrintedBefore(StringID, StringID) const;
    bool isPrintedBefore(PropertyKeyID, PropertyKeyID) const;
    bool isPrintedBefore(ObjectID, ObjectID) const;
    bool isNamedInParentheses(LayoutID) const;
    void replaceByNamesOfFamilies(const Program&, LayoutList& ascendingMembers) const;

    void printInvalid(ASCIILiteral kind, uint64_t number);
    void printID(IDPrefix, uint32_t index);
    void printQuoted(std::span<const uint8_t>);
    void printString(StringID);
    void printKey(PropertyKeyID);
    void printLayout(LayoutID);
    void printBuiltin(BuiltinID);
    void printBuiltinClass(BuiltinClassID);
    void printForm(ASCIILiteral word, IDPrefix, uint32_t index);

    void printPrimitives(const Type&, CommaPrinter& bar);
    void printStringLiterals(const Type&, CommaPrinter& bar);
    void printSymbolLiterals(const Type&, CommaPrinter& bar);
    void printUnknownWord(const Type&, CommaPrinter& bar);
    void printUnnamedLayouts(const Type&, CommaPrinter& bar);
    void printUnnamedClasses(const Type&, CommaPrinter& bar);
    void printClassPart(const ClassPart&);
    void printSeedsBehind(UnknownPart, const Type&);
    void printLayouts(const LayoutList&, CommaPrinter& separator);
    void printObjects(ObjectSetID, CommaPrinter& separator);

    PrintStream& m_out;
    TypeReader m_types;
    TypeTextOptions m_options;
};

std::optional<TypeTextPrinter::CodePoint> TypeTextPrinter::decodeCodePoint(std::span<const uint8_t> bytes)
{
    static constexpr std::array<char32_t, 5> smallestOfLength { 0, 0, 0x80, 0x800, 0x10000 };
    uint8_t lead = bytes[0];
    if ((lead >= 0x80 && lead < 0xc2) || lead > 0xf4)
        return std::nullopt;
    unsigned length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (length > bytes.size())
        return std::nullopt;
    char32_t character = length == 1 ? lead : lead & (0x7f >> length);
    for (unsigned index = 1; index < length; ++index) {
        if ((bytes[index] & 0xc0) != 0x80)
            return std::nullopt;
        character = character << 6 | (bytes[index] & 0x3f);
    }
    if (character < smallestOfLength[length] || character > 0x10ffff)
        return std::nullopt;
    return CodePoint { character, length };
}

std::span<const uint8_t> TypeTextPrinter::bytesOf(StringID string) const
{
    const Program* program = m_types.program();
    if (!program || string.index() >= program->strings.size())
        return { };
    return program->strings[string];
}

bool TypeTextPrinter::isPrintedBefore(StringID a, StringID b) const
{
    return compareSpans(bytesOf(a), bytesOf(b)) < 0;
}

bool TypeTextPrinter::isPrintedBefore(PropertyKeyID a, PropertyKeyID b) const
{
    const PropertyKey& first = m_types.at(a);
    const PropertyKey& second = m_types.at(b);
    if (first.kind != second.kind)
        return first.kind < second.kind;
    if (std::strong_ordering order = compareSpans(bytesOf(first.text), bytesOf(second.text)); order != 0)
        return order < 0;
    auto restOf = [](const PropertyKey& key) {
        return std::tuple(!key.text, key.owner, key.symbol, key.module, key.callNumber);
    };
    return restOf(first) < restOf(second);
}

bool TypeTextPrinter::isPrintedBefore(ObjectID a, ObjectID b) const
{
    auto numbersOf = [&](const ObjectRecord& record) {
        bool standsBehindClosures = !writesIRText() || record.kind != ObjectKind::Closure;
        return std::tuple(standsBehindClosures, record.kind, record.payload, record.module, record.ordinal);
    };
    return numbersOf(m_types.at(a)) < numbersOf(m_types.at(b));
}

bool TypeTextPrinter::isNamedInParentheses(LayoutID layout) const
{
    const Program* program = m_types.program();
    return program && layout.index() < program->layouts.size() && isLayoutOfBuiltinOrNamespace(program->at(layout));
}

void TypeTextPrinter::replaceByNamesOfFamilies(const Program& program, LayoutList& ascendingMembers) const
{
    auto familyOf = [&](LayoutID layout) {
        return m_types.at(program.declaredMembersOf(layout));
    };
    auto placeOf = [&](LayoutID member) {
        auto place = std::ranges::lower_bound(ascendingMembers, member);
        ASSERT(place != ascendingMembers.end() && *place == member);
        return static_cast<size_t>(place - ascendingMembers.begin());
    };

    LayoutList candidates;
    for (LayoutID member : ascendingMembers) {
        if (member.index() >= program.layouts.size() || program.at(member).isDerived() || !program.hasDeclaredMembers(member))
            continue;
        bool familyLiesInside = familyOf(member).allOf([&](LayoutID memberOfFamily) {
            return std::ranges::binary_search(ascendingMembers, memberOfFamily);
        });
        if (familyLiesInside)
            candidates.append(member);
    }
    AOT::sort(candidates, std::ranges::greater { }, [&](LayoutID candidate) {
        return familyOf(candidate).size();
    });

    LayoutList names;
    Vector<bool, 16> isAccountedFor(FillWith { }, ascendingMembers.size(), false);
    for (LayoutID candidate : candidates) {
        bool bringsNewMember = false;
        familyOf(candidate).forEach([&](LayoutID member) {
            bringsNewMember |= !std::exchange(isAccountedFor[placeOf(member)], true);
        });
        if (bringsNewMember)
            names.append(candidate);
    }
    for (size_t place = 0; place < ascendingMembers.size(); ++place) {
        if (!isAccountedFor[place])
            names.append(ascendingMembers[place]);
    }
    AOT::sort(names);
    removeRepeatedElements(names);
    ascendingMembers = WTF::move(names);
}

void TypeTextPrinter::printInvalid(ASCIILiteral kind, uint64_t number)
{
    m_out.print("<invalid "_s, kind, " "_s, number, ">"_s);
}

void TypeTextPrinter::printID(IDPrefix prefix, uint32_t index)
{
    if (index == UINT32_MAX) {
        m_out.print("-"_s);
        return;
    }
    m_out.print(letterOf(prefix), index);
}

void TypeTextPrinter::printQuoted(std::span<const uint8_t> bytes)
{
    m_out.print("\""_s);
    while (!bytes.empty()) {
        uint8_t byte = bytes.front();
        size_t length = 1;
        if (!needsEscape(byte)) {
            length = std::min<size_t>(std::ranges::find_if(bytes, needsEscape) - bytes.begin(), INT_MAX);
            m_out.print(byteCast<char>(bytes.first(length)));
        } else if (byte == '"')
            m_out.print("\\\""_s);
        else if (byte == '\\')
            m_out.print("\\\\"_s);
        else if (byte == '\n')
            m_out.print("\\n"_s);
        else if (auto codePoint = decodeCodePoint(bytes)) {
            m_out.printf("\\u{%x}", static_cast<unsigned>(codePoint->character));
            length = codePoint->length;
        } else
            m_out.printf("\\x%02x", static_cast<unsigned>(byte));
        bytes = bytes.subspan(length);
    }
    m_out.print("\""_s);
}

void TypeTextPrinter::printString(StringID string)
{
    if (!string) {
        m_out.print("-"_s);
        return;
    }
    const Program* program = m_types.program();
    if (!program || string.index() >= program->strings.size()) {
        printInvalid("string"_s, string.index());
        return;
    }
    printQuoted(program->strings[string]);
}

void TypeTextPrinter::printKey(PropertyKeyID id)
{
    const PropertyKey& key = m_types.at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        printString(key.text);
        return;
    case PropertyKeyKind::EnginePrivateName:
        m_out.print("@"_s);
        printString(key.text);
        return;
    case PropertyKeyKind::ModulePrivateName:
        m_out.print("@#"_s);
        printString(key.text);
        return;
    case PropertyKeyKind::PrivateName:
        m_out.print("#"_s);
        printString(key.text);
        m_out.print("@"_s);
        printLayout(key.owner);
        return;
    case PropertyKeyKind::WellKnownSymbol: {
        m_out.print("@@"_s);
        if (!key.symbol) {
            m_out.print("-"_s);
            return;
        }
        std::span<const char> name = nameOf(*key.symbol).span();
        m_out.print(toASCIILower(name[0]), name.subspan(1));
        return;
    }
    case PropertyKeyKind::CreatedSymbol:
        m_out.print("symbol"_s);
        if (key.text) {
            m_out.print("("_s);
            printString(key.text);
            m_out.print(")"_s);
        }
        m_out.print("@"_s);
        printID(IDPrefix::Module, key.module.index());
        m_out.print(":"_s);
        if (key.callNumber)
            m_out.print(*key.callNumber);
        else
            m_out.print("-"_s);
        return;
    case PropertyKeyKind::RegisteredSymbol:
        m_out.print("Symbol.for("_s);
        printString(key.text);
        m_out.print(")"_s);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TypeTextPrinter::printLayout(LayoutID layout)
{
    const Program* program = m_types.program();
    if (!layout || !program) {
        printID(IDPrefix::Layout, layout.index());
        return;
    }
    if (layout.index() >= program->layouts.size()) {
        printInvalid("layout"_s, layout.index());
        return;
    }

    const Layout& record = program->at(layout);
    if (!isLayoutOfBuiltinOrNamespace(record)) {
        printID(IDPrefix::Layout, program->numberInTextOf(layout));
        return;
    }

    m_out.print(letterOf(IDPrefix::Layout), "("_s);
    if (record.kind == LayoutKind::ModuleNamespace)
        printID(IDPrefix::Module, record.namespaceOf.index());
    else if (m_types.builtins().contains(record.builtinLayout))
        m_out.print(m_types.builtins().tokenOf(record.builtinLayout));
    else
        printInvalid("layout"_s, record.builtinLayout.index());
    m_out.print(")"_s);
}

void TypeTextPrinter::printBuiltin(BuiltinID builtin)
{
    if (!m_types.builtins().contains(builtin)) {
        printInvalid("builtin"_s, builtin.index());
        return;
    }
    m_out.print(m_types.builtins().tokenOf(builtin));
}

void TypeTextPrinter::printBuiltinClass(BuiltinClassID builtinClass)
{
    if (!m_types.builtins().contains(builtinClass)) {
        printInvalid("class"_s, builtinClass.index());
        return;
    }
    m_out.print(m_types.builtins().tokenOf(builtinClass));
}

void TypeTextPrinter::printForm(ASCIILiteral word, IDPrefix prefix, uint32_t index)
{
    m_out.print(word, "("_s);
    if (prefix == IDPrefix::Layout)
        printLayout(LayoutID(index));
    else
        printID(prefix, index);
    m_out.print(")"_s);
}

void TypeTextPrinter::printPrimitives(const Type& type, CommaPrinter& bar)
{
    uint16_t primitives = type.primitives;
    if (primitives & Primitive::Undefined)
        m_out.print(bar, "undefined"_s);
    if (primitives & Primitive::Null)
        m_out.print(bar, "null"_s);
    if ((primitives & Primitive::Boolean) == Primitive::Boolean)
        m_out.print(bar, "boolean"_s);
    else if (primitives & Primitive::True)
        m_out.print(bar, "true"_s);
    else if (primitives & Primitive::False)
        m_out.print(bar, "false"_s);

    bool boundsFractions = !type.boundsOfFractions.isEmpty();
    if ((primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full() && !boundsFractions)
        m_out.print(bar, "number"_s);
    else {
        if (primitives & Primitive::Integer) {
            if (type.integers == Range::full())
                m_out.print(bar, "int"_s);
            else if (type.integers == Range::int32())
                m_out.print(bar, "i32"_s);
            else if (type.integers.isSingleValue())
                m_out.print(bar, type.integers.minimum);
            else
                m_out.print(bar, "int["_s, type.integers.minimum, ", "_s, type.integers.maximum, "]"_s);
        }
        if ((primitives & Primitive::Double) == Primitive::Double && !boundsFractions)
            m_out.print(bar, "double"_s);
        else {
            if (primitives & Primitive::WholeNumber)
                m_out.print(bar, "whole"_s);
            if (primitives & Primitive::FractionalNumber)
                m_out.print(bar, "fractional"_s);
            if (boundsFractions)
                m_out.print("["_s, type.boundsOfFractions.minimum, ", "_s, type.boundsOfFractions.maximum, "]"_s);
        }
    }

    if (primitives & Primitive::String)
        m_out.print(bar, "string"_s);
    if (primitives & Primitive::Symbol)
        m_out.print(bar, "symbol"_s);
    if (primitives & Primitive::BigInt)
        m_out.print(bar, "bigint"_s);
}

void TypeTextPrinter::printStringLiterals(const Type& type, CommaPrinter& bar)
{
    if (!type.stringLiterals)
        return;
    Vector<StringID, 8> literals;
    m_types.at(type.stringLiterals).forEach([&](StringID literal) {
        literals.append(literal);
    });
    AOT::sort(literals, [&](StringID a, StringID b) {
        return isPrintedBefore(a, b);
    });
    for (StringID literal : literals) {
        m_out.print(bar);
        printString(literal);
    }
}

void TypeTextPrinter::printSymbolLiterals(const Type& type, CommaPrinter& bar)
{
    if (!type.symbolLiterals)
        return;
    Vector<PropertyKeyID, 8> literals;
    m_types.at(type.symbolLiterals).forEach([&](PropertyKeyID literal) {
        literals.append(literal);
    });
    AOT::sort(literals, [&](PropertyKeyID a, PropertyKeyID b) {
        return isPrintedBefore(a, b);
    });
    for (PropertyKeyID literal : literals) {
        m_out.print(bar);
        printKey(literal);
    }
}

void TypeTextPrinter::printUnknownWord(const Type& type, CommaPrinter& bar)
{
    if (!type.hasUnboundedUnknown())
        return;
    m_out.print(bar, type.isAnything() ? "anything"_s : type.anyObject ? "object"_s : "function"_s);
    printSeedsBehind(UnknownPart::Word, type);
}

void TypeTextPrinter::printUnnamedLayouts(const Type& type, CommaPrinter& bar)
{
    if (!type.unnamedLayouts)
        return;
    LayoutList layouts;
    m_types.at(type.unnamedLayouts).forEach([&](LayoutID member) {
        layouts.append(member);
    });
    if (const Program* program = m_types.program(); program && m_options.namesFamiliesOfLayouts)
        replaceByNamesOfFamilies(*program, layouts);
    printLayouts(layouts, bar);
    printSeedsBehind(UnknownPart::Layouts, type);
}

void TypeTextPrinter::printUnnamedClasses(const Type& type, CommaPrinter& bar)
{
    if (!type.unnamedClasses)
        return;
    for (const ClassPart& part : m_types.at(type.unnamedClasses)) {
        m_out.print(bar);
        printClassPart(part);
    }
    printSeedsBehind(UnknownPart::Classes, type);
}

void TypeTextPrinter::printClassPart(const ClassPart& part)
{
    printBuiltinClass(part.builtinClass);

    unsigned numberOfContents = mostContentsOfClass;
    while (numberOfContents && (!part.writtenContents[numberOfContents - 1] || part.writtenContents[numberOfContents - 1] == TypeTable::anything))
        --numberOfContents;
    if (!numberOfContents)
        return;
    if (writesIRText() && m_types.builtins().contains(part.builtinClass))
        numberOfContents = std::max(numberOfContents, std::min(m_types.builtins().numberOfSlotsInType(part.builtinClass), mostContentsOfClass));

    m_out.print("<"_s);
    CommaPrinter comma;
    for (unsigned index = 0; index < numberOfContents; ++index) {
        m_out.print(comma);
        printType(part.writtenContents[index] ? part.writtenContents[index] : TypeTable::anything);
    }
    m_out.print(">"_s);
}

void TypeTextPrinter::printSeedsBehind(UnknownPart part, const Type& type)
{
    UnknownPart lastPart = type.unnamedClasses ? UnknownPart::Classes : type.unnamedLayouts ? UnknownPart::Layouts : UnknownPart::Word;
    if (part != lastPart || !m_options.printsSeeds)
        return;
    CommaPrinter plus("+"_s, "?"_s);
    for (UnknownSeed seed : type.seeds)
        m_out.print(plus, nameOf(seed));
}

void TypeTextPrinter::printLayouts(const LayoutList& layouts, CommaPrinter& separator)
{
    for (bool printsNamesInParentheses : { false, true }) {
        for (LayoutID layout : layouts) {
            if (isNamedInParentheses(layout) != printsNamesInParentheses)
                continue;
            m_out.print(separator);
            printLayout(layout);
        }
    }
}

void TypeTextPrinter::printObjects(ObjectSetID set, CommaPrinter& separator)
{
    Vector<ObjectID, 16> members;
    m_types.at(set).forEach([&](ObjectID member) {
        members.append(member);
    });
    AOT::sort(members, [&](ObjectID a, ObjectID b) {
        return isPrintedBefore(a, b);
    });

    size_t numberPrinted = std::min<size_t>(members.size(), m_options.mostMembersPrinted);
    for (ObjectID member : members.span().first(numberPrinted)) {
        m_out.print(separator);
        printObject(member);
    }
    if (numberPrinted < members.size())
        m_out.print(separator, "...+"_s, members.size() - numberPrinted);
}

void TypeTextPrinter::printType(TypeID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }

    Type type = m_types.at(id);
    CommaPrinter bar("|"_s);
    if (!type.isAnything())
        printPrimitives(type, bar);
    if (writesIRText())
        printUnknownWord(type, bar);
    if (type.primitives & Primitive::Absent)
        m_out.print(bar, "absent"_s);
    if (type.primitives & Primitive::Unspecified)
        m_out.print(bar, "unspecified"_s);
    printStringLiterals(type, bar);
    printSymbolLiterals(type, bar);
    if (!writesIRText())
        printUnknownWord(type, bar);
    printUnnamedLayouts(type, bar);
    if (!writesIRText())
        printUnnamedClasses(type, bar);
    if (type.objects)
        printObjects(type.objects, bar);
    if (writesIRText())
        printUnnamedClasses(type, bar);

    if (type.environment) {
        m_out.print(bar, "env "_s);
        printLayout(type.environment);
    }
    if (type.opaque) {
        ASCIILiteral name = nameOf(*type.opaque);
        if (!writesIRText())
            m_out.print(bar, "opaque("_s, name, ")"_s);
        else {
            m_out.print(bar);
            for (char character : name.span())
                m_out.print(toASCIILower(character));
        }
    }

    if (!bar.didPrint())
        m_out.print("never"_s);
}

void TypeTextPrinter::printObject(ObjectID object)
{
    if (!object) {
        m_out.print("-"_s);
        return;
    }

    const ObjectRecord& record = m_types.at(object);
    switch (record.kind) {
    case ObjectKind::Unknown:
        m_out.print("unknown"_s);
        break;
    case ObjectKind::Builtin:
        m_out.print("builtin("_s);
        printBuiltin(BuiltinID(record.payload));
        m_out.print(")"_s);
        break;
    case ObjectKind::ModuleBuiltin:
        m_out.print("builtin("_s);
        printBuiltin(BuiltinID(record.payload));
        m_out.print(", "_s);
        printID(IDPrefix::Module, record.module.index());
        m_out.print(")"_s);
        break;
    case ObjectKind::ResultOfBuiltin:
        m_out.print("result("_s);
        printBuiltin(BuiltinID(record.payload));
        m_out.print(", "_s, record.ordinal, ")"_s);
        break;
    case ObjectKind::HostObject:
        m_out.print("host("_s, record.payload, ")"_s);
        break;
    case ObjectKind::EscapedFunctionsOfHolder:
        m_out.print("escapedFunctions("_s);
        printBuiltin(BuiltinID(record.payload));
        m_out.print(")"_s);
        break;
    case ObjectKind::EscapedOfClass:
        m_out.print("escaped("_s);
        printBuiltinClass(BuiltinClassID(record.payload));
        m_out.print(")"_s);
        break;
    case ObjectKind::EscapedWithoutShape:
        m_out.print("escapedWithoutShape("_s, record.payload, ")"_s);
        break;
    case ObjectKind::Closure:
        printID(IDPrefix::Function, record.payload);
        break;
    case ObjectKind::CoroutineResult:
        printForm("coroutine"_s, IDPrefix::Function, record.payload);
        break;
    case ObjectKind::RestArray:
        printForm("rest"_s, IDPrefix::Function, record.payload);
        break;
    case ObjectKind::FunctionPrototype:
        printForm("prototypeOf"_s, IDPrefix::Function, record.payload);
        break;
    case ObjectKind::ConstructedObject:
        printForm("constructed"_s, IDPrefix::Function, record.payload);
        break;
    case ObjectKind::Namespace:
        printForm("namespace"_s, IDPrefix::Module, record.payload);
        break;
    case ObjectKind::Instance:
        printForm("instance"_s, IDPrefix::Layout, record.payload);
        break;
    case ObjectKind::Prototype:
        printForm("prototype"_s, IDPrefix::Layout, record.payload);
        break;
    case ObjectKind::EscapedOfLayout:
        printForm("escaped"_s, IDPrefix::Layout, record.payload);
        break;
    }
}

void TypeTextPrinter::printLayoutSet(LayoutSetID set)
{
    if (!set) {
        m_out.print("-"_s);
        return;
    }
    LayoutList members;
    m_types.at(set).forEach([&](LayoutID member) {
        members.append(member);
    });
    m_out.print("["_s);
    CommaPrinter comma;
    printLayouts(members, comma);
    m_out.print("]"_s);
}

void TypeTextPrinter::printObjectSet(ObjectSetID set)
{
    if (!set) {
        m_out.print("-"_s);
        return;
    }
    m_out.print("["_s);
    CommaPrinter comma;
    printObjects(set, comma);
    m_out.print("]"_s);
}

void printType(PrintStream& out, TypeReader types, TypeID type, TypeTextOptions options)
{
    TypeTextPrinter(out, types, options).printType(type);
}

void printObject(PrintStream& out, TypeReader types, ObjectID object)
{
    TypeTextPrinter(out, types, { }).printObject(object);
}

void printLayoutSet(PrintStream& out, TypeReader types, LayoutSetID set)
{
    TypeTextPrinter(out, types, { }).printLayoutSet(set);
}

void printObjectSet(PrintStream& out, TypeReader types, ObjectSetID set, TypeTextOptions options)
{
    TypeTextPrinter(out, types, options).printObjectSet(set);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
