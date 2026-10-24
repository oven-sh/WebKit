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
        out.print(index, " \"", byteCast<char8_t>(program.string(m_strings[index])), "\"", isWithoutText(m_strings[index]) ? " without text" : "", "\n");
}

LiteralStringTable numberLiteralStrings(const Program& program, const FlowResults& flow, BuildContext& build)
{
    enum class Source : uint8_t { Program, Anywhere, Parser };
    // A validating build and the report of a mismatch hold every value against its type, also a text that the engine made and no code reads: Object.keys(). They read the texts of the members.
    bool everyWordHasText = build.validates() || build.recordedOptions().policy() == MismatchPolicy::Report;
    size_t numberOfStrings = program.strings.size();
    FastBitVector strings(numberOfStrings);
    // Each gets a word, and its text only if the program spells it.
    FastBitVector membersFromOutside(numberOfStrings);
    // Each gets a word only if the program spells it.
    FastBitVector membersHandedOn(numberOfStrings);
    FastBitVector constantsOfCode(numberOfStrings);
    FastBitVector variablesNamedByCode(program.variables.size());

    constexpr unsigned typesInShare = 4096;
    static_assert(!(typesInShare % 32));
    unsigned numberOfTypes = program.types.size();
    FastBitVector typesToVisit(numberOfTypes);
    forEachIndexInParallel(build.parallelLoops(), (numberOfTypes + typesInShare - 1) / typesInShare, [&](unsigned share) {
        for (unsigned index = share * typesInShare; index < std::min(numberOfTypes, (share + 1) * typesInShare); ++index)
            typesToVisit[index] = program.at(TypeID(index)).hasLiteralStrings();
    });
    FastBitVector typesFromOutsideToVisit = typesToVisit;
    FastBitVector typesHandedOnToVisit = typesToVisit;

    auto add = [&](StringID string) { strings.atomicSetAndCheck(string.index(), true); };
    // A string from outside the program gets its word where it comes in, so every member has one there. Where the program hands its own values on, a member that it spells nowhere does not occur.
    // A parse by type compares what it reads with the texts of the members.
    auto collectLiteralsOf = [&](TypeID id, Source source) {
        bool needsTexts = source == Source::Parser || everyWordHasText;
        FastBitVector& toVisit = needsTexts ? typesToVisit : source == Source::Anywhere ? typesFromOutsideToVisit : typesHandedOnToVisit;
        if (!toVisit.at(id.index()) || !toVisit.atomicSetAndCheck(id.index(), false))
            return;
        FastBitVector& members = needsTexts ? strings : source == Source::Anywhere ? membersFromOutside : membersHandedOn;
        for (StringID string : program.at(id).stringLiterals)
            members.atomicSetAndCheck(string.index(), true);
    };
    auto collectLiteralsOfAll = [&](std::span<const DeclaredType> positions, Source source) {
        for (const DeclaredType& declared : positions)
            collectLiteralsOf(declared.type, source);
    };
    auto collectStringsOfFunction = [&](FunctionID id, Source sourceOfArguments) {
        const Function& function = program.at(id);
        collectLiteralsOfAll(function.captures.span(), Source::Program);
        collectLiteralsOf(function.thisType.type, sourceOfArguments);
        collectLiteralsOfAll(function.parameters.span(), sourceOfArguments);
        collectLiteralsOfAll(function.results.span(), Source::Program);

        auto collectFromValue = [&](const Value& value) {
            if (value.opcode != Opcode::String)
                collectLiteralsOf(value.type, operationsHandingOnValues.contains(value.opcode) ? Source::Program : Source::Anywhere);
            else
                constantsOfCode.atomicSetAndCheck(value.string().index(), true);
            forEachType(value, [&](TypeID type) {
                collectLiteralsOf(type, Source::Anywhere);
            });
            for (unsigned side = 0; equalityComparisonsOfLiterals.contains(value.opcode) && side < 2; ++side) {
                if (!program.typeOf(function, definitionThroughUpcasts(function, value.operand(1 - side))).hasLiteralStrings())
                    continue;
                std::optional<ValueID> constant = constantThroughCasts(program, function, value.operand(side));
                if (constant && function.at(*constant).opcode == Opcode::String)
                    add(function.at(*constant).string());
            }
            if (VariableID variable = Value::opcodesWithVariable.contains(value.opcode) ? value.variable() : VariableID())
                variablesNamedByCode.atomicSetAndCheck(variable.index(), true);
        };
        auto collectFromTerminator = [&](const Terminator& terminator) {
            if (terminator.kind == TerminatorKind::SwitchString && program.typeOf(function, terminator.subjectOperand()).hasLiteralStrings())
                std::ranges::for_each(terminator.strings(), add);
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
    };

    BitVector isEnteredOtherwise = functionsCalledByHost(program, build.parallelLoops());
    for (const FlowIndirectCall& call : flow.indirectCalls())
        isEnteredOtherwise.set(call.callee.index());
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        if (!hasBody(program, id))
            return;
        bool everyCallIsSeen = !isEnteredOtherwise.get(index) && (program.at(id).hasOnlyDirectCallers() || callerStateOf(flow, id) == CallerState::AllKnown);
        collectStringsOfFunction(id, everyCallIsSeen ? Source::Program : Source::Anywhere);
    });

    for (const Layout& layout : program.layouts) {
        bool isParsedByType = layout.flow.observedUses.contains(ObservedUse::IsParsedByType);
        bool isFilledByNativeCode = layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse);
        for (const Field& field : layout.fields)
            collectLiteralsOf(field.declared.type, isParsedByType ? Source::Parser : !isFilledByNativeCode && field.flow.location.writers == WriterState::AllKnown ? Source::Program : Source::Anywhere);
    }
    BitVector described = findVariablesDescribedToRunTime(program, flow);
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        TypeID type = program.variables[index].declared.type;
        if (described.quickGet(index) || variablesNamedByCode.at(index)) {
            collectLiteralsOf(type, !described.quickGet(index) && program.variables[index].flow.writers == WriterState::AllKnown ? Source::Program : Source::Anywhere);
            continue;
        }
        // The text of a constant that was folded into its readers is in the image with them.
        for (StringID literal : program.at(type).stringLiterals) {
            if (constantsOfCode.at(literal.index()))
                add(literal);
        }
    }
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            if (const DataNode::String* string = node.tryAs<DataNode::String>())
                constantsOfCode[string->value.index()] = true;
        });
    }

    LiteralStringTable table;
    table.m_withoutText = BitVector(numberOfStrings);
    table.m_leftOut = BitVector(numberOfStrings);
    membersFromOutside.forEachSetBit([&](size_t index) {
        if (!strings.at(index) && !constantsOfCode.at(index))
            table.m_withoutText.quickSet(index);
        strings[index] = true;
    });
    membersHandedOn.forEachSetBit([&](size_t index) {
        if (strings.at(index))
            return;
        if (constantsOfCode.at(index))
            strings[index] = true;
        else
            table.m_leftOut.quickSet(index);
    });

    StringID emptyString = program.strings.emptyString();
    table.m_withoutText.quickClear(emptyString.index());
    table.m_strings.reserveInitialCapacity(strings.bitCount() + 1);
    table.m_strings.append(emptyString);
    strings.forEachSetBit([&](size_t index) {
        if (index != emptyString.index())
            table.m_strings.append(StringID(index));
    });

    // Compiled code and the run time write emptyLiteralStringWord for an empty string without asking the table, so number 0 always exists.
    if (build.exceeds(LimitID::MaxLiteralStrings, table.m_strings.size())) {
        size_t numberOfWords = std::max<uint64_t>(build.limit(LimitID::MaxLiteralStrings), 1);
        table.m_stringsWithoutWord = Vector<StringID>(table.m_strings.subspan(numberOfWords));
        for (StringID string : table.m_stringsWithoutWord)
            table.m_withoutText.quickClear(string.index());
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
