/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionRemap;
class TypeRemap;

uint64_t stampOfRecord(const Function&);
uint64_t stampOfEntry(const Function&);

class IndexedValue {
public:
    static constexpr unsigned bitsOfOpcode = 10;
    static constexpr unsigned bitsOfBlock = 22;
    static_assert(numberOfEnumerators<Opcode> <= 1u << bitsOfOpcode);

    IndexedValue() = default;
    IndexedValue(ValueID value, Opcode opcode, BlockID block)
        : m_word(static_cast<uint64_t>(value.index()) << 32 | static_cast<uint64_t>(block.index()) << bitsOfOpcode | static_cast<uint64_t>(opcode))
    {
    }

    ValueID value() const { return ValueID(static_cast<uint32_t>(m_word >> 32)); }
    BlockID block() const { return BlockID(static_cast<uint32_t>(m_word) >> bitsOfOpcode); }
    Opcode opcode() const { return static_cast<Opcode>(m_word & ((1u << bitsOfOpcode) - 1)); }

    friend bool operator==(const IndexedValue&, const IndexedValue&) = default;

private:
    uint64_t m_word { 0 };
};

enum class NameKind : uint8_t { Key, Function, Layout, Variable };

class IndexedName {
public:
    static constexpr unsigned bitsOfPosition = 30;

    IndexedName() = default;
    IndexedName(uint32_t position, NameKind kind, uint32_t index)
        : m_word(static_cast<uint64_t>(index) << 32 | static_cast<uint64_t>(position) << 2 | static_cast<uint64_t>(kind))
    {
    }

    uint32_t position() const { return static_cast<uint32_t>(m_word) >> 2; }
    NameKind kind() const { return static_cast<NameKind>(m_word & 3); }
    uint32_t index() const { return static_cast<uint32_t>(m_word >> 32); }

    friend bool operator==(const IndexedName&, const IndexedName&) = default;

private:
    uint64_t m_word { 0 };
};

template<typename Functor>
void forEachNameOf(const Value& value, const Functor& functor)
{
    if (Value::opcodesWithKey.contains(value.opcode)) {
        if (PropertyKeyID key = value.key())
            functor(NameKind::Key, key.index());
    }
    if (Value::opcodesWithFunction.contains(value.opcode)) {
        if (FunctionID function = value.function())
            functor(NameKind::Function, function.index());
    }
    if (Value::opcodesWithLayout.contains(value.opcode)) {
        if (LayoutID layout = value.layout())
            functor(NameKind::Layout, layout.index());
    }
    if (Value::opcodesWithVariable.contains(value.opcode)) {
        if (VariableID variable = value.variable())
            functor(NameKind::Variable, variable.index());
    }
}

enum class BodyTrait : uint8_t {
    SeveralBlocks = 1 << 0,
    Handlers = 1 << 1,
    BlockParameters = 1 << 2,
    SeveralTargets = 1 << 3,
    NeverValues = 1 << 4,
    ParametersOfRuntimeFunction = 1 << 5,
    UnfinishedThis = 1 << 6,
};

struct BodyContents {
    friend bool operator==(const BodyContents&, const BodyContents&) = default;

    OpcodeSet opcodes;
    TerminatorKindSet terminators;
    OptionSet<BodyTrait> traits;
};

BodyContents contentsOf(const Function&);
bool hasNeverResult(const Value&);

// What contentsOf() answers, kept by counting: only what the change log names is read again.
class CountedContents {
    WTF_MAKE_NONCOPYABLE(CountedContents);
public:
    CountedContents() = default;

    // A short body is read anew sooner than its counts are kept.
    static bool paysFor(const Function& function) { return function.values.size() >= fewestValuesOfCountedBody; }

    const BodyContents& bringUpToDate(const Function&, const ChangeLog&);

private:
    AOT_CLASS_THRESHOLD(size_t, fewestValuesOfCountedBody, 64);
    // The room of a list that has grown stays behind until the body is counted anew.
    AOT_CLASS_THRESHOLD(size_t, mostPlacesInListsForEachValue, 4);
    static constexpr uint16_t hasNeverResultBit = 1 << 15;
    static constexpr uint16_t isNotListed = std::numeric_limits<uint16_t>::max();
    static_assert(numberOfEnumerators<Opcode> < hasNeverResultBit);

