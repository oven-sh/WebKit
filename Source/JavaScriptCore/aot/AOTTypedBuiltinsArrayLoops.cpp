/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "ArrayConstructor.h"
#include "IntlCollator.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include <algorithm>

namespace JSC { namespace AOT {

static constexpr uint32_t mustHaveIndexingType = 2;
static constexpr uint32_t arrayIsUnderConstruction = 4;

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayWithHoles, JSCell*, (Instance* instance, JSCell* source, uint64_t length, uint32_t indexingType, uint32_t isCapacity))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (length > UINT32_MAX) {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        OPERATION_RETURN(scope, nullptr);
    }
    bool isUnderConstruction = isCapacity & arrayIsUnderConstruction;
    isCapacity &= ~arrayIsUnderConstruction;
    IndexingType ofSource = source ? source->indexingType() : NonArray;
    bool isStoredInline = isCapacity == mustHaveIndexingType || hasInt32(ofSource) || hasDouble(ofSource) || hasContiguous(ofSource);
    if ((isStoredInline || length < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH) && (isUnderConstruction || !globalObject->isHavingABadTime())) {
        Structure* structure = isUnderConstruction ? instance->arrayUnderConstructionStructure(indexingType) : globalObject->originalArrayStructureForIndexingType(indexingType);
        if (JSArray* array = JSArray::tryCreate(vm, structure, length))
            OPERATION_RETURN(scope, array);
    }
    if (isStoredInline) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr, isCapacity ? 0 : static_cast<uint32_t>(length)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGrowNewArray, void, (Instance* instance, JSCell* array, uint64_t length))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (length <= MAX_STORAGE_VECTOR_LENGTH / 2)
        uncheckedDowncast<JSArray>(array)->setLength(globalObject, std::max<uint32_t>(length * 2, 8), true);
    OPERATION_RETURN(scope);
}

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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTrySortByKey, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue encodedName, uint32_t flags))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    struct WithNumber {
        double key;
        EncodedJSValue element;
    };
    struct WithString {
        StringView key;
        EncodedJSValue element;
    };
    const EncodedJSValue declined = JSValue::encode(jsNull());
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    JSValue name = JSValue::decode(encodedName);
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    bool isDescending = flags & Typed::sortIsDescending;
    if ((shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape) || globalObject->isHavingABadTime())
        OPERATION_RETURN(scope, declined);
    if (shape != ContiguousShape && (name.isString() || flags & Typed::sortComparesByLocale))
        OPERATION_RETURN(scope, declined);
    if (!(flags & Typed::sortMakesCopy) && !globalObject->isOriginalArrayStructure(array->structure()))
        OPERATION_RETURN(scope, declined);
    Identifier ident;
    if (name.isString()) {
        ident = asString(name)->toIdentifier(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    unsigned length = array->length();
    Vector<WithNumber, 32> numbers;
    Vector<WithString> strings;
    StructureID lastStructureID;
    PropertyOffset lastOffset = invalidOffset;
    bool lastHasLayout = false;
    const Typed::FieldData* lastField = nullptr;
    bool isInOrder = true;
    bool isFalling = true;
    unsigned numberCount = 0;
    double lastKey = 0;
    auto appendNumber = [&](double number, EncodedJSValue element) ALWAYS_INLINE_LAMBDA {
        double key = isDescending ? -number : number;
        if (!numberCount)
            numbers.grow(length);
        else {
            isInOrder &= !(key < lastKey);
            isFalling &= key < lastKey;
        }
        lastKey = key;
        numbers[numberCount++] = WithNumber { key, element };
    };
    for (unsigned i = 0; i < length; ++i) {
        if (shape == DoubleShape) {
            double number = array->butterfly()->contiguousDouble().at(array, i);
            if (number != number)
                OPERATION_RETURN(scope, declined);
            appendNumber(number, std::bit_cast<EncodedJSValue>(number));
            continue;
        }
        JSValue element = array->butterfly()->contiguous().at(array, i).get();
        if (!element)
            OPERATION_RETURN(scope, declined);
        if (element.isUndefined())
            continue;
        isInOrder &= numberCount == i;
        JSValue keyValue = element;
        if (!ident.isNull()) {
            if (!element.isCell())
                OPERATION_RETURN(scope, declined);
            JSCell* cell = element.asCell();
            if (cell->structureID() != lastStructureID) [[unlikely]] {
                if (!cell->isObject())
                    OPERATION_RETURN(scope, declined);
                Structure* structure = cell->structure();
                const Typed::LayoutData* layout = structure->compiledLayout();
                lastHasLayout = layout && Typed::isObjectLayout(layout->kind);
                if (lastHasLayout) {
                    auto* typedObject = uncheckedDowncast<Typed::Object>(cell);
                    lastField = typedObject->fieldNamed(typedObject->state(), ident);
                } else {
                    unsigned attributes = 0;
                    PropertyOffset offset = layout || cell->type() != FinalObjectType || structure->isUncacheableDictionary() ? invalidOffset : structure->get(vm, ident, attributes);
                    if (!isValidOffset(offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
                        OPERATION_RETURN(scope, declined);
                    lastOffset = offset;
                }
                lastStructureID = cell->structureID();
            }
            if (lastHasLayout) {
                keyValue = lastField ? uncheckedDowncast<Typed::Object>(cell)->readInline(*lastField) : JSValue();
                if (!keyValue)
                    keyValue = Typed::Object::tryGetOwnData(vm, cell, ident);
            } else
                keyValue = asObject(cell)->getDirect(lastOffset);
        }
        if (!keyValue)
            OPERATION_RETURN(scope, declined);
        if (keyValue.isNumber() && strings.isEmpty() && !(flags & Typed::sortComparesByLocale)) {
            double number = keyValue.asNumber();
            if (number != number)
                OPERATION_RETURN(scope, declined);
            appendNumber(number, JSValue::encode(element));
            continue;
        }
        if (!keyValue.isString() || numberCount || !(flags & (Typed::sortKeysMayBeStrings | Typed::sortComparesByLocale)))
            OPERATION_RETURN(scope, declined);
        auto view = asString(keyValue)->view(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        strings.append(WithString { view, JSValue::encode(element) });
    }
    if (isInOrder && strings.isEmpty() && !(flags & Typed::sortMakesCopy))
        OPERATION_RETURN(scope, JSValue::encode(array));
    numbers.shrink(numberCount);
    if (flags & Typed::sortComparesByLocale) {
        IntlCollator* collator = globalObject->defaultCollator();
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        bool mayCompareASCII = globalObject->canDoASCIIUCADUCETLocaleCompare();
        auto isBefore = [&](StringView a, StringView b) {
            if (mayCompareASCII && a.is8Bit() && b.is8Bit()) {
                if (auto result = compareASCIIWithUCADUCET(a.span8(), b.span8()))
                    return *result < 0;
            }
            return collator->compareStrings(globalObject, a, b) < 0;
        };
        std::stable_sort(strings.begin(), strings.end(), [&](const WithString& a, const WithString& b) {
            return isDescending ? isBefore(b.key, a.key) : isBefore(a.key, b.key);
        });
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    } else {
        std::stable_sort(strings.begin(), strings.end(), [&](const WithString& a, const WithString& b) {
            return isDescending ? codePointCompareLessThan(b.key, a.key) : codePointCompareLessThan(a.key, b.key);
        });
    }
    constexpr unsigned maxSizeForInsertionSort = 16;
    if (isFalling)
        numbers.reverse();
    else if (numbers.size() <= maxSizeForInsertionSort) {
        for (unsigned i = 1; i < numbers.size(); ++i) {
            WithNumber inserted = numbers[i];
            unsigned at = i;
            for (; at && inserted.key < numbers[at - 1].key; --at)
                numbers[at] = numbers[at - 1];
            numbers[at] = inserted;
        }
    } else if (!isInOrder) {
        std::stable_sort(numbers.begin(), numbers.end(), [](const WithNumber& a, const WithNumber& b) {
            return a.key < b.key;
        });
    }
    JSArray* sorted = array;
    if (flags & Typed::sortMakesCopy) {
        sorted = JSArray::tryCreate(vm, globalObject->originalArrayStructureForIndexingType(IsArray | shape), length);
        if (!sorted)
            OPERATION_RETURN(scope, declined);
    } else
        array->ensureWritable(vm);
    unsigned next = 0;
    for (auto& entry : numbers) {
        if (shape == DoubleShape)
            sorted->butterfly()->contiguousDouble().at(sorted, next++) = std::bit_cast<double>(entry.element);
        else
            sorted->butterfly()->contiguous().at(sorted, next++).setWithoutWriteBarrier(JSValue::decode(entry.element));
    }
    for (auto& entry : strings)
        sorted->butterfly()->contiguous().at(sorted, next++).setWithoutWriteBarrier(JSValue::decode(entry.element));
    while (next < length)
        sorted->butterfly()->contiguous().at(sorted, next++).setWithoutWriteBarrier(jsUndefined());
    vm.writeBarrier(sorted);
    OPERATION_RETURN(scope, JSValue::encode(sorted));
}

namespace Typed {

using namespace B3;

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
    case InlineBuiltin::TrySortByKey:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

LValue Lowering::preloadedElement(ValueID array, ValueID index)
{
    if (m_preloadedElements.isEmpty())
        return nullptr;
    auto found = m_preloadedElements.find(m_currentBlockID.index());
    if (found == m_preloadedElements.end() || found->value.array != array || found->value.index != index)
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
    LBasicBlock notIndex = newColdBlock();
    Integer index = indexOrElse(id, notIndex);
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(notIndex);
    m_out.unreachable();
    m_out.appendTo(continuation);
    return index;
}

IndexingType Lowering::indexingTypeAtCreation(ValueID newArray) const
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
            areInt32 &= stored.isInteger() && stored.integers.isSubsetOf(Range::int32());
        }
    }
    if (!isStoredTo || !areNumbers)
        return ArrayWithContiguous;
    return areInt32 ? ArrayWithInt32 : ArrayWithDouble;
}

CellPointer Lowering::newArrayWithHoles(ValueID result, const Value& value, bool isCapacity)
{
    IndexingType indexingType = indexingTypeAtCreation(result);
    Integer length = indexOperand(value.arguments[0]);
    CellPointer source = typeOf(value.arguments[1]).isUndefinedOrNull() ? CellPointer(m_out.int64Zero) : arrayOperand(value.arguments[1]);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(allocateArrayWithHoles(length, indexingType, slowPath, true).value());
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayWithHoles, source, length, word32(indexingType), word32(isCapacity | arrayIsUnderConstruction)).value());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    CellPointer made(m_out.phi(Int64, fast, slow));
    ensureStillAliveHere(CellPointer(butterflyOf(made).value()));
    return made;
}

