/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBarrierEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTSweep.h"
#include "AOTThrowEmitter.h"
#include "AOTVectorOutput.h"
#include "JSType.h"
#include <bit>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

void ArrayIntrinsicEmitter::generateIsPristineArray(const ValueContext& value)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    LBasicBlock isNotObject = newBlock();
    Join<Condition> join(m_out);
    CellPointer object = objectOrElse(value->valueOperand(), isNotObject);
    Condition isArray = hasCellTypeIn(object, { ArrayType, DerivedArrayType });
    join.add(logicalAnd(isArray, elements().isPristineArray(object, { PristineAspects::Storage, PristineAspects::Names })));

    m_out.appendTo(isNotObject);
    join.add(boolean(false));
    defineTest(value, join.finish());
}

void ArrayIntrinsicEmitter::generateArrayStorageKind(const ValueContext& value)
{
    static_assert(int32s.shape + 2 == doubles.shape && doubles.shape + 2 == contiguous.shape && int32s.shape >= 2);
    static_assert(!static_cast<int32_t>(ArrayStorage::Other) && static_cast<int32_t>(ArrayStorage::Int32) == 1 && static_cast<int32_t>(ArrayStorage::Double) == 2 && static_cast<int32_t>(ArrayStorage::Contiguous) == 3);
    Word32 shape = elements().indexingMode(in<CellPointer>(value->arrayOperand()), arrayStorageTestMask);
    Word32 number = shiftRightUnsigned(sub(shape, int32s.shape - 2), 1);
    define(value, select(belowOrEqual(number, static_cast<int32_t>(ArrayStorage::Contiguous)), number, word32(static_cast<int32_t>(ArrayStorage::Other))));
}

void ArrayIntrinsicEmitter::generateGetDoubleIndex(const ValueContext& value)
{
    static_assert(doubles.element == Representation::Double);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    LBasicBlock otherwise = newBlock();
    Join<Number> join(m_out);
    branchUnless(elements().hasShape(elements().indexingMode(array, arrayStorageTestMask), doubles), otherwise);
    ArrayElements storage = elements().elementsOf(value, array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    join.add(elements().loadRawElement(doubles, storage, index).as<Number>());

    m_out.appendTo(otherwise);
    join.add(elements().rawHole(doubles).as<Number>());
    define(value, join.finish());
}

Boxed ArrayIntrinsicEmitter::contiguousElementOrEmpty(const ValueContext& access, CellPointer array, Integer index)
{
    static_assert(int32s.hole == contiguous.hole && int32s.element != Representation::Double && contiguous.element == Representation::JSValue);
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    branchUnless(elements().holdsBoxedElements(array), otherwise);
    ArrayElements storage = elements().elementsOf(access, array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, index)));
    ensureStillAliveHere(array);
    join.add(element);

    m_out.appendTo(otherwise);
    join.add(elements().rawHole(contiguous).as<Boxed>());
    return join.finish();
}

bool ArrayIntrinsicEmitter::standsBehindTestThatFoundElement(const ValueContext& user, ValueID array, ValueID position)
{
    if (!generator().testThatFoundElement)
        return false;
    auto behindNames = [&](ValueID id) {
        while (operationAt(id).opcode == Opcode::Narrowed)
            id = operationAt(id).valueOperand();
        return id;
    };
    const Operation& test = operationAt(generator().testThatFoundElement);
    if ((array && behindNames(test.arrayOperand()) != behindNames(array)) || behindNames(test.positionOperand()) != behindNames(position))
        return false;

    BlockID asked = m_code.blockOf(generator().testThatFoundElement);
    for (BlockID block = user.block();;) {
        for (ValueID id : m_code.valuesOf(block)) {
            if (id == user.id() || !array)
                break;
            if (operationAt(id).opcode != Opcode::Narrowed)
                return false;
        }
        BlockID above = generator().singlePredecessorOf(block);
        if (!above)
            return false;
        const Terminator& end = m_code.at(above).terminator;
        if (above == asked)
            return end.isConditionalBranch() && end.conditionOperand() == generator().testThatFoundElement && m_code.at(end.taken()).to == block;
        if (array && end.kind != TerminatorKind::Jump)
            return false;
        block = above;
    }
}