    struct CountedBlock {
        uint32_t firstValue { 0 };
        uint32_t numberOfValues { 0 };
        uint32_t capacity { 0 };
        TerminatorKind terminator { };
        OptionSet<BodyTrait> traits;
        bool isCounted { false };
        bool isPending { false };
    };

    template<size_t size> struct Counts {
        void add(unsigned index)
        {
            if (!counts[index]++)
                words[index / 64] |= 1ull << (index % 64);
        }
        void remove(unsigned index)
        {
            if (!--counts[index])
                words[index / 64] &= ~(1ull << (index % 64));
        }

        std::array<uint32_t, size> counts { };
        std::array<uint64_t, (size + 63) / 64> words { };
    };

    void countAnew(const Function&);
    void countWhatChanged(const Function&, const ChangeLog&);
    void addWord(uint16_t);
    void removeWord(uint16_t);
    void addValuesOf(const Function&, uint32_t blockIndex);
    void removeValuesOf(CountedBlock&);
    void countEndOf(const Function&, uint32_t blockIndex);

    bool m_hasReadBody { false };
    bool m_isCounted { false };
    ChangeLog::Cursor m_endOfLog;
    // The parameters and the listed values of each block, as they were counted.
    Vector<ValueID> m_listedValues;
    Vector<CountedBlock> m_blocks;
    // The opcode that a listed value was counted with, and whether its result was of the type never.
    Vector<uint16_t> m_wordOfValue;
    Counts<numberOfEnumerators<Opcode>> m_opcodes;
    Counts<numberOfEnumerators<TerminatorKind>> m_terminators;
    Counts<8> m_traits;
    uint32_t m_numberOfNeverValues { 0 };
    BodyContents m_contents;
};

struct RowOfBody {
    OpcodeSet opcodes;
    TerminatorKindSet terminators;
    std::span<const IndexedValue> listedValues;
    bool listsAllValues { false };
    bool holdsExpectedTypes { false };
};

struct NamesInBody {
    std::span<const TypeID> types;
    std::span<const LayoutID> layouts;
    std::span<const LayoutSetID> layoutSets;
    std::span<const VariableID> variables;
};

struct RecordFacts {
    RecordFacts() = default;
    explicit RecordFacts(const Function& function)
        : flags(function.flags)
        , capabilities(function.capabilities)
        , role(function.role)
        , kind(function.kind)
        , creatorKind(function.creator.kind())
        , numberOfResults(static_cast<uint8_t>(std::min<size_t>(function.results.size(), 127)))
        , hasBody(function.hasBody())
    {
    }

    bool has(FunctionFlags flag) const { return flags.contains(flag); }
    bool has(FunctionCapabilities capability) const { return capabilities.contains(capability); }
    bool hasOrdinaryFrame() const { return has(FunctionCapabilities::HasOrdinaryFrame); }
    bool hasOnlyDirectCallers() const { return has(FunctionCapabilities::HasOnlyDirectCallers); }
    bool mayBeCalledByHost() const { return !has(FunctionCapabilities::IsNeverCalledByHost); }

    friend bool operator==(const RecordFacts&, const RecordFacts&) = default;

    OptionSet<FunctionFlags> flags;
    OptionSet<FunctionCapabilities> capabilities;
    FunctionRole role { FunctionRole::Source };
    FunctionKind kind { FunctionKind::Plain };
    CreatorRecord::Kind creatorKind { CreatorRecord::Kind::None };
    uint8_t numberOfResults : 7 { 0 };
    uint8_t hasBody : 1 { false };
};
static_assert(sizeof(RecordFacts) <= 8);

enum class BodyIDs : bool { MayBeProvisional, Final };

class BodyIndex {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodyIndex);
    WTF_MAKE_NONCOPYABLE(BodyIndex);
