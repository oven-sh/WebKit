/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOwnPropertyBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCompiledStructure.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCellButterfly.h"
#include "StructureRareData.h"
#include <array>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr unsigned numberOfDescriptorFields = 7;

enum class KeyListKey : bool { Shape, Structure };

class KeyListReader final : public BuiltinFamilyEmitter {
public:
    explicit KeyListReader(CodeGenerator& generator)
        : BuiltinFamilyEmitter(generator)
    {
    }

    Pointer keyListOrElse(CellPointer object, B3::FrequentedBlock otherwise, KeyListKey key)
    {
        static_assert(sizeof(KeyListCacheEntry) == 16);
        static_assert(hasOneBitSet(numberOfKeyLists));
        static_assert(indexOfKeyList(3, numberOfKeyLists + 4) == 7);
        ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
        const B3::AbstractHeap& heap = key == KeyListKey::Shape ? m_heaps.entire(AbstractHeap::RuntimeState) : m_heaps.collectorState;
        Word32 shapeWord = key == KeyListKey::Shape ? shapeTests.shapeWordOf(object, LayoutSetID()) : shiftRightUnsigned(loadStructureID(object), word32(1));
        Word32 objectOrderWord = shapeTests.orderWordOf(object);
        Integer index = zeroExtended(bitAnd(add(shapeWord, objectOrderWord), word32(numberOfKeyLists - 1)));
        LValue entry = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.keyLists)), index, 4).value();
        leaveUnless(equal(Word32(m_out.load32(m_out.address(heap, entry, OBJECT_OFFSETOF(KeyListCacheEntry, shapeWord)))), shapeWord), otherwise);
        leaveUnless(equal(Word32(m_out.load32(m_out.address(heap, entry, OBJECT_OFFSETOF(KeyListCacheEntry, orderWord)))), objectOrderWord), otherwise);
        Pointer list(m_out.loadPtr(m_out.address(heap, entry, OBJECT_OFFSETOF(KeyListCacheEntry, list))));
        leaveUnless(Condition(m_out.notNull(list.value())), otherwise);
        return list;
    }

    Pointer structureKeyListOrElse(CellPointer object, OwnKeysKind kind, B3::FrequentedBlock otherwise)
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
        return hasBits(emitter<ShapeTestEmitter>().shapeWordOf(cell, LayoutSetID()), compiledObjectShapeBit);
    }

private:
    const B3::AbstractHeap& structureKeyListHeap(OwnKeysKind kind) const
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

    void leaveUnless(Condition mustHold, B3::FrequentedBlock otherwise)
    {
        LBasicBlock continuation = newBlock();
        m_out.branch(mustHold.value(), normalEdge(continuation), otherwise);
        m_out.appendTo(continuation);
    }
};

} // anonymous namespace

OwnPropertyBuiltinEmitter::OwnPropertyBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void OwnPropertyBuiltinEmitter::generateOwnPropertyKeys(const InlineCall& call)
{
    static_assert(sizeof(OptionSet<CompiledStructure::Flag>) == 1);
    OwnKeysKind kind = call.parameter<OwnKeysKind>(0);
    ValueID argument = call.argument(0);
    const Type& type = typeOf(argument);
    KeyListReader reader(generator());
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    LBasicBlock outOfLine = newBlock();
    Join<Pointer> lists(m_out);
    Join<CellPointer> results(m_out);
    CellPointer object = objectOrMiss(call, argument);
    bool listsSymbols = kind == OwnKeysKind::Symbols;

    auto listCompiledObjectKeys = [&] {
        if (listsSymbols || kind == OwnKeysKind::All) {
            Word32 flags = loadByte(loadStructure(object), m_heaps.AOTCompiledStructure_flags);
            branchUnless(hasNoBits(flags, static_cast<uint32_t>(CompiledStructure::Flag::HasFieldsWithSymbolKeys)), outOfLine);
        }
        if (listsSymbols) {
            branchUnless(hasNoBits(shapeTests.orderWordOf(object), orderWordHasExtraProperties), outOfLine);
            results.add(emitter<CellAllocationEmitter>().newArrayOf(call.context(), std::span<const Boxed> { }));
            return;
        }
        if (kind != OwnKeysKind::Keys) {
            lists.add(reader.keyListOrElse(object, rareEdge(outOfLine), KeyListKey::Shape));
            return;
        }
        LBasicBlock isNotCachedByShape = newBlock();
        lists.add(reader.keyListOrElse(object, unlikelyEdge(isNotCachedByShape), KeyListKey::Shape));
        m_out.appendTo(isNotCachedByShape);
        branchUnless(hasBits(shapeTests.orderWordOf(object), orderWordHasExtraProperties), outOfLine);
        lists.add(reader.keyListOrElse(object, rareEdge(outOfLine), KeyListKey::Structure));
    };
    bool mayBeCompiledObject = type.anyObject || type.layouts;
    if (!listsSymbols && type.hasOnlyLayouts())
        listCompiledObjectKeys();
    else {
        B3::FrequentedBlock isNotCachedOnStructure = mayBeCompiledObject ? unlikelyEdge(newBlock()) : rareEdge(outOfLine);
        lists.add(reader.structureKeyListOrElse(object, kind, isNotCachedOnStructure));
        if (mayBeCompiledObject) {
            m_out.appendTo(isNotCachedOnStructure.block());
            branchUnless(reader.isCompiledObject(object), outOfLine);
            listCompiledObjectKeys();
        }
    }

    Pointer list = lists.finish();
    Integer header = loadMutable64(Instance::offsetOfNewCopyOnWriteArrayHeader(CopyOnWriteArrayWithContiguous));
    branchUnless(isNotZero(header), outOfLine);
    results.add(ArrayAllocationEmitter(generator()).allocateArrayCell(header, offsetBy(list, JSCellButterfly::offsetOfData()), outOfLine));

    m_out.appendTo(outOfLine);
    results.add(callOperation<CellPointer>(call.context(), RuntimeOperation::OwnPropertyKeys, { boxCell(object), word32(static_cast<int32_t>(kind)) }, { .isOnRarePath = true }));

    finish(call, results.finish());
}

