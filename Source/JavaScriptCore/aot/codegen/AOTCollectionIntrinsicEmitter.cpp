/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversionEmitter.h"
#include "AOTCollectionIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTResultsJoin.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTUseLists.h"
#include "AOTVector.h"
#include "JSSetIterator.h"
#include "VM.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSSet.h"
#include "VM.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(OBJECT_OFFSETOF(JSMap, m_storage) == OBJECT_OFFSETOF(JSSet, m_storage));
static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex() && JSMap::Helper::deletedEntryCountIndex() == JSSet::Helper::deletedEntryCountIndex());
static_assert(JSMap::Helper::dataTableStartIndex(JSMap::Helper::LargeCapacity) == JSMap::Helper::hashTableStartIndex() + JSMap::Helper::LargeCapacity);
static_assert(static_cast<unsigned>(SetNextResult::HasEntry) == static_cast<unsigned>(MapNextResult::HasEntry) && static_cast<unsigned>(SetNextResult::Entry) == static_cast<unsigned>(MapNextResult::Entry) && static_cast<unsigned>(SetNextResult::Element) == static_cast<unsigned>(MapNextResult::Key));
static_assert(static_cast<unsigned>(MapNextResult::Value) == numberOfEnumerators<SetNextResult> && numberOfEnumerators<MapNextResult> == numberOfEnumerators<SetNextResult> + 1);
static_assert(!static_cast<unsigned>(MapNextResult::HasEntry) && static_cast<unsigned>(MapNextResult::Entry) == 1 && static_cast<unsigned>(MapNextResult::Key) == 2);
static_assert(numberOfEnumerators<MapNextResult> == 4);

namespace {

struct TableEntry {
    Boxed key;
    Boxed value;
};

using StepJoin = ResultsJoin<Condition, Word32, Boxed, Boxed>;

} // anonymous namespace

class CollectionIntrinsicEmitter::TableReader final : public OperationEmitter {
public:
    explicit TableReader(CodeGenerator& generator) : OperationEmitter(generator) { }