public:
    BodyIndex() = default;
    explicit BodyIndex(const ParallelLoops& loops)
        : m_loops(&loops)
    {
    }

    struct RecordStamps {
        uint64_t record { 0 };
        uint64_t entry { 0 };

        friend bool operator==(const RecordStamps&, const RecordStamps&) = default;
    };

    struct HotRow {
        TerminatorKindSet terminators() const { return std::bit_cast<TerminatorKindSet>(static_cast<uint64_t>(terminatorBits)); }

        OpcodeSet opcodes;
        uint16_t terminatorBits { 0 };
        bool isComplete { false };
        bool listsAllValues { false };
        bool holdsExpectedTypes { false };
        bool listsParameters { false };
        OptionSet<BodyTrait> traits;
    };
    static_assert(sizeof(TerminatorKindSet) == sizeof(uint64_t) && numberOfEnumerators<TerminatorKind> <= 16);
    static_assert(sizeof(HotRow) <= 64);

    struct DenseRow {
        uint32_t numberOfValues { 0 };
        RecordFacts facts;

        friend bool operator==(const DenseRow&, const DenseRow&) = default;
    };

    struct ColdRow {
        void makeNames(const Function&);
        void setNames(std::array<Vector<uint32_t, 64>, 4>&);
        NamesInBody namesInBody() const;

        MallocSpan<IndexedValue> listedValues;
        MallocSpan<TerminatorKind> terminatorKinds;
        MallocSpan<IndexedName> listedNames;
        MallocSpan<uint32_t> names;
        std::array<uint32_t, 3> endOfNames { };
        uint8_t stateOfNames { 0 };
    };

    struct Row {
        explicit Row(const Function&, BodyIDs = BodyIDs::MayBeProvisional);

        void applyRemaps(const TypeRemap&, const FunctionRemap&, const Function* functionWithChangedRecord);

        HotRow hot;
        ColdRow cold;
        DenseRow dense;
        std::optional<RecordStamps> recordStamps;
    };

    void growTo(size_t numberOfFunctions) { growTo(numberOfFunctions, State::Unknown); }
    void didAddFunctionsUpTo(size_t numberOfFunctions) { growTo(numberOfFunctions, State::GivenOut); }
    void forgetAll() { m_states.fill(static_cast<uint8_t>(State::Unknown)); }
    void didGiveOutAll() { m_states.fill(static_cast<uint8_t>(State::GivenOut)); }
    void didEndAllEdits();

    void didGiveOut(FunctionID id)
    {
        if (id.index() < m_states.size())
            stateOf(id).store(static_cast<uint8_t>(State::GivenOut), std::memory_order_relaxed);
    }
    bool write(FunctionID, const Function&, BodyIDs = BodyIDs::MayBeProvisional);
    bool write(FunctionID, Row&&);
    void didEndEdits(FunctionID);

    const HotRow* hotRowOf(const Program& program, FunctionID id)
    {
        if (!ensureRow(program, id))
            return nullptr;
        const HotRow& row = m_hotRows[id.index()];
        return row.isComplete ? &row : nullptr;
    }

    std::optional<BodyContents> contentsOf(FunctionID id)
    {
        if (!isKnown(id) || !m_hotRows[id.index()].isComplete)
            return std::nullopt;
        const HotRow& row = m_hotRows[id.index()];
        BodyContents contents { row.opcodes, row.terminators(), row.traits };
        if (row.listsParameters)
            contents.opcodes.add(Opcode::Parameter);
        return contents;
    }

    std::span<const IndexedValue> listedValuesOf(FunctionID id) const { return m_coldRows[id.index()].listedValues.span(); }
    std::span<const TerminatorKind> terminatorKindsOf(FunctionID id) const { return m_coldRows[id.index()].terminatorKinds.span(); }

    std::optional<RowOfBody> rowOf(const Program& program, FunctionID id)
    {
        const HotRow* row = hotRowOf(program, id);
        if (!row)
            return std::nullopt;
        return RowOfBody { row->opcodes, row->terminators(), listedValuesOf(id), row->listsAllValues, row->holdsExpectedTypes };
    }

    size_t numberOfValuesIn(const Program& program, FunctionID id)
    {
        if (isKnown(id))
            return m_denseRows[id.index()].numberOfValues;
        return program.functions[id.index()].values.size();
    }
    RecordFacts recordFactsOf(const Program& program, FunctionID id)
    {
        if (isKnown(id))
            return m_denseRows[id.index()].facts;
        return RecordFacts(program.functions[id.index()]);
    }

    RecordStamps recordStampsOf(const Program& program, FunctionID id)
    {
        if (!isKnown(id))
            return computeRecordStamps(program.functions[id.index()]);
        AtomicRef<uint8_t> hasStamps(m_hasRecordStamps[id.index()]);
        RecordStamps& kept = m_recordStamps[id.index()];
        if (hasStamps.load(std::memory_order_acquire))
            return RecordStamps { AtomicRef(kept.record).load(std::memory_order_relaxed), AtomicRef(kept.entry).load(std::memory_order_relaxed) };
        RecordStamps stamps = computeRecordStamps(program.functions[id.index()]);
        AtomicRef(kept.record).store(stamps.record, std::memory_order_relaxed);
        AtomicRef(kept.entry).store(stamps.entry, std::memory_order_relaxed);
        hasStamps.store(1, std::memory_order_release);
        return stamps;
    }

    std::optional<NamesInBody> namesIn(const Program& program, FunctionID id)
    {
        if (!hotRowOf(program, id))
            return std::nullopt;
        ColdRow& row = m_coldRows[id.index()];
        AtomicRef<uint8_t> state(row.stateOfNames);
        uint8_t seen = state.load(std::memory_order_acquire);
        if (seen != static_cast<uint8_t>(State::Known)) {
            if (seen != static_cast<uint8_t>(State::Unknown) || !state.compare_exchange_strong(seen, static_cast<uint8_t>(State::BeingMade), std::memory_order_relaxed))
                return std::nullopt;
            row.makeNames(program.functions[id.index()]);
            state.store(static_cast<uint8_t>(State::Known), std::memory_order_release);
        }
        return row.namesInBody();
    }

    std::optional<std::span<const IndexedName>> listedNamesIn(const Program& program, FunctionID id)
    {
        if (!namesIn(program, id))
            return std::nullopt;
        return m_coldRows[id.index()].listedNames.span();
    }

    void check(const Program&, const ParallelLoops&) const;

