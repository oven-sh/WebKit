/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOwnPropertyBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCompiledStructure.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTGuardEmitter.h"
#include "AOTObjectWords.h"
#include "AOTOperatorEmitter.h"
#include "AOTPlan.h"
#include "AOTShapeTest.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCellButterfly.h"
#include "StructureRareData.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class KeyListKey : uint8_t { Shape, Structure, StructureForEnumerableKeys };

class KeyListReader final : public BuiltinFamilyEmitter {
public:
    explicit KeyListReader(CodeGenerator& generator)
        : BuiltinFamilyEmitter(generator)
    {
    }

    Pointer keyListOrElse(CellPointer object, BlockEdge otherwise, KeyListKey key, KeyListKind kind = KeyListKind::Strings)
    {
        static_assert(sizeof(KeyListCacheEntry) == 16);
        static_assert(hasOneBitSet(numberOfKeyLists));
        static_assert(indexOfKeyList(3, numberOfKeyLists + 4) == 7);
        static_assert(keyListWordOfStructure(UINT32_MAX) == UINT32_MAX >> 1 && structureEnumerableKeysWord(16) == (16 >> 1 | 1));
        if (key == KeyListKey::Shape && kind != KeyListKind::Strings && !hasOneInstance())
            leaveUnless(emitter<GuardEmitter>().belongsToInstance(object), otherwise);
        ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
        const Machine::AbstractHeap& heap = key == KeyListKey::Shape ? m_heaps.entire(AbstractHeap::RuntimeState) : m_heaps.collectorState;
        Word32 shapeWord = key == KeyListKey::Shape ? shapeTests.shapeWordOf(object) : shiftRightUnsigned(loadStructureID(object), 1);
        if (key == KeyListKey::StructureForEnumerableKeys)
            shapeWord = bitOr(shapeWord, word32(1));
        if (kind != KeyListKind::Strings)
            shapeWord = add(shapeWord, word32(keyListWordOf(0, kind)));
        Word32 objectOrderWord = shapeTests.orderWordOf(object);
        Integer index = zeroExtended(bitAnd(add(shapeWord, objectOrderWord), numberOfKeyLists - 1));
        LValue entry = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.keyLists)), index, 4).value();
        leaveUnless(equal(Word32(m_out.load32(m_out.address(heap, entry, OBJECT_OFFSETOF(KeyListCacheEntry, shapeWord)))), shapeWord), otherwise);
        leaveUnless(equal(Word32(m_out.load32(m_out.address(heap, entry, OBJECT_OFFSETOF(KeyListCacheEntry, orderWord)))), objectOrderWord), otherwise);
        Pointer list(m_out.loadPtr(m_out.address(heap, entry, OBJECT_OFFSETOF(KeyListCacheEntry, list))));
        leaveUnless(Condition(m_out.notNull(list.value())), otherwise);
        return list;
    }

    Pointer structureKeyListOrElse(CellPointer object, OwnKeysKind kind, BlockEdge otherwise)
    {
        CellPointer previousOrRareData = loadField<CellPointer>(loadStructure(object), m_heaps.Structure_previousOrRareData);
        leaveUnless(Condition(m_out.notNull(previousOrRareData.value())), otherwise);
        leaveUnless(logicalNot(hasCellType(previousOrRareData, StructureType)), otherwise);
        Pointer list = loadField<Pointer>(previousOrRareData, structureKeyListHeap(kind));
        leaveUnless(above(list, pointerConstant(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel()))), otherwise);
        return list;
    }

    Condition isCompiledObject(CellPointer cell)
    {
        return hasBits(emitter<ShapeTestEmitter>().shapeWordOf(cell), compiledObjectShapeBit);
    }

private:
    const Machine::AbstractHeap& structureKeyListHeap(OwnKeysKind kind) const
    {
        switch (kind) {
        case OwnKeysKind::Keys:
            return m_heaps.StructureRareData_cachedEnumerableStrings;
        case OwnKeysKind::Names:
            return m_heaps.StructureRareData_cachedStrings;
        case OwnKeysKind::Symbols:
            return m_heaps.StructureRareData_cachedSymbols;
        case OwnKeysKind::All:
            return m_heaps.StructureRareData_cachedStringsAndSymbols;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void leaveUnless(Condition mustHold, BlockEdge otherwise)
    {
        LBasicBlock continuation = newBlock();
        m_out.branch(mustHold.value(), normalEdge(continuation), otherwise);
        m_out.appendToContinuation(continuation);
    }
};

} // anonymous namespace