    LValue storageOf(CellPointer owner) { return m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), owner.value(), JSMap::offsetOfStorage())); }
    TypedPointer tableField(LValue table, ptrdiff_t offset) { return m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), table, offset); }

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

    // Most tables have no deleted entries, so the first entry is checked inline and the loop that skips deleted entries is off the likely path.
    // A step that may be the first of its walk names the owner: CompactTable::deletedPrefixOf(). From a deleted entry at or in front of the mark it goes on at the mark, and writes down where it has found an entry alive.
    LValue firstAliveEntry(std::optional<CellPointer> ownerOfWalkFromFront, LValue start, LValue used, LBasicBlock hasNoEntry, NOESCAPE const Invocable<LValue(LValue)> auto& isAlive)
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
        ValueFromBlock aliveAtStart = m_out.anchor(start);
        ValueFromBlock deletedAtStart = deletedPrefix ? ValueFromBlock() : m_out.anchor(start);
        LValue beginsInDeletedPrefix = nullptr;
        LBasicBlock isDeleted = deletedPrefix ? m_out.newBlock() : loop;
        m_out.branch(isAlive(start), normalEdge(found), unlikelyEdge(isDeleted));
        if (deletedPrefix) {
            m_out.appendTo(isDeleted);
            LValue known = m_out.zeroExtPtr(m_out.load32(*deletedPrefix));
            beginsInDeletedPrefix = m_out.belowOrEqual(start, known);
            deletedAtStart = m_out.anchor(m_out.select(m_out.below(start, known), m_out.sub(known, m_out.constInt64(1)), start));
            m_out.jump(loop);
        }

        m_out.appendTo(loop);
        LValue deletedEntry = m_out.phi(B3::Int64, deletedAtStart);
        LValue candidate = m_out.add(deletedEntry, m_out.constInt64(1));
        m_out.branch(m_out.below(candidate, used), normalEdge(isUsed), unlikelyEdge(hasNoEntry));

        m_out.appendTo(isUsed);
        ValueFromBlock liveAfterDeletedEntries = deletedPrefix ? ValueFromBlock() : m_out.anchor(candidate);
        m_out.addIncomingToPhi(deletedEntry, m_out.anchor(candidate));
        LBasicBlock isBehindDeleted = deletedPrefix ? m_out.newBlock() : found;
        ValueFromBlock firstAlive;
        m_out.branch(isAlive(candidate), normalEdge(isBehindDeleted), normalEdge(loop));
        if (deletedPrefix) {
            LBasicBlock isFirstAlive = m_out.newBlock();
            m_out.appendTo(isBehindDeleted);
            liveAfterDeletedEntries = m_out.anchor(candidate);
            m_out.branch(beginsInDeletedPrefix, normalEdge(isFirstAlive), normalEdge(found));
            m_out.appendTo(isFirstAlive);
            m_out.store32(m_out.castToInt32(candidate), *deletedPrefix);
            firstAlive = m_out.anchor(candidate);
            m_out.jump(found);
        }

        m_out.appendTo(found);
        LValue result = m_out.phi(B3::Int64, aliveAtStart, liveAfterDeletedEntries);
        if (deletedPrefix)
            m_out.addIncomingToPhi(result, firstAlive);
        return result;
    }

    LValue isAliveAt(CompactTable::Width keyWidth, LValue keys, LValue entryIndex)
    {
        if (keyWidth == CompactTable::Width::Wide)
            return m_out.notZero64(m_out.load64(elementAt(keys, entryIndex, 3)));
        return m_out.notZero32(m_out.load32(elementAt(keys, entryIndex, 2)));
    }

    Boxed keyAt(CompactTable::Width keyWidth, LValue keys, LValue entryIndex)
    {
        if (keyWidth == CompactTable::Width::Wide)
            return Boxed(m_out.load64(elementAt(keys, entryIndex, 3)));
        return decodeNarrow(NarrowReference(m_out.load32(elementAt(keys, entryIndex, 2))));
    }

    LValue valuesBehind(CompactTable::Width keyWidth, LValue table, LValue keys)
    {
        return m_out.add(keys, m_out.shl(m_out.zeroExtPtr(m_out.load32(tableField(table, CompactTable::offsetOfCapacity()))), keyWidth == CompactTable::Width::Wide ? 3 : 2));
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
        join.add(decodeNarrow(NarrowReference(m_out.load32(elementAt(values, entryIndex, 2)))));

        m_out.appendTo(isWide);
        join.add(Boxed(m_out.load64(elementAt(values, entryIndex, 3))));
        return join.finish();
    }

    Condition hasKeysOfWidth(CompactTable::Width keyWidth, LValue table)
    {
        // The bit for wide keys is the sign of the byte and the bit for the image the one below it.
        static_assert(CompactTable::keysAreWide == 0x80 && CompactTable::keysAreInImage == 0x40);
        if (keyWidth == CompactTable::Width::Wide)
            return Condition(m_out.lessThan(m_out.load8SignExt32(tableField(table, CompactTable::offsetOfKeysShift())), static_cast<int8_t>(CompactTable::keysAreWide | CompactTable::keysAreInImage)));
        return hasNoBits(Word32(m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfKeysShift()))), CompactTable::keysAreWide | CompactTable::keysAreInImage);
    }
};

