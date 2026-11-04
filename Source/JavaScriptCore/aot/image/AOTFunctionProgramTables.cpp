/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <bit>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) <= Number24::max);
static_assert(defaultLimitValue(LimitID::ImageClosureFunctions, hostArchitecture()) < ConstructEntrypointData::allocatesThis);

static constexpr size_t functionsInGroupOfNames = 32;
static_assert(!(indicesInRangeOfLoop % functionsInGroupOfNames), "No group of names lies in two ranges.");

namespace {

struct DistinctFunctionRecord {
    uint32_t bits;
    uint32_t count;
    uint32_t number;
};

struct CountsOfClosureFunction {
    uint32_t length { 0 };
    unsigned references { 0 };
    unsigned captures { 0 };
};

// What a range of the functions of the image adds to the tables that do not have one row for each function. In the order of the functions.
struct RowsOfFunctionRange {
    Vector<Number24> names;
    Vector<ImplementedBuiltinData> implementedBuiltins;
    Vector<FunctionWithLargeCountsData> functionsWithLargeCounts;
    Vector<NamedCaptureData> namedCaptures;
    Vector<ConstructEntrypointData> constructEntrypoints;
    // Ascending by bits, each once.
    Vector<DistinctFunctionRecord> records;
    size_t firstRecordListedByFunction { 0 };
};

// The rows of every range, one range behind the other.
template<typename Row>
struct RowsOfAllFunctionRanges {
    void place(std::span<const RowsOfFunctionRange> ranges, Vector<Row> RowsOfFunctionRange::* list)
    {
        firstOfRange.reserveInitialCapacity(ranges.size());
        size_t count = 0;
        for (const RowsOfFunctionRange& range : ranges) {
            firstOfRange.append(count);
            count += (range.*list).size();
        }
        rows.grow(count);
    }
    void write(size_t range, const Vector<Row>& rowsOfRange) { memcpySpan(rows.mutableSpan().subspan(firstOfRange[range]), rowsOfRange.span()); }

    Vector<size_t> firstOfRange;
    Vector<Row> rows;
};

// The tables of the group, made in two loops over the ranges of forEachRangeOfIndicesInParallel() of the functions of the image.
class FunctionTablesByRange {
    WTF_MAKE_NONCOPYABLE(FunctionTablesByRange);
public:
    explicit FunctionTablesByRange(const TableEncoder&);

    size_t numberOfFunctions() const { return m_bitsOfRecords.size(); }

    // The first loop: all that is asked of the program, the plan and the numbers. What is said of a function hangs on its number alone.
    void collect(size_t first, size_t end);
    // Between the loops.
    void place();
    // The second loop: what hangs on the rows of other ranges.
    void write(size_t first, size_t end);
    // A part holds equal tables once, so where a table lies hangs on the bytes of those in front of it: they are appended one by one.
    void appendTo(TablePart&);

private:
    void collectFunction(uint32_t index, RowsOfFunctionRange&);
    void collectName(uint32_t index, std::optional<BuiltinID> implemented, Vector<Number24>&);
    void collectClosureFunction(uint32_t index, CodeUnit, FunctionKindData, Vector<FunctionWithLargeCountsData>&);
    void collectNamesOfCaptures(uint32_t index, CodeUnit, Vector<NamedCaptureData>&) const;
    void collectConstructEntrypoint(uint32_t index, CodeUnit, FunctionKindData, Vector<ConstructEntrypointData>&) const;
    void numberRecords();
    void forEachTable(const auto& functor) const;

    const TableEncoder& m_encoder;
    const uint32_t m_firstDerived;
    const uint32_t m_firstClosureFunction;
    const uint32_t m_endOfClosureFunctions;
    const bool m_numbersOfStringsFit;

    Vector<RowsOfFunctionRange> m_ranges;
    RowsOfAllFunctionRanges<Number24> m_names;
    RowsOfAllFunctionRanges<ImplementedBuiltinData> m_implementedBuiltins;
    RowsOfAllFunctionRanges<FunctionWithLargeCountsData> m_functionsWithLargeCounts;
    RowsOfAllFunctionRanges<NamedCaptureData> m_namedCaptures;
    Vector<uint8_t> m_bytesOfNamedCaptures;
    RowsOfAllFunctionRanges<ConstructEntrypointData> m_constructEntrypoints;

