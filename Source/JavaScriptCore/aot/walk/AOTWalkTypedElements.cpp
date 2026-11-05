/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkNumberGuesses.h"
#include "TypedArrayType.h"

namespace JSC { namespace AOT { namespace Typed {

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

static constexpr OpcodeSet opcodesOfTypedElements { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::PropGet };

static constexpr Range arrayIndexesOfTypedElements { 0, static_cast<int64_t>(UINT32_MAX) - 1 };

namespace {

enum class KindOfBaseOfTypedElements : uint8_t { Other, Array, TypedArray, String, Arguments };

struct BaseOfTypedElements {
    KindOfBaseOfTypedElements kind { KindOfBaseOfTypedElements::Other };
    TypedArrayType typedArrayType { NotTypedArray };
};

struct AccessOfTypedElements {
    Walker& walker;
    const BodyValue& value;
    Reference base;
    Reference key;
    Reference stored;
    TypeID typeOfBase;
    Type typeOfKey;
    TypeID typeOfStored;
    bool isOnlyAskedAbout { false };
};

} // anonymous namespace

static BuiltinTypeContext builtinContextOfTypedElements(Walker& walker)
{
    return { walker.program(), walker.types() };
}

static BaseOfTypedElements classifyBaseOfTypedElements(Walker& walker, TypeID id)
{
    Type type = walker.types().at(id);
    if (type.isAnyString())
        return { KindOfBaseOfTypedElements::String, NotTypedArray };
    if (!type.isObject())
        return { };
    const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), type);
    if (onlyClass && onlyClass->builtinClass == BuiltinClasses::Array)
        return { KindOfBaseOfTypedElements::Array, NotTypedArray };
    if (onlyClass && onlyClass->builtinClass == BuiltinClasses::Arguments)
        return { KindOfBaseOfTypedElements::Arguments, NotTypedArray };
    TypedArrayType typedArrayType = typedArrayTypeOf(builtinContextOfTypedElements(walker), id);
    if (isTypedView(typedArrayType))
        return { KindOfBaseOfTypedElements::TypedArray, typedArrayType };
    return { };
}

static bool isArrayIndexInTypedElements(const Type& key)
{
    return key.isInteger() && key.integers.isSubsetOf(arrayIndexesOfTypedElements);
}

static Draft draftOfTypedElements(Walker& walker, const BodyValue& value, Opcode opcode)
{
    Draft draft = walker.draftOf(value);
    ExpectedType expected = draft.expected;
    ValueID inputValueGiven = draft.inputValueGiven;
    draft.become(opcode);
    draft.expected = expected;
    draft.inputValueGiven = inputValueGiven;
    return draft;
}

static Reference freshObjectNamedInTypedElements(Walker& walker, Reference name)
{
    if (!name || !name.isValue())
        return { };
    const Code& code = walker.code();
    ValueID cell = rootOfCellInSweptCode(code, name.id());
    if (code.isConstant(cell))
        return { };
    Reference object = Reference::value(cell);
    return walker.state().isFresh(object) ? object : Reference();
}

static void markBaseAsPassedInTypedElements(Walker& walker, Reference base)
{
    walker.letLeave(base);
}

static void noteStoredElementInTypedElements(Walker& walker, Reference base, Reference key, Reference stored)
{
    Reference object = freshObjectNamedInTypedElements(walker, base);
    walker.state().noteElement(object ? object : base, key, stored);
}

static bool answerAheadOfTypedElements(const AccessOfTypedElements& access, Opcode opcode, KnowledgeLevel level)
{
    access.walker.noteFormAskedAhead(opcode, level);
    return true;
}

static KnowledgeLevel levelOfArrayAccessOfTypedElements(const AccessOfTypedElements& access)
{
    return access.typeOfKey.isInteger() ? KnowledgeLevel::Expected : KnowledgeLevel::Unknown;
}

static KnowledgeLevel levelOfArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    if ((access.base && isFreshListAsMade(access.walker, access.base)) || readsOffListThatStaysAsMadeByLink(access.walker, access.value))
        return KnowledgeLevel::Proven;
    return levelOfArrayAccessOfTypedElements(access);
}

