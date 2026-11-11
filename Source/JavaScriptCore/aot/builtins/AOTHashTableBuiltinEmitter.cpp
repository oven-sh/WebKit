/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHashTableBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTConversionEmitter.h"
#include "AOTRepresentation.h"

namespace JSC { namespace AOT { namespace Typed {

static KeyComparison keyComparisonForType(const Type& key)
{
    if (key.primitives & Primitive::AnyString)
        return key.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull) ? KeyComparison::String : KeyComparison::AnyValue;
    if (key.primitives & Primitive::BigInt)
        return KeyComparison::AnyValue;
    if (!(key.primitives & Primitive::Number))
        return KeyComparison::Bits;
    switch (representationOf(key)) {
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return KeyComparison::Bits;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        return KeyComparison::AnyValue;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StubKey lookupStubFor(HashTableOwner owner, LookupAnswer answer, KeyComparison comparison)
{
    switch (owner) {
    case HashTableOwner::Map:
        return answer == LookupAnswer::IsPresent ? mapHas(comparison) : mapGet(comparison);
    case HashTableOwner::Set:
        return setHas(comparison);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

HashTableBuiltinEmitter::HashTableBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Boxed HashTableBuiltinEmitter::normalizedNumber(Number key)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    LBasicBlock fitsInt32 = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    Word32 truncated = conversions.truncateToWord(key);
    m_out.branch(equal(toNumber(truncated), key).value(), normalEdge(fitsInt32), unlikelyEdge(otherwise));

    m_out.appendTo(fitsInt32);
    join.add(boxInt32(truncated));

    m_out.appendTo(otherwise);
    join.add(conversions.boxDouble(conversions.purified(key)));
    return join.finish();
}

Boxed HashTableBuiltinEmitter::normalizedKey(const OperationContext& context, ValueID id)
{
    const Type& type = typeOf(id);
    Boxed key = boxedOperand(context, id);
    bool calleeNormalizes = keyComparisonForType(type) != KeyComparison::Bits;
    if (calleeNormalizes || !(type.primitives & Primitive::Number))
        return key;

    Condition isBoxedDouble = logicalNot(isInt32(key));
    if (!type.isNumber())
        isBoxedDouble = logicalAnd(isNumber(key), isBoxedDouble);
    bool numbersFitInt32 = !(type.primitives & Primitive::Double) && type.integers.isSubsetOf(Range::int32());
    LBasicBlock needsNormalization = newBlock();
    LBasicBlock isNormalized = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isBoxedDouble.value(), numbersFitInt32 ? unlikelyEdge(needsNormalization) : normalEdge(needsNormalization), normalEdge(isNormalized));

    m_out.appendTo(isNormalized);
    join.add(key);

    m_out.appendTo(needsNormalization);
    join.add(normalizedNumber(unboxDouble(key)));
    return join.finish();
}

Word32 HashTableBuiltinEmitter::hashOfBits(Boxed key)
{
    LValue a = m_out.bitXor(key.value(), loadImmutable64(AOT_INSTANCE_OFFSET(numbers.hashSecrets)).value());
    LValue b = m_out.bitXor(key.value(), loadImmutable64(AOT_INSTANCE_OFFSET(numbers.hashSecrets) + sizeof(uint64_t)).value());
    LValue lowerHalf = m_out.mul(a, b);
    return Word32(m_out.castToInt32(m_out.bitXor(lowerHalf, m_out.uMulHigh(a, b))));
}

Integer HashTableBuiltinEmitter::sizeOf(CellPointer table, HashTableOwner owner)
{
    CellPointer storage = loadField<CellPointer>(table, owner == HashTableOwner::Map ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
    return either<Integer>(Condition(m_out.notNull(storage.value())), [&] {
        return zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), storage.value(), CompactTable::offsetOfAliveCount()))));
    }, [&] {
        return integer(0);
    });
}

