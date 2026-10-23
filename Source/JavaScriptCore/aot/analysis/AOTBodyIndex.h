/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTThreads.h"
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
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionRemap;
class TypeRemap;

uint64_t stampOfRecord(const Function&);
uint64_t stampOfEntry(const Function&);

// A value that a block lists: its number, its opcode and the block, in one word.
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

// What a listed value answers to key(), function(), layout() or variable(), where the answer is not null.
class IndexedName {
public:
    static constexpr unsigned bitsOfPosition = 30;

    IndexedName() = default;
    IndexedName(uint32_t position, NameKind kind, uint32_t index)
        : m_word(static_cast<uint64_t>(index) << 32 | static_cast<uint64_t>(position) << 2 | static_cast<uint64_t>(kind))
    {
    }

    // Among the listed values of the function.
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

// What a step that looks through the whole program asks of a body before it opens it.
struct RowOfBody {
    // Of the values that the blocks list. The parameters of blocks are not among them.
    OpcodeSet opcodes;
    TerminatorKindSet terminators;
    // Block by block as the function lists them, the values of a block in the order of its list.
    std::span<const IndexedValue> listedValues;
    // Every value of the function stands in the list of a block or is a parameter of one.
    bool listsAllValues { false };
    // False: no value of the function, listed or not, has an expected type. The end of a loop takes away an expected type that has become the type of its value, behind the row.
    bool holdsExpectedTypes { false };
};

// The records that a function names: in its own record, as the type or the expected type of a value, listed by a block or not, in the words of a value or of a terminator.
struct NamesInBody {
    std::span<const TypeID> types;
    std::span<const LayoutID> layouts;
    std::span<const LayoutSetID> layoutSets;
    std::span<const VariableID> variables;
};

// What loops over all functions ask of the record of a function before they look at anything else.
struct RecordFacts {
    RecordFacts() = default;
    explicit RecordFacts(const Function& function)
        : flags(function.flags)
        , capabilities(function.capabilities)
        , role(function.role)
        , kind(function.kind)
        , creatorKind(function.creator.kind())
        , numberOfResults(static_cast<uint8_t>(std::min<size_t>(function.results.size(), UINT8_MAX)))
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
    // 255 stands for more.
    uint8_t numberOfResults { 0 };
    bool hasBody { false };
};
static_assert(sizeof(RecordFacts) <= 8);

// Whether the ids in a body are those that it will have when the loop or the edit that changes it has ended.
enum class BodyIDs : bool { MayBeProvisional, Final };

// A program pass looks through all bodies, often several times, for a few kinds of operations, and finds them in a few bodies.
class BodyIndex {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodyIndex);
    WTF_MAKE_NONCOPYABLE(BodyIndex);
public:
    BodyIndex() = default;
    // With the loops all threads take the page faults of the lists when they grow.
    explicit BodyIndex(const ParallelLoops& loops)
        : m_loops(&loops)
    {
    }

    // stampOfRecord() and stampOfEntry() of a function.
    struct RecordStamps {
        uint64_t record { 0 };
        uint64_t entry { 0 };

        friend bool operator==(const RecordStamps&, const RecordStamps&) = default;
    };

    // The part of a row that screens a body out. A loop over all functions reads one cache line per function.
    struct HotRow {
        TerminatorKindSet terminators() const { return std::bit_cast<TerminatorKindSet>(static_cast<uint64_t>(terminatorBits)); }

        OpcodeSet opcodes;
        uint16_t terminatorBits { 0 };
        // False for a function with more blocks than a word has room for.
        bool isComplete { false };
        bool listsAllValues { false };
        bool holdsExpectedTypes { false };
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
        // By block.
        MallocSpan<TerminatorKind> terminatorKinds;
        // In the order of the listed values. Made with the names.
        MallocSpan<IndexedName> listedNames;
        // Types, layouts, sets of layouts, variables.
        MallocSpan<uint32_t> names;
        std::array<uint32_t, 3> endOfNames { };
        uint8_t stateOfNames { 0 };
    };

