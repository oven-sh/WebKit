/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTRuntimeValidation.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStringEmitter.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "JSArray.h"
#include "JSCJSValueInlines.h"
#include "JSTypeInfo.h"
#include "Structure.h"
#include <algorithm>
#include <bit>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(arrayStorageForms, [](const ArrayStorageForm& form) {
    return form.element == Representation::Int32 || form.element == Representation::Double || form.element == Representation::JSValue;
}));

namespace {

class StorageGuardValidator final : public OperationEmitter {
public:
    explicit StorageGuardValidator(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void validate(const ValueContext&, CellPointer array, ArrayStorage);
};

void StorageGuardValidator::validate(const ValueContext& value, CellPointer array, ArrayStorage kind)
{
    LBasicBlock isInvalid = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(emitter<ElementAccessEmitter>().hasArrayStorage(array, kind).value(), normalEdge(continuation), rareEdge(isInvalid));

    m_out.appendTo(isInvalid);
    callVoidOperation(value, RuntimeOperation::ReportValidationFailure, { boxCell(array), word32(static_cast<int32_t>(ValidationFailureKind::ValueType)), word32(m_id.index()), word32(value.id().index()) }, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(continuation);
}

} // anonymous namespace

static JSValue constantIndexOf(Integer index)
{
    LValue value = index.value();
    if (!value->hasInt64() || value->asInt64() < 0 || value->asInt64() > INT32_MAX)
        return { };
    return jsNumber(static_cast<int32_t>(value->asInt64()));
}

template<typename Functor>
static void forEachFormByShape(ElementAccessEmitter& elements, Output& out, Word32 shape, LBasicBlock hasNoForm, const Functor& emitArm)
{
    for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse) {
        bool isLastForm = &form == &arrayStorageForms.front();
        LBasicBlock hasForm = out.newBlock();
        LBasicBlock hasOtherForm = out.newBlock();
        out.branch(elements.hasShape(shape, form).value(), normalEdge(hasForm), isLastForm ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
        out.appendTo(hasForm);
        emitArm(form);
        out.appendTo(hasOtherForm);
    }
    out.jump(hasNoForm);
}

ElementAccessEmitter::ElementAccessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Condition ElementAccessEmitter::isPristineArray(CellPointer cell, OptionSet<PristineAspects> aspects)
{
    Condition result;
    if (aspects.contains(PristineAspects::Names))
        result = hasBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine);
    if (aspects.contains(PristineAspects::Storage)) {
        Word32 bitField = loadField<Word32>(loadStructure(cell), m_heaps.Structure_bitField);
        Condition hasPristineElements = hasBits(bitField, Structure::s_hasPristineElementsBits);
        result = result ? logicalAnd(result, hasPristineElements) : hasPristineElements;
    }
    return result ? result : boolean(true);
}

Condition ElementAccessEmitter::hasArrayStorage(CellPointer array, ArrayStorage kind, bool isWritable)
{
    if (isWritable)
        return hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), arrayStorageFormOf(kind));
    return hasShape(indexingMode(array, arrayStorageTestMask), arrayStorageFormOf(kind));
}

Word32 ElementAccessEmitter::indexingMode(CellPointer array, IndexingType mask)
{
    return bitAnd(loadByte(array, m_heaps.JSCell_indexingTypeAndMisc), word32(mask));
}

Condition ElementAccessEmitter::hasShape(Word32 maskedMode, const ArrayStorageForm& form)
{
    return equal(maskedMode, word32(form.shape));
}

Condition ElementAccessEmitter::hasWritableShape(Word32 modeWithWritability, const ArrayStorageForm& form)
{
    return equal(modeWithWritability, word32(form.shape));
}

ArrayElements ElementAccessEmitter::elementsOf(CellPointer array)
{
    return { array, loadField<Pointer>(array, m_heaps.JSObject_butterfly) };
}

Integer ElementAccessEmitter::publicLength(const ArrayElements& elements)
{
    return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength));
}

Integer ElementAccessEmitter::vectorLength(const ArrayElements& elements)
{
    return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_vectorLength));
}

void ElementAccessEmitter::setPublicLength(const ArrayElements& elements, Integer length)
{
    storeField(elements.butterfly, m_heaps.Butterfly_publicLength, low32(length));
}

Integer ElementAccessEmitter::arrayLength(CellPointer array)
{
    return publicLength(elementsOf(array));
}

