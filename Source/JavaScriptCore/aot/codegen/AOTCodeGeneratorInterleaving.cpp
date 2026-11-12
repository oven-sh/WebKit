/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTControlEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTOperationContext.h"
#include <algorithm>
#include <bit>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static constexpr int64_t mostIterationsOfRow = 160;
static constexpr unsigned rowsInStep = 4;
static constexpr unsigned consecutiveRows = 6;
static constexpr unsigned mostRows = 6;
static constexpr int64_t mostStepsOfRows = mostIterationsOfRow + mostRows;
static constexpr unsigned mostBlocksBeforeRow = 32;
static constexpr unsigned fewestDependentOperations = 4;

class InterleavingFinder {
public:
    using Interleaving = CodeGenerator::Interleaving;
    using Stream = CodeGenerator::ElementStream;

    InterleavingFinder(const CodeGenerator& generator, const CodeGenerator::CountedLoop& loop)
        : m_generator(generator)
        , m_code(generator.code())
        , m_loop(loop)
        , m_head(generator.m_order[loop.positionOfHead])
        , m_parameters(m_code.at(m_head).parameters())
        , m_back(m_code.at(m_code.at(loop.turn.last()).terminator.next()).arguments())
    {
    }

    std::optional<Interleaving> find();

private:
    struct RowIndex {
        ValueID counter;
        ValueID scale;
        int64_t added { 0 };
        int64_t offset { 0 };
    };
    const Operation& at(ValueID value) const { return m_code.at(value); }
    Representation representationOf(ValueID value) const { return m_generator.representationOf(value); }
    ValueID passed(ValueID value) const
    {
        static constexpr OpcodeSet passingOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::NumberToInteger };
        while (passingOn.contains(at(value).opcode))
            value = at(value).valueOperand();
        return value;
    }
    bool isInTurn(ValueID value) const
    {
        BlockID home = m_code.blockOf(value);
        return home && (home == m_head || m_loop.turn.contains(home));
    }
    bool dependsOnRow(ValueID value) const { return m_dependent.contains(value) || std::ranges::contains(m_outerParameters, value); }

    bool findOuterLoop();
    bool findEndOfRow();
    bool findDependentOperations();
    bool isRepeatable(ValueID) const;
    std::optional<size_t> streamReadBeforeRow(ValueID) const;
    bool findStreams();
    bool findStream(ValueID access, ValueID list, ValueID storage, ValueID position, bool isStore);
    bool carriesChain() const;
    bool onlyIndexesElements(ValueID parameter) const;
    std::optional<RowIndex> rowIndexOf(ValueID, unsigned depth = 0) const;
    void findNeighbors();

    const CodeGenerator& m_generator;
    const Code& m_code;
    const CodeGenerator::CountedLoop& m_loop;
    BlockID m_head;
    std::span<const ValueID> m_parameters;
    std::span<const ValueID> m_back;
    std::span<const ValueID> m_outerParameters;
    Vector<ValueID, 32> m_dependent;
    Interleaving m_found;
};

bool InterleavingFinder::findOuterLoop()
{
    BlockID entry;
    unsigned numberOfEntries = 0;
    m_code.forEachIncomingEdgeUnsealed(m_head, [&](const Edge& edge) {
        if (edge.to != m_head || !m_code.isReached(edge.from) || edge.from == m_loop.turn.last())
            return;
        ++numberOfEntries;
        entry = edge.from;
    });
    if (numberOfEntries != 1 || m_code.at(entry).terminator.kind != TerminatorKind::Jump)
        return false;
    m_found.entry = entry;
    auto predecessorOf = [&](BlockID block) {
        if (BlockID single = m_generator.m_singlePredecessors[block.index()])
            return single;
        BlockID found;
        m_code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            if (edge.to == block && m_code.isReached(edge.from) && m_code.at(edge.from).frequency == BlockFrequency::Normal)
                found = edge.from;
        });
        m_code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            if (edge.to == block && m_code.isReached(edge.from) && edge.from != found && m_generator.m_singlePredecessors[edge.from.index()] != found)
                found = BlockID();
        });
        return found;
    };
    for (BlockID block = entry; m_found.blocksBeforeRow.size() < mostBlocksBeforeRow; block = predecessorOf(block)) {
        if (!block || m_code.handlerOf(block) || m_code.at(block).isEntry || m_code.at(block).isHandlerBlock())
            return false;
        m_found.blocksBeforeRow.append(block);
        if (m_code.at(block).isLoopHead) {
            m_found.outerHead = block;
            m_found.blocksBeforeRow.reverse();
            m_outerParameters = m_code.at(block).parameters();
            return true;
        }
        if (!m_code.at(block).parameters().empty())
            return false;
    }
    return false;
}

bool InterleavingFinder::findEndOfRow()
{
    static constexpr unsigned mostBlocks = 4;
    const Edge& exit = m_code.at(m_code.at(m_loop.asking).terminator.notTaken());
    if (!exit.arguments().empty())
        return false;
    BlockID block = exit.to;
    for (unsigned number = 0; number < mostBlocks; ++number) {
        const Block& behind = m_code.at(block);
        if (!behind.parameters().empty() || behind.terminator.kind != TerminatorKind::Jump || m_code.handlerOf(block))
            return false;
        for (ValueID value : m_code.valuesOf(block)) {
            bool readsRow = false;
            at(value).forEachChild([&](ValueID operand) {
                readsRow |= isInTurn(operand);
            });
            if (readsRow || !at(value).traits().isPure())
                return false;
        }
        m_found.blocksAfterVersion.append(block);
        const Edge& edge = m_code.at(behind.terminator.next());
        if (edge.to == m_found.outerHead) {
            m_found.steppedParameters.append(edge.arguments());
            return m_found.steppedParameters.size() == m_outerParameters.size();
        }
        block = edge.to;
    }
    return false;
}

bool InterleavingFinder::isRepeatable(ValueID value) const
{
    static constexpr OpcodeSet repeatable { Opcode::ConvertToDoubleStorage, Opcode::RequireObjectCoercible, Opcode::CheckProof, Opcode::LoopHint };
    const Operation& operation = at(value);
    if (operation.traits().isPure() || repeatable.contains(operation.opcode))
        return true;
    static constexpr OpcodeSet reads { Opcode::GetEnvironmentField, Opcode::GetModuleVariable, Opcode::IsPristine, Opcode::HasArrayStorage, Opcode::GetArrayLength, Opcode::GetArrayStorage };
    return reads.contains(operation.opcode) && !operation.traits().mayWriteObservableState();
}

std::optional<size_t> InterleavingFinder::streamReadBeforeRow(ValueID value) const
{
    const Operation& read = at(value);
    if (read.opcode != Opcode::GetArrayElement || dependsOnRow(read.arrayOperand()) || representationOf(read.positionOperand()) != Representation::Double || representationOf(value) != Representation::JSValue)
        return std::nullopt;
    auto withoutCasts = [&](ValueID list) {
        static constexpr OpcodeSet casts { Opcode::Narrowed, Opcode::Unbox, Opcode::Box, Opcode::RequireObjectCoercible };
        while (casts.contains(at(list).opcode))
            list = at(list).valueOperand();
        return list;
    };
    for (size_t place = 0; place < m_found.streams.size(); ++place) {
        if (m_found.streams[place].length && withoutCasts(m_found.streams[place].list) == withoutCasts(read.arrayOperand()))
            return place;
    }
    return std::nullopt;
}

bool InterleavingFinder::findDependentOperations()
{
    for (size_t place = 0; place < m_found.blocksBeforeRow.size(); ++place) {
        BlockID block = m_found.blocksBeforeRow[place];
        for (ValueID value : m_code.valuesOf(block)) {
            const Operation& operation = at(value);
            bool depends = false;
            operation.forEachChild([&](ValueID operand) {
                depends |= dependsOnRow(operand);
            });
            if (!depends) {
                if (!isRepeatable(value))
                    return false;
                continue;
            }
            m_dependent.append(value);
            if (std::optional<size_t> stream = streamReadBeforeRow(value)) {
                m_found.readsBeforeRow.append({ value, *stream });
                continue;
            }
            if (operation.opcode == Opcode::CheckProof)
                continue;
            if (!operation.traits().isPure())
                return false;
        }
        const Terminator& terminator = m_code.at(block).terminator;
        if (terminator.kind != TerminatorKind::Jump && terminator.kind != TerminatorKind::Branch)
            return false;
        BlockID next = place + 1 < m_found.blocksBeforeRow.size() ? m_found.blocksBeforeRow[place + 1] : m_head;
        if (terminator.kind == TerminatorKind::Branch && m_code.at(terminator.taken()).to != next)
            return false;
        if (terminator.kind == TerminatorKind::Jump && m_code.at(terminator.next()).to != next)
            return false;
    }
    m_found.dependentOperations.append(m_dependent.span());
    return true;
}

