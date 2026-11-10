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

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet opcodesOfTypedElements { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::PropGet, Opcode::PropSet };

static constexpr Range arrayIndexesOfTypedElements { 0, static_cast<int64_t>(UINT32_MAX) - 1 };

static constexpr int64_t mostOfRangeAskedWholeInTypedElements = 255;

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
    ValueID counterThatPushGoesBy { };
};

struct LevelOfArrayReadOfTypedElements {
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    bool restsOnLinksWordAlone { false };
};

} // anonymous namespace

static bool readsElementsInLineInTypedElements(TypedArrayType type)
{
    return type == NotTypedArray || hasInlineElementAccess(type);
}

static TypeID elementTypeInTypedElements(TypedArrayType type)
{
    return type == NotTypedArray ? TypeID(TypeTable::number) : elementTypeOf(type);
}

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
    if (!onlyClass && type.hasOnlyClassObjects() && kindsOfViewsOfNumbersIn(walker.program(), walker.types(), id))
        return { KindOfBaseOfTypedElements::TypedArray, NotTypedArray };
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
    ValueID cell = underlyingCellOf(code, name.id());
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

static LevelOfArrayReadOfTypedElements levelOfArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    if (access.base && isFreshPristineArray(access.walker, access.base))
        return { KnowledgeLevel::Proven, false };
    if (!readsOffListThatStaysAsMadeByLink(access.walker, access.value))
        return { levelOfArrayAccessOfTypedElements(access), false };
    return { KnowledgeLevel::Proven, !access.isOnlyAskedAbout && !isKnownPristine(access.walker, access.base) };
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

static TypeID typeWrittenOfInputInTypedElements(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyValue definition = index.value(input);
    if (definition.isParameter())
        return definition.parameterType();
    if (definition.opcode() != Opcode::ElemGet)
        return { };
    BodyValue list = index.value(index.stripCasts(definition.objectOperand()));
    if (!list.isParameter() || !list.parameterType())
        return { };
    TypingContext expecting { walker.program(), walker.types(), walker.positionsKnowingNothing(), TypeLevel::Expected };
    return typeOfElementRead(expecting, walker.types().withoutNullish(list.parameterType()), TypeTable::int32);
}

static bool whatIsStoredIsWrittenToBeNoNumberInTypedElements(const AccessOfTypedElements& access)
{
    TypeArena& types = access.walker.types();
    if (access.isOnlyAskedAbout || !access.typeOfStored || !types.at(access.typeOfStored).isAnything())
        return false;
    TypeID written = typeWrittenOfInputInTypedElements(access.walker, access.value.valueOperand());
    if (!written || types.at(written).isAnything() || types.at(written).primitives & Primitive::Number)
        return false;
    return arrayStorageHolding(types, written) == ArrayStorage::Contiguous && !isDeclaredToHoldNumbersAlone(access.walker.program(), types, access.typeOfBase);
}

static ArrayStorage storageGuessedForStoreOfTypedElements(const AccessOfTypedElements& access)
{
    TypeArena& types = access.walker.types();
    TypeID stored = access.typeOfStored;
    ArrayStorage declared = storageDeclaredForTypedElements(access);
    if (hasArrayStorageForm(declared) && canStoreWithoutConversion(types, declared, stored))
        return declared;
    if (whatIsStoredIsWrittenToBeNoNumberInTypedElements(access))
        return ArrayStorage::Contiguous;
    if (!types.at(stored).isAnything() && !types.overlap(stored, TypeTable::number) && arrayStorageHolding(types, stored) == ArrayStorage::Contiguous && !isDeclaredToHoldNumbersAlone(access.walker.program(), types, access.typeOfBase))
        return ArrayStorage::Contiguous;
    return ArrayStorage::Other;
}

static Representation formOfNumberThatLinkHasTypedOfTypedElements(const AccessOfTypedElements&, KnowledgeLevel);

static std::span<const uint64_t> pairsOfValueAndUserWithinBlockOfTypedElements(Walker& walker, ValueID value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(value));
    Walker::UsersWithinBlock& found = walker.usersWithinBlockOfFrame();
    if (found.body != &index || found.function != walker.currentFunction() || found.block != block.id()) {
        found.body = &index;
        found.function = walker.currentFunction();
        found.block = block.id();
        found.pairsOfValueAndUser.shrink(0);
        uint32_t firstOfBlock = block.firstParameter().index();
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                if (operand.index() < firstOfBlock)
                    return;
                uint32_t uses = index.useCount(operand);
                if (uses > 1 && uses < BodyIndex::maxCountedUses)
                    found.pairsOfValueAndUser.append(static_cast<uint64_t>(operand.index()) << 32 | operation.id().index());
            });
        }
        std::ranges::sort(found.pairsOfValueAndUser);
    }
    auto ofValue = std::ranges::equal_range(found.pairsOfValueAndUser, value.index(), { }, [](uint64_t pair) {
        return static_cast<uint32_t>(pair >> 32);
    });
    return { ofValue.begin(), ofValue.end() };
}

template<typename Functor>
static bool everyUseStandsInBlockOfValueAndPassesInTypedElements(Walker& walker, ValueID value, const Functor& passes)
{
    uint32_t usesLeft = walker.currentBodyIndex().useCount(value);
    if (usesLeft < 2 || usesLeft >= BodyIndex::maxCountedUses)
        return false;
    std::span<const uint64_t> pairs = pairsOfValueAndUserWithinBlockOfTypedElements(walker, value);
    if (pairs.size() != usesLeft)
        return false;
    std::array<uint32_t, BodyIndex::maxCountedUses> users;
    for (uint32_t use = 0; use < usesLeft; ++use)
        users[use] = static_cast<uint32_t>(pairs[use]);
    for (uint32_t use = 0; use < usesLeft; ++use) {
        if ((!use || users[use] != users[use - 1]) && !passes(ValueID(users[use])))
            return false;
    }
    return true;
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
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (!hasArrayStorageForm(storage))
        read.representation = formOfNumberThatLinkHasTypedOfTypedElements(access, KnowledgeLevel::Expected);
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

static Representation formOfNumberThatLinkHasTypedOfTypedElements(const AccessOfTypedElements& access, KnowledgeLevel level)
{
    static constexpr OpcodeSet operatorsThatTakeDoubles = numberProducingOperators | allSums;
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (!walker.has(Mechanism::UnboxedNumbers) || !access.key || !access.key.isRecord() || !numberOfProvenListIsReadUnboxed(types, access.typeOfBase, walker.typeOf(access.key), walker.representationOf(access.key), level == KnowledgeLevel::Expected))
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
    bool holdsIntegersAlone = !(type.primitives & Primitive::Number & ~Primitive::Integer);
    bool claimsAnyNumberAsDouble = holdsIntegersAlone || (type.primitives & Primitive::FractionalNumber);
    auto standsBesideFraction = [&](ValueID reader, ValueID taken) {
        bool found = false;
        index.value(reader).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand == taken)
                return;
            TypeID beside;
            if (operand.index() > read.index()) {
                ValueID readToCome = index.stripCasts(operand);
                if (!index.isParameter(readToCome) && index.opcodeOf(readToCome) == Opcode::ElemGet)
                    beside = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), readToCome);
            } else if (Reference other = walker.referenceOf(operand); other && other.isRecord())
                beside = walker.typeOf(other);
            found |= beside && types.at(beside).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && (types.at(beside).primitives & Primitive::Number & ~Primitive::Integer);
        });
        return found;
    };
    auto takesDouble = [&](ValueID reader, ValueID taken) {
        if (!(operatorsThatTakeDoubles.contains(index.opcodeOf(reader)) || allRelations.contains(index.opcodeOf(reader))))
            return false;
        if (holdsIntegersAlone && !standsBesideFraction(reader, taken))
            return false;
        bool standsBesideNumbers = true;
        if (operationsThatJoinTexts.contains(index.opcodeOf(reader))) {
            auto isNumberOrUndefined = [&](TypeID beside) {
                return beside && types.at(beside).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && (types.at(beside).primitives & Primitive::Number);
            };
            index.value(reader).forEachOperand([&](ValueID operand, uint32_t) {
                if (operand == taken)
                    return;
                if (operand.index() > read.index()) {
                    ValueID readToCome = index.stripCasts(operand);
                    standsBesideNumbers &= !index.isParameter(readToCome) && (numberProducingOperators.contains(index.opcodeOf(readToCome)) || (index.opcodeOf(readToCome) == Opcode::ElemGet && isNumberOrUndefined(walker.facts().linked().resultOfReadByKey(walker.currentFunction(), readToCome))));
                    return;
                }
                Reference other = walker.referenceOf(operand);
                standsBesideNumbers &= other && other.isRecord() && isNumberOrUndefined(walker.typeOf(other));
            });
        }
        return standsBesideNumbers;
    };
    Representation unboxed = type.primitives & Primitive::Undefined ? Representation::OptionalDouble : Representation::Double;
    uint32_t uses = index.useCount(read);
    if (uses > 1 && uses < BodyIndex::maxCountedUses) {
        bool everyUserTakesDouble = everyUseStandsInBlockOfValueAndPassesInTypedElements(walker, read, [&](ValueID user) {
            return takesDouble(user, read);
        });
        if (everyUserTakesDouble && claimsAnyNumberAsDouble)
            walker.exchangeResultOfReadBeingTyped(types.withPrimitives(proven, Primitive::Number));
        return everyUserTakesDouble ? unboxed : Representation::Unchosen;
    }
    ValueID taken = read;
    ValueID reader = index.onlyUsingValue(read);
    while (reader && inputIdentityOpcodes.contains(index.opcodeOf(reader))) {
        taken = reader;
        reader = index.onlyUsingValue(reader);
    }
    if (!reader || !takesDouble(reader, taken))
        return Representation::Unchosen;
    if (claimsAnyNumberAsDouble)
        walker.exchangeResultOfReadBeingTyped(types.withPrimitives(proven, Primitive::Number));
    return unboxed;
}

static bool onlyUserReckonsInTypedElements(const BodyIndex& index, ValueID name)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    return user && (arithmetic | allBitwiseOperators | allRelations).contains(index.opcodeOf(user));
}

enum class ReadersOfElement : uint8_t { All, ThoseThatDoMoreThanReckon };

static bool elementIsReadAgainInLoopInTypedElements(const BodyIndex& index, const BodyValue& read, ReadersOfElement readers = ReadersOfElement::All)
{
    if (read.opcode() != Opcode::ElemGet || read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    if (!loop)
        return false;
    ValueID list = index.stripCasts(read.objectOperand());
    ValueID key = index.stripCasts(read.propertyOperand());
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue other : index.block(member).values()) {
            if (other.opcode() == Opcode::ElemGet && other.id() != read.id() && index.stripCasts(other.objectOperand()) == list && index.stripCasts(other.propertyOperand()) == key && (readers == ReadersOfElement::All || !onlyUserReckonsInTypedElements(index, other.id())))
                return true;
        }
    }
    return false;
}

static bool isNumberCutByWrittenMaskInTypedElements(Walker& walker, ValueID key, ValueID firstValueNotBuilt)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = index.stripCasts(key);
    if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::BitAnd)
        return false;
    bool hasMask = false;
    bool restIsNumber = true;
    index.value(written).forEachOperand([&](ValueID operand, uint32_t) {
        ValueID cut = index.stripCasts(operand);
        if (cut && !index.isParameter(cut) && index.opcodeOf(cut) == Opcode::Integer && index.value(cut).integer() >= 0 && index.value(cut).integer() <= INT32_MAX) {
            hasMask = true;
            return;
        }
        Reference built = operand.index() < firstValueNotBuilt.index() ? walker.referenceOf(operand) : Reference();
        restIsNumber &= built && walker.types().at(walker.typeOf(built)).isNumber();
    });
    return hasMask && restIsNumber;
}