void ArrayIntrinsicEmitter::generateHasContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    if (value.isBranchCondition()) {
        const BranchTargets& targets = value.branchTargets();
        Integer index = integerOperand(value->positionOperand());
        auto goOnIf = [&](Condition condition) {
            LBasicBlock next = newBlock();
            branch(condition, { normalEdge(next), targets.notTaken });
            m_out.appendToContinuation(next);
        };
        if (!value->storageOperand())
            goOnIf(elements().holdsBoxedElements(array));
        else if (elements().checksBounds())
            elements().stopUnlessInside(value, elements().holdsBoxedElements(elements().askedAnew(array)));
        ArrayElements storage = elements().elementsOf(value, array);
        if (!value->isInBounds())
            goOnIf(below(index, elements().publicLength(storage)));
        else if (elements().checksBounds())
            elements().stopUnlessInside(value, below(index, elements().publicLength(storage)));
        Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, index)));
        ensureStillAliveHere(array);
        generator().testThatFoundElement = value.id();
        generator().elementFound = element.value();
        branch(isNotEmpty(element), targets);
        return;
    }
    Boxed element = contiguousElementOrEmpty(value, array, integerOperand(value->positionOperand()));
    define(value, isNotEmpty(element));
}

void ArrayIntrinsicEmitter::generateGetContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Boxed element = contiguousElementOrEmpty(value, array, integerOperand(value->positionOperand()));
    Condition isFilled = isNotEmpty(element);
    defineAs(value, select(isFilled, element, undefined()), typeOf(value.id()));
}

Integer ArrayIntrinsicEmitter::positionThatLibraryVouchesFor(const ValueContext& value, const ArrayElements& storage, const ArrayStorageForm& form, AccessVouchedFor access)
{
    Operand position = get(value->positionOperand());
    Integer index = position.representation == Representation::Int32 ? Integer(m_out.extendWordThatIsNotNegative(position.as<Word32>().value())) : integerOperand(value->positionOperand());
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::MisplaceUncheckedAccesses) || (access == AccessVouchedFor::Reads && AOT::hasFlag(build().options().testing, Wire::TestingSwitch::MisplaceUncheckedReads))) [[unlikely]] {
        Integer further = add(index, integer(1));
        index = select(below(further, elements().vectorLength(storage)), further, index);
    }
    if (elements().checksBounds()) {
        bool writes = access == AccessVouchedFor::Writes;
        Word32 mode = elements().indexingMode(elements().askedAnew(storage.array), writes ? IndexingShapeAndWritabilityMask : arrayStorageTestMask);
        Condition hasKind = writes ? elements().hasWritableShape(mode, form) : elements().hasShape(mode, form);
        if (form.holdsCells)
            hasKind = writes ? isOneOfBoxedForms(mode, int32s.indexingType, contiguous.indexingType) : isOneOfBoxedForms(mode, int32s.shape, contiguous.shape);
        elements().stopUnlessInside(value, hasKind);
        elements().stopUnlessInside(value, below(index, elements().publicLength(storage)));
    } else
        build().statistics().count(StatisticID::RangeChecksRemoved);
    return index;
}

Integer ArrayIntrinsicEmitter::countThatLibraryVouchesFor(const ValueContext& value, Integer count, const ArrayElements& asked, const ArrayElements& vouchedFor, AccessVouchedFor access)
{
    bool shouldOverstateCount = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::MisplaceUncheckedAccesses);
    if (shouldOverstateCount) [[unlikely]] {
        Integer further = add(count, integer(1));
        count = select(logicalAnd(belowOrEqual(further, elements().vectorLength(asked)), belowOrEqual(further, elements().vectorLength(vouchedFor))), further, count);
    }
    if (elements().checksBounds())
    {
        bool writes = access == AccessVouchedFor::Writes;
        Word32 mode = elements().indexingMode(elements().askedAnew(vouchedFor.array), writes ? IndexingShapeAndWritabilityMask : arrayStorageTestMask);
        elements().stopUnlessInside(value, writes ? elements().hasWritableShape(mode, contiguous) : elements().hasShape(mode, contiguous));
        elements().stopUnlessInside(value, belowOrEqual(count, elements().publicLength(vouchedFor)));
    }
    return count;
}

void ArrayIntrinsicEmitter::generateGetOwnContiguousIndex(const ValueContext& value)
{
    if (standsBehindTestThatFoundElement(value, value->arrayOperand(), value->positionOperand())) {
        defineAs(value, Boxed(generator().elementFound), typeOf(value.id()));
        return;
    }
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    Boxed element(m_out.load64(elements().elementOfUnknownForm(storage, positionThatLibraryVouchesFor(value, storage, contiguous, AccessVouchedFor::Reads))));
    if (elements().checksBounds())
        elements().stopUnlessInside(value, isNotEmpty(element));
    ensureStillAliveHere(array);
    defineAs(value, element, typeOf(value.id()));
}

void ArrayIntrinsicEmitter::generateGetOwnDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    define(value, elements().loadRawElement(doubles, storage, positionThatLibraryVouchesFor(value, storage, doubles, AccessVouchedFor::Reads)).as<Number>());
}

