/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCollectionIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTCompileUnit.h"
#include "AOTConversionEmitter.h"
#include "AOTHashTableBuiltinEmitter.h"
#include "AOTResultRules.h"
#include "AOTResultsJoin.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTThrowEmitter.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "VM.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(OBJECT_OFFSETOF(JSMap, m_storage) == OBJECT_OFFSETOF(JSSet, m_storage));
static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex() && JSMap::Helper::deletedEntryCountIndex() == JSSet::Helper::deletedEntryCountIndex());
static_assert(JSMap::Helper::dataTableStartIndex(JSMap::Helper::LargeCapacity) == JSMap::Helper::hashTableStartIndex() + JSMap::Helper::LargeCapacity);
static_assert(static_cast<unsigned>(JSMapIterator::Field::Storage) == static_cast<unsigned>(JSSetIterator::Field::Storage));
static_assert(static_cast<unsigned>(SetNextResult::HasEntry) == static_cast<unsigned>(MapNextResult::HasEntry) && static_cast<unsigned>(SetNextResult::Entry) == static_cast<unsigned>(MapNextResult::Entry) && static_cast<unsigned>(SetNextResult::Element) == static_cast<unsigned>(MapNextResult::Key));
static_assert(static_cast<unsigned>(MapNextResult::Value) == numberOfEnumerators<SetNextResult> && numberOfEnumerators<MapNextResult> == numberOfEnumerators<SetNextResult> + 1);
static_assert(!static_cast<unsigned>(MapNextResult::HasEntry) && static_cast<unsigned>(MapNextResult::Entry) == 1 && static_cast<unsigned>(MapNextResult::Key) == 2);
static_assert(numberOfEnumerators<MapNextResult> == 4);

namespace {

struct CollectionTableEntry {
    Boxed key;
    Boxed value;
};

using CollectionStepJoin = ResultsJoin<Condition, Word32, Boxed, Boxed, Boxed>;

} // anonymous namespace

class CollectionIntrinsicEmitter::TableReader final : public OperationEmitter {
public:
    explicit TableReader(CodeGenerator& generator) : OperationEmitter(generator) { }

