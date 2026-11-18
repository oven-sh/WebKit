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

    LValue flagsOf(LValue table) { return m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfKeysShift())); }

    LValue keysOf(LValue table, LValue flags)
    {
        LValue sizeOfIndex = m_out.shl(m_out.constInt32(2), m_out.bitAnd(flags, CompactTable::keysShiftMask));
        return m_out.add(table, m_out.zeroExtPtr(sizeOfIndex));
    }

    TypedPointer elementAt(LValue column, LValue index, unsigned shift)
    {
        return m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), m_out.add(column, m_out.shl(index, m_out.constInt32(shift))), CompactTable::offsetOfIndex());
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

    Integer placeOfValue(LValue table, LValue values, LValue entryIndex)
    {
        LValue start = m_out.add(m_out.sub(values, table), m_out.constInt64(CompactTable::offsetOfIndex()));
        LValue isWide = m_out.equal(m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfValueWidth())), m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Wide)));
        LValue wide = m_out.add(start, m_out.shl(entryIndex, m_out.constInt32(3)));
        LValue narrow = m_out.add(m_out.add(start, m_out.shl(entryIndex, m_out.constInt32(2))), m_out.constInt64(placeInMapHoldsNarrowValue));
        return Integer(m_out.select(isWide, wide, narrow));
    }

    static constexpr int32_t kindMask = static_cast<uint8_t>(~CompactTable::keysShiftMask);
    static constexpr int32_t walkedTableKind(CompactTable::Width keyWidth) { return CompactTable::wasWalked | (keyWidth == CompactTable::Width::Wide ? CompactTable::keysAreWide : 0); }

    Condition isWalkedWithKeysOfWidth(CompactTable::Width keyWidth, LValue kind) { return Condition(m_out.equal(kind, m_out.constInt32(walkedTableKind(keyWidth)))); }

    Condition hasWideKeys(LValue flags) { return hasBits(Word32(flags), CompactTable::keysAreWide); }
};