    struct Row {
        // The names are made here, while the body is in the cache, where its ids are final. Otherwise they are made when they are first asked for.
        explicit Row(const Function&, BodyIDs = BodyIDs::MayBeProvisional);

        // For a row that was made as if the ids were final, in front of the remaps of the function. The function is handed in, as it is behind them, where they have changed its record.
        void applyRemaps(const TypeRemap&, const FunctionRemap&, const Function* functionWithChangedRecord);

        HotRow hot;
        ColdRow cold;
        DenseRow dense;
        std::optional<RecordStamps> recordStamps;
    };

    // While no other thread asks.
    void growTo(size_t numberOfFunctions) { growTo(numberOfFunctions, State::Unknown); }
    void didAddFunctionsUpTo(size_t numberOfFunctions) { growTo(numberOfFunctions, State::GivenOut); }
    void forgetAll() { m_states.fill(static_cast<uint8_t>(State::Unknown)); }
    void didGiveOutAll() { m_states.fill(static_cast<uint8_t>(State::GivenOut)); }
    void didEndAllEdits();

    // From any thread, each function from one thread at a time.
    void didGiveOut(FunctionID id)
    {
        if (id.index() < m_states.size())
            stateOf(id).store(static_cast<uint8_t>(State::GivenOut), std::memory_order_relaxed);
    }
    // Does nothing, and answers false, for a function that was not given out since its row was last written.
    bool write(FunctionID, const Function&, BodyIDs = BodyIDs::MayBeProvisional);
    // The row was made from the body that the function has now.
    bool write(FunctionID, Row&&);
    void didEndEdits(FunctionID);

    // Null while the function is given out, or while another thread makes the row.
    const HotRow* hotRowOf(const Program& program, FunctionID id)
    {
        if (!ensureRow(program, id))
            return nullptr;
        const HotRow& row = m_hotRows[id.index()];
        return row.isComplete ? &row : nullptr;
    }

    // Of a function for which hotRowOf() has answered.
    std::span<const IndexedValue> listedValuesOf(FunctionID id) const { return m_coldRows[id.index()].listedValues.span(); }
    std::span<const TerminatorKind> terminatorKindsOf(FunctionID id) const { return m_coldRows[id.index()].terminatorKinds.span(); }

    // Nothing where hotRowOf() gives nothing.
    std::optional<RowOfBody> rowOf(const Program& program, FunctionID id)
    {
        const HotRow* row = hotRowOf(program, id);
        if (!row)
            return std::nullopt;
        return RowOfBody { row->opcodes, row->terminators(), listedValuesOf(id), row->listsAllValues, row->holdsExpectedTypes };
    }

    // Out of an array with 12 bytes per function. No row is made for these.
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

    // They are kept for as long as the row of the function is.
    RecordStamps recordStampsOf(const Program& program, FunctionID id)
    {
        if (!isKnown(id))
            return computeRecordStamps(program.functions[id.index()]);
        AtomicRef<uint8_t> hasStamps(m_hasRecordStamps[id.index()]);
        RecordStamps& kept = m_recordStamps[id.index()];
        if (hasStamps.load(std::memory_order_acquire))
            return RecordStamps { AtomicRef(kept.record).load(std::memory_order_relaxed), AtomicRef(kept.entry).load(std::memory_order_relaxed) };
        // Two threads that come here at one time store the same words.
        RecordStamps stamps = computeRecordStamps(program.functions[id.index()]);
        AtomicRef(kept.record).store(stamps.record, std::memory_order_relaxed);
        AtomicRef(kept.entry).store(stamps.entry, std::memory_order_relaxed);
        hasStamps.store(1, std::memory_order_release);
        return stamps;
    }

    // Made when they are first asked for. Nothing where rowOf() gives nothing, or while another thread makes them.
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