// The first test or expectation of a key or a value after the step.
std::optional<bool> CollectionIntrinsicEmitter::isTakenForNumberAfter(const ValueContext& step, uint32_t result)
{
    const UseLists& useLists = analyses().useLists();
    Vector<ValueID, 4> names;
    for (const Use& use : useLists.usesOf(step.id())) {
        if (use.kind != Use::Kind::Operand)
            continue;
        const Value& user = m_function.at(use.user);
        if (user.opcode == Opcode::ResultAt && user.index() == result)
            names.append(use.user);
    }
    for (size_t i = 0; i < names.size(); ++i) {
        for (const Use& use : useLists.usesOf(names[i])) {
            if (use.kind == Use::Kind::EdgeArgument) {
                const auto& parameters = m_function.at(targetOf(m_function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    names.appendIfNotContains(parameters[use.operandIndex]);
                continue;
            }
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = m_function.at(use.user);
            TypeID taken;
            if (user.opcode == Opcode::TypeTest)
                taken = user.testedType();
            else if (user.opcode == Opcode::Expect)
                taken = user.expectedType().predicted();
            if (taken) {
                const Type& type = types().at(taken);
                if (type.isNumber())
                    return true;
                if (!(type.primitives & Primitive::Number))
                    return false;
            }
            if (isCast(user.opcode))
                names.appendIfNotContains(use.user);
        }
    }
    return std::nullopt;
}

void CollectionIntrinsicEmitter::generateNext(Collection kind, const ValueContext& value)
{
    CellPointer owner = in<CellPointer>(kind == Collection::Map ? value->mapOperand() : value->setOperand());
    Boxed table = in<Boxed>(value->tableOperand());
    Integer start = integerOf(value->entryOperand());
    const size_t numberOfResults = kind == Collection::Map ? numberOfEnumerators<MapNextResult> : numberOfEnumerators<SetNextResult>;
    // A stub stands in front of the operation and takes the step in a table that is still the owner's.
    auto addStepByOperation = [&](StepJoin& join) {
        RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
        Pointer buffer = calls.uninitializedArgumentBuffer(2);
        std::array<Operand, 4> arguments { owner, table, low32(start), buffer };
        StubCallResult step = calls.callOperationReturningPair(value, kind == Collection::Map ? RuntimeOperation::MapNext : RuntimeOperation::SetNext, arguments);
        auto resultInBuffer = [&](unsigned index) {
            return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), index * sizeof(EncodedJSValue))));
        };
        TableEntry entryFromOperation { resultInBuffer(0), kind == Collection::Map ? resultInBuffer(1) : undefined() };
        Condition hasEntry(m_out.notZero64(step.first));
        join.add(hasEntry, Word32(m_out.castToInt32(step.second)), entryFromOperation.key, entryFromOperation.value);
    };
    if (value.shape() != CodeShape::Fast) {
        StepJoin join(m_out);
        addStepByOperation(join);
        auto results = join.finish();
        defineResults(value, std::span<const Operand>(results).first(numberOfResults));
        return;
    }

    TableReader reader(generator());
    LBasicBlock isCurrent = newBlock();
    LBasicBlock isChanged = newBlock();
    LBasicBlock isCompact = newBlock();
    LBasicBlock hasNoEntry = newBlock();
    LBasicBlock slowPath = newBlock();
    StepJoin join(m_out);
    LValue storage = reader.storageOf(owner);
    // A first step takes the table that the collection holds now.
    if (m_function.at(value->tableOperand()).opcode == Opcode::Null)
        m_out.branch(m_out.isNull(storage), unlikelyEdge(isChanged), normalEdge(isCurrent));
    else
        m_out.branch(m_out.equal(storage, table.value()), normalEdge(isCurrent), unlikelyEdge(isChanged));

    m_out.appendTo(isChanged);
    m_out.branch(m_out.isNull(storage), normalEdge(hasNoEntry), rareEdge(slowPath));

    m_out.appendTo(isCurrent);
    // In compiled code a collection gets a compact table with its first entry. The stub handles the other kind of table.
    m_out.branch(hasCellType(CellPointer(storage), JSCellButterflyType).value(), rareEdge(slowPath), normalEdge(isCompact));

    m_out.appendTo(isCompact);
    // A number never fits a short key, and a table with one wide key has wide keys only: the type of the keys says which of the two widths is read in line. The other one is the stub's.
    Type keys = typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Expected }, typeOf(kind == Collection::Map ? value->mapOperand() : value->setOperand()), kind == Collection::Map ? ContentsSlot::Key : ContentsSlot::Element);
    // The type of a Map made from pairs lists keys and values together. What the code after the step takes the key for says more, and numbers that are not the values are the keys.
    // Otherwise, where numbers are among the keys and strings are not, the numbers decide.
    static_assert(static_cast<unsigned>(SetNextResult::Element) == static_cast<unsigned>(MapNextResult::Key));
    std::optional<bool> keyIsTakenForNumber = isTakenForNumberAfter(value, static_cast<uint32_t>(MapNextResult::Key));
    if (!keyIsTakenForNumber && kind == Collection::Map && !keys.isAnything() && keys == typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Expected }, typeOf(value->mapOperand()), ContentsSlot::Value)) {
        if (isTakenForNumberAfter(value, static_cast<uint32_t>(MapNextResult::Value)) == false)
            keyIsTakenForNumber = true;
    }
    const bool expectsNumberKeys = (keys.primitives & Primitive::Number) && keyIsTakenForNumber.value_or(!(keys.primitives & Primitive::AnyString));
    const CompactTable::Width keyWidth = expectsNumberKeys ? CompactTable::Width::Wide : CompactTable::Width::Narrow;
    // Where the keys may be of both kinds, the table's width is unknown: the other one is read in line behind the test that failed.
    const bool readsBothWidths = (keys.primitives & Primitive::Number) && !keys.withoutPrimitives(Primitive::Undefined | Primitive::Null | Primitive::Boolean).isNumber();
    LBasicBlock hasOtherWidth = readsBothWidths ? newBlock() : slowPath;
    LBasicBlock hasExpectedWidth = newBlock();
    auto loadUsed = [&] { return m_out.zeroExtPtr(m_out.load32(reader.tableField(storage, CompactTable::offsetOfUsed()))); };
    m_out.branch(reader.hasKeysOfWidth(keyWidth, storage).value(), normalEdge(hasExpectedWidth), readsBothWidths ? unlikelyEdge(hasOtherWidth) : rareEdge(slowPath));

    m_out.appendTo(hasExpectedWidth);
    LValue used = loadUsed();
    LValue column = reader.keysOf(storage);
    // A step in a loop has its entry from the step before. One that is written to begin at the front is the first of its walk, and often the only one: `map.keys().next()`, `const [first] = set`.
    // One that has its entry out of an iterator object may be the first.
    ValueID entryToBeginAt = value->entryOperand();
    while (resultRuleOf(m_function.at(entryToBeginAt).opcode) == ResultRule::Cast)
        entryToBeginAt = m_function.at(entryToBeginAt).valueOperand();
    const Value& origin = m_function.at(entryToBeginAt);
    const bool mayBeFirstStep = (origin.opcode == Opcode::Integer && !origin.integer()) || origin.opcode == Opcode::GetInternalField;
    if (mayBeFirstStep)
        build().statistics().count(StatisticID::StepsThatKnowTheDeletedPrefix);
    LValue entryIndex = reader.firstAliveEntry(mayBeFirstStep ? std::optional { owner } : std::nullopt, start.value(), used, hasNoEntry, [&](LValue candidate) { return reader.isAliveAt(keyWidth, column, candidate); });
    std::optional<Boxed> key;
    LValue values = nullptr;
    if (readsBothWidths) {
        const CompactTable::Width otherWidth = expectsNumberKeys ? CompactTable::Width::Narrow : CompactTable::Width::Wide;
        LBasicBlock found = newBlock();
        ValueFromBlock expectedIndex = m_out.anchor(entryIndex);
        ValueFromBlock expectedKey = m_out.anchor(reader.keyAt(keyWidth, column, entryIndex).value());
        ValueFromBlock expectedValues = kind == Collection::Map ? m_out.anchor(reader.valuesBehind(keyWidth, storage, column)) : ValueFromBlock();
        m_out.jump(found);

        // The stub skips deleted entries.
        m_out.appendTo(hasOtherWidth);
        branchUnless(reader.hasKeysOfWidth(otherWidth, storage), slowPath);
        LBasicBlock isUsed = newBlock();
        m_out.branch(m_out.below(start.value(), loadUsed()), normalEdge(isUsed), unlikelyEdge(hasNoEntry));
        m_out.appendTo(isUsed);
        LValue otherColumn = reader.keysOf(storage);
        branchUnless(Condition(reader.isAliveAt(otherWidth, otherColumn, start.value())), slowPath);
        ValueFromBlock otherIndex = m_out.anchor(start.value());
        ValueFromBlock otherKey = m_out.anchor(reader.keyAt(otherWidth, otherColumn, start.value()).value());
        ValueFromBlock otherValues = kind == Collection::Map ? m_out.anchor(reader.valuesBehind(otherWidth, storage, otherColumn)) : ValueFromBlock();
        m_out.jump(found);

        m_out.appendTo(found);
        entryIndex = m_out.phi(B3::Int64, expectedIndex, otherIndex);
        key = Boxed(m_out.phi(B3::Int64, expectedKey, otherKey));
        if (kind == Collection::Map)
            values = m_out.phi(B3::Int64, expectedValues, otherValues);
    }
    // From here on the walk stands behind an entry. The mark is bookkeeping that only clear() reads, which comes after a step as a write of the table comes after a read. The other bits of the byte never change.
    LBasicBlock marks = newBlock();
    LBasicBlock isMarked = newBlock();
    TypedPointer byteWithMark = m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), storage, CompactTable::offsetOfKeysShift());
    Word32 flagsAndMark(m_out.load8ZeroExt32(byteWithMark));
    m_out.branch(hasNoBits(flagsAndMark, CompactTable::wasWalked).value(), unlikelyEdge(marks), normalEdge(isMarked));

    m_out.appendTo(marks);
    m_out.store32As8(m_out.bitOr(flagsAndMark.value(), m_out.constInt32(CompactTable::wasWalked)), byteWithMark);
    m_out.jump(isMarked);

    m_out.appendTo(isMarked);
    // A number never fits a short value either. If the values may be of both kinds, the table's width is unknown.
    std::optional<CompactTable::Width> expectedValueWidth;
    if (kind == Collection::Map) {
        Type values = typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Expected }, typeOf(value->mapOperand()), ContentsSlot::Value).withoutPrimitives(Primitive::Undefined | Primitive::Null | Primitive::Boolean);
        if (values.isNumber())
            expectedValueWidth = CompactTable::Width::Wide;
        else if (!(values.primitives & Primitive::Number))
            expectedValueWidth = CompactTable::Width::Narrow;
    }
    TableEntry entry { key ? *key : reader.keyAt(keyWidth, column, entryIndex), undefined() };
    if (kind == Collection::Map)
        entry.value = reader.valueAt(expectedValueWidth, storage, values ? values : reader.valuesBehind(keyWidth, storage, column), entryIndex);
    Condition hasEntry = boolean(true);
    join.add(hasEntry, low32(Integer(entryIndex)), entry.key, entry.value);

    m_out.appendTo(hasNoEntry);
    Condition hasNone = boolean(false);
    Word32 noIndex = word32(0);
    Boxed noValue = undefined();
    join.add(hasNone, noIndex, noValue, noValue);

    m_out.appendTo(slowPath);
    addStepByOperation(join);

    auto results = join.finish();
    defineResults(value, std::span<const Operand>(results).first(numberOfResults));
}