CollectionIntrinsicEmitter::InlineWidths CollectionIntrinsicEmitter::inlineWidthsOf(Collection kind, const ValueContext& step, ValueID collection)
{
    const PositionTypes& positions = PositionTypes::knowingNothing();
    TypingContext expecting { m_program, types(), positions, TypeLevel::Expected };
    constexpr uint16_t primitivesOfEitherWidth = Primitive::Undefined | Primitive::Null | Primitive::Boolean;
    bool isNarrowedByResults = false;
    auto typeOfResult = [&](MapNextResult result, TypeID type) {
        forEachResultRead(step, scopedLambda<void(ValueID)>([&](ValueID read) {
            TypeID typeRead = typeIDOf(read);
            if (operationAt(read).index() != static_cast<uint32_t>(result) || types().at(typeRead).isNever())
                return;
            if (type != typeRead && (types().at(type).isNever() || types().isSubset(typeRead, type))) {
                type = typeRead;
                isNarrowedByResults = true;
            }
        }));
        return type;
    };
    static_assert(static_cast<uint32_t>(MapNextResult::Key) == static_cast<uint32_t>(SetNextResult::Element));
    const TypeID typeOfKeys = typeOfResult(MapNextResult::Key, typeOfContentsRead(expecting, typeIDOf(collection), kind == Collection::Map ? ContentsSlot::Key : ContentsSlot::Element));
    const Type keys = types().at(typeOfKeys);
    InlineWidths widths;
    widths.keysAreWide = (keys.primitives & Primitive::Number) && !(keys.primitives & Primitive::AnyString);
    widths.readsKeysOfBothWidths = keys.isNever() || ((keys.primitives & Primitive::Number) && !types().at(types().withoutPrimitives(typeOfKeys, primitivesOfEitherWidth)).isNumber());
    if (isNarrowedByResults && !widths.readsKeysOfBothWidths)
        build().statistics().count(StatisticID::CollectionWidthsByResults);
    if (kind == Collection::Set)
        return widths;

    const TypeID typeOfValues = typeOfResult(MapNextResult::Value, typeOfContentsRead(expecting, typeIDOf(collection), ContentsSlot::Value));
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
    const bool takesTableReadHere = takesTableReadBeforeStep(value, collection);
    const std::optional<BlockEdge> end = generator().edgeAfterLastStep(value, static_cast<uint32_t>(MapNextResult::HasEntry), CodeGenerator::StepTest::HasEntry);
    const InlineWidths widths = inlineWidthsOf(kind, value, collection);
    const CompactTable::Width keyWidth = widths.keysAreWide ? CompactTable::Width::Wide : CompactTable::Width::Narrow;
    const CompactTable::Width otherWidth = widths.keysAreWide ? CompactTable::Width::Narrow : CompactTable::Width::Wide;
    const bool readsBothWidths = widths.readsKeysOfBothWidths;
    LValue carriedKeys = generator().keysOfCarriedTable(value->tableOperand());
    if (carriedKeys && !generator().claimKeysOfCarriedTable(value->tableOperand(), { widths.keysAreWide, readsBothWidths }))
        carriedKeys = nullptr;
    constexpr int64_t otherWidthTag = 1;
    static_assert(!(CompactTable::offsetOfIndex() & otherWidthTag));
    Vector<ValueFromBlock, 4> tableKeysAfterStep;
    auto addKeysOfTable = [&](LValue keys) {
        if (carriedKeys)
            tableKeysAfterStep.append(m_out.anchor(keys));
    };
    const bool readsValue = kind == Collection::Map && mayReadValue(value);
    auto addStepByOperation = [&](CollectionStepJoin& join) {
        RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
        Pointer buffer = calls.uninitializedArgumentBuffer(2);
        std::array<Operand, 4> arguments { owner, takesTableReadHere ? reader.tableOrNull(reader.storageOf(owner)) : table, low32(start), buffer };
        StubCallResult step = calls.callOperationReturningPair(value, kind == Collection::Map ? RuntimeOperation::MapNext : RuntimeOperation::SetNext, arguments);
        auto resultInBuffer = [&](unsigned index) {
            return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), index * sizeof(EncodedJSValue))));
        };
        Condition hasEntry(m_out.notZero64(step.first));
        if (end) {
            LBasicBlock goesOn = newBlock();
            branch(hasEntry, { normalEdge(goesOn), *end });
            m_out.appendTo(goesOn);
        }
        CollectionTableEntry entryFromOperation { resultInBuffer(0), readsValue ? resultInBuffer(1) : undefined() };
        Boxed tableBehindStep = reader.tableOrNull(reader.storageOf(owner));
        addKeysOfTable(m_out.constInt64(0));
        join.add(hasEntry, Word32(m_out.castToInt32(step.second)), entryFromOperation.key, entryFromOperation.value, tableBehindStep);
    };

    LBasicBlock hasNoEntry = end ? end->block() : newBlock();
    LBasicBlock slowPath = newBlock();
    CollectionStepJoin join(m_out);
    const bool takesCurrentTable = opcodeOf(value->tableOperand()) == Opcode::Null || takesTableReadHere;
    LValue storage = reader.storageOf(owner);
    auto loadUsed = [&] { return m_out.zeroExtPtr(m_out.load32(reader.tableField(storage, CompactTable::offsetOfUsed()))); };
    TypedPointer byteWithMark = m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), storage, CompactTable::offsetOfKeysShift());

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
    LBasicBlock hasExpectedWidth = newBlock();
    LBasicBlock hasCarriedOtherWidth = carriedKeys && readsBothWidths ? newBlock() : nullptr;
    ValueFromBlock keysCheckedBefore;
    if (carriedKeys) {
        LBasicBlock isUnchecked = newBlock();
        if (hasCarriedOtherWidth) {
            build().statistics().count(StatisticID::CarriedKeysTaggedByWidth);
            LBasicBlock isChecked = newBlock();
            m_out.branch(m_out.notZero64(carriedKeys), normalEdge(isChecked), unlikelyEdge(isUnchecked));
            m_out.appendTo(isChecked);
            keysCheckedBefore = m_out.anchor(carriedKeys);
            m_out.branch(m_out.testNonZero64(carriedKeys, m_out.constInt64(otherWidthTag)), unlikelyEdge(hasCarriedOtherWidth), normalEdge(hasExpectedWidth));
        } else {
            keysCheckedBefore = m_out.anchor(carriedKeys);
            m_out.branch(m_out.notZero64(carriedKeys), normalEdge(hasExpectedWidth), unlikelyEdge(isUnchecked));
        }
        m_out.appendTo(isUnchecked);
    }
    m_out.branch(hasCellType(CellPointer(storage), JSCellButterflyType).value(), rareEdge(slowPath), normalEdge(isCompact));

    m_out.appendTo(isCompact);
    LBasicBlock hasOtherWidth = readsBothWidths ? newBlock() : slowPath;
    LBasicBlock hasCheckedWidth = carriedKeys ? newBlock() : hasExpectedWidth;
    LBasicBlock hasOtherKind = newBlock();
    LBasicBlock isNotInImage = newBlock();
    LBasicBlock marks = newBlock();
    LBasicBlock isMarked = newBlock();
    LValue flags = m_out.load8ZeroExt32(byteWithMark);
    LValue kindOfTable = m_out.bitAnd(flags, TableReader::kindMask);
    m_out.branch(reader.isWalkedWithKeysOfWidth(keyWidth, kindOfTable).value(), normalEdge(hasCheckedWidth), unlikelyEdge(hasOtherKind));

    m_out.appendTo(hasOtherKind);
    if (readsBothWidths) {
        LBasicBlock hasNeitherKind = newBlock();
        m_out.branch(reader.isWalkedWithKeysOfWidth(otherWidth, kindOfTable).value(), normalEdge(hasOtherWidth), unlikelyEdge(hasNeitherKind));
        m_out.appendTo(hasNeitherKind);
    }
    m_out.branch(hasBits(Word32(flags), CompactTable::keysAreInImage).value(), rareEdge(slowPath), unlikelyEdge(isNotInImage));

    m_out.appendTo(isNotInImage);
    m_out.branch(hasNoBits(Word32(flags), CompactTable::wasWalked).value(), unlikelyEdge(marks), unlikelyEdge(isMarked));

    m_out.appendTo(marks);
    m_out.store32As8(m_out.bitOr(flags, m_out.constInt32(CompactTable::wasWalked)), byteWithMark);
    m_out.jump(isMarked);

    m_out.appendTo(isMarked);
    LBasicBlock whereWide = widths.keysAreWide ? hasCheckedWidth : hasOtherWidth;
    LBasicBlock whereNarrow = widths.keysAreWide ? hasOtherWidth : hasCheckedWidth;
    auto edgeToWidth = [&](LBasicBlock to) { return to == slowPath ? rareEdge(to) : unlikelyEdge(to); };
    m_out.branch(reader.hasWideKeys(flags).value(), edgeToWidth(whereWide), edgeToWidth(whereNarrow));

    ValueFromBlock keysCheckedHere;
    if (carriedKeys) {
        m_out.appendTo(hasCheckedWidth);
        keysCheckedHere = m_out.anchor(reader.keysOf(storage, flags));
        m_out.jump(hasExpectedWidth);
    }

    m_out.appendTo(hasExpectedWidth);
    LValue column = carriedKeys ? m_out.phi(Machine::Int64, keysCheckedBefore, keysCheckedHere) : nullptr;
    LValue used = loadUsed();
    if (!column)
        column = reader.keysOf(storage, flags);
    auto requireCheckedKeys = [&](CompactTable::Width width, LValue checkedColumn) {
        if (!carriedKeys || !emittersCheckProofs())
            return;
        LValue flagsNow = m_out.load8ZeroExt32(byteWithMark);
        Condition hasExpectedKind = reader.isWalkedWithKeysOfWidth(width, m_out.bitAnd(flagsNow, TableReader::kindMask));
        Condition isCompactTable = logicalNot(hasCellType(CellPointer(storage), JSCellButterflyType));
        emitter<ThrowEmitter>().requireOrCrash(value, isCompactTable, std::nullopt, ProofSource::RememberedFact);
        Condition isKeyColumn(m_out.equal(checkedColumn, reader.keysOf(storage, flagsNow)));
        emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(hasExpectedKind, isKeyColumn), std::nullopt, ProofSource::RememberedFact);
    };
    requireCheckedKeys(keyWidth, column);
    TableReader::AliveEntry alive = reader.firstAliveEntry(ownerOfFirstStep, keyWidth, column, start.value(), used, hasNoEntry);
    LValue entryIndex = alive.index;
    Boxed key = reader.decodeKey(keyWidth, alive.keyWord);
    LValue values = nullptr;
    if (readsBothWidths) {
        LBasicBlock found = newBlock();
        ValueFromBlock expectedIndex = m_out.anchor(entryIndex);
        ValueFromBlock expectedKey = m_out.anchor(key.value());
        ValueFromBlock expectedValues = readsValue ? m_out.anchor(reader.valuesBehind(keyWidth, storage, column)) : ValueFromBlock();
        ValueFromBlock expectedColumn = carriedKeys ? m_out.anchor(column) : ValueFromBlock();
        m_out.jump(found);

        m_out.appendTo(hasOtherWidth);
        LValue otherColumn = reader.keysOf(storage, flags);
        LValue taggedColumn = nullptr;
        if (carriedKeys) {
            LBasicBlock readsOtherWidth = newBlock();
            ValueFromBlock otherCheckedHere = m_out.anchor(m_out.add(otherColumn, m_out.constInt64(otherWidthTag)));
            m_out.jump(readsOtherWidth);
            m_out.appendTo(hasCarriedOtherWidth);
            ValueFromBlock otherCheckedBefore = m_out.anchor(carriedKeys);
            m_out.jump(readsOtherWidth);
            m_out.appendTo(readsOtherWidth);
            taggedColumn = m_out.phi(Machine::Int64, otherCheckedHere, otherCheckedBefore);
            otherColumn = m_out.sub(taggedColumn, m_out.constInt64(otherWidthTag));
            requireCheckedKeys(otherWidth, otherColumn);
        }
        LBasicBlock isUsed = newBlock();
        m_out.branch(m_out.below(start.value(), loadUsed()), normalEdge(isUsed), unlikelyEdge(hasNoEntry));
        m_out.appendTo(isUsed);
        LValue otherWord = reader.loadKeyWord(otherWidth, otherColumn, start.value());
        branchUnless(Condition(reader.isLiveKeyWord(otherWidth, otherWord)), slowPath);
        ValueFromBlock otherIndex = m_out.anchor(start.value());
        ValueFromBlock otherKey = m_out.anchor(reader.decodeKey(otherWidth, otherWord).value());
        ValueFromBlock otherValues = readsValue ? m_out.anchor(reader.valuesBehind(otherWidth, storage, otherColumn)) : ValueFromBlock();
        ValueFromBlock otherKeys = carriedKeys ? m_out.anchor(taggedColumn) : ValueFromBlock();
        m_out.jump(found);

        m_out.appendTo(found);
        entryIndex = m_out.phi(Machine::Int64, expectedIndex, otherIndex);
        key = Boxed(m_out.phi(Machine::Int64, expectedKey, otherKey));
        if (readsValue)
            values = m_out.phi(Machine::Int64, expectedValues, otherValues);
        if (carriedKeys)
            column = m_out.phi(Machine::Int64, expectedColumn, otherKeys);
    }
    std::optional<CompactTable::Width> expectedValueWidth;
    if (widths.valuesAreWide)
        expectedValueWidth = *widths.valuesAreWide ? CompactTable::Width::Wide : CompactTable::Width::Narrow;
    CollectionTableEntry entry { key, undefined() };
    if (readsValue)
        entry.value = reader.valueAt(expectedValueWidth, storage, values ? values : reader.valuesBehind(keyWidth, storage, column), entryIndex);
    Condition hasEntry = boolean(true);
    addKeysOfTable(column);
    join.add(hasEntry, low32(Integer(entryIndex)), entry.key, entry.value, takesCurrentTable ? Boxed(storage) : table);

    if (!end) {
        m_out.appendTo(hasNoEntry);
        Condition hasNone = boolean(false);
        Word32 noIndex = word32(0);
        Boxed noValue = undefined();
        Boxed tableWithoutEntry = reader.tableOrNull(storage);
        addKeysOfTable(m_out.constInt64(0));
        join.add(hasNone, noIndex, noValue, noValue, tableWithoutEntry);
    }

    m_out.appendTo(slowPath);
    addStepByOperation(join);

    auto results = join.finish();
    if (end)
        results[static_cast<size_t>(MapNextResult::HasEntry)] = hasEntry;
    defineResults(value, std::span<const Operand>(results).first(numberOfResults));

    m_forwardedTableRead = findTableReadAfterStep(value, collection);
    if (end && m_forwardedTableRead && m_code.blockOf(m_forwardedTableRead) == m_code.at(m_code.at(value.block()).terminator.notTaken()).to)
        m_forwardedTableRead = { };
    m_forwardedTable = results[numberOfEnumerators<MapNextResult>].value;
    if (carriedKeys) {
        LValue keys = m_out.phi(Machine::Int64, tableKeysAfterStep);
        if (m_forwardedTableRead)
            generator().noteKeysOfCheckedTable(m_forwardedTableRead, keys);
    }
}