void ArrayIntrinsicEmitter::generateGetOwnInt32Index(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    TypedPointer place = elements().elementOfUnknownForm(storage, positionThatLibraryVouchesFor(value, storage, int32s, AccessVouchedFor::Reads));
    if (elements().checksBounds())
        elements().stopUnlessInside(value, isInt32(Boxed(m_out.load64(place))));
    define(value, Word32(m_out.load32(place)));
}

void ArrayIntrinsicEmitter::generatePutOwnContiguousIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    Boxed rawElement = in<Boxed>(stored);
    ArrayElements storage = elements().elementsOf(value, array);
    Integer position = positionThatLibraryVouchesFor(value, storage, contiguous, AccessVouchedFor::Writes);
    if (elements().checksBounds())
        elements().stopUnlessInside(value, logicalOr(elements().hasWritableShape(elements().indexingMode(elements().askedAnew(storage.array), IndexingShapeAndWritabilityMask), contiguous), isInt32(rawElement)));
    m_out.store64(rawElement.value(), elements().elementOfUnknownForm(storage, position));
    barrierAfterStore(value, array, typeOf(stored));
}

void ArrayIntrinsicEmitter::generatePutOwnDoubleIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    Number number = in<Number>(value->valueOperand());
    Integer position = positionThatLibraryVouchesFor(value, storage, doubles, AccessVouchedFor::Writes);
    if (!value->undoesIterationStore())
        elements().stopWhereHoleWouldBeStored(value, doubles, number);
    elements().storeRawElement(doubles, storage, position, number);
}

void ArrayIntrinsicEmitter::generatePutOwnInt32Index(const ValueContext& value)
{
    static_assert(std::endian::native == std::endian::little);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements storage = elements().elementsOf(value, array);
    Integer index = positionThatLibraryVouchesFor(value, storage, int32s, AccessVouchedFor::Writes);
    TypedPointer place = elements().elementOfUnknownForm(storage, index);
    if (elements().checksBounds())
        elements().stopUnlessInside(value, isInt32(Boxed(m_out.load64(place))));
    m_out.store32(low32(integerOperand(value->valueOperand())).value(), place);
}

void ArrayIntrinsicEmitter::putInBounds(const ValueContext& value, const ArrayStorageForm& form)
{
    RELEASE_ASSERT(form.kind != ArrayStorage::Int32);
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    Operand rawElement = generator().getIn(stored, form.element);
    ArrayElements storage = elements().elementsOf(value, array);
    Integer position = positionThatLibraryVouchesFor(value, storage, form, AccessVouchedFor::Writes);
    if (!form.holdsCells) {
        elements().stopWhereHoleWouldBeStored(value, form, rawElement);
        elements().storeRawElement(form, storage, position, rawElement);
        return;
    }
    if (!elements().boxIsMadeInFormOfType(stored) && !AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipInt32Reboxing))
        rawElement = emitter<ConversionEmitter>().inFormOfType(rawElement, typeOf(stored));
    if (elements().checksBounds())
        elements().stopUnlessInside(value, logicalOr(isInt32(rawElement.as<Boxed>()), elements().hasArrayStorage(elements().askedAnew(array), ArrayStorage::Contiguous)));
    elements().storeWordOfOperand(value, stored, rawElement.as<Boxed>(), storage, position);
    barrierAfterStore(value, array, typeOf(stored));
}

void ArrayIntrinsicEmitter::putNumberInBounds(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    if (form.holdsCells && representationOf(stored) == Representation::JSValue) {
        putInBounds(value, form);
        return;
    }
    ArrayElements storage = elements().elementsOf(value, array);
    if (!form.holdsCells && representationOf(stored) == Representation::Double) {
        Operand number = get(stored);
        elements().stopWhereHoleWouldBeStored(value, form, number);
        elements().storeRawElement(form, storage, positionThatLibraryVouchesFor(value, storage, form, AccessVouchedFor::Writes), number);
        return;
    }
    LBasicBlock doesNotFit = newBlock();
    std::optional<Operand> rawElement = elements().rawElementToStore(form.holdsCells && typeOf(stored).isInt32() ? int32s : form, stored, doesNotFit);
    if (!rawElement) {
        fail(value, InternalErrorKind::UnsupportedConversion);
        return;
    }
    elements().storeRawElement(form, storage, positionThatLibraryVouchesFor(value, storage, form, AccessVouchedFor::Writes), *rawElement);
    if (!doesNotFit->numberOfIncomingEdges())
        return;
    LBasicBlock goesOn = newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.jump(goesOn);
    m_out.appendTo(doesNotFit);
    emitter<ThrowEmitter>().crashOnFalseProof(value, std::nullopt, ProofSource::OtherOperation);
    m_out.appendToArm(goesOn, fork);
}