static ArrayStorage storageDeclaredForTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    ArrayStorage declared = arrayStorageDeclaredBy(walker.program(), types, access.typeOfBase).predicted();
    if (declared != ArrayStorage::Other || access.isOnlyAskedAbout)
        return declared;
    TypeID expected = findExpectation(walker, access.value.objectOperand()).type;
    ArrayStorage written = expected ? arrayStorageDeclaredBy(walker.program(), types, types.withoutNullish(expected)).predicted() : ArrayStorage::Other;
    return written == ArrayStorage::Int32 && access.value.opcode() != Opcode::ElemSet ? ArrayStorage::Other : written;
}

static ArrayStorage storageGuessedForStoreOfTypedElements(const AccessOfTypedElements& access)
{
    TypeArena& types = access.walker.types();
    TypeID stored = access.typeOfStored;
    ArrayStorage declared = storageDeclaredForTypedElements(access);
    if (hasArrayStorageForm(declared) && canStoreWithoutConversion(types, declared, stored))
        return declared;
    if (!types.at(stored).isAnything() && arrayStorageHolding(types, stored) == ArrayStorage::Contiguous)
        return ArrayStorage::Contiguous;
    return ArrayStorage::Other;
}

static bool buildArrayReadUnderAnyKeyOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!(access.typeOfKey.primitives & Primitive::Integer))
        return false;
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetArrayElement, KnowledgeLevel::Expected);

    ArrayStorage storage = storageDeclaredForTypedElements(access);
    if (storage == ArrayStorage::Int32)
        storage = ArrayStorage::Double;
    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
    read.setArrayOperand(access.base);
    read.setPositionOperand(access.key);
    read.setArrayStorage(storage);
    read.setAdmitsInt32Storage(storage == ArrayStorage::Double);
    read.setLevel(KnowledgeLevel::Expected);
    markBaseAsPassedInTypedElements(walker, access.base);
    walker.bind(access.value.id(), walker.build(WTF::move(read)));
    return true;
}

Reference buildReadOfElementOffExpectedList(Walker& walker, const BodyValue& read)
{
    Reference base = walker.referenceOf(read.objectOperand());
    Draft draft = draftOfTypedElements(walker, read, Opcode::GetArrayElement);
    draft.setArrayOperand(base);
    draft.setPositionOperand(walker.referenceOf(read.propertyOperand()));
    draft.setArrayStorage(ArrayStorage::Other);
    draft.setLevel(KnowledgeLevel::Expected);
    markBaseAsPassedInTypedElements(walker, base);
    return walker.build(WTF::move(draft));
}

static bool bindNumberReadBehindTestsOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (walker.state().answers().isEmpty())
        return false;
    std::optional<uint64_t> read = findKnown(walker, KnownNote::NumberReadOutOfList, access.base, access.key);
    if (!read)
        return false;
    walker.bind(access.value.id(), Reference::fromBits(static_cast<uint32_t>(*read)));
    return true;
}

static Representation formOfNumberThatLinkHasTypedOfTypedElements(const AccessOfTypedElements& access)
{
    static constexpr OpcodeSet operatorsThatTakeDoubles = operatorsThatMakeNumbers | allSums;
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (!walker.has(Mechanism::UnboxedNumbers) || !access.typeOfKey.isInteger() || !isProvenToBeList(types, access.typeOfBase))
        return Representation::Unchosen;
    TypeID proven = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), access.value.id());
    if (!proven)
        return Representation::Unchosen;
    Type type = types.at(proven);
    if (!type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) || !(type.primitives & Primitive::Number))
        return Representation::Unchosen;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = access.value.id();
    if (read.index() >= index.numberOfValues() || index.value(read).words() != access.value.words())
        return Representation::Unchosen;
    if (!(type.primitives & Primitive::Number & ~Primitive::NumberWithoutFraction))
        return Representation::Unchosen;
    ValueID taken = read;
    ValueID reader = index.onlyUsingValue(read);
    while (reader && inputOperationsThatOnlyHandOn.contains(index.opcodeOf(reader))) {
        taken = reader;
        reader = index.onlyUsingValue(reader);
    }
    if (!reader || !(operatorsThatTakeDoubles.contains(index.opcodeOf(reader)) || allRelations.contains(index.opcodeOf(reader))))
        return Representation::Unchosen;
    bool standsBesideNumbers = true;
    if (operationsThatJoinTexts.contains(index.opcodeOf(reader))) {
        index.value(reader).forEachOperand([&](ValueID operand, uint32_t) {
            Reference other = operand == taken ? Reference() : walker.referenceOf(operand);
            standsBesideNumbers &= operand == taken || (other && other.isRecord() && types.at(walker.typeOf(other)).isNumber());
        });
    }
    if (!standsBesideNumbers)
        return Representation::Unchosen;
    return type.primitives & Primitive::Undefined ? Representation::OptionalDouble : Representation::Double;
}

