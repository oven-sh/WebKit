/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTConstantEmitter.h"
#include "AOTDerivedFunctions.h"
#include "AOTFunctionKinds.h"
#include "AOTObjectWords.h"
#include "AOTPlacement.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTPropertyCachesFront.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "JSTypeInfo.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isDefinedByClass(const Program& program, LayoutID classLayout)
{
    FunctionID constructor = program.at(classLayout).constructor;
    if (!constructor)
        return false;
    FunctionKind kind = program.at(constructor).kind;
    return kind == FunctionKind::BaseConstructor || kind == FunctionKind::DerivedConstructor;
}

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

void AccessEmitter::generateTryDefineLiveBinding(const ValueContext& value)
{
    Boxed target = in<Boxed>(value->targetOperand());
    Boxed source = in<Boxed>(value->sourceOperand());
    Boxed sourceKey = in<Boxed>(value->sourceKeyOperand());
    Boxed targetKey = in<Boxed>(value->targetKeyOperand());
    CellPointer getter = in<CellPointer>(value->getterOperand());
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::TryDefineLiveBinding, { target, source, sourceKey, targetKey, getter }));
}

void AccessEmitter::generateTryReExportFields(const ValueContext& value)
{
    const Function& getter = m_program.at(value->function());
    FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, value->function()));
    Boxed target = in<Boxed>(value->targetOperand());
    Boxed source = in<Boxed>(value->sourceOperand());
    // A pass that took a capture away from the getter knows nothing of closures made here: then the written call does the work.
    if (!entrypoint || getter.captures.size() != 2 || closureCaptureCountsOf(m_program, getter).references != 2) {
        defineTest(value, boolean(false));
        return;
    }
    uint32_t rule = closureCaptureSlotOf(m_program, getter, value->keyIsFirstCapture() ? 0 : 1) | static_cast<uint32_t>(value->reusesDescriptors()) << 1;
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::TryReExportFields, { target, source, word32(static_cast<int32_t>(entrypoint.index())), word32(static_cast<int32_t>(rule)) }));
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

void AccessEmitter::storeStructureAfterPrototypeTransition(CellPointer object, Word32 structureID)
{
    storeField(object, m_heaps.JSCell_structureID, structureID);
    Word32 flags = loadByte(object, m_heaps.JSCell_typeInfoFlags);
    m_out.store32As8(bitOr(flags, word32(JSC::IsPristine)).value(), object.value(), m_heaps.JSCell_typeInfoFlags);
}