OwnPropertyBuiltinEmitter::OwnPropertyBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Boxed OwnPropertyBuiltinEmitter::firstArgumentOrUndefined(const InlineCall& call)
{
    ValueID argument = call.argumentOrNull(0);
    return argument ? boxedOperand(call.context(), argument) : undefined();
}

void OwnPropertyBuiltinEmitter::generateObjectValues(const InlineCall& call)
{
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::ObjectValues, { firstArgumentOrUndefined(call) }));
}

void OwnPropertyBuiltinEmitter::generateObjectEntries(const InlineCall& call)
{
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::ObjectEntries, { firstArgumentOrUndefined(call) }));
}

void OwnPropertyBuiltinEmitter::generateOwnPropertyKeys(const InlineCall& call)
{
    OwnKeysKind kind = call.parameter<OwnKeysKind>(0);
    ValueID argument = call.argumentOrNull(0);
    bool isSeldomRun = m_code.at(call.context().block()).frequency != BlockFrequency::Normal;
    if (!call.canMiss() && (!argument || !typeOf(argument).hasObjects() || (isSeldomRun && !typeOf(argument).isObject()))) {
        finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::OwnPropertyKeys, { firstArgumentOrUndefined(call), word32(static_cast<int32_t>(kind)) }));
        return;
    }
    const Type& type = typeOf(argument);
    KeyListReader reader(generator());
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    Join<CellPointer> results(m_out);
    bool testsForObject = !call.canMiss() && !type.isObject();
    LBasicBlock isNoObject = testsForObject ? newBlock() : nullptr;
    CellPointer object = testsForObject ? objectOrElse(argument, isNoObject) : objectOrMiss(call, argument);
    auto finishWithResults = [&] {
        if (testsForObject) {
            m_out.appendTo(isNoObject);
            results.add(callOperation<CellPointer>(call.context(), RuntimeOperation::OwnPropertyKeys, { firstArgumentOrUndefined(call), word32(static_cast<int32_t>(kind)) }));
        }
        finish(call, results.finish());
    };
    static_assert(!static_cast<unsigned>(OwnKeysKind::Keys));
    if (!call.canMiss() && kind == OwnKeysKind::Keys && !generator().isInLoop(call.context().block())) {
        results.add(CellPointer(callStub(call.context(), listOwnKeys(), { boxCell(object).value() }, { .calledBuiltin = functionOf(call.overload()) }).first));
        finishWithResults();
        return;
    }
    LBasicBlock outOfLine = newBlock();
    Join<Pointer> lists(m_out);
    KeyListKind listKind = kind == OwnKeysKind::Symbols ? KeyListKind::Symbols : kind == OwnKeysKind::All ? KeyListKind::StringsAndSymbols : KeyListKind::Strings;

    auto listCompiledObjectKeys = [&] {
        if (!hasKeyListsInTable(listKind, m_plan.shapes().numberOfShapes())) {
            m_out.jump(outOfLine);
            m_out.appendTo(newBlock());
            lists.add(pointerConstant(0));
            return;
        }
        if (kind == OwnKeysKind::Symbols) {
            lists.add(reader.keyListOrElse(object, rareEdge(outOfLine), KeyListKey::Shape, listKind));
            return;
        }
        LBasicBlock isNotCachedByShape = newBlock();
        lists.add(reader.keyListOrElse(object, unlikelyEdge(isNotCachedByShape), KeyListKey::Shape, listKind));
        m_out.appendTo(isNotCachedByShape);
        branchUnless(hasBits(shapeTests.orderWordOf(object), orderWordHasExtraProperties), outOfLine);
        lists.add(reader.keyListOrElse(object, rareEdge(outOfLine), kind == OwnKeysKind::Keys ? KeyListKey::StructureForEnumerableKeys : KeyListKey::Structure));
    };
    bool mayBeCompiledObject = type.anyObject || type.layouts;
    if (shapeTests.hasOnlyCompiledObjects(type))
        listCompiledObjectKeys();
    else {
        BlockEdge isNotCachedOnStructure = mayBeCompiledObject ? unlikelyEdge(newBlock()) : rareEdge(outOfLine);
        lists.add(reader.structureKeyListOrElse(object, kind, isNotCachedOnStructure));
        if (mayBeCompiledObject) {
            m_out.appendTo(isNotCachedOnStructure.block());
            branchUnless(reader.isCompiledObject(object), outOfLine);
            listCompiledObjectKeys();
        }
    }

    Pointer list = lists.finish();
    Integer header(m_out.load64(instanceField(m_heaps.entire(AbstractHeap::AllocatorState), Instance::offsetOfNewCopyOnWriteArrayHeader(CopyOnWriteArrayWithContiguous))));
    branchUnless(isNotZero(header), outOfLine);
    results.add(ArrayAllocationEmitter(generator()).allocateArrayCell(header, offsetBy(list, JSCellButterfly::offsetOfData()), outOfLine));

    m_out.appendTo(outOfLine);
    results.add(callOperation<CellPointer>(call.context(), RuntimeOperation::OwnPropertyKeys, { boxCell(object), word32(static_cast<int32_t>(kind)) }, { .calledBuiltin = functionOf(call.overload()), .isOnRarePath = true }));
    finishWithResults();
}