TypedPointer ElementAccessEmitter::elementAt(const ArrayStorageForm& form, const ArrayElements& elements, Integer index)
{
    B3::IndexedAbstractHeap* heap = m_heaps.forIndexingType(form.indexingType);
    RELEASE_ASSERT(heap);
    return m_out.baseIndex(*heap, elements.butterfly.value(), index.value(), constantIndexOf(index));
}

Operand ElementAccessEmitter::loadRawElement(const ArrayStorageForm& form, const ArrayElements& elements, Integer index)
{
    TypedPointer element = elementAt(form, elements, index);
    if (form.element == Representation::Double)
        return Number(m_out.loadDouble(element));
    Boxed rawElement(m_out.load64(element));
    if (form.holdsCells)
        ensureStillAliveHere(elements.array);
    return rawElement;
}

void ElementAccessEmitter::storeRawElement(const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement)
{
    TypedPointer element = elementAt(form, elements, index);
    if (form.element == Representation::Double) {
        m_out.storeDouble(rawElement.as<Number>().value(), element);
        return;
    }
    m_out.store64(rawElement.as<Boxed>().value(), element);
}

bool ElementAccessEmitter::isDense(ValueID array)
{
    return pristineAspectsProvenBy(typeOf(array), BuiltinClasses::Array).contains(PristineAspects::Dense);
}

// Of an element below the length.
Condition ElementAccessEmitter::isHoleIn(ValueID array, const ArrayStorageForm& form, Operand rawElement)
{
    if (isDense(array) && !generator().validates())
        return boolean(false);
    return isHole(form, rawElement);
}

Condition ElementAccessEmitter::isHole(const ArrayStorageForm& form, Operand rawElement)
{
    if (form.element == Representation::Double) {
        Number number = rawElement.as<Number>();
        return notEqualOrUnordered(number, number);
    }
    return isEmpty(rawElement.as<Boxed>());
}

Operand ElementAccessEmitter::rawHole(const ArrayStorageForm& form)
{
    if (form.element == Representation::Double)
        return number(std::bit_cast<double>(form.hole));
    return Boxed(m_out.constInt64(static_cast<int64_t>(form.hole)));
}

Boxed ElementAccessEmitter::boxRawElement(const ArrayStorageForm& form, Operand rawElement)
{
    if (form.element == Representation::Double)
        return emitter<ConversionEmitter>().boxNumber(rawElement.as<Number>());
    return rawElement.as<Boxed>();
}

static bool holdsInteger(Representation representation)
{
    return representation == Representation::Int64 || representation == Representation::OptionalInt64;
}

static bool holdsDouble(Representation representation)
{
    return representation == Representation::Double || representation == Representation::OptionalDouble;
}

static bool holdsUnboxedNumber(Representation representation)
{
    return representation == Representation::Int32 || holdsInteger(representation) || holdsDouble(representation);
}

Operand ElementAccessEmitter::convertInt32Element(Word32 element, Representation representation)
{
    if (holdsInteger(representation))
        return Operand(representation, signExtended(element).value());
    if (holdsDouble(representation))
        return Operand(representation, toNumber(element).value());
    RELEASE_ASSERT(representation == Representation::Int32);
    return element;
}

Operand ElementAccessEmitter::convertRawElement(const ArrayStorageForm& form, Operand rawElement, Representation representation, const Type& result)
{
    if (form.element == Representation::Double && holdsDouble(representation))
        return Operand(representation, rawElement.value);
    if (form.element == Representation::Int32 && holdsUnboxedNumber(representation))
        return convertInt32Element(unboxInt32(rawElement.as<Boxed>()), representation);
    return emitter<ConversionEmitter>().unbox(boxRawElement(form, rawElement), result, representation);
}