    // Nothing where namesIn() gives nothing.
    std::optional<std::span<const IndexedName>> listedNamesIn(const Program& program, FunctionID id)
    {
        if (!namesIn(program, id))
            return std::nullopt;
        return m_coldRows[id.index()].listedNames.span();
    }

    // Every row that counts is what a row made now would be.
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

    static RecordStamps computeRecordStamps(const Function&);
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

// Hands the functor (BlockID, uint32_t indexInBlock, ValueID, const Value&) for each value of the function that a block lists and whose opcode is wanted.
template<typename Functor>
void forEachListedValue(const Program& program, FunctionID id, const OpcodeSet& wanted, const Functor& functor)
{
    const Function& function = program.functions[id.index()];
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id)) {
            if (!row->opcodes.intersects(wanted))
                return;
            BlockID block;
            uint32_t indexInBlock = 0;
            for (IndexedValue listed : index->listedValuesOf(id)) {
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

// Hands the functor (BlockID, ValueID, Opcode, NameKind, uint32_t index) for each name of each value that a block lists and whose opcode is wanted: see IndexedName.
template<typename Functor>
void forEachListedName(const Program& program, FunctionID id, const OpcodeSet& wanted, const Functor& functor)
{
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id)) {
            if (!row->opcodes.intersects(wanted))
                return;
            if (std::optional<std::span<const IndexedName>> names = index->listedNamesIn(program, id)) {
                std::span<const IndexedValue> listedValues = index->listedValuesOf(id);
                for (IndexedName name : *names) {
                    IndexedValue listed = listedValues[name.position()];
                    if (wanted.contains(listed.opcode()))
                        functor(listed.block(), listed.value(), listed.opcode(), name.kind(), name.index());
                }
                return;
            }
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

// Hands the functor (BlockID, ValueID, const Value&) for each value that a block lists, whose opcode is wanted, and one of whose names the filter (NameKind, uint32_t index) accepts.
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

// Hands the functor (BlockID, ValueID, FunctionID callee) for each CallDirect that a block lists, in the order of forEachListedValue().
template<typename Functor>
void forEachDirectCall(const Program& program, FunctionID id, const Functor& functor)
{
    forEachListedName(program, id, { Opcode::CallDirect }, [&](BlockID block, ValueID value, Opcode, NameKind kind, uint32_t nameIndex) {
        if (kind == NameKind::Function)
            functor(block, value, FunctionID(nameIndex));
    });
}

// Hands the functor (ValueID, const Value&) for each value of the function whose opcode is wanted, listed by a block or not, in the order of their numbers.
template<typename Functor>
void forEachValueListedOrNot(const Program& program, FunctionID id, const OpcodeSet& wanted, const Functor& functor)
{
    ASSERT(!wanted.contains(Opcode::Parameter));
    const Function& function = program.functions[id.index()];
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id); row && row->listsAllValues) {
            if (!row->opcodes.intersects(wanted))
                return;
            Vector<ValueID, 16> found;
            for (IndexedValue listed : index->listedValuesOf(id)) {
                if (wanted.contains(listed.opcode()))
                    found.append(listed.value());
            }
            std::ranges::sort(found);
            for (ValueID value : found)
                functor(value, function.at(value));
            return;
        }
    }
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        const Value& value = function.values[valueIndex];
        if (wanted.contains(value.opcode))
            functor(ValueID(valueIndex), value);
    }
}

// Whether the predicate (ValueID, const Value&) holds for some value of the function that a block lists and whose opcode is wanted.
template<typename Predicate>
bool anyListedValue(const Program& program, FunctionID id, const OpcodeSet& wanted, const Predicate& predicate)
{
    const Function& function = program.functions[id.index()];
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id)) {
            if (!row->opcodes.intersects(wanted))
                return false;
            for (IndexedValue listed : index->listedValuesOf(id)) {
                if (wanted.contains(listed.opcode()) && predicate(listed.value(), function.at(listed.value())))
                    return true;
            }
            return false;
        }
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