void ArrayIntrinsicEmitter::generatePutContiguousIndex(const ValueContext& value)
{
    if (value->hasNumericStorage() && elements().checksBounds())
        elements().stopUnlessInside(value, elements().hasWritableNumericStorage(elements().askedAnew(in<CellPointer>(value->arrayOperand()))));
    if (!value->goesByKindOfStorage()) {
        putInBounds(value, contiguous);
        return;
    }
    if (std::optional<ArrayStorage> found = elements().kindOfListFoundInFrontOfTurns(value, value->arrayOperand(), true)) {
        putNumberInBounds(value, *found == ArrayStorage::Double ? doubles : contiguous);
        return;
    }
    CellPointer array = in<CellPointer>(value->arrayOperand());
    LBasicBlock holdsDoubles = newBlock();
    LBasicBlock holdsBoxes = newBlock();
    LBasicBlock done = newBlock();
    ScopeFork fork = m_out.forkScope();
    branch(elements().hasWritableShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), doubles), { normalEdge(holdsDoubles), normalEdge(holdsBoxes) });
    m_out.appendToArm(holdsDoubles, fork);
    putNumberInBounds(value, doubles);
    m_out.jump(done);
    m_out.appendToArm(holdsBoxes, fork);
    putNumberInBounds(value, contiguous);
    m_out.jump(done);
    m_out.appendToJoin(done, fork);
}

const ArrayStorageForm& ArrayIntrinsicEmitter::formToConstruct(const ValueContext& value) const
{
    const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage());
    return form ? *form : int32s;
}

const ArrayStorageForm* ArrayIntrinsicEmitter::findFormOfListFilled(const ValueContext& value) const
{
    const Operation& born = m_code.at(underlyingCellOf(m_code, value->arrayOperand()));
    if (born.opcode == Opcode::NewArrayLike && hasArrayStorageForm(born.arrayStorage()))
        return findArrayStorageForm(born.arrayStorage());
    return value->opcode == Opcode::TryPutByValDirect ? findArrayStorageForm(value->arrayStorage()) : nullptr;
}

bool ArrayIntrinsicEmitter::isMadeLikeSource(const ValueContext& value) const
{
    return value->sourceOperand() && !typeOf(value->sourceOperand()).isUndefinedOrNull() && !findArrayStorageForm(value->arrayStorage());
}

CellPointer ArrayIntrinsicEmitter::sourceOrNull(ValueID id)
{
    CellPointer null(m_out.int64Zero);
    if (!id || typeOf(id).isUndefinedOrNull())
        return null;
    Operand operand = get(id);
    switch (operand.representation) {
    case Representation::Cell:
        return operand.as<CellPointer>();
    case Representation::Cell32: {
        NarrowReference reference = operand.as<NarrowReference>();
        Condition isCell = isCellReference(reference);
        return select(isCell, decode(reference), null);
    }
    case Representation::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        return select(isCell(boxed), unboxCell(boxed), null);
    }
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return null;
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ArrayIntrinsicEmitter::newArrayOfHoles(const ValueContext& value, Integer length, ValueID source, const ArrayStorageForm& form, uint32_t flags)
{
    using ElementsBelowLength = ArrayAllocationEmitter::ElementsBelowLength;
    using LengthIsSetBy = ArrayAllocationEmitter::LengthIsSetBy;
    bool remembersAtSite = value->opcode == Opcode::NewArrayLike && value->remembersStorageAtSite() && !form.holdsCells && modulesAndPositionsOf(value);
    bool isWrittenEmpty = length.value()->hasInt64() && !length.value()->asInt64();
    if (!isWrittenEmpty && !remembersAtSite && !flags && (!source || typeOf(source).isUndefinedOrNull())) {
        define(value, ArrayAllocationEmitter(generator()).bearArrayOfLength(value, form, length));
        return;
    }
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    ArrayAllocationEmitter allocation(generator());
    std::optional<Pointer> profile;
    if (value->opcode == Opcode::NewArrayLike && value->remembersStorageAtSite() && !flags)
        profile = allocation.profileOfListsOfLengthAtSite(value.origin(), form);
    bool takesAnyValueAtFirst = profile && value->takesAnyValueAtFirst() && !hasArrayStorageForm(value->arrayStorage());
    if (takesAnyValueAtFirst) {
        static_assert(ArrayAllocationProfile::shapeMask == 0xffu << ArrayAllocationProfile::shapeShift && !(ArrayAllocationProfile::shapeShift % 8));
        branchUnless(Condition(m_out.notZero32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_arrayProfiles, profile->value(), OBJECT_OFFSETOF(ArrayAllocationProfile, word) + ArrayAllocationProfile::shapeShift / 8)))), slowPath);
    }
    join.add(profile ? allocation.allocateArrayOfHolesAtSite(value, length, form, *profile, slowPath) : allocation.allocateArrayOfHoles(value, length, form, slowPath, ElementsBelowLength::Cleared, LengthIsSetBy::Caller));

    m_out.appendTo(slowPath);
    if (profile)
        join.add(callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHolesAtSite, { *profile, sourceOrNull(source), length, word32(takesAnyValueAtFirst ? ArrayWithUndecided : form.indexingType) }, { .isOnRarePath = true }));
    else
        join.add(callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHoles, { sourceOrNull(source), length, word32(form.indexingType), word32(flags) }, { .isOnRarePath = true }));
    CellPointer array = join.finish();
    allocation.setLengthOfNewArray(array, length);
    define(value, array);
}

