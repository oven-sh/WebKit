/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && (ASSERT_ENABLED || defined(AOT_IR_PRINTER_FOR_TESTAOT))

#include "AOTIRPrinter.h"

#include "AOTBuiltinTable.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <climits>
#include <tuple>
#include <wtf/ASCIICType.h>
#include <wtf/CommaPrinter.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct CodePoint {
    char32_t character;
    unsigned length;
};

static std::optional<CodePoint> decodeCodePoint(std::span<const uint8_t> bytes)
{
    static constexpr char32_t minCodePointForLength[] = { 0, 0, 0x80, 0x800, 0x10000 };
    uint8_t lead = bytes[0];
    if ((lead >= 0x80 && lead < 0xc2) || lead > 0xf4)
        return std::nullopt;
    unsigned length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (length > bytes.size())
        return std::nullopt;
    char32_t character = length == 1 ? lead : lead & (0x7f >> length);
    for (unsigned k = 1; k < length; ++k) {
        if ((bytes[k] & 0xc0) != 0x80)
            return std::nullopt;
        character = character << 6 | (bytes[k] & 0x3f);
    }
    if (character < minCodePointForLength[length] || character > 0x10ffff)
        return std::nullopt;
    return CodePoint { character, length };
}

static bool isInTable(const StringTable& strings, StringID id)
{
    return StringTable::isProvisional(id) ? strings.hasOpenPhase() : id.index() < strings.size();
}

template<typename IDType>
static bool isInTable(const TypeTable& types, IDType id, unsigned numberOfRecords)
{
    return TypeTable::isProvisional(id) ? types.hasOpenPhase() : id.index() < numberOfRecords;
}

static std::span<const uint8_t> bytesOf(const StringTable& strings, StringID id)
{
    return isInTable(strings, id) ? strings[id] : std::span<const uint8_t>();
}

static bool isPrintedBefore(const StringTable& strings, StringID a, StringID b)
{
    return compareSpans(bytesOf(strings, a), bytesOf(strings, b)) < 0;
}

static bool isPrintedBefore(const StringTable& strings, const PropertyKey& a, const PropertyKey& b)
{
    if (a.kind != b.kind)
        return a.kind < b.kind;
    if (auto order = compareSpans(bytesOf(strings, a.text), bytesOf(strings, b.text)); order != 0)
        return order < 0;
    return std::tie(a.symbol, a.module, a.position) < std::tie(b.symbol, b.module, b.position);
}

static bool isPrintedBefore(const StringTable& strings, RegExpLiteralInType a, RegExpLiteralInType b)
{
    if (auto order = compareSpans(bytesOf(strings, a.pattern), bytesOf(strings, b.pattern)); order != 0)
        return order < 0;
    return a.rawFlags < b.rawFlags;
}

static bool isBuiltinOrNamespaceLayout(const Program& program, LayoutID id)
{
    if (id.index() >= program.layouts.size())
        return false;
    LayoutKind kind = program.at(id).kind;
    return kind == LayoutKind::BuiltinResult || kind == LayoutKind::ModuleNamespace;
}

static void visitInPrintedOrder(const Program& program, std::span<const LayoutID> layouts, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    for (bool visitsBuiltinAndNamespaceLayouts : { false, true }) {
        for (LayoutID layout : layouts) {
            if (isBuiltinOrNamespaceLayout(program, layout) == visitsBuiltinAndNamespaceLayouts)
                functor(layout);
        }
    }
}

void IRPrinter::print(StringID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (!isInTable(m_program.strings, id)) {
        printInvalid("string"_s, id.index());
        return;
    }
    printQuoted(m_program.string(id));
}

void IRPrinter::printQuoted(std::span<const uint8_t> bytes)
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
            m_out.printf("\\x%02x", byte);
        bytes = bytes.subspan(length);
    }
    m_out.print("\""_s);
}

