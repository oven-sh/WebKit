/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumberLiteralStrings.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIR.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotRepresentation.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
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
    explicit StringsNeedingWords(const Program& program)
        : visitedTypes(program.types.size())
        , strings(program.strings.size())
    {
    }

    void add(StringID string) { strings.atomicSetAndCheck(string.index(), true); }

    void add(std::span<const StringID> list)
    {
        for (StringID string : list)
            add(string);
    }

    FastBitVector visitedTypes;
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
    const Type& type = program.at(id);
    if (type.hasLiteralStrings() && found.visitedTypes.atomicSetAndCheck(id.index(), true))
        found.add(type.stringLiterals.span());
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

static void collectStringsNeedingWords(const Program& program, const Function& function, StringsNeedingWords& found)
{
    collectLiteralsOf(program, function.captures.span(), found);
    collectLiteralsOf(program, function.thisType.type, found);
    collectLiteralsOf(program, function.parameters.span(), found);
    collectLiteralsOf(program, function.results.span(), found);

    auto collectLiteralsOfValue = [&](ValueID id) {
        const Value& value = function.at(id);
        if (value.opcode != Opcode::String)
            collectLiteralsOf(program, value.type, found);
        forEachType(value, [&](TypeID type) {
            collectLiteralsOf(program, type, found);
        });
    };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            collectLiteralsOfValue(parameter);
        for (ValueID id : block.values) {
            collectLiteralsOfValue(id);
            if (equalityComparisons.contains(function.at(id).opcode))
                collectComparedConstants(program, function, function.at(id), found);
        }
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::SwitchString && program.typeOf(function, terminator.subjectOperand()).hasLiteralStrings())
            found.add(terminator.strings());
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
    StringsNeedingWords found(program);
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (function.hasBody())
            collectStringsNeedingWords(program, function, found);
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
    if (build.exceeds(LimitID::MaxLiteralStrings, table.m_strings.size()))
        table.m_strings.shrink(std::max<uint64_t>(build.limit(LimitID::MaxLiteralStrings), 1));

    table.m_indexes.reserveInitialCapacity(table.m_strings.size());
    for (uint32_t number = 0; number < table.m_strings.size(); ++number)
        table.m_indexes.add(table.m_strings[number], number);
    build.statistics().count(StatisticID::LiteralStringsNumbered, table.m_strings.size());
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
