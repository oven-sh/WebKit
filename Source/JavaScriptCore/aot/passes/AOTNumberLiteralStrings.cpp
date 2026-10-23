/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumberLiteralStrings.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTIR.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotRepresentation.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/FastBitVector.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxLiteralStrings, Architecture::ARM64) == maxLiteralStrings);
static_assert(defaultLimitValue(LimitID::MaxLiteralStrings, Architecture::X86_64) == maxLiteralStrings);
static_assert(literalStringWord(0) == emptyLiteralStringWord);

static constexpr OpcodeSet equalityComparisons { Opcode::StrictEq, Opcode::Eq };

namespace {

struct StringsNeedingWords {
    StringsNeedingWords(const Program& program, const ParallelLoops& loops)
        : typesToVisit(program.types.size())
        , strings(program.strings.size())
    {
        constexpr unsigned typesInShare = 4096;
        static_assert(!(typesInShare % 32));
        unsigned numberOfTypes = program.types.size();
        forEachIndexInParallel(loops, (numberOfTypes + typesInShare - 1) / typesInShare, [&](unsigned share) {
            for (unsigned index = share * typesInShare; index < std::min(numberOfTypes, (share + 1) * typesInShare); ++index)
                typesToVisit[index] = program.at(TypeID(index)).hasLiteralStrings();
        });
    }

    void add(StringID string) { strings.atomicSetAndCheck(string.index(), true); }

    void add(std::span<const StringID> list)
    {
        for (StringID string : list)
            add(string);
    }

    FastBitVector typesToVisit;
    FastBitVector strings;
};

} // anonymous namespace

std::optional<uint32_t> LiteralStringTable::indexOf(StringID string) const
{
    auto found = m_indexes.find(string);
    return found == m_indexes.end() ? std::nullopt : std::optional<uint32_t>(found->value);
}

uint32_t LiteralStringTable::numberOf(StringID string) const
{
    std::optional<uint32_t> index = indexOf(string);
    RELEASE_ASSERT(index);
    return *index;
}

void LiteralStringTable::dump(PrintStream& out, const Program& program) const
{
    for (size_t index = 0; index < m_strings.size(); ++index)
        out.print(index, " \"", byteCast<char8_t>(program.string(m_strings[index])), "\"\n");
}

static void collectLiteralsOf(const Program& program, TypeID id, StringsNeedingWords& found)
{
    if (found.typesToVisit.at(id.index()) && found.typesToVisit.atomicSetAndCheck(id.index(), false))
        found.add(program.at(id).stringLiterals.span());
}

static void collectLiteralsOf(const Program& program, std::span<const DeclaredType> positions, StringsNeedingWords& found)
{
    for (const DeclaredType& declared : positions)
        collectLiteralsOf(program, declared.type, found);
}

static void collectComparedConstants(const Program& program, const Function& function, const Value& comparison, StringsNeedingWords& found)
{
    for (unsigned side = 0; side < 2; ++side) {
        ValueID other = definitionThroughUpcasts(function, comparison.operand(1 - side));
        if (!program.typeOf(function, other).hasLiteralStrings())
            continue;
        std::optional<ValueID> constant = constantThroughCasts(program, function, comparison.operand(side));
        if (constant && function.at(*constant).opcode == Opcode::String)
            found.add(function.at(*constant).string());
    }
}

static void collectStringsNeedingWords(const Program& program, FunctionID id, StringsNeedingWords& found)
{
    const Function& function = program.at(id);
    collectLiteralsOf(program, function.captures.span(), found);
    collectLiteralsOf(program, function.thisType.type, found);
    collectLiteralsOf(program, function.parameters.span(), found);
    collectLiteralsOf(program, function.results.span(), found);

    auto collectFromValue = [&](const Value& value) {
        if (value.opcode != Opcode::String)
            collectLiteralsOf(program, value.type, found);
        forEachType(value, [&](TypeID type) {
            collectLiteralsOf(program, type, found);
        });
        if (equalityComparisons.contains(value.opcode))
            collectComparedConstants(program, function, value, found);
    };
    auto collectFromTerminator = [&](const Terminator& terminator) {
        if (terminator.kind == TerminatorKind::SwitchString && program.typeOf(function, terminator.subjectOperand()).hasLiteralStrings())
            found.add(terminator.strings());
    };

    BodyIndex* index = program.bodyIndex();
    if (std::optional<RowOfBody> row = index ? index->rowOf(program, id) : std::nullopt; row && row->listsAllValues) {
        for (const Value& value : function.values)
            collectFromValue(value);
        if (!row->terminators.contains(TerminatorKind::SwitchString))
            return;
        for (const Block& block : function.blocks)
            collectFromTerminator(block.terminator);
        return;
    }

    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            collectFromValue(function.at(parameter));
        for (ValueID value : block.values)
            collectFromValue(function.at(value));
        collectFromTerminator(block.terminator);
    }
}

static void collectStoredLiterals(const Program& program, StringsNeedingWords& found)
{
    for (const Layout& layout : program.layouts) {
        for (const Field& field : layout.fields)
            collectLiteralsOf(program, field.declared.type, found);
    }
    for (const Variable& variable : program.variables)
        collectLiteralsOf(program, variable.declared.type, found);
}

LiteralStringTable numberLiteralStrings(const Program& program, BuildContext& build)
{
    StringsNeedingWords found(program, build.parallelLoops());
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        if (hasBody(program, FunctionID(index)))
            collectStringsNeedingWords(program, FunctionID(index), found);
    });
    collectStoredLiterals(program, found);

    StringID emptyString = program.strings.emptyString();
    LiteralStringTable table;
    table.m_strings.reserveInitialCapacity(found.strings.bitCount() + 1);
    table.m_strings.append(emptyString);
    found.strings.forEachSetBit([&](size_t index) {
        if (index != emptyString.index())
            table.m_strings.append(StringID(index));
    });

    // Compiled code and the run time write emptyLiteralStringWord for an empty string without asking the table, so number 0 always exists.
    if (build.exceeds(LimitID::MaxLiteralStrings, table.m_strings.size())) {
        size_t numberOfWords = std::max<uint64_t>(build.limit(LimitID::MaxLiteralStrings), 1);
        table.m_stringsWithoutWord = Vector<StringID>(table.m_strings.subspan(numberOfWords));
        table.m_strings.shrink(numberOfWords);
    }

    table.m_indexes.reserveInitialCapacity(table.m_strings.size());
    for (uint32_t number = 0; number < table.m_strings.size(); ++number)
        table.m_indexes.add(table.m_strings[number], number);
    build.statistics().count(StatisticID::LiteralStringsNumbered, table.m_strings.size());
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