void IRPrinter::print(PropertyKeyID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (!isInTable(m_program.types, id, m_program.types.numberOfPropertyKeys())) {
        printInvalid("key"_s, id.index());
        return;
    }

    const PropertyKey& key = m_program.at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        print(key.text);
        return;
    case PropertyKeyKind::EnginePrivateName:
        m_out.print("@"_s);
        print(key.text);
        return;
    case PropertyKeyKind::PrivateName:
        m_out.print("#"_s);
        print(key.text);
        m_out.print("@"_s);
        print(key.owner);
        return;
    case PropertyKeyKind::WellKnownSymbol:
        m_out.print("@@"_s);
        if (!key.symbol)
            m_out.print("-"_s);
        else if (auto symbolNumber = static_cast<unsigned>(*key.symbol); symbolNumber >= numberOfEnumerators<WellKnownSymbol>)
            printInvalid("enumerator"_s, symbolNumber);
        else {
            std::span<const char> name = nameOf(*key.symbol).span();
            m_out.print(toASCIILower(name[0]), name.subspan(1));
        }
        return;
    case PropertyKeyKind::CreatedSymbol:
        m_out.print("symbol"_s);
        if (key.text) {
            m_out.print("("_s);
            print(key.text);
            m_out.print(")"_s);
        }
        m_out.print("@"_s);
        print(key.module);
        m_out.print(":"_s);
        if (key.position)
            print(*key.position);
        else
            m_out.print("-"_s);
        return;
    case PropertyKeyKind::RegisteredSymbol:
        m_out.print("Symbol.for("_s);
        print(key.text);
        m_out.print(")"_s);
        return;
    }
    printInvalid("enumerator"_s, static_cast<uint64_t>(key.kind));
}

void IRPrinter::print(TypeID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (!isInTable(m_program.types, id, m_program.types.size())) {
        printInvalid("type"_s, id.index());
        return;
    }
    printType(m_program.at(id));
}

void IRPrinter::printType(const Type& type)
{
    CommaPrinter bar("|"_s);
    if (type.isAnything())
        m_out.print(bar, "anything"_s);
    else {
        printPrimitives(type, bar);
        if (type.anyObject)
            m_out.print(bar, "object"_s);
        else if (type.anyFunction)
            m_out.print(bar, "function"_s);
    }
    if (type.primitives & Primitive::Absent)
        m_out.print(bar, "absent"_s);

    printStringLiterals(type, bar);
    printSymbolLiterals(type, bar);
    printObjectParts(type, bar);
    for (const ClassPart& part : type.classes) {
        m_out.print(bar);
        printClassPart(type, part);
    }
    if (type.opaque) {
        m_out.print(bar);
        auto kindNumber = static_cast<unsigned>(*type.opaque);
        if (kindNumber >= numberOfEnumerators<OpaqueKind>)
            printInvalid("opaque"_s, kindNumber);
        else {
            for (char character : nameOf(*type.opaque).span())
                m_out.print(toASCIILower(character));
        }
    }

    if (!bar.didPrint())
        m_out.print("never"_s);
}

void IRPrinter::printPrimitives(const Type& type, CommaPrinter& bar)
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

    bool isEveryNumber = (primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full();
    if (isEveryNumber || (!printsEngineFields() && primitives & Primitive::Double))
        m_out.print(bar, "number"_s);
    else {
        if (primitives & Primitive::Integer) {
            if (type.integers == Range::full())
                m_out.print(bar, "int"_s);
            else if (type.integers == Range::int32())
                m_out.print(bar, "i32"_s);
            else if (type.integers.minimum == type.integers.maximum)
                m_out.print(bar, type.integers.minimum);
            else
                m_out.print(bar, "int["_s, type.integers.minimum, ", "_s, type.integers.maximum, "]"_s);
        }
        if ((primitives & Primitive::Double) == Primitive::Double)
            m_out.print(bar, "double"_s);
        else if (primitives & Primitive::WholeNumber)
            m_out.print(bar, "whole"_s);
        else if (primitives & Primitive::FractionalNumber)
            m_out.print(bar, "fractional"_s);
    }

    if (primitives & Primitive::String)
        m_out.print(bar, "string"_s);
    if (primitives & Primitive::Symbol)
        m_out.print(bar, "symbol"_s);
    if (primitives & Primitive::BigInt)
        m_out.print(bar, "bigint"_s);
}

void IRPrinter::printStringLiterals(const Type& type, CommaPrinter& bar)
{
    Vector<StringID, 8> literals(type.stringLiterals.span());
    AOT::sort(literals, [&](StringID a, StringID b) {
        return isPrintedBefore(m_program.strings, a, b);
    });
    for (StringID literal : literals) {
        m_out.print(bar);
        print(literal);
    }
}

void IRPrinter::printSymbolLiterals(const Type& type, CommaPrinter& bar)
{
    if (!printsEngineFields())
        return;

    const TypeTable& types = m_program.types;
    Vector<PropertyKeyID, 8> literals(type.symbolLiterals.span());
    bool areAllInTable = std::ranges::all_of(literals, [&](PropertyKeyID literal) {
        return isInTable(types, literal, types.numberOfPropertyKeys());
    });
    if (areAllInTable) {
        AOT::sort(literals, [&](PropertyKeyID a, PropertyKeyID b) {
            return isPrintedBefore(m_program.strings, types.at(a), types.at(b));
        });
    }
    for (PropertyKeyID literal : literals) {
        m_out.print(bar);
        print(literal);
    }
}