private:
    enum class State : uint8_t { Unknown, BeingMade, Known, GivenOut, Written };
    static_assert(!static_cast<uint8_t>(State::Unknown));

    bool isKnown(FunctionID id) { return id.index() < m_states.size() && stateOf(id).load(std::memory_order_acquire) == static_cast<uint8_t>(State::Known); }

    bool ensureRow(const Program& program, FunctionID id)
    {
        if (id.index() >= m_states.size())
            return false;
        AtomicRef<uint8_t> state = stateOf(id);
        uint8_t seen = state.load(std::memory_order_acquire);
        if (seen == static_cast<uint8_t>(State::Known)) [[likely]]
            return true;
        if (seen != static_cast<uint8_t>(State::Unknown) || !state.compare_exchange_strong(seen, static_cast<uint8_t>(State::BeingMade), std::memory_order_relaxed))
            return false;
        store(id, Row(program.functions[id.index()]));
        state.store(static_cast<uint8_t>(State::Known), std::memory_order_release);
        return true;
    }

    static RecordStamps computeRecordStamps(const Function& function) { return { stampOfRecord(function), stampOfEntry(function) }; }
    void store(FunctionID, Row&&);
    void growTo(size_t numberOfFunctions, State);
    AtomicRef<uint8_t> stateOf(FunctionID id) { return AtomicRef(m_states[id.index()]); }

    const ParallelLoops* m_loops { nullptr };
    Vector<HotRow> m_hotRows;
    Vector<ColdRow> m_coldRows;
    Vector<DenseRow> m_denseRows;
    Vector<RecordStamps> m_recordStamps;
    Vector<uint8_t> m_hasRecordStamps;
    Vector<uint8_t> m_states;
};

inline const BodyIndex::HotRow* hotRowOf(const Program& program, FunctionID id)
{
    BodyIndex* index = program.bodyIndex();
    return index ? index->hotRowOf(program, id) : nullptr;
}