static bool onlyUserTakesUndefinedForNaNInTypedElements(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (!user)
        return false;
    BodyValue operation = index.value(user);
    if (operation.opcode() != Opcode::Add && operation.opcode() != Opcode::Sub && operation.opcode() != Opcode::Mul && operation.opcode() != Opcode::Div)
        return false;
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    Reference built = other != name ? walker.referenceOf(other) : Reference();
    return built && walker.types().at(walker.typeOf(built)).isNumber();
}

static bool goOnOrBeginTurnAgainInTypedElements(Walker& walker, Reference test)
{
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock(BlockFrequency::Rare);
    walker.branch(test, passed, failed);
    walker.continueIn(failed);
    if (!walker.isUnreachable())
        walker.beginTurnAgainInClosedCopy();
    walker.continueIn(passed);
    walker.noteGuessOpened();
    return !walker.isUnreachable() && !walker.hasFailed();
}

static bool turnMayBeginAgainInTypedElements(const AccessOfTypedElements& access)
{
    return access.isOnlyAskedAbout ? access.walker.looksTakeFormsBehindLooksAsBuilt() : access.walker.standsInTurnThatLooksCountOn();
}
static bool buildArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!access.isOnlyAskedAbout && bindNumberReadBehindTestsOfTypedElements(access))
        return true;
    KnowledgeLevel level = levelOfArrayReadOfTypedElements(access);
    std::optional<ListTestedInFrontOfLoop> tested = level == KnowledgeLevel::Unknown ? std::nullopt : findListTestedInFrontOfLoop(walker, access.value);
    if (tested)
        level = KnowledgeLevel::Proven;
    if (level == KnowledgeLevel::Unknown)
        return buildArrayReadUnderAnyKeyOfTypedElements(access);
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetArrayElement, level);

    ArrayStorage storage = level == KnowledgeLevel::Expected ? storageDeclaredForTypedElements(access) : ArrayStorage::Other;
    if (storage == ArrayStorage::Int32)
        storage = ArrayStorage::Double;

    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
    read.setArrayOperand(access.base);
    read.setPositionOperand(access.key);
    read.setArrayStorage(storage);
    read.setAdmitsInt32Storage(storage == ArrayStorage::Double);
    read.setLevel(level);
    if (!tested && !hasArrayStorageForm(storage))
        read.representation = formOfNumberThatLinkHasTypedOfTypedElements(access);
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    read.setIsInBounds(positionIsKnownToLieInsideList(walker, access.base, access.key));
    if (tested)
        nameKindOfListTestedInFrontOfLoop(read, *tested);
    bool leavesTurnAtHole = tested && turnMayBeginAgainInTypedElements(access) && holeLeavesTurnBehindRow(walker.currentBodyIndex(), access.value, *tested);
    if (!leavesTurnAtHole && tested && read.isInBounds() && holdsOnlyNumbers(read.arrayStorage()) && onlyUserTakesUndefinedForNaNInTypedElements(walker, access.value))
        read.setReadsHoleAsNaN(true);
    if (tested)
        takeStorageAndLengthFromInFrontOfLoop(walker, read, *tested);
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    bool givesNaNForHole = read.readsHoleAsNaN();
    Reference element = walker.build(WTF::move(read));
    if (leavesTurnAtHole && element && walker.types().at(walker.typeOf(element)).primitives & Primitive::Undefined) {
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        Reference isNumber = buildTestOfNumber(walker, element, TypeTable::number, origin);
        if (!goOnOrBeginTurnAgainInTypedElements(walker, isNumber))
            return true;
        element = buildProofOfNumber(walker, element, isNumber, origin);
    }
    if (element && !givesNaNForHole && level == KnowledgeLevel::Proven && isArrayIndexInTypedElements(access.typeOfKey) && !freshObjectNamedInTypedElements(walker, access.base))
        walker.state().noteElement(access.base, access.key, element);
    walker.bind(access.value.id(), element);
    return true;
}

static bool buildArrayStoreUnderAnyKeyOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!(access.typeOfKey.primitives & Primitive::Integer))
        return false;
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::PutArrayElement, KnowledgeLevel::Expected);

    ECMAMode mode = access.value.mode();
    Draft put = draftOfTypedElements(walker, access.value, Opcode::PutArrayElement);
    put.setArrayOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(access.stored);
    put.setArrayStorage(storageGuessedForStoreOfTypedElements(access));
    put.setMode(mode);
    put.setLevel(KnowledgeLevel::Expected);
    markBaseAsPassedInTypedElements(walker, access.base);
    Reference store = walker.build(WTF::move(put));
    noteStoredElementInTypedElements(walker, access.base, access.key, { });
    walker.bind(access.value.id(), store);
    return true;
}

static bool buildStoreBehindRowOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (!isArrayIndexInTypedElements(access.typeOfKey) || !access.typeOfStored || !types.at(access.typeOfStored).isNumber())
        return false;
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, access.value);
    if (!tested || !tested->isWritable || !holdsOnlyNumbers(tested->storage) || tested->admitsInt32Storage)
        return false;

    bool isInside = !access.isOnlyAskedAbout && positionIsKnownToLieInsideList(walker, access.base, access.key);
    bool fits = canStoreWithoutConversion(types, tested->storage, access.typeOfStored);
    if ((!isInside || !fits) && !turnMayBeginAgainInTypedElements(access))
        return false;
    Opcode opcode = tested->storage == ArrayStorage::Double ? Opcode::PutDoubleIndex : Opcode::PutContiguousIndex;
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, opcode, KnowledgeLevel::Proven);

    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    if (!isInside) {
        Draft length(Opcode::GetArrayLength, origin);
        walker.chooseConservativeFields(length);
        length.setArrayOperand(tested->name);
        Draft isBelowLength(Opcode::Less, origin);
        walker.chooseConservativeFields(isBelowLength);
        isBelowLength.setLeftOperand(access.key);
        isBelowLength.setRightOperand(walker.build(WTF::move(length)));
        if (!isBelowLength.rightOperand())
            return true;
        isBelowLength.setForm(formAllowedForTypedOperator(walker, isBelowLength));
        if (!goOnOrBeginTurnAgainInTypedElements(walker, walker.build(WTF::move(isBelowLength))))
            return true;
    }
    Reference stored = access.stored;
    if (!fits && tested->storage == ArrayStorage::Int32) {
        Reference isInt32 = buildTestOfNumber(walker, stored, TypeTable::int32, origin);
        if (!goOnOrBeginTurnAgainInTypedElements(walker, isInt32))
            return true;
        stored = buildProofOfNumber(walker, stored, isInt32, origin);
    } else if (!fits) {
        Draft isItself(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(isItself);
        isItself.setLeftOperand(stored);
        isItself.setRightOperand(stored);
        isItself.setForm(formAllowedForTypedOperator(walker, isItself));
        if (!goOnOrBeginTurnAgainInTypedElements(walker, walker.build(WTF::move(isItself))))
            return true;
    }
    if (!stored)
        return true;

    Draft put = draftOfTypedElements(walker, access.value, opcode);
    put.setArrayOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(stored);
    if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
        put.setStorageOperand(pointer);
    Reference store = walker.build(WTF::move(put));
    noteStoredElementInTypedElements(walker, access.base, access.key, stored);
    walker.bind(access.value.id(), store);
    return true;
}
static bool buildArrayStoreOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (buildStoreBehindRowOfTypedElements(access))
        return true;
    KnowledgeLevel level = levelOfArrayAccessOfTypedElements(access);
    if (level == KnowledgeLevel::Unknown)
        return buildArrayStoreUnderAnyKeyOfTypedElements(access);
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::PutArrayElement, level);

    ECMAMode mode = access.value.mode();
    Draft put = draftOfTypedElements(walker, access.value, Opcode::PutArrayElement);
    put.setArrayOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(access.stored);
    put.setArrayStorage(storageGuessedForStoreOfTypedElements(access));
    put.setMode(mode);
    put.setLevel(level);
    put.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    put.setIsInBounds(level == KnowledgeLevel::Expected && positionIsKnownToLieInsideList(walker, access.base, access.key));
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    Reference store = walker.build(WTF::move(put));
    bool holdsWhatWasStored = level == KnowledgeLevel::Proven && mode == ECMAMode::Strict && isArrayIndexInTypedElements(access.typeOfKey);
    noteStoredElementInTypedElements(walker, access.base, access.key, holdsWhatWasStored ? access.stored : Reference());
    walker.bind(access.value.id(), store);
    return true;
}