void ArrayIntrinsicEmitter::newArrayOfForm(const ValueContext& value, const ArrayStorageForm& form)
{
    newArrayOfHoles(value, integerOperand(value->lengthOperand()), ValueID(), form, NewArrayWithHolesFlag::mustHaveIndexingType);
}

void ArrayIntrinsicEmitter::generateNewArrayLike(const ValueContext& value)
{
    const Type& length = typeOf(value->lengthOperand());
    bool isEmpty = length.isInteger() && length.integers == Range::one(0);
    Integer count = isEmpty ? integer(0) : integerOperand(value->lengthOperand());
    if (!isMadeLikeSource(value)) {
        newArrayOfHoles(value, count, value->sourceOperand(), formToConstruct(value), 0);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock isMadeOfNoList = newBlock();
    Join<CellPointer> join(m_out);
    ArrayAllocationEmitter allocation(generator());
    CellPointer source = sourceOrNull(value->sourceOperand());
    branchUnless(Condition(m_out.notNull(source.value())), isMadeOfNoList);
    branchUnless(hasCellType(source, ArrayType), isMadeOfNoList);
    std::optional<Integer> roomOfEmptyList;
    std::optional<Pointer> profile;
    if (isEmpty && value->capacityOperand()) {
        roomOfEmptyList = integerOperand(value->capacityOperand());
        profile = allocation.emptyArraySiteProfile(value.origin());
    }
    allocation.allocateListLike(value, source, count, roomOfEmptyList, profile, isMadeOfNoList, scopedLambda<void(CellPointer)>([&](CellPointer list) {
        join.add(list);
    }));

    m_out.appendTo(isMadeOfNoList);
    join.add(allocation.allocateArrayOfHoles(value, count, contiguous, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewArrayWithHoles, { source, count, word32(contiguous.indexingType), word32(0) }, { .isOnRarePath = true }));
    CellPointer list = join.finish();
    if (profile)
        allocation.countEmptyArrayAtSite(value, *profile, list);
    define(value, list);
}

void ArrayIntrinsicEmitter::generateTryPutByValDirect(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    ValueID stored = value->valueOperand();
    if (const Operation& born = m_code.at(underlyingCellOf(m_code, value->arrayOperand())); !hasArrayStorageForm(value->arrayStorage()) && born.opcode == Opcode::NewArrayLike && born.remembersStorageAtSite()) {
        const ArrayStorageForm* seed = findArrayStorageForm(born.arrayStorage());
        define(value, elements().tryStoreElement(value, array, index, stored, true, ArrayAllocationEmitter(generator()).profileOfListsOfLengthAtSite(born.origin(), seed ? *seed : int32s)));
        return;
    }
    const ArrayStorageForm* formOfList = findFormOfListFilled(value);
    const ArrayStorageForm& form = formOfList ? *formOfList : contiguous;
    LBasicBlock otherwise = newBlock();
    if (&form == &int32s && typeOf(stored).isNumber() && !typeOf(stored).isInt32()) {
        LBasicBlock holdsIntegers = newBlock();
        LBasicBlock holdsNoIntegers = newBlock();
        Join<Condition> isStored(m_out);
        Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
        branch(elements().hasWritableShape(mode, int32s), { normalEdge(holdsIntegers), normalEdge(holdsNoIntegers) });

        auto storeInForm = [&](const ArrayStorageForm& held) {
            ArrayElements found = elements().elementsOf(array);
            branchUnless(below(index, elements().publicLength(found)), otherwise);
            if (std::optional<Operand> element = elements().rawElementToStore(held, stored, otherwise)) {
                elements().storeRawElement(held, found, index, *element);
                isStored.add(boolean(true));
            } else
                m_out.jump(otherwise);
        };
        m_out.appendTo(holdsIntegers);
        storeInForm(int32s);

        m_out.appendTo(holdsNoIntegers);
        branchUnless(elements().hasWritableShape(mode, doubles), otherwise);
        storeInForm(doubles);

        m_out.appendTo(otherwise);
        isStored.add(boolean(false));
        define(value, isStored.finish());
        return;
    }
    if (!formOfList && get(stored).is<Boxed>() && (typeOf(stored).primitives & Primitive::Integer) && !typeOf(stored).isNumber()) {
        LBasicBlock holdsAnything = newBlock();
        LBasicBlock holdsNoCells = newBlock();
        Join<Condition> isStored(m_out);
        Boxed word = get(stored).as<Boxed>();
        Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
        branch(elements().hasWritableShape(mode, contiguous), { normalEdge(holdsAnything), normalEdge(holdsNoCells) });

        m_out.appendTo(holdsAnything);
        {
            ArrayElements found = elements().elementsOf(array);
            branchUnless(below(index, elements().publicLength(found)), otherwise);
            elements().storeRawElement(contiguous, found, index, Operand(word));
            barrierAfterStore(value, array, typeOf(stored));
            isStored.add(boolean(true));
        }

        m_out.appendTo(holdsNoCells);
        {
            branchUnless(elements().hasWritableShape(mode, int32s), otherwise);
            branchUnless(isInt32(word), otherwise);
            ArrayElements found = elements().elementsOf(array);
            branchUnless(below(index, elements().publicLength(found)), otherwise);
            elements().storeRawElement(int32s, found, index, Operand(word));
            isStored.add(boolean(true));
        }

        m_out.appendTo(otherwise);
        isStored.add(boolean(false));
        define(value, isStored.finish());
        return;
    }
    std::optional<Operand> rawElement = elements().rawElementToStore(form, stored, otherwise);
    if (!rawElement) {
        m_out.jump(otherwise);
        m_out.appendTo(otherwise);
        define(value, boolean(false));
        return;
    }

    Join<Condition> join(m_out);
    branchUnless(elements().hasWritableShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), form), otherwise);
    ArrayElements storage = elements().elementsOf(array);
    branchUnless(below(index, elements().publicLength(storage)), otherwise);
    elements().storeRawElement(form, storage, index, *rawElement);
    if (form.holdsCells)
        barrierAfterStore(value, array, typeOf(stored));
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    define(value, join.finish());
}

