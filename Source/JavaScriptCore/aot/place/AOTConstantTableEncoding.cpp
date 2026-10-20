/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTFlowBuiltinLocations.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TextProperties {
    uint32_t hash { 0 };
    uint32_t length { 0 };
};

} // anonymous namespace

static Vector<TextProperties> computeTextProperties(const TableEncoder& encoder, std::span<const StringID> strings)
{
    Vector<TextProperties> properties(strings.size());
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(strings.size()), [&](unsigned index) {
        DecodedString decoded = decodeForImage(encoder.program.strings[strings[index]]);
        properties[index] = { decoded.hash, decoded.length() };
    });
    return properties;
}

static Vector<uint32_t> createHashTable(size_t numberOfEntries)
{
    return Vector<uint32_t>(FillWith { }, WTF::roundUpToPowerOfTwo(std::max<uint32_t>(numberOfEntries, 4) * 2), 0);
}

static void insertIntoHashTable(Vector<uint32_t>& table, uint32_t hash, uint32_t entry)
{
    ASSERT(entry);
    uint32_t mask = table.size() - 1;
    uint32_t slot = hash & mask;
    while (table[slot])
        slot = (slot + 1) & mask;
    table[slot] = entry;
}

static void encodeBuiltinCells(TableEncoder& encoder)
{
    Vector<uint32_t> builtinCells;
    for (BuiltinCellKey cell : encoder.placement.builtinCells())
        builtinCells.append(cell.encoded());
    encoder.setTable(ProgramTableIndex::builtinCells, builtinCells);
    encoder.setTable(ProgramTableIndex::eagerBuiltinCells, encoder.placement.eagerBuiltinCells());
}

static void encodeConstantBuiltinMembers(TableEncoder& encoder)
{
    Vector<uint32_t> constantBuiltinMembers;
    for (BuiltinID member : constantBuiltinMembersToLock(encoder.program, encoder.flow))
        constantBuiltinMembers.append(member.index());
    encoder.setTable(ProgramTableIndex::constantBuiltinMembers, constantBuiltinMembers);
}

static void encodeClosedAbsentNames(TableEncoder& encoder)
{
    Vector<ClosedAbsentNamesOfHolder> holders;
    Vector<uint32_t> names;
    std::span<const BuiltinID> holdersWithFixedLink = encoder.flow.holdersWithFixedPrototypeLink();
    auto listHoldersBefore = [&](unsigned limit) {
        while (!holdersWithFixedLink.empty() && holdersWithFixedLink.front().index() < limit) {
            if (holders.isEmpty() || holders.last().holder != holdersWithFixedLink.front().index())
                holders.append({ holdersWithFixedLink.front().index(), static_cast<uint32_t>(names.size()), 0 });
            holdersWithFixedLink = holdersWithFixedLink.subspan(1);
        }
    };
    for (const ClosedAbsentName& name : encoder.flow.closedAbsentNames()) {
        listHoldersBefore(name.holder.index());
        if (holders.isEmpty() || holders.last().holder != name.holder.index())
            holders.append({ name.holder.index(), static_cast<uint32_t>(names.size()), 0 });
        names.append(encoder.identifierNumber(name.key));
        ++holders.last().numberOfNames;
    }
    listHoldersBefore(UINT_MAX);
    for (const ClosedAbsentNamesOfHolder& holder : holders)
        std::ranges::sort(names.mutableSpan().subspan(holder.firstName, holder.numberOfNames));
    encoder.setTable(ProgramTableIndex::holdersOfClosedAbsentNames, holders);
    encoder.setTable(ProgramTableIndex::closedAbsentNames, names);
}

static void encodeBigIntConstants(TableEncoder& encoder)
{
    Vector<uint32_t> bigIntConstants;
    for (StringID digits : encoder.placement.bigIntConstants())
        bigIntConstants.append(encoder.stringNumber(digits));
    encoder.setTable(ProgramTableIndex::bigIntConstants, bigIntConstants);
}

static void encodeLiteralStrings(TableEncoder& encoder)
{
    std::span<const StringID> literalStrings = encoder.inputs.pipeline.literalStrings->strings();
    RELEASE_ASSERT(literalStrings.size() <= maxLiteralStrings);
    encoder.setTable(ProgramTableIndex::literalStrings, Vector<uint32_t>(literalStrings.size(), [&](size_t number) {
        return encoder.stringNumber(literalStrings[number]);
    }));

    Vector<TextProperties> properties = computeTextProperties(encoder, literalStrings);
    Vector<uint32_t> table = createHashTable(literalStrings.size());
    Vector<LiteralStringLimits, 1> limits;
    if (!literalStrings.empty())
        limits.append(LiteralStringLimits { 0 });
    for (uint32_t number = 0; number < literalStrings.size(); ++number) {
        uint32_t hash = properties[number].hash;
        insertIntoHashTable(table, hash, AOT::LiteralStringTable::entryFor(number, hash));
        limits[0].maxLength = std::max(limits[0].maxLength, properties[number].length);
    }
    encoder.setTable(ProgramTableIndex::literalStringTable, table);
    encoder.setTable(ProgramTableIndex::literalStringLimits, limits);
}

static void encodeIdentifiersByHash(TableEncoder& encoder)
{
    Vector<uint32_t> numbers;
    Vector<StringID> texts;
    for (uint32_t number = 1; number < encoder.identifiers.count(); ++number) {
        StringID text = encoder.identifiers.at(number).text;
        if (encoder.identifiers.numberOfText(text) != number)
            continue;
        numbers.append(number);
        texts.append(text);
    }

    Vector<TextProperties> properties = computeTextProperties(encoder, texts.span());
    Vector<uint32_t> table = createHashTable(numbers.size());
    for (size_t index = 0; index < numbers.size(); ++index)
        insertIntoHashTable(table, properties[index].hash, numbers[index] + 1);
    encoder.setTable(ProgramTableIndex::identifiersByHash, table);
}

static bool isEngineSymbol(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The engine hands out these symbols before the program names them, so the run time creates their identifiers before it looks up any uid.
static void encodeIdentifiersOfEngineSymbols(TableEncoder& encoder)
{
    Vector<uint32_t> numbers;
    for (uint32_t number = 1; number < encoder.identifiers.count(); ++number) {
        PropertyKeyID key = encoder.identifiers.at(number).key;
        if (key && isEngineSymbol(encoder.program.at(key).kind))
            numbers.append(number);
    }
    encoder.setTable(ProgramTableIndex::identifiersOfEngineSymbols, numbers);
}

static void encodeEntryPoints(TableEncoder& encoder)
{
    Vector<uint32_t> entryPoints;
    for (const Entry& entry : encoder.program.entries)
        entryPoints.append(entry.module.index());
    encoder.setTable(ProgramTableIndex::entryPoints, entryPoints);
}

void encodeConstantTables(TableEncoder& encoder)
{
    encodeBuiltinCells(encoder);
    encodeConstantBuiltinMembers(encoder);
    encodeClosedAbsentNames(encoder);
    encodeBigIntConstants(encoder);
    encodeLiteralStrings(encoder);
    encodeIdentifiersByHash(encoder);
    encodeIdentifiersOfEngineSymbols(encoder);
    encodeEntryPoints(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