Operand HashTableBuiltinEmitter::lookUpByStub(const OperationContext& context, CellPointer owner, Boxed key, StubKey stub, LookupAnswer answer)
{
    Boxed found(callStub(context, stub, { owner.value(), key.value() }).first);
    switch (answer) {
    case LookupAnswer::IsPresent:
        return unboxBoolean(found);
    case LookupAnswer::Value:
        return found;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand HashTableBuiltinEmitter::search(const OperationContext& context, ValueID collection, ValueID keyOfSearch, HashTableOwner owner, LookupAnswer answer)
{
    RELEASE_ASSERT(owner == HashTableOwner::Map || answer == LookupAnswer::IsPresent);

    CellPointer table = cellOperand(collection);
    KeyComparison comparison = keyComparisonForType(typeOf(keyOfSearch));
    Boxed key = normalizedKey(context, keyOfSearch);
    return lookUpByStub(context, table, key, lookupStubFor(owner, answer, comparison), answer);
}

Integer HashTableBuiltinEmitter::findPlace(const OperationContext& context, ValueID map, ValueID keyOfSearch)
{
    CellPointer table = cellOperand(map);
    KeyComparison comparison = keyComparisonForType(typeOf(keyOfSearch));
    Boxed key = normalizedKey(context, keyOfSearch);
    return Integer(callStub(context, findKeyInMap(comparison), { table.value(), key.value() }).first);
}

void HashTableBuiltinEmitter::lookUp(const InlineCall& call, HashTableOwner owner, LookupAnswer answer)
{
    Operand found = search(call.context(), call.receiver(), call.argument(0), owner, answer);
    switch (answer) {
    case LookupAnswer::IsPresent:
        finish(call, found.as<Condition>());
        return;
    case LookupAnswer::Value:
        finish(call, found.as<Boxed>());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void HashTableBuiltinEmitter::insert(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonForType(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    switch (owner) {
    case HashTableOwner::Map:
        callStub(call.context(), mapSet(comparison), { table.value(), key.value(), boxedOperand(call.context(), call.argument(1)).value() });
        break;
    case HashTableOwner::Set:
        callStub(call.context(), setAdd(comparison), { table.value(), key.value() });
        break;
    }
    finish(call, table);
}

void HashTableBuiltinEmitter::remove(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonForType(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    StubKey stub = owner == HashTableOwner::Map ? mapDelete(comparison) : setDelete(comparison);
    finish(call, unboxBoolean(Boxed(callStub(call.context(), stub, { table.value(), key.value() }).first)));
}

void HashTableBuiltinEmitter::generateSize(const InlineCall& call, HashTableOwner owner)
{
    finish(call, sizeOf(cellOperand(call.receiver()), owner), Range::length());
}

void HashTableBuiltinEmitter::generateNewMap(const InlineCall& call)
{
    finish(call, CellPointer(callStub(call.context(), newMap(), { }).first));
}

void HashTableBuiltinEmitter::generateNewSet(const InlineCall& call)
{
    finish(call, CellPointer(callStub(call.context(), newSet(), { }).first));
}

void HashTableBuiltinEmitter::generateMapGet(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Map, LookupAnswer::Value);
}

void HashTableBuiltinEmitter::generateMapHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Map, LookupAnswer::IsPresent);
}

void HashTableBuiltinEmitter::generateSetHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Set, LookupAnswer::IsPresent);
}

void HashTableBuiltinEmitter::generateMapSet(const InlineCall& call)
{
    insert(call, HashTableOwner::Map);
}

void HashTableBuiltinEmitter::generateSetAdd(const InlineCall& call)
{
    insert(call, HashTableOwner::Set);
}

void HashTableBuiltinEmitter::generateMapSize(const InlineCall& call)
{
    generateSize(call, HashTableOwner::Map);
}

void HashTableBuiltinEmitter::generateSetSize(const InlineCall& call)
{
    generateSize(call, HashTableOwner::Set);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