    LValue storageOf(CellPointer owner) { return m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), owner.value(), JSMap::offsetOfStorage())); }
    TypedPointer tableField(LValue table, ptrdiff_t offset) { return m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), table, offset); }

    LValue closedMark() { return m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, vm().value(), VM::offsetOfOrderedHashTableSentinel())); }

    LValue keysOf(LValue table)
    {
        LValue keysShift = m_out.bitAnd(m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfKeysShift())), CompactTable::keysShiftMask);
        LValue sizeOfIndex = m_out.shl(m_out.constInt32(2), keysShift);
        return m_out.add(table, m_out.add(m_out.zeroExtPtr(sizeOfIndex), CompactTable::offsetOfIndex()));
    }

    TypedPointer elementAt(LValue base, LValue index, unsigned shift)
    {
        return m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), m_out.add(base, m_out.shl(index, m_out.constInt32(shift))));
    }

    Boxed decodeNarrow(NarrowReference word)
    {
        Condition isReference = isCellReferenceAmongAllWords(word);
        Boxed cell = boxCell(decode(word));
        Boxed immediate(m_out.zeroExtPtr(word.value()));
        return select(isReference, cell, immediate);
    }

    Boxed tableOrNull(LValue storage) { return Boxed(m_out.select(m_out.notNull(storage), storage, m_out.constInt64(JSValue::encode(jsNull())))); }

    LValue loadKeyWord(CompactTable::Width keyWidth, LValue keys, LValue entryIndex)
    {
        if (keyWidth == CompactTable::Width::Wide)
            return m_out.load64(elementAt(keys, entryIndex, 3));
        return m_out.load32(elementAt(keys, entryIndex, 2));
    }

    LValue isLiveKeyWord(CompactTable::Width keyWidth, LValue word) { return keyWidth == CompactTable::Width::Wide ? m_out.notZero64(word) : m_out.notZero32(word); }

    Boxed decodeKey(CompactTable::Width keyWidth, LValue word)
    {
        if (keyWidth == CompactTable::Width::Wide)
            return Boxed(word);
        return decodeNarrow(NarrowReference(word));
    }

    struct AliveEntry {
        LValue index { nullptr };
        LValue keyWord { nullptr };
    };

    AliveEntry firstAliveEntry(std::optional<CellPointer> ownerOfWalkFromFront, CompactTable::Width keyWidth, LValue keys, LValue start, LValue used, LBasicBlock hasNoEntry)
    {
        std::optional<TypedPointer> deletedPrefix;
        if (ownerOfWalkFromFront)
            deletedPrefix = m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), ownerOfWalkFromFront->value(), JSMap::offsetOfDeletedPrefix());
        LBasicBlock isFirstUsed = m_out.newBlock();
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock isUsed = m_out.newBlock();
        LBasicBlock found = m_out.newBlock();
        m_out.branch(m_out.below(start, used), normalEdge(isFirstUsed), unlikelyEdge(hasNoEntry));

        m_out.appendTo(isFirstUsed);
        LValue wordAtStart = loadKeyWord(keyWidth, keys, start);
        ValueFromBlock aliveAtStart = m_out.anchor(start);
        ValueFromBlock aliveWordAtStart = m_out.anchor(wordAtStart);
        ValueFromBlock deletedAtStart = deletedPrefix ? ValueFromBlock() : m_out.anchor(start);
        LValue beginsInDeletedPrefix = nullptr;
        LBasicBlock isDeleted = deletedPrefix ? m_out.newBlock() : loop;
        m_out.branch(isLiveKeyWord(keyWidth, wordAtStart), normalEdge(found), unlikelyEdge(isDeleted));
        if (deletedPrefix) {
            m_out.appendTo(isDeleted);
            LValue known = m_out.zeroExtPtr(m_out.load32(*deletedPrefix));
            beginsInDeletedPrefix = m_out.belowOrEqual(start, known);
            deletedAtStart = m_out.anchor(m_out.select(m_out.below(start, known), m_out.sub(known, m_out.constInt64(1)), start));
            m_out.jump(loop);
        }

        m_out.appendTo(loop);
        LValue deletedEntry = m_out.phi(Machine::Int64, deletedAtStart);
        LValue candidate = m_out.add(deletedEntry, m_out.constInt64(1));
        m_out.branch(m_out.below(candidate, used), normalEdge(isUsed), unlikelyEdge(hasNoEntry));

        m_out.appendTo(isUsed);
        LValue wordOfCandidate = loadKeyWord(keyWidth, keys, candidate);
        ValueFromBlock liveAfterDeletedEntries = deletedPrefix ? ValueFromBlock() : m_out.anchor(candidate);
        ValueFromBlock liveWordAfterDeletedEntries = deletedPrefix ? ValueFromBlock() : m_out.anchor(wordOfCandidate);
        m_out.addIncomingToPhi(deletedEntry, m_out.anchor(candidate));
        LBasicBlock isBehindDeleted = deletedPrefix ? m_out.newBlock() : found;
        ValueFromBlock firstAlive;
        ValueFromBlock firstAliveWord;
        m_out.branch(isLiveKeyWord(keyWidth, wordOfCandidate), normalEdge(isBehindDeleted), normalEdge(loop));
        if (deletedPrefix) {
            LBasicBlock isFirstAlive = m_out.newBlock();
            m_out.appendTo(isBehindDeleted);
            liveAfterDeletedEntries = m_out.anchor(candidate);
            liveWordAfterDeletedEntries = m_out.anchor(wordOfCandidate);
            m_out.branch(beginsInDeletedPrefix, normalEdge(isFirstAlive), normalEdge(found));
            m_out.appendTo(isFirstAlive);
            m_out.store32(m_out.castToInt32(candidate), *deletedPrefix);
            firstAlive = m_out.anchor(candidate);
            firstAliveWord = m_out.anchor(wordOfCandidate);
            m_out.jump(found);
        }

        m_out.appendTo(found);
        AliveEntry alive;
        alive.index = m_out.phi(Machine::Int64, aliveAtStart, liveAfterDeletedEntries);
        alive.keyWord = m_out.phi(keyWidth == CompactTable::Width::Wide ? Machine::Int64 : Machine::Int32, aliveWordAtStart, liveWordAfterDeletedEntries);
        if (deletedPrefix) {
            m_out.addIncomingToPhi(alive.index, firstAlive);
            m_out.addIncomingToPhi(alive.keyWord, firstAliveWord);
        }
        return alive;
    }

    LValue valuesBehind(CompactTable::Width keyWidth, LValue table, LValue keys)
    {
        return m_out.add(keys, m_out.shl(m_out.zeroExtPtr(m_out.load32(tableField(table, CompactTable::offsetOfCapacity()))), keyWidth == CompactTable::Width::Wide ? 3 : 2));
    }

    Boxed loadValue(CompactTable::Width width, LValue values, LValue entryIndex)
    {
        if (width == CompactTable::Width::Wide)
            return Boxed(m_out.load64(elementAt(values, entryIndex, 3)));
        return decodeNarrow(NarrowReference(m_out.load32(elementAt(values, entryIndex, 2))));
    }

    Boxed valueAt(std::optional<CompactTable::Width> expectedValueWidth, LValue table, LValue values, LValue entryIndex)
    {
        LBasicBlock isNarrow = m_out.newBlock();
        LBasicBlock isWide = m_out.newBlock();
        Join<Boxed> join(m_out);
        LValue width = m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfValueWidth()));
        m_out.branch(m_out.equal(width, m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Narrow))),
            expectedValueWidth == CompactTable::Width::Wide ? unlikelyEdge(isNarrow) : normalEdge(isNarrow), expectedValueWidth == CompactTable::Width::Narrow ? unlikelyEdge(isWide) : normalEdge(isWide));

        m_out.appendTo(isNarrow);
        join.add(loadValue(CompactTable::Width::Narrow, values, entryIndex));

        m_out.appendTo(isWide);
        join.add(loadValue(CompactTable::Width::Wide, values, entryIndex));
        return join.finish();
    }

    Condition hasKeysOfWidth(CompactTable::Width keyWidth, LValue table)
    {
        static_assert(CompactTable::keysAreWide == 0x80);
        if (keyWidth == CompactTable::Width::Wide)
            return Condition(m_out.lessThan(m_out.load8SignExt32(tableField(table, CompactTable::offsetOfKeysShift())), m_out.int32Zero));
        return hasNoBits(Word32(m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfKeysShift()))), CompactTable::keysAreWide | CompactTable::keysAreInImage);
    }
};

