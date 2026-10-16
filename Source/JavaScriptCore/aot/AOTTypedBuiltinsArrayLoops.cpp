/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "ArrayConstructor.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

// For `isCapacity` of operationAOTTypedNewArrayWithHoles.
static constexpr uint32_t mustHaveIndexingType = 2;
static constexpr uint32_t arrayIsInTheMaking = 4;

// Where compiled code goes on to store without a look, the array is of the indexing type: for a source that has its elements in place, and where there is no source and
// a length is asked for, which then is that of an array with its elements in place.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayWithHoles, JSCell*, (Instance* instance, JSCell* source, uint64_t length, uint32_t indexingType, uint32_t isCapacity))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (length > UINT32_MAX) {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        OPERATION_RETURN(scope, nullptr);
    }
    bool isInTheMaking = isCapacity & arrayIsInTheMaking;
    isCapacity &= ~arrayIsInTheMaking;
    IndexingType ofSource = source ? source->indexingType() : NonArray;
    bool isStoredToInPlace = isCapacity == mustHaveIndexingType || hasInt32(ofSource) || hasDouble(ofSource) || hasContiguous(ofSource);
    if ((isStoredToInPlace || length < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH) && (isInTheMaking || !globalObject->isHavingABadTime())) {
        Structure* structure = isInTheMaking ? instance->arrayInTheMakingStructure(indexingType) : globalObject->originalArrayStructureForIndexingType(indexingType);
        if (JSArray* array = JSArray::tryCreate(vm, structure, length))
            OPERATION_RETURN(scope, array);
    }
    if (isStoredToInPlace) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr, isCapacity ? 0 : static_cast<uint32_t>(length)));
}

// Up to its new length, which is twice what is asked for, the array has holes. Where that is too much nothing is done.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedGrowNewArray, void, (Instance* instance, JSCell* array, uint64_t length))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (length <= MAX_STORAGE_VECTOR_LENGTH / 2)
        uncheckedDowncast<JSArray>(array)->setLength(globalObject, std::max<uint32_t>(length * 2, 8), true);
    OPERATION_RETURN(scope);
}

// Nobody has seen the array, so another will do in its place: one without the room that was not needed.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedTruncateNewArray, JSCell*, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    if (!globalObject->isOriginalArrayStructure(array->structure())) {
        array->setStructure(vm, globalObject->originalArrayStructureForIndexingType(array->indexingType()));
        if (globalObject->isHavingABadTime() && !hasSlowPutArrayStorage(array->indexingType()))
            array->switchToSlowPutArrayStorage(vm);
    }
    if (length < array->length()) {
        if (JSArray* copy = JSArray::fastSlice(globalObject, array, 0, length))
            OPERATION_RETURN(scope, copy);
    }
    array->setLength(globalObject, length, true);
    OPERATION_RETURN(scope, array);
}

namespace Typed {

using namespace B3;

// The family aot/builtins/array-loops.def.

std::optional<Writes> writesOfInlineArrayLoops(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::IsOriginalArray:
    case InlineBuiltin::IsDoubleArray:
    case InlineBuiltin::IsContiguousArray:
    case InlineBuiltin::GetDoubleIndex:
    case InlineBuiltin::HasContiguousIndex:
    case InlineBuiltin::GetContiguousIndex:
    case InlineBuiltin::IsPlainArrayLike:
        return Writes::Nothing;
    // To an array that nobody else has seen.
    case InlineBuiltin::NewArrayWithSpecies:
    case InlineBuiltin::NewArrayWithCapacity:
    case InlineBuiltin::TryPutByValDirect:
    case InlineBuiltin::NewDoubleArray:
    case InlineBuiltin::NewContiguousArray:
    case InlineBuiltin::PutDoubleIndex:
    case InlineBuiltin::PutContiguousIndex:
        return Writes::RuntimeState;
    case InlineBuiltin::TruncateNewArray:
    case InlineBuiltin::GrowNewArray:
    case InlineBuiltin::TryPutIndex:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

LValue Lowering::elementLoadedAhead(ValueID array, ValueID index)
{
    if (m_elementsLoadedAhead.isEmpty())
        return nullptr;
    auto found = m_elementsLoadedAhead.find(m_currentBlockID.index());
    if (found == m_elementsLoadedAhead.end() || found->value.array != array || found->value.index != index)
        return nullptr;
    for (size_t i = 0; i < m_currentIndexInBlock; ++i) {
        switch (m_function.at(m_currentBlock->values[i]).opcode) {
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
            return nullptr;
        default:
            break;
        }
        if (writesOf(m_program, m_function, *m_currentBlock, i) != Writes::Nothing)
            return nullptr;
    }
    return found->value.element;
}

bool Lowering::isInlineBuiltin(ValueID id, InlineBuiltin which) const
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == which;
}

CellPointer Lowering::arrayOperand(ValueID id)
{
    Operand operand = get(id);
    return operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>());
}

