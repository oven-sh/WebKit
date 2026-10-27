/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMapSetBuiltinEmitter.h"

#if ENABLE(AOT)

#include "IterationKind.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

MapSetBuiltinEmitter::MapSetBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void MapSetBuiltinEmitter::createFromIterable(const InlineCall& call, HashTableOwner owner)
{
    Boxed iterable = boxedOperand(call.context(), call.argument(0));
    RuntimeOperation operation = owner == HashTableOwner::Map ? RuntimeOperation::TryNewMapFromEntries : RuntimeOperation::TryNewSetFromIterable;
    CellPointer created = callOperation<CellPointer>(call.context(), operation, { iterable });
    missUnless(call, Condition(m_out.notNull(created.value())));
    finish(call, created);
}

void MapSetBuiltinEmitter::clear(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    LBasicBlock hasEntries = newBlock();
    LBasicBlock done = newBlock();
    branch(isNotZero(emitter<HashTableBuiltinEmitter>().sizeOf(table, owner)), { normalEdge(hasEntries), normalEdge(done) });

    m_out.appendTo(hasEntries);
    callVoidOperation(call.context(), owner == HashTableOwner::Map ? RuntimeOperation::MapClear : RuntimeOperation::SetClear, { table });
    m_out.jump(done);

    m_out.appendTo(done);
    finishWithUndefined(call);
}

void MapSetBuiltinEmitter::createIterator(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    Word32 kind = word32(static_cast<int32_t>(call.parameter<IterationKind>(0)));
    RuntimeOperation operation = owner == HashTableOwner::Map ? RuntimeOperation::NewMapIterator : RuntimeOperation::NewSetIterator;
    finish(call, callOperation<CellPointer>(call.context(), operation, { table, kind }));
}

void MapSetBuiltinEmitter::lookUpWeakTable(const InlineCall& call, WeakLookup lookup)
{
    CellPointer table = cellOperand(call.receiver());
    Boxed key = boxedOperand(call.context(), call.argument(0));
    if (call.context().shape() != CodeShape::Compact) {
        probeWeakTable(call, lookup, table, key);
        return;
    }
    auto lookUpInStub = [&](StubKey stub) {
        return Boxed(callStub(call.context(), stub, { table.value(), key.value() }).first);
    };
    switch (lookup) {
    case WeakLookup::MapGet:
        finish(call, lookUpInStub(weakMapGet()));
        return;
    case WeakLookup::MapHas:
        finish(call, unboxBoolean(lookUpInStub(weakMapHas())));
        return;
    case WeakLookup::SetHas:
        finish(call, unboxBoolean(lookUpInStub(weakSetHas())));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MapSetBuiltinEmitter::probeWeakTable(const InlineCall& call, WeakLookup lookup, CellPointer table, Boxed key)
{
    using MapBucket = WeakMapBucket<WeakMapBucketDataKeyValue>;
    using SetBucket = WeakMapBucket<WeakMapBucketDataKey>;
    static_assert(hasOneBitSet(sizeof(MapBucket)) && hasOneBitSet(sizeof(SetBucket)));
    static_assert(JSWeakMap::offsetOfBuffer() == JSWeakSet::offsetOfBuffer() && JSWeakMap::offsetOfCapacity() == JSWeakSet::offsetOfCapacity());
    static_assert(MapBucket::offsetOfKey() == SetBucket::offsetOfKey());
    size_t bucketSize = lookup == WeakLookup::SetHas ? sizeof(SetBucket) : sizeof(MapBucket);
    LBasicBlock mayBeKey = newBlock();
    LBasicBlock loop = newBlock();
    LBasicBlock isOtherKey = newBlock();
    LBasicBlock advance = newBlock();
    LBasicBlock found = newBlock();
    LBasicBlock absent = newBlock();
    if (typeOf(call.argument(0)).isObject())
        m_out.jump(mayBeKey);
    else
        branch(isCell(key), { normalEdge(mayBeKey), rareEdge(absent) });

    m_out.appendTo(mayBeKey);
    ValueFromBlock first = m_out.anchor(emitter<HashTableBuiltinEmitter>().hashOfBits(key).value());
    LValue buffer = m_out.loadPtr(table.value(), m_heaps.WeakMapImpl_buffer);
    LValue mask = m_out.sub(m_out.load32(table.value(), m_heaps.WeakMapImpl_capacity), m_out.int32One);
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue unmasked = m_out.phi(B3::Int32, first);
    LValue index = m_out.bitAnd(unmasked, mask);
    LValue bucket = m_out.add(buffer, m_out.shl(m_out.zeroExtPtr(index), getLSBSet(bucketSize)));
    LValue bucketKey = m_out.loadPtr(bucket, m_heaps.WeakMapBucket_key);
    m_out.branch(m_out.equal(bucketKey, unboxCell(key).value()), normalEdge(found), normalEdge(isOtherKey));

    m_out.appendTo(isOtherKey);
    m_out.branch(m_out.isNull(bucketKey), normalEdge(absent), normalEdge(advance));

    m_out.appendTo(advance);
    m_out.addIncomingToPhi(unmasked, m_out.anchor(m_out.add(index, m_out.int32One)));
    m_out.jump(loop);

    m_out.appendTo(found);
    if (lookup == WeakLookup::MapGet) {
        Join<Boxed> join(m_out);
        join.add(Boxed(m_out.load64(bucket, m_heaps.WeakMapBucket_value)));
        m_out.appendTo(absent);
        join.add(undefined());
        finish(call, join.finish());
        return;
    }
    Join<Condition> join(m_out);
    join.add(boolean(true));
    m_out.appendTo(absent);
    join.add(boolean(false));
    finish(call, join.finish());
}

void MapSetBuiltinEmitter::generateSetFromIterable(const InlineCall& call)
{
    createFromIterable(call, HashTableOwner::Set);
}

void MapSetBuiltinEmitter::generateMapFromEntries(const InlineCall& call)
{
    createFromIterable(call, HashTableOwner::Map);
}

void MapSetBuiltinEmitter::generateWeakMapGet(const InlineCall& call)
{
    lookUpWeakTable(call, WeakLookup::MapGet);
}

void MapSetBuiltinEmitter::generateWeakMapHas(const InlineCall& call)
{
    lookUpWeakTable(call, WeakLookup::MapHas);
}

void MapSetBuiltinEmitter::generateWeakSetHas(const InlineCall& call)
{
    lookUpWeakTable(call, WeakLookup::SetHas);
}

void MapSetBuiltinEmitter::generateMapDelete(const InlineCall& call)
{
    emitter<HashTableBuiltinEmitter>().remove(call, HashTableOwner::Map);
}

void MapSetBuiltinEmitter::generateSetDelete(const InlineCall& call)
{
    emitter<HashTableBuiltinEmitter>().remove(call, HashTableOwner::Set);
}

void MapSetBuiltinEmitter::generateMapClear(const InlineCall& call)
{
    clear(call, HashTableOwner::Map);
}

void MapSetBuiltinEmitter::generateSetClear(const InlineCall& call)
{
    clear(call, HashTableOwner::Set);
}

void MapSetBuiltinEmitter::generateMapIterator(const InlineCall& call)
{
    createIterator(call, HashTableOwner::Map);
}

void MapSetBuiltinEmitter::generateSetIterator(const InlineCall& call)
{
    createIterator(call, HashTableOwner::Set);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