Operand ElementAccessEmitter::absentElement(const OperationContext& context, Representation representation)
{
    switch (representation) {
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
        return emitter<ConstantEmitter>().constantOf(jsUndefined(), representation);
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(representation);
}

Integer ElementAccessEmitter::indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex)
{
    Operand operand = get(position);
    switch (operand.representation) {
    case Representation::Int32:
        return signExtended(operand.as<Word32>());
    case Representation::Int64:
        return operand.as<Integer>();
    case Representation::Double: {
        Number number = operand.as<Number>();
        if (std::optional<Integer> exact = emitter<ConversionEmitter>().exactIntegerBehind(number))
            return *exact;
        Integer index = emitter<ConversionEmitter>().toInteger(number);
        // x86-64 truncates NaN and every number outside int64 to INT64_MIN, which is above every length when compared unsigned.
        if (!keyIsIntegral || !isX86_64())
            branchUnless(equal(toNumber(index), number), isNotAnIndex);
        return index;
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed ElementAccessEmitter::boxedPosition(ValueID position)
{
    Operand operand = get(position);
    switch (operand.representation) {
    case Representation::Int32:
        return boxInt32(operand.as<Word32>());
    case Representation::Int64:
        return emitter<ConversionEmitter>().boxInteger(operand.as<Integer>(), typeOf(position).integers);
    case Representation::Double:
        return emitter<ConversionEmitter>().boxNumber(operand.as<Number>());
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ElementAccessEmitter::generateGetArrayLength(const ValueContext& value)
{
    define(value, arrayLength(in<CellPointer>(value->arrayOperand())));
}

// The guard has found the position to be an index.
Integer ElementAccessEmitter::indexInsideLength(ValueID position)
{
    Operand operand = get(position);
    if (operand.representation == Representation::Double)
        return emitter<ConversionEmitter>().toInteger(operand.as<Number>());
    return indexOrElse(position, true, nullptr);
}

Operand ElementAccessEmitter::readElement(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isAbsent)
{
    branchUnless(below(index, publicLength(elements)), isAbsent);
    return readElementInsideLength(value, form, elements, index, isAbsent);
}

Operand ElementAccessEmitter::readElementInsideLength(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, LBasicBlock isHole)
{
    bool isPresent = isDense(value->arrayOperand());
    // The low half of a boxed int32 is the integer.
    if (isPresent && form.element == Representation::Int32 && holdsUnboxedNumber(value.representation()) && !generator().validates())
        return convertInt32Element(Word32(m_out.load32(elementAt(form, elements, index))), value.representation());

    Operand rawElement = loadRawElement(form, elements, index);
    if (isPresent) {
        if (generator().validates())
            emitter<RuntimeValidationEmitter>().checkPrecondition(value, logicalNot(this->isHole(form, rawElement)));
        return convertRawElement(form, rawElement, value.representation(), typeOf(value.id()));
    }

    LBasicBlock holdsValue = newBlock();
    m_out.branch(this->isHole(form, rawElement).value(), rareEdge(isHole), normalEdge(holdsValue));
    m_out.appendTo(holdsValue);
    return convertRawElement(form, rawElement, value.representation(), typeOf(value.id()));
}

void ElementAccessEmitter::generateGetArrayElement(const ValueContext& value)
{
    if (const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage())) {
        readWithKnownStorage(value, *form);
        return;
    }
    readWithAnyStorage(value);
}

void ElementAccessEmitter::readInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    if (generator().validates())
        StorageGuardValidator(generator()).validate(value, array, form.kind);
    ArrayElements elements = elementsOf(array);
    Integer index = indexInsideLength(value->positionOperand());
    if (generator().validates())
        emitter<RuntimeValidationEmitter>().checkPrecondition(value, below(index, publicLength(elements)));
    if (isDense(value->arrayOperand())) {
        define(value, readElementInsideLength(value, form, elements, index, nullptr));
        return;
    }

    LBasicBlock isHole = newBlock();
    OperandJoin join(m_out, value.representation());
    join.add(readElementInsideLength(value, form, elements, index, isHole));
    m_out.appendTo(isHole);
    join.add(absentElement(value, value.representation()));
    define(value, join.finish());
}

void ElementAccessEmitter::readWithKnownStorage(const ValueContext& value, const ArrayStorageForm& form)
{
    if (value->isInBounds()) {
        readInsideLength(value, form);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    Representation representation = value.representation();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock isNotAnIndex = isArrayIndex(typeOf(position)) ? nullptr : newBlock();
    OperandJoin join(m_out, representation);

    if (generator().validates())
        StorageGuardValidator(generator()).validate(value, array, form.kind);
    Integer index = indexOrElse(position, value->keyIsIntegral(), isNotAnIndex);
    join.add(readElement(value, form, elementsOf(array), index, isAbsent));

    m_out.appendTo(isAbsent);
    if (isNotAnIndex)
        branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isNotAnIndex);
    join.add(absentElement(value, representation));

    if (isNotAnIndex) {
        m_out.appendTo(isNotAnIndex);
        join.add(emitter<ConversionEmitter>().unbox(readOutOfLine(value, array), typeOf(value.id()), representation));
    }
    define(value, join.finish());
}

void ElementAccessEmitter::readWithAnyStorage(const ValueContext& value)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Representation representation = value.representation();
    const Type& result = typeOf(value.id());
    if (value.shape() == CodeShape::Compact) {
        define(value, conversions.unbox(readOutOfLine(value, array), result, representation));
        return;
    }

    ValueID position = value->positionOperand();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock outOfLine = newBlock();
    OperandJoin join(m_out, representation);

    Integer index = indexOrElse(position, value->keyIsIntegral(), outOfLine);
    Word32 shape = indexingMode(array, arrayStorageTestMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByShape(*this, m_out, shape, outOfLine, [&](const ArrayStorageForm& form) {
        join.add(readElement(value, form, elements, index, isAbsent));
    });

    m_out.appendTo(isAbsent);
    if (!isArrayIndex(typeOf(position)))
        branchUnless(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), outOfLine);
    join.add(absentElement(value, representation));

    m_out.appendTo(outOfLine);
    join.add(conversions.unbox(readOutOfLine(value, array), result, representation));
    define(value, join.finish());
}

Boxed ElementAccessEmitter::readOutOfLine(const ValueContext& value, CellPointer array)
{
    return emitter<AccessEmitter>().getByVal(value, boxCell(array), boxedPosition(value->positionOperand()));
}

void ElementAccessEmitter::branchOnElementPresence(const ValueContext& value, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, BranchTargets targets)
{
    if (isDense(value->arrayOperand()) && !generator().validates()) {
        branch(below(index, publicLength(elements)), targets);
        return;
    }

    LBasicBlock isInBounds = newBlock();
    branch(below(index, publicLength(elements)), { normalEdge(isInBounds), targets.notTaken });

    m_out.appendTo(isInBounds);
    branch(isHole(form, loadRawElement(form, elements, index)), targets.swapped());
}

void ElementAccessEmitter::generateHasArrayElement(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID position = value->positionOperand();
    const ArrayStorageForm* knownForm = findArrayStorageForm(value->arrayStorage());
    auto hasOutOfLine = [&] {
        return emitter<AccessEmitter>().hasByVal(value, boxCell(array), boxedPosition(position));
    };
    if (!knownForm && value.shape() == CodeShape::Compact) {
        defineTest(value, hasOutOfLine());
        return;
    }

    if (knownForm && generator().validates())
        StorageGuardValidator(generator()).validate(value, array, knownForm->kind);

    // A read of the element on the taken edge repeats these two branches, and B3 folds a branch that an equal one dominates.
    defineTestByBranching(value, [&](BranchTargets targets) {
        bool isAlwaysIndex = isArrayIndex(typeOf(position));
        LBasicBlock outOfLine = knownForm && isAlwaysIndex ? nullptr : newBlock();
        LBasicBlock isAbsent = isAlwaysIndex ? nullptr : newBlock();
        BranchTargets presenceTargets { targets.taken, isAbsent ? normalEdge(isAbsent) : targets.notTaken };

        Integer index = indexOrElse(position, value->keyIsIntegral(), outOfLine);
        ArrayElements elements = elementsOf(array);
        auto branchOnPresence = [&](const ArrayStorageForm& form) {
            branchOnElementPresence(value, form, elements, index, presenceTargets);
        };
        if (knownForm)
            branchOnPresence(*knownForm);
        else
            forEachFormByShape(*this, m_out, indexingMode(array, arrayStorageTestMask), outOfLine, branchOnPresence);

        if (isAbsent) {
            m_out.appendTo(isAbsent);
            branch(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), { targets.notTaken, rareEdge(outOfLine) });
        }
        if (outOfLine) {
            m_out.appendTo(outOfLine);
            branch(hasOutOfLine(), targets);
        }
    });
}

void ElementAccessEmitter::generateGetStringLength(const ValueContext& value)
{
    define(value, emitter<StringEmitter>().lengthOf(value->stringOperand()));
}

void ElementAccessEmitter::generateGetStringCodeUnit(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CharacterView view = strings.viewOf(value, value->stringOperand());
    Word32 position = strings.word32Of(value->positionOperand());
    if (generator().validates())
        emitter<RuntimeValidationEmitter>().checkPrecondition(value, below(position, view.length));
    define(value, strings.codeUnitAt(view, position));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