Integer Lowering::indexOperand(ValueID id)
{
    LBasicBlock isNoIndex = newColdBlock();
    Integer index = indexOrElse(id, isNoIndex);
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(isNoIndex);
    m_out.unreachable();
    m_out.appendTo(continuation);
    return index;
}

// By what TryPutByValDirect stores to it, going by the types. Of the whole function: the birth and the stores may be in different parts of it, see FunctionSplitting.
IndexingType Lowering::indexingTypeAtBirth(ValueID newArray) const
{
    bool isStoredTo = false;
    bool areNumbers = true;
    bool areInt32 = true;
    for (auto& block : m_program.at(m_id).blocks) {
        for (ValueID id : block.values) {
            if (!isInlineBuiltin(id, InlineBuiltin::TryPutByValDirect) || originOf(m_function.at(id).arguments[0]) != newArray)
                continue;
            auto& stored = m_program.at(m_function.at(m_function.at(id).arguments[2]).type);
            isStoredTo = true;
            areNumbers &= stored.isNumber();
            areInt32 &= stored.isInteger() && stored.integers.isWithin(Range::int32());
        }
    }
    if (!isStoredTo || !areNumbers)
        return ArrayWithContiguous;
    return areInt32 ? ArrayWithInt32 : ArrayWithDouble;
}

// The arguments: the length, and the array it is made for. Nothing but holes, up to the capacity.
CellPointer Lowering::newArrayWithHoles(ValueID result, const Value& value, bool isCapacity)
{
    IndexingType indexingType = indexingTypeAtBirth(result);
    Integer length = indexOperand(value.arguments[0]);
    CellPointer source = typeOf(value.arguments[1]).isUndefinedOrNull() ? CellPointer(m_out.int64Zero) : arrayOperand(value.arguments[1]);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(allocateArrayWithHoles(length, indexingType, slowPath, true).value());
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayWithHoles, source, length, word32(indexingType), word32(isCapacity | arrayIsInTheMaking)).value());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    CellPointer made(m_out.phi(Int64, fast, slow));
    // Read here, so that a loop that stores to the array and changes nothing else does not read it again.
    ensureStillAliveHere(CellPointer(butterflyOf(made).value()));
    return made;
}

