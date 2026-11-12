/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTObjectWords.h"
#include "AOTPendingStars.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubCalls.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsObjectCopy.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "JSTypeInfo.h"
#include <algorithm>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

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

static bool captureIsHeldAsBoxedWord(Representation held)
{
    switch (held) {
    case Representation::JSValue:
    case Representation::Cell:
        return true;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::RawWord:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed AccessEmitter::keyOperand(const OperationContext& context, PropertyKeyID key, ValueID computedKey)
{
    if (!key)
        return in<Boxed>(computedKey);
    return boxCell(emitter<ConstantEmitter>().keyConstant(context, key));
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
    FunctionID function = value->function();
    unsigned keyCapture = value->keyIsFirstCapture() ? 0 : 1;
    auto isHeldAsBoxedWord = [&](unsigned capture) {
        return captureIsHeldAsBoxedWord(closureCaptureRepresentationOf(m_program, conventions(), function, capture));
    };
    if (!conventions().exists(conventions().closureEntryOf(function)) || m_program.at(function).captures.size() != 2 || !isHeldAsBoxedWord(0) || !isHeldAsBoxedWord(1)) {
        defineTest(value, boolean(false));
        return;
    }
    RELEASE_ASSERT(closureCaptureCountsOf(m_program, conventions(), function).references == 2);
    Boxed target = in<Boxed>(value->targetOperand());
    Boxed source = in<Boxed>(value->sourceOperand());
    Word32 closureNumber = numberOf(m_mentions.add(NumberSpace::ClosureFunction, function));
    uint32_t keySlot = closureCaptureSlotOf(m_program, conventions(), function, keyCapture);
    RELEASE_ASSERT(!(keySlot & ~starRuleKeyCaptureSlot));
    uint32_t rule = keySlot | (value->reusesDescriptors() ? starRuleReusesDescriptors : 0);
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::TryReExportFields, { target, source, closureNumber, word32(static_cast<int32_t>(rule)) }));
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
    const Machine::AbstractHeap& heap = m_heaps.collectorState;
    Word32 before = loadStructureID(object);
    Word32 hash = shiftRightUnsigned(bitXor(before, Word32(m_out.castToInt32(prototype.value()))), prototypeTransitionHashShift);
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
    LBasicBlock isOther = newBlock();
    branch(equal(prototype, loadField<Boxed>(loadStructure(object), m_heaps.Structure_prototype)), { normalEdge(continuation), normalEdge(isOther) });
    m_out.appendTo(isOther);
    storeStructureAfterPrototypeTransition(object, findPrototypeTransition(object, prototype, isUnknown));
    emitter<BarrierEmitter>().barrier(value, object);
    m_out.jump(continuation);

    m_out.appendTo(isUnknown);
    callVoidOperation(value, RuntimeOperation::SetLiteralPrototype, { object, prototype, word32(value->refusesOtherValues()) }, { .calledBuiltin = value->refusesOtherValues() ? Builtins::ObjectCreate : BuiltinID(), .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

AccessEmitter::ExcludedKeys AccessEmitter::excludedKeysOf(const ValueContext& value)
{
    static_assert(sizeof(PropertyKeyID) == sizeof(uint32_t));
    std::span<const PropertyKeyID> namedKeys = value->excludedKeys();
    std::span<const uint32_t> namedKeyWords = spanReinterpretCast<const uint32_t>(namedKeys);
    bool hasRun = canBePropertyRun(m_program, namedKeyWords);
    Word32 run = hasRun ? numberOf(m_mentions.add(NumberSpace::PropertyRun, namedKeyWords, 1)) : word32(0);
    Vector<Boxed, 8> keys;
    if (!hasRun) {
        for (PropertyKeyID key : namedKeys)
            keys.append(boxInt32(identifierNumber(key)));
    }
    for (ValueID key : value->excludedOperands())
        keys.append(in<Boxed>(key));
    return { run, word32(static_cast<int32_t>(keys.size())), keys.isEmpty() ? pointerConstant(0) : argumentBuffer(keys.span()) };
}

AccessEmitter::ExcludedKeys AccessEmitter::noExcludedKeys()
{
    return { word32(0), word32(0), pointerConstant(0) };
}

void AccessEmitter::copyDataProperties(const ValueContext& value, CellPointer target, ValueID sourceID, Origin origin, NOESCAPE const Invocable<ExcludedKeys()> auto& excludedKeys)
{
    Boxed source = in<Boxed>(sourceID);
    auto copyByOperation = [&] {
        ExcludedKeys excluded = excludedKeys();
        callVoidOperation(value, RuntimeOperation::CopyDataProperties, { target, source, excluded.run, excluded.count, excluded.keys }, { .origin = origin });
    };
    bool sourceIsAlwaysCell = !(typeOf(sourceID).primitives & ~cellPrimitives);
    if (sourceIsAlwaysCell) {
        copyByOperation();
        return;
    }

    LBasicBlock mayHaveOwnProperties = newBlock();
    LBasicBlock continuation = newBlock();
    branch(isCell(source), { normalEdge(mayHaveOwnProperties), normalEdge(continuation) });

    m_out.appendTo(mayHaveOwnProperties);
    copyByOperation();
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void AccessEmitter::generateCopyDataProperties(const ValueContext& value)
{
    if (value->excludedKeys().empty() && value->excludedOperands().empty() && !m_generator.isInLoop(value.block())) {
        callStub(value, copyAllDataProperties(), { in<CellPointer>(value->targetOperand()).value(), in<Boxed>(value->sourceOperand()).value() });
        return;
    }
    copyDataProperties(value, in<CellPointer>(value->targetOperand()), value->sourceOperand(), value.origin(), [&] {
        return excludedKeysOf(value);
    });
}

CellPointer AccessEmitter::copyObject(const ValueContext& value, Boxed source, ExcludedKeys excluded)
{
    return CellPointer(callStub(value, copyObjectFrontEnd(), { source.value(), excluded.run.value(), excluded.count.value(), excluded.keys.value() }).first);
}

void AccessEmitter::generateCopyObject(const ValueContext& value)
{
    Boxed source = in<Boxed>(value->sourceOperand());
    ExcludedKeys excluded = excludedKeysOf(value);
    if (value->assigns()) {
        define(value, CellPointer(callStub(value, assignToNewObjectFrontEnd(), { source.value(), excluded.run.value(), excluded.count.value(), excluded.keys.value() }, { .calledBuiltin = Builtins::ObjectAssign }).first));
        return;
    }
    define(value, copyObject(value, source, excluded));
}

void AccessEmitter::generateCopyObjectAndDefine(const ValueContext& value)
{
    std::span<const PropertyKeyID> keys = value->definedKeys();
    std::span<const ValueID> values = value->valuesOperands();
    RELEASE_ASSERT(!keys.empty() && keys.size() == values.size());
    Boxed source = in<Boxed>(value->sourceOperand());
    auto copyByOperation = [&] {
        Word32 count = word32(static_cast<int32_t>(keys.size()));
        Vector<Boxed, 12> list { boxInt32(count) };
        for (PropertyKeyID key : keys)
            list.append(boxInt32(identifierNumber(key)));
        for (ValueID defined : values)
            list.append(in<Boxed>(defined));
        return callOperation<CellPointer>(value, RuntimeOperation::CopyObjectAndDefine, { source, identifierNumber(keys[0]), count, argumentBuffer(list.span()) });
    };
    Type sourceType = typeOf(value->sourceOperand());
    if (!sourceType.hasObjects()) {
        define(value, copyByOperation());
        return;
    }

    LBasicBlock sourceIsCompiled = newBlock();
    LBasicBlock sourceIsOtherValue = newBlock();
    Join<CellPointer> join(m_out);
    if (sourceType.primitives & ~cellPrimitives) {
        LBasicBlock sourceIsCell = newBlock();
        branch(isCell(source), { normalEdge(sourceIsCell), normalEdge(sourceIsOtherValue) });
        m_out.appendTo(sourceIsCell);
    }
    branch(emitter<ShapeTestEmitter>().isCompiledObject(unboxCell(source)), { normalEdge(sourceIsCompiled), normalEdge(sourceIsOtherValue) });

    m_out.appendTo(sourceIsCompiled);
    CellPointer copy = copyObject(value, source, noExcludedKeys());
    for (size_t index = 0; index < keys.size(); ++index)
        putById(value, boxCell(copy), keys[index], in<Boxed>(values[index]), PutMode::Define);
    join.add(copy);

    m_out.appendTo(sourceIsOtherValue);
    join.add(copyByOperation());
    define(value, join.finish());
}

void AccessEmitter::generateCopyTwoObjects(const ValueContext& value)
{
    Boxed first = in<Boxed>(value->sourceOperand());
    Boxed second = in<Boxed>(value->secondSourceOperand());

    LBasicBlock firstIsCompiledObject = newBlock();
    LBasicBlock mayHavePlan = newBlock();
    Join<CellPointer> join(m_out);
    if (typeOf(value->sourceOperand()).primitives & ~cellPrimitives) {
        LBasicBlock firstIsCell = newBlock();
        branch(isCell(first), { normalEdge(firstIsCell), normalEdge(mayHavePlan) });
        m_out.appendTo(firstIsCell);
    }
    branch(emitter<ShapeTestEmitter>().isCompiledObject(unboxCell(first)), { normalEdge(firstIsCompiledObject), normalEdge(mayHavePlan) });

    m_out.appendTo(firstIsCompiledObject);
    join.add(CellPointer(m_out.constInt64(0)));

    m_out.appendTo(mayHavePlan);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::TryCopyTwoObjects, { first, second }));

    CellPointer copyOfBoth = join.finish();
    define(value, either<CellPointer>(Condition(m_out.notZero64(copyOfBoth.value())), [&] {
        return copyOfBoth;
    }, [&] {
        CellPointer copy = copyObject(value, first, noExcludedKeys());
        copyDataProperties(value, copy, value->secondSourceOperand(), value->secondOrigin(), [&] {
            return noExcludedKeys();
        });
        return copy;
    }));
}

std::optional<CellPointer> AccessEmitter::tryAllocateCopyOfFields(const ValueContext& allocation, const MentionedAllocation& mentioned, ValueID copy, const ScopedLambda<CellPointer()>& allocateEmpty)
{
    LayoutID layout = m_code.at(copy).layout();
    const PlannedLayout& planned = m_plan.layout(layout);
    const ShapeTable& shapes = m_plan.shapes();
    ShapeRange shapesOfLayout = shapes.shapesOf(layout);
    if (!planned.holdsCompiledObjects() || planned.cellsAreHandedBack || shapesOfLayout.count != 1 || m_code.at(copy).sourceLayout())
        return std::nullopt;
    if (mentioned.placed.layout != layout || m_plan.slots().extensionWord(layout).hasStorage() || planned.size <= offsetOfFirstField)
        return std::nullopt;
    for (const PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
        if (slot.hasStorage() && (slot.isOutOfLine() || slot.representation == SlotRepresentation::DeferredClosure))
            return std::nullopt;
    }

    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    CellPointer source = in<CellPointer>(m_code.at(copy).sourceOperand());
    LBasicBlock hasShape = newBlock();
    LBasicBlock copies = newBlock();
    LBasicBlock fills = newBlock();
    LBasicBlock continuation = newBlock();
    {
        Machine::DeclaredAccessesSuspension readsForCopy(m_out);
        branch(equal(shapeTests.shapeWordWithMarkOf(source), static_cast<int32_t>(AOT::shapeWordOf(shapesOfLayout.first))), { normalEdge(hasShape), rareEdge(fills) });

        m_out.appendTo(hasShape);
        Word32 orderWord = shapeTests.orderWordOf(source);
        Condition hasNoOtherBits = hasNoBits(orderWord, ~(orderAndCountMask | orderWordIntegrityBits));
        branch(logicalAnd(hasNoOtherBits, notEqual(bitAnd(orderWord, orderWordOrderMask), orderStoredOutOfLine)), { normalEdge(copies), rareEdge(fills) });
    }

    m_out.appendTo(copies);
    unsigned fieldBytes = static_cast<unsigned>(WTF::roundUpToMultipleOf<copyChunkBytes>(planned.size)) - offsetOfFirstField;
    ValueFromBlock copiedCell = m_out.anchor(emitter<CellAllocationEmitter>().allocateObjectCopy(allocation, mentioned, source, fieldBytes).value());
    ValueFromBlock didCopy = m_out.anchor(m_out.booleanTrue);
    m_out.jump(continuation);

    m_out.appendTo(fills);
    ValueFromBlock emptyCell = m_out.anchor(allocateEmpty().value());
    ValueFromBlock didNotCopy = m_out.anchor(m_out.booleanFalse);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    m_copyAtAllocation = CopyAtAllocation { copy, Condition(m_out.phi(Machine::Int32, didCopy, didNotCopy)) };
    return CellPointer(m_out.phi(Machine::pointerType(), copiedCell, emptyCell));
}

Vector<FieldIndex, 8> AccessEmitter::fieldsThatTakeCopyFromOtherLayout(const Operation& copy)
{
    Vector<FieldIndex, 8> takers;
    for (FieldIndex field : copy.fields())
        takers.append(findField(m_program, copy.layout(), m_program.at(copy.sourceLayout()).fields[field].key)->field);
    return takers;
}

void AccessEmitter::copyFieldsFromOtherLayout(const Operation& copy, CellPointer source, CellPointer target)
{
    const PlannedLayout& read = m_plan.layout(copy.sourceLayout());
    const PlannedLayout& written = m_plan.layout(copy.layout());
    std::span<const FieldIndex> fieldsRead = copy.fields();
    Vector<FieldIndex, 8> fieldsWritten = fieldsThatTakeCopyFromOtherLayout(copy);
    const Machine::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    SlotEmitter& slots = emitter<SlotEmitter>();
    size_t numberOfWords = read.size / sizeof(uint64_t);
    Vector<uint32_t, 64> offsetsInTarget(FillWith { }, numberOfWords, 0);
    for (size_t index = 0; index < fieldsRead.size(); ++index) {
        const PlacedSlot& slotRead = read.slots[fieldsRead[index]];
        const PlacedSlot& slotWritten = written.slots[fieldsWritten[index]];
        RELEASE_ASSERT(slotRead.storage == SlotStorage::Inline && slotWritten.storage == SlotStorage::Inline && slotRead.representation == slotWritten.representation);
        if (widthOf(slotRead.representation) == sizeof(uint64_t) && !(slotRead.offset % sizeof(uint64_t))) {
            offsetsInTarget[slotRead.offset / sizeof(uint64_t)] = slotWritten.offset;
            continue;
        }
        slots.copyBits(m_out.address(fields, source.value(), slotRead.offset), m_out.address(fields, target.value(), slotWritten.offset), slotRead.representation);
    }
    for (size_t begin = 0, end = 0; begin < numberOfWords; begin = end) {
        end = begin + 1;
        if (!offsetsInTarget[begin])
            continue;
        while (end < numberOfWords && offsetsInTarget[end] == offsetsInTarget[end - 1] + sizeof(uint64_t))
            ++end;
        emitter<CellAllocationEmitter>().copyWords(source, static_cast<unsigned>(begin * sizeof(uint64_t)), target, offsetsInTarget[begin], static_cast<unsigned>((end - begin) * sizeof(uint64_t)));
    }
}

void AccessEmitter::noteCopyMadeAtAllocation(ValueID copy)
{
    m_copyAtAllocation = CopyAtAllocation { copy, boolean(true) };
}

void AccessEmitter::generateTryCopyFields(const ValueContext& value)
{
    if (m_copyAtAllocation && m_copyAtAllocation->copy == value.id()) {
        defineTest(value, std::exchange(m_copyAtAllocation, std::nullopt)->didCopy);
        return;
    }

    LayoutID layout = value->layout();
    const PlannedLayout& planned = m_plan.layout(layout);
    if (value->sourceLayout()) {
        CellPointer target = in<CellPointer>(value->objectOperand());
        copyFieldsFromOtherLayout(value.value(), in<CellPointer>(value->sourceOperand()), target);
        m_out.store32(m_out.constInt32(static_cast<int32_t>(*m_plan.keyOrders().findOrderWord(layout, fieldsThatTakeCopyFromOtherLayout(value.value()).span()))), m_out.address(m_heaps.AOTObject_orderWord, target.value(), offsetOfOrderWord));
        emitter<BarrierEmitter>().barrier(value, target);
        defineTest(value, boolean(true));
        return;
    }
    const ShapeTable& shapes = m_plan.shapes();
    ShapeRange shapesOfLayout = shapes.shapesOf(layout);
    if (!planned.holdsCompiledObjects() || planned.cellsAreHandedBack || shapesOfLayout.isEmpty()) {
        defineTest(value, boolean(false));
        return;
    }

    std::span<const PlacedSlot> slots = planned.slots.span().first(planned.numberOfFields());
    bool mayCopyCell = std::ranges::any_of(slots, [](const PlacedSlot& slot) {
        return slot.hasStorage() && traitsOf(slot.representation).holdsCell;
    });

    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    CellPointer target = in<CellPointer>(value->objectOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    const Machine::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    auto isGeneralShape = [&](Word32 shapeWord) {
        return equal(shapeWord, static_cast<int32_t>(AOT::shapeWordOf(shapesOfLayout.first)));
    };
    LBasicBlock refused = newBlock();
    auto continueIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), normalEdge(refused));
        m_out.appendToContinuation(next);
    };
    Join<Condition> didCopy(m_out);
    continueIf(isGeneralShape(shapeTests.shapeWordWithMarkOf(source)));
    Word32 orderWord = shapeTests.orderWordOf(source);
    continueIf(hasNoBits(orderWord, ~(orderAndCountMask | orderWordIntegrityBits)));
    continueIf(notEqual(bitAnd(orderWord, orderWordOrderMask), orderStoredOutOfLine));
    if (const PlacedSlot& extensionWord = m_plan.slots().extensionWord(layout); extensionWord.hasStorage()) {
        TypedPointer address = m_out.address(fields, source.value(), extensionWord.offset);
        bool isNarrow = traitsOf(extensionWord.representation).isNarrowReference;
        continueIf(isNarrow ? isZero(Word32(m_out.load32(address))) : isZero(Integer(m_out.load64(address))));
    }
    for (const PlacedSlot& slot : slots) {
        if (!slot.hasStorage() || slot.isOutOfLine() || slot.representation != SlotRepresentation::DeferredClosure)
            continue;
        Word32 word(m_out.load32(m_out.address(fields, source.value(), slot.offset)));
        continueIf(notEqual(word, uncreatedDeferredClosure));
    }
    if (planned.size > offsetOfFirstField)
        emitter<CellAllocationEmitter>().copyFieldWords(source, target, static_cast<unsigned>(WTF::roundUpToMultipleOf<copyChunkBytes>(planned.size)) - offsetOfFirstField);
    LValue orderWordOfTarget = bitAnd(orderWord, static_cast<int32_t>(orderAndCountMask)).value();
    if (shapesOfLayout.count > 1)
        orderWordOfTarget = m_out.select(isGeneralShape(shapeTests.shapeWordOf(target)).value(), orderWordOfTarget, m_out.constInt32(unorderedOrderWord));
    m_out.store32(orderWordOfTarget, m_out.address(m_heaps.AOTObject_orderWord, target.value(), offsetOfOrderWord));
    if (planned.mayBearMark)
        shapeTests.clearMark(target);
    if (mayCopyCell)
        emitter<BarrierEmitter>().barrier(value, target);
    didCopy.add(boolean(true));

    m_out.appendTo(refused);
    didCopy.add(boolean(false));
    defineTest(value, didCopy.finish());
}

