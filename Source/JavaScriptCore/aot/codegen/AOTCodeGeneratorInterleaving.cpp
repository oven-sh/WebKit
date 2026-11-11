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

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned rowsInterleaved = 4;
static constexpr int64_t mostIterationsOfRow = 160;
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
    std::optional<size_t> streamOfReadBeforeRow(ValueID) const;
    bool findStreams();
    bool findStream(ValueID access, ValueID list, ValueID storage, ValueID position, bool isStore);
    bool carriesChain() const;
    bool onlyIndexesElements(ValueID parameter) const;

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
        m_found.blocksBehindRow.append(block);
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

std::optional<size_t> InterleavingFinder::streamOfReadBeforeRow(ValueID value) const
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
            if (std::optional<size_t> stream = streamOfReadBeforeRow(value)) {
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
    for (BlockID block : nest.blocksBehindRow) {
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
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LValue old = m_out.loadDouble(elements.doubleElement(nest.baseOfStore[nest.row], nest.iteration));
    m_out.storeDouble(old, keptElement(nest, nest.row, nest.iteration));
}

TypedPointer CodeGenerator::keptElement(const Interleaving& nest, unsigned row, LValue iteration)
{
    RELEASE_ASSERT(row);
    return TypedPointer(heaps().frameSlots, m_out.add(m_out.slotBase(nest.kept), m_out.shl(iteration, 3)), (row - 1) * mostIterationsOfRow * sizeof(double));
}

void CodeGenerator::beginInterleavedOperation(ValueID id, const Operation& operation)
{
    Interleaving& nest = *m_interleaving;
    if (id == nest.store && nest.row) {
        ValueContext context(operation.block(), id, operation, nullptr);
        SetForScope<const OperationContext*> whileKeeping(m_operation, &context);
        declareAccessesOf(context);
        keepOverwrittenElement(nest);
    }
    for (const ElementStream& stream : nest.streams) {
        if (stream.access != id)
            continue;
        m_values[stream.storage.index()] = nest.bases[nest.row][&stream - nest.streams.begin()];
        return;
    }
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
    Vector<Values, rowsInterleaved> entries;
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
    Vector<Vector<LValue, 2>, rowsInterleaved> addressesReadByRows;
    addressesReadByRows.append({ });
    Vector<std::pair<ValueID, LValue>, 64> saved;
    auto save = [&](ValueID value) {
        saved.append({ value, m_values[value.index()] });
    };
    for (ValueID parameter : outerParameters)
        save(parameter);
    for (BlockID block : nest.blocksBehindRow) {
        for (ValueID value : code().valuesOf(block))
            save(value);
    }
    for (ValueID value : nest.dependentOperations)
        save(value);
    for (unsigned row = 1; row < rowsInterleaved; ++row) {
        Vector<LValue, 2> addressesRead;
        generateNextRow(nest, isNoRow, addressesRead);
        if (m_hasFailed)
            return;
        addressesReadByRows.append(WTF::move(addressesRead));
        SetForScope<const OperationContext*> whileEntering(m_operation, &contextOfEntry);
        Values entry;
        for (size_t index = 0; index < parameters.size(); ++index)
            entry.append(phisOfHead[index] ? getIn(entryArguments[index], representationOf(parameters[index])).value : nullptr);
        entries.append(WTF::move(entry));
    }
    generateEndOfRow(nest);
    Vector<LValue, 4> outerParametersBehindRows;
    {
        BlockID last = nest.blocksBehindRow.last();
        TerminatorContext contextOfEnd(last, code().at(last).terminator, nullptr);
        SetForScope<const OperationContext*> whileStepping(m_operation, &contextOfEnd);
        for (size_t index = 0; index < outerParameters.size(); ++index)
            outerParametersBehindRows.append(getIn(nest.steppedParameters[index], representationOf(outerParameters[index])).value);
    }
    for (auto [value, lowered] : saved)
        m_values[value.index()] = lowered;

    nest.bases.shrink(0);
    nest.baseOfStore.shrink(0);
    {
        SetForScope<const OperationContext*> whileAddressing(m_operation, &context);
        auto greatestIndexIsInside = [&](const SteppedIndex& held, const Values& entry) {
            LValue greatest = m_out.add(m_out.add(wide(entry[held.indexOfParameter]), held.step > 0 ? numberOfIterations : m_out.constInt64(0)), m_out.constInt64(held.offset - (held.step > 0 ? 1 : 0)));
            return m_out.lessThan(greatest, m_out.zeroExt(get(held.length).value, Machine::Int64));
        };
        for (const Values& entry : entries) {
            for (const SteppedIndex& held : loop.steppedIndexes)
                require(greatestIndexIsInside(held, entry));
            Vector<LValue, 8> bases;
            for (const ElementStream& stream : nest.streams)
                bases.append(m_out.add(get(stream.storage).value, m_out.shl(entry[stream.indexOfParameter], 3)));
            for (size_t place = 0; place < nest.streams.size(); ++place) {
                if (nest.streams[place].isStore)
                    nest.baseOfStore.append(m_out.add(bases[place], m_out.constInt64(nest.streams[place].offset * static_cast<int64_t>(sizeof(double)))));
            }
            nest.bases.append(WTF::move(bases));
        }
        LValue bytesOfRow = m_out.sub(m_out.shl(numberOfIterations, 3), m_out.constInt64(1));
        for (unsigned row = 0; row < rowsInterleaved; ++row) {
            for (unsigned later = row + 1; later < rowsInterleaved; ++later) {
                for (LValue address : addressesReadByRows[later])
                    require(m_out.above(m_out.sub(address, nest.baseOfStore[row]), bytesOfRow));
                for (size_t first = 0; first < nest.streams.size(); ++first) {
                    for (size_t second = 0; second < nest.streams.size(); ++second) {
                        if (!nest.streams[first].isStore && !nest.streams[second].isStore)
                            continue;
                        int64_t between = (nest.streams[second].offset - nest.streams[first].offset) * static_cast<int64_t>(sizeof(double));
                        LValue distance = m_out.add(m_out.sub(nest.bases[later][second], nest.bases[row][first]), m_out.constInt64(between - 1));
                        require(m_out.aboveOrEqual(distance, bytesOfRow));
                    }
                }
            }
        }
    }

    nest.kept = m_proc.addStackSlot((rowsInterleaved - 1) * mostIterationsOfRow * sizeof(double));
    LValue iteration = m_out.phiIn(rows, Machine::Int64);
    Vector<Values, rowsInterleaved> phisOfRows;
    for (unsigned row = 0; row < rowsInterleaved; ++row) {
        Values phis;
        for (size_t index = 0; index < parameters.size(); ++index)
            phis.append(phisOfHead[index] && index != indexOfCounter && !isStreamed(index) ? m_out.phiIn(rows, phisOfHead[index]->type()) : nullptr);
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phis[index])
                m_out.upsilon(entries[row][index], phis[index]);
        }
        phisOfRows.append(WTF::move(phis));
    }
    m_out.upsilon(m_out.constInt64(0), iteration);
    m_out.jump(normalEdge(rows));

    LBasicBlock undo = m_out.newBlock();
    LValue iterationsToUndo = m_out.phiIn(undo, Machine::Int64);
    Values phisOfUndo;
    for (size_t index = 0; index < parameters.size(); ++index)
        phisOfUndo.append(phisOfHead[index] ? m_out.phiIn(undo, phisOfHead[index]->type()) : nullptr);

    m_out.appendTo(rows);
    Vector<std::pair<ValueID, LValue>, 8> storages;
    for (const ElementStream& stream : nest.streams)
        storages.append({ stream.storage, m_values[stream.storage.index()] });
    Vector<LBasicBlock, 8> blocksOfWrittenTurn;
    for (BlockID block : turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);
    m_headOfTurnsLaidOut = head;
    m_turnLaidOut = turn;
    if (loop.poll && code().blockOf(loop.poll) != head)
        m_valuesLeftOutOfTurnsLaidOut.append(loop.poll);
    m_listsAskedForKindInFrontOfTurnsLaidOut = loop.listsAskedForKind.span();
    m_relationsAskedInFrontOfTurnsLaidOut = loop.relationsAskedInFront;
    m_relationsAskedInFrontOfTurnsLaidOut.appendVector(loop.relationsOfCounterBelowBound);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = loop.accessesUnderCounterBelowBound.span();
    auto counterAt = [&](LValue iterations) {
        LValue counter = phisOfHead[indexOfCounter];
        return m_out.add(counter, counter->type() == Machine::Int32 ? m_out.castToInt32(iterations) : iterations);
    };
    Vector<Values, rowsInterleaved> valuesBehind;
    m_interleaving = &nest;
    nest.iteration = iteration;
    for (unsigned row = 0; row < rowsInterleaved; ++row) {
        nest.row = row;
        for (BlockID block : turn) {
            for (ValueID value : code().valuesOf(block))
                m_values[value.index()] = nullptr;
        }
        for (size_t index = 0; index < parameters.size(); ++index) {
            ValueID parameter = parameters[index];
            m_values[parameter.index()] = !phisOfHead[index] ? nullptr : index == indexOfCounter ? counterAt(iteration) : isStreamed(index) ? iteration : phisOfRows[row][index];
        }
        LBasicBlock blockLeavingTurn = nullptr;
        m_blockLeavingTurnLaidOut = [&] {
            if (blockLeavingTurn)
                return blockLeavingTurn;
            LBasicBlock here = m_out.m_block;
            blockLeavingTurn = m_out.newBlock();
            m_out.appendTo(blockLeavingTurn);
            if (row > 1) {
                ValueContext store(code().blockOf(nest.store), nest.store, code().at(nest.store), nullptr);
                {
                    SetForScope<const OperationContext*> whileUndoing(m_operation, &store);
                    declareAccessesOf(store);
                    for (unsigned undone = row; undone-- > 1;)
                        m_out.storeDouble(m_out.loadDouble(keptElement(nest, undone, iteration)), elements.doubleElement(nest.baseOfStore[undone], iteration));
                }
                declareAccessesOf(*m_operation);
            }
            LValue iterations = row ? m_out.add(iteration, m_out.constInt64(1)) : iteration;
            m_out.upsilon(iteration, iterationsToUndo);
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (!phisOfHead[index])
                    continue;
                LValue value = index == indexOfCounter ? counterAt(iterations) : isStreamed(index) ? m_out.add(phisOfHead[index], iterations) : row ? valuesBehind[0][index] : phisOfRows[0][index];
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
                return;
            m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
        }
        RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
        Values behind;
        for (size_t index = 0; index < parameters.size(); ++index)
            behind.append(phisOfRows[row][index] ? m_argumentsForNextTurn[index].value : nullptr);
        valuesBehind.append(WTF::move(behind));
    }
    m_interleaving = nullptr;
    m_nextBlockGoesOnHere = false;
    m_headOfTurnsLaidOut = BlockID();
    m_turnLaidOut = { };
    m_listsAskedForKindInFrontOfTurnsLaidOut = { };
    m_relationsAskedInFrontOfTurnsLaidOut.shrink(0);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = { };
    m_blockLeavingTurnLaidOut = nullptr;
    m_valuesLeftOutOfTurnsLaidOut.shrink(0);
    for (auto [value, lowered] : storages)
        m_values[value.index()] = lowered;
    m_out.setOrigin(Machine::Origin());

    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    LValue iterationBehind = m_out.add(iteration, m_out.constInt64(1));
    m_out.branch(m_out.lessThan(iterationBehind, numberOfIterations), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    for (unsigned row = 0; row < rowsInterleaved; ++row) {
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phisOfRows[row][index])
                m_out.upsilon(valuesBehind[row][index], phisOfRows[row][index]);
        }
    }
    m_out.upsilon(iterationBehind, iteration);
    m_out.jump(normalEdge(rows));
    m_out.appendTo(ended);
    for (size_t index = 0; index < outerParameters.size(); ++index)
        m_out.upsilon(outerParametersBehindRows[index], m_values[outerParameters[index].index()]);
    m_out.jump(normalEdge(blockFor(nest.outerHead)));

    m_out.appendTo(undo);
    {
        SetForScope<const OperationContext*> whileUndoing(m_operation, &context);
        LBasicBlock undoOne = m_out.newBlock();
        LBasicBlock isUndone = m_out.newBlock();
        LValue left = m_out.phiIn(undoOne, Machine::Int64);
        LBasicBlock someAreLeft = m_out.newBlock();
        m_out.branch(m_out.equal(iterationsToUndo, m_out.constInt64(0)), normalEdge(isUndone), normalEdge(someAreLeft));
        m_out.appendTo(someAreLeft);
        m_out.upsilon(iterationsToUndo, left);
        m_out.jump(normalEdge(undoOne));
        m_out.appendTo(undoOne);
        LValue last = m_out.sub(left, m_out.constInt64(1));
        for (unsigned row = rowsInterleaved; row-- > 1;)
            m_out.storeDouble(m_out.loadDouble(keptElement(nest, row, last)), elements.doubleElement(nest.baseOfStore[row], last));
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