bool InterleavingFinder::findStream(ValueID access, ValueID list, ValueID storage, ValueID position, bool isStore)
{
    if (!storage || isInTurn(storage) || isInTurn(list))
        return false;
    Stream stream;
    stream.access = access;
    stream.list = list;
    stream.storage = storage;
    stream.isStore = isStore;
    std::span<const ValueID> lengths = isStore ? std::span<const ValueID>() : at(access).lengthOperands();
    stream.length = lengths.empty() ? ValueID() : lengths[0];
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    auto findStepped = [&](ValueID value, int64_t& added) {
        while (true) {
            value = passed(value);
            const Operation& sum = at(value);
            if (!steps.contains(sum.opcode) || !isInTurn(value) || (sum.form() != OperatorForm::Integers && sum.form() != OperatorForm::CheckedIntegers))
                return value;
            if (sum.opcode == Opcode::Increment || sum.opcode == Opcode::Decrement) {
                added += sum.opcode == Opcode::Increment ? 1 : -1;
                value = sum.valueOperand();
                continue;
            }
            if (!m_code.isConstant(sum.rightOperand()) || at(sum.rightOperand()).opcode != Opcode::Integer)
                return value;
            int64_t constant = at(sum.rightOperand()).integer();
            added += sum.opcode == Opcode::Add ? constant : -constant;
            value = sum.leftOperand();
        }
    };
    ValueID carried = findStepped(position, stream.offset);
    stream.indexOfParameter = std::ranges::find(m_parameters, carried) - m_parameters.begin();
    if (stream.indexOfParameter == m_parameters.size() || !m_generator.m_values[carried.index()])
        return false;
    int64_t step = 0;
    if (findStepped(m_back[stream.indexOfParameter], step) != carried || step != 1 || carried == m_loop.counter || representationOf(carried) != Representation::Int64 || !onlyIndexesElements(carried))
        return false;
    m_found.streams.append(stream);
    return true;
}

bool InterleavingFinder::findStreams()
{
    for (BlockID block : m_loop.turn) {
        for (ValueID value : m_code.valuesOf(block)) {
            const Operation& operation = at(value);
            switch (operation.opcode) {
            case Opcode::GetArrayElement:
            case Opcode::IsElementPresent:
                if (operation.arrayStorage() != ArrayStorage::Double || !findStream(value, operation.arrayOperand(), operation.storageOperand(), operation.positionOperand(), false))
                    return false;
                break;
            case Opcode::PutDoubleIndex:
                if (m_found.store || !findStream(value, operation.arrayOperand(), operation.storageOperand(), operation.positionOperand(), true))
                    return false;
                m_found.store = value;
                break;
            default:
                if (!operation.traits().isPure() && operation.opcode != Opcode::LoopHint)
                    return false;
            }
        }
    }
    return !!m_found.store;
}

bool InterleavingFinder::onlyIndexesElements(ValueID parameter) const
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Add, Opcode::Sub, Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::NumberToInteger };
    Vector<ValueID, 8> indexes { parameter };
    for (BlockID block : m_loop.turn) {
        for (ValueID value : m_code.valuesOf(block)) {
            const Operation& operation = at(value);
            bool readsIndex = false;
            operation.forEachChild([&](ValueID operand) {
                readsIndex |= indexes.contains(operand);
            });
            if (!readsIndex)
                continue;
            if (steps.contains(operation.opcode)) {
                indexes.append(value);
                continue;
            }
            if (m_loop.accessesUnderCounterBelowBound.contains(value) || operation.opcode == Opcode::PutDoubleIndex)
                continue;
            if (operation.opcode == Opcode::GetArrayElement && (operation.isPresent() || operation.isInBounds()))
                continue;
            if (m_loop.relationsOfCounterBelowBound.contains(value))
                continue;
            return false;
        }
    }
    return true;
}

bool InterleavingFinder::carriesChain() const
{
    for (size_t index = 0; index < m_parameters.size(); ++index) {
        ValueID parameter = m_parameters[index];
        if (!m_generator.m_values[parameter.index()] || representationOf(parameter) != Representation::Double)
            continue;
        Vector<ValueID, 16> chain { parameter };
        unsigned length = 0;
        for (BlockID block : m_loop.turn) {
            for (ValueID value : m_code.valuesOf(block)) {
                if (representationOf(value) != Representation::Double)
                    continue;
                bool depends = false;
                at(value).forEachChild([&](ValueID operand) {
                    depends |= chain.contains(operand);
                });
                if (!depends)
                    continue;
                chain.append(value);
                length += at(value).opcode != Opcode::Narrowed;
            }
        }
        if (length >= fewestDependentOperations && chain.contains(passed(m_back[index])))
            return true;
    }
    return false;
}

std::optional<InterleavingFinder::RowIndex> InterleavingFinder::rowIndexOf(ValueID value, unsigned depth) const
{
    static constexpr unsigned mostDepth = 16;
    static constexpr OpcodeSet conversions { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::NumberToInteger, Opcode::IntegerToNumber };
    while (conversions.contains(at(value).opcode))
        value = at(value).valueOperand();
    if (std::ranges::contains(m_outerParameters, value))
        return RowIndex { value, ValueID(), 0, 0 };
    const Operation& operation = at(value);
    static constexpr OpcodeSet operators { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub, Opcode::Mul };
    if (depth == mostDepth || !operators.contains(operation.opcode) || operation.form() == OperatorForm::Generic)
        return std::nullopt;
    auto plus = [](std::optional<RowIndex> index, int64_t added) {
        if (index)
            (index->scale ? index->offset : index->added) += added;
        return index;
    };
    if (operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement)
        return plus(rowIndexOf(operation.valueOperand(), depth + 1), operation.opcode == Opcode::Increment ? 1 : -1);
    if (operation.opcode == Opcode::Mul) {
        bool leftDepends = dependsOnRow(operation.leftOperand());
        if (leftDepends == dependsOnRow(operation.rightOperand()))
            return std::nullopt;
        std::optional<RowIndex> index = rowIndexOf(leftDepends ? operation.leftOperand() : operation.rightOperand(), depth + 1);
        if (!index || index->scale)
            return std::nullopt;
        index->scale = leftDepends ? operation.rightOperand() : operation.leftOperand();
        return index;
    }
    ValueID constant = operation.rightOperand();
    static constexpr int64_t greatestConstant = 1 << 20;
    if (!m_code.isConstant(constant) || at(constant).opcode != Opcode::Integer || std::abs(at(constant).integer()) > greatestConstant)
        return std::nullopt;
    return plus(rowIndexOf(operation.leftOperand(), depth + 1), operation.opcode == Opcode::Add ? at(constant).integer() : -at(constant).integer());
}

void InterleavingFinder::findNeighbors()
{
    std::span<const ValueID> entryArguments = m_code.at(m_code.at(m_found.entry).terminator.next()).arguments();
    Vector<std::optional<RowIndex>, 8> indexes;
    std::optional<RowIndex> ofStore;
    Stream* store = nullptr;
    for (Stream& stream : m_found.streams) {
        indexes.append(rowIndexOf(entryArguments[stream.indexOfParameter]));
        if (stream.isStore) {
            store = &stream;
            ofStore = indexes.last();
        }
    }
    auto isOfSameRows = [&](const std::optional<RowIndex>& index) {
        return index && index->scale && index->counter == ofStore->counter && index->scale == ofStore->scale;
    };
    if (!ofStore || !isOfSameRows(ofStore))
        return;
    size_t outerIndex = std::ranges::find(m_outerParameters, ofStore->counter) - m_outerParameters.begin();
    std::optional<RowIndex> next = rowIndexOf(m_found.steppedParameters[outerIndex]);
    if (!next || next->counter != ofStore->counter || next->scale || !next->added)
        return;
    m_found.rowsAreEvenlySpaced = std::ranges::all_of(indexes, isOfSameRows);
    if (!m_found.rowsAreEvenlySpaced)
        return;
    for (size_t place = 0; place < m_found.streams.size(); ++place) {
        Stream& stream = m_found.streams[place];
        if (stream.isStore || stream.storage != store->storage || !isOfSameRows(indexes[place]) || representationOf(stream.access) != Representation::Double || at(stream.access).opcode != Opcode::GetArrayElement)
            continue;
        if (indexes[place]->offset + stream.offset != ofStore->offset + store->offset)
            continue;
        int64_t distance = indexes[place]->added - ofStore->added;
        stream.isStoredByPreviousRow = distance == -next->added;
        m_found.rowsAreConsecutive |= stream.isStoredByPreviousRow;
    }
}

