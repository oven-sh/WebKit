/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinWords.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTShapeTestEmitter.h"
#include "JSObject.h"
#include "JSTypeInfo.h"
#include "Structure.h"

namespace JSC { namespace AOT { namespace Typed {

ObjectBuiltinEmitter::ObjectBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

CellPointer ObjectBuiltinEmitter::allocateEmptyObject(InlineAllocation kind, std::optional<Integer> header, LBasicBlock slowPath)
{
    CellAllocationEmitter& allocation = emitter<CellAllocationEmitter>();
    CodeGenerator::InitializationScope scope(generator());
    CellPointer object = allocation.allocateObject(kind, slowPath);
    if (header)
        allocation.storeHeader(object, *header);
    for (PropertyOffset slot = 0; slot < static_cast<PropertyOffset>(JSFinalObject::defaultInlineCapacity); ++slot)
        m_out.store64(m_out.int64Zero, inlineProperty(object, slot));
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return object;
}

CellPointer ObjectBuiltinEmitter::createObjectWithPrototype(const InlineCall& call, CellPointer prototype)
{
    using CacheEntry = ObjectCreateCacheEntry;
    constexpr unsigned logEntrySize = 4;
    static_assert(sizeof(CacheEntry) == (1u << logEntrySize) && ObjectBuiltinWords::objectCreateShift >= logEntrySize);
    static_assert(ObjectBuiltinWords::objectCreateIndex(0x7f40) == ((0x7f40 >> ObjectBuiltinWords::objectCreateShift) & (ObjectBuiltinWords::objectCreateSize - 1)));
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    TypedPointer cache = instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(builtins.objects.objectCreate));
    LValue offset = m_out.bitAnd(m_out.lShr(prototype.value(), m_out.constInt32(ObjectBuiltinWords::objectCreateShift - logEntrySize)), m_out.constIntPtr((ObjectBuiltinWords::objectCreateSize - 1) << logEntrySize));
    LValue entry = m_out.add(cache.base(), offset);
    CellPointer cachedPrototype(m_out.loadPtr(m_out.address(*cache.heap(), entry, cache.offset() + OBJECT_OFFSETOF(CacheEntry, prototype))));
    branchUnless(equal(cachedPrototype, prototype), slowPath);
    Integer header(m_out.load64(m_out.address(*cache.heap(), entry, cache.offset() + OBJECT_OFFSETOF(CacheEntry, header))));
    join.add(allocateEmptyObject(InlineAllocation::DefaultFinalObject, header, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(call.context(), RuntimeOperation::ObjectCreate, { prototype }, { .isOnRarePath = true }));
    return join.finish();
}

// Every object without a prototype starts with one Structure of the realm, which the allocation has in its header.
CellPointer ObjectBuiltinEmitter::createObjectWithoutPrototype(const InlineCall& call)
{
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(allocateEmptyObject(InlineAllocation::NullPrototypeObject, std::nullopt, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(call.context(), RuntimeOperation::ObjectCreate, { CellPointer(m_out.intPtrZero) }, { .isOnRarePath = true }));
    return join.finish();
}

void ObjectBuiltinEmitter::generateObjectCreate(const InlineCall& call)
{
    ValueID prototype = call.argument(0);
    Join<CellPointer> join(m_out);
    if (typeOf(prototype).primitives & Primitive::Null) {
        const TypeTestPlan& plan = typeTestPlanFor(m_function.at(prototype).type, representationOf(prototype), TypeTable::null);
        LBasicBlock isNull = newBlock();
        LBasicBlock isNotNull = newBlock();
        branchOnType(get(prototype), plan, { normalEdge(isNull), normalEdge(isNotNull) });
        m_out.appendTo(isNull);
        join.add(createObjectWithoutPrototype(call));
        m_out.appendTo(isNotNull);
    }
    join.add(createObjectWithPrototype(call, objectOrMiss(call, prototype)));
    finish(call, join.finish());
}

void ObjectBuiltinEmitter::generateGetPrototypeOf(const InlineCall& call)
{
    CellPointer object = objectOrMiss(call, call.argument(0));
    missUnless(call, hasNoBits(loadByte(object, m_heaps.JSCell_typeInfoFlags), OverridesGetPrototype));
    Boxed prototype = loadField<Boxed>(loadStructure(object), m_heaps.Structure_prototype);
    missUnless(call, isNotEmpty(prototype));
    finish(call, prototype);
}

// A transition is recorded for a compiled object that is on no chain, so what is left of the built-in is the test of extensibility and the store.
void ObjectBuiltinEmitter::generateSetPrototypeOf(const InlineCall& call)
{
    ValueID argument = call.argument(0);
    CellPointer object = objectOrMiss(call, argument);
    Boxed prototype = boxedOperand(call.context(), call.argument(1));
    if (generator().validates())
        missUnless(call, Condition(m_out.booleanFalse));
    Word32 structureID = emitter<AccessEmitter>().findPrototypeTransition(object, prototype, missBlockOf(call));
    missUnless(call, hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(object), orderWordIsNotExtensible));
    emitter<AccessEmitter>().storeStructureAfterPrototypeTransition(object, structureID);
    emitter<BarrierEmitter>().barrier(call.context(), object);
    switch (call.parameter<PrototypeChange>(0)) {
    case PrototypeChange::ObjectSetPrototypeOf:
        finish(call, get(argument), typeOf(argument));
        return;
    case PrototypeChange::ReflectSetPrototypeOf:
        finish(call, Condition(m_out.booleanTrue));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition ObjectBuiltinEmitter::isSameValue(const OperationContext& context, ValueID a, ValueID b)
{
    const Type& left = typeOf(a);
    const Type& right = typeOf(b);
    auto bitsOf = [&](Number x) {
        return Integer(m_out.bitCast(x.value(), B3::Int64));
    };
    auto areSameNumber = [&](Number x, Number y) {
        return logicalOr(equal(bitsOf(x), bitsOf(y)), logicalAnd(notEqualOrUnordered(x, x), notEqualOrUnordered(y, y)));
    };
    if (left.isInteger() && right.isInteger())
        return equal(integerOperand(a), integerOperand(b));
    if (left.isNumber() && right.isNumber())
        return areSameNumber(numberOperand(a), numberOperand(b));
    Boxed x = boxedOperand(context, a);
    Boxed y = boxedOperand(context, b);
    if (!(left.primitives & Primitive::Number) || !(right.primitives & Primitive::Number))
        return equal(x, y);
    LBasicBlock numbers = newBlock();
    LBasicBlock others = newBlock();
    Join<Condition> join(m_out);
    branch(logicalAnd(isNumber(x), isNumber(y)), { normalEdge(numbers), normalEdge(others) });
    m_out.appendTo(numbers);
    join.add(areSameNumber(numberToDouble(x), numberToDouble(y)));
    m_out.appendTo(others);
    join.add(equal(x, y));
    return join.finish();
}

void ObjectBuiltinEmitter::generateObjectIs(const InlineCall& call)
{
    ValueID a = call.argument(0);
    ValueID b = call.argument(1);
    Condition isSame = isSameValue(call.context(), a, b);
    if (call.canMiss()) {
        Boxed x = boxedOperand(call.context(), a);
        Boxed y = boxedOperand(call.context(), b);
        LBasicBlock differentBits = newBlock();
        LBasicBlock cells = newBlock();
        LBasicBlock decided = newBlock();
        branch(isSame, { normalEdge(decided), normalEdge(differentBits) });
        m_out.appendTo(differentBits);
        branch(logicalAnd(isCell(x), isCell(y)), { normalEdge(cells), normalEdge(decided) });
        m_out.appendTo(cells);
        branch(hasCellTypeIn(unboxCell(x), { StringType, HeapBigIntType }), { rareEdge(missBlockOf(call)), normalEdge(decided) });
        m_out.appendTo(decided);
    }
    finish(call, isSame);
}

void ObjectBuiltinEmitter::generateCallObjectConstructor(const InlineCall& call)
{
    finish(call, objectOrMiss(call, call.argument(0)));
}

void ObjectBuiltinEmitter::generateObjectFreeze(const InlineCall& call)
{
    static_assert(orderWordHasExtraProperties < orderWordIsFrozen && orderWordIsFrozen < orderWordIsSealed && orderWordIsSealed < orderWordIsNotExtensible);
    ValueID argument = call.argument(0);
    const Type& type = typeOf(argument);
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    Word32 frozen = word32(static_cast<int32_t>(orderWordIntegrityBits));
    CellPointer cell = objectOrMiss(call, argument);
    if (!shapeTests.hasOnlyCompiledObjects(type))
        missUnless(call, shapeTests.hasShapeIn(cell, type.layouts, type));
    missUnless(call, hasNoBits(loadField<Word32>(loadStructure(cell), m_heaps.Structure_bitField), Structure::s_mayBePrototypeBits));
    Word32 word = shapeTests.orderWordOf(cell);
    missUnless(call, below(word, word32(orderWordHasExtraProperties)));
    storeField(cell, m_heaps.AOTObject_orderWord, bitOr(word, frozen));
    finish(call, get(argument), type);
}

void ObjectBuiltinEmitter::generateObjectPrototypeToString(const InlineCall& call)
{
    CellPointer object = objectOrMiss(call, call.receiver());
    CellPointer previousOrRareData = loadField<CellPointer>(loadStructure(object), m_heaps.Structure_previousOrRareData);
    missUnless(call, Condition(m_out.notNull(previousOrRareData.value())));
    missUnless(call, logicalNot(hasCellType(previousOrRareData, StructureType)));
    Pointer cache = loadField<Pointer>(previousOrRareData, m_heaps.StructureRareData_specialPropertyCache);
    missUnless(call, Condition(m_out.notNull(cache.value())));
    CellPointer text = loadField<CellPointer>(cache, m_heaps.SpecialPropertyCache_cachedToStringTagValue);
    missUnless(call, above(asPointer(text), pointerConstant(std::bit_cast<uintptr_t>(JSCell::seenMultipleCalleeObjects()))));
    finish(call, text);
}

void ObjectBuiltinEmitter::generateReturnThis(const InlineCall& call)
{
    ValueID receiver = call.receiver();
    finish(call, get(receiver), typeOf(receiver));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