template<typename Functor>
void forEachListedValue(const Program& program, FunctionID id, const OpcodeSet& wanted, const Functor& functor)
{
    const Function& function = program.functions[id.index()];
    if (const BodyIndex::HotRow* row = hotRowOf(program, id)) {
        if (!row->opcodes.intersects(wanted))
            return;
        BlockID block;
        uint32_t indexInBlock = 0;
        for (IndexedValue listed : program.bodyIndex()->listedValuesOf(id)) {
            if (listed.block() != block) {
                block = listed.block();
                indexInBlock = 0;
            }
            if (wanted.contains(listed.opcode()))
                functor(block, indexInBlock, listed.value(), function.at(listed.value()));
            ++indexInBlock;
        }
        return;
    }
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        uint32_t indexInBlock = 0;
        for (ValueID value : function.blocks[blockIndex].values) {
            const Value& definition = function.at(value);
            if (wanted.contains(definition.opcode))
                functor(BlockID(blockIndex), indexInBlock, value, definition);
            ++indexInBlock;
        }
    }
}

template<typename Functor>
void forEachListedName(const Program& program, FunctionID id, const OpcodeSet& wanted, const Functor& functor)
{
    if (const BodyIndex::HotRow* row = hotRowOf(program, id)) {
        if (!row->opcodes.intersects(wanted))
            return;
        if (std::optional<std::span<const IndexedName>> names = program.bodyIndex()->listedNamesIn(program, id)) {
            std::span<const IndexedValue> listedValues = program.bodyIndex()->listedValuesOf(id);
            for (IndexedName name : *names) {
                IndexedValue listed = listedValues[name.position()];
                if (wanted.contains(listed.opcode()))
                    functor(listed.block(), listed.value(), listed.opcode(), name.kind(), name.index());
            }
            return;
        }
    }
    const Function& function = program.functions[id.index()];
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (ValueID value : function.blocks[blockIndex].values) {
            const Value& definition = function.at(value);
            if (!wanted.contains(definition.opcode))
                continue;
            forEachNameOf(definition, [&](NameKind kind, uint32_t nameIndex) {
                functor(BlockID(blockIndex), value, definition.opcode, kind, nameIndex);
            });
        }
    }
}

template<typename Filter, typename Functor>
void forEachListedValueNaming(const Program& program, FunctionID id, const OpcodeSet& wanted, const Filter& filter, const Functor& functor)
{
    const Function& function = program.functions[id.index()];
    ValueID last;
    forEachListedName(program, id, wanted, [&](BlockID block, ValueID value, Opcode, NameKind kind, uint32_t nameIndex) {
        if (value == last || !filter(kind, nameIndex))
            return;
        last = value;
        functor(block, value, function.at(value));
    });
}

template<typename Functor>
void forEachDirectCall(const Program& program, FunctionID id, const Functor& functor)
{
    forEachListedName(program, id, { Opcode::CallDirect }, [&](BlockID block, ValueID value, Opcode, NameKind kind, uint32_t nameIndex) {
        if (kind == NameKind::Function)
            functor(block, value, FunctionID(nameIndex));
    });
}

template<typename Functor>
void forEachValueListedOrNot(const Program& program, FunctionID id, const OpcodeSet& wanted, const Functor& functor)
{
    ASSERT(!wanted.contains(Opcode::Parameter));
    const Function& function = program.functions[id.index()];
    if (const BodyIndex::HotRow* row = hotRowOf(program, id); row && row->listsAllValues) {
        if (!row->opcodes.intersects(wanted))
            return;
        Vector<ValueID, 16> found;
        for (IndexedValue listed : program.bodyIndex()->listedValuesOf(id)) {
            if (wanted.contains(listed.opcode()))
                found.append(listed.value());
        }
        AOT::sort(found);
        for (ValueID value : found)
            functor(value, function.at(value));
        return;
    }
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        const Value& value = function.values[valueIndex];
        if (wanted.contains(value.opcode))
            functor(ValueID(valueIndex), value);
    }
}