std::optional<CodeGenerator::Interleaving> InterleavingFinder::find()
{
    if (!carriesChain() || !findOuterLoop() || !findEndOfRow() || !findStreams() || !findDependentOperations())
        return std::nullopt;
    if (dependsOnRow(m_code.at(m_code.at(m_found.entry).terminator.next()).arguments()[std::ranges::find(m_parameters, m_loop.counter) - m_parameters.begin()]))
        return std::nullopt;
    if (!m_loop.bound || isInTurn(m_loop.bound) || dependsOnRow(m_loop.bound))
        return std::nullopt;
    auto someDependsOnRow = [&](const auto& values) {
        return std::ranges::any_of(values, [&](ValueID value) { return dependsOnRow(value); });
    };
    if (std::ranges::any_of(m_loop.steppedIndexes, [&](const CodeGenerator::SteppedIndex& held) { return held.step > 1 || dependsOnRow(held.length); }))
        return std::nullopt;
    if (someDependsOnRow(m_loop.lengthsHeldAgainstBound) || someDependsOnRow(m_loop.relationsAskedInFront) || !m_loop.listsWhoseKindIsRead.isEmpty())
        return std::nullopt;
    for (const CodeGenerator::ListAskedForKind& asked : m_loop.listsAskedForKind) {
        if (asked.kind != ArrayStorage::Double || asked.isStoredInto || dependsOnRow(asked.list))
            return std::nullopt;
    }
    for (const Stream& stream : m_found.streams) {
        if (dependsOnRow(stream.list) || dependsOnRow(stream.storage))
            return std::nullopt;
    }
    if (std::ranges::any_of(m_outerParameters, [&](ValueID parameter) { return !m_generator.m_values[parameter.index()] || m_generator.isCarriedBoxed(parameter); }))
        return std::nullopt;
    for (ValueID stepped : m_found.steppedParameters) {
        if (isInTurn(stepped))
            return std::nullopt;
    }
    findNeighbors();
    m_found.rows = m_found.rowsAreConsecutive ? consecutiveRows : rowsInStep;
    return WTF::move(m_found);
}

std::optional<CodeGenerator::Interleaving> CodeGenerator::findInterleaving(const CountedLoop& loop)
{
    if (loop.testsAtEnd || loop.step != 1 || (loop.relation != Opcode::Less && loop.relation != Opcode::LessEq) || m_keepsValuesInHeapFrame || m_followsNumbersOutOfBytes || !loop.loopCarriedValueStores.isEmpty() || loop.storesIntoSlots || !loop.storesIntoLists || !loop.flatStrings.isEmpty())
        return std::nullopt;
    if (!loop.viewsAskedForKind.isEmpty() || !loop.boundsAskedInFront.isEmpty())
        return std::nullopt;
    if (checksBoundsUnder(build().options().testing) || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly))
        return std::nullopt;
    return InterleavingFinder(*this, loop).find();
}

void CodeGenerator::generateEndOfRow(const Interleaving& nest)
{
    SetForScope<BlockID> whileLayingOut(m_headOfTurnsLaidOut, nest.outerHead);
    for (BlockID block : nest.blocksAfterVersion) {
        for (ValueID value : code().valuesOf(block)) {
            m_values[value.index()] = nullptr;
            generateValue(block, value, code().at(value), false, { });
        }
    }
}

void CodeGenerator::generateNextRow(const Interleaving& nest, LBasicBlock isNoRow, Vector<LValue, 2>& addressesRead)
{
    generateEndOfRow(nest);
    Vector<LValue, 4> stepped;
    for (ValueID parameter : nest.steppedParameters)
        stepped.append(get(parameter).value);
    std::span<const ValueID> parameters = code().at(nest.outerHead).parameters();
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = stepped[index];
    auto require = [&](LValue condition) {
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(condition, normalEdge(holds), normalEdge(isNoRow));
        m_out.appendTo(holds);
    };
    for (BlockID block : nest.blocksBeforeRow) {
        for (ValueID value : code().valuesOf(block)) {
            if (!nest.dependentOperations.contains(value))
                continue;
            const Operation& operation = code().at(value);
            m_values[value.index()] = nullptr;
            if (auto read = std::ranges::find(nest.readsBeforeRow, value, &std::pair<ValueID, size_t>::first); read != nest.readsBeforeRow.end()) {
                const ElementStream& stream = nest.streams[read->second];
                TerminatorContext contextOfEntry(nest.entry, code().at(nest.entry).terminator, nullptr);
                LValue storage = nullptr;
                LValue length = nullptr;
                {
                    SetForScope<const OperationContext*> whileEntering(m_operation, &contextOfEntry);
                    storage = get(stream.storage).value;
                    length = m_out.zeroExt(get(stream.length).value, Machine::Int64);
                }
                ValueContext context(block, value, operation, nullptr);
                SetForScope<const OperationContext*> whileReading(m_operation, &context);
                declareAccessesOf(context);
                Number position = get(operation.positionOperand()).as<Number>();
                Integer index = emitter<ConversionEmitter>().toInteger(position);
                require(m_out.doubleEqual(m_out.intToDouble(index.value()), position.value()));
                require(m_out.below(index.value(), length));
                LValue element = m_out.loadDouble(emitter<ElementAccessEmitter>().doubleElement(storage, index.value()));
                require(m_out.doubleEqual(element, element));
                m_values[value.index()] = emitter<ConversionEmitter>().box(context, Operand(Number(element)), typeOf(value)).value();
                addressesRead.append(m_out.add(storage, m_out.shl(index.value(), 3)));
                if (m_checksAllocation) [[unlikely]]
                    m_out.clearDeclaredAccesses();
                continue;
            }
            if (operation.opcode == Opcode::CheckProof || operation.opcode == Opcode::LoopHint)
                continue;
            SetForScope<BlockID> whileLayingOut(m_headOfTurnsLaidOut, nest.outerHead);
            generateValue(block, value, operation, false, { });
        }
        const Terminator& terminator = code().at(block).terminator;
        if (terminator.kind == TerminatorKind::Branch && nest.dependentOperations.contains(terminator.conditionOperand())) {
            TerminatorContext context(block, terminator, nullptr);
            SetForScope<const OperationContext*> whileTesting(m_operation, &context);
            require(get(terminator.conditionOperand()).value);
        }
    }
}

void CodeGenerator::keepOverwrittenElement(const Interleaving& nest)
{
    LValue old = m_out.loadDouble(emitter<ElementAccessEmitter>().doubleElement(nest.baseOfStore[nest.row], nest.step));
    m_out.storeDouble(old, keptElement(nest, nest.row, nest.step));
}

TypedPointer CodeGenerator::keptElement(const Interleaving& nest, unsigned row, LValue step)
{
    RELEASE_ASSERT(row);
    return TypedPointer(heaps().frameSlots, m_out.add(m_out.slotBase(nest.kept), m_out.shl(step, 3)), (row - 1) * mostStepsOfRows * sizeof(double));
}

bool CodeGenerator::beginInterleavedOperation(ValueID id, const Operation& operation)
{
    Interleaving& nest = *m_interleaving;
    if (id == nest.store && nest.row) {
        ValueContext context(operation.block(), id, operation, nullptr);
        SetForScope<const OperationContext*> whileKeeping(m_operation, &context);
        declareAccessesOf(context);
        keepOverwrittenElement(nest);
    }
    if (id == nest.store)
        nest.numberStored = getIn(operation.valueOperand(), Representation::Double).value;
    for (const ElementStream& stream : nest.streams) {
        if (stream.access != id)
            continue;
        if (nest.row && stream.isStoredByPreviousRow) {
            m_values[id.index()] = nest.numberPassedOn;
            return true;
        }
        m_values[stream.storage.index()] = nest.bases[nest.row][&stream - nest.streams.begin()];
        return false;
    }
    return false;
}

