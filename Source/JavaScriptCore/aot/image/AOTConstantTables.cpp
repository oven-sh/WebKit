/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierTable.h"
#include "AOTLiteralStringDigest.h"
#include "AOTNameClass.h"
#include "AOTOperationData.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

// No table of this part holds an offset into the tables, so no word of it is linked.

namespace {

struct MeasuredTextOfConstantTables {
    uint32_t hash { 0 };
    uint32_t length { 0 };
    // Only of ImageNumbers::literalStringsWithoutText().
    std::array<uint8_t, 20> digest { };
};

} // anonymous namespace

static void nameStringOfConstantTables(const Typed::Program& program, NamesOfTables& names, StringID string)
{
    uint32_t index = (string ? string : program.strings.emptyString()).index();
    names.strings.set(index);
    names.stringsWithOtherUsesThanShowing.set(index);
}

void nameWhatConstantTablesName(const TableInputs& inputs, const GatheredMentions& mentions, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;

    // ImageNumbersBuilder::numberConstants() gives a cell to each key that code mentions as a cell, and to every symbol.
    names.keys.merge(mentions.byID(NumberSpace::KeyCell).isMentioned);
    for (unsigned index = 0; index < program.types.numberOfPropertyKeys(); ++index) {
        if (Typed::isSymbolKind(program.at(PropertyKeyID(index)).kind))
            names.keys.set(index);
    }

    // A literal string is not named for having a number: ImageNumbersBuilder::numberIdentifiersAndStrings() names those that something spells.
    for (const Typed::OpenedName& opened : program.openedNames)
        nameStringOfConstantTables(program, names, opened.name);
    // ImageNumbersBuilder::numberConstants() makes a constant of the digits that code mentions, and of NamesOfTables::bigInts. Whoever names a BigInt there names its digits as a string too: nameWhatDataTableNames().
    for (size_t digits : mentions.byID(NumberSpace::BigInt).isMentioned)
        nameStringOfConstantTables(program, names, StringID(digits));
}

template<typename Item>
static void setConstantTableOfWords(TableEncoder& encoder, ProgramTableIndex table, std::span<const Item> items, const Invocable<uint32_t(const Item&)> auto& wordOf)
{
    encoder.part.setTableWrittenInPlace<uint32_t>(table, items.size(), [&](std::span<uint32_t> words) {
        forEachRangeOfIndicesInParallel(encoder.build.parallelLoops(), items.size(), [&](size_t first, size_t end) {
            for (size_t place = first; place < end; ++place)
                words[place] = wordOf(items[place]);
        });
    });
}

static Vector<MeasuredTextOfConstantTables> measureTextsOfConstantTables(const TableEncoder& encoder, std::span<const StringID> strings)
{
    const BitVector& literalStringsWithoutText = encoder.numbers.literalStringsWithoutText();
    Vector<MeasuredTextOfConstantTables> texts(strings.size());
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(strings.size()), [&](unsigned index) {
        DecodedString decoded = decodeForImage(encoder.program.strings[strings[index]]);
        texts[index] = { decoded.hash, decoded.length(), { } };
        if (literalStringsWithoutText.get(strings[index].index()))
            texts[index].digest = digestOfLiteralString(decoded.is16Bit() ? StringView(decoded.units16.span()) : StringView(byteCast<Latin1Character>(decoded.units8.span())));
    });
    return texts;
}

static void encodeTablesOfLiteralStrings(TableEncoder& encoder)
{
    std::span<const StringID> strings = encoder.plan.literalStrings().strings();
    if (strings.size() > maxLiteralStrings) {
        encoder.fail("TooManyLiteralStrings"_s);
        strings = strings.first(maxLiteralStrings);
    }
    setConstantTableOfWords(encoder, ProgramTableIndex::literalStrings, strings, [&](StringID string) {
        return encoder.numberOfLiteralString(string);
    });

    const BitVector& literalStringsWithoutText = encoder.numbers.literalStringsWithoutText();
    Vector<MeasuredTextOfConstantTables> texts = measureTextsOfConstantTables(encoder, strings);
    uint32_t numberOfSlots = std::max<uint32_t>(static_cast<uint32_t>(strings.size()), 4) * 2;
    Vector<LiteralStringLimits, 1> limits;
    if (!strings.empty())
        limits.append(LiteralStringLimits { 0 });
    // Which slot a string gets depends on the strings in front of it, so one thread hands them out.
    encoder.part.setTableWrittenInPlace<uint32_t>(ProgramTableIndex::literalStringTable, numberOfSlots, [&](std::span<uint32_t> slots) {
        zeroSpan(slots);
        for (uint32_t number = 0; number < strings.size(); ++number) {
            uint32_t hash = texts[number].hash;
            uint32_t slot = LiteralStringTable::firstSlot(hash, numberOfSlots);
            while (slots[slot])
                slot = slot + 1 == numberOfSlots ? 0 : slot + 1;
            slots[slot] = LiteralStringTable::entryFor(number, hash);
            limits[0].maxLength = std::max(limits[0].maxLength, texts[number].length);
        }
    });
    // The run time finds a digest by a binary search over the numbers.
    Vector<LiteralStringDigest> digests = collectInParallel<LiteralStringDigest>(encoder.build.parallelLoops(), static_cast<unsigned>(strings.size()), [&](unsigned number, Vector<LiteralStringDigest>& found) {
        if (literalStringsWithoutText.get(strings[number].index()))
            found.append(LiteralStringDigest { number, texts[number].length, texts[number].digest });
    });
    encoder.part.setTable(ProgramTableIndex::literalStringLimits, limits);
    encoder.part.setTable(ProgramTableIndex::literalStringDigests, digests);
}