// What a closed iterator holds in place of a table.
void CollectionIntrinsicEmitter::generateCollectionIteratorClosedMark(const ValueContext& value)
{
    define(value, Boxed(m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), vm().value(), VM::offsetOfOrderedHashTableSentinel()))));
}

// An iterator made for a collection without a table holds no value at all. In compiled code that is null.
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
    barrierAfterStore(value, iterator, table, typeOf(value->valueOperand()));
    define(value, emitter<ConversionEmitter>().inRepresentation(value, get(value->valueOperand()), typeOf(value->valueOperand()), value.representation()));
}

void CollectionIntrinsicEmitter::generateIsCollectionIteratorClosedMark(const ValueContext& value)
{
    // An iterator that has made no step. Where the last run of SinkAllocations finds that out, nothing folds the test any more.
    if (m_function.at(value->tableOperand()).opcode == Opcode::Null) {
        defineTest(value, boolean(false));
        return;
    }
    LValue mark = m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), vm().value(), VM::offsetOfOrderedHashTableSentinel()));
    defineTest(value, Condition(m_out.equal(in<Boxed>(value->tableOperand()).value(), mark)));
}

// Storage of another kind answers anything: the operation that follows looks again.
void CollectionIntrinsicEmitter::generateIsCollectionTableFull(const ValueContext& value)
{
    static_assert(std::max(CompactTable::offsetOfUsed(), CompactTable::offsetOfCapacity()) + sizeof(uint32_t) <= JSCellButterfly::offsetOfData() + JSMap::Helper::hashTableStartIndex() * sizeof(EncodedJSValue));
    TableReader reader(generator());
    LBasicBlock hasTable = newBlock();
    LBasicBlock continuation = newBlock();
    LValue storage = reader.storageOf(in<CellPointer>(value->collectionOperand()));
    ValueFromBlock withoutTable = m_out.anchor(m_out.constInt32(0));
    m_out.branch(m_out.isNull(storage), unlikelyEdge(continuation), normalEdge(hasTable));

    m_out.appendTo(hasTable);
    LValue used = m_out.load32(reader.tableField(storage, CompactTable::offsetOfUsed()));
    ValueFromBlock withTable = m_out.anchor(m_out.equal(used, m_out.load32(reader.tableField(storage, CompactTable::offsetOfCapacity()))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    defineTest(value, Condition(m_out.phi(B3::Int32, withoutTable, withTable)));
}

void CollectionIntrinsicEmitter::generateReserveCollectionCapacity(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ReserveCollectionCapacity, { in<CellPointer>(value->collectionOperand()), integerOf(value->remainingOperand()), integerOf(value->finishedOperand()), integerOf(value->sizeAtEntryOperand()) });
}

void CollectionIntrinsicEmitter::generateCollectionTable(const ValueContext& value)
{
    LValue storage = TableReader(generator()).storageOf(in<CellPointer>(value->collectionOperand()));
    LValue hasStorage = m_out.notNull(storage);
    define(value, Boxed(m_out.select(hasStorage, storage, m_out.constInt64(JSValue::encode(jsNull())))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