void CodeGenerator::layOutInterleaved(const CountedLoop& loop, Interleaving& nest)
{
    build().statistics().count(StatisticID::LoopNestsInterleaved);
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    std::span<const ValueID> outerParameters = code().at(nest.outerHead).parameters();
    std::span<const ValueID> entryArguments = code().at(code().at(nest.entry).terminator.next()).arguments();
    std::span<const BlockID> turn = loop.turn.span();
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    using Values = Vector<LValue, 8>;
    Values phisOfHead;
    Values phisOfSingle;
    LBasicBlock single = m_out.newBlock();
    LBasicBlock rows = m_out.newBlock();
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phisOfHead.append(phi);
        phisOfSingle.append(phi ? m_out.phiIn(single, phi->type()) : nullptr);
    }
    auto passToSingle = [&](auto&& valueAt) {
        Values values;
        for (size_t index = 0; index < parameters.size(); ++index)
            values.append(phisOfSingle[index] ? valueAt(index) : nullptr);
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phisOfSingle[index])
                m_out.upsilon(values[index], phisOfSingle[index]);
        }
        m_out.jump(normalEdge(single));
    };
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    auto isStreamed = [&](size_t index) {
        return std::ranges::contains(nest.streams, index, &ElementStream::indexOfParameter);
    };

    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    TerminatorContext context(head, blockOfHead.terminator, nullptr);
    TerminatorContext contextOfEntry(nest.entry, code().at(nest.entry).terminator, nullptr);
    LBasicBlock isNoRow = m_out.newBlock();
    {
        LBasicBlock here = m_out.m_block;
        m_out.appendTo(isNoRow);
        passToSingle([&](size_t index) { return phisOfHead[index]; });
        m_out.appendTo(here);
    }
    auto require = [&](LValue condition) {
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(condition, normalEdge(holds), normalEdge(isNoRow));
        m_out.appendTo(holds);
    };
    auto wide = [&](LValue value) { return value->type() == Machine::Int32 ? m_out.signExt32To64(value) : value; };
    LValue numberOfIterations = nullptr;
    Vector<Values, mostRows> entries;
    Vector<LValue, 4> outerParametersOfHead;
    {
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LValue counter = wide(phisOfHead[indexOfCounter]);
        numberOfIterations = m_out.sub(wide(get(loop.bound).value), counter);
        if (loop.relation == Opcode::LessEq)
            numberOfIterations = m_out.add(numberOfIterations, m_out.constInt64(1));
        static constexpr int64_t fewestIterations = 8;
        require(m_out.below(m_out.sub(numberOfIterations, m_out.constInt64(fewestIterations)), m_out.constInt64(mostIterationsOfRow - fewestIterations + 1)));
        for (ValueID relation : loop.relationsAskedInFront)
            require(emitter<ControlEmitter>().hoistedResult(relation).value());
        for (ValueID length : loop.lengthsHeldAgainstBound)
            require(m_out.greaterThanOrEqual(m_out.zeroExt(get(length).value, Machine::Int64), wide(get(loop.bound).value)));
        for (const ListAskedForKind& asked : loop.listsAskedForKind)
            require(elements.listHasArrayStorage(asked.list, asked.kind).value());
    }
    entries.append(phisOfHead);
    Vector<Vector<LValue, 2>, mostRows> addressesReadByRows;
    addressesReadByRows.append({ });
    Vector<std::pair<ValueID, LValue>, 64> saved;
    auto save = [&](ValueID value) {
        saved.append({ value, m_values[value.index()] });
    };
    for (ValueID parameter : outerParameters)
        save(parameter);
    for (BlockID block : nest.blocksAfterVersion) {
        for (ValueID value : code().valuesOf(block))
            save(value);
    }
    for (ValueID value : nest.dependentOperations)
        save(value);
    Vector<Vector<LValue, 64>, mostRows> outerValuesOfRows;
    auto noteOuterValues = [&] {
        Vector<LValue, 64> outerValues;
        for (auto& [value, lowered] : saved)
            outerValues.append(m_values[value.index()]);
        outerValuesOfRows.append(WTF::move(outerValues));
    };
    noteOuterValues();
    for (unsigned row = 1; row < nest.rows; ++row) {
        Vector<LValue, 2> addressesRead;
        generateNextRow(nest, isNoRow, addressesRead);
        if (m_hasFailed)
            return;
        noteOuterValues();
        addressesReadByRows.append(WTF::move(addressesRead));
        SetForScope<const OperationContext*> whileEntering(m_operation, &contextOfEntry);
        Values entry;
        for (size_t index = 0; index < parameters.size(); ++index)
            entry.append(phisOfHead[index] ? getIn(entryArguments[index], representationOf(parameters[index])).value : nullptr);
        entries.append(WTF::move(entry));
    }
    generateEndOfRow(nest);
    Vector<LValue, 4> outerParametersAfterVersions;
    {
        BlockID last = nest.blocksAfterVersion.last();
        TerminatorContext contextOfEnd(last, code().at(last).terminator, nullptr);
        SetForScope<const OperationContext*> whileStepping(m_operation, &contextOfEnd);
        for (size_t index = 0; index < outerParameters.size(); ++index)
            outerParametersAfterVersions.append(getIn(nest.steppedParameters[index], representationOf(outerParameters[index])).value);
    }
    for (auto [value, lowered] : saved)
        m_values[value.index()] = lowered;

    nest.bases.shrink(0);
    nest.baseOfStore.shrink(0);
    {
        SetForScope<const OperationContext*> whileAddressing(m_operation, &context);
        auto firstElementOf = [&](const ElementStream& stream, LValue base) {
            return m_out.add(base, m_out.constInt64(stream.offset * static_cast<int64_t>(sizeof(double))));
        };
        auto greatestIndexIsInside = [&](const SteppedIndex& held, const Values& entry) {
            LValue greatest = m_out.add(m_out.add(wide(entry[held.indexOfParameter]), held.step > 0 ? numberOfIterations : m_out.constInt64(0)), m_out.constInt64(held.offset - (held.step > 0 ? 1 : 0)));
            return m_out.lessThan(greatest, m_out.zeroExt(get(held.length).value, Machine::Int64));
        };
        for (const Values& entry : entries) {
            bool isTestedByLastVersion = nest.rowsAreEvenlySpaced && &entry != &entries.last();
            for (const SteppedIndex& held : loop.steppedIndexes) {
                if (!isTestedByLastVersion || !isStreamed(held.indexOfParameter))
                    require(greatestIndexIsInside(held, entry));
            }
            Vector<LValue, 8> bases;
            for (const ElementStream& stream : nest.streams)
                bases.append(m_out.add(get(stream.storage).value, m_out.shl(entry[stream.indexOfParameter], 3)));
            for (size_t place = 0; place < nest.streams.size(); ++place) {
                if (nest.streams[place].isStore)
                    nest.baseOfStore.append(firstElementOf(nest.streams[place], bases[place]));
            }
            nest.bases.append(WTF::move(bases));
        }
        for (size_t place = 0; place < nest.streams.size(); ++place) {
            const ElementStream& stream = nest.streams[place];
            if (stream.isStoredByPreviousRow)
                require(m_out.equal(firstElementOf(stream, nest.bases[1][place]), nest.baseOfStore[0]));
        }
        LValue bytesOfRow = m_out.sub(m_out.shl(numberOfIterations, 3), m_out.constInt64(1));
        for (unsigned row = 0; row < nest.rows; ++row) {
            for (unsigned later = row + 1; later < nest.rows; ++later) {
                for (LValue address : addressesReadByRows[later])
                    require(m_out.above(m_out.sub(address, nest.baseOfStore[row]), bytesOfRow));
            }
        }
        auto isApart = [&](LValue distance) {
            return m_out.aboveOrEqual(m_out.sub(distance, m_out.constInt64(1)), bytesOfRow);
        };
        auto distanceOf = [&](size_t first, unsigned row, size_t second, unsigned later) {
            int64_t between = (nest.streams[second].offset - nest.streams[first].offset) * static_cast<int64_t>(sizeof(double));
            return m_out.add(m_out.sub(nest.bases[later][second], nest.bases[row][first]), m_out.constInt64(between));
        };
        LValue spacing = nullptr;
        if (nest.rowsAreEvenlySpaced) {
            spacing = m_out.sub(nest.baseOfStore[1], nest.baseOfStore[0]);
            require(m_out.greaterThan(spacing, m_out.constInt64(0)));
            for (size_t place = 0; place < nest.streams.size(); ++place) {
                for (unsigned row = 1; row < nest.rows; ++row) {
                    if (row > 1 || !nest.streams[place].isStore)
                        require(m_out.equal(m_out.sub(nest.bases[row][place], nest.bases[row - 1][place]), spacing));
                }
            }
        }
        auto forEachPairWithStore = [&](auto&& function) {
            for (size_t first = 0; first < nest.streams.size(); ++first) {
                for (size_t second = 0; second < nest.streams.size(); ++second) {
                    if (nest.streams[first].isStore || nest.streams[second].isStore)
                        function(first, second);
                }
            }
        };
        if (spacing) {
            LValue furthest = m_out.mul(spacing, m_out.constInt64(nest.rows - 2));
            forEachPairWithStore([&](size_t first, size_t second) {
                LValue distance = distanceOf(first, 0, second, 1);
                require(isApart(distance));
                LBasicBlock isBehind = m_out.newBlock();
                LBasicBlock isNotBehind = m_out.newBlock();
                m_out.branch(m_out.greaterThan(m_out.add(distance, spacing), bytesOfRow), normalEdge(isBehind), normalEdge(isNotBehind));
                m_out.appendTo(isNotBehind);
                m_out.branch(m_out.lessThanOrEqual(m_out.add(distance, furthest), m_out.constInt64(0)), normalEdge(isBehind), normalEdge(isNoRow));
                m_out.appendTo(isBehind);
            });
        } else {
            for (unsigned row = 0; row < nest.rows; ++row) {
                for (unsigned later = row + 1; later < nest.rows; ++later) {
                    forEachPairWithStore([&](size_t first, size_t second) {
                        require(isApart(distanceOf(first, row, second, later)));
                    });
                }
            }
        }
    }

    auto lagOf = [&](unsigned row) -> int64_t { return nest.rowsAreConsecutive ? row : 0; };
    if (nest.rowsAreConsecutive) {
        SetForScope<const OperationContext*> whileAddressing(m_operation, &context);
        for (unsigned row = 1; row < nest.rows; ++row) {
            LValue lag = m_out.constInt64(-lagOf(row) * static_cast<int64_t>(sizeof(double)));
            for (LValue& base : nest.bases[row])
                base = m_out.add(base, lag);
            nest.baseOfStore[row] = m_out.add(nest.baseOfStore[row], lag);
        }
    }
    nest.kept = m_proc.addStackSlot((nest.rows - 1) * mostStepsOfRows * sizeof(double));
    LBasicBlock undo = m_out.newBlock();
    LValue stepsToUndo = m_out.phiIn(undo, Machine::Int64);
    Values phisOfUndo;
    for (size_t index = 0; index < parameters.size(); ++index)
        phisOfUndo.append(phisOfHead[index] ? m_out.phiIn(undo, phisOfHead[index]->type()) : nullptr);
    auto isCarried = [&](size_t index) {
        return phisOfHead[index] && index != indexOfCounter && !isStreamed(index);
    };
    Vector<std::pair<ValueID, LValue>, 8> storages;
    for (const ElementStream& stream : nest.streams)
        storages.append({ stream.storage, m_values[stream.storage.index()] });
    Vector<LBasicBlock, 8> blocksOfWrittenTurn;
    for (BlockID block : turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);
    m_headOfTurnsLaidOut = head;
    m_turnLaidOut = turn;
    m_valuesLeftOutOfTurnsLaidOut.append(loop.poll);
    m_listsAskedForKindInFrontOfTurnsLaidOut = loop.listsAskedForKind.span();
    m_relationsAskedInFrontOfTurnsLaidOut = loop.relationsAskedInFront;
    m_relationsAskedInFrontOfTurnsLaidOut.appendVector(loop.relationsOfCounterBelowBound);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = loop.accessesUnderCounterBelowBound.span();
    m_interleaving = &nest;
    auto counterAt = [&](LValue iterations) {
        LValue counter = phisOfHead[indexOfCounter];
        return m_out.add(counter, counter->type() == Machine::Int32 ? m_out.castToInt32(iterations) : iterations);
    };
    Vector<Values, mostRows> carried;
    for (unsigned row = 0; row < nest.rows; ++row) {
        Values values;
        for (size_t index = 0; index < parameters.size(); ++index)
            values.append(isCarried(index) ? entries[row][index] : nullptr);
        carried.append(WTF::move(values));
    }
    Vector<LValue, mostRows> numbersStored;
    numbersStored.fill(nullptr, nest.rows);
    auto emitStep = [&](LValue step, unsigned firstRow, unsigned endOfRows) {
        nest.step = step;
        Vector<LValue, mostRows> numbersStoredBefore = numbersStored;
        Values carriedByFirstRow = carried[0];
        for (unsigned row = firstRow; row < endOfRows; ++row) {
            nest.row = row;
            nest.numberPassedOn = row && nest.rowsAreConsecutive ? numbersStoredBefore[row - 1] : nullptr;
            LValue iteration = lagOf(row) ? m_out.sub(step, m_out.constInt64(lagOf(row))) : step;
            for (BlockID block : turn) {
                for (ValueID value : code().valuesOf(block))
                    m_values[value.index()] = nullptr;
            }
            for (size_t place = 0; place < saved.size(); ++place)
                m_values[saved[place].first.index()] = outerValuesOfRows[row][place];
            for (size_t index = 0; index < parameters.size(); ++index)
                m_values[parameters[index].index()] = !phisOfHead[index] ? nullptr : index == indexOfCounter ? counterAt(iteration) : isStreamed(index) ? step : carried[row][index];
            LBasicBlock blockLeavingTurn = nullptr;
            m_blockLeavingTurnLaidOut = [&] {
                if (blockLeavingTurn)
                    return blockLeavingTurn;
                LBasicBlock here = m_out.m_block;
                blockLeavingTurn = m_out.newBlock();
                m_out.appendTo(blockLeavingTurn);
                unsigned firstUndone = std::max(firstRow, 1u);
                if (row > firstUndone) {
                    ValueContext store(code().blockOf(nest.store), nest.store, code().at(nest.store), nullptr);
                    {
                        SetForScope<const OperationContext*> whileUndoing(m_operation, &store);
                        declareAccessesOf(store);
                        for (unsigned undone = row; undone-- > firstUndone;)
                            m_out.storeDouble(m_out.loadDouble(keptElement(nest, undone, step)), elements.doubleElement(nest.baseOfStore[undone], step));
                    }
                    declareAccessesOf(*m_operation);
                }
                LValue iterations = firstRow ? numberOfIterations : row ? m_out.add(step, m_out.constInt64(1)) : step;
                const Values& valuesOfFirstRow = row ? carried[0] : carriedByFirstRow;
                m_out.upsilon(step, stepsToUndo);
                for (size_t index = 0; index < parameters.size(); ++index) {
                    if (!phisOfHead[index])
                        continue;
                    LValue value = index == indexOfCounter ? counterAt(iterations) : isStreamed(index) ? m_out.add(phisOfHead[index], iterations) : valuesOfFirstRow[index];
                    m_out.upsilon(value, phisOfUndo[index]);
                }
                m_out.jump(normalEdge(undo));
                m_out.appendTo(here);
                return blockLeavingTurn;
            };
            for (size_t index = 0; index < turn.size(); ++index) {
                LBasicBlock& block = m_blocks[turn[index].index()];
                if (!index) {
                    block = m_out.m_block;
                    m_nextBlockGoesOnHere = true;
                } else
                    block = goesOnBehindJump(loop.positionsOfTurn[index]) ? nullptr : m_out.newBlock();
            }
            for (BlockID block : turn) {
                generateBlock(block);
                if (m_hasFailed)
                    return false;
                m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
            }
            RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
            m_nextBlockGoesOnHere = false;
            numbersStored[row] = nest.numberStored;
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (isCarried(index))
                    carried[row][index] = m_argumentsForNextTurn[index].value;
            }
        }
        m_out.setOrigin(Machine::Origin());
        return true;
    };

    unsigned firstStepAcrossRows = nest.rowsAreConsecutive ? nest.rows - 1 : 0;
    for (unsigned step = 0; step < firstStepAcrossRows; ++step) {
        if (!emitStep(m_out.constInt64(step), 0, step + 1))
            return;
    }
    LValue step = m_out.phiIn(rows, Machine::Int64);
    Vector<Values, mostRows> phisOfRows;
    Vector<LValue, mostRows> phisOfNumbersStored;
    phisOfNumbersStored.fill(nullptr, nest.rows);
    auto passToRows = [&](LValue nextStep) {
        for (unsigned row = 0; row < nest.rows; ++row) {
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (phisOfRows[row][index])
                    m_out.upsilon(carried[row][index], phisOfRows[row][index]);
            }
            if (phisOfNumbersStored[row])
                m_out.upsilon(numbersStored[row], phisOfNumbersStored[row]);
        }
        m_out.upsilon(nextStep, step);
        m_out.jump(normalEdge(rows));
    };
    auto indexOfCarriedNumber = [&](unsigned row) {
        return static_cast<size_t>(std::ranges::find(carried[row], numbersStored[row]) - carried[row].begin());
    };
    for (unsigned row = 0; row < nest.rows; ++row) {
        Values phis;
        for (size_t index = 0; index < parameters.size(); ++index)
            phis.append(isCarried(index) ? m_out.phiIn(rows, phisOfHead[index]->type()) : nullptr);
        phisOfRows.append(WTF::move(phis));
    }
    size_t indexOfNumberStored = firstStepAcrossRows ? indexOfCarriedNumber(0) : parameters.size();
    bool numberStoredIsCarried = indexOfNumberStored < parameters.size();
    for (unsigned row = 0; row + 1 < nest.rows && firstStepAcrossRows && !numberStoredIsCarried; ++row)
        phisOfNumbersStored[row] = m_out.phiIn(rows, Machine::Double);
    passToRows(m_out.constInt64(firstStepAcrossRows));

    m_out.appendTo(rows);
    for (unsigned row = 0; row < nest.rows; ++row) {
        carried[row] = phisOfRows[row];
        numbersStored[row] = numberStoredIsCarried ? phisOfRows[row][indexOfNumberStored] : phisOfNumbersStored[row];
    }
    if (!emitStep(step, 0, nest.rows))
        return;
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    LValue nextStep = m_out.add(step, m_out.constInt64(1));
    m_out.branch(m_out.lessThan(nextStep, numberOfIterations), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    passToRows(nextStep);
    m_out.appendTo(ended);
    for (unsigned behind = 0; behind < firstStepAcrossRows; ++behind) {
        if (!emitStep(m_out.add(numberOfIterations, m_out.constInt64(behind)), behind + 1, nest.rows))
            return;
    }
    for (auto& [value, lowered] : saved)
        m_values[value.index()] = lowered;
    m_interleaving = nullptr;
    m_headOfTurnsLaidOut = BlockID();
    m_turnLaidOut = { };
    m_listsAskedForKindInFrontOfTurnsLaidOut = { };
    m_relationsAskedInFrontOfTurnsLaidOut.shrink(0);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = { };
    m_blockLeavingTurnLaidOut = nullptr;
    m_valuesLeftOutOfTurnsLaidOut.shrink(0);
    for (auto [value, lowered] : storages)
        m_values[value.index()] = lowered;
    for (size_t index = 0; index < outerParameters.size(); ++index)
        m_out.upsilon(outerParametersAfterVersions[index], m_values[outerParameters[index].index()]);
    m_out.jump(normalEdge(blockFor(nest.outerHead)));

    m_out.appendTo(undo);
    {
        SetForScope<const OperationContext*> whileUndoing(m_operation, &context);
        LBasicBlock undoOne = m_out.newBlock();
        LBasicBlock isUndone = m_out.newBlock();
        LValue left = m_out.phiIn(undoOne, Machine::Int64);
        LBasicBlock someAreLeft = m_out.newBlock();
        m_out.branch(m_out.equal(stepsToUndo, m_out.constInt64(0)), normalEdge(isUndone), normalEdge(someAreLeft));
        m_out.appendTo(someAreLeft);
        m_out.upsilon(stepsToUndo, left);
        m_out.jump(normalEdge(undoOne));
        m_out.appendTo(undoOne);
        LValue last = m_out.sub(left, m_out.constInt64(1));
        for (unsigned row = nest.rows; row-- > 1;) {
            LBasicBlock hasNotRun = nullptr;
            if (lagOf(row)) {
                LBasicBlock hasRun = m_out.newBlock();
                hasNotRun = m_out.newBlock();
                m_out.branch(m_out.below(m_out.sub(last, m_out.constInt64(lagOf(row))), numberOfIterations), normalEdge(hasRun), normalEdge(hasNotRun));
                m_out.appendTo(hasRun);
            }
            m_out.storeDouble(m_out.loadDouble(keptElement(nest, row, last)), elements.doubleElement(nest.baseOfStore[row], last));
            if (hasNotRun) {
                m_out.jump(normalEdge(hasNotRun));
                m_out.appendTo(hasNotRun);
            }
        }
        LBasicBlock goesOn = m_out.newBlock();
        m_out.branch(m_out.equal(last, m_out.constInt64(0)), normalEdge(isUndone), normalEdge(goesOn));
        m_out.appendTo(goesOn);
        m_out.upsilon(last, left);
        m_out.jump(normalEdge(undoOne));
        m_out.appendTo(isUndone);
        passToSingle([&](size_t index) { return phisOfUndo[index]; });
    }

    for (BlockID block : turn) {
        for (ValueID value : code().valuesOf(block))
            m_values[value.index()] = nullptr;
    }
    for (size_t index = 0; index < turn.size(); ++index)
        m_blocks[turn[index].index()] = blocksOfWrittenTurn[index];
    m_blocks[head.index()] = single;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfSingle[index];
}