static bool comesOffListOfNumbersTestedInFrontInTypedElements(Walker& walker, const BodyValue& later, ValueID firstValueNotBuilt)
{
    if (later.objectOperand().index() >= firstValueNotBuilt.index())
        return false;
    if (later.propertyOperand().index() >= firstValueNotBuilt.index()) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID written = index.stripCasts(later.propertyOperand());
        bool isWrittenPosition = written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer && index.value(written).integer() >= 0 && index.value(written).integer() < UINT32_MAX;
        if (!isWrittenPosition && !isNumberCutByWrittenMaskInTypedElements(walker, later.propertyOperand(), firstValueNotBuilt))
            return false;
    } else if (Reference key = walker.referenceOf(later.propertyOperand()); !key || !isArrayIndexInTypedElements(walker.types().at(walker.typeOf(key))))
        return false;
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(walker, later);
    return tested && isNumericStorage(tested->storage);
}

static bool userTakesUndefinedForNaNInTypedElements(Walker& walker, const BodyValue& read, ValueID name, ValueID user, WhatStandsBesideRead beside, bool isOneOfSeveral, bool readGivesUpItsHole)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyValue operation = index.value(user);
    BodyBlock blockOfRead = index.block(index.blockOf(read.id()));
    if (uint32_t end = walker.endOfStretchBeingWalked(); walker.standsInArmOfRowOfShapeTests() && end < blockOfRead.numberOfValues() && (!blockOfRead.holds(user) || user.index() - blockOfRead.firstValue().index() >= end))
        return false;
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    bool takesBits = allBitwiseOperators.contains(operation.opcode());
    if (takesBits && operation.opcode() == Opcode::BitNot)
        return true;
    bool isRelation = allRelations.contains(operation.opcode());
    if (!takesBits && !isRelation && !arithmetic.contains(operation.opcode()))
        return false;
    if (readGivesUpItsHole && isRelation && elementIsReadAgainInLoopInTypedElements(index, read))
        return false;
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    if (other == name)
        return isOneOfSeveral;
    ValueID written = index.stripCasts(other);
    if (written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer && (takesBits || isRelation || std::abs(index.value(written).integer()) > 1))
        return true;
    Reference built = other.index() < read.id().index() ? walker.referenceOf(other) : Reference();
    if (beside == WhatStandsBesideRead::Number || isRelation)
        return built && walker.types().at(walker.typeOf(built)).isNumber();
    auto isNumberOrNothing = [&](TypeID type) {
        Type held = walker.types().at(type);
        return (held.primitives & Primitive::Number) && held.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    };
    if (other.index() < read.id().index())
        return built && isNumberOrNothing(walker.typeOf(built));
    if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::ElemGet)
        return false;
    if (operation.opcode() == Opcode::Add) {
        if (index.blockOf(written) != index.blockOf(read.id()))
            return false;
        for (uint32_t between = read.id().index() + 1; between < written.index(); ++between) {
            if (!primitiveConstants.contains(index.opcodeOf(ValueID(between))) && !opcodesThatStateTypes.contains(index.opcodeOf(ValueID(between))))
                return false;
        }
        return (!readGivesUpItsHole || !elementIsReadAgainInLoopInTypedElements(index, read, ReadersOfElement::ThoseThatDoMoreThanReckon)) && (comesOffListOfNumbersTestedInFrontInTypedElements(walker, index.value(written), read.id()) || walker.shouldAssumeNumericAddends());
    }
    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), written);
    return (seenByLink && isNumberOrNothing(seenByLink)) || ((!readGivesUpItsHole || !elementIsReadAgainInLoopInTypedElements(index, read, ReadersOfElement::ThoseThatDoMoreThanReckon)) && comesOffListOfNumbersTestedInFrontInTypedElements(walker, index.value(written), read.id()));
}

bool onlyUserTakesUndefinedForNaNInTypedElements(Walker& walker, const BodyValue& read, WhatStandsBesideRead beside)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    return user && userTakesUndefinedForNaNInTypedElements(walker, read, name, user, beside, false, true);
}

static bool everyUserTakesUndefinedForNaNInTypedElements(Walker& walker, const BodyValue& read, WhatStandsBesideRead beside, bool readGivesUpItsHole = true)
{
    if (walker.shouldAssumeArithmeticUsers()) [[unlikely]]
        return true;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (user)
        return userTakesUndefinedForNaNInTypedElements(walker, read, name, user, beside, false, readGivesUpItsHole);
    return everyUseStandsInBlockOfValueAndPassesInTypedElements(walker, name, [&](ValueID userOfSeveral) {
        return userTakesUndefinedForNaNInTypedElements(walker, read, name, userOfSeveral, beside, true, readGivesUpItsHole);
    });
}

static Reference numberMadeOnceOfElementInTypedElements(Walker& walker, const BodyValue& read, Reference element)
{
    if (!element || !element.isValue())
        return element;
    Type type = walker.types().at(walker.typeOf(element));
    if (!(type.primitives & Primitive::Undefined) || !(type.primitives & Primitive::Number) || !type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
        return element;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(read.id());
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    if (user || !everyUserTakesUndefinedForNaNInTypedElements(walker, read, WhatStandsBesideRead::NumberOrNothing, false))
        return element;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Draft conversion(Opcode::ToNumber, origin);
    conversion.setValueOperand(element);
    walker.chooseConservativeFields(conversion);
    conversion.setForm(operatorFormFor(walker, conversion));
    Reference number = walker.build(WTF::move(conversion));
    return number && number.isValue() ? number : element;
}

static bool onlyUserHoldsItAgainstNumberInTypedElements(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet equalities { Opcode::Eq, Opcode::StrictEq };
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (!user || !(allRelations | equalities).contains(index.opcodeOf(user)) || elementIsReadAgainInLoopInTypedElements(index, read))
        return false;
    BodyValue operation = index.value(user);
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    if (other == name)
        return false;
    ValueID written = index.stripCasts(other);
    if (written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer)
        return true;
    Reference built = other.index() < read.id().index() ? walker.referenceOf(other) : Reference();
    return built && walker.types().at(walker.typeOf(built)).isNumber();
}

static bool turnTakesWhatIsReadForKeyOrStoresItIntoViewInTypedElements(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    if (!loop)
        return false;
    Vector<ValueID, 4> names { read.id() };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            Opcode opcode = operation.opcode();
            if (opcodesThatStateTypes.contains(opcode)) {
                if (names.contains(operation.operand(0)))
                    names.append(operation.id());
                continue;
            }
            if (!accessesByKey.contains(opcode))
                continue;
            if (names.contains(operation.propertyOperand()))
                return true;
            if (opcode != Opcode::ElemSet || !names.contains(operation.valueOperand()))
                continue;
            Reference view = walker.referenceOf(operation.objectOperand());
            if (view && isViewOfNumbersReadInLine(walker, walker.typeOf(view)))
                return true;
        }
    }
    return false;
}

static void jumpToGuardFallback(Walker& walker)
{
    if (walker.stretchMayRunAsWrittenInstead())
        walker.runStretchAsWrittenInstead();
    else
        walker.restartIteration();
}

static bool continueOrRestartIteration(Walker& walker, Reference test, bool shouldRecordEdgeOutcomes = false)
{
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock(BlockFrequency::Rare);
    walker.branch(test, passed, failed, shouldRecordEdgeOutcomes);
    walker.continueIn(failed);
    if (!walker.isUnreachable())
        jumpToGuardFallback(walker);
    walker.continueIn(passed);
    walker.recordSpeculation();
    return !walker.isUnreachable() && !walker.hasFailed();
}

static bool canGuardAccess(const AccessOfTypedElements& access)
{
    return access.isOnlyAskedAbout ? access.walker.looksTakeFormsBehindLooksAsBuilt() : access.walker.stretchMayRunAsWrittenInstead() || access.walker.canBuildIterationGuard() || access.walker.canGuardAfterStores();
}

static bool askElementInTurnOfTypedElements(const AccessOfTypedElements& access, Draft& read, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    if (!read.keyIsIntegral() || !hasArrayStorageForm(read.arrayStorage()))
        return true;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Draft isPresent(Opcode::IsElementPresent, origin);
    walker.chooseConservativeFields(isPresent);
    isPresent.setArrayOperand(access.base);
    isPresent.setPositionOperand(access.key);
    isPresent.setKeyIsIntegral(true);
    if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(access.base, access.key))
        isPresent.setIsInBounds(*inside);
    nameKindOfListTestedInFrontOfLoop(isPresent, tested);
    takeStorageAndLengthFromInFrontOfLoop(walker, isPresent, tested);
    Reference test = walker.build(WTF::move(isPresent));
    if (!test)
        return false;
    BlockID found = walker.addBlock();
    BlockID absent = walker.addBlock(BlockFrequency::Rare);
    walker.branchOnTest(test, found, absent);
    walker.continueIn(absent);
    if (!walker.isUnreachable())
        jumpToGuardFallback(walker);
    walker.continueIn(found);
    walker.recordSpeculation();
    std::optional<PassedTest> present = walker.isUnreachable() || walker.hasFailed() ? std::nullopt : walker.showElementPresentBehindTest(access.base, access.key, walker.findPassedTest(test));
    if (!present)
        return false;
    noteKnown(walker, KnownNote::PositionFoundInsideList, access.base, access.key, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ArrayStorage }), test.bits());
    read.setIsPresent(*present);
    return true;
}

static bool turnsAroundStoreByKeyInTypedElements(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return true;
    if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemSet }))
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    if (!loop)
        return false;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemSet)
                return true;
        }
    }
    return false;
}

static ArrayStorage kindAskedInTurnOfTypedElements(const AccessOfTypedElements& access)
{
    ArrayStorage storage = storageDeclaredForTypedElements(access);
    if (storage == ArrayStorage::Int32 || (!hasArrayStorageForm(storage) && isDeclaredToHoldNumbersAlone(access.walker.program(), access.walker.types(), access.typeOfBase)))
        storage = ArrayStorage::Double;
    if (storage == ArrayStorage::Double && turnsAroundStoreByKeyInTypedElements(access))
        return ArrayStorage::Other;
    if (storage == ArrayStorage::Contiguous) {
        Walker& walker = access.walker;
        TypeArena& types = walker.types();
        TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };
        TypeID element = typeOfElementRead(expecting, types.withoutNullish(access.typeOfBase), TypeTable::int32);
        if (!element || types.at(element).isAnything() || !isDeclaredToHoldNumbersAlone(walker.program(), types, types.withoutNullish(element)))
            return ArrayStorage::Other;
    }
    return storage;
}

static bool looksCountOnTurnsOfLoopAroundInTypedElements(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    return loop && access.walker.canUseIterationGuards(index.loop(loop).head);
}

static bool someElementMeetsWhatGoesRoundInTypedElements(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    return loop && access.walker.someElementOfLoopMeetsWhatGoesRoundInCurrentBody(loop);
}