void ArrayIntrinsicEmitter::generateTryPutIndex(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer index = integerOperand(value->positionOperand());
    define(value, elements().tryStoreElement(value, array, index, value->valueOperand()));
}

template<typename Functor>
void ArrayIntrinsicEmitter::forEachPositionBelow(Integer count, const Functor& functor)
{
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock done = newBlock();
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(Machine::Int64, start));
    branch(below(position, count), { normalEdge(element), normalEdge(done) });

    m_out.appendTo(element);
    functor(position);
    m_out.addIncomingToPhi(position.value(), m_out.anchor(add(position, integer(1)).value()));
    m_out.jump(loop);

    m_out.appendTo(done);
}

void ArrayIntrinsicEmitter::fillElements(const ArrayStorageForm& form, const ArrayElements& storage, Integer length, Operand filler)
{
    constexpr int elementsOfVector = 2;
    constexpr int logElementsInIteration = 3;
    constexpr int elementsInIteration = 1 << logElementsInIteration;
    generator().procedure().noteVectors();
    VectorOutput vectors(m_out);
    LValue word = form.element == Representation::Double ? nullptr : filler.as<Boxed>().value();
    LValue fillers = word ? vectors.joinWords(word, word) : vectors.splat(filler.as<Number>().value());
    forEachPositionBelow(Integer(m_out.lShr(length.value(), logElementsInIteration)), [&](Integer iteration) {
        TypedPointer first = m_out.baseIndex(*m_heaps.forIndexingType(form.indexingType), storage.butterfly.value(), m_out.shl(iteration.value(), logElementsInIteration));
        for (int element = 0; element < elementsInIteration; element += elementsOfVector)
            m_out.store(fillers, TypedPointer(*first.heap(), first.base(), first.offset() + element * static_cast<int>(sizeof(EncodedJSValue))));
    });
    LValue inVectors = m_out.bitAnd(length.value(), -int64_t { elementsInIteration });
    forEachPositionBelow(Integer(m_out.sub(length.value(), inVectors)), [&](Integer position) {
        elements().storeRawElement(form, storage, Integer(m_out.add(inVectors, position.value())), filler);
    });
}

Condition ArrayIntrinsicEmitter::isOneOfBoxedForms(Word32 mode, IndexingType ofIntegers, IndexingType ofValues)
{
    IndexingType distance = ofValues - ofIntegers;
    RELEASE_ASSERT(ofValues > ofIntegers && hasOneBitSet(distance));
    return Condition(m_out.testIsZero32(m_out.sub(mode.value(), ofIntegers), ~static_cast<int32_t>(distance)));
}