CollectionIntrinsicEmitter::InlineWidths CollectionIntrinsicEmitter::inlineWidthsOf(Collection kind, ValueID collection)
{
    const PositionTypes& positions = PositionTypes::knowingNothing();
    TypingContext expecting { m_program, types(), positions, TypeLevel::Expected };
    constexpr uint16_t primitivesOfEitherWidth = Primitive::Undefined | Primitive::Null | Primitive::Boolean;
    const TypeID typeOfKeys = typeOfContentsRead(expecting, typeIDOf(collection), kind == Collection::Map ? ContentsSlot::Key : ContentsSlot::Element);
    const Type keys = types().at(typeOfKeys);
    InlineWidths widths;
    widths.keysAreWide = (keys.primitives & Primitive::Number) && !(keys.primitives & Primitive::AnyString);
    widths.readsKeysOfBothWidths = keys.isNever() || ((keys.primitives & Primitive::Number) && !types().at(types().withoutPrimitives(typeOfKeys, primitivesOfEitherWidth)).isNumber());
    if (kind == Collection::Set)
        return widths;

    const TypeID typeOfValues = typeOfContentsRead(expecting, typeIDOf(collection), ContentsSlot::Value);
    const Type values = types().at(types().withoutPrimitives(typeOfValues, primitivesOfEitherWidth));
    if (values.isNumber())
        widths.valuesAreWide = true;
    else if (!(values.primitives & Primitive::Number) && !types().at(typeOfValues).isNever())
        widths.valuesAreWide = false;
    return widths;
}

