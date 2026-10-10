/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedCompactTable.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include <wtf/text/IntegerToStringConversion.h>

namespace JSC { namespace AOT {

using Typed::CompactTable;

template<typename Traits>
static size_t nextEntry(Instance* instance, JSCell* collection, EncodedJSValue encodedTable, uint32_t entry)
{
    JSValue table = JSValue::decode(encodedTable);
    auto position = CompactTable::next(*instance->vm, uncheckedDowncast<JSOrderedHashTable<Traits>>(collection), table.isCell() ? table.asCell() : nullptr, entry);
    return position.table ? position.entry : UINT32_MAX;
}

// The entry is alive, in the table the collection has.
template<typename Traits>
static EncodedJSValue partOfEntry(JSCell* collection, uint32_t entry, bool isKey)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    auto* owner = uncheckedDowncast<JSOrderedHashTable<Traits>>(collection);
    if (owner->m_compactTable) {
        auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
        return JSValue::encode(isKey ? JSValue(table->keyAt(entry)) : table->valueAt(entry));
    }
    if constexpr (Traits::hasValueData) {
        if (!isKey)
            return JSValue::encode(Helper::getValue(owner->storageRef(), entry));
    }
    return JSValue::encode(Helper::getKey(owner->storageRef(), entry));
}

// For the stubs, where they do not remove the entry themselves.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedMapDelete, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(uncheckedDowncast<JSMap>(map)->remove(globalObject, JSValue::decode(key)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetDelete, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(uncheckedDowncast<JSSet>(set)->remove(globalObject, JSValue::decode(key)))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewMapIterator, JSCell*, (Instance* instance, JSCell* map, uint32_t kind))
{
    return JSMapIterator::create(*instance->vm, instance->globalObject->mapIteratorStructure(), uncheckedDowncast<JSMap>(map), static_cast<IterationKind>(kind));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewSetIterator, JSCell*, (Instance* instance, JSCell* set, uint32_t kind))
{
    return JSSetIterator::create(*instance->vm, instance->globalObject->setIteratorStructure(), uncheckedDowncast<JSSet>(set), static_cast<IterationKind>(kind));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedMapClear, void, (Instance* instance, JSCell* map))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSMap>(map)->clear(globalObject);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetClear, void, (Instance* instance, JSCell* set))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSSet>(set)->clear(globalObject);
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE unsigned lengthAsDecimal(uint32_t value)
{
    static constexpr std::array<uint32_t, 10> powersOfTen { 0, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
    unsigned approximate = ((32 - clz(value | 1)) * 1233) >> 12;
    return approximate - (value < powersOfTen[approximate]) + 1;
}

// Backwards: the last digit comes to stand right ahead of `end`.
static ALWAYS_INLINE void writeAsDecimal(uint32_t value, Latin1Character* end)
{
    static constexpr char pairs[] = "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
        "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
    while (value >= 100) {
        unsigned pair = value % 100 * 2;
        value /= 100;
        *--end = pairs[pair + 1];
        *--end = pairs[pair];
    }
    if (value >= 10) {
        *--end = pairs[value * 2 + 1];
        *--end = pairs[value * 2];
    } else
        *--end = '0' + value;
}

// The parts are strings and integers: `a.id + "," + b.id`, the key of a Map more often than not. Where the result is short it is made in
// one piece, and hashed: no string for each integer, no rope, nothing left to do when it is looked up.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcatenateWithIntegers, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    static constexpr size_t maxLength = 64;
    std::array<uint32_t, 8> lengths;
    size_t length = 0;
    bool isShort = count <= lengths.size();
    for (uint32_t i = 0; i < count && isShort; ++i) {
        JSValue part = JSValue::decode(parts[i]);
        if (part.isInt32()) {
            int32_t integer = part.asInt32();
            lengths[i] = integer < 0 ? lengthAsDecimal(0u - static_cast<uint32_t>(integer)) + 1 : lengthAsDecimal(integer);
        } else {
            const StringImpl* impl = part.isString() ? asString(part)->tryGetValueImpl() : nullptr;
            isShort = impl && impl->is8Bit();
            lengths[i] = isShort ? impl->length() : 0;
        }
        length += lengths[i];
    }
    if (isShort && length <= maxLength) {
        std::span<Latin1Character> characters;
        String result = StringImpl::createUninitialized(length, characters);
        Latin1Character* cursor = characters.data();
        for (uint32_t i = 0; i < count; cursor += lengths[i++]) {
            JSValue part = JSValue::decode(parts[i]);
            if (part.isInt32()) {
                int32_t integer = part.asInt32();
                if (integer < 0)
                    *cursor = '-';
                writeAsDecimal(integer < 0 ? 0u - static_cast<uint32_t>(integer) : integer, cursor + lengths[i]);
                continue;
            }
            auto source = asString(part)->tryGetValueImpl()->span8();
            if (source.size() == 1)
                *cursor = source[0];
            else
                memcpy(cursor, source.data(), source.size());
        }
        result.impl()->hash();
        OPERATION_RETURN(scope, jsString(vm, WTF::move(result)));
    }

    JSString* result = jsEmptyString(vm);
    for (uint32_t i = 0; i < count; ++i) {
        JSString* part = JSValue::decode(parts[i]).toString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        result = jsString(globalObject, result, part);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMapNext, size_t, (Instance* instance, JSCell* map, EncodedJSValue table, uint32_t entry))
{
    return nextEntry<MapTraits>(instance, map, table, entry);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedSetNext, size_t, (Instance* instance, JSCell* set, EncodedJSValue table, uint32_t entry))
{
    return nextEntry<SetTraits>(instance, set, table, entry);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMapKeyAt, EncodedJSValue, (Instance*, JSCell* map, uint32_t entry))
{
    return partOfEntry<MapTraits>(map, entry, true);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMapValueAt, EncodedJSValue, (Instance*, JSCell* map, uint32_t entry))
{
    return partOfEntry<MapTraits>(map, entry, false);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedSetKeyAt, EncodedJSValue, (Instance*, JSCell* set, uint32_t entry))
{
    return partOfEntry<SetTraits>(set, entry, true);
}

namespace Typed {

using namespace B3;

// The family aot/builtins/map-set.def.

std::optional<Writes> writesOfInlineMapSet(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::MapSize:
    case InlineBuiltin::SetSize:
    case InlineBuiltin::MapNext:
    case InlineBuiltin::MapKeyAt:
    case InlineBuiltin::MapValueAt:
    case InlineBuiltin::SetNext:
    case InlineBuiltin::SetKeyAt:
        return Writes::Nothing;
    case InlineBuiltin::CollectionTable:
    case InlineBuiltin::MapNew:
    case InlineBuiltin::MapSet:
    case InlineBuiltin::SetAdd:
    case InlineBuiltin::MapClear:
    case InlineBuiltin::SetClear:
    case InlineBuiltin::MapDelete:
    case InlineBuiltin::SetDelete:
    case InlineBuiltin::MapIterator:
    case InlineBuiltin::SetIterator:
        return Writes::RuntimeState;
    // Hashing a string flattens a rope.
    case InlineBuiltin::MapGet:
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
        return arguments[0].primitives & Primitive::String ? Writes::RuntimeState : Writes::Nothing;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineMapSet(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    // Of the prelude, which hands over its receiver. Where that has been inlined, code that is never reached may have anything here by its type.
    auto collection = [&] {
        Operand operand = get(arguments[0]);
        return operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(in<Boxed>(arguments[0]));
    };
    auto entry = [&](unsigned argument) {
        return low32(in<Integer>(arguments[argument]));
    };
    // What follows reads a CompactTable in place, see there. With wide references its keys are pointers: left to the operations.
    bool readsInPlace = !Options::useAOTWideReferences();
    auto fieldOf = [&](LValue table, ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTTyped_runtimeState, table, offset);
    };
    auto compactTableOf = [&](CellPointer owner) {
        return m_out.loadPtr(owner.value(), m_heaps.JSMap_compactTable);
    };
    auto keysOf = [&](LValue table) {
        LValue bits = m_out.load8ZeroExt32(fieldOf(table, CompactTable::offsetOfIndexBits()));
        LValue hasShortSlots = m_out.belowOrEqual(m_out.load32(fieldOf(table, CompactTable::offsetOfCapacity())), m_out.constInt32(CompactTable::maxCapacityWithShortSlots));
        LValue sizeOfSlots = m_out.shl(m_out.select(hasShortSlots, m_out.constInt32(sizeof(uint16_t)), m_out.constInt32(sizeof(uint32_t))), bits);
        LValue sizeOfIndex = m_out.select(m_out.isZero32(bits), m_out.constInt32(CompactTable::maxCapacityWithTags), sizeOfSlots);
        return m_out.add(table, m_out.add(m_out.zeroExtPtr(sizeOfIndex), m_out.constIntPtr(CompactTable::offsetOfIndex())));
    };
    auto at = [&](LValue base, LValue index, unsigned shift) {
        return fieldOf(m_out.add(base, m_out.shl(index, m_out.constInt32(shift))), 0);
    };
    // The engine's own table, see JSOrderedHashTableHelper. What is no key or value in there is a count or an index, boxed as an int32.
    static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
    static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex() && JSMap::Helper::deletedEntryCountIndex() == JSSet::Helper::deletedEntryCountIndex());
    auto dataAt = [&](LValue base, ptrdiff_t index) {
        return m_out.address(m_heaps.OrderedHashTableData, base, index * sizeof(EncodedJSValue));
    };
    auto dataOf = [&](LValue storage) {
        return m_out.add(storage, m_out.constIntPtr(JSCellButterfly::offsetOfData()));
    };
    auto entriesOf = [&](LValue data) {
        LValue capacity = m_out.zeroExtPtr(m_out.load32(dataAt(data, JSMap::Helper::capacityIndex())));
        return m_out.add(data, m_out.shl(m_out.add(capacity, m_out.constIntPtr(JSMap::Helper::hashTableStartIndex())), m_out.constInt32(3)));
    };
    auto entryAt = [&](LValue entries, LValue number, bool isMap) {
        return m_out.add(entries, m_out.mul(number, m_out.constIntPtr((isMap ? JSMap::Helper::EntrySize : JSSet::Helper::EntrySize) * sizeof(EncodedJSValue))));
    };
    // Opaque, and the table alive until here: as Lowering::load() of a Store::Cell32.
    auto decoded = [&](LValue index, LValue table) {
        LValue cell = m_out.opaque(decodeCell32(index));
        ensureStillAliveHere(CellPointer(table));
        return cell;
    };
    auto next = [&](bool isMap) {
        CellPointer owner = collection();
        Boxed from = in<Boxed>(arguments[1]);
        Integer start = in<Integer>(arguments[2]);
        auto generally = [&] {
            return signExtended(isMap ? AOT_TYPED_OPERATION(MapNext, owner, from, low32(start)) : AOT_TYPED_OPERATION(SetNext, owner, from, low32(start)));
        };
        Type guarantee = Type::integer(Range { -1, INT32_MAX });
        if (!readsInPlace)
            return ensure(generally(), guarantee, type);
        LBasicBlock isUnchanged = m_out.newBlock();
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock isUsed = m_out.newBlock();
        LBasicBlock isDeleted = m_out.newBlock();
        LBasicBlock isAlive = m_out.newBlock();
        LBasicBlock isBehind = m_out.newBlock();
        LBasicBlock isNotCompact = m_out.newBlock();
        LBasicBlock isUnchangedStorage = m_out.newBlock();
        LBasicBlock storageLoop = m_out.newBlock();
        LBasicBlock isUsedInStorage = m_out.newBlock();
        LBasicBlock isDeletedInStorage = m_out.newBlock();
        LBasicBlock isAliveInStorage = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Int64, Int64);
        // The table the iteration is at is the one the collection has: then it is neither replaced nor cleared.
        LValue table = compactTableOf(owner);
        m_out.branch(m_out.bitAnd(m_out.notNull(table), m_out.equal(table, from.value())), unsure(isUnchanged), unsure(isNotCompact));

        m_out.appendTo(isNotCompact);
        LValue storage = m_out.loadPtr(owner.value(), m_heaps.JSMap_storage);
        m_out.branch(m_out.bitAnd(m_out.notNull(storage), m_out.equal(storage, from.value())), usually(isUnchangedStorage), rarely(otherwise));

        m_out.appendTo(isUnchangedStorage);
        LValue data = dataOf(storage);
        LValue usedInStorage = m_out.zeroExtPtr(m_out.add(m_out.load32(dataAt(data, JSMap::Helper::aliveEntryCountIndex())), m_out.load32(dataAt(data, JSMap::Helper::deletedEntryCountIndex()))));
        LValue entries = entriesOf(data);
        LValue deletedValue = m_out.loadPtr(fieldOf(fixedPointer(Instance::offsetOfVM()), VM::offsetOfOrderedHashTableDeletedValue()));
        ValueFromBlock firstInStorage = m_out.anchor(start.value());
        m_out.jump(storageLoop);

        m_out.appendTo(storageLoop);
        LValue entryInStorage = m_out.phi(Int64, firstInStorage);
        m_out.branch(m_out.below(entryInStorage, usedInStorage), usually(isUsedInStorage), rarely(isBehind));

        m_out.appendTo(isUsedInStorage);
        m_out.branch(m_out.notEqual(m_out.load64(dataAt(entryAt(entries, entryInStorage, isMap), 0)), deletedValue), usually(isAliveInStorage), rarely(isDeletedInStorage));

        m_out.appendTo(isDeletedInStorage);
        m_out.addIncomingToPhi(entryInStorage, m_out.anchor(m_out.add(entryInStorage, m_out.constInt64(1))));
        m_out.jump(storageLoop);

        m_out.appendTo(isAliveInStorage);
        join.add(Integer(entryInStorage));

        m_out.appendTo(isUnchanged);
        LValue used = m_out.zeroExtPtr(m_out.load32(table, m_heaps.AOTTypedCompactTable_used));
        LValue keys = keysOf(table);
        ValueFromBlock first = m_out.anchor(start.value());
        m_out.jump(loop);

        m_out.appendTo(loop);
        LValue entry = m_out.phi(Int64, first);
        m_out.branch(m_out.below(entry, used), usually(isUsed), rarely(isBehind));

        m_out.appendTo(isUsed);
        m_out.branch(m_out.notZero32(m_out.load32(at(keys, entry, 2))), usually(isAlive), rarely(isDeleted));

        m_out.appendTo(isDeleted);
        m_out.addIncomingToPhi(entry, m_out.anchor(m_out.add(entry, m_out.constInt64(1))));
        m_out.jump(loop);

        m_out.appendTo(isAlive);
        join.add(Integer(entry));

        m_out.appendTo(isBehind);
        join.add(integer(-1));

        m_out.appendTo(otherwise);
        join.add(generally());
        Operand result = join.finish();
        ensureStillAliveHere(owner);
        return ensure(result, guarantee, type);
    };
    // The entry is alive, in the table the collection has.
    auto partOfEntry = [&](bool isKey, auto generally) {
        if (!readsInPlace)
            return ensure(generally(), type);
        CellPointer owner = collection();
        LValue number = in<Integer>(arguments[1]).value();
        LBasicBlock isCompact = m_out.newBlock();
        LBasicBlock otherwise = m_out.newBlock();
        Join join(m_out, Rep::JSValue, Int64);
        LValue table = compactTableOf(owner);
        m_out.branch(m_out.notNull(table), unsure(isCompact), unsure(otherwise));

        m_out.appendTo(isCompact);
        LValue keys = keysOf(table);
        if (isKey)
            join.add(Boxed(decoded(m_out.zeroExtPtr(m_out.load32(at(keys, number, 2))), table)));
        else {
            LBasicBlock isNarrow = m_out.newBlock();
            LBasicBlock isWide = m_out.newBlock();
            LValue values = m_out.add(keys, m_out.shl(m_out.zeroExtPtr(m_out.load32(fieldOf(table, CompactTable::offsetOfCapacity()))), m_out.constInt32(2)));
            LValue width = m_out.load8ZeroExt32(fieldOf(table, CompactTable::offsetOfValueWidth()));
            m_out.branch(m_out.equal(width, m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Narrow))), unsure(isNarrow), unsure(isWide));
            m_out.appendTo(isNarrow);
            LValue index = m_out.zeroExtPtr(m_out.load32(at(values, number, 2)));
            join.add(Boxed(m_out.select(m_out.belowOrEqual(index, m_out.constInt64(undefinedCell32)), index, decoded(index, table))));
            m_out.appendTo(isWide);
            join.add(Boxed(m_out.load64(at(values, number, 3))));
        }

        m_out.appendTo(otherwise);
        join.add(Boxed(m_out.load64(dataAt(entryAt(entriesOf(dataOf(m_out.loadPtr(owner.value(), m_heaps.JSMap_storage))), number, which != InlineBuiltin::SetKeyAt), isKey ? 0 : 1))));
        Operand result = join.finish();
        ensureStillAliveHere(owner);
        return ensure(result.as<Boxed>(), type);
    };
    switch (which) {
    case InlineBuiltin::MapDelete:
    case InlineBuiltin::SetDelete: {
        CellPointer owner = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        Vector<std::pair<LValue, GPRReg>, 4> operands { { owner.value(), thisGPR }, { mapKey(arguments[0]).value(), argumentGPR(0) } };
        Boxed isRemoved(callICStub(which == InlineBuiltin::MapDelete ? Stub::MapDelete : Stub::SetDelete, Int64, operands, std::nullopt, OperationEffects { inPlace, true }));
        return yieldBoolean(unboxedBoolean(isRemoved), type);
    }
    case InlineBuiltin::MapIterator:
    case InlineBuiltin::SetIterator: {
        bool isMap = which == InlineBuiltin::MapIterator;
        auto& builtin = m_builtins.at(value.builtin());
        CellPointer owner = nonNil(value.child(0), m_program.stringOf(builtin.nameName));
        IterationKind kind = builtin.name == "entries"_s || (isMap && builtin.name == "@@iterator"_s) ? IterationKind::Entries : isMap && builtin.name == "keys"_s ? IterationKind::Keys : IterationKind::Values;
        Word32 encodedKind = word32(static_cast<int32_t>(kind));
        return ensure(isMap ? AOT_TYPED_OPERATION(NewMapIterator, owner, encodedKind) : AOT_TYPED_OPERATION(NewSetIterator, owner, encodedKind), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);
    }
    case InlineBuiltin::MapClear:
    case InlineBuiltin::SetClear: {
        // What has no table has no entries.
        bool isMap = which == InlineBuiltin::MapClear;
        CellPointer owner = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        LValue storage = m_out.loadPtr(owner.value(), isMap ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
        LValue compactTable = m_out.loadPtr(owner.value(), isMap ? m_heaps.JSMap_compactTable : m_heaps.JSSet_compactTable);
        LBasicBlock hasTable = m_out.newBlock();
        LBasicBlock hasNone = m_out.newBlock();
        LBasicBlock isCompact = m_out.newBlock();
        LBasicBlock isReset = m_out.newBlock();
        LBasicBlock generally = m_out.newBlock();
        Join join(m_out, Rep::Void, Void);
        m_out.branch(m_out.notZero64(m_out.bitOr(storage, compactTable)), unsure(hasTable), unsure(hasNone));
        m_out.appendTo(hasTable);
        m_out.branch(m_out.notNull(compactTable), usually(isCompact), rarely(generally));
        // As CompactTable::clear(): it has tags, and no iteration has been at it.
        m_out.appendTo(isCompact);
        static_assert(CompactTable::offsetOfState() == CompactTable::offsetOfIndexBits() + 3 && !static_cast<uint8_t>(CompactTable::State::Live));
        LValue kinds = m_out.load32(fieldOf(compactTable, CompactTable::offsetOfIndexBits()));
        m_out.branch(m_out.testIsZero32(kinds, m_out.constInt32(static_cast<int32_t>(0xff0000ff))), usually(isReset), rarely(generally));
        m_out.appendTo(isReset);
        didEmitWrite(Writes::RuntimeState);
        m_out.store32(m_out.int32Zero, compactTable, m_heaps.AOTTypedCompactTable_used);
        m_out.store32(m_out.int32Zero, compactTable, m_heaps.AOTTypedCompactTable_deleted);
        join.add({ });
        m_out.appendTo(generally);
        if (isMap)
            AOT_TYPED_OPERATION(MapClear, owner);
        else
            AOT_TYPED_OPERATION(SetClear, owner);
        join.add({ });
        m_out.appendTo(hasNone);
        join.add({ });
        join.finish();
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::CollectionTable: {
        RELEASE_ASSERT(JSMap::offsetOfStorage() == JSSet::offsetOfStorage() && JSMap::offsetOfCompactTable() == JSSet::offsetOfCompactTable());
        CellPointer owner = collection();
        LValue compactTable = m_out.loadPtr(owner.value(), m_heaps.JSMap_compactTable);
        LValue storage = m_out.loadPtr(owner.value(), m_heaps.JSMap_storage);
        LBasicBlock isCompact = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.notNull(compactTable), unsure(isCompact), unsure(continuation));
        m_out.appendTo(isCompact);
        didEmitWrite(Writes::RuntimeState);
        m_out.store32As8(m_out.constInt32(static_cast<int32_t>(CompactTable::State::Iterated)), fieldOf(compactTable, CompactTable::offsetOfState()));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        LValue table = m_out.select(m_out.notNull(compactTable), compactTable, m_out.select(m_out.notNull(storage), storage, m_out.constInt64(JSValue::encode(jsNull()))));
        ensureStillAliveHere(owner);
        return ensure(Boxed(table), type);
    }
    case InlineBuiltin::MapNext:
        return next(true);
    case InlineBuiltin::SetNext:
        return next(false);
    case InlineBuiltin::MapKeyAt:
        return partOfEntry(true, [&] { return AOT_TYPED_OPERATION(MapKeyAt, collection(), entry(1)); });
    case InlineBuiltin::MapValueAt:
        return partOfEntry(false, [&] { return AOT_TYPED_OPERATION(MapValueAt, collection(), entry(1)); });
    case InlineBuiltin::SetKeyAt:
        return partOfEntry(true, [&] { return AOT_TYPED_OPERATION(SetKeyAt, collection(), entry(1)); });
    default:
        return lowerMapOrSetBuiltin(which, result, value, inPlace);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