bool Walker::someElementOfLoopMeetsWhatGoesRoundInCurrentBody(LoopID loop)
{
    static constexpr OpcodeSet operatorsOfRunningValues { Opcode::Add, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
    enum : uint8_t { NotAsked, No, Yes };
    const BodyIndex& index = currentBodyIndex();
    Vector<uint8_t>& answers = m_whetherElementsOfLoopsMeetWhatGoesRound.ensure(&index, [&] {
        Vector<uint8_t> none;
        none.fill(NotAsked, index.numberOfLoops());
        return none;
    }).iterator->value;
    uint8_t& answer = answers[loop.index()];
    if (answer == NotAsked)
        answer = findsElementThatMeetsWhatGoesRound(index, loop, operatorsOfRunningValues) ? Yes : No;
    return answer == Yes;
}

bool Walker::findsElementThatMeetsWhatGoesRound(const BodyIndex& index, LoopID loop, OpcodeSet operatorsOfRunningValues)
{
    BodyBlock head = index.block(index.loop(loop).head);
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!operatorsOfRunningValues.contains(operation.opcode()))
                continue;
            for (uint32_t side = 0; side < 2; ++side) {
                ValueID element = index.stripCasts(operation.operand(side));
                ValueID goesRound = index.stripCasts(operation.operand(1 - side));
                if (element && goesRound && !index.isParameter(element) && index.opcodeOf(element) == Opcode::ElemGet && head.indexOfParameter(goesRound))
                    return true;
            }
        }
    }
    return false;
}

static bool buildReadBehindTestsInTurnOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!walker.has(Mechanism::Folding) || !access.base || !access.base.isValue() || !access.typeOfKey.isInteger() || !walker.canRestartIteration() || walker.hasCallInSpecializedIteration() || walker.readStandsUnderHandler() || (walker.isInvariantInSpecializedVersion(access.base) && !looksCountOnTurnsOfLoopAroundInTypedElements(access) && someElementMeetsWhatGoesRoundInTypedElements(access)))
        return false;
    ArrayStorage storage = kindAskedInTurnOfTypedElements(access);
    if (!hasArrayStorageForm(storage))
        return false;
    bool admitsInt32Storage = storage == ArrayStorage::Double;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;

    Draft hasKind(Opcode::HasArrayStorage, origin);
    walker.chooseConservativeFields(hasKind);
    hasKind.setArrayOperand(access.base);
    hasKind.setArrayStorage(storage);
    hasKind.setAdmitsInt32Storage(admitsInt32Storage);
    if (!continueOrRestartIteration(walker, walker.build(WTF::move(hasKind))))
        return true;

    Draft isPresent(Opcode::IsElementPresent, origin);
    walker.chooseConservativeFields(isPresent);
    isPresent.setArrayOperand(access.base);
    isPresent.setPositionOperand(access.key);
    isPresent.setArrayStorage(storage);
    isPresent.setAdmitsInt32Storage(admitsInt32Storage);
    isPresent.setKeyIsIntegral(true);
    if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(access.base, access.key))
        isPresent.setIsInBounds(*inside);
    Reference test = walker.build(WTF::move(isPresent));
    if (!test)
        return true;
    BlockID found = walker.addBlock();
    BlockID absent = walker.addBlock(BlockFrequency::Rare);
    walker.branchOnTest(test, found, absent);
    walker.continueIn(absent);
    if (!walker.isUnreachable())
        jumpToGuardFallback(walker);
    walker.continueIn(found);
    walker.recordSpeculation();
    std::optional<PassedTest> present = walker.isUnreachable() || walker.hasFailed() ? std::nullopt : walker.showElementPresentBehindTest(access.base, access.key, walker.findPassedTest(test));
    if (!present)
        return true;

    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
    read.setArrayOperand(access.base);
    read.setPositionOperand(access.key);
    read.setArrayStorage(storage);
    read.setAdmitsInt32Storage(admitsInt32Storage);
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(true);
    read.setIsPresent(*present);
    Reference element = walker.build(WTF::move(read));
    if (element && isArrayIndexInTypedElements(access.typeOfKey) && !freshObjectNamedInTypedElements(walker, access.base)) {
        walker.state().noteElement(access.base, access.key, element);
        walker.state().noteElementsLieOutsideViews(access.base);
    }
    walker.bind(access.value.id(), element);
    walker.count(StatisticID::ElementsGuardedInIterations);
    return true;
}

static Reference mostBelowWrittenBoundInTypedElements(Walker& walker, Reference position, Origin origin)
{
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    Type type = walker.types().at(walker.typeOf(position));
    if (!position.isValue() || !type.isInteger() || type.integers.minimum < 0)
        return { };
    const Code& code = walker.code();
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    bool changesFromTurnToTurn = !walker.isInvariantInSpecializedVersion(position);
    ValueID held = position.id();
    while (conversionsOfForm.contains(code.at(held).opcode) || (changesFromTurnToTurn && sums.contains(code.at(held).opcode) && code.isConstant(code.at(held).operand(1))))
        held = code.at(held).operand(0);
    const Operation& narrowed = code.at(held);
    if (narrowed.opcode != Opcode::Narrowed || !narrowed.isOnTakenEdge() || narrowed.operandIndex() != OperandIndex::Less::left)
        return { };
    const Operation& relation = code.at(narrowed.testOperand());
    if (!relations.contains(relation.opcode) || !code.isConstant(relation.operand(OperandIndex::Less::right)))
        return { };
    Draft most(Opcode::Integer, origin);
    most.setInteger(type.integers.maximum);
    return walker.constant(WTF::move(most));
}

static void hoistArrayBoundsCheck(const AccessOfTypedElements& access, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (tested.isWritable || tested.turnsStoreIntoLists || !access.typeOfKey.isInteger() || !walker.isInvariantInSpecializedVersion(access.base, purity) || !walker.iterationsPreserveArrayGuards(access.base, purity))
        return;
    if (walker.findWhatHoldsPositionInsideList(access.base, access.key) || walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key))
        return;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference most = mostBelowWrittenBoundInTypedElements(walker, access.key, origin);
    if (!most)
        return;
    Reference length;
    Reference isBelow;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        length = buildArrayLength(walker, access.base, origin);
        if (!length || !length.isValue())
            return;
        Draft test(Opcode::Less, origin);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(most);
        test.setRightOperand(length);
        test.setForm(operatorFormFor(walker, test));
        isBelow = walker.build(WTF::move(test));
    }
    if (!isBelow || !isBelow.isValue() || !walker.tryHoistConditionalCheck(isBelow, true, purity))
        return;
    Reference held;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(length);
        narrowed.setTestOperand(isBelow);
        narrowed.setOperandIndex(OperandIndex::Less::right);
        narrowed.setIsOnTakenEdge(true);
        held = walker.build(WTF::move(narrowed));
    }
    if (!held || !held.isValue() || held == length)
        return;
    noteKnown(walker, KnownNote::LengthOfList, access.base, { }, 0, placesUnderLengthsOfLists, held.bits());
    walker.recordSpeculation();
    walker.count(StatisticID::ArrayBoundsChecksHoisted);
}

static constexpr AbstractHeapSet heapsUnderElementOfFreshListInTypedElements {
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
static_assert(perObjectHeaps.containsAll(heapsUnderElementOfFreshListInTypedElements));

static bool keyGoesRoundLoopInTypedElements(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    ValueID key = index.stripCasts(access.value.propertyOperand());
    for (OpcodeSet made : { OpcodeSet { Opcode::Increment, Opcode::Decrement }, OpcodeSet { Opcode::ToNumeric } }) {
        if (key && !index.isParameter(key) && made.contains(index.opcodeOf(key)))
            key = index.stripCasts(index.value(key).operand(0));
    }
    return key && index.isParameter(key) && index.isLoopHead(index.blockOf(key));
}

static std::optional<AccessOfTypedElements> tryGuardIntegerKey(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (access.isOnlyAskedAbout || !access.key || !access.key.isValue() || !access.typeOfKey.isNumber() || access.typeOfKey.isNumberWithoutFraction())
        return std::nullopt;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.canRestartIteration() || walker.readStandsUnderHandler())
        return std::nullopt;
    if (keyGoesRoundLoopInTypedElements(access) || !isDeclaredToHoldNumbersAlone(walker.program(), walker.types(), access.typeOfBase))
        return std::nullopt;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference isWhole = buildTestOfNumber(walker, access.key, TypeTable::int32, origin);
    if (!isWhole || !isWhole.isValue() || !continueOrRestartIteration(walker, isWhole))
        return std::nullopt;
    Reference whole = buildProofOfNumber(walker, access.key, isWhole, origin);
    if (!whole || !whole.isValue() || !walker.types().at(walker.typeOf(whole)).isInteger())
        return std::nullopt;
    walker.count(StatisticID::NumberKeysGuardedInIterations);
    return AccessOfTypedElements { walker, access.value, access.base, whole, access.stored, access.typeOfBase, walker.types().at(walker.typeOf(whole)), access.typeOfStored, false, access.counterThatPushGoesBy };
}