// No barrier: nothing has run since `array` was allocated, so the collector has not marked it. The barrier would have fenced: so a fence keeps these stores in front of the store that hands `array` on.
void ArrayIntrinsicEmitter::generateTryCopyFilledElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    Integer count = integerOperand(value->countOperand());
    LBasicBlock holdsBoxedValues = newBlock();
    LBasicBlock holdsNoBoxedValues = newBlock();
    LBasicBlock copied = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 shape = elements().indexingMode(source, arrayStorageTestMask);
    branch(isOneOfBoxedForms(shape, int32s.shape, contiguous.shape), { normalEdge(holdsBoxedValues), normalEdge(holdsNoBoxedValues) });

    m_out.appendTo(holdsBoxedValues);
    {
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(from)), otherwise);
        forEachPositionBelow(countThatLibraryVouchesFor(value, count, from, to, AccessVouchedFor::Writes), [&](Integer position) {
            LBasicBlock mayBeGap = newBlock();
            LBasicBlock isValue = newBlock();
            Boxed word(m_out.load64(elements().elementOfUnknownForm(from, position)));
            static_assert(!contiguous.hole && JSValue::ValueUndefined == (JSValue::ValueNull | JSValue::UndefinedTag));
            m_out.branch(m_out.testIsZero64(word.value(), ~static_cast<int64_t>(JSValue::ValueUndefined)), rareEdge(mayBeGap), normalEdge(isValue));

            m_out.appendTo(mayBeGap);
            m_out.branch(m_out.equal(word.value(), m_out.constInt64(JSValue::ValueNull)), normalEdge(isValue), normalEdge(otherwise));

            m_out.appendTo(isValue);
            m_out.store64(word.value(), elements().elementOfUnknownForm(to, position));
        });
        m_out.jump(copied);
    }

    m_out.appendTo(holdsNoBoxedValues);
    {
        branchUnless(elements().hasShape(shape, doubles), otherwise);
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(from)), otherwise);
        forEachPositionBelow(countThatLibraryVouchesFor(value, count, from, to, AccessVouchedFor::Writes), [&](Integer position) {
            Number number = elements().loadRawElement(doubles, from, position).as<Number>();
            branchUnless(equal(number, number), otherwise);
            m_out.store64(elements().boxRawElement(doubles, number).value(), elements().elementOfUnknownForm(to, position));
        });
        m_out.jump(copied);
    }

    m_out.appendTo(copied);
    ensureStillAliveHere(source);
    if (generator().keepsValuesInHeapFrame()) [[unlikely]]
        emitter<BarrierEmitter>().barrier(value, array);
    else
        emitter<BarrierEmitter>().fenceAfterInitialization();
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    elements().setPublicLength(elements().elementsOf(array), integer(0));
    join.add(boolean(false));
    define(value, join.finish());
}

void ArrayIntrinsicEmitter::generateTryPutElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    Integer count = integerOperand(value->countOperand());
    LBasicBlock holdsBoxedValues = newBlock();
    LBasicBlock holdsNoBoxedValues = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    branch(isOneOfBoxedForms(mode, int32s.indexingType, contiguous.indexingType), { normalEdge(holdsBoxedValues), normalEdge(holdsNoBoxedValues) });

    m_out.appendTo(holdsBoxedValues);
    {
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(to)), otherwise);
        forEachPositionBelow(countThatLibraryVouchesFor(value, count, to, from, AccessVouchedFor::Reads), [&](Integer position) {
            Boxed word(m_out.load64(elements().elementOfUnknownForm(from, position)));
            if (elements().checksBounds()) {
                elements().stopUnlessInside(value, isNotEmpty(word));
                elements().stopUnlessInside(value, logicalOr(elements().hasWritableShape(mode, contiguous), isInt32(word)));
            }
            m_out.store64(word.value(), elements().elementOfUnknownForm(to, position));
        });
        ensureStillAliveHere(source);
        emitter<BarrierEmitter>().barrier(value, array);
        join.add(boolean(true));
    }

    m_out.appendTo(holdsNoBoxedValues);
    {
        branchUnless(elements().hasWritableShape(mode, doubles), otherwise);
        ArrayElements from = elements().elementsOf(source);
        ArrayElements to = elements().elementsOf(array);
        branchUnless(belowOrEqual(count, elements().publicLength(to)), otherwise);
        forEachPositionBelow(countThatLibraryVouchesFor(value, count, to, from, AccessVouchedFor::Reads), [&](Integer position) {
            Boxed word(m_out.load64(elements().elementOfUnknownForm(from, position)));
            branchUnless(isNumber(word), otherwise);
            Number number = select(isInt32(word), toNumber(unboxInt32(word)), unboxDouble(word));
            elements().storeRawElement(doubles, to, position, number);
        });
        ensureStillAliveHere(source);
        join.add(boolean(true));
    }

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    define(value, join.finish());
}