static bool buildArrayTestOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    KnowledgeLevel level = levelOfArrayReadOfTypedElements(access);
    if (level == KnowledgeLevel::Unknown)
        return false;
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::HasArrayElement, level);

    Draft test = draftOfTypedElements(walker, access.value, Opcode::HasArrayElement);
    test.setArrayOperand(access.base);
    test.setPositionOperand(access.key);
    test.setArrayStorage(level == KnowledgeLevel::Expected ? storageDeclaredForTypedElements(access) : ArrayStorage::Other);
    test.setLevel(level);
    test.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    walker.bind(access.value.id(), walker.build(WTF::move(test)));
    return true;
}

static Reference lengthOfViewInTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
{
    Walker& walker = access.walker;
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::LengthOfList, access.base))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    if (!walker.isInLoop() || !walker.opensGuesses())
        return { };
    Draft length(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(access.base);
    length.setTypedArrayType(typedArrayType);
    Reference built = walker.build(WTF::move(length));
    if (!built || !built.isValue() || walker.isUnreachable())
        return { };
    auto takeLength = [&] {
        noteKnown(walker, KnownNote::LengthOfList, access.base, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
        return built;
    };
    std::optional<uint32_t> loop = walker.innermostOpenLoop();
    if (loop && walker.standsInFrontOfOpenLoop(*loop, built))
        return takeLength();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = access.value.id();
    bool isNamedAgain = written.index() < index.numberOfValues() && index.value(written).words() == access.value.words() && index.useCount(access.value.objectOperand()) > 1;
    return isNamedAgain ? takeLength() : Reference();
}

static void nameStorageOfViewInTypedElements(const AccessOfTypedElements& written, Draft& access, TypedArrayType typedArrayType)
{
    Walker& walker = written.walker;
    Reference view = written.base;
    if (access.level() != KnowledgeLevel::Proven || !hasInlineElementAccess(typedArrayType) || walker.testsEveryPosition())
        return;
    Reference length = access.isInsideView() ? Reference() : lengthOfViewInTypedElements(written, typedArrayType, access.origin());
    if (!access.isInsideView() && (!length || walker.isUnreachable()))
        return;
    Draft vector(Opcode::GetTypedArrayVector, access.origin());
    walker.chooseConservativeFields(vector);
    vector.setViewOperand(view);
    Reference built = walker.build(WTF::move(vector));
    if (!built)
        return;
    access.setVectorOperand(built);
    if (length)
        access.setLengthOperands(singleElementSpan(length));
}

static bool buildViewReadOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetTypedArrayElement, KnowledgeLevel::Proven);
    Walker& walker = access.walker;
    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetTypedArrayElement);
    read.setViewOperand(access.base);
    read.setPositionOperand(access.key);
    read.setTypedArrayType(typedArrayType);
    read.setIsInsideView(positionIsKnownToLieInsideList(walker, access.base, access.key));
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    nameStorageOfViewInTypedElements(access, read, typedArrayType);
    walker.bind(access.value.id(), walker.build(WTF::move(read)));
    return true;
}

static KnowledgeLevel levelOfViewStoreOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    TypeArena& types = access.walker.types();
    TypeID storedID = access.typeOfStored;
    Type stored = types.at(storedID);
    bool fits = isBigInt(typedArrayType) ? !stored.isNever() && stored.hasOnlyPrimitives(Primitive::BigInt) : stored.isNumber();
    if (fits)
        return KnowledgeLevel::Proven;
    bool mayPassTests = hasInlineElementAccess(typedArrayType) && access.typeOfKey.isInteger() && types.overlap(storedID, TypeTable::number);
    return mayPassTests ? KnowledgeLevel::Expected : KnowledgeLevel::Unknown;
}