std::optional<Operand> Lowering::lowerInlineArrayLoops(InlineBuiltin which, ValueID result, const Value& value, Writes inlinePath)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto mayContainNumbers = [&](ValueID array) {
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
            return booleanResult(Word32(isOriginalArray(operand.value)), type);
        if (operand.rep != Rep::JSValue)
            return booleanResult(boolean(false), type);
        Boxed boxed = operand.as<Boxed>();
        return booleanResult(either<Word32>(isCell(boxed), [&] {
            return Word32(isOriginalArray(asCell(boxed).value()));
        }, [&] {
            return boolean(false);
        }), type);
    }
    case InlineBuiltin::IsPlainArrayLike: {
        ValueID object = arguments[0];
        while (!typeOf(object).singleLayout() && m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        LayoutID view = typeOf(object).singleLayout();
        Operand operand = get(object);
        if (!holdsCells(operand.rep) || !typeOf(object).hasObjects())
            return booleanResult(boolean(false), type);
        LValue objectPrototype = fixedPointer(Instance::offsetOfObjectPrototype());
        CellPointer cell = operand.rep == Rep::Cell ? operand.as<CellPointer>() : CellPointer(m_out.select(isCell(operand.as<Boxed>()).value(), asCell(operand.as<Boxed>()).value(), objectPrototype));
        LValue structure = structureWithID(loadField<Word32>(cell, m_heaps.JSCell_structureID).value());
        if (!view || m_program.at(view).kind != LayoutKind::Record) {
            using Seen = Structure::SeenProperties;
            if constexpr (sizeof(Seen) != sizeof(void*))
                return booleanResult(boolean(false), type);
            static const BuiltinID iterator = m_builtins.findStatic(BuiltinKind::Constant, m_builtins.nameOf(asBytes("Symbol"_s.span8())), m_builtins.nameOf(asBytes("iterator"_s.span8())));
            RELEASE_ASSERT(iterator);
            static_assert(!NoIndexingShape);
            LValue name = m_out.loadPtr(builtinCell(m_builtins.cellOf(iterator)).value(), m_heaps.Symbol_symbolImpl);
            Word32 hasNotSeenName(m_out.notEqual(m_out.bitAnd(m_out.loadPtr(structure, m_heaps.Structure_seenProperties), name), name));
            Word32 isOrdinary = logicalAnd(Word32(isCellOfType(cell.value(), FinalObjectType)), isZero(anyArrayIndexingMode(cell, IndexingShapeMask)));
            LValue orderAndShapeWords = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
            Word32 isProgramObjectWithOnlyFields(m_out.bitAnd(m_out.aboveOrEqual(orderAndShapeWords, m_out.constInt64(0xffULL << 56)), m_out.testIsZero64(orderAndShapeWords, m_out.constInt64(orderWordHasExtraProperties))));
            return booleanResult(logicalAnd(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), objectPrototype)), logicalOr(logicalAnd(isOrdinary, hasNotSeenName), isProgramObjectWithOnlyFields)), type);
        }
        Word32 isPlain = logicalAnd(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), objectPrototype)), hasNoBits(orderWordOf(cell), orderWordHasExtraProperties));
        return booleanResult(logicalAnd(isPlain, passes(shapeWordOf(cell), m_program.at(view).test)), type);
    }
    case InlineBuiltin::IsDoubleArray:
        if (!mayContainNumbers(arguments[0]))
            return booleanResult(boolean(false), type);
        return booleanResult(equal(anyArrayIndexingMode(arrayOperand(arguments[0]), IndexingShapeMask), word32(DoubleShape)), type);
    case InlineBuiltin::IsContiguousArray: {
        Word32 shape = anyArrayIndexingMode(arrayOperand(arguments[0]), IndexingShapeMask);
        if (!mayContainNumbers(arguments[0]))
            return booleanResult(equal(shape, word32(ContiguousShape)), type);
        return booleanResult(isContiguousOrInt32Shape(shape), type);
    }
    case InlineBuiltin::GetDoubleIndex:
        return ensure(Number(m_out.loadDouble(doubleAt(arrayOperand(arguments[0]), indexOperand(arguments[1])))), m_program.at(m_program.types.number), type);
    case InlineBuiltin::HasContiguousIndex: {
        CellPointer array = arrayOperand(arguments[0]);
        LValue element = m_out.load64(valueAt(array, indexOperand(arguments[1])));
        ensureStillAliveHere(array);
        auto& terminator = m_currentBlock->terminator;
        if (terminator.kind == TerminatorKind::Branch && terminator.value == result && m_currentBlock->values.last() == result) {
            BlockID hasProperty = terminator.targets[0].block;
            if (hasProperty.index() && hasProperty != terminator.targets[1].block && numberOfPredecessorsOf(hasProperty) == 1)
                m_preloadedElements.set(hasProperty.index(), PreloadedElement { arguments[0], arguments[1], element });
        }
        return booleanResult(isNotEmpty(Boxed(element)), type);
    }
    case InlineBuiltin::GetContiguousIndex: {
        if (LValue element = preloadedElement(arguments[0], arguments[1]))
            return ensure(Boxed(element), type);
        CellPointer array = arrayOperand(arguments[0]);
        Boxed element(m_out.load64(valueAt(array, indexOperand(arguments[1]))));
        ensureStillAliveHere(array);
        return ensure(element, type);
    }
    case InlineBuiltin::NewArrayWithSpecies:
    case InlineBuiltin::NewArrayWithCapacity:
        return ensure(newArrayWithHoles(result, value, which == InlineBuiltin::NewArrayWithCapacity), Type::engineClass(m_builtins.arrayClass()), type);
    case InlineBuiltin::TryPutByValDirect: {
        ValueID newArray = originOf(arguments[0]);
        if (!isInlineBuiltin(newArray, InlineBuiltin::NewArrayWithSpecies) && !isInlineBuiltin(newArray, InlineBuiltin::NewArrayWithCapacity))
            return booleanResult(boolean(false), type);
        CellPointer array = arrayOperand(arguments[0]);
        Integer index = indexOperand(arguments[1]);
        ValueID stored = arguments[2];
        didEmitWrite(inlinePath);
        switch (indexingTypeAtCreation(newArray)) {
        case ArrayWithInt32:
            m_out.store64(boxInt32(low32(in<Integer>(stored))).value(), valueAt(array, index));
            return booleanResult(boolean(true), type);
        case ArrayWithDouble: {
            Number number = in<Number>(stored);
            m_out.storeDouble(number.value(), doubleAt(array, index));
            return booleanResult(typeOf(stored).isInteger() ? boolean(true) : equal(number, number), type);
        }
        default:
            m_out.store64(in<Boxed>(stored).value(), valueAt(array, index));
            if (typeOf(stored).hasCells())
                barrier(array);
            return booleanResult(boolean(true), type);
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
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayWithHoles, CellPointer(m_out.int64Zero), length, word32(indexingType), word32(mustHaveIndexingType | arrayIsUnderConstruction)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(CellPointer(m_out.phi(Int64, fast, slow)), Type::engineClass(m_builtins.arrayClass()), type);
    }
    case InlineBuiltin::PutDoubleIndex:
        didEmitWrite(inlinePath);
        m_out.storeDouble(in<Number>(arguments[2]).value(), doubleAt(arrayOperand(arguments[0]), indexOperand(arguments[1])));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::PutContiguousIndex: {
        CellPointer array = arrayOperand(arguments[0]);
        didEmitWrite(inlinePath);
        m_out.store64(in<Boxed>(arguments[2]).value(), valueAt(array, indexOperand(arguments[1])));
        if (typeOf(arguments[2]).hasCells())
            barrier(array);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::GrowNewArray:
        AOT_TYPED_OPERATION_WITH_EFFECTS(GrowNewArray, (OperationEffects { Writes::Heap, true, false }), arrayOperand(arguments[0]), indexOperand(arguments[1]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::TrySortByKey:
        return ensure(AOT_TYPED_OPERATION(TrySortByKey, arrayOperand(arguments[0]), in<Boxed>(arguments[1]), low32(indexOperand(arguments[2]))), type);
    case InlineBuiltin::TryPutIndex:
        didEmitWrite(inlinePath);
        return booleanResult(tryStoreArrayElement(arrayOperand(arguments[0]), indexOperand(arguments[1]), arguments[2]), type);
    case InlineBuiltin::TruncateNewArray: {
        CellPointer array = arrayOperand(arguments[0]);
        Integer length = indexOperand(arguments[1]);
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        didEmitWrite(inlinePath);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        Word32 kind = binary<Word32>(&Output::sub, anyArrayIndexingMode(array, IndexingShapeAndWritabilityMask), word32(Int32Shape));
        orElse(belowOrEqual(kind, word32(ContiguousShape - Int32Shape)), slowPath);
        static_assert(OBJECT_OFFSETOF(Instance::NegativeOffsetFields, newArrayWithDoubleHeader) == OBJECT_OFFSETOF(Instance::NegativeOffsetFields, newArrayWithInt32Header) + sizeof(uint64_t) && OBJECT_OFFSETOF(Instance::NegativeOffsetFields, newArrayWithContiguousHeader) == OBJECT_OFFSETOF(Instance::NegativeOffsetFields, newArrayWithInt32Header) + 2 * sizeof(uint64_t));
        LValue headers = m_out.add(m_instance, m_out.shl(m_out.zeroExtPtr(kind.value()), m_out.constInt32(2)));
        LValue underConstructionHeader = m_out.load64(TypedPointer(m_heaps.AOTInstance_mutableFields, m_out.add(headers, m_out.constIntPtr(Instance::offsetOfArrayUnderConstructionHeaders()))));
        orElse(Word32(m_out.equal(m_out.load32(array.value(), m_heaps.JSCell_structureID), m_out.castToInt32(underConstructionHeader))), slowPath);
        LValue original = m_out.load64(TypedPointer(m_heaps.AOTInstance_mutableFields, m_out.add(headers, m_out.constIntPtr(Instance::offsetOfNewArrayHeader(ArrayWithInt32)))));
        orElse(Word32(m_out.notZero64(original)), slowPath);
        m_out.store32(m_out.castToInt32(original), array.value(), m_heaps.JSCell_structureID);
        Pointer butterfly = butterflyOf(array);
        constexpr int64_t smallUnusedCapacity = 8;
        Integer unusedCapacity = sub(publicLength(butterfly), length);
        orElse(logicalOr(belowOrEqual(unusedCapacity, integer(smallUnusedCapacity)), belowOrEqual(unusedCapacity, length)), slowPath);
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