static void encodeTablesOfIdentifiers(TableEncoder& encoder)
{
    const ParallelLoops& loops = encoder.build.parallelLoops();
    const IdentifierNumbering& identifiers = encoder.numbers.identifiers();
    Vector<uint32_t> numbersFoundByText = findIndicesInParallel(loops, identifiers.count(), [&](unsigned number) {
        return number && identifiers.numberOfText(identifiers.at(number).text) == number;
    });
    Vector<uint32_t> engineSymbols = findIndicesInParallel(loops, identifiers.count(), [&](unsigned number) {
        PropertyKeyID key = identifiers.at(number).key;
        return number && key && isKindOfEngineSymbol(encoder.program.at(key).kind);
    });
    Vector<IdentifierTable::Key> keys;
    keys.grow(numbersFoundByText.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(keys.size()), [&](unsigned index) {
        uint32_t number = numbersFoundByText[index];
        keys[index] = { decodeForImage(encoder.program.strings[identifiers.at(number).text]).hash, number };
    });
    parallelSort(loops, keys.mutableSpan(), IdentifierTable::isOrderedBefore);
    encoder.part.setTable(ProgramTableIndex::identifiersByHash, IdentifierTable::createFromSortedKeys(keys.span()));
    encoder.part.setTable(ProgramTableIndex::identifiersOfEngineSymbols, engineSymbols);
}

static Vector<uint32_t> listConstantAndUnlockedBuiltinMembers(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    const BuiltinTable& builtins = program.builtins();

    Vector<BuiltinID> constantMembers;
    for (size_t index : encoder.facts.builtinMembersLockedAtStartUp()) {
        BuiltinID member(static_cast<uint32_t>(index));
        const Builtin& entry = builtins.at(member);
        // A default export is no property of its holder: the loader hands it out.
        if (entry.holder && entry.name && builtins.findOwnMember(entry.holder, entry.name) == member)
            constantMembers.append(member);
    }
    // The run time finds the members of a holder by a binary search over the holders.
    AOT::sortInLine(constantMembers, { }, [&](BuiltinID member) {
        return std::pair(builtins.at(member).holder, member);
    });

    // The run time finds an unlocked member by a binary search.
    BitVector unlockedMembers(builtins.builtinCount());
    for (BuiltinID member : program.unlockedBuiltins())
        unlockedMembers.set(member.index());
    for (BuiltinID member : encoder.facts.builtinMembersThatTestsReplace())
        unlockedMembers.set(member.index());

    Vector<uint32_t> words;
    words.reserveInitialCapacity(constantMembers.size() + unlockedMembers.bitCount() + 2 * program.openedNames.size());
    for (BuiltinID member : constantMembers)
        words.append(member.index());
    for (size_t member : unlockedMembers)
        words.append(static_cast<uint32_t>(member) | ProgramTables::unlockedBuiltinMember);
    for (const Typed::OpenedName& opened : program.openedNames)
        words.appendList({ opened.holder.index() | ProgramTables::openedName, encoder.stringNumber(opened.name) | ProgramTables::openedName });
    return words;
}

void encodeConstantTables(TableEncoder& encoder)
{
    const ImageNumbers& numbers = encoder.numbers;
    StepTimes times(encoder.build, "image: constant tables"_s);
    setConstantTableOfWords(encoder, ProgramTableIndex::builtinCells, numbers.builtinCells(), [](BuiltinCellKey key) {
        return key.encoded();
    });
    encoder.part.setTable(ProgramTableIndex::eagerBuiltinCells, numbers.eagerBuiltinCells());
    encoder.part.setTable(ProgramTableIndex::constantAndUnlockedBuiltinMembers, listConstantAndUnlockedBuiltinMembers(encoder));
    setConstantTableOfWords(encoder, ProgramTableIndex::bigIntConstants, numbers.bigIntConstants(), [&](StringID digits) {
        return encoder.stringNumber(digits);
    });
    setConstantTableOfWords(encoder, ProgramTableIndex::keyCellIdentifiers, numbers.keyCells(), [&](PropertyKeyID key) {
        return encoder.identifierNumber(key, "a key that code holds as a cell"_s);
    });
    times.didFinish("built-ins, BigInts, keys"_s);
    encodeTablesOfLiteralStrings(encoder);
    times.didFinish("literal strings"_s);
    encodeTablesOfIdentifiers(encoder);
    times.didFinish("identifiers"_s);
    setConstantTableOfWords(encoder, ProgramTableIndex::entryPoints, encoder.program.entries.span(), [](const Typed::Entry& entry) {
        return entry.module.index();
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