    // [function of the image]: its FunctionRecordData.
    Vector<uint32_t> m_bitsOfRecords;
    Vector<uint8_t> m_recordNumbers;
    Vector<FunctionRecordData> m_records;
    Vector<FunctionRecordByFunctionData> m_recordsByFunction;
    // Until write(), the second word of a group counts from the first name of its range.
    Vector<uint32_t> m_groupsOfNames;
    // [function of the image - m_firstDerived]
    Vector<Number24> m_sources;
    // [closure number]
    Vector<FunctionKindData> m_kinds;
    Vector<uint8_t> m_lengths;
    Vector<uint8_t> m_captureCounts;
    Vector<Number24> m_cellNumbers;
};

} // anonymous namespace

// ---- What both functions of the group read

// A stack trace names the functions that have code and those that were inlined into it. A closure shows the name of its function, and its entry has code.
static bool isShownByName(const TableInputs& inputs, FunctionID id)
{
    return inputs.bodyFacts.functionsWithFrames.get(id.index());
}

static StringID nameShownForFunction(const Typed::Program& program, FunctionID id, std::optional<BuiltinID> implemented)
{
    const Typed::Function& function = program.at(id);
    if (!function.name || !implemented)
        return function.name;
    const BuiltinTable& builtins = program.builtins();
    // A function of the compiler stands for what the program wrote, and its own name says what that is.
    if (function.enclosingRuntimeFunction && builtins.at(function.enclosingRuntimeFunction).role == RuntimeFunctionRole::CompilerFunction)
        return function.name;
    const Builtin& builtin = builtins.at(*implemented);
    if (builtin.kind == BuiltinKind::Accessor || !builtin.name)
        return function.name;
    StringID nameOfBuiltin = program.stringOf(builtin.name);
    return nameOfBuiltin ? nameOfBuiltin : function.name;
}

// The references that the closures of a function hold in place of variables. A capture that is removed lies in a variable, which has the name: nameOfVariableInHeapSnapshots() of AOTStaticsTables.cpp.
static void forEachCaptureShownByName(const TableInputs& inputs, FunctionID id, NOESCAPE const Invocable<void(unsigned capture, StringID name)> auto& functor)
{
    const Typed::Function& function = inputs.program.at(id);
    if (!showsVariablesOf(inputs, function.module))
        return;
    for (unsigned capture = 0; capture < function.captures.size(); ++capture) {
        StringID name = function.captures[capture].variableName;
        if (name && Typed::holdsCells(Typed::closureCaptureRepresentationOf(inputs.program, inputs.plan.conventions(), id, capture)))
            functor(capture, name);
    }
}