static bool buildViewStoreOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    Walker& walker = access.walker;
    KnowledgeLevel level = levelOfViewStoreOfTypedElements(access, typedArrayType);
    if (level == KnowledgeLevel::Unknown)
        return false;
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::PutTypedArrayElement, level);

    ECMAMode mode = access.value.mode();
    Draft put = draftOfTypedElements(walker, access.value, Opcode::PutTypedArrayElement);
    put.setViewOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(access.stored);
    put.setTypedArrayType(typedArrayType);
    put.setIsInsideView(level == KnowledgeLevel::Proven && positionIsKnownToLieInsideList(walker, access.base, access.key));
    put.setMode(mode);
    put.setLevel(level);
    put.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    nameStorageOfViewInTypedElements(access, put, typedArrayType);
    walker.bind(access.value.id(), walker.build(WTF::move(put)));
    return true;
}

static ExpectedElements elementsExpectedByTypedElements(TypedArrayType typedArrayType)
{
    if (typedArrayType == TypeInt32)
        return ExpectedElements::Int32Array;
    if (typedArrayType == TypeUint8)
        return ExpectedElements::Uint8Array;
    return ExpectedElements::Anything;
}

static bool buildAccessByIndexOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    bool keyIsIntegral = access.typeOfKey.isNumberWithoutFraction();
    bool namesWhatIsExpected = access.typeOfKey.isNumber() && elementsExpectedByTypedElements(typedArrayType) != ExpectedElements::Anything;
    if ((!keyIsIntegral && !namesWhatIsExpected) || access.isOnlyAskedAbout)
        return false;

    Walker& walker = access.walker;
    Draft draft = walker.draftOf(access.value);
    walker.chooseConservativeFields(draft);
    draft.setKeyIsIntegral(keyIsIntegral);
    draft.setExpectedElements(elementsExpectedByTypedElements(typedArrayType));
    markBaseAsPassedInTypedElements(walker, access.base);
    walker.bind(access.value.id(), walker.build(WTF::move(draft)));
    return true;
}