void CollectionIntrinsicEmitter::generateNext(Collection kind, const ValueContext& value)
{
    const ValueID collection = kind == Collection::Map ? value->mapOperand() : value->setOperand();
    CellPointer owner = in<CellPointer>(collection);
    Boxed table = in<Boxed>(value->tableOperand());
    Integer start = integerOf(value->entryOperand());
    const size_t numberOfResults = kind == Collection::Map ? numberOfEnumerators<MapNextResult> : numberOfEnumerators<SetNextResult>;
    TableReader reader(generator());
    auto addStepByOperation = [&](CollectionStepJoin& join) {
        RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
        Pointer buffer = calls.uninitializedArgumentBuffer(2);
        std::array<Operand, 4> arguments { owner, table, low32(start), buffer };
        StubCallResult step = calls.callOperationReturningPair(value, kind == Collection::Map ? RuntimeOperation::MapNext : RuntimeOperation::SetNext, arguments);
        auto resultInBuffer = [&](unsigned index) {
            return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), index * sizeof(EncodedJSValue))));
        };
        CollectionTableEntry entryFromOperation { resultInBuffer(0), kind == Collection::Map ? resultInBuffer(1) : undefined() };
        Condition hasEntry(m_out.notZero64(step.first));
        Boxed tableBehindStep = reader.tableOrNull(reader.storageOf(owner));
        join.add(hasEntry, Word32(m_out.castToInt32(step.second)), entryFromOperation.key, entryFromOperation.value, tableBehindStep);
    };

    LBasicBlock hasNoEntry = newBlock();
    LBasicBlock slowPath = newBlock();
    CollectionStepJoin join(m_out);
    const bool takesCurrentTable = opcodeOf(value->tableOperand()) == Opcode::Null;
    LValue storage = reader.storageOf(owner);
    auto loadUsed = [&] { return m_out.zeroExtPtr(m_out.load32(reader.tableField(storage, CompactTable::offsetOfUsed()))); };
    TypedPointer byteWithMark = m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), storage, CompactTable::offsetOfKeysShift());
    auto markAsWalked = [&](Word32 flagsAndMark) {
        LBasicBlock marks = newBlock();
        LBasicBlock isMarked = newBlock();
        m_out.branch(hasNoBits(flagsAndMark, CompactTable::wasWalked).value(), unlikelyEdge(marks), normalEdge(isMarked));

        m_out.appendTo(marks);
        m_out.store32As8(m_out.bitOr(flagsAndMark.value(), m_out.constInt32(CompactTable::wasWalked)), byteWithMark);
        m_out.jump(isMarked);

        m_out.appendTo(isMarked);
    };

    ValueID entryToBeginAt = definitionOf(value->entryOperand());
    while (conversionsOfForm.contains(opcodeOf(entryToBeginAt)))
        entryToBeginAt = definitionOf(operationAt(entryToBeginAt).valueOperand());
    const Operation& origin = operationAt(entryToBeginAt);
    const bool mayBeFirstStep = (origin.opcode == Opcode::Integer && !origin.integer()) || origin.opcode == Opcode::GetInternalField;
    const std::optional<CellPointer> ownerOfFirstStep = mayBeFirstStep ? std::optional { owner } : std::nullopt;
    if (mayBeFirstStep)
        build().statistics().count(StatisticID::CollectionStepsSkippingDeletedPrefix);

    LBasicBlock isCurrent = newBlock();
    LBasicBlock isCompact = newBlock();
    if (takesCurrentTable)
        m_out.branch(m_out.isNull(storage), unlikelyEdge(hasNoEntry), normalEdge(isCurrent));
    else {
        LBasicBlock isChanged = newBlock();
        m_out.branch(m_out.equal(storage, table.value()), normalEdge(isCurrent), unlikelyEdge(isChanged));

        m_out.appendTo(isChanged);
        m_out.branch(m_out.isNull(storage), normalEdge(hasNoEntry), rareEdge(slowPath));
    }

    m_out.appendTo(isCurrent);
    m_out.branch(hasCellType(CellPointer(storage), JSCellButterflyType).value(), rareEdge(slowPath), normalEdge(isCompact));

    m_out.appendTo(isCompact);
    const InlineWidths widths = inlineWidthsOf(kind, collection);
    const CompactTable::Width keyWidth = widths.keysAreWide ? CompactTable::Width::Wide : CompactTable::Width::Narrow;
    const bool readsBothWidths = widths.readsKeysOfBothWidths;
    LBasicBlock hasOtherWidth = readsBothWidths ? newBlock() : slowPath;
    LBasicBlock hasExpectedWidth = newBlock();
    m_out.branch(reader.hasKeysOfWidth(keyWidth, storage).value(), normalEdge(hasExpectedWidth), readsBothWidths ? unlikelyEdge(hasOtherWidth) : rareEdge(slowPath));

    m_out.appendTo(hasExpectedWidth);
    LValue used = loadUsed();
    LValue column = reader.keysOf(storage);
    TableReader::AliveEntry alive = reader.firstAliveEntry(ownerOfFirstStep, keyWidth, column, start.value(), used, hasNoEntry);
    LValue entryIndex = alive.index;
    Boxed key = reader.decodeKey(keyWidth, alive.keyWord);
    LValue values = nullptr;
    if (readsBothWidths) {
        const CompactTable::Width otherWidth = widths.keysAreWide ? CompactTable::Width::Narrow : CompactTable::Width::Wide;
        LBasicBlock found = newBlock();
        ValueFromBlock expectedIndex = m_out.anchor(entryIndex);
        ValueFromBlock expectedKey = m_out.anchor(key.value());
        ValueFromBlock expectedValues = kind == Collection::Map ? m_out.anchor(reader.valuesBehind(keyWidth, storage, column)) : ValueFromBlock();
        m_out.jump(found);

        m_out.appendTo(hasOtherWidth);
        branchUnless(reader.hasKeysOfWidth(otherWidth, storage), slowPath);
        LBasicBlock isUsed = newBlock();
        m_out.branch(m_out.below(start.value(), loadUsed()), normalEdge(isUsed), unlikelyEdge(hasNoEntry));
        m_out.appendTo(isUsed);
        LValue otherColumn = reader.keysOf(storage);
        LValue otherWord = reader.loadKeyWord(otherWidth, otherColumn, start.value());
        branchUnless(Condition(reader.isLiveKeyWord(otherWidth, otherWord)), slowPath);
        ValueFromBlock otherIndex = m_out.anchor(start.value());
        ValueFromBlock otherKey = m_out.anchor(reader.decodeKey(otherWidth, otherWord).value());
        ValueFromBlock otherValues = kind == Collection::Map ? m_out.anchor(reader.valuesBehind(otherWidth, storage, otherColumn)) : ValueFromBlock();
        m_out.jump(found);

        m_out.appendTo(found);
        entryIndex = m_out.phi(Machine::Int64, expectedIndex, otherIndex);
        key = Boxed(m_out.phi(Machine::Int64, expectedKey, otherKey));
        if (kind == Collection::Map)
            values = m_out.phi(Machine::Int64, expectedValues, otherValues);
    }
    markAsWalked(Word32(m_out.load8ZeroExt32(byteWithMark)));
    std::optional<CompactTable::Width> expectedValueWidth;
    if (widths.valuesAreWide)
        expectedValueWidth = *widths.valuesAreWide ? CompactTable::Width::Wide : CompactTable::Width::Narrow;
    CollectionTableEntry entry { key, undefined() };
    if (kind == Collection::Map)
        entry.value = reader.valueAt(expectedValueWidth, storage, values ? values : reader.valuesBehind(keyWidth, storage, column), entryIndex);
    Condition hasEntry = boolean(true);
    join.add(hasEntry, low32(Integer(entryIndex)), entry.key, entry.value, Boxed(storage));

    m_out.appendTo(hasNoEntry);
    Condition hasNone = boolean(false);
    Word32 noIndex = word32(0);
    Boxed noValue = undefined();
    Boxed tableWithoutEntry = reader.tableOrNull(storage);
    join.add(hasNone, noIndex, noValue, noValue, tableWithoutEntry);

    m_out.appendTo(slowPath);
    addStepByOperation(join);

    auto results = join.finish();
    defineResults(value, std::span<const Operand>(results).first(numberOfResults));

    m_forwardedTableRead = { };
    for (ValueID behind : value.operationsBehind()) {
        const Operation& next = operationAt(behind);
        if (next.opcode == Opcode::ResultAt)
            continue;
        if (next.opcode == Opcode::CollectionTable && definitionOf(next.collectionOperand()) == definitionOf(collection)) {
            m_forwardedTableRead = behind;
            m_forwardedTable = results[numberOfEnumerators<MapNextResult>].value;
        }
        break;
    }
}