uint32_t CollectionIntrinsicEmitter::forEachResultRead(const ValueContext& step, const ScopedLambda<void(ValueID)>& functor)
{
    uint32_t partsLeft = m_code.useCountOf(step.id());
    auto lookAt = [&](ValueID reader) {
        const Operation& part = operationAt(reader);
        if (part.opcode != Opcode::ResultAt || part.resultsOperand() != step.id())
            return;
        --partsLeft;
        functor(reader);
    };
    std::ranges::for_each(step.operationsBehind(), lookAt);
    const Terminator& terminator = m_code.at(step.block()).terminator;
    for (uint32_t index = 0; index < terminator.numberOfEdges; ++index) {
        BlockID to = m_code.at(terminator.edge(index)).to;
        if (m_generator.onlyPredecessorOf(to) == step.block())
            std::ranges::for_each(m_code.valuesOf(to), lookAt);
    }
    return partsLeft;
}

bool CollectionIntrinsicEmitter::mayReadValue(const ValueContext& step)
{
    bool readsValue = false;
    uint32_t readElsewhere = forEachResultRead(step, scopedLambda<void(ValueID)>([&](ValueID result) {
        readsValue |= operationAt(result).index() == static_cast<uint32_t>(MapNextResult::Value);
    }));
    return readsValue || readElsewhere;
}

bool CollectionIntrinsicEmitter::takesTableReadBeforeStep(const ValueContext& step, ValueID collection)
{
    const ValueID table = definitionOf(step->tableOperand());
    if (opcodeOf(table) != Opcode::CollectionTable || definitionOf(operationAt(table).collectionOperand()) != definitionOf(collection))
        return false;
    BlockID block = m_code.blockOf(table);
    if (block != step.block() && m_generator.onlyPredecessorOf(step.block()) != block)
        return false;
    auto touchesMemory = [&](ValueID operation) {
        EffectTraits traits = operationAt(operation).traits();
        return traits.mayTouchMemory() || traits.mayRunOtherCode() || traits.mayCollect();
    };
    std::span<const ValueID> afterRead = m_code.valuesOf(block);
    afterRead = afterRead.subspan(std::ranges::find(afterRead, table) - afterRead.begin() + 1);
    std::span<const ValueID> beforeStep = block == step.block() ? afterRead : m_code.valuesOf(step.block());
    if (block != step.block() && std::ranges::any_of(afterRead, touchesMemory))
        return false;
    beforeStep = beforeStep.first(std::ranges::find(beforeStep, step.id()) - beforeStep.begin());
    return std::ranges::none_of(beforeStep, touchesMemory);
}