// The same for all values of the function, listed by a block or not. No parameter of a block is wanted.
template<typename Predicate>
bool anyValueListedOrNot(const Program& program, FunctionID id, const OpcodeSet& wanted, const Predicate& predicate)
{
    ASSERT(!wanted.contains(Opcode::Parameter));
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id); row && row->listsAllValues)
            return anyListedValue(program, id, wanted, predicate);
    }
    const Function& function = program.functions[id.index()];
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        const Value& value = function.values[valueIndex];
        if (wanted.contains(value.opcode) && predicate(ValueID(valueIndex), value))
            return true;
    }
    return false;
}

// False where no block of the function lists a value with one of the opcodes. True says nothing.
inline bool mayListValueWith(const Program& program, FunctionID id, const OpcodeSet& wanted)
{
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id))
            return row->opcodes.intersects(wanted);
    }
    return true;
}

// False where the function has no value with one of the opcodes, listed by a block or not. True says nothing.
inline bool mayHoldValueWith(const Program& program, FunctionID id, const OpcodeSet& wanted)
{
    ASSERT(!wanted.contains(Opcode::Parameter));
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id); row && row->listsAllValues)
            return row->opcodes.intersects(wanted);
    }
    return true;
}

// True where every value of the function stands in the list of a block or is a parameter of one. False says nothing.
inline bool listsAllValues(const Program& program, FunctionID id)
{
    BodyIndex* index = program.bodyIndex();
    const BodyIndex::HotRow* row = index ? index->hotRowOf(program, id) : nullptr;
    return row && row->listsAllValues;
}

// False where no value of the function, listed or not, has an expected type. True says nothing.
inline bool mayHoldExpectedTypes(const Program& program, FunctionID id)
{
    BodyIndex* index = program.bodyIndex();
    const BodyIndex::HotRow* row = index ? index->hotRowOf(program, id) : nullptr;
    return !row || row->holdsExpectedTypes;
}

// False where no block of the function ends in a terminator of one of the kinds. True says nothing.
inline bool mayEndBlockWith(const Program& program, FunctionID id, const TerminatorKindSet& wanted)
{
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id))
            return row->terminators().intersects(wanted);
    }
    return true;
}

// False where the function names no record whose bit is set: see NamesInBody. True says nothing.
template<typename IDType>
bool mayNameAnyOf(std::span<const IDType> names, const BitVector& wanted)
{
    return std::ranges::any_of(names, [&](IDType name) {
        return wanted.get(name.index());
    });
}

inline bool mayNameLayoutIn(const Program& program, FunctionID id, const BitVector& wanted)
{
    BodyIndex* index = program.bodyIndex();
    std::optional<NamesInBody> names = index ? index->namesIn(program, id) : std::nullopt;
    return !names || mayNameAnyOf(names->layouts, wanted);
}

inline bool mayNameVariableIn(const Program& program, FunctionID id, const BitVector& wanted)
{
    BodyIndex* index = program.bodyIndex();
    std::optional<NamesInBody> names = index ? index->namesIn(program, id) : std::nullopt;
    return !names || mayNameAnyOf(names->variables, wanted);
}

// Hands the functor (BlockID) for each block of the function that ends in a terminator of one of the kinds, in ascending order. With a row of the index no block is read.
template<typename Functor>
void forEachBlockEndingWith(const Program& program, FunctionID id, const TerminatorKindSet& wanted, const Functor& functor)
{
    if (BodyIndex* index = program.bodyIndex()) {
        if (const BodyIndex::HotRow* row = index->hotRowOf(program, id)) {
            if (!row->terminators().intersects(wanted))
                return;
            std::span<const TerminatorKind> kinds = index->terminatorKindsOf(id);
            for (uint32_t blockIndex = 0; blockIndex < kinds.size(); ++blockIndex) {
                if (wanted.contains(kinds[blockIndex]))
                    functor(BlockID(blockIndex));
            }
            return;
        }
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