void CollectionIntrinsicEmitter::generateCollectionIteratorClosedMark(const ValueContext& value)
{
    define(value, Boxed(TableReader(generator()).closedMark()));
}

void CollectionIntrinsicEmitter::generateGetCollectionIteratorTable(const ValueContext& value)
{
    LValue table = m_out.load64(m_out.address(in<CellPointer>(value->objectOperand()).value(), m_heaps.JSInternalFieldObjectImpl_internalFields[static_cast<unsigned>(JSSetIterator::Field::Storage)]));
    define(value, Boxed(m_out.select(m_out.notNull(table), table, m_out.constInt64(JSValue::encode(jsNull())))));
}

void CollectionIntrinsicEmitter::generatePutCollectionIteratorTable(const ValueContext& value)
{
    Boxed table = in<Boxed>(value->valueOperand());
    LValue stored = m_out.select(m_out.equal(table.value(), m_out.constInt64(JSValue::encode(jsNull()))), m_out.constInt64(0), table.value());
    CellPointer iterator = in<CellPointer>(value->objectOperand());
    m_out.store64(stored, m_out.address(iterator.value(), m_heaps.JSInternalFieldObjectImpl_internalFields[static_cast<unsigned>(JSSetIterator::Field::Storage)]));
    barrierAfterStore(value, iterator, typeOf(value->valueOperand()));
    define(value, emitter<ConversionEmitter>().inRepresentation(value, table, typeOf(value->valueOperand()), value.representation()));
}