static constexpr int64_t bytesBetweenConflictingLines = 4096;
static constexpr size_t storesPerPass = 6;
static constexpr size_t fewestStoresDistributed = 8;
static constexpr unsigned mostOperationsRepeated = 64;
static constexpr int64_t mostIterationsWithoutPoll = 1 << 20;

static constexpr int64_t largestStepOfStores = int64_t { 1 } << 32;
static constexpr int64_t storesOfIteration = 4;

std::optional<CodeGenerator::SteppedStore> CodeGenerator::findSteppedStore(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    if (m_keepsValuesInHeapFrame || code().handlerOf(head) || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock() || blockOfHead.terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    SteppedStore found { .positionOfHead = positionOfHead };
    std::span<const ValueID> parameters = blockOfHead.parameters();
    static constexpr OpcodeSet passingOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    auto passed = [&](ValueID value) {
        while (passingOn.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        return value;
    };
    ValueID test = blockOfHead.terminator.conditionOperand();
    const Operation& comparison = code().at(test);
    if (code().blockOf(test) != head || (comparison.opcode != Opcode::Less && comparison.opcode != Opcode::LessEq) || comparison.form() != OperatorForm::Integers)
        return std::nullopt;
    found.relation = comparison.opcode;
    found.counter = passed(comparison.leftOperand());
    found.bound = comparison.rightOperand();
    size_t indexOfCounter = std::ranges::find(parameters, found.counter) - parameters.begin();
    const Edge& intoTurn = code().at(blockOfHead.terminator.taken());
    if (indexOfCounter == parameters.size() || !intoTurn.arguments().empty())
        return std::nullopt;
    auto isInteger = [&](ValueID value) {
        return representationOf(value) == Representation::Int32 || representationOf(value) == Representation::Int64;
    };
    auto isIterated = [&](ValueID value) {
        BlockID home = code().blockOf(value);
        return home && (home == head || found.turn.contains(home));
    };

    static constexpr size_t mostBlocks = 4;
    const Edge* back = nullptr;
    BlockID from = head;
    for (BlockID next = intoTurn.to; !back;) {
        size_t position = positionOfHead + 1 + found.turn.size();
        if (found.turn.size() == mostBlocks || position >= m_order.size() || m_order[position] != next || m_singlePredecessors[next.index()] != from)
            return std::nullopt;
        const Block& block = code().at(next);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(next) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency || !block.parameters().empty() || block.terminator.kind != TerminatorKind::Jump)
            return std::nullopt;
        found.turn.append(next);
        const Edge& edge = code().at(block.terminator.next());
        if (edge.to == head)
            back = &edge;
        else if (!edge.arguments().empty())
            return std::nullopt;
        from = std::exchange(next, edge.to);
    }
    if (!isInteger(found.counter) || !isInteger(found.bound) || isIterated(found.bound) || back->arguments().size() != parameters.size())
        return std::nullopt;
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (index != indexOfCounter && m_values[parameters[index].index()] && (isCarriedBoxed(parameters[index]) || passed(back->arguments()[index]) != parameters[index]))
            return std::nullopt;
    }
    found.counterPassedOn = back->arguments()[indexOfCounter];
    ValueID stepped = passed(found.counterPassedOn);
    const Operation& addition = code().at(stepped);
    if (isCarriedBoxed(found.counter) || !isIterated(stepped) || representationOf(found.counterPassedOn) != representationOf(found.counter))
        return std::nullopt;
    if (addition.opcode == Opcode::Add && addition.form() == OperatorForm::Integers) {
        found.step = passed(addition.leftOperand()) == found.counter ? addition.rightOperand() : addition.leftOperand();
        if (passed(addition.leftOperand()) != found.counter && passed(addition.rightOperand()) != found.counter)
            return std::nullopt;
        const Type& type = typeOf(found.step);
        if (isIterated(found.step) || !isInteger(found.step) || !type.isInteger() || type.integers.maximum > largestStepOfStores || type.integers.maximum < 1)
            return std::nullopt;
    } else if (addition.opcode != Opcode::Increment || addition.form() != OperatorForm::Integers || passed(addition.valueOperand()) != found.counter)
        return std::nullopt;

    auto take = [&](ValueID value) {
        const Operation& operation = code().at(value);
        if (operation.opcode == Opcode::LoopHint) {
            if (found.poll || operation.throwsOwedError())
                return false;
            found.poll = value;
            return true;
        }
        if (value == test)
            return true;
        if (operation.opcode == Opcode::PutArrayElement || operation.opcode == Opcode::PutTypedArrayElement) {
            if (found.store || isIterated(operation.valueOperand()) || passed(operation.positionOperand()) != found.counter || !isInteger(operation.positionOperand()))
                return false;
            found.store = value;
        } else if (value != stepped && (!passingOn.contains(operation.opcode) || !isInteger(value)))
            return false;
        found.values.append(value);
        return true;
    };
    if (!std::ranges::all_of(code().valuesOf(head), take))
        return std::nullopt;
    for (BlockID block : found.turn) {
        if (!std::ranges::all_of(code().valuesOf(block), take))
            return std::nullopt;
    }
    if (!found.store)
        return std::nullopt;
    const Operation& store = code().at(found.store);
    const Type& stored = typeOf(store.valueOperand());
    if (stored.hasCells())
        return std::nullopt;
    if (store.opcode == Opcode::PutTypedArrayElement) {
        bool isLaidOutOtherwise = !found.step || code().isConstant(found.step);
        if (isLaidOutOtherwise || !store.isInsideView() || store.typedArrayType() == NotTypedArray || isIterated(store.viewOperand()))
            return std::nullopt;
        return found;
    }
    bool isOfBoxes = store.arrayStorage() == ArrayStorage::Contiguous ? !stored.isNumber() || stored.isInt32() : store.arrayStorage() == ArrayStorage::Int32 && stored.isInt32();
    if (!isOfBoxes || isIterated(store.arrayOperand()) || representationOf(store.valueOperand()) != Representation::JSValue || !isProvenToBeList(m_inputs.types, typeIDOf(store.arrayOperand())))
        return std::nullopt;
    return found;
}