// No barrier: nothing has run since `array` was allocated, so the collector has not marked it. The barrier would have fenced: so a fence keeps these stores in front of the store that hands `array` on.
void ArrayIntrinsicEmitter::generateTryFillNewArray(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    LBasicBlock otherwise = newBlock();
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    auto fill = [&](const ArrayStorageForm& form, Operand filler) {
        ArrayElements storage = elements().elementsOf(array);
        fillElements(form, storage, elements().publicLength(storage), filler);
        if (generator().keepsValuesInHeapFrame()) [[unlikely]]
            emitter<BarrierEmitter>().barrier(value, array);
        else
            emitter<BarrierEmitter>().fenceAfterInitialization();
    };

    if (const ArrayStorageForm* form = findFormOfListFilled(value)) {
        std::optional<Operand> filler = elements().rawElementToStore(*form, value->valueOperand(), otherwise);
        if (!filler) {
            m_out.jump(otherwise);
            m_out.appendTo(otherwise);
            define(value, boolean(false));
            return;
        }
        Join<Condition> join(m_out);
        branchUnless(elements().hasWritableShape(mode, *form), otherwise);
        fill(*form, *filler);
        join.add(boolean(true));

        m_out.appendTo(otherwise);
        join.add(boolean(false));
        define(value, join.finish());
        return;
    }

    Boxed filler = in<Boxed>(value->valueOperand());
    LBasicBlock takesFiller = newBlock();
    LBasicBlock mayHoldIntegers = newBlock();
    LBasicBlock isNoInteger = newBlock();
    LBasicBlock isFraction = newBlock();
    LBasicBlock isAnyValue = newBlock();
    Join<Condition> join(m_out);
    branch(elements().hasWritableShape(mode, contiguous), { normalEdge(takesFiller), normalEdge(mayHoldIntegers) });

    m_out.appendTo(mayHoldIntegers);
    branchUnless(elements().hasWritableShape(mode, int32s), otherwise);
    branch(isInt32(filler), { normalEdge(takesFiller), normalEdge(isNoInteger) });

    m_out.appendTo(isNoInteger);
    branch(isNumber(filler), { normalEdge(isFraction), normalEdge(isAnyValue) });

    m_out.appendTo(isFraction);
    Number fraction = unboxDouble(filler);
    branchUnless(equal(fraction, fraction), isAnyValue);
    elements().widenStorageOfIntegers(array, doubles, otherwise);
    {
        ArrayElements storage = elements().elementsOf(array);
        Integer length = elements().publicLength(storage);
        fillElements(doubles, storage, length, fraction);
        forEachPositionBelow(sub(elements().vectorLength(storage), length), [&](Integer position) {
            elements().storeRawElement(doubles, storage, add(length, position), elements().rawHole(doubles));
        });
    }
    join.add(boolean(true));

    m_out.appendTo(isAnyValue);
    elements().widenStorageOfIntegers(array, contiguous, otherwise);
    m_out.jump(takesFiller);

    m_out.appendTo(takesFiller);
    fill(contiguous, filler);
    join.add(boolean(true));

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    define(value, join.finish());
}

void ArrayIntrinsicEmitter::generateSortNumbers(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer direction = integerOperand(value->directionOperand());
    define(value, callOperation<Condition>(value, RuntimeOperation::SortNumbers, { array, low32(direction) }));
}

void ArrayIntrinsicEmitter::generateSortKeyArray(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::SortKeyArray, { integerOperand(value->lengthOperand()) }));
}

void ArrayIntrinsicEmitter::generateSortByKeys(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer keys = in<CellPointer>(value->keysOperand());
    Integer direction = integerOperand(value->directionOperand());
    define(value, callOperation<Condition>(value, RuntimeOperation::SortByKeys, { array, keys, low32(direction) }));
}

void ArrayIntrinsicEmitter::generateTruncateNewArray(const ValueContext& value)
{
    static constexpr int64_t smallUnusedCapacity = 8;
    CellPointer array = in<CellPointer>(value->arrayOperand());
    Integer length = integerOperand(value->lengthOperand());
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    static_assert(ArrayWithInt32 + 2 == ArrayWithDouble && ArrayWithDouble + 2 == ArrayWithContiguous, "an array has no mode between these");
    branchUnless(belowOrEqual(sub(elements().indexingMode(array, IndexingShapeAndWritabilityMask), ArrayWithInt32), ArrayWithContiguous - ArrayWithInt32), slowPath);
    ArrayElements storage = elements().elementsOf(array);
    Integer unusedCapacity = sub(elements().publicLength(storage), length);
    Condition leavesLittleUnused = belowOrEqual(unusedCapacity, smallUnusedCapacity);
    branchUnless(logicalOr(leavesLittleUnused, belowOrEqual(unusedCapacity, length)), slowPath);
    elements().setPublicLength(storage, length);
    join.add(array);

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::TruncateNewArray, { array, low32(length) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