static bool buildReadOfFreshArgumentsOfTypedElements(const AccessOfTypedElements& access)
{
    if (access.isOnlyAskedAbout || !access.typeOfKey.isInteger())
        return false;
    Walker& walker = access.walker;
    Range positions = access.typeOfKey.integers;
    if (positions.isSingleValue() && positions.minimum >= 0 && positions.minimum < UINT32_MAX) {
        if (Reference held = elementOfFreshArguments(walker, access.base, static_cast<uint32_t>(positions.minimum))) {
            walker.bind(access.value.id(), held);
            return true;
        }
    }

    std::optional<ListBehindParametersOfArguments> behind = listBehindParametersOfFreshArguments(walker, access.base);
    if (!behind || !positionsOfFreshArgumentsAreInSight(walker, access.base, positions.minimum))
        return false;
    BlockID behindArms;
    Reference met;
    for (uint32_t parameter = static_cast<uint32_t>(positions.minimum); parameter < behind->firstPosition && !walker.isUnreachable(); ++parameter) {
        Origin origin = walker.originOf(access.value);
        Reference held = elementOfFreshArguments(walker, access.base, parameter);
        if (!behindArms) {
            behindArms = walker.addBlock();
            met = walker.addParameter(behindArms, { }, Representation::JSValue);
        }
        Draft number(Opcode::Integer, origin);
        number.setInteger(parameter);
        Draft test(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(access.key);
        test.setRightOperand(walker.constant(WTF::move(number)));
        test.setForm(formAllowedForTypedOperator(walker, test));
        Reference isParameter = walker.build(WTF::move(test));
        if (!isParameter)
            return true;
        BlockID takesParameter = walker.addBlock();
        BlockID otherwise = walker.addBlock();
        walker.branch(isParameter, takesParameter, otherwise);
        walker.continueIn(takesParameter);
        walker.jumpTo(behindArms, singleElementSpan(held));
        walker.continueIn(otherwise);
    }
    Reference position = access.key;
    if (behind->firstPosition && positions.isSingleValue()) {
        Draft inList(Opcode::Integer, walker.originOf(access.value));
        inList.setInteger(positions.minimum - behind->firstPosition);
        position = walker.constant(WTF::move(inList));
    } else if (behind->firstPosition) {
        Origin origin = walker.originOf(access.value);
        Draft first(Opcode::Integer, origin);
        first.setInteger(behind->firstPosition);
        Draft difference(Opcode::Sub, origin);
        walker.chooseConservativeFields(difference);
        difference.setLeftOperand(position);
        difference.setRightOperand(walker.constant(WTF::move(first)));
        difference.setForm(formAllowedForTypedOperator(walker, difference));
        position = walker.build(WTF::move(difference));
        if (!position)
            return true;
        position = boxedUnlessNumbersAreUnboxed(walker, position, origin);
    }

    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
    read.setArrayOperand(behind->list);
    read.setPositionOperand(position);
    read.setArrayStorage(ArrayStorage::Other);
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(true);
    Reference element = walker.build(WTF::move(read));
    if (!behindArms) {
        walker.bind(access.value.id(), element);
        return true;
    }
    if (element)
        walker.jumpTo(behindArms, singleElementSpan(element));
    walker.continueIn(behindArms);
    if (!walker.isUnreachable())
        walker.bind(access.value.id(), met);
    return true;
}

static bool buildReadOfTypedElements(const AccessOfTypedElements& access)
{
    BaseOfTypedElements base = classifyBaseOfTypedElements(access.walker, access.typeOfBase);
    if (!access.typeOfKey.isNumber())
        return base.kind == KindOfBaseOfTypedElements::Array && buildArrayReadUnderAnyKeyOfTypedElements(access);
    switch (base.kind) {
    case KindOfBaseOfTypedElements::Array:
        if (buildArrayReadOfTypedElements(access))
            return true;
        break;
    case KindOfBaseOfTypedElements::TypedArray:
        return buildViewReadOfTypedElements(access, base.typedArrayType);
    case KindOfBaseOfTypedElements::Arguments:
        if (buildReadOfFreshArgumentsOfTypedElements(access))
            return true;
        break;
    case KindOfBaseOfTypedElements::String:
        break;
    case KindOfBaseOfTypedElements::Other:
        return false;
    }
    return buildAccessByIndexOfTypedElements(access, base.typedArrayType);
}

static bool buildStoreOfTypedElements(const AccessOfTypedElements& access)
{
    BaseOfTypedElements base = classifyBaseOfTypedElements(access.walker, access.typeOfBase);
    if (!access.typeOfKey.isNumber())
        return base.kind == KindOfBaseOfTypedElements::Array && buildArrayStoreUnderAnyKeyOfTypedElements(access);
    switch (base.kind) {
    case KindOfBaseOfTypedElements::Array:
        if (buildArrayStoreOfTypedElements(access))
            return true;
        break;
    case KindOfBaseOfTypedElements::TypedArray:
        if (buildViewStoreOfTypedElements(access, base.typedArrayType))
            return true;
        break;
    case KindOfBaseOfTypedElements::String:
    case KindOfBaseOfTypedElements::Arguments:
        break;
    case KindOfBaseOfTypedElements::Other:
        return false;
    }
    return buildAccessByIndexOfTypedElements(access, base.typedArrayType);
}

static bool buildTestOfTypedElements(const AccessOfTypedElements& access)
{
    switch (classifyBaseOfTypedElements(access.walker, access.typeOfBase).kind) {
    case KindOfBaseOfTypedElements::Array:
        return buildArrayTestOfTypedElements(access);
    case KindOfBaseOfTypedElements::TypedArray:
    case KindOfBaseOfTypedElements::String:
    case KindOfBaseOfTypedElements::Arguments:
    case KindOfBaseOfTypedElements::Other:
        break;
    }
    return false;
}

static bool buildLengthOfTypedElements(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.isFreeName() || value.key() != walker.program().keyOf(BuiltinNames::length))
        return false;

    TypeArena& types = walker.types();
    TypeID typeOfBase = inputs.typeOf(value.objectOperand());
    bool isOnlyAskedAbout = inputs.areOnlyTypes();
    Reference base = isOnlyAskedAbout ? Reference() : walker.referenceOf(value.objectOperand());
    auto answerAhead = [&](Opcode built) {
        walker.noteFormAskedAhead(built, KnowledgeLevel::Proven);
        return true;
    };
    if (hasKind(types, typeOfBase, OperandKind::Array)) {
        if (isOnlyAskedAbout)
            return answerAhead(Opcode::GetArrayLength);
        if (std::optional<uint32_t> known = lengthOfFreshList(walker, base)) {
            Draft constant(Opcode::Integer, walker.originOf(value));
            constant.setInteger(*known);
            walker.bind(value.id(), walker.constant(WTF::move(constant)));
            return true;
        }
        Draft length = draftOfTypedElements(walker, value, Opcode::GetArrayLength);
        length.setArrayOperand(base);
        walker.bind(value.id(), walker.build(WTF::move(length)));
        return true;
    }

    BaseOfTypedElements classified = classifyBaseOfTypedElements(walker, typeOfBase);
    switch (classified.kind) {
    case KindOfBaseOfTypedElements::String: {
        if (isOnlyAskedAbout)
            return answerAhead(Opcode::GetStringLength);
        Draft length = draftOfTypedElements(walker, value, Opcode::GetStringLength);
        length.setStringOperand(base);
        walker.bind(value.id(), walker.build(WTF::move(length)));
        return true;
    }
    case KindOfBaseOfTypedElements::TypedArray: {
        const ClassPart* onlyClass = singleBuiltinClassOf(types, types.at(typeOfBase));
        bool isSeenToFindGetter = onlyClass && ((!isOnlyAskedAbout && findConstantMemberReadOff(walker, base, value.key())) || readsOffEntryParameterThatStaysAsMade(walker, value));
        if (!isSeenToFindGetter)
            return false;
        if (isOnlyAskedAbout)
            return answerAhead(Opcode::GetTypedArrayLength);
        Draft length = draftOfTypedElements(walker, value, Opcode::GetTypedArrayLength);
        length.setViewOperand(base);
        length.setTypedArrayType(classified.typedArrayType);
        Reference built = walker.build(WTF::move(length));
        walker.bind(value.id(), built);
        if (built && built.isValue() && !walker.isUnreachable())
            noteKnown(walker, KnownNote::LengthOfList, base, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
        return true;
    }
    case KindOfBaseOfTypedElements::Arguments:
        if (Reference length = isOnlyAskedAbout ? Reference() : lengthOfFreshArguments(walker, base)) {
            walker.bind(value.id(), length);
            return true;
        }
        break;
    case KindOfBaseOfTypedElements::Array:
    case KindOfBaseOfTypedElements::Other:
        break;
    }
    return false;
}

bool buildTypedElementAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (buildElementAccessUnderNumber(walker, value, inputs))
        return true;
    Opcode opcode = value.opcode();
    if (!opcodesOfTypedElements.contains(opcode))
        return false;
    if (opcode == Opcode::PropGet)
        return buildLengthOfTypedElements(walker, value, inputs);

    Type typeOfKey = walker.types().at(inputs.typeOf(value.propertyOperand()));
    if (!typeOfKey.isNumber() && opcode == Opcode::ElemHas)
        return false;

    bool isOnlyAskedAbout = inputs.areOnlyTypes();
    auto builtFor = [&](ValueID input) {
        return isOnlyAskedAbout ? Reference() : walker.referenceOf(input);
    };
    AccessOfTypedElements access { walker, value, builtFor(value.objectOperand()), builtFor(value.propertyOperand()), { }, inputs.typeOf(value.objectOperand()), typeOfKey, { }, isOnlyAskedAbout };
    if (opcode == Opcode::ElemGet)
        return buildReadOfTypedElements(access);
    if (opcode == Opcode::ElemHas)
        return buildTestOfTypedElements(access);
    access.stored = builtFor(value.valueOperand());
    access.typeOfStored = inputs.typeOf(value.valueOperand());
    return buildStoreOfTypedElements(access);
}

bool buildTypedElementAccess(Walker& walker, const BodyValue& value)
{
    return buildTypedElementAccess(walker, value, InputsOfTypedOperation(walker, nullptr));
}

bool buildAccessByIndexOffExpectedViewOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    TypeID expected = findExpectation(walker, value.objectOperand()).type;
    if (!expected)
        return false;
    BaseOfTypedElements base = classifyBaseOfTypedElements(walker, walker.types().withoutNullish(expected));
    if (base.kind != KindOfBaseOfTypedElements::TypedArray || elementsExpectedByTypedElements(base.typedArrayType) == ExpectedElements::Anything)
        return false;
    Reference object = walker.referenceOf(value.objectOperand());
    Reference key = walker.referenceOf(value.propertyOperand());
    if (!object || !key)
        return false;
    AccessOfTypedElements access { walker, value, object, key, { }, walker.typeOf(object), walker.types().at(walker.typeOf(key)), { }, false };
    return buildAccessByIndexOfTypedElements(access, base.typedArrayType);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