static bool buildArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!access.isOnlyAskedAbout && bindNumberReadBehindTestsOfTypedElements(access))
        return true;
    bool walkStoodInFront = !walker.isUnreachable() && !walker.hasFailed();
    if (std::optional<AccessOfTypedElements> held = tryGuardIntegerKey(access))
        return buildArrayReadOfTypedElements(*held);
    if (walkStoodInFront && (walker.isUnreachable() || walker.hasFailed()))
        return true;
    auto [level, restsOnLinksWordAlone] = levelOfArrayReadOfTypedElements(access);
    std::optional<PassedTest> presentAtHead = level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout ? walker.tryGuardArrayAtHeader(access.value) : std::nullopt;
    std::optional<HoistedArrayGuard> tested = level == KnowledgeLevel::Unknown ? std::nullopt : findHoistedArrayGuard(walker, access.value);
    if (!tested && level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout)
        tested = tryHoistArrayGuardForAccess(walker, access.value, access.base);
    if (!tested && level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout)
        tested = tryGuardNumericArrayInIteration(walker, access.value, access.base);
    if (!tested && level == KnowledgeLevel::Expected && !access.isOnlyAskedAbout && buildReadBehindTestsInTurnOfTypedElements(access))
        return true;
    if (!tested && level == KnowledgeLevel::Expected && access.isOnlyAskedAbout && walker.purchaseAsksWhetherTurnsCanBeQuiet() && walker.looksTakeFormsBehindLooksAsBuilt() && access.typeOfKey.isInteger() && hasArrayStorageForm(kindAskedInTurnOfTypedElements(access)))
        level = KnowledgeLevel::Proven;
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
    read.setAsksCellWhereNothingIsFound(restsOnLinksWordAlone && !tested);
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (tested && !lengthOfListMadeInSight(walker, access.base))
        hoistArrayBoundsCheck(access, *tested);
    std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    if (!inside && tested && access.typeOfKey.isInteger())
        inside = walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key);
    if (inside)
        read.setIsInBounds(*inside);
    bool namesStorage = !tested && level == KnowledgeLevel::Proven && !restsOnLinksWordAlone && access.typeOfKey.isInteger() && nameStorageOfListMadeInSight(walker, read);
    if (!tested && !namesStorage && !hasArrayStorageForm(storage))
        read.representation = formOfNumberThatLinkHasTypedOfTypedElements(access, level);
    if (tested)
        nameKindOfListTestedInFrontOfLoop(read, *tested);
    Reference freshListReadUnderKey = namesStorage && access.key.isValue() && isArrayIndexInTypedElements(access.typeOfKey) ? freshObjectNamedInTypedElements(walker, access.base) : Reference();
    bool elementIsKnownToBeThere = read.isPresent();
    if (std::optional<uint64_t> readBefore = freshListReadUnderKey ? findKnown(walker, KnownNote::ElementReadOffFreshList, freshListReadUnderKey, access.key) : std::nullopt) {
        Reference known = Reference::fromBits(static_cast<uint32_t>(*readBefore));
        if (walker.checksProofs()) {
            WalkState believed = walker.state();
            walker.checkThatMemoryAnswersAsBelieved(known, WTF::move(read));
            walker.state() = WTF::move(believed);
        }
        walker.count(StatisticID::FreshArrayElementReadsEliminated);
        walker.bind(access.value.id(), numberMadeOnceOfElementInTypedElements(walker, access.value, known));
        return true;
    }
    if (std::optional<PassedTest> insideAsWritten = tested && access.typeOfKey.isInteger() ? walker.findWhatHoldsPositionInsideListMadeInSight(access.base, access.key) : std::nullopt) {
        read.testsPassed = std::nullopt;
        read.setIsInBounds(*insideAsWritten);
        read.setIsPresent(*insideAsWritten);
    }
    bool takesNaNForHole = tested && read.isInBounds() && read.arrayStorage() == ArrayStorage::Double && !read.admitsInt32Storage() && everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing);
    if (!takesNaNForHole && tested && tested->turnsStoreIntoLists && !tested->isWritable && isNumericStorage(read.arrayStorage()) && onlyUserHoldsItAgainstNumberInTypedElements(walker, access.value))
        takesNaNForHole = true;
    if (takesNaNForHole && tested->turnsStoreIntoLists)
        walker.count(StatisticID::StoringLoopHoleChecksRemoved);
    bool leavesTurnAtHole = !presentAtHead && !takesNaNForHole && tested && canGuardAccess(access) && holeLeavesTurnBehindRow(walker, access.value, *tested);
    bool findsNothingOutsideList = read.isInBounds() || isArrayIndexInTypedElements(access.typeOfKey);
    if (!leavesTurnAtHole && tested && findsNothingOutsideList && isNumericStorage(read.arrayStorage()) && everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing))
        read.setReadsHoleAsNaN(true);
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    bool takesNumber = !isNumericStorage(read.arrayStorage()) || read.readsHoleAsNaN() || leavesTurnAtHole;
    std::optional<PassedTest> presentInFront = tested && takesNumber && read.keyIsIntegral() && !presentAtHead ? tryHoistElementPresenceCheck(walker, access.value, *tested, access.base, access.key) : std::nullopt;
    if (tested && presentAtHead) {
        read.testsPassed = std::nullopt;
        read.setReadsHoleAsNaN(false);
        read.setIsInBounds(*presentAtHead);
        read.setIsPresent(*presentAtHead);
    }
    if (presentInFront && read.readsHoleAsNaN())
        read.setReadsHoleAsNaN(false);
    bool givesNaNForHole = read.readsHoleAsNaN();
    Reference element = leavesTurnAtHole ? walker.state().element(access.base, access.key) : Reference();
    bool isReadInFront = !!presentInFront;
    if (presentInFront) {
        if (std::exchange(read.testsPassed, std::nullopt))
            read.setIsInBounds(*presentInFront);
        read.setIsPresent(*presentInFront);
    }
    if (tested && !isReadInFront)
        takeStorageAndLengthFromInFrontOfLoop(walker, read, *tested);
    std::optional<Draft> readAgain;
    if (givesNaNForHole && !isReadInFront) {
        element = walker.state().elementWithNaNForHole(access.base, access.key);
        if (element) {
            walker.count(StatisticID::NumericElementReadsEliminated);
            walker.noteReadAnsweredByHoleReadAsNaN(access.value.id(), element);
        }
    }
    if (!element || !walker.types().at(walker.typeOf(element)).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined)) {
        if (leavesTurnAtHole && !isReadInFront && !askElementInTurnOfTypedElements(access, read, *tested))
            return true;
        if (isReadInFront && walker.checksBounds())
            readAgain = read;
        Walker::SpecializedPreheaderScope preheaderScope(walker, isReadInFront, walker.arrayGuardPurity());
        WhatListWasMadeWith held = namesStorage && read.isPresent() && read.arrayStorage() == ArrayStorage::Contiguous ? whatListWasMadeWith(walker, access.base) : WhatListWasMadeWith();
        element = held.proven ? walker.buildReadTypedByWhatIsRemembered(WTF::move(read), held.proven, held.shown) : walker.build(WTF::move(read));
    }
    if (readAgain && element && element.isValue()) {
        WalkState believed = walker.state();
        walker.checkThatMemoryAnswersAsBelieved(element, WTF::move(*readAgain));
        walker.state() = WTF::move(believed);
    }
    if (leavesTurnAtHole && element && walker.types().at(walker.typeOf(element)).primitives & Primitive::Undefined) {
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        Reference isNumber = buildTestOfNumber(walker, element, TypeTable::number, origin);
        if (!continueOrRestartIteration(walker, isNumber))
            return true;
        element = buildProofOfNumber(walker, element, isNumber, origin);
    }
    if (element && level == KnowledgeLevel::Proven && isArrayIndexInTypedElements(access.typeOfKey) && !freshObjectNamedInTypedElements(walker, access.base)) {
        if (givesNaNForHole)
            walker.state().noteElementWithNaNForHole(access.base, access.key, element);
        else
            walker.state().noteElement(access.base, access.key, element);
        walker.state().noteElementsLieOutsideViews(access.base);
    }
    if (freshListReadUnderKey && element && element.isValue())
        noteKnown(walker, KnownNote::ElementReadOffFreshList, freshListReadUnderKey, access.key, 0, elementIsKnownToBeThere ? LocationMasks(heapsUnderElementOfFreshListInTypedElements) : LocationMasks::top().without(unobservableWriteHeaps), element.bits());
    walker.bind(access.value.id(), numberMadeOnceOfElementInTypedElements(walker, access.value, element));
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
    if (whatIsStoredIsWrittenToBeNoNumberInTypedElements(access))
        askBirthOfListForStorage(walker, access.base, ArrayStorage::Contiguous);
    askBirthOfListForStorageThatTakes(walker, access.base, access.stored);
    markBaseAsPassedInTypedElements(walker, access.base);
    Reference store = walker.build(WTF::move(put));
    noteStoredElementInTypedElements(walker, access.base, access.key, { });
    walker.bind(access.value.id(), store);
    return true;
}

static bool keyIsLengthOfListItselfInTypedElements(const AccessOfTypedElements& access)
{
    if (!access.key.isValue() || !access.base.isValue())
        return false;
    const Code& code = access.walker.code();
    const Operation& key = code.at(underlyingCellOf(code, access.key.id()));
    return key.opcode == Opcode::GetArrayLength && underlyingCellOf(code, key.arrayOperand()) == underlyingCellOf(code, access.base.id());
}

static bool isReadOutOfDoubleStorageInTypedElements(Walker& walker, Reference stored)
{
    if (!stored || !stored.isValue())
        return false;
    const Code& code = walker.code();
    auto behindNames = [&](ValueID held) {
        while (valuePreservingOpcodes.contains(code.at(held).opcode))
            held = code.at(held).operand(0);
        return held;
    };
    auto isSuchRead = [&](ValueID held) {
        const Operation& read = code.at(behindNames(held));
        return read.opcode == Opcode::GetArrayElement && read.arrayStorage() == ArrayStorage::Double && read.level() == KnowledgeLevel::Proven && read.isPresent() && !read.readsHoleAsNaN();
    };
    ValueID held = behindNames(stored.id());
    const Operation& chosen = code.at(held);
    if (chosen.opcode != Opcode::Parameter)
        return isSuchRead(held);
    BlockID join = chosen.block();
    if (!join || code.at(join).isLoopHead || code.at(join).isEntry || !code.at(join).firstEdgeIn())
        return false;
    std::span<const ValueID> parameters = code.at(join).parameters();
    size_t position = std::ranges::find(parameters, held) - parameters.begin();
    bool everyArmIsSuchRead = position < parameters.size();
    code.forEachIncomingEdgeUnsealed(join, [&](const Edge& edge) {
        everyArmIsSuchRead = everyArmIsSuchRead && position < edge.arguments().size() && isSuchRead(edge.arguments()[position]);
    });
    return everyArmIsSuchRead;
}

static bool askKeyOfLaterStoresOfTypedElements(const AccessOfTypedElements& access, Origin origin)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    const BodyValue& first = access.value;
    if (first.opcode() != Opcode::ElemSet || first.id().index() >= index.numberOfValues() || index.value(first.id()).words() != first.words())
        return true;
    for (auto [blockAndKey, numberOfLater] : walker.laterStoresUnderSameKeyInBlockInCurrentBody(first)) {
        BodyValue later = index.value(ValueID(numberOfLater));
        Reference list = nameOfListThatEntersLoopAround(walker, index, later);
        std::optional<HoistedArrayGuard> tested = list && list != access.base ? findListTestedForStoresInFrontOfLoop(walker, list) : std::nullopt;
        if (!tested || !tested->isWritable || positionIsKnownToLieInsideList(walker, list, access.key))
            continue;
        Draft length(Opcode::GetArrayLength, origin);
        walker.chooseConservativeFields(length);
        length.setArrayOperand(tested->name);
        Reference held = walker.build(WTF::move(length));
        if (!held)
            return false;
        Draft lengthComparison(Opcode::Less, origin);
        walker.chooseConservativeFields(lengthComparison);
        lengthComparison.setLeftOperand(access.key);
        lengthComparison.setRightOperand(held);
        lengthComparison.setForm(operatorFormFor(walker, lengthComparison));
        Reference test = walker.build(WTF::move(lengthComparison));
        if (!test || !test.isValue())
            return false;
        if (!continueOrRestartIteration(walker, test, true))
            return false;
        noteKnown(walker, KnownNote::LengthOfList, list, { }, 0, placesUnderLengthsOfLists, held.bits());
        walker.count(StatisticID::LaterStoreBoundsChecksHoisted);
    }
    return true;
}

static bool textOfLoopsReadsListByKeyInTypedElements(const BodyIndex& index, const BodyValue& store)
{
    LoopID outermost = index.loopOf(index.blockOf(store.id()));
    if (!outermost)
        return true;
    outermost = index.loop(outermost).outermost;
    ValueID list = index.stripCasts(store.objectOperand());
    for (BlockID member : index.membersOf(outermost)) {
        for (BodyValue value : index.block(member).values()) {
            if (value.opcode() == Opcode::ElemGet && index.stripCasts(value.objectOperand()) == list)
                return true;
        }
    }
    return false;
}

static bool turnStoresAgainBehindInTypedElements(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return true;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    return !loop || access.walker.lastStoreByKeyOfLoopInCurrentBody(loop) > access.value.id().index();
}

uint32_t Walker::lastStoreByKeyOfLoopInCurrentBody(LoopID loop)
{
    static constexpr uint32_t notAsked = std::numeric_limits<uint32_t>::max();
    const BodyIndex& index = currentBodyIndex();
    Vector<uint32_t>& last = m_lastStoresByKeyOfLoops.ensure(&index, [&] {
        Vector<uint32_t> none;
        none.fill(notAsked, index.numberOfLoops());
        return none;
    }).iterator->value;
    uint32_t& answer = last[loop.index()];
    if (answer != notAsked)
        return answer;
    answer = 0;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemSet)
                answer = std::max(answer, operation.id().index());
        }
    }
    return answer;
}