template<typename Predicate>
bool anyListedValue(const Program& program, FunctionID id, const OpcodeSet& wanted, const Predicate& predicate)
{
    const Function& function = program.functions[id.index()];
    if (const BodyIndex::HotRow* row = hotRowOf(program, id)) {
        if (!row->opcodes.intersects(wanted))
            return false;
        for (IndexedValue listed : program.bodyIndex()->listedValuesOf(id)) {
            if (wanted.contains(listed.opcode()) && predicate(listed.value(), function.at(listed.value())))
                return true;
        }
        return false;
    }
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            const Value& definition = function.at(value);
            if (wanted.contains(definition.opcode) && predicate(value, definition))
                return true;
        }
    }
    return false;
}

inline bool listsAllValues(const Program& program, FunctionID id)
{
    const BodyIndex::HotRow* row = hotRowOf(program, id);
    return row && row->listsAllValues;
}

template<typename Predicate>
bool anyValueListedOrNot(const Program& program, FunctionID id, const OpcodeSet& wanted, const Predicate& predicate)
{
    ASSERT(!wanted.contains(Opcode::Parameter));
    if (listsAllValues(program, id))
        return anyListedValue(program, id, wanted, predicate);
    const Function& function = program.functions[id.index()];
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        const Value& value = function.values[valueIndex];
        if (wanted.contains(value.opcode) && predicate(ValueID(valueIndex), value))
            return true;
    }
    return false;
}

inline bool mayListValueWith(const Program& program, FunctionID id, const OpcodeSet& wanted)
{
    const BodyIndex::HotRow* row = hotRowOf(program, id);
    return !row || row->opcodes.intersects(wanted);
}

inline bool mayHoldValueWith(const Program& program, FunctionID id, const OpcodeSet& wanted)
{
    ASSERT(!wanted.contains(Opcode::Parameter));
    const BodyIndex::HotRow* row = hotRowOf(program, id);
    return !row || !row->listsAllValues || row->opcodes.intersects(wanted);
}

inline bool mayHoldExpectedTypes(const Program& program, FunctionID id)
{
    const BodyIndex::HotRow* row = hotRowOf(program, id);
    return !row || row->holdsExpectedTypes;
}

inline bool mayEndBlockWith(const Program& program, FunctionID id, const TerminatorKindSet& wanted)
{
    const BodyIndex::HotRow* row = hotRowOf(program, id);
    return !row || row->terminators().intersects(wanted);
}

template<typename Functor>
void forEachBlockEndingWith(const Program& program, FunctionID id, const TerminatorKindSet& wanted, const Functor& functor)
{
    if (const BodyIndex::HotRow* row = hotRowOf(program, id)) {
        if (!row->terminators().intersects(wanted))
            return;
        std::span<const TerminatorKind> kinds = program.bodyIndex()->terminatorKindsOf(id);
        for (uint32_t blockIndex = 0; blockIndex < kinds.size(); ++blockIndex) {
            if (wanted.contains(kinds[blockIndex]))
                functor(BlockID(blockIndex));
        }
        return;
    }
    const Function& function = program.functions[id.index()];
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        if (wanted.contains(function.blocks[blockIndex].terminator.kind))
            functor(BlockID(blockIndex));
    }
}

inline size_t numberOfValuesIn(const Program& program, FunctionID id)
{
    if (BodyIndex* index = program.bodyIndex())
        return index->numberOfValuesIn(program, id);
    return program.functions[id.index()].values.size();
}

inline uint64_t stampOfRecord(const Program& program, FunctionID id)
{
    if (BodyIndex* index = program.bodyIndex())
        return index->recordStampsOf(program, id).record;
    return stampOfRecord(program.functions[id.index()]);
}

inline uint64_t stampOfEntry(const Program& program, FunctionID id)
{
    if (BodyIndex* index = program.bodyIndex())
        return index->recordStampsOf(program, id).entry;
    return stampOfEntry(program.functions[id.index()]);
}

inline RecordFacts recordFactsOf(const Program& program, FunctionID id)
{
    if (BodyIndex* index = program.bodyIndex())
        return index->recordFactsOf(program, id);
    return RecordFacts(program.functions[id.index()]);
}

inline bool hasBody(const Program& program, FunctionID id)
{
    return recordFactsOf(program, id).hasBody;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