void CodeGenerator::layOutSteppedStore(const SteppedStore& loop)
{
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    using Values = Vector<LValue, 8>;
    LBasicBlock front = m_out.newBlock();
    LBasicBlock iteration = m_out.newBlock();
    LBasicBlock writtenHead = m_out.newBlock();
    LBasicBlock written = m_out.newBlock();
    Values phisOfHead;
    Values phisOfWrittenHead;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phisOfHead.append(phi);
        phisOfWrittenHead.append(phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
    }
    LType typeOfCounter = phisOfHead[indexOfCounter]->type();
    LValue counter = m_out.phiIn(front, typeOfCounter);
    LValue first = m_out.phiIn(iteration, typeOfCounter);
    auto wide = [&](LValue value) { return value->type() == Machine::Int32 ? m_out.signExt32To64(value) : value; };
    auto passToWritten = [&](LValue counterBehind) {
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phisOfHead[index])
                m_out.upsilon(index == indexOfCounter ? counterBehind : phisOfHead[index], phisOfWrittenHead[index]);
        }
        m_out.jump(normalEdge(writtenHead));
    };
    const Operation& store = code().at(loop.store);
    bool isIntoList = store.opcode == Opcode::PutArrayElement;
    const ArrayStorageForm& form = arrayStorageFormOf(ArrayStorage::Contiguous);
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    TerminatorContext context(head, blockOfHead.terminator, nullptr);

    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    m_out.upsilon(phisOfHead[indexOfCounter], counter);
    m_out.jump(normalEdge(front));

    m_out.appendTo(front);
    LValue lastStart = nullptr;
    LValue end = nullptr;
    std::optional<ArrayElements> elementsOfList;
    std::optional<Operand> box;
    {
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        auto require = [&](LValue condition) {
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(condition, normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        };
        LValue step = loop.step ? wide(get(loop.step).value) : m_out.constInt64(1);
        LValue firstBehind = wide(get(loop.bound).value);
        if (loop.relation == Opcode::LessEq)
            firstBehind = m_out.add(firstBehind, m_out.constInt64(1));
        if (typeOf(loop.bound).integers.maximum >= std::numeric_limits<int32_t>::max())
            require(m_out.lessThanOrEqual(firstBehind, m_out.constInt64(typeOfCounter == Machine::Int32 ? std::numeric_limits<int32_t>::max() : int64_t { 1 } << 53)));
        if (loop.step && typeOf(loop.step).integers.minimum < 1)
            require(m_out.greaterThan(step, m_out.constInt64(0)));
        if (typeOf(loop.counter).integers.minimum < 0)
            require(m_out.greaterThanOrEqual(counter, m_out.constantOfType(typeOfCounter, 0)));
        lastStart = m_out.sub(firstBehind, m_out.mul(step, m_out.constInt64(storesOfIteration - 1)));
        require(m_out.lessThan(wide(counter), lastStart));
        if (isIntoList) {
            require(elements.listTakesBox(store.arrayOperand(), store.valueOperand()).value());
            elementsOfList = elements.elementsOfList(store.arrayOperand());
            require(m_out.lessThanOrEqual(firstBehind, elements.publicLength(*elementsOfList).value()));
            box = elements.rawElementToStore(form, store.valueOperand(), nullptr);
            RELEASE_ASSERT(box);
        }
        LValue behindStretch = m_out.add(wide(counter), m_out.mul(step, m_out.constInt64(mostIterationsWithoutPoll)));
        end = m_out.select(m_out.lessThan(behindStretch, lastStart), behindStretch, lastStart);
        if (typeOfCounter == Machine::Int32)
            end = m_out.castToInt32(end);
        m_out.upsilon(counter, first);
        m_out.jump(normalEdge(iteration));
    }

    m_out.appendTo(iteration);
    LValue next = first;
    {
        SetForScope<BlockID> whileLayingOut(m_headOfTurnsLaidOut, head);
        for (int64_t number = 0; number < storesOfIteration; ++number) {
            m_values[loop.counter.index()] = next;
            for (ValueID value : loop.values)
                m_values[value.index()] = nullptr;
            for (ValueID value : loop.values) {
                const Operation& operation = code().at(value);
                if (value != loop.store || !isIntoList) {
                    generateValue(code().blockOf(value), value, operation, false, { });
                    continue;
                }
                ValueContext contextOfStore(operation.block(), value, operation, nullptr);
                SetForScope<const OperationContext*> whileStoring(m_operation, &contextOfStore);
                m_out.setOrigin(machineOriginOf(value));
                declareAccessesOf(contextOfStore);
                LValue index = next->type() == Machine::Int32 ? m_out.extendWordThatIsNotNegative(next) : next;
                elements.storeRawElement(form, *elementsOfList, Integer(index), *box);
                m_out.clearDeclaredAccesses();
            }
            next = get(loop.counterPassedOn).value;
        }
        m_out.setOrigin(Machine::Origin());
    }
    LBasicBlock again = m_out.newBlock();
    LBasicBlock stretchHasEnded = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, end), normalEdge(again), normalEdge(stretchHasEnded));
    m_out.appendTo(again);
    m_out.upsilon(next, first);
    m_out.jump(normalEdge(iteration));

    m_out.appendTo(stretchHasEnded);
    for (ValueID value : loop.values)
        m_values[value.index()] = nullptr;
    LBasicBlock polls = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    {
        SetForScope<const OperationContext*> whileEnding(m_operation, &context);
        m_out.branch(m_out.lessThan(wide(next), lastStart), unlikelyEdge(polls), normalEdge(ended));
        m_out.appendTo(polls);
        if (loop.poll) {
            generateValue(code().blockOf(loop.poll), loop.poll, code().at(loop.poll), false, { });
            m_out.setOrigin(Machine::Origin());
        }
        m_out.upsilon(next, counter);
        m_out.jump(normalEdge(front));
    }
    m_out.appendTo(ended);
    passToWritten(next);
    m_out.appendTo(written);
    passToWritten(counter);

    m_blocks[head.index()] = writtenHead;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfWrittenHead[index];
}