std::optional<Operand> Lowering::lowerInlineArrayLoops(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    // Going by what is expected of its elements, which decides nothing but which loops there are.
    auto mayHoldNumbers = [&](ValueID array) {
        for (;; array = m_function.at(array).child(0)) {
            TypeID elements = typeOf(array).elements;
            if (elements && !(m_program.at(elements).primitives & Primitive::Number))
                return false;
            if (m_function.at(array).opcode != Opcode::Cast)
                return true;
        }
    };
    auto doubleAt = [&](CellPointer array, Integer index) {
        return m_out.baseIndex(m_heaps.indexedDoubleProperties, butterflyOf(array).value(), index.value());
    };
    auto valueAt = [&](CellPointer array, Integer index) {
        return m_out.baseIndex(m_heaps.indexedContiguousProperties, butterflyOf(array).value(), index.value());
    };

    switch (which) {
    case InlineBuiltin::IsOriginalArray: {
        Operand operand = get(arguments[0]);
        if (operand.rep == Rep::Cell)
            return yieldBoolean(Word32(isOriginalArray(operand.value)), type);
        if (operand.rep != Rep::JSValue)
            return yieldBoolean(boolean(false), type);
        Boxed boxed = operand.as<Boxed>();
        return yieldBoolean(either<Word32>(isCell(boxed), [&] {
            return Word32(isOriginalArray(asCell(boxed).value()));
        }, [&] {
            return boolean(false);
        }), type);
    }
    // An object of the program that has nothing but its fields, none of which is named by an index or a symbol, and inherits from Object.prototype alone, which has no
    // elements and no @@iterator and gets none. Of what is not looked at as a record type of the program nothing is known here.
    case InlineBuiltin::IsPlainArrayLike: {
        ValueID object = arguments[0];
        while (!typeOf(object).onlyView() && m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        LayoutID view = typeOf(object).onlyView();
        Operand operand = get(object);
        if (!holdsCells(operand.rep) || !typeOf(object).hasObjects())
            return yieldBoolean(boolean(false), type);
        // In the place of `undefined` or `null` stands an object that is always there and is not plain: its prototype is `null`.
        LValue objectPrototype = fixedPointer(Instance::offsetOfObjectPrototype());
        CellPointer cell = operand.rep == Rep::Cell ? operand.as<CellPointer>() : CellPointer(m_out.select(isCell(operand.as<Boxed>()).value(), asCell(operand.as<Boxed>()).value(), objectPrototype));
        LValue structure = structureWithID(loadField<Word32>(cell, m_heaps.JSCell_structureID).value());
        if (!view || m_program.at(view).kind != LayoutKind::Record) {
            // `{ length: n }` of code without types, or checked against a type of the library: an ordinary object of the engine that has no elements, inherits from Object.prototype alone, and has no
            // @@iterator of its own: its Structure has never seen the name.
            using Seen = Structure::SeenProperties;
            if constexpr (sizeof(Seen) != sizeof(void*))
                return yieldBoolean(boolean(false), type);
            static const BuiltinID iterator = m_builtins.findStatic(BuiltinKind::Constant, m_builtins.nameOf(asBytes("Symbol"_s.span8())), m_builtins.nameOf(asBytes("iterator"_s.span8())));
            RELEASE_ASSERT(iterator);
            static_assert(!NoIndexingShape);
            LValue name = m_out.loadPtr(builtinCell(m_builtins.cellOf(iterator)).value(), m_heaps.Symbol_symbolImpl);
            Word32 hasNotSeenName(m_out.notEqual(m_out.bitAnd(m_out.loadPtr(structure, m_heaps.Structure_seenProperties), name), name));
            Word32 isOrdinary = logicalAnd(Word32(isCellOfType(cell.value(), FinalObjectType)), isZero(indexingModeOfAnyArray(cell, IndexingShapeMask)));
            LValue orderAndShapeWords = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
            Word32 isOfProgramWithFieldsAlone(m_out.bitAnd(m_out.aboveOrEqual(orderAndShapeWords, m_out.constInt64(0xffULL << 56)), m_out.testIsZero64(orderAndShapeWords, m_out.constInt64(orderWordHasExtraProperties))));
            return yieldBoolean(logicalAnd(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), objectPrototype)), logicalOr(logicalAnd(isOrdinary, hasNotSeenName), isOfProgramWithFieldsAlone)), type);
        }
        Word32 isPlain = logicalAnd(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), objectPrototype)), lacksBits(orderWordOf(cell), orderWordHasExtraProperties));
        return yieldBoolean(logicalAnd(isPlain, passes(shapeWordOf(cell), m_program.at(view).test)), type);
    }
    // Of what was taken for an array: with its elements in place it still is as it was made. See EngineClass::type.
    case InlineBuiltin::IsDoubleArray:
        if (!mayHoldNumbers(arguments[0]))
            return yieldBoolean(boolean(false), type);
        return yieldBoolean(equal(indexingModeOfAnyArray(arrayOperand(arguments[0]), IndexingShapeMask), word32(DoubleShape)), type);
    case InlineBuiltin::IsContiguousArray: {
        Word32 shape = indexingModeOfAnyArray(arrayOperand(arguments[0]), IndexingShapeMask);
        Word32 isContiguous = equal(shape, word32(ContiguousShape));
        if (!mayHoldNumbers(arguments[0]))
            return yieldBoolean(isContiguous, type);
        return yieldBoolean(logicalOr(isContiguous, equal(shape, word32(Int32Shape))), type);
    }
    case InlineBuiltin::GetDoubleIndex:
        return ensure(Number(m_out.loadDouble(doubleAt(arrayOperand(arguments[0]), indexOperand(arguments[1])))), m_program.at(m_program.types.number), type);
    case InlineBuiltin::HasContiguousIndex: {
        CellPointer array = arrayOperand(arguments[0]);
        LValue element = m_out.load64(valueAt(array, indexOperand(arguments[1])));
        ensureStillAliveHere(array);
        auto& terminator = m_currentBlock->terminator;
        if (terminator.kind == TerminatorKind::Branch && terminator.value == result && m_currentBlock->values.last() == result) {
            BlockID hasIt = terminator.targets[0].block;
            if (hasIt.index() && hasIt != terminator.targets[1].block && numberOfPredecessorsOf(hasIt) == 1)
                m_elementsLoadedAhead.set(hasIt.index(), ElementLoadedAhead { arguments[0], arguments[1], element });
        }
        return yieldBoolean(isNotEmpty(Boxed(element)), type);
    }
    case InlineBuiltin::GetContiguousIndex: {
        if (LValue element = elementLoadedAhead(arguments[0], arguments[1]))
            return ensure(Boxed(element), type);
        CellPointer array = arrayOperand(arguments[0]);
        Boxed element(m_out.load64(valueAt(array, indexOperand(arguments[1]))));
        ensureStillAliveHere(array);
        return ensure(element, type);
    }
    case InlineBuiltin::NewArrayWithSpecies:
    case InlineBuiltin::NewArrayWithCapacity:
        return ensure(newArrayWithHoles(result, value, which == InlineBuiltin::NewArrayWithCapacity), Type::engineClass(m_builtins.arrayClass()), type);
    // The array is as it was born, and the index within its length. What an ArrayWithDouble cannot hold reads as a hole there.
    case InlineBuiltin::TryPutByValDirect: {
        ValueID newArray = originOf(arguments[0]);
        if (!isInlineBuiltin(newArray, InlineBuiltin::NewArrayWithSpecies) && !isInlineBuiltin(newArray, InlineBuiltin::NewArrayWithCapacity))
            return yieldBoolean(boolean(false), type);
        CellPointer array = arrayOperand(arguments[0]);
        Integer index = indexOperand(arguments[1]);
        ValueID stored = arguments[2];
        didEmitWrite(inPlace);
        switch (indexingTypeAtBirth(newArray)) {
        case ArrayWithInt32:
            m_out.store64(boxInt32(low32(in<Integer>(stored))).value(), valueAt(array, index));
            return yieldBoolean(boolean(true), type);
        case ArrayWithDouble: {
            Number number = in<Number>(stored);
            m_out.storeDouble(number.value(), doubleAt(array, index));
            return yieldBoolean(typeOf(stored).isInteger() ? boolean(true) : equal(number, number), type);
        }
        default:
            m_out.store64(in<Boxed>(stored).value(), valueAt(array, index));
            if (typeOf(stored).hasCells())
                barrier(array);
            return yieldBoolean(boolean(true), type);
        }
    }
    case InlineBuiltin::NewDoubleArray:
    case InlineBuiltin::NewContiguousArray: {
        IndexingType indexingType = which == InlineBuiltin::NewDoubleArray ? ArrayWithDouble : ArrayWithContiguous;
        Integer length = indexOperand(arguments[0]);
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fast = m_out.anchor(allocateArrayWithHoles(length, indexingType, slowPath, true).value());
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayWithHoles, CellPointer(m_out.int64Zero), length, word32(indexingType), word32(mustHaveIndexingType | arrayIsInTheMaking)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(CellPointer(m_out.phi(Int64, fast, slow)), Type::engineClass(m_builtins.arrayClass()), type);
    }
    // The array keeps its elements that way, and the index is within its length.
    case InlineBuiltin::PutDoubleIndex:
        didEmitWrite(inPlace);
        m_out.storeDouble(in<Number>(arguments[2]).value(), doubleAt(arrayOperand(arguments[0]), indexOperand(arguments[1])));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::PutContiguousIndex: {
        CellPointer array = arrayOperand(arguments[0]);
        didEmitWrite(inPlace);
        m_out.store64(in<Boxed>(arguments[2]).value(), valueAt(array, indexOperand(arguments[1])));
        if (typeOf(arguments[2]).hasCells())
            barrier(array);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::GrowNewArray:
        AOT_TYPED_OPERATION_WITH_EFFECTS(GrowNewArray, (OperationEffects { Writes::Heap, true, false }), arrayOperand(arguments[0]), indexOperand(arguments[1]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::TryPutIndex:
        didEmitWrite(inPlace);
        return yieldBoolean(tryStoreArrayElement(arrayOperand(arguments[0]), indexOperand(arguments[1]), arguments[2]), type);
    // From the new length on there are only holes.
    case InlineBuiltin::TruncateNewArray: {
        CellPointer array = arrayOperand(arguments[0]);
        Integer length = indexOperand(arguments[1]);
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        didEmitWrite(inPlace);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        Word32 kind = binary<Word32>(&Output::sub, indexingModeOfAnyArray(array, IndexingShapeAndWritabilityMask), word32(Int32Shape));
        orElse(belowOrEqual(kind, word32(ContiguousShape - Int32Shape)), slowPath);
        static_assert(OBJECT_OFFSETOF(Instance::FieldsBelow, newArrayWithDoubleHeader) == OBJECT_OFFSETOF(Instance::FieldsBelow, newArrayWithInt32Header) + sizeof(uint64_t) && OBJECT_OFFSETOF(Instance::FieldsBelow, newArrayWithContiguousHeader) == OBJECT_OFFSETOF(Instance::FieldsBelow, newArrayWithInt32Header) + 2 * sizeof(uint64_t));
        LValue headers = m_out.add(m_instance, m_out.shl(m_out.zeroExtPtr(kind.value()), m_out.constInt32(2)));
        LValue inTheMaking = m_out.load64(TypedPointer(m_heaps.AOTInstance_mutableFields, m_out.add(headers, m_out.constIntPtr(Instance::offsetOfArrayInTheMakingHeaders()))));
        orElse(Word32(m_out.equal(m_out.load32(array.value(), m_heaps.JSCell_structureID), m_out.castToInt32(inTheMaking))), slowPath);
        LValue original = m_out.load64(TypedPointer(m_heaps.AOTInstance_mutableFields, m_out.add(headers, m_out.constIntPtr(Instance::offsetOfNewArrayHeader(ArrayWithInt32)))));
        orElse(Word32(m_out.notZero64(original)), slowPath);
        m_out.store32(m_out.castToInt32(original), array.value(), m_heaps.JSCell_structureID);
        Pointer butterfly = butterflyOf(array);
        // No more than is used, or than an empty array has anyway.
        constexpr int64_t fewLeftUnused = 8;
        Integer leftUnused = sub(publicLength(butterfly), length);
        orElse(logicalOr(belowOrEqual(leftUnused, integer(fewLeftUnused)), belowOrEqual(leftUnused, length)), slowPath);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(length));
        ValueFromBlock fast = m_out.anchor(array.value());
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION_WITH_EFFECTS(TruncateNewArray, (OperationEffects { Writes::Heap, true, false }), array, low32(length)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(CellPointer(m_out.phi(Int64, fast, slow)), Type::engineClass(m_builtins.arrayClass()), type);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