void nameWhatFunctionTablesName(const TableInputs& inputs, const GatheredMentions&, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;
    // In place of a name that was never written, and of every name in an obfuscated build: a string that is named gets its text into the image.
    names.strings.set(program.strings.emptyString().index());
    if (inputs.options.obfuscates())
        return;
    const Typed::CallingConventions& conventions = inputs.plan.conventions();

    auto nameFunctions = [&](size_t first, size_t end, Vector<StringID>& named) {
        for (size_t index = first; index < end; ++index) {
            FunctionID id(index);
            if (!isShownByName(inputs, id))
                continue;
            if (StringID name = nameShownForFunction(program, id, Typed::functionTraceOf(program, conventions, { id, Typed::CodeRole::Body }).implements()))
                named.append(name);
        }
    };
    auto nameCaptures = [&](size_t first, size_t end, Vector<StringID>& named) {
        for (size_t position = first; position < end; ++position) {
            const CompiledCode& code = inputs.functions[position];
            // BodyFactsForTables::functionsWithFrames holds the function of every unit that has code.
            RELEASE_ASSERT(isShownByName(inputs, code.unit.function));
            if (code.unit != conventions.closureEntryOf(code.unit.function))
                continue;
            forEachCaptureShownByName(inputs, code.unit.function, [&](unsigned, StringID name) {
                named.append(name);
            });
        }
    };
    std::span<const FusedCallEntry> fusedCallEntries = inputs.functions.fusedCallEntries();
    auto nameCapturesOfFusedCallEntries = [&](size_t first, size_t end, Vector<StringID>& named) {
        for (const FusedCallEntry& fused : fusedCallEntries.subspan(first, end - first)) {
            forEachCaptureShownByName(inputs, fused.unit.function, [&](unsigned, StringID name) {
                named.append(name);
            });
        }
    };

    // The ranges of the functions, behind them those of the units and those of the fused call entries, in one loop: to start a second one costs more than some of these ranges.
    const size_t numberOfFunctions = program.functions.size();
    const size_t numberOfUnits = inputs.functions.size();
    const size_t rangesOfFunctions = numberOfRangesOfIndices(numberOfFunctions);
    const size_t rangesOfUnits = numberOfRangesOfIndices(numberOfUnits);
    Vector<Vector<StringID>> namedInRanges(rangesOfFunctions + rangesOfUnits + numberOfRangesOfIndices(fusedCallEntries.size()));
    forEachIndexInParallel(inputs.build->parallelLoops(), static_cast<unsigned>(namedInRanges.size()), [&](unsigned range) {
        Vector<StringID> named;
        if (range < rangesOfFunctions) {
            size_t first = range * indicesInRangeOfLoop;
            nameFunctions(first, std::min(numberOfFunctions, first + indicesInRangeOfLoop), named);
        } else if (range < rangesOfFunctions + rangesOfUnits) {
            size_t first = (range - rangesOfFunctions) * indicesInRangeOfLoop;
            nameCaptures(first, std::min(numberOfUnits, first + indicesInRangeOfLoop), named);
        } else {
            size_t first = (range - rangesOfFunctions - rangesOfUnits) * indicesInRangeOfLoop;
            nameCapturesOfFusedCallEntries(first, std::min(fusedCallEntries.size(), first + indicesInRangeOfLoop), named);
        }
        namedInRanges[range] = WTF::move(named);
    });
    for (const Vector<StringID>& named : namedInRanges) {
        for (StringID name : named)
            names.strings.set(name.index());
    }
}

// ---- The functions as the image numbers them

static CodeUnit unitOfImageFunction(const TableEncoder& encoder, uint32_t index)
{
    const FunctionNumbers& functions = encoder.numbers.functions();
    if (index < functions.firstDerived())
        return { FunctionID(index), Typed::CodeRole::Body };
    return encoder.plan.conventions().unitAt(functions.unitOf(FunctionID(index)));
}