std::optional<CodeGenerator::LoopDistribution> CodeGenerator::findLoopDistribution(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    if (m_keepsValuesInHeapFrame || blockOfHead.frequency != BlockFrequency::Normal || code().handlerOf(head) || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock() || blockOfHead.terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    LoopDistribution found { .positionOfHead = positionOfHead };
    ValueID test = blockOfHead.terminator.conditionOperand();
    for (ValueID value : code().valuesOf(head)) {
        if (value == test)
            continue;
        if (opcodeOf(value) != Opcode::LoopHint || found.poll)
            return std::nullopt;
        found.poll = value;
    }
    std::span<const ValueID> parameters = blockOfHead.parameters();
    const Operation& comparison = code().at(test);
    if (code().blockOf(test) != head || (comparison.opcode != Opcode::Less && comparison.opcode != Opcode::LessEq) || comparison.form() != OperatorForm::Integers)
        return std::nullopt;
    found.relation = comparison.opcode;
    found.counter = comparison.leftOperand();
    found.bound = comparison.rightOperand();
    auto isIterated = [&](ValueID value) {
        BlockID home = code().blockOf(value);
        return home && (home == head || found.turn.contains(home));
    };
    size_t indexOfCounter = std::ranges::find(parameters, found.counter) - parameters.begin();
    if (indexOfCounter == parameters.size() || representationOf(found.counter) != Representation::Int32 || representationOf(found.bound) != Representation::Int32)
        return std::nullopt;
    if (std::ranges::any_of(parameters, [&](ValueID parameter) { return isCarriedBoxed(parameter); }))
        return std::nullopt;

    static constexpr size_t mostBlocks = 4;
    const Edge& intoTurn = code().at(blockOfHead.terminator.taken());
    const Edge* back = nullptr;
    BlockID from = head;
    BlockID next = intoTurn.to;
    if (!intoTurn.arguments().empty())
        return std::nullopt;
    while (!back) {
        size_t position = positionOfHead + 1 + found.turn.size();
        if (found.turn.size() == mostBlocks || position >= m_order.size() || m_order[position] != next || m_singlePredecessors[next.index()] != from)
            return std::nullopt;
        const Block& block = code().at(next);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(next) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency || !block.parameters().empty() || block.terminator.kind != TerminatorKind::Jump)
            return std::nullopt;
        found.turn.append(next);
        const Edge& edge = code().at(block.terminator.next());
        if (edge.to == head) {
            back = &edge;
            break;
        }
        if (!edge.arguments().empty())
            return std::nullopt;
        from = std::exchange(next, edge.to);
    }
    if (isIterated(found.bound) || back->arguments().size() != parameters.size())
        return std::nullopt;

    auto passed = [&](ValueID value) {
        static constexpr OpcodeSet passingOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
        while (passingOn.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        return value;
    };
    auto integerAt = [&](ValueID value) -> std::optional<int64_t> {
        if (!code().isConstant(value) || opcodeOf(value) != Opcode::Integer)
            return std::nullopt;
        return code().at(value).integer();
    };
    const Operation& step = code().at(passed(back->arguments()[indexOfCounter]));
    bool isStepped = step.opcode == Opcode::Increment ? passed(step.valueOperand()) == found.counter : step.opcode == Opcode::Add && step.form() == OperatorForm::Integers && passed(step.leftOperand()) == found.counter && integerAt(step.rightOperand()).value_or(0) > 0;
    if (!isStepped)
        return std::nullopt;

    struct Store {
        ValueID id;
        int64_t first { 0 };
        int64_t last { 0 };
    };
    auto positionsOf = [&](Store& store, ValueID key) {
        ValueID counter = key;
        key = passed(key);
        const Operation& made = code().at(key);
        int64_t offset = 0;
        bool isUnion = made.opcode == Opcode::BitOr;
        if (isIterated(key) && (isUnion || made.opcode == Opcode::Add) && made.form() == OperatorForm::Integers) {
            std::optional<int64_t> constant = integerAt(made.leftOperand());
            counter = constant ? made.rightOperand() : made.leftOperand();
            if (!constant)
                constant = integerAt(made.rightOperand());
            if (!constant)
                return false;
            offset = *constant;
        } else
            isUnion = false;
        if (passed(counter) != found.counter || representationOf(counter) != Representation::Int32)
            return false;
        Range range = typeOf(counter).integers;
        if (range.minimum < 0 || range.maximum - range.minimum >= mostIterationsWithoutPoll)
            return false;
        if (isUnion && (offset < 0 || offset & static_cast<int64_t>(std::bit_ceil(static_cast<uint64_t>(range.maximum) + 1) - 1)))
            return false;
        store.first = offset + range.minimum;
        store.last = offset + range.maximum;
        return true;
    };
    Vector<Store, 16> stores;
    ValueID view;
    TypedArrayType typedArrayType { NotTypedArray };
    unsigned numberOfOperations = 0;
    for (BlockID block : found.turn) {
        for (ValueID value : code().valuesOf(block)) {
            const Operation& operation = code().at(value);
            if (operation.opcode == Opcode::PutTypedArrayElement) {
                const Type& stored = typeOf(operation.valueOperand());
                if (operation.typedArrayType() == NotTypedArray || stored.hasObjects() || stored.primitives & cellPrimitives || (view && (operation.viewOperand() != view || operation.typedArrayType() != typedArrayType)))
                    return std::nullopt;
                view = operation.viewOperand();
                typedArrayType = operation.typedArrayType();
                Store store { value };
                if (!positionsOf(store, operation.positionOperand()))
                    return std::nullopt;
                stores.append(store);
                continue;
            }
            if (!operation.traits().isPure() || isNeverType(operation.type) || ++numberOfOperations > mostOperationsRepeated)
                return std::nullopt;
        }
    }
    if (stores.size() < fewestStoresDistributed || isIterated(view))
        return std::nullopt;
    std::ranges::stable_sort(stores, { }, &Store::first);
    for (size_t index = 1; index < stores.size(); ++index) {
        if (stores[index].first <= stores[index - 1].last || (stores[index].first - stores[0].first) * static_cast<int64_t>(elementSize(typedArrayType)) % bytesBetweenConflictingLines)
            return std::nullopt;
    }

    size_t numberOfPasses = (stores.size() + storesPerPass - 1) / storesPerPass;
    for (size_t pass = 0; pass < numberOfPasses; ++pass) {
        Vector<ValueID, 16> values;
        auto append = [&](ValueID value) {
            values.append(value);
            for (ValueID operand : { code().at(value).positionOperand(), code().at(value).valueOperand() }) {
                if (isIterated(operand) && code().useCountOf(operand) == 1 && !std::ranges::contains(parameters, operand))
                    values.append(operand);
            }
        };
        for (size_t index = stores.size() * pass / numberOfPasses; index < stores.size() * (pass + 1) / numberOfPasses; ++index)
            append(stores[index].id);
        found.passes.append(WTF::move(values));
    }
    return found;
}

std::span<const ValueID> CodeGenerator::valuesOfLastPass(std::span<const ValueID> values, Vector<ValueID>& kept) const
{
    Vector<ValueID> ofLastPass;
    for (ValueID value : values) {
        if (!m_valuesOfEarlierPasses.contains(value))
            ofLastPass.append(value);
    }
    if (ofLastPass.size() == values.size())
        return values;
    kept = WTF::move(ofLastPass);
    return kept.span();
}

void CodeGenerator::distributeLoop(const LoopDistribution& loop)
{
    build().statistics().count(StatisticID::LoopsDistributed);
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    using Values = Vector<LValue, 8>;
    LBasicBlock lastPass = m_out.newBlock();
    Values phisOfHead;
    Values phisOfLastPass;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phisOfHead.append(phi);
        phisOfLastPass.append(phi ? m_out.phiIn(lastPass, phi->type()) : nullptr);
    }
    Vector<LBasicBlock, 4> blocksOfWrittenTurn;
    for (BlockID block : loop.turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);

    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    TerminatorContext context(head, blockOfHead.terminator, nullptr);
    for (size_t pass = 0; pass + 1 < loop.passes.size(); ++pass) {
        LBasicBlock iterations = m_out.newBlock();
        LBasicBlock iteration = m_out.newBlock();
        LBasicBlock behind = m_out.newBlock();
        Values phis;
        for (LValue phi : phisOfHead)
            phis.append(phi ? m_out.phiIn(iterations, phi->type()) : nullptr);
        auto passOn = [&](auto&& valueAt) {
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (phis[index])
                    m_out.upsilon(valueAt(index), phis[index]);
            }
            m_out.jump(normalEdge(iterations));
        };
        passOn([&](size_t index) { return phisOfHead[index]; });
        m_out.appendTo(iterations);
        {
            SetForScope<const OperationContext*> whileTesting(m_operation, &context);
            m_out.branch(askCounter(loop.relation, phis[indexOfCounter], get(loop.bound).value), normalEdge(iteration), normalEdge(behind));
        }
        m_out.appendTo(iteration);
        for (size_t index = 0; index < parameters.size(); ++index)
            m_values[parameters[index].index()] = phis[index];
        for (BlockID block : loop.turn) {
            for (ValueID value : code().valuesOf(block))
                m_values[value.index()] = nullptr;
        }
        for (size_t other = 0; other < loop.passes.size(); ++other) {
            if (other != pass)
                m_valuesLeftOutOfTurnsLaidOut.appendVector(loop.passes[other]);
        }
        m_headOfTurnsLaidOut = head;
        m_turnLaidOut = loop.turn.span();
        for (size_t index = 0; index < loop.turn.size(); ++index) {
            LBasicBlock& block = m_blocks[loop.turn[index].index()];
            if (!index) {
                block = m_out.m_block;
                m_nextBlockGoesOnHere = true;
            } else
                block = goesOnBehindJump(loop.positionOfHead + 1 + index) ? nullptr : m_out.newBlock();
        }
        for (BlockID block : loop.turn) {
            generateBlock(block);
            if (m_hasFailed)
                return;
            m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
        }
        RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
        m_nextBlockGoesOnHere = false;
        m_headOfTurnsLaidOut = BlockID();
        m_turnLaidOut = { };
        m_valuesLeftOutOfTurnsLaidOut.shrink(0);
        m_out.setOrigin(Machine::Origin());
        passOn([&](size_t index) { return m_argumentsForNextTurn[index].value; });
        m_out.appendTo(behind);
        m_valuesOfEarlierPasses.appendVector(loop.passes[pass]);
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (phisOfLastPass[index])
            m_out.upsilon(phisOfHead[index], phisOfLastPass[index]);
    }
    m_out.jump(normalEdge(lastPass));
    for (BlockID block : loop.turn) {
        for (ValueID value : code().valuesOf(block))
            m_values[value.index()] = nullptr;
        m_scopeAtEndOfBlock[block.index()] = ScopeFork();
    }
    for (size_t index = 0; index < loop.turn.size(); ++index)
        m_blocks[loop.turn[index].index()] = blocksOfWrittenTurn[index];
    m_blocks[head.index()] = lastPass;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfLastPass[index];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