std::span<const std::pair<uint64_t, uint32_t>> Walker::laterStoresUnderSameKeyInBlockInCurrentBody(const BodyValue& first)
{
    const BodyIndex& index = currentBodyIndex();
    auto blockAndKeyOf = [&](const BodyValue& store) {
        ValueID key = index.stripCasts(store.propertyOperand());
        return static_cast<uint64_t>(index.blockOf(store.id()).index()) << 32 | (key ? key.index() : std::numeric_limits<uint32_t>::max());
    };
    const Vector<std::pair<uint64_t, uint32_t>>& stores = m_storesByKeyOfBlocks.ensure(&index, [&] {
        Vector<std::pair<uint64_t, uint32_t>> found;
        if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemSet }))
            return found;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            for (BodyValue operation : index.block(BlockID(block)).values()) {
                if (operation.opcode() == Opcode::ElemSet)
                    found.append({ blockAndKeyOf(operation), operation.id().index() });
            }
        }
        std::ranges::sort(found);
        return found;
    }).iterator->value;
    uint64_t asked = blockAndKeyOf(first);
    auto begin = std::ranges::upper_bound(stores, std::pair<uint64_t, uint32_t> { asked, first.id().index() });
    auto end = std::ranges::upper_bound(stores, std::pair<uint64_t, uint32_t> { asked, std::numeric_limits<uint32_t>::max() });
    return stores.span().subspan(begin - stores.begin(), end - begin);
}

static_assert(declaredBoundOf(Opcode::PutContiguousIndex).writes.isSubsetOf(declaredBoundOf(Opcode::ExtendArrayLength).writes) && declaredBoundOf(Opcode::PutDoubleIndex).writes.isSubsetOf(declaredBoundOf(Opcode::ExtendArrayLength).writes));
static_assert(declaredBoundOf(Opcode::PutContiguousIndex).reads.isSubsetOf(declaredBoundOf(Opcode::ExtendArrayLength).reads) && declaredBoundOf(Opcode::PutDoubleIndex).reads.isSubsetOf(declaredBoundOf(Opcode::ExtendArrayLength).reads));

static bool literalMayHoldStorageAskedInTypedElements(Walker& walker, const HoistedArrayGuard& tested)
{
    const Operation& made = walker.code().at(underlyingCellOf(walker.code(), tested.name.id()));
    ArrayStorage born = made.opcode == Opcode::NewArray ? storageThatLiteralIsSurelyBornWith(walker.program(), walker.types(), walker.code(), made) : ArrayStorage::Other;
    return !hasArrayStorageForm(born) || !hasArrayStorageForm(tested.storage) || born == tested.storage || (born == ArrayStorage::Int32 && tested.storage == ArrayStorage::Double);
}

static std::optional<uint32_t> notedCounterPosition(const AccessOfTypedElements& access, KnownNote note, Reference list)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return std::nullopt;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    if (!loop)
        return std::nullopt;
    BodyBlock head = index.block(index.loop(loop).head);
    std::optional<uint64_t> noted = findKnown(access.walker, note, list, { }, head.id().index());
    if (!noted || *noted >> 32 != access.walker.currentFunction().index())
        return std::nullopt;
    uint32_t place = static_cast<uint32_t>(*noted);
    ValueID key = access.counterThatPushGoesBy ? access.counterThatPushGoesBy : index.stripCasts(access.value.propertyOperand());
    return place <= head.numberOfParameters() && key == head.parameter(place - 1) ? std::optional(place) : std::nullopt;
}

static bool isAccessInLoopHeader(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    BlockID block = index.blockOf(access.value.id());
    LoopID loop = index.loopOf(block);
    return !loop || index.loop(loop).head == block;
}

static std::optional<PassedTest> findHoistedCapacityProof(const AccessOfTypedElements& access, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    std::optional<uint64_t> noted = loop ? findKnown(walker, KnownNote::LoopCapacityGuard, tested.name, { }, index.loop(loop).head.index()) : std::nullopt;
    std::optional<uint64_t> filledTo = noted ? findKnown(walker, KnownNote::FinalLiteralLength, tested.name, { }, index.loop(loop).head.index()) : std::nullopt;
    Reference loopCapacityGuard = filledTo ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
    if (!loopCapacityGuard || !loopCapacityGuard.isValue() || walker.code().at(loopCapacityGuard.id()).opcode != Opcode::LessEq || !access.base.isRecord() || !access.key.isRecord())
        return std::nullopt;
    std::optional<PassedTest> below = walker.findUpperBoundProof(access.key, Reference::fromBits(static_cast<uint32_t>(*filledTo)));
    if (BodyTerminator testOfHead = index.terminator(index.loop(loop).head); !below && testOfHead.isConditionalBranch())
        below = walker.findPassedTest(walker.referenceOf(testOfHead.operand(0)));
    if (!below)
        return std::nullopt;
    GroundsOfPosition grounds { .view = access.base.id(), .position = access.key.id(), .length = walker.code().at(loopCapacityGuard.id()).rightOperand(), .limitWithinLengthCheck = loopCapacityGuard.id() };
    if (!below->sides().empty()) {
        grounds.isBelow = below->sides().front().test;
        grounds.isBelowResult = below->sides().front().testResult;
    }
    return walker.showPositionInside(grounds);
}

static bool buildLengthExtension(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, Origin origin, std::optional<PassedTest>& newBoundsProof)
{
    Walker& walker = access.walker;
    std::optional<PassedTest> capacityProof;
    if (!walker.shouldSkipCapacityChecks() && notedCounterPosition(access, KnownNote::CapacityCoversLoop, tested.name) && !isAccessInLoopHeader(access))
        capacityProof = findHoistedCapacityProof(access, tested);
    if (capacityProof)
        walker.count(StatisticID::AppendsUsingLoopCapacityGuards);
    if (!capacityProof && !walker.shouldSkipCapacityChecks()) {
        Draft room(Opcode::GetArrayCapacity, origin);
        walker.chooseConservativeFields(room);
        room.setArrayOperand(tested.name);
        Draft capacityComparison(Opcode::Less, origin);
        walker.chooseConservativeFields(capacityComparison);
        capacityComparison.setLeftOperand(access.key);
        capacityComparison.setRightOperand(walker.build(WTF::move(room)));
        if (!capacityComparison.rightOperand())
            return false;
        capacityComparison.setForm(operatorFormFor(walker, capacityComparison));
        Reference currentCapacity = capacityComparison.rightOperand();
        Reference isKeyWithinCapacity = walker.build(WTF::move(capacityComparison));
        if (!continueOrRestartIteration(walker, isKeyWithinCapacity, true))
            return false;
        capacityProof = walker.boundsProofAfterComparison(access.base, access.key, currentCapacity, isKeyWithinCapacity);
    }
    Draft extend(Opcode::ExtendArrayLength, origin);
    walker.chooseConservativeFields(extend);
    extend.setArrayOperand(access.base);
    extend.setPositionOperand(access.key);
    if (capacityProof)
        extend.restOn(*capacityProof);
    if (Reference pointer = storageOfListFromInFrontOfLoop(walker, tested, origin))
        extend.setStorageOperand(pointer);
    newBoundsProof = walker.boundsProofAfterExtension(access.base, access.key, walker.build(WTF::move(extend)));
    walker.count(StatisticID::IndexedStoresCompiledAsAppends);
    return !walker.isUnreachable() && !walker.hasFailed();
}

static bool buildStoreBehindRowOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (!access.typeOfKey.isInteger() || !access.typeOfStored || !types.at(access.typeOfStored).isNumber())
        return false;
    bool isPush = !!access.counterThatPushGoesBy;
    std::optional<HoistedArrayGuard> tested = isPush ? findListTestedForStoresInFrontOfLoop(walker, access.base) : findHoistedArrayGuard(walker, access.value);
    bool rowAsksForWritableList = tested && tested->isWritable;
    if (isPush && !rowAsksForWritableList)
        return false;
    if (!rowAsksForWritableList && !access.isOnlyAskedAbout) {
        ArrayStorage asked = types.at(access.typeOfStored).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Int32;
        bool mayGoByKind = !tested && (asked == ArrayStorage::Double || types.isSubset(access.typeOfStored, TypeTable::int32)) && walker.isInvariantInSpecializedVersion(access.base) && !textOfLoopsReadsListByKeyInTypedElements(walker.currentBodyIndex(), access.value);
        tested = tryHoistArrayGuardForAccess(walker, access.value, access.base, asked, mayGoByKind);
    }
    bool goesByKind = tested && tested->isWritable && tested->storage == ArrayStorage::Other;
    if (goesByKind && tested->admitsInt32Storage && !types.isSubset(access.typeOfStored, TypeTable::int32))
        return false;
    if (!tested || !tested->isWritable || (!goesByKind && (!isNumericStorage(tested->storage) || tested->admitsInt32Storage)))
        return false;

    std::optional<PassedTest> inside = access.isOnlyAskedAbout ? std::nullopt : walker.findWhatHoldsPositionInsideList(access.base, access.key);
    bool isInside = !!inside;
    if (!isInside && !isArrayIndexInTypedElements(access.typeOfKey) && (!rowAsksForWritableList || keyIsLengthOfListItselfInTypedElements(access)))
        return false;
    bool fits = canStoreWithoutConversion(types, goesByKind ? ArrayStorage::Double : tested->storage, access.typeOfStored);
    bool isMovedOutOfDoubleStorage = !fits && (goesByKind || tested->storage == ArrayStorage::Double) && isReadOutOfDoubleStorageInTypedElements(walker, access.stored);
    if (isMovedOutOfDoubleStorage)
        fits = true;
    if (walker.shouldAssumeNoNaNStores() && (goesByKind || tested->storage == ArrayStorage::Double) && types.at(access.typeOfStored).isNumber())
        fits = true;
    if ((!isInside || !fits) && !canGuardAccess(access))
        return false;
    Opcode opcode = tested->storage == ArrayStorage::Double ? Opcode::PutDoubleIndex : Opcode::PutContiguousIndex;
    bool appendsToLiteral = !isInside && notedCounterPosition(access, KnownNote::LoopAppendsToLiteral, tested->name) && literalMayHoldStorageAskedInTypedElements(walker, *tested);
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, appendsToLiteral ? Opcode::ExtendArrayLength : opcode, KnowledgeLevel::Proven);

    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference isKeyNonNegative;
    if (!isInside && access.typeOfKey.integers.minimum < 0) {
        Draft zero(Opcode::Integer, origin);
        zero.setInteger(0);
        Draft isNotNegative(Opcode::GreaterEq, origin);
        walker.chooseConservativeFields(isNotNegative);
        isNotNegative.setLeftOperand(access.key);
        isNotNegative.setRightOperand(walker.constant(WTF::move(zero)));
        isNotNegative.setForm(operatorFormFor(walker, isNotNegative));
        isKeyNonNegative = walker.build(WTF::move(isNotNegative));
        if (!continueOrRestartIteration(walker, isKeyNonNegative, true))
            return true;
    }
    bool extendsLength = appendsToLiteral && !turnStoresAgainBehindInTypedElements(access) && walker.canRestartIteration() && notedCounterPosition(access, KnownNote::LengthEqualsInitialCounter, tested->name);
    if (isPush && !extendsLength)
        return false;
    if (!isInside && !extendsLength && !walker.shouldSkipLengthChecks()) {
        Draft length(Opcode::GetArrayLength, origin);
        walker.chooseConservativeFields(length);
        length.setArrayOperand(tested->name);
        Draft lengthComparison(Opcode::Less, origin);
        walker.chooseConservativeFields(lengthComparison);
        lengthComparison.setLeftOperand(access.key);
        lengthComparison.setRightOperand(walker.build(WTF::move(length)));
        if (!lengthComparison.rightOperand())
            return true;
        lengthComparison.setForm(operatorFormFor(walker, lengthComparison));
        Reference currentLength = lengthComparison.rightOperand();
        Reference isKeyWithinLength = walker.build(WTF::move(lengthComparison));
        if (!continueOrRestartIteration(walker, isKeyWithinLength, true))
            return true;
        std::optional<PassedTest> below = walker.findPassedTest(isKeyWithinLength);
        std::optional<PassedTest> fromZeroUp = isKeyNonNegative ? walker.findPassedTest(isKeyNonNegative) : walker.findNonNegativeProof(access.key);
        if (below && fromZeroUp)
            inside = walker.boundsProofFromChecks(access.base, access.key, currentLength, *fromZeroUp, *below);
    }
    Reference stored = access.stored;
    if (!fits && tested->storage == ArrayStorage::Int32) {
        Reference isInt32 = buildTestOfNumber(walker, stored, TypeTable::int32, origin);
        if (!continueOrRestartIteration(walker, isInt32))
            return true;
        stored = buildProofOfNumber(walker, stored, isInt32, origin);
        if (stored && stored.isValue() && access.stored.isValue() && stored != access.stored && !walker.state().checkedNameOf(access.stored)) {
            walker.state().noteCheckedName(access.stored, stored);
            walker.count(StatisticID::StoredValuesNarrowedToInt32);
        }
    } else if (!fits) {
        Draft isItself(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(isItself);
        isItself.setLeftOperand(stored);
        isItself.setRightOperand(stored);
        isItself.setForm(operatorFormFor(walker, isItself));
        if (!continueOrRestartIteration(walker, walker.build(WTF::move(isItself))))
            return true;
    }
    if (!stored)
        return true;
    if (!walker.standsInStretchWithArmsAsWritten() && canGuardAccess(access) && !askKeyOfLaterStoresOfTypedElements(access, origin))
        return true;

    if (extendsLength && !buildLengthExtension(access, *tested, origin, inside))
        return true;
    Reference valueBeforeStore;
    if (opcode == Opcode::PutDoubleIndex && !goesByKind && walker.canRecordUndoableStore() && turnStoresAgainBehindInTypedElements(access)) {
        valueBeforeStore = walker.state().element(access.base, access.key);
        if (!valueBeforeStore || !types.at(walker.typeOf(valueBeforeStore)).isNumber()) {
            Draft read(Opcode::GetOwnDoubleIndex, origin);
            walker.chooseConservativeFields(read);
            read.setArrayOperand(access.base);
            read.setPositionOperand(access.key);
            if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
                read.setStorageOperand(pointer);
            valueBeforeStore = walker.build(WTF::move(read));
            if (!valueBeforeStore)
                return true;
        }
    }

    Draft put = isPush ? Draft(opcode, origin) : draftOfTypedElements(walker, access.value, opcode);
    if (isPush) {
        walker.chooseConservativeFields(put);
        walker.count(StatisticID::PushesCompiledAsAppends);
    }
    put.setArrayOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(stored);
    if (inside)
        put.setIsInBounds(*inside);
    if (goesByKind)
        put.setGoesByKindOfStorage(true);
    if (valueBeforeStore && walker.canRecordUndoableStore())
        walker.recordUndoableElementStore(access.base, access.key, valueBeforeStore);
    if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
        put.setStorageOperand(pointer);
    if (isMovedOutOfDoubleStorage)
        walker.count(StatisticID::DoubleElementsCopiedUnchecked);
    Reference store = walker.build(WTF::move(put));
    walker.takePendingUndoableStore();
    noteStoredElementInTypedElements(walker, access.base, access.key, stored);
    walker.bind(access.value.id(), store);
    walker.letRestOfStretchRunAsWrittenBehindFirstStore(access.value.id());
    return true;
}
static bool storesUnderKnownIndexIntoFreshListInTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (access.isOnlyAskedAbout || !access.base || !access.key || !access.key.isConstant() || !isFreshPristineArray(walker, access.base) || !isArrayPrototypeChainSane(walker))
        return false;
    const Code& code = walker.code();
    const Operation& key = code.at(access.key.id());
    std::optional<uint32_t> length = lengthOfFreshList(walker, access.base);
    std::optional<uint64_t> lengthAsMade = lengthThatListIsMadeWith(code, code.at(underlyingCellOf(code, access.base.id())));
    return length && lengthAsMade && key.opcode == Opcode::Integer && key.integer() >= 0 && key.integer() < static_cast<int64_t>(UINT32_MAX);
}

