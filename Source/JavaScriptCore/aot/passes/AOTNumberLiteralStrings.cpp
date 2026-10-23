/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumberLiteralStrings.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTIR.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotPlan.h"
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

static constexpr OpcodeSet equalityComparisonsOfLiterals { Opcode::StrictEq, Opcode::Eq };
// Their value is one that another value, a field or a variable of the program held before.
static constexpr OpcodeSet operationsHandingOnValues { Opcode::Parameter, Opcode::Upcast, Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::CallDirect };

namespace {

struct StringsNeedingWords {
    StringsNeedingWords(const Program& program, const ParallelLoops& loops)
        : typesToVisit(program.types.size())
        , typesHandedOnToVisit(program.types.size())
        , strings(program.strings.size())
        , membersHandedOn(program.strings.size())
        , constantsOfCode(program.strings.size())
        , variablesNamedByCode(program.variables.size())
    {
        constexpr unsigned typesInShare = 4096;
        static_assert(!(typesInShare % 32));
        unsigned numberOfTypes = program.types.size();
        forEachIndexInParallel(loops, (numberOfTypes + typesInShare - 1) / typesInShare, [&](unsigned share) {
            for (unsigned index = share * typesInShare; index < std::min(numberOfTypes, (share + 1) * typesInShare); ++index) {
                bool hasLiteralStrings = program.at(TypeID(index)).hasLiteralStrings();
                typesToVisit[index] = hasLiteralStrings;
                typesHandedOnToVisit[index] = hasLiteralStrings;
            }
        });
    }

    void add(StringID string) { strings.atomicSetAndCheck(string.index(), true); }

    void add(std::span<const StringID> list)
    {
        for (StringID string : list)
            add(string);
    }

    FastBitVector typesToVisit;
    FastBitVector typesHandedOnToVisit;
    FastBitVector strings;
    // Each gets a word only if the program spells it.
    FastBitVector membersHandedOn;
    FastBitVector constantsOfCode;
    FastBitVector variablesNamedByCode;
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

enum class Source : bool { Program, Anywhere };

// A string from outside the program gets its word where it comes in, so every member has one there. Where the program hands its own values on, a member that it spells nowhere does not occur.
static void collectLiteralsOf(const Program& program, TypeID id, Source source, StringsNeedingWords& found)
{
    if (source == Source::Anywhere) {
        collectLiteralsOf(program, id, found);
        return;
    }
    if (!found.typesHandedOnToVisit.at(id.index()) || !found.typesHandedOnToVisit.atomicSetAndCheck(id.index(), false))
        return;
    for (StringID string : program.at(id).stringLiterals)
        found.membersHandedOn.atomicSetAndCheck(string.index(), true);
}

static void collectLiteralsOf(const Program& program, std::span<const DeclaredType> positions, Source source, StringsNeedingWords& found)
{
    for (const DeclaredType& declared : positions)
        collectLiteralsOf(program, declared.type, source, found);
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

static void collectStringsNeedingWords(const Program& program, FunctionID id, Source sourceOfArguments, StringsNeedingWords& found)
{
    const Function& function = program.at(id);
    collectLiteralsOf(program, function.captures.span(), Source::Program, found);
    collectLiteralsOf(program, function.thisType.type, sourceOfArguments, found);
    collectLiteralsOf(program, function.parameters.span(), sourceOfArguments, found);
    collectLiteralsOf(program, function.results.span(), Source::Program, found);

    auto collectFromValue = [&](const Value& value) {
        if (value.opcode != Opcode::String)
            collectLiteralsOf(program, value.type, operationsHandingOnValues.contains(value.opcode) ? Source::Program : Source::Anywhere, found);
        else
            found.constantsOfCode.atomicSetAndCheck(value.string().index(), true);
        forEachType(value, [&](TypeID type) {
            collectLiteralsOf(program, type, found);
        });
        if (equalityComparisonsOfLiterals.contains(value.opcode))
            collectComparedConstants(program, function, value, found);
        if (VariableID variable = Value::opcodesWithVariable.contains(value.opcode) ? value.variable() : VariableID())
            found.variablesNamedByCode.atomicSetAndCheck(variable.index(), true);
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

static void collectStoredLiterals(const Program& program, const FlowResults& flow, StringsNeedingWords& found)
{
    for (const Layout& layout : program.layouts) {
        bool isFilledByParse = layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse);
        for (const Field& field : layout.fields)
            collectLiteralsOf(program, field.declared.type, !isFilledByParse && field.flow.location.writers == WriterState::AllKnown ? Source::Program : Source::Anywhere, found);
    }
    BitVector described = findVariablesDescribedToRunTime(program, flow);
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        TypeID type = program.variables[index].declared.type;
        if (described.quickGet(index) || found.variablesNamedByCode.at(index)) {
            collectLiteralsOf(program, type, !described.quickGet(index) && program.variables[index].flow.writers == WriterState::AllKnown ? Source::Program : Source::Anywhere, found);
            continue;
        }
        // The text of a constant that was folded into its readers is in the image with them.
        for (StringID literal : program.at(type).stringLiterals) {
            if (found.constantsOfCode.at(literal.index()))
                found.add(literal);
        }
    }
}

static BitVector leaveOutMembersSpelledNowhere(const Program& program, StringsNeedingWords& found)
{
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            if (const DataNode::String* string = node.tryAs<DataNode::String>())
                found.constantsOfCode[string->value.index()] = true;
        });
    }
    BitVector leftOut(program.strings.size());
    found.membersHandedOn.forEachSetBit([&](size_t index) {
        if (found.strings.at(index))
            return;
        if (found.constantsOfCode.at(index))
            found.strings[index] = true;
        else
            leftOut.quickSet(index);
    });
    return leftOut;
}

LiteralStringTable numberLiteralStrings(const Program& program, const FlowResults& flow, BuildContext& build)
{
    StringsNeedingWords found(program, build.parallelLoops());
    BitVector isEnteredOtherwise = functionsCalledByHost(program, build.parallelLoops());
    for (const FlowIndirectCall& call : flow.indirectCalls())
        isEnteredOtherwise.set(call.callee.index());
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        if (!hasBody(program, id))
            return;
        bool everyCallIsSeen = !isEnteredOtherwise.get(index) && (program.at(id).hasOnlyDirectCallers() || callerStateOf(flow, id) == CallerState::AllKnown);
        collectStringsNeedingWords(program, id, everyCallIsSeen ? Source::Program : Source::Anywhere, found);
    });
    collectStoredLiterals(program, flow, found);
    BitVector leftOut = leaveOutMembersSpelledNowhere(program, found);

    StringID emptyString = program.strings.emptyString();
    LiteralStringTable table;
    table.m_leftOut = WTF::move(leftOut);
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