void CollectionIntrinsicEmitter::generateIsCollectionIteratorClosedMark(const ValueContext& value)
{
    if (opcodeOf(value->tableOperand()) == Opcode::Null) {
        defineTest(value, boolean(false));
        return;
    }
    LValue mark = TableReader(generator()).closedMark();
    defineTest(value, Condition(m_out.equal(in<Boxed>(value->tableOperand()).value(), mark)));
}

void CollectionIntrinsicEmitter::generateCollectionTable(const ValueContext& value)
{
    TableReader reader(generator());
    CellPointer owner = in<CellPointer>(value->collectionOperand());
    if (value.id() != m_forwardedTableRead) {
        define(value, reader.tableOrNull(reader.storageOf(owner)));
        return;
    }
    Boxed table(m_forwardedTable);
    define(value, table);
}

void CollectionIntrinsicEmitter::generateGetValueAtPlaceInMap(const ValueContext& value)
{
    TableReader reader(generator());
    CellPointer map = in<CellPointer>(value->mapOperand());
    Integer place = integerOf(value->placeOperand());
    const Machine::AbstractHeap& entries = m_heaps.entire(AbstractHeap::CollectionEntries);
    Boxed found = selectByBranch<Boxed>(lessThan(place, integer(0)), [&] {
        return undefined();
    }, [&] {
        LValue table = reader.storageOf(map);
        LValue slot = m_out.add(table, place.value());
        if (checksBoundsUnder(build().options().testing) || emittersCheckProofs()) {
            emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.notNull(table)), std::nullopt, ProofSource::RememberedFact);
            Condition placeIsOfNarrowValue = hasBits(place, placeInMapHoldsNarrowValue);
            Condition isLive(m_out.equal(m_out.load8ZeroExt32(reader.tableField(table, CompactTable::offsetOfStatus())), m_out.constInt32(static_cast<int32_t>(CompactTable::Status::Live))));
            LValue widthThatPlaceSays = m_out.select(placeIsOfNarrowValue.value(), m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Narrow)), m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Wide)));
            Condition holdsValuesOfThatWidth(m_out.equal(m_out.load8ZeroExt32(reader.tableField(table, CompactTable::offsetOfValueWidth())), widthThatPlaceSays));
            LValue keys = reader.keysOf(table);
            Condition keysAreWide = reader.hasKeysOfWidth(CompactTable::Width::Wide, table);
            LValue values = m_out.select(keysAreWide.value(), reader.valuesBehind(CompactTable::Width::Wide, table, keys), reader.valuesBehind(CompactTable::Width::Narrow, table, keys));
            LValue distance = m_out.sub(slot, values);
            LValue entry = m_out.select(placeIsOfNarrowValue.value(), m_out.lShr(m_out.sub(distance, m_out.constInt64(placeInMapHoldsNarrowValue)), m_out.constInt32(2)), m_out.lShr(distance, m_out.constInt32(3)));
            Condition isInUse(m_out.below(entry, m_out.zeroExtPtr(m_out.load32(reader.tableField(table, CompactTable::offsetOfUsed())))));
            emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(logicalAnd(isLive, holdsValuesOfThatWidth), isInUse), std::nullopt, ProofSource::RememberedFact);
            emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.select(keysAreWide.value(), reader.isLiveKeyWord(CompactTable::Width::Wide, reader.loadKeyWord(CompactTable::Width::Wide, keys, entry)), reader.isLiveKeyWord(CompactTable::Width::Narrow, reader.loadKeyWord(CompactTable::Width::Narrow, keys, entry)))), std::nullopt, ProofSource::RememberedFact);
        }
        return selectByBranch<Boxed>(hasBits(place, placeInMapHoldsNarrowValue), [&] {
            return reader.decodeNarrow(NarrowReference(m_out.load32(m_out.address(entries, slot, -placeInMapHoldsNarrowValue))));
        }, [&] {
            return Boxed(m_out.load64(m_out.address(entries, slot)));
        });
    });
    define(value, emitter<ConversionEmitter>().inRepresentation(value, found, typeOf(value.id()), value.representation()));
}