ValueID CollectionIntrinsicEmitter::findTableReadAfterStep(const ValueContext& step, ValueID collection)
{
    enum class Search : uint8_t { Continue, Found, Stop };
    ValueID read;
    auto searchIn = [&](std::span<const ValueID> operations) {
        for (ValueID behind : operations) {
            const Operation& next = operationAt(behind);
            if (next.opcode == Opcode::CollectionTable && definitionOf(next.collectionOperand()) == definitionOf(collection)) {
                read = behind;
                return Search::Found;
            }
            if (next.traits().mayTouchMemory() || next.traits().mayRunOtherCode() || next.traits().mayCollect())
                return Search::Stop;
        }
        return Search::Continue;
    };
    if (searchIn(step.operationsBehind()) != Search::Continue)
        return read;
    const Terminator& terminator = m_code.at(step.block()).terminator;
    for (uint32_t index = 0; index < terminator.numberOfEdges && !read; ++index) {
        BlockID to = m_code.at(terminator.edge(index)).to;
        if (m_generator.onlyPredecessorOf(to) == step.block())
            searchIn(m_code.valuesOf(to));
    }
    return read;
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
    TypedPointer field = m_out.address(iterator.value(), m_heaps.JSInternalFieldObjectImpl_internalFields[static_cast<unsigned>(JSSetIterator::Field::Storage)]);
    LBasicBlock isOther = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(m_out.equal(m_out.load64(field), stored), normalEdge(continuation), unlikelyEdge(isOther));

    m_out.appendTo(isOther);
    m_out.store64(stored, field);
    barrierAfterStore(value, iterator, typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    define(value, emitter<ConversionEmitter>().inRepresentation(value, table, typeOf(value->valueOperand()), value.representation()));
}

void CollectionIntrinsicEmitter::generateIsCollectionIteratorClosedMark(const ValueContext& value)
{
    const Opcode opcodeOfTable = opcodeOf(definitionOf(value->tableOperand()));
    if (opcodeOfTable == Opcode::Null || opcodeOfTable == Opcode::CollectionTable) {
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
            LValue flags = reader.flagsOf(table);
            LValue keys = reader.keysOf(table, flags);
            Condition keysAreWide = reader.hasWideKeys(flags);
            LValue values = m_out.select(keysAreWide.value(), reader.valuesBehind(CompactTable::Width::Wide, table, keys), reader.valuesBehind(CompactTable::Width::Narrow, table, keys));
            LValue distance = m_out.sub(m_out.sub(slot, values), m_out.constInt64(CompactTable::offsetOfIndex()));
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

bool CollectionIntrinsicEmitter::isCounterOrSmallNumber(ValueID key) const
{
    static constexpr OpcodeSet passThrough { Opcode::Box, Opcode::Unbox, Opcode::Narrowed, Opcode::WidenInteger, Opcode::IntegerToNumber };
    static constexpr OpcodeSet cuts { Opcode::BitAnd, Opcode::Mod };
    constexpr Range smallNumbers { 0, UINT16_MAX };
    auto strip = [&](ValueID value) {
        while (!m_code.isConstant(value) && passThrough.contains(opcodeOf(value)))
            value = operationAt(value).operand(0);
        return value;
    };
    const ValueID counter = strip(key);
    if (m_code.isConstant(counter))
        return false;
    if (cuts.contains(opcodeOf(counter))) {
        static constexpr OpcodeSet scattering { Opcode::Mul, Opcode::MultiplyAddIntegers, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
        const ValueID cut = strip(operationAt(counter).operand(0));
        return typeOf(key).integers.isSubsetOf(smallNumbers) && (m_code.isConstant(cut) || !scattering.contains(opcodeOf(cut)));
    }
    if (opcodeOf(counter) != Opcode::Parameter || !m_code.at(m_code.blockOf(counter)).isLoopHead)
        return false;
    const BlockID head = m_code.blockOf(counter);
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    const size_t place = std::ranges::find(parameters, counter) - parameters.begin();
    bool isStepped = false;
    m_code.forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        const ValueID next = place < edge.arguments().size() && edge.arguments()[place] ? strip(edge.arguments()[place]) : ValueID();
        isStepped |= next && !m_code.isConstant(next) && opcodeOf(next) == Opcode::Increment && strip(operationAt(next).operand(0)) == counter;
    });
    return isStepped;
}

Operand CollectionIntrinsicEmitter::findNumberAtHomeSlot(CellPointer owner, Boxed key, EntryAnswer answer, const ScopedLambda<Operand()>& lookUpByHash)
{
    build().statistics().count(StatisticID::NumbersFoundAtHomeSlots);
    TableReader reader(generator());
    LBasicBlock hasTable = newBlock();
    LBasicBlock liesInside = newBlock();
    LBasicBlock hasWideKeys = newBlock();
    LBasicBlock liesAtItsPlace = newBlock();
    LBasicBlock liesElsewhere = newBlock();
    OperandJoin join(m_out, answer == EntryAnswer::Value ? Representation::JSValue : answer == EntryAnswer::Place ? Representation::Int64 : Representation::Boolean);
    LValue table = reader.storageOf(owner);
    m_out.branch(m_out.notNull(table), normalEdge(hasTable), unlikelyEdge(liesElsewhere));

    m_out.appendTo(hasTable);
    LValue place = m_out.zeroExtPtr(m_out.castToInt32(key.value()));
    LValue used = m_out.zeroExtPtr(m_out.load32(reader.tableField(table, CompactTable::offsetOfUsed())));
    m_out.branch(m_out.below(place, used), normalEdge(liesInside), normalEdge(liesElsewhere));

    m_out.appendTo(liesInside);
    LValue flags = reader.flagsOf(table);
    m_out.branch(m_out.equal(m_out.bitAnd(flags, CompactTable::keysAreWide | CompactTable::keysAreInImage), m_out.constInt32(CompactTable::keysAreWide)), normalEdge(hasWideKeys), normalEdge(liesElsewhere));

    m_out.appendTo(hasWideKeys);
    LValue column = reader.keysOf(table, flags);
    m_out.branch(m_out.equal(reader.loadKeyWord(CompactTable::Width::Wide, column, place), key.value()), normalEdge(liesAtItsPlace), normalEdge(liesElsewhere));

    m_out.appendTo(liesAtItsPlace);
    switch (answer) {
    case EntryAnswer::IsPresent:
        join.add(boolean(true));
        break;
    case EntryAnswer::Value:
        join.add(reader.valueAt(std::nullopt, table, reader.valuesBehind(CompactTable::Width::Wide, table, column), place));
        break;
    case EntryAnswer::Place:
        join.add(reader.placeOfValue(table, reader.valuesBehind(CompactTable::Width::Wide, table, column), place));
        break;
    }

    m_out.appendTo(liesElsewhere);
    join.add(lookUpByHash());
    return join.finish();
}

void CollectionIntrinsicEmitter::generateFindKeyInMap(const ValueContext& value)
{
    define(value, emitter<HashTableBuiltinEmitter>().findPlace(value, value->mapOperand(), value->keyOperand()));
}

Integer CollectionIntrinsicEmitter::movesOfEntriesInCollections()
{
    return Integer(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), vm().value(), VM::offsetOfAOTState() + OBJECT_OFFSETOF(VMState, movesOfEntriesInCollections))));
}

static constexpr int64_t columnIsOfOtherKind = 1;
static constexpr int64_t columnIsInImage = 2;

static constexpr unsigned numberOfCachedWords = 5;

TypedPointer CollectionIntrinsicEmitter::cachedWord(LValue cache, unsigned index)
{
    return m_out.address(m_heaps.frameSlots, cache, index * sizeof(uint64_t));
}

void CollectionIntrinsicEmitter::cacheColumns(LValue cache, LValue table, KeyOfLookup key, bool readsValues)
{
    std::array<Operand, 4> columns = readColumns(table, key, readsValues);
    m_out.store64(table, cachedWord(cache, 0));
    for (unsigned column = 0; column < (readsValues ? 4 : 3); ++column)
        m_out.store64(columns[column].as<Integer>().value(), cachedWord(cache, 1 + column));
}

void CollectionIntrinsicEmitter::generateCollectionColumns(const ValueContext& value)
{
    KeyOfLookup key = value->keysAreStrings() ? KeyOfLookup::String : value->keysAreCells() ? KeyOfLookup::Cell : KeyOfLookup::Number;
    LValue table = TableReader(generator()).storageOf(in<CellPointer>(value->collectionOperand()));
    if (value->areCachedInFrame()) {
        LValue cache = m_out.lockedStackSlot(numberOfCachedWords * sizeof(uint64_t));
        cache->slot()->setIsDeadAtSuspensions(true);
        cacheColumns(cache, table, key, value->readsValues());
        defineResults(value, { Integer(cache), integer(0), integer(0), integer(0) });
        return;
    }
    defineResults(value, readColumns(table, key, value->readsValues()));
}

std::array<Operand, 4> CollectionIntrinsicEmitter::readColumns(LValue table, KeyOfLookup key, bool readsValues)
{
    TableReader reader(generator());
    const bool keysAreCells = key != KeyOfLookup::Number;
    LBasicBlock hasTable = newBlock();
    LBasicBlock hasOtherKeys = newBlock();
    LBasicBlock isInHeap = newBlock();
    LBasicBlock isNotHuge = newBlock();
    LBasicBlock isNotSearchedHere = newBlock();
    LBasicBlock hasNoTable = newBlock();
    ResultsJoin<Integer, Integer, Integer, Integer> join(m_out);
    auto indexBitsOf = [&](LValue table) { return m_out.load8SignExt32(reader.tableField(table, CompactTable::offsetOfIndexBits())); };
    auto joinColumns = [&](LValue table, bool isInImage) {
        LValue slots = m_out.shl(m_out.constInt32(1), indexBitsOf(table));
        LValue capacity = m_out.load32(reader.tableField(table, CompactTable::offsetOfCapacity()));
        LValue hasLongSlots = m_out.zeroExtPtr(m_out.above(capacity, m_out.constInt32(CompactTable::maxCapacityWithShortSlots)));
        LValue tags = m_out.add(table, m_out.constInt64(CompactTable::offsetOfIndex()));
        LValue keys = m_out.add(reader.keysOf(table, reader.flagsOf(table)), m_out.constInt64(CompactTable::offsetOfIndex()));
        LValue entries = m_out.zeroExtPtr(capacity);
        LValue values = m_out.constInt64(0);
        if (isInImage)
            keys = m_out.bitOr(m_out.add(keys, m_out.shl(entries, m_out.constInt32(2))), m_out.constInt64(columnIsInImage));
        else if (readsValues) {
            LValue isNarrow = m_out.equal(m_out.load8ZeroExt32(reader.tableField(table, CompactTable::offsetOfValueWidth())), m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Narrow)));
            values = m_out.bitOr(m_out.add(keys, m_out.shl(entries, m_out.constInt32(keysAreCells ? 2 : 3))), m_out.zeroExtPtr(isNarrow));
        }
        join.add(Integer(tags), Integer(m_out.zeroExtPtr(m_out.sub(slots, m_out.constInt32(1)))), Integer(m_out.bitOr(keys, hasLongSlots)), Integer(values));
    };
    m_out.branch(m_out.notNull(table), normalEdge(hasTable), unlikelyEdge(hasNoTable));

    m_out.appendTo(hasNoTable);
    join.add(integer(0), integer(0), integer(0), integer(0));

    m_out.appendTo(hasTable);
    LValue flags = reader.flagsOf(table);
    LValue widthInHeap = m_out.bitAnd(flags, CompactTable::keysAreWide | CompactTable::keysAreInImage);
    m_out.branch(m_out.equal(widthInHeap, m_out.constInt32(keysAreCells ? 0 : CompactTable::keysAreWide)), normalEdge(isInHeap), normalEdge(hasOtherKeys));

    m_out.appendTo(isInHeap);
    m_out.branch(m_out.greaterThan(indexBitsOf(table), m_out.constInt32(-1)), normalEdge(isNotHuge), unlikelyEdge(isNotSearchedHere));

    m_out.appendTo(isNotHuge);
    joinColumns(table, false);

    m_out.appendTo(hasOtherKeys);
    if (key == KeyOfLookup::String) {
        LBasicBlock isInImage = newBlock();
        m_out.branch(hasBits(Word32(flags), CompactTable::keysAreInImage).value(), normalEdge(isInImage), normalEdge(isNotSearchedHere));
        m_out.appendTo(isInImage);
        joinColumns(m_out.loadPtr(reader.tableField(table, CompactTable::offsetOfIndex())), true);
    } else
        m_out.jump(isNotSearchedHere);

    m_out.appendTo(isNotSearchedHere);
    join.add(Integer(table), integer(0), integer(0), integer(0));
    return join.finish();
}