void AccessEmitter::generateGetSuperBase(const ValueContext& value)
{
    HomeObjectKind homeKind = value->homeKind();
    if (ValueID read = m_code.useCountOf(value.id()) == 1 ? m_code.onlyUserOf(value.id()) : ValueID(); read && homeKind == HomeObjectKind::ClassPrototype && m_code.at(value.block()).frequency == BlockFrequency::Rare && m_code.blockOf(read) == value.block()) {
        const auto& user = m_code.at(read);
        std::span<const ValueID> behind = value.operationsBehind();
        while (!behind.empty() && behind.front() != read && (m_code.at(behind.front()).opcode == Opcode::Box || m_code.at(behind.front()).traits().isPure()))
            behind = behind.subspan(1);
        bool nothingSeenStandsBetween = !behind.empty() && behind.front() == read;
        if (nothingSeenStandsBetween && user.opcode == Opcode::SuperGet && user.baseOperand() == value.id() && user.receiverOperand() != value.id() && user.key() && !liesInThread(user.key()) && !isSymbolOfRealm(user.key())) {
            RELEASE_ASSERT(!m_baseOfSuperLeftToRead);
            m_baseOfSuperLeftToRead = value.id();
            define(value, undefined());
            return;
        }
    }
    CellPointer home = in<CellPointer>(value->homeOperand());
    auto readByOperation = [&](bool isOnRarePath) {
        return callWithoutSite<Boxed>(RuntimeOperation::GetSuperBase, { boxCell(home), word32(static_cast<int32_t>(homeKind)) }, { .isOnRarePath = isOnRarePath });
    };
    if (!operandIsHomeObject(homeKind)) {
        if (LayoutID classLayout = value->layout(); classLayout && m_plan.hasClassRecord(classLayout) && m_facts.isEvaluatedAtMostOnce(classLayout)) {
            define(value, Boxed(callStub(value, getSuperBaseOfClass(), { boxCell(home).value() }, { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::ClassRank, classLayout) } }).first));
            return;
        }
        define(value, readByOperation(false));
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
    Boxed receiver = in<Boxed>(value->receiverOperand());
    PropertyKeyID key = value->key();
    if (ValueID left = std::exchange(m_baseOfSuperLeftToRead, ValueID())) {
        RELEASE_ASSERT(left == value->baseOperand());
        define(value, callOperation<Boxed>(value, RuntimeOperation::GetSuperPropertyOfClassById, { boxCell(in<CellPointer>(m_code.at(left).homeOperand())), receiver, numberOf(mentionOfName(key)) }, { .isOnRarePath = true }));
        return;
    }
    Boxed base = in<Boxed>(value->baseOperand());
    bool keyIsThreadLocal = liesInThread(key);
    if (!key || (!keyIsThreadLocal && isSymbolOfRealm(key))) {
        Boxed property = keyOperand(value, key, value->computedKeyOperand());
        define(value, Boxed(callStub(value, getSuperByVal(), { base.value(), property.value(), receiver.value() }).first));
        return;
    }

    if (!keyIsThreadLocal && value->baseOperand() == value->receiverOperand()) {
        define(value, getById(value, receiver, key));
        return;
    }

    uint32_t name = mentionOfName(key);
    auto readByOperation = [&](bool isOnRarePath) {
        return callOperation<Boxed>(value, RuntimeOperation::GetSuperPropertyById, { base, receiver, numberOf(name) }, { .isOnRarePath = isOnRarePath });
    };
    bool standsInRareBlock = m_code.at(value.block()).frequency == BlockFrequency::Rare;
    if (keyIsThreadLocal || standsInRareBlock) {
        define(value, readByOperation(standsInRareBlock));
        return;
    }

    Boxed found(callStub(value, tryGetSuperById(), { base.value() }, { .numberedImmediate = NumberedImmediate { name } }).first);
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