void OwnPropertyBuiltinEmitter::generateObjectHasOwn(const InlineCall& call)
{
    bool takesReceiver = m_builtins.at(call.overload()).receiver;
    Boxed object = boxedOperand(call.context(), takesReceiver ? call.receiver() : call.argument(0));
    Boxed key = boxedOperand(call.context(), call.argument(takesReceiver ? 0 : 1));
    StubCallResult result = callStub(call.context(), inByValCached(PresenceQuestion::HasOwn), { object.value(), key.value() });
    finish(call, Condition(m_out.castToInt32(result.first)));
}

void OwnPropertyBuiltinEmitter::generateObjectPropertyIsEnumerable(const InlineCall& call)
{
    Boxed receiver = boxedOperand(call.context(), call.receiver());
    Boxed key = boxedOperand(call.context(), call.argument(0));
    finish(call, callOperation<Condition>(call.context(), RuntimeOperation::PropertyIsEnumerable, { receiver, key }));
}

void OwnPropertyBuiltinEmitter::generateObjectGetOwnPropertyDescriptor(const InlineCall& call)
{
    CellPointer object = objectOrMiss(call, call.argument(0));
    Boxed key = boxedOperand(call.context(), call.argument(1));
    finish(call, callOperation<Boxed>(call.context(), RuntimeOperation::GetOwnPropertyDescriptor, { boxCell(object), key }));
}

void OwnPropertyBuiltinEmitter::generateOwnPropertyDescriptorFields(const InlineCall& call)
{
    ValueID base = call.argument(0);
    std::array<Boxed, 2> arguments { boxedOperand(call.context(), base), boxedOperand(call.context(), call.argument(1)) };
    if (m_builtins.at(call.overload()).builtin == Builtins::ReflectGetOwnPropertyDescriptor && !typeOf(base).isObject()) {
        LBasicBlock isNotObject = newBlock();
        LBasicBlock isObject = newBlock();
        objectOrElse(base, isNotObject);
        m_out.jump(isObject);

        m_out.appendTo(isNotObject);
        emitter<BuiltinCallEmitter>().callBuiltinFunction(call.context(), call.overload(), std::nullopt, arguments);
        m_out.unreachable();

        m_out.appendTo(isObject);
    }

    Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(numberOfDescriptorFields);
    callVoidOperation(call.context(), RuntimeOperation::GetOwnPropertyFields, { arguments[0], arguments[1], buffer });
    auto fieldAt = [&](unsigned index) {
        return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), index * sizeof(EncodedJSValue))));
    };
    std::array<Operand, numberOfDescriptorFields> results;
    results[0] = unboxBoolean(fieldAt(0));
    for (unsigned index = 1; index < numberOfDescriptorFields; ++index)
        results[index] = fieldAt(index);
    finishWithResults(call, results);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