static LValue trailingZerosOfNonZero(Output& out, LValue word)
{
    Machine::PatchOperation* zeros = out.patchpoint(Machine::Int64);
    zeros->appendSomeRegister(word);
    zeros->effects = Machine::Effects::none();
    zeros->setRunsWithoutFrame(true);
    zeros->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
#if CPU(X86_64)
        jit.m_assembler.tzcntq_rr(params[1].gpr(), params[0].gpr());
#else
        jit.countTrailingZeros64(params[1].gpr(), params[0].gpr());
#endif
    });
    return zeros;
}

Operand CollectionIntrinsicEmitter::lookUpByTag(CollectionColumns columns, Boxed key, const ScopedLambda<Operand()>& lookUpByHash)
{
    build().statistics().count(StatisticID::CollectionLookupsByTag);
    constexpr int64_t tops = static_cast<int64_t>(CompactTable::onesOfTags << 7);
    TableReader reader(generator());
    const Machine::AbstractHeap& storage = m_heaps.entire(AbstractHeap::CollectionStorage);
    const Machine::AbstractHeap& entriesHeap = m_heaps.entire(AbstractHeap::CollectionEntries);
    LBasicBlock hasSlots = newBlock();
    LBasicBlock hasNoSlots = newBlock();
    LBasicBlock hasFewEntries = newBlock();
    LBasicBlock isAmongFew = newBlock();
    LBasicBlock isUsed = newBlock();
    LBasicBlock hasOtherTag = newBlock();
    LBasicBlock searchesGroup = newBlock();
    LBasicBlock isInGroup = newBlock();
    LBasicBlock isNotInGroup = newBlock();
    LBasicBlock hasSameTag = newBlock();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock isFound = newBlock();
    LBasicBlock liesElsewhere = newBlock();
    LBasicBlock hasNoColumn = newBlock();
    const bool searchesToEnd = !columns.keyIsString;
    OperandJoin join(m_out, columns.readsValues ? Representation::JSValue : Representation::Boolean);
    LValue hash = nullptr;
    LValue impl = nullptr;
    if (columns.cache) {
        LBasicBlock hasOtherTable = newBlock();
        LBasicBlock hasCachedTable = newBlock();
        LValue table = reader.storageOf(*columns.collection);
        m_out.branch(m_out.equal(table, m_out.load64(cachedWord(columns.cache, 0))), normalEdge(hasCachedTable), unlikelyEdge(hasOtherTable));

        m_out.appendTo(hasOtherTable);
        cacheColumns(columns.cache, table, columns.keyIsString ? KeyOfLookup::String : columns.keysAreCells ? KeyOfLookup::Cell : KeyOfLookup::Number, columns.readsValues);
        m_out.jump(hasCachedTable);

        m_out.appendTo(hasCachedTable);
        columns.tags = m_out.load64(cachedWord(columns.cache, 1));
        columns.mask = m_out.load64(cachedWord(columns.cache, 2));
        columns.keys = m_out.load64(cachedWord(columns.cache, 3));
        if (columns.readsValues)
            columns.values = m_out.load64(cachedWord(columns.cache, 4));
    }
    std::optional<ValueFromBlock> entryAtItsPlace;
    if (columns.keyMayBeAtItsPlace) {
        build().statistics().count(StatisticID::NumbersFoundAtHomeSlots);
        LBasicBlock hasColumn = newBlock();
        LBasicBlock liesInside = newBlock();
        LBasicBlock isSought = newBlock();
        m_out.branch(m_out.notZero64(columns.keys), normalEdge(hasColumn), unlikelyEdge(hasNoColumn));

        m_out.appendTo(hasColumn);
        LValue place = m_out.zeroExtPtr(m_out.castToInt32(key.value()));
        LValue used = m_out.zeroExtPtr(m_out.load32(m_out.address(storage, columns.tags, CompactTable::offsetOfUsed() - CompactTable::offsetOfIndex())));
        m_out.branch(m_out.below(place, used), normalEdge(liesInside), normalEdge(isSought));

        m_out.appendTo(liesInside);
        LValue column = m_out.bitAnd(columns.keys, m_out.constInt64(~columnIsOfOtherKind));
        LValue word = m_out.load64(m_out.address(entriesHeap, m_out.add(column, m_out.shl(place, m_out.constInt32(3))), 0));
        entryAtItsPlace = m_out.anchor(place);
        m_out.branch(m_out.equal(word, key.value()), normalEdge(isFound), normalEdge(isSought));

        m_out.appendTo(isSought);
    }
    if (columns.keyMayBeNullish) {
        LBasicBlock hasCell = newBlock();
        m_out.branch(isCell(key).value(), normalEdge(hasCell), unlikelyEdge(liesElsewhere));
        m_out.appendTo(hasCell);
    }
    if (columns.keyIsString) {
        LBasicBlock isResolved = newBlock();
        LBasicBlock hasHash = newBlock();
        impl = m_out.loadPtr(key.value(), m_heaps.JSString_value);
        m_out.branch(m_out.testNonZeroPtr(impl, JSString::isRopeInPointer), normalEdge(liesElsewhere), normalEdge(isResolved));

        m_out.appendTo(isResolved);
        hash = m_out.lShr(m_out.load32(impl, m_heaps.StringImpl_hashAndFlags), m_out.constInt32(StringImpl::s_flagCount));
        m_out.branch(m_out.notZero32(hash), normalEdge(hasHash), unlikelyEdge(liesElsewhere));
        m_out.appendTo(hasHash);
    } else {
        LValue multiplier = loadImmutable64(AOT_INSTANCE_OFFSET(numbers.keyMultiplierOfTables)).value();
        hash = m_out.foldedProduct(multiplier, key.value());
    }
    auto bytesOf = [&](LValue byte) { return m_out.mul(m_out.zeroExtPtr(m_out.bitAnd(byte, m_out.constInt32(0xff))), m_out.constInt64(CompactTable::onesOfTags)); };
    auto placeOfLowest = [&](LValue matches) {
        return m_out.lShr(trailingZerosOfNonZero(m_out, matches), m_out.constInt32(3));
    };
    Vector<ValueFromBlock, 6> foundEntries;
    if (entryAtItsPlace)
        foundEntries.append(*entryAtItsPlace);
    auto branchOnKeyAt = [&](LValue entry, ptrdiff_t offset, BlockEdge otherKey) {
        if (!columns.keyIsString)
            foundEntries.append(m_out.anchor(entry));
        LValue place = m_out.add(columns.keys, m_out.shl(entry, m_out.constInt32(columns.keysAreCells ? 2 : 3)));
        if (!columns.keysAreCells) {
            m_out.branch(m_out.equal(m_out.load64(m_out.address(entriesHeap, place, -offset)), key.value()), normalEdge(isFound), otherKey);
            return;
        }
        LValue candidateWord = nullptr;
        if (columns.keyIsString) {
            LBasicBlock isInImage = newBlock();
            LBasicBlock isInHeap = newBlock();
            LBasicBlock hasCandidate = newBlock();
            m_out.branch(m_out.notZero64(m_out.bitAnd(columns.keys, m_out.constInt64(columnIsInImage))), normalEdge(isInImage), normalEdge(isInHeap));
            m_out.appendTo(isInHeap);
            ValueFromBlock candidateInHeap = m_out.anchor(m_out.load32(m_out.address(entriesHeap, place, -offset)));
            m_out.jump(hasCandidate);

            m_out.appendTo(isInImage);
            if (columns.readsValues) {
                m_out.jump(liesElsewhere);
                m_out.appendTo(hasCandidate);
                candidateWord = m_out.phi(Machine::Int32, candidateInHeap);
            } else {
                LValue constant = m_out.zeroExtPtr(m_out.load32(m_out.address(entriesHeap, place, -offset - columnIsInImage)));
                TypedPointer cells = instanceField(m_heaps.AOTInstance_fieldsFilledOnce, InstanceLayout::offsetOfStringCells);
                ValueFromBlock candidateInImage = m_out.anchor(m_out.load32(m_out.address(m_heaps.AOTInstance_fieldsFilledOnce, m_out.add(cells.base(), m_out.shl(constant, m_out.constInt32(2))), cells.offset())));
                m_out.jump(hasCandidate);
                m_out.appendTo(hasCandidate);
                candidateWord = m_out.phi(Machine::Int32, candidateInHeap, candidateInImage);
            }
            foundEntries.append(m_out.anchor(entry));
        } else
            candidateWord = m_out.load32(m_out.address(entriesHeap, place, -offset));
        NarrowReference candidate(candidateWord);
        LBasicBlock isOtherCell = columns.keyIsString ? newBlock() : nullptr;
        m_out.branch(m_out.equal(candidate.value(), compress(CellPointer(key.value())).value()), normalEdge(isFound), columns.keyIsString ? normalEdge(isOtherCell) : otherKey);
        if (!columns.keyIsString)
            return;
        LBasicBlock hasOtherCell = newBlock();
        LBasicBlock hasOtherString = newBlock();
        m_out.appendTo(isOtherCell);
        m_out.branch(isCellReferenceAmongAllWords(candidate).value(), normalEdge(hasOtherCell), unlikelyEdge(liesElsewhere));
        m_out.appendTo(hasOtherCell);
        CellPointer cell = decode(candidate);
        m_out.branch(hasCellType(cell, StringType).value(), normalEdge(hasOtherString), unlikelyEdge(liesElsewhere));
        m_out.appendTo(hasOtherString);
        foundEntries.append(m_out.anchor(entry));
        m_out.branch(m_out.equal(m_out.loadPtr(cell.value(), m_heaps.JSString_value), impl), normalEdge(isFound), normalEdge(liesElsewhere));
    };
    m_out.branch(m_out.notZero64(columns.mask), normalEdge(hasSlots), normalEdge(hasNoSlots));

    m_out.appendTo(hasNoSlots);
    m_out.branch(m_out.notZero64(columns.keys), normalEdge(hasFewEntries), searchesToEnd ? unlikelyEdge(hasNoColumn) : normalEdge(hasNoColumn));

    m_out.appendTo(hasNoColumn);
    m_out.branch(m_out.notZero64(columns.tags), searchesToEnd ? rareEdge(liesElsewhere) : normalEdge(liesElsewhere), normalEdge(isAbsent));

    m_out.appendTo(hasFewEntries);
    LValue fewDifferences = m_out.bitXor(m_out.load64(m_out.address(storage, columns.tags, 0)), bytesOf(m_out.lShr(hash, m_out.constInt32(CompactTable::tagShiftOfTags))));
    LValue belowFew = m_out.sub(fewDifferences, m_out.constInt64(CompactTable::onesOfTags));
    LValue fewMatches = m_out.bitAnd(m_out.bitAnd(belowFew, m_out.bitXor(fewDifferences, m_out.constInt64(-1))), m_out.constInt64(tops));
    m_out.branch(m_out.notZero64(fewMatches), normalEdge(isAmongFew), normalEdge(isAbsent));

    m_out.appendTo(isAmongFew);
    LValue fewEntry = placeOfLowest(fewMatches);
    LValue used = m_out.zeroExtPtr(m_out.load32(m_out.address(storage, columns.tags, CompactTable::offsetOfUsed() - CompactTable::offsetOfIndex())));
    m_out.branch(m_out.below(fewEntry, used), normalEdge(isUsed), normalEdge(isAbsent));

    m_out.appendTo(isUsed);
    branchOnKeyAt(fewEntry, 0, searchesToEnd ? rareEdge(liesElsewhere) : unlikelyEdge(liesElsewhere));

    auto branchOnKeyInSlot = [&](LValue slot, BlockEdge otherKey) {
        LBasicBlock hasShortSlot = newBlock();
        LBasicBlock hasLongSlot = newBlock();
        LValue entries = m_out.add(m_out.add(columns.tags, columns.mask), slot);
        m_out.branch(m_out.notZero64(m_out.bitAnd(columns.keys, m_out.constInt64(columnIsOfOtherKind))), normalEdge(hasLongSlot), normalEdge(hasShortSlot));

        m_out.appendTo(hasShortSlot);
        LValue shortEntry = m_out.zeroExtPtr(m_out.load8ZeroExt32(m_out.address(storage, entries, 1)));
        branchOnKeyAt(shortEntry, 0, otherKey);

        m_out.appendTo(hasLongSlot);
        LValue word = m_out.load32(m_out.address(storage, m_out.add(entries, m_out.shl(slot, m_out.constInt32(1))), 1));
        LValue longEntry = m_out.zeroExtPtr(m_out.bitAnd(word, m_out.constInt32(0xffffff)));
        branchOnKeyAt(longEntry, columnIsOfOtherKind, otherKey);
    };
    auto slotsWithTagAmong = [&](LValue eightTags, LValue bytesOfTag) {
        LValue differences = m_out.bitXor(eightTags, bytesOfTag);
        LValue below = m_out.sub(differences, m_out.constInt64(CompactTable::belowDifferenceOfOtherTags));
        return m_out.bitAnd(m_out.bitAnd(below, m_out.bitXor(differences, m_out.constInt64(-1))), m_out.constInt64(tops));
    };
    auto freeSlotsAmong = [&](LValue eightTags) { return m_out.bitAnd(m_out.bitXor(eightTags, m_out.constInt64(-1)), m_out.constInt64(tops)); };

    m_out.appendTo(hasSlots);
    LValue home = m_out.bitAnd(m_out.zeroExtPtr(hash), columns.mask);
    auto tagOfHash = [&](LValue hash) { return m_out.bitOr(m_out.lShr(hash, m_out.constInt32(CompactTable::tagShiftOfSlots - 1)), m_out.constInt32(CompactTable::takenSlot | CompactTable::keyLeftHomeSlot)); };
    LValue tag = tagOfHash(hash);
    LValue difference = m_out.bitXor(tag, m_out.load8ZeroExt32(m_out.address(storage, m_out.add(columns.tags, home), 0)));
    LBasicBlock hasTagAtHome = searchesToEnd ? newBlock() : hasSameTag;
    ValueFromBlock slotAtHome = searchesToEnd ? ValueFromBlock() : m_out.anchor(home);
    m_out.branch(m_out.testNonZero32(difference, m_out.constInt32(0xff & ~CompactTable::keyLeftHomeSlot)), normalEdge(hasOtherTag), normalEdge(hasTagAtHome));

    if (searchesToEnd) {
        m_out.appendTo(hasTagAtHome);
        branchOnKeyInSlot(home, unlikelyEdge(hasOtherTag));
    }

    m_out.appendTo(hasOtherTag);
    m_out.branch(m_out.testNonZero32(difference, m_out.constInt32(CompactTable::keyLeftHomeSlot)), normalEdge(isAbsent), normalEdge(searchesGroup));

    m_out.appendTo(searchesGroup);
    LValue group = m_out.bitAnd(home, m_out.constInt64(~static_cast<int64_t>(CompactTable::slotsPerGroup - 1)));
    LValue eightTags = m_out.load64(m_out.address(storage, m_out.add(columns.tags, group), 0));
    LValue matches = slotsWithTagAmong(eightTags, bytesOf(tag));
    m_out.branch(m_out.notZero64(matches), normalEdge(isInGroup), normalEdge(isNotInGroup));

    LBasicBlock searchesOn = searchesToEnd ? newBlock() : nullptr;
    LBasicBlock groupIsFull = searchesToEnd ? newBlock() : liesElsewhere;
    m_out.appendTo(isNotInGroup);
    m_out.branch(m_out.notZero64(freeSlotsAmong(eightTags)), normalEdge(isAbsent), searchesToEnd ? rareEdge(groupIsFull) : normalEdge(groupIsFull));

    m_out.appendTo(isInGroup);
    LValue firstSlotInGroup = m_out.add(group, placeOfLowest(matches));
    if (!searchesToEnd) {
        ValueFromBlock slotInGroup = m_out.anchor(firstSlotInGroup);
        m_out.jump(hasSameTag);

        m_out.appendTo(hasSameTag);
        branchOnKeyInSlot(m_out.phi(Machine::Int64, slotAtHome, slotInGroup), unlikelyEdge(liesElsewhere));
    } else {
        LBasicBlock hasOtherKeyInGroup = newBlock();
        LBasicBlock isInLaterGroup = newBlock();
        LBasicBlock hasOtherKeyLater = newBlock();
        LBasicBlock isNotInLaterGroup = newBlock();
        LBasicBlock laterGroupIsFull = newBlock();
        LValue laterGroup = m_out.phiIn(searchesOn, Machine::Int64);
        LValue allowed = m_out.phiIn(searchesOn, Machine::Int64);
        LValue bytesOfTag = m_out.phiIn(searchesOn, Machine::Int64);
        auto tagAgain = [&] { return tagOfHash(m_out.foldedProduct(key.value(), m_out.constInt64(static_cast<int64_t>(CompactTable::keyMultiplier)))); };
        auto searchBehindSlot = [&](LValue slot, LValue bytes) {
            LValue bitOfSlot = m_out.add(m_out.shl(m_out.castToInt32(m_out.bitAnd(slot, m_out.constInt64(CompactTable::slotsPerGroup - 1))), m_out.constInt32(3)), m_out.constInt32(7));
            m_out.upsilon(m_out.bitAnd(slot, m_out.constInt64(~static_cast<int64_t>(CompactTable::slotsPerGroup - 1))), laterGroup);
            m_out.upsilon(m_out.shl(m_out.constInt64(-2), bitOfSlot), allowed);
            m_out.upsilon(bytes, bytesOfTag);
            m_out.jump(searchesOn);
        };
        auto searchBehindGroup = [&](LValue group, LValue bytes) {
            m_out.upsilon(m_out.bitAnd(m_out.add(group, m_out.constInt64(CompactTable::slotsPerGroup)), columns.mask), laterGroup);
            m_out.upsilon(m_out.constInt64(-1), allowed);
            m_out.upsilon(bytes, bytesOfTag);
            m_out.jump(searchesOn);
        };
        branchOnKeyInSlot(firstSlotInGroup, rareEdge(hasOtherKeyInGroup));

        m_out.appendTo(hasOtherKeyInGroup);
        searchBehindSlot(firstSlotInGroup, bytesOf(tagAgain()));

        m_out.appendTo(groupIsFull);
        searchBehindGroup(group, bytesOf(tagAgain()));

        m_out.appendTo(searchesOn);
        LValue laterTags = m_out.load64(m_out.address(storage, m_out.add(columns.tags, laterGroup), 0));
        LValue laterMatches = m_out.bitAnd(slotsWithTagAmong(laterTags, bytesOfTag), allowed);
        m_out.branch(m_out.notZero64(laterMatches), normalEdge(isInLaterGroup), normalEdge(isNotInLaterGroup));

        m_out.appendTo(isInLaterGroup);
        LValue laterSlot = m_out.add(laterGroup, placeOfLowest(laterMatches));
        LValue isLong = m_out.bitAnd(columns.keys, m_out.constInt64(columnIsOfOtherKind));
        LValue wordOfSlot = m_out.load32(m_out.address(storage, m_out.add(m_out.add(columns.tags, columns.mask), m_out.add(laterSlot, m_out.mul(m_out.shl(isLong, m_out.constInt32(1)), laterSlot))), 1));
        LValue laterEntry = m_out.bitAnd(m_out.zeroExtPtr(wordOfSlot), m_out.bitOr(m_out.mul(isLong, m_out.constInt64(0xffff00)), m_out.constInt64(0xff)));
        LValue placeOfLaterKey = m_out.add(m_out.sub(columns.keys, isLong), m_out.shl(laterEntry, m_out.constInt32(columns.keysAreCells ? 2 : 3)));
        foundEntries.append(m_out.anchor(laterEntry));
        LValue isLaterKey = columns.keysAreCells ? m_out.equal(m_out.load32(m_out.address(entriesHeap, placeOfLaterKey, 0)), compress(CellPointer(key.value())).value()) : m_out.equal(m_out.load64(m_out.address(entriesHeap, placeOfLaterKey, 0)), key.value());
        m_out.branch(isLaterKey, normalEdge(isFound), normalEdge(hasOtherKeyLater));

        m_out.appendTo(hasOtherKeyLater);
        searchBehindSlot(laterSlot, bytesOfTag);

        m_out.appendTo(isNotInLaterGroup);
        m_out.branch(m_out.notZero64(freeSlotsAmong(laterTags)), normalEdge(isAbsent), normalEdge(laterGroupIsFull));

        m_out.appendTo(laterGroupIsFull);
        searchBehindGroup(laterGroup, bytesOfTag);
    }

    m_out.appendTo(isFound);
    LValue entry = m_out.phi(Machine::Int64, foundEntries);
    Vector<ValueFromBlock, 3> keptEntries;
    auto keepEntry = [&](LValue kept) {
        if (columns.keepsEntry)
            keptEntries.append(m_out.anchor(kept));
    };
    if (columns.readsValues)
        join.add(valueOfEntry(columns, entry));
    else {
        keepEntry(entry);
        join.add(boolean(true));
    }

    m_out.appendTo(isAbsent);
    if (columns.edgeIfAbsent)
        generator().jumpIfAbsent(*columns.edgeIfAbsent);
    else if (columns.readsValues)
        join.add(undefined());
    else {
        keepEntry(m_out.constInt64(entryOfAbsentKey));
        join.add(boolean(false));
    }

    m_out.appendTo(liesElsewhere);
    Operand foundByHash = lookUpByHash();
    keepEntry(m_out.constInt64(unknownEntry));
    join.add(foundByHash);
    Operand result = join.finish();
    m_entryOfTest = columns.keepsEntry ? m_out.phi(Machine::Int64, keptEntries) : nullptr;
    return result;
}