static bool buildArrayStoreOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (buildStoreBehindRowOfTypedElements(access))
        return true;
    KnowledgeLevel level = levelOfArrayAccessOfTypedElements(access);
    bool storesIntoFreshList = level == KnowledgeLevel::Expected && storesUnderKnownIndexIntoFreshListInTypedElements(access);
    if (storesIntoFreshList)
        level = KnowledgeLevel::Proven;
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
    put.setFillsPlaceOfListMadeHere(storesIntoFreshList);
    put.setMode(mode);
    put.setLevel(level);
    put.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (std::optional<PassedTest> inside = level == KnowledgeLevel::Expected ? walker.findWhatHoldsPositionInsideList(access.base, access.key) : std::nullopt)
        put.setIsInBounds(*inside);
    if (whatIsStoredIsWrittenToBeNoNumberInTypedElements(access))
        askBirthOfListForStorage(walker, access.base, ArrayStorage::Contiguous);
    askBirthOfListForStorageThatTakes(walker, access.base, access.stored);
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    Reference store = walker.build(WTF::move(put));
    bool holdsWhatWasStored = storesIntoFreshList || (level == KnowledgeLevel::Proven && mode == ECMAMode::Strict && isArrayIndexInTypedElements(access.typeOfKey));
    noteStoredElementInTypedElements(walker, access.base, access.key, holdsWhatWasStored ? access.stored : Reference());
    walker.bind(access.value.id(), store);
    return true;
}

static bool buildArrayTestOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    auto [level, restsOnLinksWordAlone] = levelOfArrayReadOfTypedElements(access);
    if (level == KnowledgeLevel::Unknown)
        return false;
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::HasArrayElement, level);

    Draft test = draftOfTypedElements(walker, access.value, Opcode::HasArrayElement);
    test.setArrayOperand(access.base);
    test.setPositionOperand(access.key);
    test.setArrayStorage(level == KnowledgeLevel::Expected ? storageDeclaredForTypedElements(access) : ArrayStorage::Other);
    test.setLevel(level);
    test.setAsksCellWhereNothingIsFound(restsOnLinksWordAlone);
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
    isNamedAgain |= walker.standsInStretchWithArmsAsWritten() && walker.stretchWithArmsAsWritten().list == access.base;
    return isNamedAgain ? takeLength() : Reference();
}

static void nameStorageOfViewInTypedElements(const AccessOfTypedElements& written, Draft& access, TypedArrayType typedArrayType)
{
    Walker& walker = written.walker;
    Reference view = written.base;
    if (access.level() != KnowledgeLevel::Proven || !readsElementsInLineInTypedElements(typedArrayType))
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

static bool viewIsUsedElsewhereInTypedElements(Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    bool isOfThisBody = access.id().index() < index.numberOfValues() && index.value(access.id()).words() == access.words();
    return isOfThisBody && !index.hasOneUse(index.stripCasts(access.objectOperand()));
}

static Reference mostOfSmallRangeInTypedElements(Walker& walker, Reference position, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(position));
    if (!position.isValue() || !type.isInteger() || type.integers.minimum < 0 || type.integers.maximum > mostOfRangeAskedWholeInTypedElements)
        return { };
    Draft most(Opcode::Integer, origin);
    most.setInteger(type.integers.maximum);
    return walker.constant(WTF::move(most));
}

static void noteLengthOfViewAboveWhatPassedInTypedElements(Walker& walker, Reference view, Reference length, Reference isBelow, Origin origin)
{
    if (!length.isValue() || !isBelow.isValue() || walker.isUnreachable())
        return;
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(length);
    narrowed.setTestOperand(isBelow);
    narrowed.setOperandIndex(OperandIndex::Less::right);
    narrowed.setIsOnTakenEdge(true);
    Reference held = walker.build(WTF::move(narrowed));
    if (held && held.isValue() && held != length)
        noteKnown(walker, KnownNote::LengthOfList, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), held.bits());
}

static std::optional<PassedTest> tryGuardIndexInBounds(const AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
{
    Walker& walker = access.walker;
    if ((!walker.canRestartIteration() && !walker.stretchMayRunAsWrittenInstead()) || !walker.opensGuesses() || !readsElementsInLineInTypedElements(typedArrayType))
        return std::nullopt;
    if (!access.typeOfKey.isInteger() || positionIsKnownToLieInsideList(walker, access.base, access.key))
        return std::nullopt;
    bool turnNeedsNumber = turnTakesWhatIsReadForKeyOrStoresItIntoViewInTypedElements(walker, access.value);
    std::optional<PassedTest> fromZeroUp = walker.findNonNegativeProof(access.key);
    if ((!fromZeroUp && !turnNeedsNumber) || (!turnNeedsNumber && !walker.stretchMayRunAsWrittenInstead() && !onlyUserTakesUndefinedForNaNInTypedElements(walker, access.value)))
        return std::nullopt;
    origin.isTailCall = false;
    Reference length = lengthOfViewInTypedElements(access, typedArrayType, origin);
    if (!length || walker.isUnreachable())
        return std::nullopt;
    auto continueOrFallBack = [&](Reference test) -> std::optional<PassedTest> {
        if (!test || !test.isValue())
            return std::nullopt;
        if (std::optional<PassedTest> known = walker.findPassedTest(test))
            return known;
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        std::optional<PassedTest> passes = walker.continueIfCheckPasses(test, failed);
        BlockID passed = walker.currentBlock();
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            jumpToGuardFallback(walker);
        walker.continueIn(passed);
        walker.recordSpeculation();
        return walker.isUnreachable() ? std::nullopt : passes;
    };
    Reference key = access.key;
    if (!fromZeroUp) {
        Draft zero(Opcode::Integer, origin);
        zero.setInteger(0);
        Draft isNotNegative(Opcode::GreaterEq, origin);
        walker.chooseConservativeFields(isNotNegative);
        isNotNegative.setLeftOperand(key);
        isNotNegative.setRightOperand(walker.constant(WTF::move(zero)));
        isNotNegative.setForm(operatorFormFor(walker, isNotNegative));
        Reference isFromZeroUp = walker.build(WTF::move(isNotNegative));
        BlockID whereSignIsAsked = walker.currentBlock();
        fromZeroUp = continueOrFallBack(isFromZeroUp);
        if (!fromZeroUp)
            return std::nullopt;
        if (walker.currentBlock() != whereSignIsAsked && key.isValue() && isFromZeroUp.isValue() && !walker.state().checkedNameOf(key)) {
            Draft narrowed(Opcode::Narrowed, origin);
            narrowed.setValueOperand(key);
            narrowed.setTestOperand(isFromZeroUp);
            narrowed.setOperandIndex(OperandIndex::GreaterEq::left);
            narrowed.setIsOnTakenEdge(true);
            Reference held = walker.build(WTF::move(narrowed));
            if (held && held.isValue() && held != key) {
                walker.state().noteCheckedName(key, held);
                key = held;
            }
        }
    }
    Reference most = mostOfSmallRangeInTypedElements(walker, key, origin);
    if (most && !viewIsUsedElsewhereInTypedElements(walker, access.value))
        most = { };
    Draft test(Opcode::Less, origin);
    walker.chooseConservativeFields(test);
    test.setLeftOperand(most ? most : key);
    test.setRightOperand(length);
    test.setForm(operatorFormFor(walker, test));
    Reference isBelow = walker.build(WTF::move(test));
    BlockID whereBoundIsAsked = walker.currentBlock();
    std::optional<PassedTest> below = continueOrFallBack(isBelow);
    if (!below)
        return std::nullopt;
    if (most && walker.currentBlock() != whereBoundIsAsked)
        noteLengthOfViewAboveWhatPassedInTypedElements(walker, access.base, length, isBelow, origin);
    return walker.boundsProofFromChecks(access.base, access.key, length, *fromZeroUp, *below);
}

static std::optional<PassedTest> holdPositionBelowLengthOrStopInTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !readsElementsInLineInTypedElements(typedArrayType) || !access.typeOfKey.isInteger() || walker.facts().linked().readsOffViewsOfWrittenLengths.isEmpty())
        return std::nullopt;
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words() || !linksWordCanBeHeldAgainst(walker, access.base))
        return std::nullopt;
    uint32_t least = walker.facts().linked().leastLengthOfViewsReadAt(walker.currentFunction(), access.value.id());
    std::optional<PassedTest> fromZeroUp = least ? walker.findNonNegativeProof(access.key) : std::nullopt;
    if (!fromZeroUp)
        return std::nullopt;
    origin.isTailCall = false;
    Draft written(Opcode::Integer, origin);
    written.setInteger(least);
    if (!walker.findUpperBoundProof(access.key, walker.constant(WTF::move(written))))
        return std::nullopt;
    Draft read(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(read);
    read.setViewOperand(access.base);
    read.setTypedArrayType(typedArrayType);
    Reference length = walker.build(WTF::move(read));
    if (!length || !length.isValue() || walker.isUnreachable())
        return std::nullopt;
    Draft test(Opcode::Less, origin);
    walker.chooseConservativeFields(test);
    Draft greatest(Opcode::Integer, origin);
    greatest.setInteger(least - 1);
    test.setLeftOperand(walker.constant(WTF::move(greatest)));
    test.setRightOperand(length);
    test.setForm(operatorFormFor(walker, test));
    Reference isBelow = walker.build(WTF::move(test));
    if (!isBelow || !isBelow.isValue())
        return std::nullopt;
    std::optional<PassedTest> below = walker.findPassedTest(isBelow);
    bool standsAtHeadOfBlock = !below;
    if (!below) {
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        below = walker.continueIfCheckPasses(isBelow, failed);
        BlockID passed = walker.currentBlock();
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.stopWhereWordOfLinkIsFalse(origin);
        walker.continueIn(passed);
    }
    if (!below || walker.isUnreachable())
        return std::nullopt;
    if (standsAtHeadOfBlock)
        noteLengthOfViewAboveWhatPassedInTypedElements(walker, access.base, length, isBelow, origin);
    return walker.boundsProofFromChecks(access.base, access.key, length, *fromZeroUp, *below);
}