void CollectionIntrinsicEmitter::generateFindKeyInMap(const ValueContext& value)
{
    define(value, emitter<HashTableBuiltinEmitter>().findPlace(value, value->mapOperand(), value->keyOperand()));
}

Integer CollectionIntrinsicEmitter::movesOfEntriesInCollections()
{
    return Integer(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), vm().value(), VM::offsetOfAOTState() + OBJECT_OFFSETOF(VMState, movesOfEntriesInCollections))));
}

void CollectionIntrinsicEmitter::generateMovesOfEntriesInCollections(const ValueContext& value)
{
    define(value, movesOfEntriesInCollections());
}

void CollectionIntrinsicEmitter::generateTryPutValueAtPlaceInMap(const ValueContext& value)
{
    TableReader reader(generator());
    CellPointer map = in<CellPointer>(value->mapOperand());
    Integer place = integerOf(value->placeOperand());
    Integer moves = integerOf(value->movesOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock holdsWideValue = newBlock();
        LBasicBlock liesWhereItWasFound = newBlock();
        m_out.branch(m_out.testIsZero64(place.value(), m_out.constInt64(std::numeric_limits<int64_t>::min() | placeInMapHoldsNarrowValue)), normalEdge(holdsWideValue), targets.notTaken);

        m_out.appendTo(holdsWideValue);
        m_out.branch(equal(moves, movesOfEntriesInCollections()).value(), normalEdge(liesWhereItWasFound), targets.notTaken);

        m_out.appendTo(liesWhereItWasFound);
        LValue table = reader.storageOf(map);
        if (checksBoundsUnder(build().options().testing) || emittersCheckProofs()) {
            emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.notNull(table)), std::nullopt, ProofSource::RememberedFact);
            Condition isLive(m_out.equal(m_out.load8ZeroExt32(reader.tableField(table, CompactTable::offsetOfStatus())), m_out.constInt32(static_cast<int32_t>(CompactTable::Status::Live))));
            Condition holdsWideValues(m_out.equal(m_out.load8ZeroExt32(reader.tableField(table, CompactTable::offsetOfValueWidth())), m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Wide))));
            LValue keys = reader.keysOf(table);
            Condition keysAreWide = reader.hasKeysOfWidth(CompactTable::Width::Wide, table);
            LValue values = m_out.select(keysAreWide.value(), reader.valuesBehind(CompactTable::Width::Wide, table, keys), reader.valuesBehind(CompactTable::Width::Narrow, table, keys));
            LValue entry = m_out.lShr(m_out.sub(m_out.add(table, place.value()), values), m_out.constInt32(3));
            Condition isInUse(m_out.below(entry, m_out.zeroExtPtr(m_out.load32(reader.tableField(table, CompactTable::offsetOfUsed())))));
            emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(logicalAnd(isLive, holdsWideValues), isInUse), std::nullopt, ProofSource::RememberedFact);
            emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.select(keysAreWide.value(), reader.isLiveKeyWord(CompactTable::Width::Wide, reader.loadKeyWord(CompactTable::Width::Wide, keys, entry)), reader.isLiveKeyWord(CompactTable::Width::Narrow, reader.loadKeyWord(CompactTable::Width::Narrow, keys, entry)))), std::nullopt, ProofSource::RememberedFact);
        }
        m_out.store64(stored.value(), m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), m_out.add(table, place.value())));
        barrierAfterStore(value, CellPointer(table), typeOf(value->valueOperand()));
        m_out.jump(targets.taken);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