Boxed CollectionIntrinsicEmitter::valueOfEntry(const CollectionColumns& columns, LValue entry)
{
    TableReader reader(generator());
    const Machine::AbstractHeap& entries = m_heaps.entire(AbstractHeap::CollectionEntries);
    return selectByBranch<Boxed>(Condition(m_out.notZero64(m_out.bitAnd(columns.values, m_out.constInt64(columnIsOfOtherKind)))), [&] {
        return reader.decodeNarrow(NarrowReference(m_out.load32(m_out.address(entries, m_out.add(columns.values, m_out.shl(entry, m_out.constInt32(2))), -columnIsOfOtherKind))));
    }, [&] {
        return Boxed(m_out.load64(m_out.address(entries, m_out.add(columns.values, m_out.shl(entry, m_out.constInt32(3))), 0)));
    });
}

LValue CollectionIntrinsicEmitter::entryOfTest(ValueID test)
{
    if (!test || generator().isLayingOutTurns())
        return nullptr;
    for (size_t index = m_entriesOfTests.size(); index--;) {
        if (m_entriesOfTests[index].first == test)
            return m_entriesOfTests[index].second;
    }
    return nullptr;
}

Operand CollectionIntrinsicEmitter::valueAtTestedEntry(const ValueContext& value, const CollectionColumns& columns, LValue entry, const ScopedLambda<Operand()>& lookUpByHash)
{
    LBasicBlock isFound = newBlock();
    LBasicBlock isNotFound = newBlock();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock liesElsewhere = newBlock();
    OperandJoin join(m_out, Representation::JSValue);
    if (columns.keyIsString) {
        LBasicBlock hasColumn = newBlock();
        m_out.branch(m_out.notZero64(columns.values), normalEdge(hasColumn), unlikelyEdge(liesElsewhere));
        m_out.appendTo(hasColumn);
    }
    m_out.branch(m_out.lessThan(entry, m_out.constInt64(0)), unlikelyEdge(isNotFound), normalEdge(isFound));

    m_out.appendTo(isFound);
    Boxed found = valueOfEntry(columns, entry);
    if (emittersCheckProofs())
        emitter<ThrowEmitter>().requireOrCrash(value, Condition(m_out.equal(found.value(), lookUpByHash().as<Boxed>().value())), std::nullopt, ProofSource::RememberedFact);
    join.add(found);

    m_out.appendTo(isNotFound);
    m_out.branch(m_out.equal(entry, m_out.constInt64(entryOfAbsentKey)), normalEdge(isAbsent), rareEdge(liesElsewhere));

    m_out.appendTo(isAbsent);
    join.add(undefined());

    m_out.appendTo(liesElsewhere);
    join.add(lookUpByHash());
    return join.finish();
}