static bool onlyUserTakesNumberInTypedElements(Walker& walker, const BodyValue& read)
{
    if (onlyUserTakesUndefinedForNaNInTypedElements(walker, read))
        return true;
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
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
    if (accessesByKey.contains(operation.opcode())) {
        if (operation.objectOperand() == name)
            return false;
        if (operation.propertyOperand() == name)
            return true;
        Reference view = operation.opcode() == Opcode::ElemSet ? walker.referenceOf(operation.objectOperand()) : Reference();
        if (!view || !view.isRecord())
            return false;
        BaseOfTypedElements base = classifyBaseOfTypedElements(walker, walker.typeOf(view));
        return base.kind == KindOfBaseOfTypedElements::TypedArray && readsElementsInLineInTypedElements(base.typedArrayType) && !isBigInt(base.typedArrayType);
    }
    return allBitwiseOperators.contains(operation.opcode());
}

TypeID typeOfElementInsideViewOfNumbers(Walker& walker, const BodyValue& read, TypeID typeOfView, TypeID typeOfPosition, bool readIsUpdated)
{
    if (!typeOfView || !typeOfPosition)
        return { };
    Type position = walker.types().at(typeOfPosition);
    if (!position.isInteger() || (position.integers.minimum < 0 && !readIsUpdated && !onlyUserTakesNumberInTypedElements(walker, read)))
        return { };
    BaseOfTypedElements base = classifyBaseOfTypedElements(walker, typeOfView);
    if (base.kind != KindOfBaseOfTypedElements::TypedArray || !readsElementsInLineInTypedElements(base.typedArrayType) || isBigInt(base.typedArrayType))
        return { };
    return elementTypeInTypedElements(base.typedArrayType);
}

bool isViewOfNumbersReadInLine(Walker& walker, TypeID typeOfView)
{
    if (!typeOfView)
        return false;
    BaseOfTypedElements base = classifyBaseOfTypedElements(walker, typeOfView);
    return base.kind == KindOfBaseOfTypedElements::TypedArray && readsElementsInLineInTypedElements(base.typedArrayType) && !isBigInt(base.typedArrayType);
}

Reference buildIntegerOperator(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    Draft draft(opcode, origin);
    walker.chooseConservativeFields(draft);
    draft.setLeftOperand(left);
    draft.setRightOperand(right);
    draft.setForm(operatorFormFor(walker, draft));
    return walker.build(WTF::move(draft));
}

Reference buildRoomOfViewBesideAddend(Walker& walker, Reference view, Reference addend, Origin origin)
{
    Draft length(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(view);
    length.setTypedArrayType(classifyBaseOfTypedElements(walker, walker.typeOf(view)).typedArrayType);
    Reference room = walker.build(WTF::move(length));
    if (room && addend && !walker.isUnreachable())
        room = buildIntegerOperator(walker, Opcode::Sub, room, addend, origin);
    return room && room.isRecord() && walker.types().at(walker.typeOf(room)).isInteger() ? room : Reference();
}

Reference buildArrayLength(Walker& walker, Reference list, Origin origin)
{
    Draft draft(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(draft);
    draft.setArrayOperand(list);
    Reference length = walker.build(WTF::move(draft));
    return length && length.isRecord() && walker.types().at(walker.typeOf(length)).isInteger() ? length : Reference();
}

static void hoistTypedArrayBoundsCheck(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    Walker& walker = access.walker;
    if (!walker.opensGuesses() || !readsElementsInLineInTypedElements(typedArrayType) || !access.typeOfKey.isInteger() || !walker.isInvariantInSpecializedVersion(access.base, purity) || !walker.iterationsPreserveTypedArrays(purity) || walker.leavesOutQuestionThatEveryTurnWouldAsk(Walker::QuestionIsAsked::Anywhere))
        return;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference most = mostBelowWrittenBoundInTypedElements(walker, access.key, origin);
    if (!most && access.key.isConstant() && access.typeOfKey.integers.minimum >= 0)
        most = access.key;
    if (!most)
        return;
    Reference length;
    Reference isBelow;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft draft(Opcode::GetTypedArrayLength, origin);
        walker.chooseConservativeFields(draft);
        draft.setViewOperand(access.base);
        draft.setTypedArrayType(typedArrayType);
        length = walker.build(WTF::move(draft));
        if (!length || !length.isValue())
            return;
        Draft test(Opcode::Less, origin);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(most);
        test.setRightOperand(length);
        test.setForm(operatorFormFor(walker, test));
        isBelow = walker.build(WTF::move(test));
    }
    if (!isBelow || !isBelow.isValue() || !walker.tryHoistConditionalCheck(isBelow, true, purity))
        return;
    Reference held;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(length);
        narrowed.setTestOperand(isBelow);
        narrowed.setOperandIndex(OperandIndex::Less::right);
        narrowed.setIsOnTakenEdge(true);
        held = walker.build(WTF::move(narrowed));
    }
    if (!held || !held.isValue() || held == length)
        return;
    noteKnown(walker, KnownNote::LengthOfList, access.base, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), held.bits());
    walker.recordSpeculation();
    walker.recordHoistedArrayGuard();
    walker.count(StatisticID::TypedArrayBoundsChecksHoisted);
}

Reference buildReadOfElementOfViewBehindTestOfLength(Walker& walker, const BodyValue& read, Reference view, Origin origin, BlockID whereItLiesOutside, bool readIsUpdated)
{
    if (walker.shouldDropLengthCheckedReads())
        return { };
    Reference position = walker.referenceOf(read.propertyOperand());
    TypedArrayType typedArrayType = classifyBaseOfTypedElements(walker, walker.typeOf(view)).typedArrayType;
    AccessOfTypedElements access { walker, read, view, position, { }, walker.typeOf(view), walker.types().at(walker.typeOf(position)), { }, false };
    Reference length = lengthOfViewInTypedElements(access, typedArrayType, origin);
    if (!length) {
        Draft draft(Opcode::GetTypedArrayLength, origin);
        walker.chooseConservativeFields(draft);
        draft.setViewOperand(view);
        draft.setTypedArrayType(typedArrayType);
        length = walker.build(WTF::move(draft));
        if (!length || walker.isUnreachable())
            return { };
    }
    std::optional<PassedTest> fromZeroUp = walker.findNonNegativeProof(position);
    if (!fromZeroUp) {
        Draft zero(Opcode::Integer, origin);
        zero.setInteger(0);
        Draft isNotNegative(Opcode::GreaterEq, origin);
        walker.chooseConservativeFields(isNotNegative);
        isNotNegative.setLeftOperand(position);
        isNotNegative.setRightOperand(walker.constant(WTF::move(zero)));
        isNotNegative.setForm(operatorFormFor(walker, isNotNegative));
        fromZeroUp = walker.continueIfCheckPasses(walker.build(WTF::move(isNotNegative)), whereItLiesOutside);
        if (!fromZeroUp)
            return { };
    }
    Reference most = readIsUpdated ? Reference() : mostOfSmallRangeInTypedElements(walker, position, origin);
    if (most && !viewIsUsedElsewhereInTypedElements(walker, read))
        most = { };
    Draft isBelow(Opcode::Less, origin);
    walker.chooseConservativeFields(isBelow);
    isBelow.setLeftOperand(most ? most : position);
    isBelow.setRightOperand(length);
    isBelow.setForm(operatorFormFor(walker, isBelow));
    Reference test = walker.build(WTF::move(isBelow));
    BlockID whereItIsAsked = walker.currentBlock();
    std::optional<PassedTest> below = walker.continueIfCheckPasses(test, whereItLiesOutside);
    if (!below)
        return { };
    if (most && walker.currentBlock() != whereItIsAsked)
        noteLengthOfViewAboveWhatPassedInTypedElements(walker, view, length, test, origin);
    else if (!most && walker.currentBlock() != whereItIsAsked && position.isValue() && test.isValue() && !walker.state().checkedNameOf(position)) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(position);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::Less::left);
        narrowed.setIsOnTakenEdge(true);
        Reference held = walker.build(WTF::move(narrowed));
        if (held && held.isValue() && held != position) {
            if (walker.typeOf(held) != walker.typeOf(position))
                walker.state().noteCheckedName(position, held);
            position = held;
        }
    }
    if (readIsUpdated && length.isValue())
        noteKnown(walker, KnownNote::LengthOfList, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), length.bits());

    Draft element = draftOfTypedElements(walker, read, Opcode::GetTypedArrayElement);
    element.setViewOperand(view);
    element.setPositionOperand(position);
    element.setTypedArrayType(typedArrayType);
    if (std::optional<PassedTest> inside = walker.boundsProofFromChecks(view, position, length, *fromZeroUp, *below))
        element.setIsInsideView(*inside);
    element.setLevel(KnowledgeLevel::Proven);
    element.setKeyIsIntegral(true);
    nameStorageOfViewInTypedElements(access, element, typedArrayType);
    return walker.build(WTF::move(element));
}

