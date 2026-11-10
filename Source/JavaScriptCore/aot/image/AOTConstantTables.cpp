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
#include "AOTNameClass.h"
#include "AOTOperationData.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static void nameStringOfConstantTables(const Typed::Program& program, NamesOfTables& names, StringID string)
{
    uint32_t index = (string ? string : program.strings.emptyString()).index();
    names.strings.set(index);
    names.stringsWithOtherUsesThanShowing.set(index);
}

void nameWhatConstantTablesName(const TableInputs& inputs, const GatheredMentions& mentions, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;

    names.keys.merge(mentions.byID(NumberSpace::KeyCell).isMentioned);
    for (unsigned index = 0; index < program.types.numberOfPropertyKeys(); ++index) {
        if (Typed::isSymbolKind(program.at(PropertyKeyID(index)).kind))
            names.keys.set(index);
    }

    for (const Typed::OpenedName& opened : program.openedNames)
        nameStringOfConstantTables(program, names, opened.name);
    for (size_t digits : mentions.byID(NumberSpace::BigInt).isMentioned)
        nameStringOfConstantTables(program, names, StringID(digits));
}

template<typename Item>
static void setConstantTableOfWords(TableEncoder& encoder, ProgramTableIndex table, std::span<const Item> items, const Invocable<uint32_t(const Item&)> auto& wordOf)
{
    encoder.part.setTableInPlace<uint32_t>(table, items.size(), [&](std::span<uint32_t> words) {
        forEachRangeOfIndicesInParallel(encoder.build.parallelLoops(), items.size(), [&](size_t first, size_t end) {
            for (size_t place = first; place < end; ++place)
                words[place] = wordOf(items[place]);
        });
    });
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
        if (entry.holder && entry.name && builtins.findOwnMember(entry.holder, entry.name) == member)
            constantMembers.append(member);
    }
    AOT::sortInLine(constantMembers, { }, [&](BuiltinID member) {
        return std::pair(builtins.at(member).holder, member);
    });

    BitVector unlockedMembers(builtins.builtinCount());
    for (BuiltinID member : program.unlockedBuiltins())
        unlockedMembers.set(member.index());
    for (BuiltinID member : encoder.facts.builtinMembersThatTestsReplace())
        unlockedMembers.set(member.index());

    Vector<uint32_t> words;
    words.reserveInitialCapacity(constantMembers.size() + encoder.numbers.membersAskedByByteAlone().size() + unlockedMembers.bitCount() + 2 * program.openedNames.size());
    for (BuiltinID member : constantMembers)
        words.append(member.index());
    for (BuiltinID member : encoder.numbers.membersAskedByByteAlone())
        words.append(member.index() | ProgramTables::memberAskedByByteAlone);
    for (size_t member : unlockedMembers)
        words.append(static_cast<uint32_t>(member) | ProgramTables::unlockedBuiltinMember);
    for (const Typed::OpenedName& opened : program.openedNames)
        words.appendList({ opened.holder.index() | ProgramTables::openedName, encoder.stringNumber(opened.name) | ProgramTables::openedName });
    return words;
}

static Vector<uint32_t> listNamesTakenForAbsent(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    Vector<uint32_t> numbers;
    for (PropertyKeyID key : program.keysTakenForAbsent())
        numbers.append(ProgramTables::numberOfNameTakenForAbsent(program.string(program.at(key).text)));
    for (ASCIILiteral name : { "prototype"_s, "displayName"_s })
        numbers.append(ProgramTables::numberOfNameTakenForAbsent(byteCast<uint8_t>(name.span())));
    AOT::sortInLine(numbers);
    numbers.shrink(std::unique(numbers.begin(), numbers.end()) - numbers.begin());
    return numbers;
}

void encodeConstantTables(TableEncoder& encoder)
{
    const ImageNumbers& numbers = encoder.numbers;
    StepTimes times(encoder.build, "image: constant tables"_s);
    std::span<const BuiltinCellKey> builtinCells = numbers.builtinCells();
    bool swapsFirstTwo = builtinCells.size() > 1 && AOT::hasFlag(encoder.build.options().injectedFaults, Wire::FaultInjection::SwapBuiltinCellNumbers);
    encoder.part.setTableInPlace<uint32_t>(ProgramTableIndex::builtinCells, builtinCells.size(), [&](std::span<uint32_t> words) {
        for (size_t place = 0; place < words.size(); ++place)
            words[place] = builtinCells[swapsFirstTwo && place < 2 ? 1 - place : place].encoded();
        if (std::ranges::adjacent_find(words, std::ranges::greater_equal { }) != words.end())
            encoder.fail("TableNotSorted"_s, { }, "builtinCells"_s);
    });
    encoder.part.setTable(ProgramTableIndex::eagerBuiltinCells, numbers.eagerBuiltinCells());
    encoder.part.setTable(ProgramTableIndex::constantAndUnlockedBuiltinMembers, listConstantAndUnlockedBuiltinMembers(encoder));
    encoder.part.setTable(ProgramTableIndex::namesTakenForAbsent, listNamesTakenForAbsent(encoder));
    setConstantTableOfWords(encoder, ProgramTableIndex::bigIntConstants, numbers.bigIntConstants(), [&](StringID digits) {
        return encoder.stringNumber(digits);
    });
    setConstantTableOfWords(encoder, ProgramTableIndex::keyCellIdentifiers, numbers.keyCells(), [&](PropertyKeyID key) {
        return encoder.identifierNumber(key, "a key that code holds as a cell"_s);
    });
    times.didFinish("built-ins, BigInts, keys"_s);
    encodeTablesOfIdentifiers(encoder);
    times.didFinish("identifiers"_s);
    setConstantTableOfWords(encoder, ProgramTableIndex::entryPoints, encoder.program.entries.span(), [](const Typed::Entry& entry) {
        return entry.module.index();
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