bool CollectionIntrinsicEmitter::mayBeNullish(ValueID key) const
{
    return typeOf(key).primitives & Primitive::UndefinedOrNull;
}

CollectionIntrinsicEmitter::CollectionColumns CollectionIntrinsicEmitter::columnsOfLookup(const ValueContext& value, ValueID collection, KeyOfLookup key, bool readsValues)
{
    CollectionColumns columns;
    columns.keysAreCells = key != KeyOfLookup::Number;
    columns.keyIsString = key == KeyOfLookup::String;
    columns.keyMayBeNullish = mayBeNullish(value->keyOperand());
    columns.readsValues = readsValues;
    LValue tags = integerOf(value->tagsOperand()).value();
    if (!value->maskOperand()) {
        columns.cache = tags;
        columns.collection = in<CellPointer>(collection);
        return columns;
    }
    columns.tags = tags;
    columns.mask = integerOf(value->maskOperand()).value();
    columns.keys = integerOf(value->keysOperand()).value();
    if (readsValues)
        columns.values = integerOf(value->valuesOperand()).value();
    return columns;
}

void CollectionIntrinsicEmitter::getMapValue(const ValueContext& value, CollectionColumns columns)
{
    auto& tables = emitter<HashTableBuiltinEmitter>();
    Boxed key = tables.normalizedKey(value, value->keyOperand());
    auto lookUpByHash = [&]() -> Operand { return tables.lookUpNormalizedKey(value, value->mapOperand(), value->keyOperand(), key, HashTableOwner::Map, LookupAnswer::Value); };
    if (LValue entry = entryOfTest(value->testOperand())) {
        Boxed found = valueAtTestedEntry(value, columns, entry, scopedLambda<Operand()>(lookUpByHash)).as<Boxed>();
        define(value, emitter<ConversionEmitter>().inRepresentation(value, found, typeOf(value.id()), value.representation()));
        return;
    }
    columns.edgeIfAbsent = generator().findEdgeIfAbsent(value);
    CollectionColumns searched = columns;
    searched.keyMayBeAtItsPlace = !columns.keysAreCells && isCounterOrSmallNumber(value->keyOperand());
    Boxed found = lookUpByTag(searched, key, scopedLambda<Operand()>(lookUpByHash)).as<Boxed>();
    define(value, emitter<ConversionEmitter>().inRepresentation(value, found, typeOf(value.id()), value.representation()));
}