static bool buildViewReadOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetTypedArrayElement, KnowledgeLevel::Proven);
    if (bindNumberReadBehindTestsOfTypedElements(access))
        return true;
    Walker& walker = access.walker;
    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetTypedArrayElement);
    std::optional<PassedTest> inside = access.typeOfKey.isInteger() ? walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key) : std::nullopt;
    if (!inside && access.key.isConstant()) {
        inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
        if (!inside) {
            hoistTypedArrayBoundsCheck(access, typedArrayType);
            inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
        }
    }
    if (!inside && walker.isIterationEffectFree())
        inside = tryGuardIndexInBounds(access, typedArrayType, read.origin());
    if (!inside)
        inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    if (!inside) {
        hoistTypedArrayBoundsCheck(access, typedArrayType);
        inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    }
    if (!inside)
        inside = tryGuardIndexInBounds(access, typedArrayType, read.origin());
    if (!inside)
        inside = holdPositionBelowLengthOrStopInTypedElements(access, typedArrayType, read.origin());
    read.setViewOperand(access.base);
    read.setPositionOperand(access.key);
    read.setTypedArrayType(typedArrayType);
    if (inside)
        read.setIsInsideView(*inside);
    else if (access.key.isConstant() && access.typeOfKey.isInteger() && access.typeOfKey.integers.minimum >= 0 && readsElementsInLineInTypedElements(typedArrayType))
        walker.noteLengthOfViewAskedForWrittenPlace(access.base);
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
    bool convertsWithoutCode = !stored.isNever() && stored.isNumberLike() && (!isClamped(typedArrayType) || stored.isBoolean());
    bool convertsInLine = convertsWithoutCode && readsElementsInLineInTypedElements(typedArrayType) && access.walker.has(Mechanism::UnboxedNumbers);
    bool fits = isBigInt(typedArrayType) ? !stored.isNever() && stored.hasOnlyPrimitives(Primitive::BigInt) : stored.isNumber() || convertsInLine;
    if (fits)
        return KnowledgeLevel::Proven;
    bool mayPassTests = readsElementsInLineInTypedElements(typedArrayType) && access.typeOfKey.isInteger() && types.overlap(storedID, TypeTable::number);
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
    Reference stored = access.stored;
    if (level == KnowledgeLevel::Proven && !isBigInt(typedArrayType) && !walker.types().at(access.typeOfStored).isNumber()) {
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        if (isInt(typedArrayType) && walker.types().at(access.typeOfStored).isNumberOrNullish()) {
            Draft zero(Opcode::Integer, origin);
            zero.setInteger(0);
            stored = buildIntegerOperator(walker, Opcode::BitOr, stored, walker.constant(WTF::move(zero)), origin);
        } else {
            Draft conversion(Opcode::ToNumber, origin);
            conversion.setValueOperand(stored);
            walker.chooseConservativeFields(conversion);
            conversion.setForm(operatorFormFor(walker, conversion));
            stored = walker.build(WTF::move(conversion));
        }
        if (!stored)
            return true;
    }
    Draft put = draftOfTypedElements(walker, access.value, Opcode::PutTypedArrayElement);
    put.setViewOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(stored);
    put.setTypedArrayType(typedArrayType);
    std::optional<PassedTest> inside = level == KnowledgeLevel::Proven ? walker.findWhatHoldsPositionInsideList(access.base, access.key) : std::nullopt;
    if (!inside && level == KnowledgeLevel::Proven && access.typeOfKey.isInteger())
        inside = walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key);
    if (!inside && level == KnowledgeLevel::Proven && access.key.isConstant()) {
        hoistTypedArrayBoundsCheck(access, typedArrayType);
        inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    }
    if (inside)
        put.setIsInsideView(*inside);
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
    if (typedArrayType == TypeFloat64)
        return ExpectedElements::Float64Array;
    return ExpectedElements::Anything;
}

static bool buildAccessByIndexOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    bool keyIsIntegral = access.typeOfKey.isNumberWithoutFraction();
    bool namesWhatIsExpected = (access.typeOfKey.isNumber() || access.typeOfKey.primitives & Primitive::Integer) && elementsExpectedByTypedElements(typedArrayType) != ExpectedElements::Anything;
    if ((!keyIsIntegral && !namesWhatIsExpected) || access.isOnlyAskedAbout)
        return false;

    Walker& walker = access.walker;
    Draft draft = walker.draftOf(access.value);
    walker.chooseConservativeFields(draft);
    draft.setKeyIsIntegral(keyIsIntegral);
    draft.setExpectedElements(elementsExpectedByTypedElements(typedArrayType));
    if (typedArrayType == TypeFloat64)
        walker.count(StatisticID::Float64ArrayGenericAccessesHinted);
    if (!access.typeOfKey.isNumber())
        walker.count(StatisticID::UntypedKeyGenericAccessesHinted);
    if (draft.opcode == Opcode::ElemGet && keyIsIntegral && walker.isInLoop() && isExpectedToBeArgumentsObject(walker, access.value.objectOperand())) {
        draft.setReceiverIsExpectedToBeArguments(true);
        walker.count(StatisticID::ArgumentsElementsReadInline);
    }
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
        test.setForm(operatorFormFor(walker, test));
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
        difference.setForm(operatorFormFor(walker, difference));
        position = walker.build(WTF::move(difference));
        if (!position)
            return true;
        position = boxNumberIfNeeded(walker, position, origin);
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
        std::optional<uint32_t> known = lengthOfFreshList(walker, base);
        if (!known)
            known = lengthOfListMadeInSight(walker, base);
        if (known) {
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
        Reference built = walker.build(WTF::move(length));
        if (Type known = built && base.isConstant() ? types.at(walker.typeOf(built)) : Type(); known.isInteger() && known.integers.isSingleValue()) {
            Draft constant(Opcode::Integer, walker.originOf(value));
            constant.setInteger(known.integers.minimum);
            built = walker.constant(WTF::move(constant));
        }
        walker.bind(value.id(), built);
        return true;
    }
    case KindOfBaseOfTypedElements::TypedArray: {
        bool onlyClass = singleBuiltinClassOf(types, types.at(typeOfBase)) || classified.typedArrayType == NotTypedArray || classOfViewBesideInstancesBelowIt(walker, typeOfBase);
        bool isFoundAboveByNote = onlyClass && !isOnlyAskedAbout && base && base.isValue() && walker.program().nameClassOf(value.key()) == NameClass::Length && findKnown(walker, KnownNote::LengthOfViewIsFoundAbove, base);
        bool isSeenToFindGetter = onlyClass && !isOnlyAskedAbout && (findConstantMemberReadOff(walker, base, value.key()) || isFoundAboveByNote);
        bool isSaidByLinkToFindGetter = onlyClass && !isSeenToFindGetter && (isOnlyAskedAbout || linksWordCanBeHeldAgainst(walker, base)) && readsOffEntryParameterThatStaysAsMade(walker, value);
        if (!isSeenToFindGetter && !isSaidByLinkToFindGetter)
            return false;
        if (isOnlyAskedAbout)
            return answerAhead(Opcode::GetTypedArrayLength);
        if (isSaidByLinkToFindGetter)
            holdLinksWordAgainstObjectAsMade(walker, base, walker.originOf(value));
        if (isFoundAboveByNote)
            askAgainWhetherLengthOfViewIsFoundAbove(walker, base, walker.originOf(value));
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

static bool buildStoreOfLengthOfTypedElements(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.isFreeName() || value.key() != walker.program().keyOf(BuiltinNames::length))
        return false;
    TypeArena& types = walker.types();
    if (!hasKind(types, inputs.typeOf(value.objectOperand()), OperandKind::Array))
        return false;
    Type stored = types.at(inputs.typeOf(value.valueOperand()));
    if (!stored.isInteger() || stored.integers.minimum < 0 || stored.integers.maximum > std::numeric_limits<uint32_t>::max())
        return false;
    if (inputs.areOnlyTypes()) {
        walker.noteFormAskedAhead(Opcode::SetArrayLength, KnowledgeLevel::Proven);
        return true;
    }
    Reference list = walker.referenceOf(value.objectOperand());
    if (lengthOfFreshList(walker, list) || lengthOfListMadeInSight(walker, list))
        return false;
    ECMAMode mode = value.mode();
    Draft store = draftOfTypedElements(walker, value, Opcode::SetArrayLength);
    store.setArrayOperand(list);
    store.setLengthOperand(walker.referenceOf(value.valueOperand()));
    store.setMode(mode);
    walker.bind(value.id(), walker.build(WTF::move(store)));
    return true;
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
    if (opcode == Opcode::PropSet)
        return buildStoreOfLengthOfTypedElements(walker, value, inputs);

    Type typeOfKey = walker.types().at(inputs.typeOf(value.propertyOperand()));
    if (!typeOfKey.isNumber() && opcode == Opcode::ElemHas)
        return false;

    bool isOnlyAskedAbout = inputs.areOnlyTypes();
    if (isOnlyAskedAbout && opcode != Opcode::ElemHas && walker.looksTakeFormsBehindLooksAsBuilt() && keyOfProvenListMayBeAskedForInt32(walker.types(), inputs.typeOf(value.objectOperand()), inputs.typeOf(value.propertyOperand())))
        typeOfKey = walker.types().at(TypeTable::int32);
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

constinit const OpcodeSet opcodesThatHearRaiseWherePushIsBuiltAsWrittenTakes { Opcode::Call };

bool hearRaiseWherePushIsBuiltAsWrittenOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.isInTrialVisitOfCurrentFrame() || walker.isInSpecializedIteration() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop || !walker.pushOfLoopFillsLiteral(index.loop(loop).head) || !literalThatCallPushesOntoInEveryTurn(walker, call, index.loop(loop).head))
        return false;
    walker.recordTrialEffects(declaredBoundOf(Opcode::ExtendArrayLength));
    return true;
}

constinit const OpcodeSet opcodesThatBuildPushOntoLiteralThatLoopFillsTakes { Opcode::Call };

bool buildPushOntoLiteralThatLoopFillsOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.isInSpecializedIteration() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop)
        return false;
    BodyBlock head = index.block(index.loop(loop).head);
    Reference list = literalThatCallPushesOntoInEveryTurn(walker, call, head.id());
    std::optional<uint64_t> noted = list ? findKnown(walker, KnownNote::LoopAppendsToLiteral, list, { }, head.id().index()) : std::nullopt;
    if (!noted || *noted >> 32 != walker.currentFunction().index() || !static_cast<uint32_t>(*noted) || static_cast<uint32_t>(*noted) > head.numberOfParameters())
        return false;
    ValueID counter = head.parameter(static_cast<uint32_t>(*noted) - 1);
    Reference key = walker.referenceOf(counter);
    Reference pushed = walker.referenceOf(call.as<Opcode::Call>().argumentsOperands()[0]);
    if (!key || !pushed || !walker.typeOf(key) || !walker.typeOf(pushed))
        return false;
    AccessOfTypedElements access { walker, call, list, key, pushed, walker.typeOf(list), walker.types().at(walker.typeOf(key)), walker.typeOf(pushed), false, counter };
    return buildStoreBehindRowOfTypedElements(access);
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

bool buildReadByIndexOffAnyBaseOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    if (walker.textNamesHolderThatIsNoBuiltin(value.objectOperand()) && tryReadAtSite(walker, value))
        return true;
    Reference object = walker.referenceOf(value.objectOperand());
    Reference key = walker.referenceOf(value.propertyOperand());
    if (!object || !key)
        return false;
    AccessOfTypedElements access { walker, value, object, key, { }, walker.typeOf(object), walker.types().at(walker.typeOf(key)), { }, false };
    return buildAccessByIndexOfTypedElements(access, NotTypedArray);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
