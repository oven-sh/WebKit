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
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static KeyComparison keyComparisonOf(const Type& key)
{
    if (key.primitives & Primitive::AnyString)
        return key.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull) ? KeyComparison::String : KeyComparison::AnyValue;
    if (key.primitives & Primitive::BigInt)
        return KeyComparison::AnyValue;
    if (!(key.primitives & Primitive::Number))
        return KeyComparison::Bits;
    switch (defaultRepresentationOf(key)) {
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
    case Representation::LiteralString:
    case Representation::RawWord:
        return KeyComparison::AnyValue;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StubKey lookupStubFor(HashTableOwner owner, bool returnsValue, KeyComparison comparison)
{
    switch (owner) {
    case HashTableOwner::Map:
        return returnsValue ? mapGet(comparison) : mapHas(comparison);
    case HashTableOwner::Set:
        return setHas(comparison);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

HashTableBuiltinEmitter::HashTableBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Boxed HashTableBuiltinEmitter::normalizedNumber(const OperationContext& context, Number key)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto anyNumber = [&] {
        Boxed boxedKey = conversions.boxNumber(key);
        return select(equal(key, number(0)), boxInt32(word32(0)), boxedKey);
    };
    if (context.shape() != CodeShape::Fast)
        return anyNumber();

    LBasicBlock fitsInt32 = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    Word32 truncated = low32(conversions.toInteger(key));
    m_out.branch(equal(toNumber(truncated), key).value(), normalEdge(fitsInt32), unlikelyEdge(otherwise));

    m_out.appendTo(fitsInt32);
    join.add(boxInt32(truncated));

    m_out.appendTo(otherwise);
    join.add(anyNumber());
    return join.finish();
}

Boxed HashTableBuiltinEmitter::normalizedKey(const OperationContext& context, ValueID id)
{
    const Type& type = typeOf(id);
    Boxed key = boxedOperand(context, id);
    bool calleeNormalizes = keyComparisonOf(type) != KeyComparison::Bits;
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
    join.add(normalizedNumber(context, unboxDouble(key)));
    return join.finish();
}

Word32 HashTableBuiltinEmitter::hashOfBits(Boxed key)
{
    LValue a = m_out.bitXor(key.value(), loadImmutable64(AOT_INSTANCE_OFFSET(numbers.hashSecrets)).value());
    LValue b = m_out.bitXor(key.value(), loadImmutable64(AOT_INSTANCE_OFFSET(numbers.hashSecrets) + sizeof(uint64_t)).value());
    return Word32(m_out.castToInt32(m_out.bitXor(m_out.mul(a, b), m_out.uMulHigh(a, b))));
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

void HashTableBuiltinEmitter::lookUp(const InlineCall& call, HashTableOwner owner, bool returnsValue)
{
    RELEASE_ASSERT(owner == HashTableOwner::Map || !returnsValue);
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    Boxed answer(callStub(call.context(), lookupStubFor(owner, returnsValue, comparison), { table.value(), key.value() }).first);
    if (returnsValue)
        finish(call, answer);
    else
        finish(call, unboxBoolean(answer));
}

void HashTableBuiltinEmitter::insert(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
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
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
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
    lookUp(call, HashTableOwner::Map, true);
}

void HashTableBuiltinEmitter::generateMapHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Map, false);
}

void HashTableBuiltinEmitter::generateSetHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Set, false);
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

KeyPartsLookupEmitter::KeyPartsLookupEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Pointer KeyPartsLookupEmitter::partsBuffer(const ValueContext& value)
{
    Vector<Boxed, 8> parts;
    for (ValueID part : value->partsOperands())
        parts.append(boxedOperand(value, part));
    return argumentBuffer(parts.span());
}

void KeyPartsLookupEmitter::generateMapGetByKeyParts(const ValueContext& value)
{
    Word32 count = word32(static_cast<int32_t>(value->partsOperands().size()));
    define(value, callOperation<Boxed>(value, RuntimeOperation::MapGetByKeyParts, { cellOperand(value->mapOperand()), count, partsBuffer(value) }));
}

void KeyPartsLookupEmitter::generateHasByKeyParts(const ValueContext& value)
{
    Word32 count = word32(static_cast<int32_t>(value->partsOperands().size()));
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::HasByKeyParts, { cellOperand(value->collectionOperand()), count, partsBuffer(value) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