// The kind of a function is that of its function objects, whichever of its units runs.
static CodeUnit closureEntryThatDescribes(const Typed::CallingConventions& conventions, CodeUnit unit)
{
    switch (unit.role) {
    case Typed::CodeRole::GenericCallEntry:
    case Typed::CodeRole::GenericConstructEntry:
    case Typed::CodeRole::AllocatingConstructEntry:
    case Typed::CodeRole::GenericEntryOfBoundFunction:
        return unit;
    case Typed::CodeRole::BoundFunction:
        return { unit.function, Typed::CodeRole::GenericEntryOfBoundFunction, unit.numberOfBoundArguments };
    case Typed::CodeRole::Body:
    case Typed::CodeRole::CoroutineBody:
    case Typed::CodeRole::ModuleStart:
    case Typed::CodeRole::SlowBody:
    case Typed::CodeRole::ParameterGuard:
    case Typed::CodeRole::TableEntry:
    case Typed::CodeRole::TableLookup:
    case Typed::CodeRole::Serializer:
        return conventions.closureEntryOf(unit.function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FunctionTablesByRange::FunctionTablesByRange(const TableEncoder& encoder)
    : m_encoder(encoder)
    , m_firstDerived(encoder.numbers.functions().firstDerived())
    , m_firstClosureFunction(encoder.numbers.functions().firstClosureFunction().index())
    , m_endOfClosureFunctions(encoder.numbers.functions().endOfClosureFunctions().index())
    , m_numbersOfStringsFit(encoder.program.strings.size() <= Number24::max)
{
    RELEASE_ASSERT(m_firstDerived == encoder.program.functions.size());
    if (!m_numbersOfStringsFit)
        encoder.build.reportLimitExceeded(LimitID::MaxListedStrings, encoder.program.strings.size());

    size_t numberOfFunctions = encoder.numbers.functions().numberOfFunctions();
    size_t numberOfClosureFunctions = m_endOfClosureFunctions - m_firstClosureFunction;
    m_ranges.grow(numberOfRangesOfIndices(numberOfFunctions));
    m_bitsOfRecords.grow(numberOfFunctions);
    m_recordNumbers.grow(numberOfFunctions);
    m_groupsOfNames.fill(0, ProgramTables::numberOfFunctionNameGroupWords(m_firstDerived));
    m_sources.grow(numberOfFunctions - m_firstDerived);
    m_kinds.grow(numberOfClosureFunctions);
    m_lengths.grow(numberOfClosureFunctions);
    m_captureCounts.grow(numberOfClosureFunctions);
    m_cellNumbers.grow(numberOfClosureFunctions);
}

// ---- Records

static void uniteEqualFunctionRecords(Vector<DistinctFunctionRecord>& records)
{
    AOT::sortInLine(records, { }, &DistinctFunctionRecord::bits);
    size_t count = 0;
    for (const DistinctFunctionRecord& next : records) {
        if (count && records[count - 1].bits == next.bits)
            records[count - 1].count += next.count;
        else
            records[count++] = next;
    }
    records.shrink(count);
}

void FunctionTablesByRange::numberRecords()
{
    Vector<DistinctFunctionRecord> distinct;
    for (const RowsOfFunctionRange& range : m_ranges)
        distinct.appendVector(range.records);
    uniteEqualFunctionRecords(distinct);
    // The most common ones get the numbers that fit a byte. Records that are equally common stay in the order of their bits.
    AOT::stableSortInLine(distinct, std::ranges::greater { }, &DistinctFunctionRecord::count);
    for (uint32_t number = 0; number < distinct.size(); ++number)
        distinct[number].number = number;
    m_records = Vector<FunctionRecordData>(std::min<size_t>(distinct.size(), functionRecordIsListedByFunction), [&](size_t number) {
        return std::bit_cast<FunctionRecordData>(distinct[number].bits);
    });
    AOT::sortInLine(distinct, { }, &DistinctFunctionRecord::bits);

    size_t numberListedByFunction = 0;
    for (RowsOfFunctionRange& range : m_ranges) {
        range.firstRecordListedByFunction = numberListedByFunction;
        for (DistinctFunctionRecord& record : range.records) {
            record.number = std::ranges::lower_bound(distinct, record.bits, { }, &DistinctFunctionRecord::bits)->number;
            if (record.number >= functionRecordIsListedByFunction)
                numberListedByFunction += record.count;
        }
    }
    m_recordsByFunction.grow(numberListedByFunction);
}

// ---- Names

// A function that is not listed has the number 0, whichever string that is.
// Two words for every 32 functions: which of them have a name in the list, and how many names lie in front of theirs.
void FunctionTablesByRange::collectName(uint32_t index, std::optional<BuiltinID> implemented, Vector<Number24>& names)
{
    std::span<uint32_t> group = m_groupsOfNames.mutableSpan().subspan(index / functionsInGroupOfNames * 2, 2);
    if (!(index % functionsInGroupOfNames))
        group[1] = static_cast<uint32_t>(names.size());
    if (!m_numbersOfStringsFit || !isShownByName(m_encoder.inputs, FunctionID(index)))
        return;
    // Whoever finds no row takes the name for empty. Two of five functions of a large program are written without a name.
    StringID name = m_encoder.obfuscates() ? StringID() : nameShownForFunction(m_encoder.program, FunctionID(index), implemented);
    if (!name || name == m_encoder.program.strings.emptyString())
        return;
    group[0] |= 1u << index % functionsInGroupOfNames;
    names.append(Number24(m_encoder.stringNumber(name)));
}

// ---- The functions that closures are made of

static CountsOfClosureFunction countsOfClosureFunction(const TableEncoder& encoder, CodeUnit entry)
{
    if (!Typed::isBoundRole(entry.role)) {
        const Typed::Function& function = encoder.program.at(entry.function);
        Typed::ClosureCaptureCounts counts = Typed::closureCaptureCountsOf(encoder.program, encoder.plan.conventions(), entry.function);
        return { function.length, counts.references, counts.captures };
    }
    const Typed::BoundFunctionPlan* bound = encoder.plan.conventions().boundFunction(entry.function, entry.numberOfBoundArguments);
    RELEASE_ASSERT(bound);
    Typed::ClosureCaptureCounts counts = Typed::closureCaptureCountsOf(encoder.program, *bound);
    return { bound->length, counts.references, counts.captures };
}

void FunctionTablesByRange::collectClosureFunction(uint32_t index, CodeUnit unit, FunctionKindData kind, Vector<FunctionWithLargeCountsData>& functionsWithLargeCounts)
{
    if (!Typed::isGenericEntry(unit.role))
        m_encoder.fail("ClosureFunctionIsNoGenericEntry"_s, unit.function);
    CountsOfClosureFunction found = countsOfClosureFunction(m_encoder, unit);
    uint32_t length = found.length;
    if (length > UINT16_MAX) {
        m_encoder.build.reportLimitExceeded(LimitID::MaxReservedStackArguments, length, Subject { .function = unit.function });
        length = 0;
    }
    CaptureCounts counts { 0, 0 };
    if (found.captures <= UINT8_MAX && found.references <= found.captures)
        counts = { static_cast<uint8_t>(found.references), static_cast<uint8_t>(found.captures) };
    else
        m_encoder.fail("CapturesOfClosureDoNotFitTheirCounts"_s, unit.function);

    uint32_t number = index - m_firstClosureFunction;
    m_kinds[number] = kind;
    m_lengths[number] = length < countsAreListed ? static_cast<uint8_t>(length) : countsAreListed;
    m_captureCounts[number] = counts.encoded();
    if (m_lengths[number] == countsAreListed || m_captureCounts[number] == countsAreListed)
        functionsWithLargeCounts.append({ Number24(index), counts, { static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8) } });
    // The word of a closure names the entry for calls, so that entry stands for the closure in the table.
    m_cellNumbers[number] = Number24(unit.role == Typed::CodeRole::GenericCallEntry ? m_encoder.numbers.closureCellNumber(unit.function) : 0);
}

void FunctionTablesByRange::collectNamesOfCaptures(uint32_t index, CodeUnit unit, Vector<NamedCaptureData>& named) const
{
    const Typed::CallingConventions& conventions = m_encoder.plan.conventions();
    if (unit != conventions.closureEntryOf(unit.function))
        return;
    size_t first = named.size();
    forEachCaptureShownByName(m_encoder.inputs, unit.function, [&](unsigned capture, StringID name) {
        // collectClosureFunction() stops the build where a closure has more words than a byte counts.
        unsigned slot = Typed::closureCaptureSlotOf(m_encoder.program, conventions, unit.function, capture);
        if (slot <= UINT8_MAX)
            named.append({ Number24(m_encoder.numbers.functions().closureNumberOf(FunctionID(index))), static_cast<uint8_t>(slot), m_encoder.stringNumber(name) });
    });
    AOT::sort(named.mutableSpan().subspan(first), { }, &NamedCaptureData::slot);
}

void FunctionTablesByRange::collectConstructEntrypoint(uint32_t index, CodeUnit unit, FunctionKindData kind, Vector<ConstructEntrypointData>& constructEntrypoints) const
{
    if (unit.role != Typed::CodeRole::GenericCallEntry)
        return;
    const FunctionNumbers& functions = m_encoder.numbers.functions();
    const Typed::CallingConventions& conventions = m_encoder.plan.conventions();
    uint32_t number = functions.closureNumberOf(FunctionID(index));
    CodeUnit constructEntry = conventions.genericEntry(unit).constructEntry;
    if (!constructEntry) {
        // Only here: the stub looks up a function whose plan has a construct entry, and would read the mark as the number of an entry.
        if (m_encoder.inputs.bodyFacts.functionsThatOnlyReturn.get(unit.function.index()) && kind.isConstructiblePlainFunction())
            constructEntrypoints.append({ number, ConstructEntrypointData::onlyReturns });
        return;
    }
    // The word of the closure says that a construction enters elsewhere, and the stub stops the program where it finds no row.
    if (!m_encoder.hasCode(constructEntry)) {
        m_encoder.fail("ConstructEntryWithoutCode"_s, unit.function);
        return;
    }
    FunctionID entry = functions.idOf(conventions.indexOf(constructEntry));
    if (entry < functions.firstClosureFunction() || entry >= functions.endOfClosureFunctions()) {
        m_encoder.fail("ConstructEntryWithoutClosureNumber"_s, unit.function);
        return;
    }
    uint32_t flag = conventions.genericEntry(constructEntry).allocatesThis ? ConstructEntrypointData::allocatesThis : 0;
    constructEntrypoints.append({ number, functions.closureNumberOf(entry) | flag });
}

// ---- The two loops

void FunctionTablesByRange::collectFunction(uint32_t index, RowsOfFunctionRange& rows)
{
    const Typed::Program& program = m_encoder.program;
    const Typed::CallingConventions& conventions = m_encoder.plan.conventions();
    CodeUnit unit = unitOfImageFunction(m_encoder, index);
    // The unwinder goes by the record, and only the plan knows that a body never suspends and so rejects nothing.
    FunctionTraceRecord trace = Typed::functionTraceOf(program, conventions, unit);
    FunctionRecordData record { m_encoder.plan.functionKind(program, closureEntryThatDescribes(conventions, unit)), trace };
    uint32_t bits = std::bit_cast<uint32_t>(record);
    m_bitsOfRecords[index] = bits;
    if (rows.records.isEmpty() || rows.records.last().bits != bits)
        rows.records.append({ bits, 0, 0 });
    ++rows.records.last().count;

    std::optional<BuiltinID> builtin = trace.implements();
    // The run time looks for a row wherever the bit is set.
    if (builtin.has_value() != !!trace.implementsBuiltin)
        m_encoder.fail("TraceOfFunctionDisagreesAboutItsBuiltin"_s, unit.function);
    if (builtin)
        rows.implementedBuiltins.append({ index, builtin->index() });

    if (index < m_firstDerived) {
        collectName(index, builtin, rows.names);
        return;
    }
    // A function of the program is its own source. The table starts behind the last of them.
    m_sources[index - m_firstDerived] = Number24(unit.function.index());
    if (index < m_firstClosureFunction || index >= m_endOfClosureFunctions)
        return;
    collectClosureFunction(index, unit, record.kind, rows.functionsWithLargeCounts);
    collectNamesOfCaptures(index, unit, rows.namedCaptures);
    collectConstructEntrypoint(index, unit, record.kind, rows.constructEntrypoints);
}

void FunctionTablesByRange::collect(size_t first, size_t end)
{
    // The lists grow here and not in m_ranges, where those of several threads would share a line of the cache.
    RowsOfFunctionRange rows;
    for (size_t index = first; index < end; ++index)
        collectFunction(static_cast<uint32_t>(index), rows);
    uniteEqualFunctionRecords(rows.records);
    m_ranges[first / indicesInRangeOfLoop] = WTF::move(rows);
}

void FunctionTablesByRange::place()
{
    numberRecords();
    m_names.place(m_ranges.span(), &RowsOfFunctionRange::names);
    m_implementedBuiltins.place(m_ranges.span(), &RowsOfFunctionRange::implementedBuiltins);
    m_functionsWithLargeCounts.place(m_ranges.span(), &RowsOfFunctionRange::functionsWithLargeCounts);
    m_namedCaptures.place(m_ranges.span(), &RowsOfFunctionRange::namedCaptures);
    m_constructEntrypoints.place(m_ranges.span(), &RowsOfFunctionRange::constructEntrypoints);
}

void FunctionTablesByRange::write(size_t first, size_t end)
{
    size_t numberOfRange = first / indicesInRangeOfLoop;
    const RowsOfFunctionRange& range = m_ranges[numberOfRange];

    size_t nextListedByFunction = range.firstRecordListedByFunction;
    for (size_t index = first; index < end; ++index) {
        uint32_t bits = m_bitsOfRecords[index];
        uint32_t number = std::ranges::lower_bound(range.records, bits, { }, &DistinctFunctionRecord::bits)->number;
        if (number < functionRecordIsListedByFunction) {
            m_recordNumbers[index] = static_cast<uint8_t>(number);
            continue;
        }
        m_recordNumbers[index] = functionRecordIsListedByFunction;
        m_recordsByFunction[nextListedByFunction++] = { static_cast<uint32_t>(index), std::bit_cast<FunctionRecordData>(bits) };
    }

    uint32_t namesInFrontOfRange = static_cast<uint32_t>(m_names.firstOfRange[numberOfRange]);
    for (size_t index = first; index < std::min<size_t>(end, m_firstDerived); index += functionsInGroupOfNames)
        m_groupsOfNames[index / functionsInGroupOfNames * 2 + 1] += namesInFrontOfRange;

    m_names.write(numberOfRange, range.names);
    m_implementedBuiltins.write(numberOfRange, range.implementedBuiltins);
    m_functionsWithLargeCounts.write(numberOfRange, range.functionsWithLargeCounts);
    m_namedCaptures.write(numberOfRange, range.namedCaptures);
    m_constructEntrypoints.write(numberOfRange, range.constructEntrypoints);
}

void FunctionTablesByRange::forEachTable(const auto& functor) const
{
    functor(ProgramTableIndex::functionNames, m_names.rows);
    functor(ProgramTableIndex::functionNameGroups, m_groupsOfNames);
    // Every function behind those of the program is a form of one of them and has its name, so none is looked for here.
    functor(ProgramTableIndex::functionNameRuns, Vector<FunctionNameRunData> { });
    functor(ProgramTableIndex::functionRecordNumbers, m_recordNumbers);
    functor(ProgramTableIndex::functionRecords, m_records);
    functor(ProgramTableIndex::functionRecordsByFunction, m_recordsByFunction);
    functor(ProgramTableIndex::functionSources, m_sources);
    functor(ProgramTableIndex::implementedBuiltins, m_implementedBuiltins.rows);
    functor(ProgramTableIndex::closureFunctionKinds, m_kinds);
    functor(ProgramTableIndex::functionLengths, m_lengths);
    functor(ProgramTableIndex::closureCaptureCounts, m_captureCounts);
    functor(ProgramTableIndex::functionsWithLargeCounts, m_functionsWithLargeCounts.rows);
    functor(ProgramTableIndex::closureCellNumbers, m_cellNumbers);
    functor(ProgramTableIndex::namedCaptures, m_bytesOfNamedCaptures);
    functor(ProgramTableIndex::constructEntrypoints, m_constructEntrypoints.rows);
}

void FunctionTablesByRange::appendTo(TablePart& part)
{
    m_bytesOfNamedCaptures = encodeNamedCaptures(m_namedCaptures.rows.span());
    uint64_t room = 0;
    forEachTable([&](ProgramTableIndex, const auto& rows) {
        room += rows.sizeInBytes() + sizeof(uint64_t);
    });
    part.reserveRoomFor(room);
    forEachTable([&](ProgramTableIndex table, const auto& rows) {
        part.setTable(table, rows);
    });
}

void encodeFunctionProgramTables(TableEncoder& encoder)
{
    const ParallelLoops& loops = encoder.build.parallelLoops();
    StepTimes times(encoder.build, "image: function tables"_s);

    FunctionTablesByRange tables(encoder);
    forEachRangeOfIndicesInParallel(loops, tables.numberOfFunctions(), [&](size_t first, size_t end) {
        tables.collect(first, end);
    });
    times.didFinish("ask about each function"_s);
    tables.place();
    times.didFinish("number the records, place the rows"_s);
    forEachRangeOfIndicesInParallel(loops, tables.numberOfFunctions(), [&](size_t first, size_t end) {
        tables.write(first, end);
    });
    times.didFinish("write what hangs on other ranges"_s);
    tables.appendTo(encoder.part);
    times.didFinish("append the tables"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