void CollectionIntrinsicEmitter::testForKey(const ValueContext& value, const CollectionColumns& columns)
{
    auto& tables = emitter<HashTableBuiltinEmitter>();
    Boxed key = tables.normalizedKey(value, value->keyOperand());
    HashTableOwner owner = value->searchesSet() ? HashTableOwner::Set : HashTableOwner::Map;
    auto lookUpByHash = [&]() -> Operand { return tables.lookUpNormalizedKey(value, value->collectionOperand(), value->keyOperand(), key, owner, LookupAnswer::IsPresent); };
    CollectionColumns searched = columns;
    searched.keyMayBeAtItsPlace = !columns.keysAreCells && isCounterOrSmallNumber(value->keyOperand());
    searched.keepsEntry = !columns.cache && owner == HashTableOwner::Map && m_code.useCountOf(value.id()) > 1 && !generator().isLayingOutTurns();
    define(value, lookUpByTag(searched, key, scopedLambda<Operand()>(lookUpByHash)).as<Condition>());
    if (searched.keepsEntry)
        m_entriesOfTests.append({ value.id(), m_entryOfTest });
}

void CollectionIntrinsicEmitter::generateGetMapValueByTag(const ValueContext& value)
{
    getMapValue(value, columnsOfLookup(value, value->mapOperand(), value->keysAreCells() ? KeyOfLookup::Cell : KeyOfLookup::Number, true));
}

void CollectionIntrinsicEmitter::generateHasKeyByTag(const ValueContext& value)
{
    testForKey(value, columnsOfLookup(value, value->collectionOperand(), value->keysAreCells() ? KeyOfLookup::Cell : KeyOfLookup::Number, false));
}

void CollectionIntrinsicEmitter::generateGetMapValueByStringTag(const ValueContext& value)
{
    getMapValue(value, columnsOfLookup(value, value->mapOperand(), KeyOfLookup::String, true));
}

void CollectionIntrinsicEmitter::generateHasStringKeyByTag(const ValueContext& value)
{
    testForKey(value, columnsOfLookup(value, value->collectionOperand(), KeyOfLookup::String, false));
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
            LValue flags = reader.flagsOf(table);
            LValue keys = reader.keysOf(table, flags);
            Condition keysAreWide = reader.hasWideKeys(flags);
            LValue values = m_out.select(keysAreWide.value(), reader.valuesBehind(CompactTable::Width::Wide, table, keys), reader.valuesBehind(CompactTable::Width::Narrow, table, keys));
            LValue distance = m_out.sub(m_out.sub(m_out.add(table, place.value()), values), m_out.constInt64(CompactTable::offsetOfIndex()));
            LValue entry = m_out.lShr(distance, m_out.constInt32(3));
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
