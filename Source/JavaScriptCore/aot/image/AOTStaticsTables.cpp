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
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

// No table of this part holds an offset into the tables, so nothing here is linked: the offsets are those of the statics.

static constexpr uint32_t sizeOfStaticsWord = sizeof(uint64_t);
static constexpr unsigned bitsInByteOfStatics = 8;

static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) <= GlobalBindings::identifierMask);

// Whoever finds the word empty takes it for the one closure of the function, which lies in the closure table.
static bool isFunctionVariableInStatics(const Typed::PlannedVariable& planned)
{
    return planned.hasWordInStatics() && planned.encoding == Typed::VariableEncoding::FunctionAsZero;
}

// Where the run time is the first to read such a variable, it makes that closure and keeps it in the cell of the function.
static bool runTimeMayMakeClosureOfVariable(const Typed::PlannedVariable& planned)
{
    if (planned.encoding != Typed::VariableEncoding::FunctionAsZero)
        return false;
    switch (planned.storage) {
    case Typed::VariableStorage::Statics:
        // Whether the variable gets a word depends on what the other groups name, and they are naming it at this moment. One cell too many costs four bytes.
        return true;
    case Typed::VariableStorage::ClosureTable:
        return planned.isDescribedToRunTime;
    case Typed::VariableStorage::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A variable that the plan added has the module and the name of what it stands for: a variable with a dead zone, a field of an environment that is never made, what the closures of a function that runs once would hold.
// Null: a heap snapshot shows nothing under the variable. A static field is shown with its class.
static StringID nameOfVariableInHeapSnapshots(const TableInputs& inputs, VariableID id, ModuleID& module)
{
    const Typed::Program& program = inputs.program;
    StringID name;
    if (const Typed::PromotedVariable* promoted = inputs.plan.promotedEnvironments().find(id)) {
        module = promoted->module;
        name = promoted->name;
    } else {
        VariableID written = id.index() < program.variables.size() ? id : inputs.plan.deferredDefinitions().variableBesideAdded(id);
        if (!written || program.at(written).owner)
            return { };
        module = program.at(written).module;
        name = program.at(written).name;
    }
    return showsVariablesOf(inputs, module) ? name : StringID();
}

static bool mayHoldCellForHeapSnapshots(const Typed::PlannedVariable& planned)
{
    switch (planned.storage) {
    case Typed::VariableStorage::Statics:
        // In a row of the table that representation says that the closure lies in the closure table.
        return traitsOf(planned.representation).holdsCell && planned.representation != SlotRepresentation::DeferredClosure;
    case Typed::VariableStorage::ClosureTable:
        return true;
    case Typed::VariableStorage::None:
        return false;
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

// `function helper() { }` that only code which is left out calls: nothing names the variable, it gets no word, and its name is in no table. A name that no table holds could be read out of the executable.
// So this runs behind the groups: what all of them have named says which variables get a word.
void nameVariablesThatHeapSnapshotsShow(const TableInputs& inputs, const GatheredMentions& mentions, const ParallelLoops& loops, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;
    auto mayHavePlace = [&](VariableID id, const Typed::PlannedVariable& planned) {
        switch (planned.storage) {
        case Typed::VariableStorage::Statics:
            return isVariableOfStaticsNamed(planned, id, names, mentions);
        case Typed::VariableStorage::ClosureTable: {
            FunctionID function = planned.function || id.index() >= program.variables.size() ? planned.function : program.at(id).function;
            return function && inputs.plan.hasClosureCell(function);
        }
        case Typed::VariableStorage::None:
            return false;
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

// Code has the number of a group from the plan. It tests the byte with that number behind the last word of the groups, and hands the number to the run time, which takes it for a row of the table.
// There a group ends where the next one begins.
static void encodeGroupsOfFilesInStatics(TableEncoder& encoder, const PlacedStatics& statics)
{
    const Typed::StaticsGroups& planned = encoder.plan.staticsGroups();
    uint32_t numberOfGroups = static_cast<uint32_t>(statics.groupsOfFilesFilledWhenEntered.size());

    Vector<StaticsOfFiles> files;
    Vector<uint32_t> importedFiles;
    bool agreesWithCode = numberOfGroups == planned.groups.size();
    uint32_t endOfPreviousGroup = statics.firstWordFilledWithFiles;
    for (const PlacedStaticsOfFiles& group : statics.groupsOfFilesFilledWhenEntered) {
        agreesWithCode &= group.firstWord == endOfPreviousGroup;
        endOfPreviousGroup = group.firstWord + group.numberOfWords;
        files.append({ group.firstWord, static_cast<uint32_t>(importedFiles.size()) });
        importedFiles.appendVector(group.importedGroups);
    }
    if (numberOfGroups) {
        uint64_t endOfWords = static_cast<uint64_t>(statics.endWordFilledWithFiles) * sizeOfStaticsWord;
        agreesWithCode &= endOfPreviousGroup == statics.endWordFilledWithFiles && statics.offsetOfFilledFiles == endOfWords && endOfWords + numberOfGroups <= statics.size;
        files.append({ statics.endWordFilledWithFiles, static_cast<uint32_t>(importedFiles.size()) });
    }
    agreesWithCode &= std::ranges::all_of(importedFiles, [&](uint32_t group) {
        return group < numberOfGroups;
    });
    if (!agreesWithCode)
        encoder.fail("GroupsOfFilesDifferFromWhatCodeTests"_s);

    for (size_t index : planned.functionsThatEnterFiles) {
        FunctionID function(index);
        std::optional<uint32_t> group = planned.groupFilledWhenEntered(encoder.program.at(function).module);
        if (!group || *group >= numberOfGroups)
            encoder.fail("FunctionEntersFileWithoutGroup"_s, function);
    }

    encoder.part.setTable(ProgramTableIndex::staticsOfFiles, files);
    encoder.part.setTable(ProgramTableIndex::staticsOfImportedFiles, importedFiles);
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
        for (const PlacedStaticsPadding& padding : m_statics.paddingOfFilesFilledWhenEntered)
            recordPadding(padding);
        for (const PlacedClassGuardCount& count : m_statics.classGuardCounts)
            recordClassGuardCount(count);

        TablePart& part = m_encoder.part;
        part.setTable(ProgramTableIndex::staticsRuns, collectRuns());
        // The words of modules, of module built-in cells, of data and of literals are among these. They have no table of their own here: each starts empty.
        part.setTable(ProgramTableIndex::staticsRoots, joinAdjacentRoots(m_statics.roots.span(), sizeOfStaticsWord));
        AOT::sort(m_narrowRoots);
        part.setTable(ProgramTableIndex::staticsNarrowRoots, joinAdjacentRoots(m_narrowRoots.span(), sizeof(uint32_t)));
        AOT::sort(m_functionVariables, { }, &FunctionVariableData::offset);
        part.setTable(ProgramTableIndex::functionVariables, m_functionVariables);
    }

private:
    bool hasGroupsOfFiles() const { return !m_statics.groupsOfFilesFilledWhenEntered.isEmpty(); }

    // One broken row says that the layout is broken. The rows behind it would say the same thing.
    void fail(ASCIILiteral rule, RowOwner owner = { })
    {
        if (!std::exchange(m_hasFailed, true))
            m_encoder.fail(rule, owner);
    }

    std::optional<uint32_t> groupOfFilesOf(VariableID id) const
    {
        // A variable that the plan added has the module of the variable that it stands beside.
        VariableID written = id.index() < m_encoder.program.variables.size() ? id : m_encoder.plan.deferredDefinitions().variableBesideAdded(id);
        if (!written)
            return std::nullopt;
        return m_encoder.plan.staticsGroups().groupFilledWhenEntered(m_encoder.program.at(written).module);
    }

    bool isPlacedInItsRegion(VariableID id, const PlacedVariable& placed) const
    {
        uint32_t word = placed.offset / sizeOfStaticsWord;
        switch (placed.plan.region) {
        case StaticsRegion::FilledAtProgramEntry:
            return placed.offset < m_statics.sizeFilledAtProgramEntry;
        case StaticsRegion::FilledAtInstanceCreation:
            // The words in front are read-only until the program is entered. From the first group of files on, the run time fills nothing with the instance.
            if (placed.offset < m_statics.sizeFilledAtProgramEntry)
                return false;
            return !placed.initialWord || !hasGroupsOfFiles() || word < m_statics.firstWordFilledWithFiles;
        case StaticsRegion::FilledWithFile: {
            // Whoever enters a file fills the group of that file, and the groups that it imports.
            std::optional<uint32_t> group = groupOfFilesOf(id);
            if (!group || *group >= m_statics.groupsOfFilesFilledWhenEntered.size())
                return false;
            const PlacedStaticsOfFiles& files = m_statics.groupsOfFilesFilledWhenEntered[*group];
            return word - files.firstWord < files.numberOfWords;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Null where the slot is recorded, else the rule that it breaks.
    ASCIILiteral recordSlot(uint32_t offset, SlotRepresentation representation, uint64_t initialWord)
    {
        SlotTraits traits = traitsOf(representation);
        if (offset % traits.width || static_cast<uint64_t>(offset) + traits.width > m_statics.size)
            return "SlotOutsideStatics"_s;
        if (traits.width < sizeOfStaticsWord && initialWord >> (bitsInByteOfStatics * traits.width))
            return "InitialWordWiderThanSlot"_s;
        // The layout lists the words that the collector visits whole. The narrow references are listed here.
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
        if (!isPlacedInItsRegion(id, placed))
            fail("VariableOutsideItsRegionOfStatics"_s, id);
        if (isFunctionVariableInStatics(placed.plan))
            recordFunctionVariable(id, placed);
    }

    void recordFunctionVariable(VariableID id, const PlacedVariable& placed)
    {
        uint32_t entrypoint = m_encoder.entrypointOf(placed.function, "what the first read makes of the variable"_s, id);
        m_functionVariables.append({ placed.offset, entrypoint });
        // The run time keeps the closure in the cell that the table of closure functions has for the entry.
        const FunctionNumbers& functions = m_encoder.numbers.functions();
        bool isClosureFunction = entrypoint >= functions.firstClosureFunction().index() && entrypoint < functions.endOfClosureFunctions().index();
        if (!isClosureFunction || !m_encoder.numbers.closureCellNumber(placed.function))
            fail("FunctionVariableWithoutClosureCell"_s, id);
    }

    void recordPadding(const PlacedStaticsPadding& padding)
    {
        uint32_t word = padding.offset / sizeOfStaticsWord;
        if (word < m_statics.firstWordFilledWithFiles || word >= m_statics.endWordFilledWithFiles) {
            fail("PaddingOutsideGroupsOfFiles"_s);
            return;
        }
        if (ASCIILiteral brokenRule = recordSlot(padding.offset, padding.representation, padding.initialWord); !brokenRule.isNull())
            fail(brokenRule);
    }

    // The run time counts from the first class on, so the word is writable and filled from the start of the instance.
    void recordClassGuardCount(const PlacedClassGuardCount& count)
    {
        uint32_t word = count.offset / sizeOfStaticsWord;
        bool liesInStatics = !(count.offset % sizeof(uint32_t)) && static_cast<uint64_t>(count.offset) + sizeof(uint32_t) <= m_statics.size;
        bool isFilledWithInstance = count.offset >= m_statics.sizeFilledAtProgramEntry && (!hasGroupsOfFiles() || word < m_statics.firstWordFilledWithFiles);
        if (!liesInStatics || !isFilledWithInstance) {
            fail("ClassGuardCountOutsideWordsFilledWithInstance"_s);
            return;
        }
        m_initialWords[word] |= static_cast<uint64_t>(count.firstValue) << (bitsInByteOfStatics * (count.offset % sizeOfStaticsWord));
    }

    // Divided as ProgramStatics divides the words. A run goes on from one group of files into the next.
    Vector<StaticsRun> collectRuns() const
    {
        uint32_t firstWordFilledAtInstanceCreation = m_statics.sizeFilledAtProgramEntry / sizeOfStaticsWord;
        uint32_t firstWordFilledWithFiles = hasGroupsOfFiles() ? m_statics.firstWordFilledWithFiles : UINT32_MAX;
        uint32_t numberOfWords = static_cast<uint32_t>(m_initialWords.size());
        Vector<StaticsRun> runs;
        for (uint32_t first = 0; first < numberOfWords;) {
            uint64_t bits = m_initialWords[first];
            uint32_t end = first + 1;
            if (bits) {
                while (end < numberOfWords && m_initialWords[end] == bits && end != firstWordFilledAtInstanceCreation && end != firstWordFilledWithFiles)
                    ++end;
                runs.append({ first, end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
            }
            first = end;
        }
        return runs;
    }

    TableEncoder& m_encoder;
    const PlacedStatics& m_statics;
    // [word of the statics]
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

// Where ProgramStatics::cellIn() looks. Nothing: no cell ever lies there.
static std::optional<SlotData> slotOfVariableInHeapSnapshots(const TableEncoder& encoder, VariableID id, const PlacedVariable& placed)
{
    if (!mayHoldCellForHeapSnapshots(placed.plan))
        return std::nullopt;
    if (placed.plan.storage == Typed::VariableStorage::Statics)
        return placed.hasWord && placed.offset <= SlotData::maxOffset ? std::optional(SlotData::create(placed.offset, placed.plan.representation)) : std::nullopt;
    FunctionID function = placed.function;
    if (!function && id.index() < encoder.program.variables.size())
        function = encoder.program.at(id).function;
    // Without the entry and the cell nothing ever makes the closure of the table.
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

    NamedVariablesWriter writer;
    for (size_t first = 0; first < named.size();) {
        size_t end = first;
        while (end < named.size() && named[end].module == named[first].module)
            ++end;
        writer.beginModule(end - first);
        for (const NamedVariable& variable : named.span().subspan(first, end - first))
            writer.append(variable.data);
        first = end;
    }
    encoder.part.setTable(ProgramTableIndex::namedVariables, writer.bytes());
}

void encodeStaticsTables(TableEncoder& encoder)
{
    const PlacedStatics& statics = encoder.numbers.statics();
    StaticsLayout layout { };
    layout.sizeFilledAtProgramEntry = statics.sizeFilledAtProgramEntry;
    layout.distanceFromArrayProfileToLastArray = statics.distanceFromArrayProfileToLastArray;
    encoder.part.setTable(ProgramTableIndex::staticsLayout, Vector<StaticsLayout, 1> { layout });
    encodeGroupsOfFilesInStatics(encoder, statics);
    StaticsWordTables(encoder).encode();
    encodeNamesOfVariables(encoder);
    encoder.part.setTable(ProgramTableIndex::staticsClearedEachCollection, statics.clearedEachCollection);
    encoder.part.setTable(ProgramTableIndex::classGuardCounts, Vector<ClassGuardCountData>(statics.guardsOfClassGuardCounts.size(), [&](size_t index) {
        return ClassGuardCountData { statics.guardsOfClassGuardCounts[index].bit, statics.guardsOfClassGuardCounts[index].offsetOfCount };
    }));
    encodeTableOfGlobalBindings(encoder);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
