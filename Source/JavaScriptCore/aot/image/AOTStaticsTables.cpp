/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTGlobalBindings.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTPrefixCode.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

static constexpr uint32_t sizeOfStaticsWord = sizeof(uint64_t);
static constexpr unsigned bitsInByteOfStatics = 8;

static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) <= GlobalBindings::identifierMask);

static bool isFunctionVariableInStatics(const Typed::PlannedVariable& planned)
{
    return planned.hasWordInStatics() && planned.encoding == Typed::VariableEncoding::FunctionAsZero;
}

static bool runTimeMayMakeClosureOfVariable(const Typed::PlannedVariable& planned)
{
    if (planned.encoding != Typed::VariableEncoding::FunctionAsZero)
        return false;
    switch (planned.storage) {
    case Typed::VariableStorage::Statics:
        return true;
    case Typed::VariableStorage::ClosureTable:
        return planned.isDescribedToRunTime;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StringID nameOfVariableInHeapSnapshots(const TableInputs& inputs, VariableID id, ModuleID& module)
{
    const Typed::Program& program = inputs.program;
    StringID name;
    if (const Typed::PromotedVariable* promoted = inputs.plan.promotedEnvironments().find(id)) {
        module = promoted->module;
        name = promoted->name;
    } else {
        if (id.index() >= program.variables.size() || program.at(id).owner)
            return { };
        module = program.at(id).module;
        name = program.at(id).name;
    }
    return showsVariablesOf(inputs, module) ? name : StringID();
}

static bool mayHoldCellForHeapSnapshots(const Typed::PlannedVariable& planned)
{
    switch (planned.storage) {
    case Typed::VariableStorage::Statics:
        return traitsOf(planned.representation).holdsCell && planned.representation != SlotRepresentation::DeferredClosure;
    case Typed::VariableStorage::ClosureTable:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void nameWhatStaticsTablesName(const TableInputs& inputs, const GatheredMentions& mentions, NamesOfTables& names)
{
    const NumberedKeys& globalBindings = mentions.keys(NumberSpace::GlobalBinding);
    for (uint32_t index = 0; index < globalBindings.size(); ++index) {
        std::span<const uint32_t> key = globalBindings.keyAt(index);
        RELEASE_ASSERT(key.size() == 2);
        names.keys.set(key[0]);
    }

    uint32_t numberOfVariables = inputs.plan.slots().numberOfVariables();
    for (uint32_t index = 0; index < numberOfVariables; ++index) {
        Typed::PlannedVariable planned = inputs.plan.variable(VariableID(index));
        if (!runTimeMayMakeClosureOfVariable(planned))
            continue;
        RELEASE_ASSERT(planned.function);
        names.functionsWithClosureCell.set(planned.function.index());
    }
}

void nameVariablesThatHeapSnapshotsShow(const TableInputs& inputs, const GatheredMentions& mentions, const ParallelLoops& loops, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;
    auto mayHavePlace = [&](VariableID id, const Typed::PlannedVariable& planned) {
        switch (planned.storage) {
        case Typed::VariableStorage::Statics:
            return isVariableOfStaticsNamed(planned, id, names, mentions);
        case Typed::VariableStorage::ClosureTable: {
            FunctionID function = planned.function || id.index() >= program.variables.size() ? planned.function : program.at(id).function;
            return function && inputs.plan.hasClosureCell(function) && isClosureCellNamed(function, names, mentions);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    Vector<uint32_t> shown = findIndicesInParallel(loops, inputs.plan.slots().numberOfVariables(), [&](unsigned index) {
        Typed::PlannedVariable planned = inputs.plan.variable(VariableID(index));
        return mayHoldCellForHeapSnapshots(planned) && mayHavePlace(VariableID(index), planned);
    });
    for (uint32_t index : shown) {
        ModuleID module;
        if (StringID name = nameOfVariableInHeapSnapshots(inputs, VariableID(index), module))
            names.strings.set(name.index());
    }
}

static Vector<StaticsRange> joinAdjacentRoots(std::span<const uint32_t> ascendingOffsets, uint32_t width)
{
    Vector<StaticsRange> ranges;
    for (uint32_t offset : ascendingOffsets) {
        RELEASE_ASSERT(!(offset % width));
        if (!ranges.isEmpty() && ranges.last().end() == offset)
            ranges.last().size += width;
        else
            ranges.append({ offset, width });
    }
    return ranges;
}

namespace {

class StaticsWordTables {
    WTF_MAKE_NONCOPYABLE(StaticsWordTables);
public:
    explicit StaticsWordTables(TableEncoder& encoder)
        : m_encoder(encoder)
        , m_statics(encoder.numbers.statics())
        , m_initialWords(FillWith { }, (static_cast<size_t>(m_statics.size) + sizeOfStaticsWord - 1) / sizeOfStaticsWord, 0)
    {
    }

    void encode()
    {
        for (uint32_t index = 0; index < m_statics.variables.size(); ++index)
            recordVariable(VariableID(index), m_statics.variables[index]);
        for (const PlacedClassGuardCount& count : m_statics.classGuardCounts)
            recordClassGuardCount(count);

        TablePart& part = m_encoder.part;
        part.setTable(ProgramTableIndex::staticsRuns, collectRuns());
        part.setTable(ProgramTableIndex::staticsRoots, joinAdjacentRoots(m_statics.roots.span(), sizeOfStaticsWord));
        AOT::sort(m_narrowRoots);
        part.setTable(ProgramTableIndex::staticsNarrowRoots, joinAdjacentRoots(m_narrowRoots.span(), sizeof(uint32_t)));
        AOT::sort(m_functionVariables, { }, &FunctionVariableData::offset);
        part.setTable(ProgramTableIndex::functionVariables, m_functionVariables);
    }

private:
    COLD NEVER_INLINE void fail(ASCIILiteral rule, RowOwner owner = { })
    {
        if (!std::exchange(m_hasFailed, true))
            m_encoder.fail(rule, owner);
    }

    ASCIILiteral recordSlot(uint32_t offset, SlotRepresentation representation, uint64_t initialWord)
    {
        SlotTraits traits = traitsOf(representation);
        if (offset % traits.width || static_cast<uint64_t>(offset) + traits.width > m_statics.size)
            return "SlotOutsideStatics"_s;
        if (traits.width < sizeOfStaticsWord && initialWord >> (bitsInByteOfStatics * traits.width))
            return "InitialWordWiderThanSlot"_s;
        if (traits.isNarrowReference)
            m_narrowRoots.append(offset);
        else if (traits.holdsCell && !std::ranges::binary_search(m_statics.roots, offset))
            return "CellOfStaticsIsNoRoot"_s;
        m_initialWords[offset / sizeOfStaticsWord] |= initialWord << (bitsInByteOfStatics * (offset % sizeOfStaticsWord));
        return { };
    }

    void recordVariable(VariableID id, const PlacedVariable& placed)
    {
        if (!placed.hasWordInStatics())
            return;
        if (ASCIILiteral brokenRule = recordSlot(placed.offset, placed.plan.representation, placed.initialWord); !brokenRule.isNull()) {
            fail(brokenRule, id);
            return;
        }
        if (isFunctionVariableInStatics(placed.plan))
            recordFunctionVariable(id, placed);
    }

    void recordFunctionVariable(VariableID id, const PlacedVariable& placed)
    {
        uint32_t entrypoint = m_encoder.entrypointOf(placed.function, "what the first read makes of the variable"_s, id);
        m_functionVariables.append({ placed.offset, entrypoint });
        const FunctionNumbers& functions = m_encoder.numbers.functions();
        bool isClosureFunction = entrypoint >= functions.firstClosureFunction().index() && entrypoint < functions.endOfClosureFunctions().index();
        if (!isClosureFunction || !m_encoder.numbers.closureCellNumber(placed.function))
            fail("FunctionVariableWithoutClosureCell"_s, id);
    }

    void recordClassGuardCount(const PlacedClassGuardCount& count)
    {
        uint32_t word = count.offset / sizeOfStaticsWord;
        bool liesInStatics = !(count.offset % sizeof(uint32_t)) && static_cast<uint64_t>(count.offset) + sizeof(uint32_t) <= m_statics.size;
        if (!liesInStatics) {
            fail("ClassGuardCountOutsideWordsFilledWithInstance"_s);
            return;
        }
        m_initialWords[word] |= static_cast<uint64_t>(count.firstValue) << (bitsInByteOfStatics * (count.offset % sizeOfStaticsWord));
    }

    Vector<StaticsRun> collectRuns() const
    {
        uint32_t numberOfWords = static_cast<uint32_t>(m_initialWords.size());
        Vector<StaticsRun> runs;
        for (uint32_t first = 0; first < numberOfWords;) {
            uint64_t bits = m_initialWords[first];
            uint32_t end = first + 1;
            if (bits) {
                while (end < numberOfWords && m_initialWords[end] == bits)
                    ++end;
                runs.append({ first, end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
            }
            first = end;
        }
        return runs;
    }

    TableEncoder& m_encoder;
    const PlacedStatics& m_statics;
    Vector<uint64_t> m_initialWords;
    Vector<uint32_t> m_narrowRoots;
    Vector<FunctionVariableData> m_functionVariables;
    bool m_hasFailed { false };
};

} // anonymous namespace

static void encodeTableOfGlobalBindings(TableEncoder& encoder)
{
    std::span<const PlacedGlobalBinding> bindings = encoder.numbers.globalBindings();
    encoder.part.setTable(ProgramTableIndex::globalBindings, Vector<uint32_t>(bindings.size(), [&](size_t index) {
        const PlacedGlobalBinding& binding = bindings[index];
        uint32_t word = encoder.identifierNumber(binding.key, "a global name with a binding record"_s);
        if (binding.flags.contains(GlobalBindingFlags::SeesScriptBindings))
            word |= GlobalBindings::seesScriptBindingsFlag;
        if (binding.isStoredByName)
            word |= GlobalBindings::isStoredByNameFlag;
        if (binding.flags.contains(GlobalBindingFlags::IsForTypeOf))
            word |= GlobalBindings::isForTypeOfFlag;
        if (binding.isStoredThroughBinding)
            word |= GlobalBindings::isStoredThroughBindingFlag;
        return word;
    }));
}

static std::optional<SlotData> slotOfVariableInHeapSnapshots(const TableEncoder& encoder, VariableID id, const PlacedVariable& placed)
{
    if (!mayHoldCellForHeapSnapshots(placed.plan))
        return std::nullopt;
    if (placed.plan.storage == Typed::VariableStorage::Statics)
        return placed.hasWord && placed.offset <= SlotData::maxOffset ? std::optional(SlotData::create(placed.offset, placed.plan.representation)) : std::nullopt;
    FunctionID function = placed.function;
    if (!function && id.index() < encoder.program.variables.size())
        function = encoder.program.at(id).function;
    std::optional<FunctionID> entry = encoder.numbers.closureCellNumber(function) ? encoder.findEntrypoint(function) : std::nullopt;
    return entry && entry->index() <= SlotData::maxOffset ? std::optional(SlotData::create(entry->index(), SlotRepresentation::DeferredClosure)) : std::nullopt;
}

static void encodeNamesOfVariables(TableEncoder& encoder)
{
    struct NamedVariable {
        uint32_t module;
        VariableData data;
    };
    const Vector<PlacedVariable>& variables = encoder.numbers.statics().variables;
    Vector<NamedVariable> named;
    for (uint32_t index = 0; index < variables.size(); ++index) {
        std::optional<SlotData> slot = slotOfVariableInHeapSnapshots(encoder, VariableID(index), variables[index]);
        ModuleID module;
        if (StringID name = slot ? nameOfVariableInHeapSnapshots(encoder.inputs, VariableID(index), module) : StringID())
            named.append({ module.index(), { encoder.stringNumber(name), *slot } });
    }
    AOT::sort(named, { }, [](const NamedVariable& variable) {
        return std::tuple(variable.module, variable.data.name, variable.data.slot.encoded());
    });

    Vector<Vector<uint32_t>> closureNumbers(encoder.program.modules.size());
    uint32_t firstClosureFunction = encoder.numbers.functions().firstClosureFunction().index();
    uint32_t endOfClosureFunctions = encoder.numbers.functions().endOfClosureFunctions().index();
    for (uint32_t index = 0; index < encoder.program.functions.size(); ++index) {
        ModuleID module = encoder.program.at(FunctionID(index)).module;
        std::optional<FunctionID> entry = showsVariablesOf(encoder.inputs, module) ? encoder.findEntrypoint(FunctionID(index)) : std::nullopt;
        if (entry && entry->index() >= firstClosureFunction && entry->index() < endOfClosureFunctions)
            closureNumbers[module.index()].append(entry->index() - firstClosureFunction);
    }

    NamedVariablesWriter writer;
    size_t first = 0;
    for (uint32_t module = 0; module < encoder.program.modules.size(); ++module) {
        if (!showsVariablesOf(encoder.inputs, ModuleID(module)))
            continue;
        size_t end = first;
        while (end < named.size() && named[end].module == module)
            ++end;
        writer.beginModule(module, end - first);
        for (const NamedVariable& variable : named.span().subspan(first, end - first))
            writer.append(variable.data);
        first = end;
        AOT::sort(closureNumbers[module]);
        closureNumbers[module].shrink(std::ranges::unique(closureNumbers[module]).begin() - closureNumbers[module].begin());
        writer.endModule(closureNumbers[module].span());
    }
    RELEASE_ASSERT(first == named.size());
    encoder.part.setTable(ProgramTableIndex::namedVariables, encodeVarintBytes(writer.bytes().span()));
}

static Vector<uint32_t> collectBitsOfNamesOfClassGuards(const TableEncoder& encoder)
{
    const PlacedStatics& statics = encoder.numbers.statics();
    Vector<uint32_t> names;
    for (const Vector<PlacedClassGuardOfName>* guards : { &statics.classGuardsOfNames, &statics.namesOfClassGuardsOfSeveralNames }) {
        for (const PlacedClassGuardOfName& guard : *guards) {
            if (std::optional<uint32_t> name = encoder.numbers.numberOf(guard.spaceOfName, std::span<const uint32_t>(&guard.name, 1)))
                names.append(*name);
        }
    }
    Vector<uint32_t> words(FillWith { }, names.isEmpty() ? 0 : *std::ranges::max_element(names) / 32 + 1, 0);
    for (uint32_t name : names)
        words[name / 32] |= 1u << name % 32;
    return words;
}

void encodeStaticsTables(TableEncoder& encoder)
{
    const PlacedStatics& statics = encoder.numbers.statics();
    StaticsLayout layout { };
    layout.distanceFromArrayProfileToLastArray = statics.distanceFromArrayProfileToLastArray;
    encoder.part.setTable(ProgramTableIndex::staticsLayout, Vector<StaticsLayout, 1> { layout });
    StaticsWordTables(encoder).encode();
    encodeNamesOfVariables(encoder);
    encoder.part.setTable(ProgramTableIndex::staticsClearedEachCollection, statics.clearedEachCollection);
    encoder.part.setTable(ProgramTableIndex::classGuardCounts, Vector<ClassGuardCountData>(statics.guardsOfClassGuardCounts.size(), [&](size_t index) {
        return ClassGuardCountData { statics.guardsOfClassGuardCounts[index].bit, statics.guardsOfClassGuardCounts[index].offsetOfCount };
    }));
    encoder.part.setTable(ProgramTableIndex::namesOfClassGuards, collectBitsOfNamesOfClassGuards(encoder));
    encoder.part.setTable(ProgramTableIndex::classesBelowWithGuardsOfNames, collectClassesBelowWithGuardsOfNames(encoder));
    encodeTableOfGlobalBindings(encoder);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