void IRPrinter::printObjectParts(const Type& type, CommaPrinter& bar)
{
    const TypeTable& types = m_program.types;
    if (type.layouts && !isInTable(types, type.layouts, types.numberOfLayoutSets())) {
        m_out.print(bar);
        printInvalid("layoutset"_s, type.layouts.index());
    } else if (type.layouts) {
        const LayoutSet& set = types.at(type.layouts);
        visitInPrintedOrder(m_program, set.members.span(), [&](LayoutID layout) {
            m_out.print(bar);
            print(layout);
        });
        if (set.hasMembersWithoutLayout)
            m_out.print(bar, "nolayout"_s);
    }
    if (type.environment) {
        m_out.print(bar, "env "_s);
        print(type.environment);
    }
    for (FunctionID function : type.functions) {
        m_out.print(bar);
        print(function);
    }
    for (BuiltinID builtin : type.builtins) {
        m_out.print(bar, "builtin("_s);
        print(builtin);
        m_out.print(")"_s);
    }
}

void IRPrinter::printClassPart(const Type& type, const ClassPart& part)
{
    print(part.builtinClass);

    const BuiltinTable& builtins = m_program.builtins();
    unsigned numberOfContents = builtins.contains(part.builtinClass) ? builtins.numberOfSlotsInType(part.builtinClass) : 0;
    bool hasKnownContents = false;
    for (unsigned index = 0; index < numberOfContents; ++index)
        hasKnownContents |= part.contentsAt(index) != TypeTable::anything;

    Vector<RegExpLiteralInType, 4> literals;
    if (part.builtinClass == BuiltinClasses::RegExp)
        literals.append(type.regExpLiterals());
    bool hasEngineParts = part.contentsAreProven || part.arrayStorage != ArrayStorage::Other || !part.provenPristine.isEmpty() || !literals.isEmpty();
    bool printsEngineParts = printsEngineFields() && hasEngineParts;
    if (!hasKnownContents && !printsEngineParts)
        return;

    m_out.print("<"_s);
    CommaPrinter comma;
    for (unsigned index = 0; index < numberOfContents; ++index) {
        m_out.print(comma);
        print(part.contentsAt(index));
    }
    if (printsEngineParts) {
        AOT::sort(literals, [&](RegExpLiteralInType a, RegExpLiteralInType b) {
            return isPrintedBefore(m_program.strings, a, b);
        });
        for (RegExpLiteralInType literal : literals) {
            m_out.print(comma);
            print(literal.pattern);
            print(literal.flags());
        }
        if (part.contentsAreProven)
            m_out.print(comma, "proven"_s);
        if (part.arrayStorage != ArrayStorage::Other) {
            m_out.print(comma);
            print(part.arrayStorage);
        }
        if (!part.provenPristine.isEmpty()) {
            m_out.print(comma, "pristine"_s);
            print(part.provenPristine);
        }
    }
    m_out.print(">"_s);
}

void IRPrinter::print(LayoutSetID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (!isInTable(m_program.types, id, m_program.types.numberOfLayoutSets())) {
        printInvalid("layoutset"_s, id.index());
        return;
    }

    const LayoutSet& set = m_program.at(id);
    m_out.print("["_s);
    CommaPrinter comma;
    visitInPrintedOrder(m_program, set.members.span(), [&](LayoutID member) {
        m_out.print(comma);
        print(member);
    });
    if (set.hasMembersWithoutLayout)
        m_out.print(comma, "nolayout"_s);
    if (!set.isComplete)
        m_out.print(comma, ".."_s);
    m_out.print("]"_s);
}

void IRPrinter::print(CalleeSetID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (!isInTable(m_program.types, id, m_program.types.numberOfCalleeSets())) {
        printInvalid("calleeset"_s, id.index());
        return;
    }

    const CalleeSet& set = m_program.at(id);
    m_out.print("["_s);
    CommaPrinter comma;
    for (FunctionID function : set.functions) {
        m_out.print(comma);
        print(function);
    }
    for (BuiltinID builtin : set.builtins) {
        m_out.print(comma, "builtin("_s);
        print(builtin);
        m_out.print(")"_s);
    }
    if (!set.isComplete)
        m_out.print(comma, ".."_s);
    m_out.print("]"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (ASSERT_ENABLED || defined(AOT_IR_PRINTER_FOR_TESTAOT))
