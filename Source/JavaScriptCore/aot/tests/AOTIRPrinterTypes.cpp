/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"

#include <algorithm>
#include <climits>
#include <wtf/ASCIICType.h>
#include <wtf/CommaPrinter.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct CodePointInIRText {
    char32_t character;
    unsigned length;
};

static std::optional<CodePointInIRText> decodeCodePointForIRText(std::span<const uint8_t> bytes)
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
    return CodePointInIRText { character, length };
}

template<typename IDType>
static bool isReadThroughArenaForIRText(const std::optional<TypeReader>& typesOfCode, IDType id)
{
    return typesOfCode && positionOf(id) <= largestIDPosition;
}

static bool isCanonicalTypeWithinIDForIRText(TypeID id)
{
    if (homeOf(id) == IDHome::SmallRange)
        return !integersInID(id).isEmpty();
    if ((id.bits() >> maximumLandmarkShift & 7) >= maximumLandmarks.size())
        return false;
    return tryMakeTypeOfPrimitives(primitivesInID(id), integersInID(id)) == id;
}

static bool canBeReadForIRText(const TypeTable& table, const std::optional<TypeReader>& typesOfCode, TypeID id)
{
    switch (homeOf(id)) {
    case IDHome::Value:
    case IDHome::SmallRange:
        return isCanonicalTypeWithinIDForIRText(id);
    case IDHome::Program:
        return positionOf(id) < table.numberOfTypes();
    case IDHome::Thread:
        return isReadThroughArenaForIRText(typesOfCode, id);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeReadForIRText(const TypeTable& table, const std::optional<TypeReader>& typesOfCode, PropertyKeyID id)
{
    switch (homeOf(id)) {
    case IDHome::Value:
        return positionOf(id) < table.numberOfPropertyKeys();
    case IDHome::SmallRange:
    case IDHome::Program:
        return false;
    case IDHome::Thread:
        return isReadThroughArenaForIRText(typesOfCode, id);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeReadForIRText(const std::optional<TypeReader>& typesOfCode, LayoutSetID id)
{
    switch (homeOf(id)) {
    case IDHome::Value:
    case IDHome::Program:
        return true;
    case IDHome::SmallRange:
        return false;
    case IDHome::Thread:
        return isReadThroughArenaForIRText(typesOfCode, id);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isBuiltinOrNamespaceLayoutForIRText(const Program& program, LayoutID id)
{
    if (id.index() >= program.layouts.size())
        return false;
    LayoutKind kind = program.at(id).kind;
    return kind == LayoutKind::BuiltinResult || kind == LayoutKind::ModuleNamespace;
}

static void visitInOrderOfIRText(const Program& program, SetView<LayoutID, LayoutSetID> layouts, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    for (bool visitsBuiltinAndNamespaceLayouts : { false, true }) {
        layouts.forEach([&](LayoutID layout) {
            if (isBuiltinOrNamespaceLayoutForIRText(program, layout) == visitsBuiltinAndNamespaceLayouts)
                functor(layout);
        });
    }
}

void IRPrinter::print(StringID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (liesInThread(id) && isReadThroughArenaForIRText(m_typesOfCode, id)) {
        m_out.print("made "_s);
        printQuoted(m_typesOfCode->bytesOf(id));
        return;
    }
    if (id.index() >= m_program.strings.size()) {
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
        else if (auto codePoint = decodeCodePointForIRText(bytes)) {
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
    if (!canBeReadForIRText(m_program.types, m_typesOfCode, id)) {
        printInvalid("key"_s, id.index());
        return;
    }

    const PropertyKey& key = m_typesOfCode ? m_typesOfCode->at(id) : m_program.at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        print(key.text);
        return;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        m_out.print(key.kind == PropertyKeyKind::ModulePrivateName ? "@#"_s : "@"_s);
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
        if (key.callNumber)
            print(*key.callNumber);
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
    if (!canBeReadForIRText(m_program.types, m_typesOfCode, id)) {
        printInvalid("type"_s, id.index());
        return;
    }
    if (m_typesOfCode) {
        printType(m_out, *m_typesOfCode, id, TypeTextOptions { });
        return;
    }

    TypeTextOptions options;
    options.printsSeeds = printsEngineFields();
    options.namesFamiliesOfLayouts = true;
    printType(m_out, TypeReader(m_program), id, options);
}

void IRPrinter::print(LayoutSetID id)
{
    if (!id) {
        m_out.print("-"_s);
        return;
    }
    if (!canBeReadForIRText(m_typesOfCode, id)) {
        printInvalid("layoutset"_s, id.index());
        return;
    }
    if (m_typesOfCode) {
        printLayoutSet(m_out, *m_typesOfCode, id);
        return;
    }

    m_out.print("["_s);
    CommaPrinter comma;
    visitInOrderOfIRText(m_program, m_program.at(id), [&](LayoutID member) {
        m_out.print(comma);
        print(member);
    });
    m_out.print("]"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
