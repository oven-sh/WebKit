/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTObjectWords.h"
#include "AOTPlacement.h"
#include "AOTPropertyCachesFront.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "JSTypeInfo.h"

namespace JSC { namespace AOT { namespace Typed {

static bool operandIsHomeObject(HomeObjectKind kind)
{
    switch (kind) {
    case HomeObjectKind::Object:
        return true;
    case HomeObjectKind::ClassPrototype:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed AccessEmitter::keyOperand(const OperationContext& context, PropertyKeyID key, ValueID computedKey)
{
    if (key)
        return boxCell(emitter<ConstantEmitter>().keyConstant(context, key));
    return in<Boxed>(computedKey);
}

void AccessEmitter::defineAccessor(const ValueContext& value, uint32_t accessorFlags)
{
    Boxed object = boxCell(in<CellPointer>(value->objectOperand()));
    Boxed key = keyOperand(value, value->key(), value->computedKeyOperand());
    Boxed function = boxCell(in<CellPointer>(value->functionOperand()));
    callVoidOperation(value, RuntimeOperation::DefineAccessor, { object, key, function, word32(static_cast<int32_t>(accessorFlags)) });
}

void AccessEmitter::generateDefineGetter(const ValueContext& value)
{
    defineAccessor(value, accessorIsEnumerable | accessorIsGetter);
}

void AccessEmitter::generateDefineSetter(const ValueContext& value)
{
    defineAccessor(value, accessorIsEnumerable);
}

void AccessEmitter::generateSetFunctionName(const ValueContext& value)
{
    CellPointer function = in<CellPointer>(value->functionOperand());
    Boxed name = in<Boxed>(value->computedKeyOperand());
    callVoidOperation(value, RuntimeOperation::SetFunctionName, { function, name, word32(static_cast<int32_t>(value->methodKind())) });
}

Word32 AccessEmitter::findPrototypeTransition(CellPointer object, Boxed prototype, LBasicBlock isUnknown)
{
    static_assert(sizeof(PrototypeTransition) == 16);
    static_assert(hasOneBitSet(numberOfPrototypeTransitions));
    static_assert(indexOfPrototypeTransition(5 << prototypeTransitionHashShift, 3 << prototypeTransitionHashShift) == 6);
    LBasicBlock isKnown = newBlock();
    const B3::AbstractHeap& heap = m_heaps.collectorState;
    Word32 before = loadStructureID(object);
    Word32 hash = shiftRightUnsigned(bitXor(before, low32(prototype)), word32(prototypeTransitionHashShift));
    Integer index = zeroExtended(bitAnd(hash, word32(numberOfPrototypeTransitions - 1)));
    LValue transition = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.prototypeTransitions)), index, 4).value();
    Condition hasPrototype = equal(Boxed(m_out.load64(m_out.address(heap, transition, OBJECT_OFFSETOF(PrototypeTransition, prototype)))), prototype);
    Condition hasStructure = equal(Word32(m_out.load32(m_out.address(heap, transition, OBJECT_OFFSETOF(PrototypeTransition, before)))), before);
    branch(logicalAnd(hasPrototype, hasStructure), { normalEdge(isKnown), rareEdge(isUnknown) });