void OwnPropertyBuiltinEmitter::generateObjectHasOwn(const InlineCall& call)
{
    bool takesReceiver = m_builtins.at(call.overload()).receiver;
    ValueID keyOperand = call.argument(takesReceiver ? 0 : 1);
    RELEASE_ASSERT(!takesReceiver || !typeOf(keyOperand).hasObjects());
    Boxed object = boxedOperand(call.context(), takesReceiver ? call.receiver() : call.argument(0));
    Boxed key = boxedOperand(call.context(), keyOperand);
    StubCallResult result = callStub(call.context(), inByValCached(PresenceQuestion::HasOwn), { object.value(), key.value() }, { .calledBuiltin = functionOf(call.overload()) });
    finish(call, Condition(m_out.castToInt32(result.first)));
}

void OwnPropertyBuiltinEmitter::generateObjectIsPrototypeOf(const InlineCall& call)
{
    Boxed receiver = boxedOperand(call.context(), call.receiver());
    Boxed value = boxedOperand(call.context(), call.argument(0));
    auto askHost = [&] {
        return callOperation<Condition>(call.context(), RuntimeOperation::IsPrototypeOf, { receiver, value }, { .calledBuiltin = functionOf(call.overload()) });
    };
    if (!generator().isInLoop(call.context().block())) {
        finish(call, askHost());
        return;
    }
    finish(call, asBoolean([&](BranchTargets targets) {
        LBasicBlock slowPath = newBlock();
        CellPointer prototype = objectOrElse(call.receiver(), slowPath);
        emitter<OperatorEmitter>().branchOnPrototypeAbove(call.argument(0), prototype, slowPath, targets);
        m_out.appendTo(slowPath);
        branch(askHost(), targets);
    }));
}

void OwnPropertyBuiltinEmitter::generateObjectPropertyIsEnumerable(const InlineCall& call)
{
    Boxed receiver = boxedOperand(call.context(), call.receiver());
    Boxed key = boxedOperand(call.context(), call.argument(0));
    finish(call, callOperation<Condition>(call.context(), RuntimeOperation::PropertyIsEnumerable, { receiver, key }, { .calledBuiltin = functionOf(call.overload()) }));
}

void OwnPropertyBuiltinEmitter::generateReflectSet(const InlineCall& call)
{
    Boxed target = boxedOperand(call.context(), call.argument(0));
    Boxed key = boxedOperand(call.context(), call.argument(1));
    Boxed value = boxedOperand(call.context(), call.argument(2));
    ValueID receiver = call.argumentOrNull(3);
    StubCallResult result = callStub(call.context(), reflectSet(), { target.value(), key.value(), value.value(), receiver ? boxedOperand(call.context(), receiver).value() : target.value() }, { .calledBuiltin = functionOf(call.overload()) });
    finish(call, Condition(m_out.notZero64(result.first)));
}

void OwnPropertyBuiltinEmitter::generateObjectGetOwnPropertyDescriptor(const InlineCall& call)
{
    CellPointer object = objectOrMiss(call, call.argument(0));
    Boxed key = boxedOperand(call.context(), call.argument(1));
    finish(call, callOperation<Boxed>(call.context(), RuntimeOperation::GetOwnPropertyDescriptor, { boxCell(object), key }, { .calledBuiltin = functionOf(call.overload()) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