void AccessEmitter::generateSetLiteralPrototype(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    Boxed prototype = in<Boxed>(value->prototypeOperand());
    LBasicBlock isUnknown = newBlock();
    LBasicBlock continuation = newBlock();
    // What Object.create makes is born below the object that the first call was handed.
    LBasicBlock isOther = newBlock();
    branch(equal(prototype, loadField<Boxed>(loadStructure(object), m_heaps.Structure_prototype)), { normalEdge(continuation), normalEdge(isOther) });
    m_out.appendTo(isOther);
    storeStructureAfterPrototypeTransition(object, findPrototypeTransition(object, prototype, isUnknown));
    emitter<BarrierEmitter>().barrier(value, object);
    m_out.jump(continuation);

    m_out.appendTo(isUnknown);
    callVoidOperation(value, RuntimeOperation::SetLiteralPrototype, { object, prototype, word32(value->refusesOtherValues()) }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// The keys that the source text names lie in the image as a run, which the operation gets by its number plus one.
AccessEmitter::ExcludedKeys AccessEmitter::excludedKeysOf(const ValueContext& value)
{
    std::optional<Request> run = value->request();
    Vector<Boxed, 8> keys;
    if (!run) {
        // A key that the operation takes is a string or a symbol, so a number among the keys is the number of an identifier.
        for (PropertyKeyID key : value->excludedKeys())
            keys.append(boxedConstant(jsNumber(findIdentifierNumber(value, key).value_or(0))));
    }
    for (ValueID key : value->excludedOperands())
        keys.append(in<Boxed>(key));
    return { word32(run ? static_cast<int32_t>(run->value() + 1) : 0), word32(static_cast<int32_t>(keys.size())), argumentBuffer(keys.span()) };
}

void AccessEmitter::generateCopyDataProperties(const ValueContext& value)
{
    if (std::optional<CopyOfTwo> copyOfTwo = std::exchange(m_copyOfTwo, std::nullopt)) {
        RELEASE_ASSERT(copyOfTwo->second == value.valueID());
        ifElse(Condition(m_out.notNull(copyOfTwo->copy.value())), [] { }, [&] {
            copyDataProperties(value);
        });
        return;
    }
    copyDataProperties(value);
}

void AccessEmitter::copyDataProperties(const ValueContext& value)
{
    CellPointer target = in<CellPointer>(value->targetOperand());
    Boxed source = in<Boxed>(value->sourceOperand());
    auto copyByOperation = [&] {
        ExcludedKeys excluded = excludedKeysOf(value);
        callVoidOperation(value, RuntimeOperation::CopyDataProperties, { target, source, excluded.run, excluded.count, excluded.keys });
    };
    bool sourceIsAlwaysCell = !(typeOf(value->sourceOperand()).primitives & ~cellPrimitives);
    if (sourceIsAlwaysCell) {
        copyByOperation();
        return;
    }

    // In code of every shape: the test is shorter than what it saves is long, `...(condition && { .. })` in code that runs once a turn.
    LBasicBlock mayHaveOwnProperties = newBlock();
    LBasicBlock continuation = newBlock();
    branch(isCell(source), { normalEdge(mayHaveOwnProperties), normalEdge(continuation) });

    m_out.appendTo(mayHaveOwnProperties);
    copyByOperation();
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void AccessEmitter::generateCopyObject(const ValueContext& value)
{
    Boxed source = in<Boxed>(value->sourceOperand());
    auto copyByOperation = [&]() -> CellPointer {
        ExcludedKeys excluded = excludedKeysOf(value);
        return callOperation<CellPointer>(value, RuntimeOperation::CopyObject, { source, excluded.run, excluded.count, excluded.keys });
    };
    ValueID second = findCopyIntoCopy(m_function, m_function.at(value.block()), value.valueID());
    // The copy of a compiled object is an object of its layout, which only the copy by itself makes.
    const Type& typeOfSource = typeOf(value->sourceOperand());
    bool mayBeOtherThanCompiledObject = typeOfSource.anyObject || typeOfSource.primitives & ~cellPrimitives;
    if (!second || !mayBeOtherThanCompiledObject) {
        define(value, copyByOperation());
        return;
    }

    LBasicBlock isCellBlock = newBlock();
    LBasicBlock isCompiledObject = newBlock();
    LBasicBlock mayCopyBoth = newBlock();
    Join<CellPointer> join(m_out);
    branch(isCell(source), { normalEdge(isCellBlock), normalEdge(mayCopyBoth) });

    m_out.appendTo(isCellBlock);
    branch(hasMutableCellType(unboxCell(source), CompiledObjectType), { normalEdge(isCompiledObject), normalEdge(mayCopyBoth) });

    m_out.appendTo(isCompiledObject);
    join.add(CellPointer(m_out.constIntPtr(0)));

    m_out.appendTo(mayCopyBoth);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::TryCopyTwoObjects, { source, in<Boxed>(m_function.at(second).sourceOperand()) }));

    CellPointer copyOfTwo = join.finish();
    m_copyOfTwo = CopyOfTwo { second, copyOfTwo };
    define(value, either<CellPointer>(Condition(m_out.notNull(copyOfTwo.value())), [&] {
        return copyOfTwo;
    }, copyByOperation));
}

// Only an object of the layout's ordered shape keeps its key order in its order word. That word then says that the source has nothing but fields, no integrity level, and a key order that the word alone names. What lies in an Extension is copied by the run time.
std::optional<CellPointer> AccessEmitter::tryAllocateCopyOfFields(const ValueContext& allocation, ValueID copy, const ScopedLambda<CellPointer()>& allocateEmpty)
{
    LayoutID layout = m_function.at(copy).layout();
    const PlannedLayout& planned = m_placement.layout(layout);
    const ShapeTable& shapes = m_placement.shapes();
    ShapeRange shapesOfLayout = shapes.shapesOf(layout);
    if (planned.storage != LayoutStorage::CompiledObject || planned.isEscapedOffset || shapesOfLayout.count != 1 || shapes.shape(shapesOfLayout.first).keysAreNeverEnumerated)
        return std::nullopt;
    if (m_placement.allocation(allocation->allocation()).layout != layout || m_placement.freeCellOffset(layout) || m_placement.slots().extensionWord(layout).hasStorage())
        return std::nullopt;
    if (planned.size <= offsetOfFirstField)
        return std::nullopt;
    for (uint32_t field = 0; field < m_program.at(layout).fields.size(); ++field) {
        const PlacedSlot& slot = m_placement.slot(layout, field);
        if (slot.hasStorage() && (slot.isOutOfLine() || slot.representation == SlotRepresentation::DeferredClosure))
            return std::nullopt;
    }

    CellPointer source = in<CellPointer>(m_function.at(copy).sourceOperand());
    LBasicBlock hasShape = newBlock();
    LBasicBlock copies = newBlock();
    LBasicBlock fills = newBlock();
    LBasicBlock continuation = newBlock();
    Word32 orderedShape = word32(static_cast<int32_t>(AOT::shapeWordOf(shapesOfLayout.first)));
    branch(equal(emitter<ShapeTestEmitter>().shapeWordOf(source), orderedShape), { normalEdge(hasShape), rareEdge(fills) });

    m_out.appendTo(hasShape);
    Word32 orderWord = loadField<Word32>(source, m_heaps.AOTObject_orderWord);
    Condition hasOnlyFields = logicalAnd(hasNoBits(orderWord, ~orderAndCountMask), notEqual(bitAnd(orderWord, word32(orderWordOrderMask)), word32(orderStoredOutOfLine)));
    branch(hasOnlyFields, { normalEdge(copies), rareEdge(fills) });

    m_out.appendTo(copies);
    unsigned fieldBytes = WTF::roundUpToMultipleOf<copyChunkBytes>(planned.size) - offsetOfFirstField;
    ValueFromBlock copiedCell = m_out.anchor(emitter<CellAllocationEmitter>().allocateObjectCopy(allocation, allocation->allocation(), source, fieldBytes).value());
    ValueFromBlock didCopy = m_out.anchor(m_out.constInt32(1));
    m_out.jump(continuation);

    m_out.appendTo(fills);
    ValueFromBlock emptyCell = m_out.anchor(allocateEmpty().value());
    ValueFromBlock didNotCopy = m_out.anchor(m_out.constInt32(0));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    m_copyAtAllocation = CopyAtAllocation { copy, Condition(m_out.phi(B3::Int32, didCopy, didNotCopy)) };
    return CellPointer(m_out.phi(B3::Int64, copiedCell, emptyCell));
}

void AccessEmitter::generateTryCopyFields(const ValueContext& value)
{
    if (m_copyAtAllocation && m_copyAtAllocation->copy == value.id()) {
        defineTest(value, std::exchange(m_copyAtAllocation, std::nullopt)->didCopy);
        return;
    }

    LayoutID layout = value->layout();
    const PlannedLayout& planned = m_placement.layout(layout);
    const ShapeTable& shapes = m_placement.shapes();
    ShapeRange shapesOfLayout = shapes.shapesOf(layout);
    if (planned.storage != LayoutStorage::CompiledObject || planned.isEscapedOffset || shapesOfLayout.isEmpty() || shapes.shape(shapesOfLayout.first).keysAreNeverEnumerated) {
        defineTest(value, boolean(false));
        return;
    }

    // The words that are no fields are zero in a source that is copied.
    bool mayCopyCell = std::ranges::any_of(planned.slots.span().first(planned.numberOfFields()), [](const PlacedSlot& slot) {
        return slot.hasStorage() && traitsOf(slot.representation).holdsCell;
    });

    CellPointer target = in<CellPointer>(value->objectOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    const B3::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    auto hasOrderedShape = [&](CellPointer cell) {
        return equal(shapeTests.shapeWordOf(cell), word32(static_cast<int32_t>(AOT::shapeWordOf(shapesOfLayout.first))));
    };
    auto refuse = [&] {
        return boolean(false);
    };
    defineTest(value, either<Condition>(hasOrderedShape(source), [&] {
        Word32 orderWord = loadField<Word32>(source, m_heaps.AOTObject_orderWord);
        Condition hasOnlyFields = logicalAnd(hasNoBits(orderWord, ~orderAndCountMask), notEqual(bitAnd(orderWord, word32(orderWordOrderMask)), word32(orderStoredOutOfLine)));
        if (const PlacedSlot& extensionWord = m_placement.slots().extensionWord(layout); extensionWord.hasStorage()) {
            TypedPointer address = m_out.address(fields, source.value(), extensionWord.offset);
            bool isNarrow = traitsOf(extensionWord.representation).isNarrowReference;
            hasOnlyFields = logicalAnd(hasOnlyFields, isNarrow ? isZero(Word32(m_out.load32(address))) : isZero(Integer(m_out.load64(address))));
        }
        // The copy holds the function objects of the source. One that the source has not made yet is made first, by the operation that copies after a refusal.
        for (uint32_t field = 0; field < m_program.at(layout).fields.size(); ++field) {
            const PlacedSlot& slot = m_placement.slot(layout, field);
            if (!slot.hasStorage() || slot.isOutOfLine() || slot.representation != SlotRepresentation::DeferredClosure)
                continue;
            Word32 word(m_out.load32(m_out.address(fields, source.value(), slot.offset)));
            hasOnlyFields = logicalAnd(hasOnlyFields, notEqual(word, word32(uncreatedDeferredClosure)));
        }
        return either<Condition>(hasOnlyFields, [&] {
            for (uint32_t offset = offsetOfFirstField; offset < planned.size; offset += sizeof(uint64_t))
                m_out.store64(m_out.load64(m_out.address(fields, source.value(), offset)), m_out.address(fields, target.value(), offset));
            bool targetMayBeUnordered = shapesOfLayout.count > 1;
            storeField(target, m_heaps.AOTObject_orderWord, targetMayBeUnordered ? select(hasOrderedShape(target), orderWord, word32(unorderedOrderWord)) : orderWord);
            if (mayCopyCell)
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
    if (!operandIsHomeObject(homeKind)) {
        // The operand is the class object. Nobody can give a class another prototype object, and that of a class that is evaluated once is in ClassWords::classPrototypes.
        LayoutID classLayout = classOfClassObject(m_program, m_program.typeOf(m_function, value->homeOperand()));
        std::optional<uint32_t> rank = classLayout && isEvaluatedOnce(m_program, m_program.at(classLayout)) ? m_placement.rankOfClass(classLayout) : std::nullopt;
        if (!rank || !isDefinedByClass(m_program, classLayout)) {
            define(value, readByOperation(isCompact));
            return;
        }
        define(value, Boxed(callStub(value, getSuperBaseOfClass(), { boxCell(home).value() }, { .immediate = *rank }).first));
        return;
    }
    if (isCompact) {
        define(value, readByOperation(true));
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
        define(value, Boxed(callStub(value, getSuperByVal(), { base.value(), in<Boxed>(value->computedKeyOperand()).value(), receiver.value() }).first));
        return;
    }

    // A lookup that starts at the receiver is a read by name: so reads a field whose slot was found empty.
    if (value->baseOperand() == value->receiverOperand()) {
        define(value, getById(value, receiver, key, SiteCacheID()));
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
    if (!RowDisplacementTable::hasColumn(*identifier)) {
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