    m_out.appendTo(isKnown);
    return Word32(m_out.load32(m_out.address(heap, transition, OBJECT_OFFSETOF(PrototypeTransition, after))));
}

void AccessEmitter::generateSetLiteralPrototype(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    Boxed prototype = in<Boxed>(value->prototypeOperand());
    if (value.shape() == CodeShape::Compact) {
        callVoidOperation(value, RuntimeOperation::SetLiteralPrototype, { object, prototype });
        return;
    }

    LBasicBlock isUnknown = newBlock();
    LBasicBlock continuation = newBlock();
    storeField(object, m_heaps.JSCell_structureID, findPrototypeTransition(object, prototype, isUnknown));
    emitter<BarrierEmitter>().barrier(value, object);
    m_out.jump(continuation);

    m_out.appendTo(isUnknown);
    callVoidOperation(value, RuntimeOperation::SetLiteralPrototype, { object, prototype }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// A key that the operation takes is a string or a symbol, so a number among the keys is the number of an identifier.
Boxed AccessEmitter::identifierOperand(const OperationContext& context, PropertyKeyID key)
{
    return boxedConstant(jsNumber(findIdentifierNumber(context, key).value_or(0)));
}

// The keys that the source text names lie in the image as a run, which the operation gets by its number plus one.
AccessEmitter::ExcludedKeys AccessEmitter::excludedKeysOf(const ValueContext& value)
{
    std::optional<Request> run = value->request();
    Vector<Boxed, 8> keys;
    if (!run) {
        for (PropertyKeyID key : value->excludedKeys())
            keys.append(identifierOperand(value, key));
    }
    for (ValueID key : value->excludedOperands())
        keys.append(in<Boxed>(key));
    return { word32(run ? static_cast<int32_t>(run->value() + 1) : 0), word32(static_cast<int32_t>(keys.size())), argumentBuffer(keys.span()) };
}

void AccessEmitter::generateCopyDataProperties(const ValueContext& value)
{
    CellPointer target = in<CellPointer>(value->targetOperand());
    Boxed source = in<Boxed>(value->sourceOperand());
    ExcludedKeys excluded = excludedKeysOf(value);
    callVoidOperation(value, RuntimeOperation::CopyDataProperties, { target, source, excluded.run, excluded.count, excluded.keys });
}

void AccessEmitter::generateCopyObject(const ValueContext& value)
{
    Boxed source = in<Boxed>(value->sourceOperand());
    ExcludedKeys excluded = excludedKeysOf(value);
    define(value, callOperation<CellPointer>(value, RuntimeOperation::CopyObject, { source, excluded.run, excluded.count, excluded.keys }));
}

// Only an object of the layout's ordered shape keeps its key order in its order word. That word then says that the source has nothing but fields, no integrity level, and a key order that the word alone names. What lies in an Extension is copied by the run time.
void AccessEmitter::generateTryCopyFields(const ValueContext& value)
{
    LayoutID layout = value->layout();
    const PlannedLayout& planned = m_placement.layout(layout);
    const ShapeTable& shapes = m_placement.shapes();
    ShapeRange shapesOfLayout = shapes.shapesOf(layout);
    if (planned.storage != LayoutStorage::CompiledObject || planned.isEscapedOffset || shapesOfLayout.isEmpty() || shapes.shape(shapesOfLayout.first).keysAreNeverEnumerated) {
        defineTest(value, boolean(false));
        return;
    }

    CellPointer target = in<CellPointer>(value->objectOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    const B3::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    auto hasOrderedShape = [&](CellPointer cell, ValueID operand) {
        return equal(shapeTests.shapeWordOf(cell, typeOf(operand).layouts), word32(static_cast<int32_t>(AOT::shapeWordOf(shapesOfLayout.first))));
    };
    auto refuse = [&] {
        return boolean(false);
    };
    defineTest(value, either<Condition>(hasOrderedShape(source, value->sourceOperand()), [&] {
        Word32 orderWord = loadField<Word32>(source, m_heaps.AOTObject_orderWord);
        Condition hasOnlyFields = logicalAnd(hasNoBits(orderWord, ~orderAndCountMask), notEqual(bitAnd(orderWord, word32(orderWordOrderMask)), word32(orderStoredOutOfLine)));
        if (const PlacedSlot& extensionWord = m_placement.slots().extensionWord(layout); extensionWord.hasStorage()) {
            TypedPointer address = m_out.address(fields, source.value(), extensionWord.offset);
            bool isNarrow = traitsOf(extensionWord.representation).isNarrowReference;
            hasOnlyFields = logicalAnd(hasOnlyFields, isNarrow ? isZero(Word32(m_out.load32(address))) : isZero(Integer(m_out.load64(address))));
        }
        return either<Condition>(hasOnlyFields, [&] {
            for (uint32_t offset = offsetOfFirstField; offset < planned.size; offset += sizeof(uint64_t))
                m_out.store64(m_out.load64(m_out.address(fields, source.value(), offset)), m_out.address(fields, target.value(), offset));
            bool targetMayBeUnordered = shapesOfLayout.count > 1;
            storeField(target, m_heaps.AOTObject_orderWord, targetMayBeUnordered ? select(hasOrderedShape(target, value->objectOperand()), orderWord, word32(unorderedOrderWord)) : orderWord);
            emitter<BarrierEmitter>().barrier(value, target);
            return boolean(true);
        }, refuse);
    }, refuse));
}

void AccessEmitter::generateGetSuperBase(const ValueContext& value)
{
    CellPointer home = in<CellPointer>(value->homeOperand());
    HomeObjectKind homeKind = value->homeKind();
    bool isCompact = value.shape() == CodeShape::Compact;
    auto readByOperation = [&](bool isOnRarePath) {
        return callWithoutSite<Boxed>(RuntimeOperation::GetSuperBase, { boxCell(home), word32(static_cast<int32_t>(homeKind)) }, { .isOnRarePath = isOnRarePath });
    };
    if (isCompact || !operandIsHomeObject(homeKind)) {
        define(value, readByOperation(isCompact));
        return;
    }

    LBasicBlock slowPath = newBlock();
    Join<Boxed> join(m_out);
    branchUnless(hasNoBits(loadByte(home, m_heaps.JSCell_typeInfoFlags), OverridesGetPrototype), slowPath);
    Boxed base = loadField<Boxed>(loadStructure(home), m_heaps.Structure_prototype);
    branchUnless(isNotEmpty(base), slowPath);
    join.add(base);
    m_out.appendTo(slowPath);
    join.add(readByOperation(true));
    define(value, join.finish());
}

void AccessEmitter::generateSuperGet(const ValueContext& value)
{
    Boxed base = in<Boxed>(value->baseOperand());
    Boxed receiver = in<Boxed>(value->receiverOperand());
    PropertyKeyID key = value->key();
    if (!key) {
        define(value, callOperation<Boxed>(value, RuntimeOperation::SuperGet, { base, in<Boxed>(value->computedKeyOperand()), receiver }));
        return;
    }

    std::optional<uint32_t> identifier = findIdentifierNumber(value, key);
    if (!identifier) {
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    auto readByOperation = [&](bool isOnRarePath) {
        return callOperation<Boxed>(value, RuntimeOperation::GetSuperPropertyById, { base, receiver, word32(static_cast<int32_t>(*identifier)) }, { .isOnRarePath = isOnRarePath });
    };
    if (value.shape() == CodeShape::Compact || !RowDisplacementTable::hasColumn(*identifier)) {
        define(value, readByOperation(false));
        return;
    }

    Boxed found(callStub(value, tryGetSuperById(), { base.value() }, { .immediate = *identifier }).first);
    define(value, either<Boxed>(isNotEmpty(found), [&] {
        return found;
    }, [&] {
        return readByOperation(true);
    }));
}

void AccessEmitter::generateSuperSet(const ValueContext& value)
{
    Boxed base = in<Boxed>(value->baseOperand());
    Boxed key = keyOperand(value, value->key(), value->computedKeyOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    Boxed receiver = in<Boxed>(value->receiverOperand());
    callVoidOperation(value, RuntimeOperation::SuperSet, { base, key, stored, receiver, word32(static_cast<int32_t>(value->mode())) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
