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
#include "AOTSumOverRecords.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkStrings.h"
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
    bool isPushAtLength { false };
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
    if (!onlyClass && isProvenToBeList(walker.types(), id))
        return { KindOfBaseOfTypedElements::Array, NotTypedArray };
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

static bool listHasNoHolesByLink(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    return access.value.opcode() == Opcode::ElemGet && walker.facts().linked().listHasNoHolesAt(walker.currentFunction(), access.value.id())
        && walker.facts().linked().receiverStaysAsMadeAt(walker.currentFunction(), access.value.id()) && readsOffListThatStaysAsMadeByLink(walker, access.value);
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

static bool storesOwnListElement(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    BodyValue read = index.value(index.stripCasts(access.value.valueOperand()));
    if (read.isParameter() || read.opcode() != Opcode::ElemGet)
        return false;
    Reference list = access.walker.referenceOf(read.objectOperand());
    if (!list || !list.isValue() || !access.base || !access.base.isValue())
        return false;
    return underlyingCellOf(access.walker.code(), list.id()) == underlyingCellOf(access.walker.code(), access.base.id());
}

static void requestStorageForStoredValue(const AccessOfTypedElements& access)
{
    if (whatIsStoredIsWrittenToBeNoNumberInTypedElements(access))
        askBirthOfListForStorage(access.walker, access.base, ArrayStorage::Contiguous);
    if (!storesOwnListElement(access))
        askBirthOfListForStorageThatTakes(access.walker, access.base, access.stored);
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

static Representation formOfNumberThatLinkHasTypedOfTypedElements(const AccessOfTypedElements&);
static std::optional<Walker::CallOfCopy> findComputingCaller(Walker&, const BodyValue& read);

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
        read.representation = formOfNumberThatLinkHasTypedOfTypedElements(access);
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

static bool callerComputesInDoubles(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    std::optional<Walker::CallOfCopy> caller = findComputingCaller(walker, read);
    if (!caller)
        return false;
    ValueID user = caller->index->onlyUsingValue(caller->call);
    while (user && opcodesThatStateTypes.contains(caller->index->opcodeOf(user)))
        user = caller->index->onlyUsingValue(user);
    return user && arithmetic.contains(caller->index->opcodeOf(user));
}

static Representation formOfNumberThatLinkHasTypedOfTypedElements(const AccessOfTypedElements& access)
{
    static constexpr OpcodeSet operatorsThatTakeDoubles = numberProducingOperators | allSums;
    static constexpr OpcodeSet operationsThatMakeNumbers = numberProducingOperators | OpcodeSet { Opcode::Integer, Opcode::Double };
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (!walker.has(Mechanism::UnboxedNumbers) || !access.key || !access.key.isRecord() || !numberOfProvenListIsReadUnboxed(types, access.typeOfBase, walker.typeOf(access.key), walker.representationOf(access.key)))
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
    bool mostlyHoldsInt32s = holdsIntegersAlone || (!(type.primitives & Primitive::FractionalNumber) && !readTakesIndexOfProvenList(types, access.typeOfBase, walker.typeOf(access.key)));
    bool holdsWideIntegers = holdsIntegersAlone && type.integers.isSubsetOf(Range::uint32()) && !type.integers.isSubsetOf(Range::int32());
    static constexpr OpcodeSet operatorsOnWideIntegers = allBitwiseOperators | allRelations | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
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
    Representation unboxed = type.primitives & Primitive::Undefined ? Representation::OptionalDouble : Representation::Double;
    auto isStoredIntoNumberSlot = [&](ValueID reader, ValueID taken) {
        auto store = index.value(reader).as<Opcode::PropSet>();
        if (index.value(reader).isFreeName() || store.valueOperand() != taken || store.objectOperand().index() >= read.index())
            return false;
        Reference object = walker.referenceOf(store.objectOperand());
        TypeID objects = object && object.isRecord() ? types.withOnlyObjects(walker.typeOf(object)) : TypeID();
        if (!objects || !types.at(objects).hasObjects() || !types.at(objects).hasOnlyLayouts())
            return false;
        return std::ranges::all_of(membersOf(types, types.at(objects).layouts), [&](LayoutID member) {
            std::optional<FieldReference> field = findField(walker.program(), member, store.key());
            Representation stored = field ? representationOf(walker.plan().slot(member, field->field).representation) : Representation::Unchosen;
            return stored == unboxed || (stored == Representation::OptionalDouble && unboxed == Representation::Double);
        });
    };
    auto takesDouble = [&](ValueID reader, ValueID taken) {
        if (index.opcodeOf(reader) == Opcode::PropSet)
            return !mostlyHoldsInt32s && isStoredIntoNumberSlot(reader, taken);
        if (index.opcodeOf(reader) == Opcode::ElemSet)
            return (type.primitives & Primitive::FractionalNumber) && index.value(reader).valueOperand() == taken && index.value(reader).propertyOperand() != taken;
        if (!(operatorsThatTakeDoubles.contains(index.opcodeOf(reader)) || allRelations.contains(index.opcodeOf(reader))))
            return false;
        if (mostlyHoldsInt32s && !(holdsWideIntegers && operatorsOnWideIntegers.contains(index.opcodeOf(reader))) && !standsBesideFraction(reader, taken))
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
                    standsBesideNumbers &= !index.isParameter(readToCome) && (operationsThatMakeNumbers.contains(index.opcodeOf(readToCome)) || (index.opcodeOf(readToCome) == Opcode::ElemGet && isNumberOrUndefined(walker.facts().linked().resultOfReadByKey(walker.currentFunction(), readToCome))));
                    return;
                }
                Reference other = walker.referenceOf(operand);
                standsBesideNumbers &= other && other.isRecord() && isNumberOrUndefined(walker.typeOf(other));
            });
        }
        return standsBesideNumbers;
    };
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
    if (reader ? !takesDouble(reader, taken) : mostlyHoldsInt32s || !callerComputesInDoubles(walker, access.value))
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

const Walker::ElementReads* Walker::findElementReads(const BodyValue& read)
{
    const BodyIndex& index = currentBodyIndex();
    auto placeOf = [](const ElementReads& reads) { return std::tuple { reads.loop, reads.list, reads.key }; };
    const Vector<ElementReads>& found = m_elementReadsOfLoops.ensure(&index, [&] {
        Vector<ElementReads> made;
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            BlockID block(number);
            if (!index.isReachable(block) || !index.loopOf(block))
                continue;
            for (BodyValue operation : index.block(block).values()) {
                if (operation.opcode() != Opcode::ElemGet)
                    continue;
                ElementReads reads { 0, index.stripCasts(operation.objectOperand()).index(), index.stripCasts(operation.propertyOperand()).index(), 1, onlyUserReckonsInTypedElements(index, operation.id()) ? 0u : 1u };
                for (LoopID loop = index.loopOf(block); loop; loop = index.loop(loop).parent) {
                    reads.loop = loop.index();
                    made.append(reads);
                }
            }
        }
        std::ranges::sort(made, { }, placeOf);
        size_t last = 0;
        for (size_t place = 1; place < made.size(); ++place) {
            if (placeOf(made[place]) == placeOf(made[last])) {
                made[last].all += made[place].all;
                made[last].keeping += made[place].keeping;
            } else
                made[++last] = made[place];
        }
        if (!made.isEmpty())
            made.shrink(last + 1);
        return made;
    }).iterator->value;
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    if (!loop)
        return nullptr;
    std::tuple asked { loop.index(), index.stripCasts(read.objectOperand()).index(), index.stripCasts(read.propertyOperand()).index() };
    auto place = std::ranges::lower_bound(found, asked, { }, placeOf);
    return place != found.end() && placeOf(*place) == asked ? &*place : nullptr;
}

static bool elementIsReadAgainInLoopInTypedElements(Walker& walker, const BodyValue& read, ReadersOfElement readers = ReadersOfElement::All)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.opcode() != Opcode::ElemGet || read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    const Walker::ElementReads* reads = walker.findElementReads(read);
    if (!reads)
        return false;
    if (readers == ReadersOfElement::All)
        return reads->all > 1;
    return reads->keeping > (onlyUserReckonsInTypedElements(index, read.id()) ? 0u : 1u);
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

static bool isNumberProductOfRead(Walker& walker, const BodyValue& read, ValueID made, unsigned depth = 0)
{
    static constexpr OpcodeSet products { Opcode::Sub, Opcode::Mul, Opcode::Div };
    static constexpr unsigned deepestTerm = 4;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!made || made.index() < read.id().index() || index.isParameter(made) || (!products.contains(index.opcodeOf(made)) && (!depth || index.opcodeOf(made) != Opcode::Add)))
        return false;
    auto isNumberOrNothing = [&](TypeID type) {
        return type && !isNeverType(type) && (walker.types().at(type).primitives & Primitive::Number) && walker.types().at(type).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    };
    for (uint32_t place : { 0u, 1u }) {
        ValueID operand = index.value(made).operand(place);
        if (!operand)
            return false;
        if (operand.index() < read.id().index()) {
            Reference built = walker.referenceOf(operand);
            if (!built || !built.isRecord() || !isNumberOrNothing(walker.typeOf(built)))
                return false;
            continue;
        }
        ValueID written = index.stripCasts(operand);
        if (!written || index.isParameter(written))
            return false;
        Opcode opcode = index.opcodeOf(written);
        if (opcode == Opcode::Integer || opcode == Opcode::Double)
            continue;
        if (opcode == Opcode::ElemGet ? !isNumberOrNothing(walker.facts().linked().resultOfReadByKey(walker.currentFunction(), written)) : depth == deepestTerm || !isNumberProductOfRead(walker, read, written, depth + 1))
            return false;
    }
    return true;
}

static bool userTakesUndefinedForNaNInTypedElements(Walker& walker, const BodyValue& read, ValueID name, ValueID user, WhatStandsBesideRead beside, bool isOneOfSeveral, bool readGivesUpItsHole)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyValue operation = index.value(user);
    BodyBlock blockOfRead = index.block(index.blockOf(read.id()));
    if (uint32_t end = walker.endOfStretchBeingWalked(); (walker.standsInArmOfRowOfShapeTests() || walker.standsInClosedArm()) && end < blockOfRead.numberOfValues() && (!blockOfRead.holds(user) || user.index() - blockOfRead.firstValue().index() >= end))
        return false;
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    bool takesBits = allBitwiseOperators.contains(operation.opcode());
    if (takesBits && operation.opcode() == Opcode::BitNot)
        return true;
    bool isRelation = allRelations.contains(operation.opcode());
    if (!takesBits && !isRelation && !arithmetic.contains(operation.opcode()))
        return false;
    if (readGivesUpItsHole && isRelation && elementIsReadAgainInLoopInTypedElements(walker, read))
        return false;
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    if (other == name)
        return isOneOfSeveral;
    ValueID written = index.stripCasts(other);
    if (written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer && (takesBits || isRelation || std::abs(index.value(written).integer()) > 1))
        return true;
    if (!isRelation && isNumberProductOfRead(walker, read, written))
        return !readGivesUpItsHole || !elementIsReadAgainInLoopInTypedElements(walker, read, ReadersOfElement::ThoseThatDoMoreThanReckon);
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
        return (!readGivesUpItsHole || !elementIsReadAgainInLoopInTypedElements(walker, read, ReadersOfElement::ThoseThatDoMoreThanReckon)) && (comesOffListOfNumbersTestedInFrontInTypedElements(walker, index.value(written), read.id()) || walker.shouldAssumeNumericAddends());
    }
    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), written);
    return (seenByLink && isNumberOrNothing(seenByLink)) || ((!readGivesUpItsHole || !elementIsReadAgainInLoopInTypedElements(walker, read, ReadersOfElement::ThoseThatDoMoreThanReckon)) && comesOffListOfNumbersTestedInFrontInTypedElements(walker, index.value(written), read.id()));
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
    auto isAfterGuardedReads = [&](ValueID taken) {
        if (!walker.guardedReadArray())
            return false;
        BodyBlock block = index.block(index.blockOf(read.id()));
        return !block.holds(taken) || taken.index() - block.firstValue().index() >= walker.endOfStretchBeingWalked();
    };
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (user)
        return !isAfterGuardedReads(user) && userTakesUndefinedForNaNInTypedElements(walker, read, name, user, beside, false, readGivesUpItsHole);
    return everyUseStandsInBlockOfValueAndPassesInTypedElements(walker, name, [&](ValueID userOfSeveral) {
        return !isAfterGuardedReads(userOfSeveral) && userTakesUndefinedForNaNInTypedElements(walker, read, name, userOfSeveral, beside, true, readGivesUpItsHole);
    });
}

static std::optional<Walker::CallOfCopy> findComputingCaller(Walker& walker, const BodyValue& read)
{
    std::optional<Walker::CallOfCopy> copied = walker.callOfCopy();
    if (!copied || walker.standsInArmOfRowOfShapeTests() || walker.guardedReadArray())
        return std::nullopt;
    const BodyIndex& callee = walker.currentBodyIndex();
    ValueID returned = read.id();
    for (ValueID user = callee.onlyUsingValue(returned); user && opcodesThatStateTypes.contains(callee.opcodeOf(user)); user = callee.onlyUsingValue(returned))
        returned = user;
    BodyTerminator end = callee.block(callee.blockOf(returned)).terminator();
    if (!callee.hasOneUse(returned) || end.kind() != TerminatorKind::Return || end.operand(0) != returned)
        return std::nullopt;
    const BodyIndex& index = *copied->index;
    ValueID name = copied->call;
    ValueID user = index.onlyUsingValue(name);
    for (; user && opcodesThatStateTypes.contains(index.opcodeOf(user)); user = index.onlyUsingValue(user))
        name = user;
    if (!user)
        return std::nullopt;
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    BodyValue operation = index.value(user);
    if (operation.opcode() == Opcode::BitNot)
        return copied;
    if (!arithmetic.contains(operation.opcode()) && !allBitwiseOperators.contains(operation.opcode()))
        return std::nullopt;
    if (operation.opcode() != Opcode::Add)
        return copied;
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    Reference built = other != name && other.index() < copied->call.index() ? walker.referenceInCaller(other) : Reference();
    if (!built)
        return std::nullopt;
    Type held = walker.types().at(walker.typeOf(built));
    if (!(held.primitives & Primitive::Number) || !held.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
        return std::nullopt;
    return copied;
}

static Reference numberMadeOnceOfElementInTypedElements(Walker& walker, const BodyValue& read, Reference element)
{
    if (!element || !element.isValue() || walker.isBuildingInPlace())
        return element;
    Type type = walker.types().at(walker.typeOf(element));
    if (!(type.primitives & Primitive::Undefined) || !(type.primitives & Primitive::Number) || !type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
        return element;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(read.id());
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    LoopID loopOfUser = user ? index.loopOf(index.blockOf(user)) : LoopID();
    bool userIsInInnerLoop = loopOfUser && loopOfUser != loop && index.loop(loopOfUser).parent == loop;
    if ((user && !userIsInInnerLoop) || !everyUserTakesUndefinedForNaNInTypedElements(walker, read, WhatStandsBesideRead::NumberOrNothing, false))
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
    if (!user || !(allRelations | equalities).contains(index.opcodeOf(user)) || elementIsReadAgainInLoopInTypedElements(walker, read))
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

static bool isElementUsedWithConstant(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    if (!loop)
        return false;
    ValueID name = read.id();
    std::optional<BodyUser> user = index.onlyUser(name);
    while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value))) {
        name = user->value;
        user = index.onlyUser(name);
    }
    if (!user || user->kind != BodyUseKind::EdgeArgument)
        return false;
    BlockID join;
    size_t position = 0;
    for (BodyTarget target : index.terminator(user->block).targets()) {
        std::span<const ValueID> arguments = target.arguments();
        if (auto found = std::ranges::find(arguments, name); found != arguments.end()) {
            join = target.block();
            position = found - arguments.begin();
        }
    }
    if (!join || index.isLoopHead(join) || index.loopOf(join) != loop)
        return false;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != join || position >= target.arguments().size())
                continue;
            ValueID other = index.stripCasts(target.arguments()[position]);
            if (other && !index.isParameter(other) && (index.opcodeOf(other) == Opcode::Integer || index.opcodeOf(other) == Opcode::Double))
                return true;
        }
    }
    return false;
}

static bool iterationComputesWithElement(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet numberOnlyOperators { Opcode::Sub, Opcode::Mul, Opcode::Div };
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    std::optional<BodyUser> user = index.firstUser(read.id());
    while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value)))
        user = index.firstUser(user->value);
    if (!user || user->kind != BodyUseKind::Operand)
        return false;
    Opcode opcode = index.opcodeOf(user->value);
    return numberOnlyOperators.contains(opcode) || (opcode == Opcode::Add && onlyUserTakesUndefinedForNaNInTypedElements(walker, read));
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

bool guardElementPresenceInIteration(Walker& walker, Draft& read, Reference list, Reference position, const HoistedArrayGuard& tested)
{
    if (!read.keyIsIntegral() || !hasArrayStorageForm(read.arrayStorage()))
        return true;
    Draft isPresent(Opcode::IsElementPresent, read.origin());
    walker.chooseConservativeFields(isPresent);
    isPresent.setArrayOperand(list);
    isPresent.setPositionOperand(position);
    isPresent.setKeyIsIntegral(true);
    isPresent.setListHasNoHoles(read.listHasNoHoles());
    if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position))
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
    std::optional<PassedTest> present = walker.isUnreachable() || walker.hasFailed() ? std::nullopt : walker.showElementPresentBehindTest(list, position, walker.findPassedTest(test));
    if (!present)
        return false;
    noteKnown(walker, KnownNote::PositionFoundInsideList, list, position, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ArrayStorage }), test.bits());
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

static bool isOnlyIndexed(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    ValueID element = access.value.id();
    if (element.index() >= index.numberOfValues() || index.value(element).words() != access.value.words())
        return false;
    ValueID user = index.onlyUsingValue(element);
    return user && (index.opcodeOf(user) == Opcode::ElemGet || index.opcodeOf(user) == Opcode::ElemSet) && index.value(user).objectOperand() == element;
}

static ArrayStorage kindAskedInTurnOfTypedElements(const AccessOfTypedElements& access)
{
    if (isArraySubclassInstance(access.walker, access.typeOfBase))
        return isOnlyIndexed(access) ? ArrayStorage::Contiguous : ArrayStorage::Other;
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

static bool isMaskedKey(const Code& code, ValueID key, const Type& type)
{
    static constexpr OpcodeSet masks { Opcode::BitAnd, Opcode::RShift, Opcode::URShift };
    const Operation& operation = code.at(key);
    return masks.contains(operation.opcode) && (code.isConstant(operation.operand(0)) || code.isConstant(operation.operand(1))) && type.integers.maximum <= std::numeric_limits<uint16_t>::max();
}

enum class WrittenMasks : bool { AreLeft, AreTaken };

static bool isBelowConstantBound(const Code& code, ValueID position, unsigned depth = 0)
{
    static constexpr OpcodeSet operators { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::LShift };
    static constexpr OpcodeSet upperBounds { Opcode::Less, Opcode::LessEq };
    static constexpr OpcodeSet lowerBounds { Opcode::Greater, Opcode::GreaterEq };
    constexpr unsigned deepestOperator = 3;
    while (conversionsOfForm.contains(code.at(position).opcode))
        position = code.at(position).operand(0);
    const Operation& made = code.at(position);
    if (code.isConstant(position))
        return made.opcode == Opcode::Integer;
    if (operators.contains(made.opcode))
        return depth < deepestOperator && isBelowConstantBound(code, made.operand(0), depth + 1) && isBelowConstantBound(code, made.operand(1), depth + 1);
    if (made.opcode != Opcode::Narrowed || !made.isOnTakenEdge())
        return false;
    ValueID test = made.testOperand();
    if (!test || code.isConstant(test) || made.operandIndex() > OperandIndex::Less::right)
        return false;
    const Operation& relation = code.at(test);
    const OpcodeSet& bounds = made.operandIndex() == OperandIndex::Less::left ? upperBounds : lowerBounds;
    return bounds.contains(relation.opcode) && code.isConstant(relation.operand(1 - made.operandIndex()));
}

static Reference mostBelowWrittenBoundInTypedElements(Walker& walker, Reference position, Origin origin, WrittenMasks masks = WrittenMasks::AreLeft)
{
    Type type = walker.types().at(walker.typeOf(position));
    if (!position.isRecord() || !type.isInteger() || type.integers.minimum < 0 || (position.isConstant() && (masks == WrittenMasks::AreTaken || type.integers.maximum > std::numeric_limits<uint16_t>::max())))
        return { };
    const Code& code = walker.code();
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    bool changesFromTurnToTurn = position.isValue() && !walker.isInvariantInSpecializedVersion(position);
    ValueID held = position.id();
    while (conversionsOfForm.contains(code.at(held).opcode) || (changesFromTurnToTurn && sums.contains(code.at(held).opcode) && code.isConstant(code.at(held).operand(1))))
        held = code.at(held).operand(0);
    if ((masks == WrittenMasks::AreLeft || !isMaskedKey(code, held, type)) && !isBelowConstantBound(code, held))
        return { };
    Draft most(Opcode::Integer, origin);
    most.setInteger(type.integers.maximum);
    return walker.constant(WTF::move(most));
}

static std::optional<Range> indexRangeOverIterations(Walker& walker, const BodyIndex& index, ValueID written, unsigned depth = 0)
{
    static constexpr OpcodeSet operators { Opcode::Add, Opcode::Sub, Opcode::Mul };
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    constexpr unsigned deepestOperator = 3;
    ValueID value = index.stripCasts(written);
    if (!value)
        return std::nullopt;
    if (!index.isParameter(value) && operators.contains(index.opcodeOf(value)) && depth < deepestOperator) {
        Opcode opcode = index.opcodeOf(value);
        std::optional<Range> left = indexRangeOverIterations(walker, index, index.value(value).operand(0), depth + 1);
        std::optional<Range> right = left ? indexRangeOverIterations(walker, index, index.value(value).operand(1), depth + 1) : std::nullopt;
        if (!right)
            return std::nullopt;
        Range made;
        if (opcode == Opcode::Add)
            made = { left->minimum + right->minimum, left->maximum + right->maximum };
        else if (opcode == Opcode::Sub)
            made = { left->minimum - right->maximum, left->maximum - right->minimum };
        else
            made = { left->minimum * right->minimum, left->maximum * right->maximum };
        return made.minimum >= 0 && made.maximum <= std::numeric_limits<uint16_t>::max() ? std::optional(made) : std::nullopt;
    }
    Reference held = walker.referenceOf(value);
    if (!held)
        return std::nullopt;
    Type type = walker.types().at(walker.typeOf(held));
    if (!type.isInteger() || type.integers.minimum < 0 || type.integers.maximum > std::numeric_limits<uint16_t>::max())
        return std::nullopt;
    Range found { type.integers.minimum, type.integers.maximum };
    BlockID head = walker.unrolledLoopHeader();
    if (!index.isParameter(value) || index.blockOf(value) != head)
        return found;
    BodyTerminator end = index.terminator(head);
    ValueID relation = end.isConditionalBranch() ? end.operand(0) : ValueID();
    if (!relation || index.isParameter(relation) || !relations.contains(index.opcodeOf(relation)))
        return found;
    ValueID bound = index.stripCasts(index.value(relation).operand(OperandIndex::Less::right));
    if (index.stripCasts(index.value(relation).operand(OperandIndex::Less::left)) != value || !bound || index.isParameter(bound) || index.opcodeOf(bound) != Opcode::Integer)
        return found;
    int64_t last = index.value(bound).as<Opcode::Integer>().integer() - (index.opcodeOf(relation) == Opcode::Less);
    if (last <= std::numeric_limits<uint16_t>::max())
        found.maximum = std::max(found.maximum, last);
    return found;
}

static Reference mostUnderLoweredCounter(const AccessOfTypedElements& access, Walker::IterationPurity purity, Origin origin)
{
    Walker& walker = access.walker;
    int64_t offset = 0;
    Reference most = walker.findNonNegativeProof(access.key) ? walker.findLoweredCounterStart(access.key, offset) : Reference();
    if (!most || !walker.isInvariantInSpecializedVersion(most, purity))
        return { };
    if (!offset) {
        static constexpr OpcodeSet lengths { Opcode::GetArrayLength, Opcode::GetTypedArrayLength };
        ValueID start = walker.code().valueBehindNames<cellPreservingOpcodes>(most.id());
        return lengths.contains(walker.code().at(start).opcode) ? Reference() : most;
    }
    Draft difference(Opcode::Integer, origin);
    difference.setInteger(std::abs(offset));
    Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
    Reference last = buildIntegerOperator(walker, offset < 0 ? Opcode::Sub : Opcode::Add, most, walker.constant(WTF::move(difference)), origin);
    return last && last.isRecord() && walker.types().at(walker.typeOf(last)).isInteger() ? last : Reference();
}

static bool isAboveEveryKey(Walker& walker, Reference most, Reference length)
{
    SumOverRecords distance(walker.code(), walker.types());
    return most.isRecord() && length.isRecord() && distance.add(most.id(), 1) && distance.add(length.id(), -1) && distance.isFromZeroUp();
}

static bool hoistUpperBoundCheck(Walker& walker, Reference isBelow, Walker::IterationPurity purity, bool isLowered)
{
    bool isHoisted = isLowered ? walker.tryBranchInPreheader(isBelow, true, purity, Walker::GuardedIterations::All) : !!walker.tryHoistConditionalCheck(isBelow, true, purity);
    if (isHoisted && isLowered)
        walker.state().answers().noteOutcome(isBelow, true);
    return isHoisted;
}

static std::optional<int64_t> greatestWrittenIndex(const BodyIndex& index, const BodyValue& firstAccess)
{
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet };
    ValueID list = index.stripCasts(firstAccess.objectOperand());
    std::optional<int64_t> greatest;
    for (BodyValue access : index.block(index.blockOf(firstAccess.id())).values()) {
        ValueID key = accesses.contains(access.opcode()) && index.stripCasts(access.objectOperand()) == list ? index.stripCasts(access.propertyOperand()) : ValueID();
        if (!key || index.isParameter(key) || index.opcodeOf(key) != Opcode::Integer)
            continue;
        if (!greatest && access.id() != firstAccess.id())
            return std::nullopt;
        greatest = std::max(greatest.value_or(0), index.value(key).as<Opcode::Integer>().integer());
    }
    return greatest && *greatest <= std::numeric_limits<uint16_t>::max() ? greatest : std::nullopt;
}

static bool isInLoopTooLargeToUnroll(Walker& walker, const BodyValue& access)
{
    static constexpr uint32_t maxUnrolledLoopSize = 100;
    const BodyIndex& index = walker.currentBodyIndex();
    if (access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    uint32_t numberOfOperations = 0;
    for (BlockID member : loop ? index.membersOf(loop) : std::span<const BlockID>())
        numberOfOperations += index.block(member).numberOfValues();
    return numberOfOperations > maxUnrolledLoopSize;
}

static void hoistArrayBoundsCheck(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (!access.typeOfKey.isInteger() || !walker.isInvariantInSpecializedVersion(access.base, purity) || !walker.iterationsPreserveArrayGuards(access.base, purity) || !walker.iterationsPreserveArrayLength(access.base, purity))
        return;
    if (walker.findWhatHoldsPositionInsideList(access.base, access.key) || walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key))
        return;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference most = mostBelowWrittenBoundInTypedElements(walker, access.key, origin);
    if (!most && isInLoopTooLargeToUnroll(walker, access.value))
        most = mostBelowWrittenBoundInTypedElements(walker, access.key, origin, WrittenMasks::AreTaken);
    bool isLowered = !most;
    if (isLowered)
        most = mostUnderLoweredCounter(access, purity, origin);
    if (!most)
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    bool isInBody = access.value.id().index() < index.numberOfValues() && index.value(access.value.id()).words() == access.value.words();
    if (std::optional<Range> overTurns = isInBody && walker.unrolledLoopHeader() ? indexRangeOverIterations(walker, index, access.value.propertyOperand()) : std::nullopt; overTurns && overTurns->maximum > access.typeOfKey.integers.maximum) {
        Draft iterationMaximum(Opcode::Integer, origin);
        iterationMaximum.setInteger(overTurns->maximum);
        most = walker.constant(WTF::move(iterationMaximum));
        walker.count(StatisticID::UnrolledIterationLengthChecks);
    }
    if (std::optional<int64_t> greatest = isInBody && access.key.isConstant() ? greatestWrittenIndex(index, access.value) : std::nullopt; greatest && *greatest > access.typeOfKey.integers.maximum) {
        Draft mostOfBlock(Opcode::Integer, origin);
        mostOfBlock.setInteger(*greatest);
        most = walker.constant(WTF::move(mostOfBlock));
    }
    Reference length;
    Reference isBelow;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        length = buildArrayLength(walker, access.base, origin);
        if (!length || !length.isValue() || isAboveEveryKey(walker, most, length))
            return;
        Draft test(Opcode::Less, origin);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(most);
        test.setRightOperand(length);
        test.setForm(operatorFormFor(walker, test));
        isBelow = walker.build(WTF::move(test));
    }
    if (!isBelow || !isBelow.isValue() || !hoistUpperBoundCheck(walker, isBelow, purity, isLowered))
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

static constexpr AbstractHeapSet elementGuardHeaps {
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
static_assert(perObjectHeaps.containsAll(elementGuardHeaps));

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
    bool isRounded = access.typeOfKey.isNumberWithoutFraction();
    if (access.isOnlyAskedAbout || !access.key || !access.key.isValue() || !access.typeOfKey.isNumber() || access.typeOfKey.isInteger())
        return std::nullopt;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.canRestartIteration() || walker.readStandsUnderHandler())
        return std::nullopt;
    TypeID linkedElementType = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), access.value.id());
    bool holdsNumbersAlone = isDeclaredToHoldNumbersAlone(walker.program(), walker.types(), access.typeOfBase)
        || (linkedElementType && (walker.types().at(linkedElementType).primitives & Primitive::Number) && walker.types().at(linkedElementType).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined));
    bool isReadAlone = isRounded && access.value.opcode() == Opcode::ElemGet && walker.currentBodyIndex().onlyUsingValue(access.value.propertyOperand()) == access.value.id();
    if (keyGoesRoundLoopInTypedElements(access) || holdsNumbersAlone == isRounded || isReadAlone)
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

static Reference buildTestedRead(const AccessOfTypedElements& access, Draft&& read)
{
    Walker& walker = access.walker;
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::EffectFree;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    bool isInvariant = walker.isInvariantInSpecializedVersion(access.base, purity) && walker.isInvariantInSpecializedVersion(access.key, purity) && walker.iterationsPreserveArrayGuards(access.base, purity) && walker.hasNoElementStores() && !walker.readStandsUnderHandler();
    Reference element;
    Reference isNumber;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInvariant, purity);
        element = walker.build(WTF::move(read));
        if (element && element.isValue() && !walker.isUnreachable() && !walker.types().at(walker.typeOf(element)).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
            isNumber = buildTestOfNumber(walker, element, TypeArena::fromPrimitives(Primitive::Number | Primitive::Undefined), origin);
    }
    if (!isNumber || !isNumber.isValue() || walker.hasFailed())
        return element;
    bool isTestedInFront = isInvariant && walker.tryBranchInPreheader(isNumber, true, purity);
    if (!isTestedInFront && !continueOrRestartIteration(walker, isNumber))
        return { };
    walker.recordSpeculation();
    Walker::SpecializedPreheaderScope preheaderScope(walker, isTestedInFront, purity);
    return buildProofOfNumber(walker, element, isNumber, origin);
}

static bool isReadOfBody(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    return access.value.id().index() < index.numberOfValues() && index.value(access.value.id()).words() == access.value.words();
}

static Reference narrowTruncatedElement(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, Reference element)
{
    Walker& walker = access.walker;
    if (!element || !element.isValue() || walker.isUnreachable() || walker.hasFailed() || !walker.isInLoop() || !walker.has(Mechanism::UnboxedNumbers) || !canGuardAccess(access))
        return element;
    TypeArena& types = walker.types();
    Type type = types.at(walker.typeOf(element));
    Representation form = walker.representationOf(element);
    bool isBoxed = form == Representation::JSValue && walker.wasAskedForStorageOfNumbersInVain(tested.name) && !type.isNumber();
    if (!isBoxed && (form != Representation::Double || !type.isNumber() || !(type.primitives & Primitive::Double)))
        return element;
    if (!isReadOfBody(access))
        return element;
    TruncatedUse use = findTruncatedUse(walker, access.value.id());
    if (use == TruncatedUse::None || (use == TruncatedUse::OperandOfBits && !isBoxed))
        return element;
    bool takesFractions = use == TruncatedUse::OperandOfBits && isOnlyTruncated(walker, access.value.id());
    TypeID asked = takesFractions ? TypeID(TypeTable::number) : types.integer(integersAddedWithoutRounding);
    if (!types.overlap(walker.typeOf(element), asked))
        return element;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference passes = buildTestOfNumber(walker, element, asked, origin);
    if (!passes || !passes.isValue() || walker.isUnreachable() || walker.hasFailed())
        return element;
    if (!continueOrRestartIteration(walker, passes))
        return { };
    walker.count(StatisticID::ElementsGuardedInIterations);
    Reference number = buildProofOfNumber(walker, element, passes, origin);
    if (takesFractions || !number || !number.isValue() || walker.representationOf(number) != Representation::JSValue)
        return number;
    Draft unboxed(Opcode::Unbox, origin);
    unboxed.setOperand(0, number);
    unboxed.representation = Representation::Int64;
    return walker.build(WTF::move(unboxed));
}

static Reference buildIntegerReadByStorage(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, std::optional<PassedTest> inside)
{
    Walker& walker = access.walker;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    auto buildInteger = [&](Opcode conversion, Reference number) {
        if (!number || !number.isValue() || walker.representationOf(number) == Representation::Int64)
            return number;
        Draft integer(conversion, origin);
        integer.setOperand(0, number);
        integer.representation = Representation::Int64;
        return walker.build(WTF::move(integer));
    };
    auto readFrom = [&](ArrayStorage storage) -> Reference {
        HoistedArrayGuard exact = tested;
        exact.storage = storage;
        exact.admitsInt32Storage = false;
        Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
        read.setArrayOperand(access.base);
        read.setPositionOperand(access.key);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        if (inside)
            read.setIsInBounds(*inside);
        nameKindOfListTestedInFrontOfLoop(read, exact);
        if (!guardElementPresenceInIteration(walker, read, access.base, access.key, exact))
            return { };
        takeStorageAndLengthFromInFrontOfLoop(walker, read, exact);
        Reference element = walker.build(WTF::move(read));
        if (!element || !element.isValue() || storage == ArrayStorage::Int32)
            return buildInteger(Opcode::WidenInteger, element);
        Reference isInteger = buildTestOfNumber(walker, element, walker.types().integer(integersAddedWithoutRounding), origin);
        if (!continueOrRestartIteration(walker, isInteger))
            return { };
        return buildInteger(Opcode::NumberToInteger, buildProofOfNumber(walker, element, isInteger, origin));
    };
    Draft kind(Opcode::HasArrayStorage, origin);
    walker.chooseConservativeFields(kind);
    kind.setArrayOperand(access.base);
    kind.setArrayStorage(ArrayStorage::Int32);
    Reference holdsInt32s = walker.build(WTF::move(kind));
    if (!holdsInt32s || !holdsInt32s.isValue() || walker.hasFailed())
        return { };
    BlockID int32s = walker.addBlock();
    BlockID doubles = walker.addBlock();
    walker.branch(holdsInt32s, int32s, doubles);
    Vector<WayInNumberForms, 2> ways;
    for (ArrayStorage storage : { ArrayStorage::Int32, ArrayStorage::Double }) {
        walker.continueIn(storage == ArrayStorage::Int32 ? int32s : doubles);
        Reference integer = walker.isUnreachable() ? Reference() : readFrom(storage);
        if (walker.hasFailed())
            return { };
        if (integer && !walker.isUnreachable())
            ways.append(WayInNumberForms { walker.currentBlock(), integer, { } });
    }
    walker.count(StatisticID::ElementsGuardedInIterations);
    return meetInNumberForms(walker, walker.addBlock(), ways.span());
}

static bool isInLongLoop(Walker& walker, const BodyValue& access)
{
    static constexpr uint32_t maxShortLoopSize = 20;
    static constexpr OpcodeSet uncounted = primitiveConstants | opcodesThatStateTypes;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    uint32_t numberOfOperations = 0;
    for (BlockID member : loop ? index.membersOf(loop) : std::span<const BlockID>()) {
        for (BodyValue operation : index.block(member).values()) {
            if (!uncounted.contains(operation.opcode()) && ++numberOfOperations > maxShortLoopSize)
                return true;
        }
    }
    return false;
}

static Reference buildTruncatedRead(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, std::optional<PassedTest> inside)
{
    static constexpr uint32_t truncated = 1;
    Walker& walker = access.walker;
    bool isNoted = access.base.isValue() && access.key.isRecord();
    if (std::optional<uint64_t> known = isNoted ? findKnown(walker, KnownNote::ElementReadUnderNumber, access.base, access.key, truncated) : std::nullopt) {
        walker.count(StatisticID::NumericElementReadsEliminated);
        return Reference::fromBits(static_cast<uint32_t>(*known));
    }
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
    read.setArrayOperand(access.base);
    read.setPositionOperand(access.key);
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(true);
    read.setReadsHoleAsNaN(true);
    if (inside)
        read.setIsInBounds(*inside);
    nameKindOfListTestedInFrontOfLoop(read, tested);
    takeStorageAndLengthFromInFrontOfLoop(walker, read, tested);
    Reference element = walker.build(WTF::move(read));
    if (!element)
        return { };
    Draft conversion(Opcode::ToInt32, origin);
    conversion.setOperand(0, element);
    Reference integer = walker.build(WTF::move(conversion));
    if (isNoted && integer && integer.isValue() && !walker.isUnreachable())
        noteKnown(walker, KnownNote::ElementReadUnderNumber, access.base, access.key, truncated, LocationMasks(elementGuardHeaps), integer.bits());
    return integer;
}

static std::optional<PassedTest> guardKeyBelowLength(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    std::optional<PassedTest> fromZeroUp = canGuardAccess(access) && access.base.isValue() && access.key.isValue() ? walker.findNonNegativeProof(access.key) : std::nullopt;
    if (!fromZeroUp)
        return std::nullopt;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference length = buildArrayLength(walker, access.base, origin);
    Reference test = length ? buildIntegerOperator(walker, Opcode::Less, access.key, length, origin) : Reference();
    if (!test || !test.isValue() || walker.code().isConstant(test.id()) || !continueOrRestartIteration(walker, test, true))
        return std::nullopt;
    std::optional<PassedTest> below = walker.findPassedTest(test);
    return below ? walker.boundsProofFromChecks(access.base, access.key, length, *fromZeroUp, *below) : std::nullopt;
}

static bool buildArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (Reference ahead = access.isOnlyAskedAbout ? Reference() : walker.elementReadAhead(access.value.id())) {
        walker.bind(access.value.id(), ahead);
        return true;
    }
    if (!access.isOnlyAskedAbout && bindNumberReadBehindTestsOfTypedElements(access))
        return true;
    bool walkStoodInFront = !walker.isUnreachable() && !walker.hasFailed();
    if (std::optional<AccessOfTypedElements> held = tryGuardIntegerKey(access))
        return buildArrayReadOfTypedElements(*held);
    if (walkStoodInFront && (walker.isUnreachable() || walker.hasFailed()))
        return true;
    auto [level, restsOnLinksWordAlone] = levelOfArrayReadOfTypedElements(access);
    if (level == KnowledgeLevel::Unknown && !access.isOnlyAskedAbout && access.typeOfKey.isNumber() && (findHoistedArrayGuard(walker, access.value) || tryHoistArrayGuardForAccess(walker, access.value, access.base)))
        level = KnowledgeLevel::Expected;
    std::optional<PassedTest> presentAtHead = level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout ? walker.tryGuardArrayAtHeader(access.value) : std::nullopt;
    std::optional<HoistedArrayGuard> tested = level == KnowledgeLevel::Unknown ? std::nullopt : findHoistedArrayGuard(walker, access.value);
    std::optional<HoistedArrayGuard> rowHolder = !tested && level != KnowledgeLevel::Unknown && access.typeOfKey.isInteger() ? findRowHolderGuard(walker, access.value) : std::nullopt;
    std::optional<PassedTest> rowIsPresent = rowHolder && !access.isOnlyAskedAbout && !canGuardAccess(access) ? tryHoistElementPresenceCheck(walker, access.value, *rowHolder, access.base, access.key) : std::nullopt;
    if (rowHolder && (access.isOnlyAskedAbout || rowIsPresent || canGuardAccess(access)))
        tested = rowHolder;
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
    bool mayHaveNumberNames = !access.isOnlyAskedAbout && level != KnowledgeLevel::Proven && access.typeOfKey.isNumber() && readsPlainListByLink(walker, access.value);
    if (mayHaveNumberNames) {
        level = KnowledgeLevel::Proven;
        restsOnLinksWordAlone = true;
    }
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
    read.setMayHaveNumberNames(mayHaveNumberNames);
    read.setListHasNoHoles(listHasNoHolesByLink(access));
    if (read.listHasNoHoles())
        walker.count(StatisticID::ReadsOfListsWithoutHoles);
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (access.typeOfKey.isInteger())
        walker.noteLoweredCounter(access.value);
    if (tested && !lengthOfListMadeInSight(walker, access.base) && (!access.key.isConstant() || tested->turnsStoreIntoLists))
        hoistArrayBoundsCheck(access);
    std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    if (!inside && tested && access.typeOfKey.isInteger())
        inside = walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key);
    if (inside)
        read.setIsInBounds(*inside);
    bool isOfEitherNumericStorage = tested && tested->storage == ArrayStorage::Double && tested->admitsInt32Storage && !tested->isWritable && !tested->written;
    bool isIndex = isArrayIndexInTypedElements(access.typeOfKey);
    TruncatedUse truncatedUse = isOfEitherNumericStorage && !presentAtHead && access.typeOfKey.isInteger() && walker.isInLoop() && walker.has(Mechanism::UnboxedNumbers) && isReadOfBody(access) ? findTruncatedUse(walker, access.value.id()) : TruncatedUse::None;
    bool isKnownInIteration = truncatedUse == TruncatedUse::OperandOfBits && (walker.state().element(access.base, access.key) || walker.state().elementWithNaNForHole(access.base, access.key));
    bool isTruncatedInLoop = truncatedUse == TruncatedUse::OperandOfBits && !isKnownInIteration && isInLongLoop(walker, access.value);
    if (isTruncatedInLoop && !isIndex && !inside)
        inside = guardKeyBelowLength(access);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    if ((truncatedUse == TruncatedUse::TermOfSums && isIndex && canGuardAccess(access)) || (isTruncatedInLoop && (isIndex || inside))) {
        Reference integer = truncatedUse == TruncatedUse::OperandOfBits ? buildTruncatedRead(access, *tested, inside) : buildIntegerReadByStorage(access, *tested, inside);
        if (integer || !walker.isUnreachable())
            walker.bind(access.value.id(), integer);
        return true;
    }
    bool namesStorage = !tested && level == KnowledgeLevel::Proven && !restsOnLinksWordAlone && access.typeOfKey.isInteger() && nameStorageOfListMadeInSight(walker, read);
    if (!tested && !namesStorage && !hasArrayStorageForm(storage))
        read.representation = formOfNumberThatLinkHasTypedOfTypedElements(access);
    if (tested)
        nameKindOfListTestedInFrontOfLoop(read, *tested);
    Reference freshListReadUnderKey = namesStorage && access.key.isValue() && isArrayIndexInTypedElements(access.typeOfKey) ? freshObjectNamedInTypedElements(walker, access.base) : Reference();
    bool elementIsKnownToBeThere = read.isPresent();
    bool isReadUnderNumber = level == KnowledgeLevel::Proven && !namesStorage && access.base.isValue() && access.key.isValue() && !isArrayIndexInTypedElements(access.typeOfKey) && !freshObjectNamedInTypedElements(walker, access.base);
    bool readsTestedList = tested && tested->storage == ArrayStorage::Contiguous && !tested->admitsInt32Storage && level == KnowledgeLevel::Proven && !freshListReadUnderKey && !isReadUnderNumber && access.base.isValue() && access.key.isValue() && isArrayIndexInTypedElements(access.typeOfKey);
    std::optional<uint64_t> readBefore = freshListReadUnderKey ? findKnown(walker, KnownNote::ElementReadOffFreshList, freshListReadUnderKey, access.key)
        : isReadUnderNumber || readsTestedList ? findKnown(walker, KnownNote::ElementReadUnderNumber, access.base, access.key) : std::nullopt;
    if (readBefore && (freshListReadUnderKey || walker.types().isSubset(walker.typeOf(Reference::fromBits(static_cast<uint32_t>(*readBefore))), walker.resultTypeOf(read)))) {
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
    bool takesNaNForHole = tested && read.arrayStorage() == ArrayStorage::Double && (read.isInBounds() ? !read.admitsInt32Storage() || tested->turnsStoreIntoLists : tested->turnsStoreIntoLists && isArrayIndexInTypedElements(access.typeOfKey)) && everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing);
    if (!takesNaNForHole && tested && tested->turnsStoreIntoLists && !tested->isWritable && isNumericStorage(read.arrayStorage()) && onlyUserHoldsItAgainstNumberInTypedElements(walker, access.value))
        takesNaNForHole = true;
    if (takesNaNForHole && tested->turnsStoreIntoLists)
        walker.count(StatisticID::StoringLoopHoleChecksRemoved);
    bool leavesTurnAtHole = !presentAtHead && !takesNaNForHole && tested && canGuardAccess(access) && (tested->onlyHoldsRows || holeLeavesTurnBehindRow(walker, access.value, *tested));
    bool findsNothingOutsideList = read.isInBounds() || isArrayIndexInTypedElements(access.typeOfKey);
    bool canTakeNaN = !leavesTurnAtHole && tested && findsNothingOutsideList && isNumericStorage(read.arrayStorage());
    std::optional<Walker::CallOfCopy> computingCaller = canTakeNaN ? findComputingCaller(walker, access.value) : std::nullopt;
    if (canTakeNaN && (computingCaller || everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing)))
        read.setReadsHoleAsNaN(true);
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    bool takesNumber = !isNumericStorage(read.arrayStorage()) || read.readsHoleAsNaN() || leavesTurnAtHole;
    std::optional<PassedTest> presentInFront = rowIsPresent ? rowIsPresent : tested && takesNumber && access.typeOfKey.isInteger() && !presentAtHead ? tryHoistElementPresenceCheck(walker, access.value, *tested, access.base, access.key) : std::nullopt;
    if (tested && presentAtHead) {
        read.testsPassed = std::nullopt;
        read.setReadsHoleAsNaN(false);
        read.setIsInBounds(*presentAtHead);
        read.setIsPresent(*presentAtHead);
    }
    if (presentInFront && read.readsHoleAsNaN())
        read.setReadsHoleAsNaN(false);
    bool givesNaNForHole = read.readsHoleAsNaN();
    if (!givesNaNForHole && tested && !tested->isWritable && read.arrayStorage() == ArrayStorage::Double && read.admitsInt32Storage() && walker.isQuietVersionOfUnswitchLoop())
        walker.noteInt32Candidate();
    Reference element = leavesTurnAtHole ? walker.state().element(access.base, access.key) : Reference();
    if (presentInFront) {
        if (std::exchange(read.testsPassed, std::nullopt))
            read.setIsInBounds(*presentInFront);
        read.setIsPresent(*presentInFront);
    }
    bool isReadInFront = presentInFront && !isNeverType(walker.resultTypeOf(read));
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
        if (leavesTurnAtHole && !isReadInFront && !guardElementPresenceInIteration(walker, read, access.base, access.key, *tested))
            return true;
        if (read.isPresent())
            read.setListHasNoHoles(false);
        if (isReadInFront && walker.checksBounds())
            readAgain = read;
        Walker::SpecializedPreheaderScope preheaderScope(walker, isReadInFront, walker.arrayGuardPurity());
        WhatListWasMadeWith held = namesStorage && read.isPresent() && read.arrayStorage() == ArrayStorage::Contiguous ? whatListWasMadeWith(walker, access.base) : WhatListWasMadeWith();
        auto typeAsBuilt = [&](ValueID input) {
            Reference built = walker.referenceOf(input);
            return built ? walker.typeOf(built) : TypeID();
        };
        bool takesEveryStorage = tested && tested->storage == ArrayStorage::Other && !held.proven && walker.walksCopyWithoutSomeIterationGuesses() && walker.depthOfCopies() && access.base.isValue() && canGuardAccess(access) && valueIsOnlyReckonedWithAsNumber(walker, access.value.id(), TypesOfInputs(typeAsBuilt));
        element = held.proven ? walker.buildReadTypedByWhatIsRemembered(WTF::move(read), held.proven, held.shown) : takesEveryStorage ? buildTestedRead(access, WTF::move(read)) : walker.build(WTF::move(read));
    }
    if (isReadInFront && !element && !walker.isUnreachable()) {
        walker.endAsUnreachable(walker.originOf(access.value));
        return true;
    }
    if (readAgain && element && element.isValue()) {
        WalkState believed = walker.state();
        walker.checkThatMemoryAnswersAsBelieved(element, WTF::move(*readAgain));
        walker.state() = WTF::move(believed);
    }
    if (givesNaNForHole && computingCaller && element) {
        walker.noteHoleForCaller(*computingCaller, element);
        walker.count(StatisticID::ReturnedHolesReadAsNaN);
    }
    if (leavesTurnAtHole && isNumericStorage(tested->storage) && element && walker.types().at(walker.typeOf(element)).primitives & Primitive::Undefined) {
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
        noteKnown(walker, KnownNote::ElementReadOffFreshList, freshListReadUnderKey, access.key, 0, elementIsKnownToBeThere ? LocationMasks(elementGuardHeaps) : LocationMasks::top().without(unobservableWriteHeaps), element.bits());
    if (readsTestedList && !givesNaNForHole && element && element.isValue()) {
        LocationMasks places(elementGuardHeaps - AbstractHeapSet { AbstractHeap::Elements });
        places.add(AbstractHeap::Elements, cellHoldingElementsMask);
        noteKnown(walker, KnownNote::ElementReadUnderNumber, access.base, access.key, 0, places, element.bits());
    }
    if (isReadUnderNumber && !givesNaNForHole && element && element.isValue())
        noteKnown(walker, KnownNote::ElementReadUnderNumber, access.base, access.key, 0, LocationMasks(elementGuardHeaps), element.bits());
    element = numberMadeOnceOfElementInTypedElements(walker, access.value, element);
    if (tested && !isReadInFront)
        element = narrowTruncatedElement(access, *tested, element);
    if (element || !walker.isUnreachable())
        walker.bind(access.value.id(), element);
    return true;
}

Reference findElementRead(Walker& walker, Reference array, Reference position)
{
    std::optional<uint64_t> known = findKnown(walker, KnownNote::ElementReadUnderNumber, array, position);
    return known ? Reference::fromBits(static_cast<uint32_t>(*known)) : Reference();
}

void renameElementRead(Walker& walker, Reference array, Reference position, Reference name)
{
    noteKnown(walker, KnownNote::ElementReadUnderNumber, array, position, 0, LocationMasks(elementGuardHeaps), name.bits());
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
    requestStorageForStoredValue(access);
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

static bool continueIfKeyIsNonNegative(Walker& walker, Reference key, Origin origin)
{
    Draft zero(Opcode::Integer, origin);
    zero.setInteger(0);
    Draft isNotNegative(Opcode::GreaterEq, origin);
    walker.chooseConservativeFields(isNotNegative);
    isNotNegative.setLeftOperand(key);
    isNotNegative.setRightOperand(walker.constant(WTF::move(zero)));
    isNotNegative.setForm(operatorFormFor(walker, isNotNegative));
    return continueOrRestartIteration(walker, walker.build(WTF::move(isNotNegative)), true);
}

static bool testNextStoreKey(const AccessOfTypedElements& access, Origin origin)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.value.id()));
    uint32_t next = access.value.id().index() + 1;
    uint32_t end = block.firstValue().index() + block.numberOfValues();
    while (next < end && index.opcodeOf(ValueID(next)) != Opcode::ElemSet)
        ++next;
    if (next >= end)
        return true;
    BodyValue later = index.value(ValueID(next));
    ValueID written = index.stripCasts(later.propertyOperand());
    if (written.index() > access.value.id().index())
        return true;
    Reference key = walker.referenceOf(later.propertyOperand());
    Reference list = nameOfListThatEntersLoopAround(walker, index, later);
    std::optional<HoistedArrayGuard> tested = key && key.isRecord() && key != access.key && list ? findListTestedForStoresInFrontOfLoop(walker, list) : std::nullopt;
    if (!tested || !tested->isWritable)
        return true;
    Type typeOfKey = walker.types().at(walker.typeOf(key));
    if (!typeOfKey.isInteger() || positionIsKnownToLieInsideList(walker, list, key))
        return true;
    if (isUntouchedLiteral(index, later.objectOperand(), index.loopOf(block.id())) && storesUnderSteppedCounter(walker, index, later))
        return true;
    if (typeOfKey.integers.minimum < 0 && !continueIfKeyIsNonNegative(walker, key, origin))
        return false;
    Draft length(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setArrayOperand(tested->name);
    Reference held = walker.build(WTF::move(length));
    if (!held)
        return false;
    Draft lengthComparison(Opcode::Less, origin);
    walker.chooseConservativeFields(lengthComparison);
    lengthComparison.setLeftOperand(key);
    lengthComparison.setRightOperand(held);
    lengthComparison.setForm(operatorFormFor(walker, lengthComparison));
    Reference test = walker.build(WTF::move(lengthComparison));
    if (!test || !test.isValue())
        return !walker.isUnreachable() && !walker.hasFailed();
    if (!continueOrRestartIteration(walker, test, true))
        return false;
    noteKnown(walker, KnownNote::LengthOfList, list, { }, 0, placesUnderLengthsOfLists, held.bits());
    walker.count(StatisticID::LaterStoreBoundsChecksHoisted);
    return true;
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
        if (isUntouchedLiteral(index, later.objectOperand(), index.loopOf(index.blockOf(later.id()))) && storesUnderSteppedCounter(walker, index, later))
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
    return testNextStoreKey(access, origin);
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

static bool isReadOffListInTypedElements(const BodyIndex& index, const BodyValue& store)
{
    if (store.opcode() != Opcode::ElemSet || store.id().index() >= index.numberOfValues() || index.value(store.id()).words() != store.words())
        return false;
    ValueID row = index.stripCasts(store.objectOperand());
    return row && !index.isParameter(row) && index.opcodeOf(row) == Opcode::ElemGet;
}

static bool isNamedList(const BodyIndex& index, const BodyValue& store)
{
    if (store.opcode() != Opcode::ElemSet || store.id().index() >= index.numberOfValues() || index.value(store.id()).words() != store.words())
        return false;
    ValueID list = index.stripCasts(store.objectOperand());
    return list && !index.isParameter(list) && index.opcodeOf(list) == Opcode::PropGet && !index.value(list).isFreeName();
}

static bool turnStoresAgainBehindInTypedElements(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return true;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    if (BlockID unrolled = access.walker.unrolledLoopHeader(); loop && unrolled && index.loopOf(unrolled) == loop)
        return true;
    return !loop || access.walker.lastStoreByKeyOfLoopInCurrentBody(loop) > access.value.id().index();
}

static bool readsNamesAfterStore(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    return loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            return operation.opcode() == Opcode::PropGet && operation.id().index() > access.value.id().index() && siteMayAnswerReadWithoutOtherCode(access.walker, operation);
        });
    });
}

static bool readsRowsAfterStore(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    return loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            if (operation.opcode() != Opcode::ElemGet || operation.id().index() < access.value.id().index())
                return false;
            ValueID row = index.stripCasts(operation.objectOperand());
            return row && !index.isParameter(row) && index.opcodeOf(row) == Opcode::ElemGet;
        });
    });
}

static bool mayStoreAgainInIteration(const AccessOfTypedElements& access)
{
    if (!turnStoresAgainBehindInTypedElements(access))
        return false;
    const BodyIndex& index = access.walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return true;
    BlockID stored = index.blockOf(access.value.id());
    LoopID loop = index.loopOf(stored);
    if (!loop)
        return true;
    std::span<const BlockID> members = index.membersOf(loop);
    auto placeOf = [&](BlockID block) {
        return static_cast<size_t>(std::ranges::lower_bound(members, block.index(), { }, [](BlockID member) { return member.index(); }) - members.begin());
    };
    BitVector isReached(members.size());
    isReached.set(placeOf(stored));
    for (size_t place = placeOf(stored); place < members.size(); ++place) {
        if (!isReached.get(place))
            continue;
        BodyBlock block = index.block(members[place]);
        for (BodyValue operation : block.values()) {
            if (operation.opcode() == Opcode::ElemSet && (block.id() != stored || operation.id().index() > access.value.id().index()))
                return true;
        }
        for (BodyTarget target : block.terminator().targets()) {
            if (!index.isInLoop(target.block(), loop) || index.isLoopHead(target.block()))
                continue;
            if (placeOf(target.block()) <= place)
                return true;
            isReached.set(placeOf(target.block()));
        }
    }
    return false;
}

static bool mayBeNumberByText(const BodyIndex& index, ValueID value)
{
    static constexpr OpcodeSet numbers = allNumericOperators | allBitwiseOperators | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::ElemGet, Opcode::Increment, Opcode::Decrement, Opcode::Negate, Opcode::ToNumeric };
    ValueID made = index.stripCasts(value);
    if (!made || index.isParameter(made))
        return !!made;
    Opcode opcode = index.opcodeOf(made);
    if (opcode == Opcode::Call) {
        ValueID callee = index.stripCasts(index.value(made).as<Opcode::Call>().calleeOperand());
        return callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::Builtin;
    }
    if (!numbers.contains(opcode))
        return false;
    for (uint32_t side = 0; opcode == Opcode::Add && side < 2; ++side) {
        ValueID addend = index.stripCasts(index.value(made).operand(side));
        if (addend && !index.isParameter(addend) && index.opcodeOf(addend) == Opcode::String)
            return false;
    }
    return true;
}

static bool pushesOntoUntouchedLiteral(const BodyIndex& index, const Program& program, const BodyValue& call)
{
    auto called = call.as<Opcode::Call>();
    ValueID literal = called.thisValueOperand() ? index.stripCasts(called.thisValueOperand()) : ValueID();
    if (!literal || index.isParameter(literal) || (index.opcodeOf(literal) == Opcode::NewArray && !std::ranges::all_of(index.value(literal).as<Opcode::NewArray>().elementsOperands(), [&](ValueID element) {
        ValueID written = index.stripCasts(element);
        return written && !index.isParameter(written) && (index.opcodeOf(written) == Opcode::Integer || index.opcodeOf(written) == Opcode::Double);
    })))
        return false;
    ValueID member = called.argumentsOperands().size() == 1 ? index.stripCasts(called.calleeOperand()) : ValueID();
    return member && !index.isParameter(member) && index.opcodeOf(member) == Opcode::PropGet && !index.value(member).isFreeName() && index.value(member).key() == program.keyOf(BuiltinNames::push) && mayBeNumberByText(index, called.argumentsOperands()[0]) && isUntouchedLiteral(index, called.thisValueOperand(), index.loopOf(index.blockOf(call.id())));
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
    if (std::span<const uint32_t> pushes = pushesOntoLiteralsOf(loop); !pushes.empty())
        answer = std::max(answer, pushes.back());
    return answer;
}

std::span<const uint32_t> Walker::pushesOntoLiteralsOf(LoopID loop)
{
    const BodyIndex& index = currentBodyIndex();
    Vector<std::optional<Vector<uint32_t>>>& ofLoops = m_pushesOntoLiterals.ensure(&index, [&] {
        Vector<std::optional<Vector<uint32_t>>> none;
        none.grow(index.numberOfLoops());
        return none;
    }).iterator->value;
    std::optional<Vector<uint32_t>>& pushes = ofLoops[loop.index()];
    if (pushes)
        return pushes->span();
    pushes.emplace();
    if (!index.opcodes().contains(Opcode::Call) || !index.opcodes().contains(Opcode::ElemGet))
        return { };
    Vector<LoopID, 4> loopsThatRead;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemGet)
                loopsThatRead.appendIfNotContains(index.loopOf(member));
            else if (operation.opcode() == Opcode::Call && pushesOntoUntouchedLiteral(index, program(), operation))
                pushes->append(operation.id().index());
        }
    }
    pushes->removeAllMatching([&](uint32_t call) { return !loopsThatRead.contains(index.loopOf(index.blockOf(ValueID(call)))); });
    std::ranges::sort(*pushes);
    return pushes->span();
}

bool Walker::isPushOntoLiteral(const BodyValue& call)
{
    const BodyIndex& index = currentBodyIndex();
    if (call.opcode() != Opcode::Call || call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    return loop && std::ranges::binary_search(pushesOntoLiteralsOf(loop), call.id().index());
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
    ValueID key = access.counterThatPushGoesBy || access.isPushAtLength ? access.counterThatPushGoesBy : index.stripCasts(access.value.propertyOperand());
    return key && place <= head.numberOfParameters() && key == head.parameter(place - 1) ? std::optional(place) : std::nullopt;
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

static bool checkCapacityForExtension(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, Origin origin, std::optional<PassedTest>& capacityProof)
{
    Walker& walker = access.walker;
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
    return true;
}

static bool buildLengthExtension(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, Origin origin, const std::optional<PassedTest>& capacityProof, std::optional<PassedTest>& newBoundsProof)
{
    Walker& walker = access.walker;
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

static bool mayAppendToLiteral(const AccessOfTypedElements& access, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!tested.name.isValue() || access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    if (!loop || !isUntouchedLiteral(index, access.value.objectOperand(), loop))
        return false;
    const Operation& made = walker.code().at(underlyingCellOf(walker.code(), tested.name.id()));
    const auto* written = made.opcode == Opcode::Data && made.data() ? walker.program().at(made.data()).root.tryAs<DataNode::Array>() : nullptr;
    if (!written && (made.opcode != Opcode::NewArray || made.storage() != AllocationStorage::Heap || !made.holes().empty()))
        return false;
    size_t length = written ? written->elements.size() : made.elementsOperands().size();
    if (access.typeOfKey.integers.maximum < static_cast<int64_t>(length))
        return false;
    return keyIsLengthOfListItselfInTypedElements(access) || storesUnderSteppedCounter(walker, index, access.value);
}

static bool isPresentInStoredArray(const AccessOfTypedElements& access)
{
    if (!access.stored || !access.stored.isValue() || !access.base.isRecord())
        return false;
    const Code& code = access.walker.code();
    ValueID stored = access.stored.id();
    while (valuePreservingOpcodes.contains(code.at(stored).opcode))
        stored = code.at(stored).operand(0);
    const Operation& read = code.at(stored);
    return read.opcode == Opcode::GetArrayElement && read.arrayStorage() == ArrayStorage::Contiguous && read.isPresent() && underlyingCellOf(code, read.arrayOperand()) == underlyingCellOf(code, access.base.id());
}

static Reference hoistStoredNumber(const AccessOfTypedElements& access, Origin origin, bool& isChecked)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    Walker& walker = access.walker;
    if (!access.stored || !access.stored.isValue() || !access.typeOfStored || walker.representationOf(access.stored) != Representation::JSValue || !walker.isInvariantInSpecializedVersion(access.stored, purity))
        return access.stored;
    Reference raw;
    Reference test;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        if (walker.types().at(access.typeOfStored).isNumber()) {
            Draft unboxed(Opcode::Unbox, origin);
            unboxed.setOperand(0, access.stored);
            unboxed.representation = Representation::Double;
            raw = walker.build(WTF::move(unboxed));
        } else {
            Draft read(Opcode::NumberOrNaN, origin);
            read.setValueOperand(access.stored);
            raw = walker.build(WTF::move(read));
        }
        if (!raw || !raw.isValue())
            return access.stored;
        Draft isItself(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(isItself);
        isItself.setLeftOperand(raw);
        isItself.setRightOperand(raw);
        isItself.setForm(operatorFormFor(walker, isItself));
        test = walker.build(WTF::move(isItself));
    }
    isChecked = test && walker.tryBranchInPreheader(test, true, purity);
    return raw;
}

static Reference placeholderForAppend(Walker& walker, ArrayStorage storage, Origin origin)
{
    if (storage == ArrayStorage::Int32) {
        Draft placeholder(Opcode::Integer, origin);
        placeholder.setInteger(0);
        return walker.constant(WTF::move(placeholder));
    }
    Draft placeholder(Opcode::Double, origin);
    placeholder.setNumber(0.5);
    return walker.constant(WTF::move(placeholder));
}

static Draft draftOfStoredElement(const AccessOfTypedElements& access, Opcode opcode, Origin origin)
{
    if (access.value.opcode() != Opcode::Call)
        return draftOfTypedElements(access.walker, access.value, opcode);
    Draft put(opcode, origin);
    access.walker.chooseConservativeFields(put);
    return put;
}

static bool buildRoomAtEnd(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, Origin origin, const PassedTest& fromZeroUp, std::optional<PassedTest>& room)
{
    Walker& walker = access.walker;
    auto compareWithCapacity = [&] {
        Draft capacity(Opcode::GetArrayCapacity, origin);
        walker.chooseConservativeFields(capacity);
        capacity.setArrayOperand(tested.name);
        Reference held = walker.build(WTF::move(capacity));
        Reference isWithin = held ? buildIntegerOperator(walker, Opcode::Less, access.key, held, origin) : Reference();
        return std::pair { held, isWithin };
    };
    auto integer = [&](int64_t number) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(number);
        return walker.constant(WTF::move(written));
    };
    Reference isWithinCapacity = compareWithCapacity().second;
    if (!isWithinCapacity || !isWithinCapacity.isValue() || walker.isUnreachable())
        return false;
    BlockID hasRoom = walker.addBlock();
    BlockID isFull = walker.addBlock(BlockFrequency::Unlikely);
    walker.branch(isWithinCapacity, hasRoom, isFull);
    walker.continueIn(isFull);
    if (!walker.isUnreachable()) {
        Draft grow(Opcode::ReserveArrayCapacity, origin);
        walker.chooseConservativeFields(grow);
        grow.setArrayOperand(tested.name);
        grow.setCapacityOperand(buildIntegerOperator(walker, Opcode::Add, buildIntegerOperator(walker, Opcode::Add, access.key, buildIntegerOperator(walker, Opcode::URShift, access.key, integer(1), origin), origin), integer(4), origin));
        walker.build(WTF::move(grow));
        walker.jumpTo(hasRoom);
    }
    walker.continueIn(hasRoom);
    if (walker.isUnreachable() || walker.hasFailed())
        return false;
    auto [capacity, isWithin] = compareWithCapacity();
    if (!isWithin || !continueOrRestartIteration(walker, isWithin, true))
        return false;
    if (std::optional<PassedTest> below = walker.findPassedTest(isWithin))
        room = walker.boundsProofFromChecks(access.base, access.key, capacity, fromZeroUp, *below);
    walker.count(StatisticID::StoresGrowingArrays);
    return !walker.isUnreachable() && !walker.hasFailed();
}

static bool buildAppendingArm(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, Origin origin, Opcode opcode, Reference stored, Reference length, const PassedTest& fromZeroUp, bool goesByKind, bool isUndoable, BlockID& behind, std::optional<PassedTest>& inside)
{
    Walker& walker = access.walker;
    if (walker.isUnreachable() || walker.hasFailed())
        return false;
    auto integer = [&](int64_t number) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(number);
        return walker.constant(WTF::move(written));
    };
    auto readCapacity = [&] {
        Draft capacity(Opcode::GetArrayCapacity, origin);
        walker.chooseConservativeFields(capacity);
        capacity.setArrayOperand(tested.name);
        return walker.build(WTF::move(capacity));
    };
    auto positionBelow = [&](Reference bound, Reference isBelow) {
        std::optional<PassedTest> below = walker.findPassedTest(isBelow);
        return below ? walker.boundsProofFromChecks(access.base, access.key, bound, fromZeroUp, *below) : std::nullopt;
    };
    auto append = [&](Reference capacity, Reference isWithinCapacity) {
        Draft extend(Opcode::ExtendArrayLength, origin);
        walker.chooseConservativeFields(extend);
        extend.setArrayOperand(access.base);
        extend.setPositionOperand(access.key);
        if (std::optional<PassedTest> proof = positionBelow(capacity, isWithinCapacity))
            extend.restOn(*proof);
        std::optional<PassedTest> below = walker.boundsProofAfterExtension(access.base, access.key, walker.build(WTF::move(extend)));
        Draft put = draftOfStoredElement(access, opcode, origin);
        put.setArrayOperand(access.base);
        put.setPositionOperand(access.key);
        put.setValueOperand(stored);
        if (below)
            put.setIsInBounds(*below);
        if (goesByKind)
            put.setGoesByKindOfStorage(true);
        if (opcode == Opcode::PutContiguousIndex && isNumericStorage(tested.storage))
            put.setHasNumericStorage(true);
        if (walker.build(WTF::move(put)); walker.isUnreachable())
            return;
        Reference nothing = isUndoable ? placeholderForAppend(walker, tested.storage, origin) : Reference();
        walker.jumpTo(behind, nothing ? singleElementSpan(nothing) : std::span<const Reference>());
    };

    bool isAlwaysAtEnd = access.key == length;
    Reference isAtEnd = isAlwaysAtEnd ? Reference() : buildIntegerOperator(walker, Opcode::StrictEq, access.key, length, origin);
    if (!isAlwaysAtEnd && !isAtEnd)
        return false;
    BlockID atEnd = isAlwaysAtEnd ? BlockID() : walker.addBlock();
    BlockID elsewhere = isAlwaysAtEnd ? BlockID() : walker.addBlock();
    behind = walker.addBlock();
    if (isUndoable)
        walker.addParameter(behind, tested.storage == ArrayStorage::Int32 ? TypeID(TypeTable::int32) : TypeID(TypeTable::number), tested.storage == ArrayStorage::Int32 ? Representation::Int32 : Representation::Double);
    if (!isAlwaysAtEnd) {
        walker.branch(isAtEnd, atEnd, elsewhere, true);
        walker.continueIn(atEnd);
    }
    if (!walker.isUnreachable()) {
        Reference capacity = readCapacity();
        Reference isWithinCapacity = capacity ? buildIntegerOperator(walker, Opcode::Less, access.key, capacity, origin) : Reference();
        if (isWithinCapacity && isWithinCapacity.isValue() && !walker.isUnreachable()) {
            BlockID hasRoom = walker.addBlock();
            BlockID isFull = walker.addBlock(BlockFrequency::Unlikely);
            walker.branch(isWithinCapacity, hasRoom, isFull, true);
            walker.continueIn(isFull);
            Draft grow(Opcode::ReserveArrayCapacity, origin);
            walker.chooseConservativeFields(grow);
            grow.setArrayOperand(tested.name);
            grow.setCapacityOperand(buildIntegerOperator(walker, Opcode::Add, buildIntegerOperator(walker, Opcode::Add, access.key, buildIntegerOperator(walker, Opcode::URShift, access.key, integer(1), origin), origin), integer(4), origin));
            walker.build(WTF::move(grow));
            Reference grown = readCapacity();
            Reference isWithinGrown = grown ? buildIntegerOperator(walker, Opcode::Less, access.key, grown, origin) : Reference();
            if (isWithinGrown && continueOrRestartIteration(walker, isWithinGrown, true))
                append(grown, isWithinGrown);
            walker.continueIn(hasRoom);
            append(capacity, isWithinCapacity);
        }
    }
    if (walker.hasFailed())
        return false;
    walker.count(StatisticID::StoresGrowingArrays);
    if (isAlwaysAtEnd)
        return true;
    walker.continueIn(elsewhere);
    Reference isWithinLength = walker.isUnreachable() ? Reference() : buildIntegerOperator(walker, Opcode::Less, access.key, length, origin);
    if (!isWithinLength || !continueOrRestartIteration(walker, isWithinLength, true))
        return !walker.hasFailed();
    inside = positionBelow(length, isWithinLength);
    return true;
}

static bool buildStoreBehindRowOfTypedElements(const AccessOfTypedElements&);

static bool expectsStoredNumber(const AccessOfTypedElements& access, const std::optional<HoistedArrayGuard>& tested)
{
    TypeArena& types = access.walker.types();
    return tested && tested->isWritable && isNumericStorage(tested->storage) && !access.counterThatPushGoesBy && access.stored && access.stored.isValue() && access.typeOfStored && types.overlap(access.typeOfStored, TypeTable::number)
        && access.walker.representationOf(access.stored) == Representation::JSValue && canGuardAccess(access);
}

static Reference readAsNumber(const AccessOfTypedElements& access)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    Walker& walker = access.walker;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference value = access.stored;
    if (value.isValue() && !walker.isInvariantInSpecializedVersion(value, purity)) {
        Reference read = Reference::value(walker.code().valueBehindNames<valuePreservingOpcodes>(value.id()));
        if (walker.representationOf(read) == Representation::JSValue && walker.isInvariantInSpecializedVersion(read, purity))
            value = read;
    }
    Walker::SpecializedPreheaderScope preheaderScope(walker, walker.isInvariantInSpecializedVersion(value, purity), purity);
    Draft read(Opcode::NumberOrNaN, origin);
    read.setValueOperand(value);
    return walker.build(WTF::move(read));
}

static bool buildStoreBehindRowOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (!access.typeOfKey.isInteger())
        return false;
    bool storesNumber = access.typeOfStored && types.at(access.typeOfStored).isNumber();
    bool isPush = access.counterThatPushGoesBy || access.isPushAtLength;
    std::optional<HoistedArrayGuard> tested = isPush ? findListTestedForStoresInFrontOfLoop(walker, access.base) : findHoistedArrayGuard(walker, access.value);
    bool hasBoxedElements = tested && tested->isWritable && tested->storage == ArrayStorage::Contiguous;
    bool storesByAnyKind = tested && tested->isWritable && tested->storage == ArrayStorage::Other && tested->admitsInt32Storage && (!isPush || access.isPushAtLength) && access.typeOfStored && !types.isSubset(access.typeOfStored, TypeTable::int32);
    bool storesInt32AmongBoxes = storesNumber && hasBoxedElements && !isPush && !tested->admitsInt32Storage && types.isSubset(access.typeOfStored, TypeTable::int32);
    if (storesNumber ? hasBoxedElements && !storesInt32AmongBoxes : (isPush && !access.isPushAtLength) || !access.typeOfStored || (types.at(access.typeOfStored).primitives & Primitive::Absent))
        return false;
    bool rowAsksForWritableList = tested && tested->isWritable;
    if (!storesNumber && !hasBoxedElements && !storesByAnyKind) {
        if (!expectsStoredNumber(access, tested))
            return false;
        if (tested->storage != ArrayStorage::Double || tested->admitsInt32Storage || !walker.isInvariantInSpecializedVersion(access.stored, Walker::IterationPurity::KnownEffectsOnly)) {
            AccessOfTypedElements number = access;
            number.typeOfStored = TypeTable::number;
            if (!access.isOnlyAskedAbout && tested->storage == ArrayStorage::Double)
                number.stored = readAsNumber(access);
            return number.stored && buildStoreBehindRowOfTypedElements(number);
        }
    }
    if (isPush && !rowAsksForWritableList)
        return false;
    if (!rowAsksForWritableList && !access.isOnlyAskedAbout) {
        ArrayStorage asked = types.at(access.typeOfStored).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Int32;
        bool isRowReadForEitherKind = tested && tested->storage == ArrayStorage::Double && tested->admitsInt32Storage && (isReadOffListInTypedElements(walker.currentBodyIndex(), access.value) || isNamedList(walker.currentBodyIndex(), access.value));
        if (tested && tested->storage == ArrayStorage::Other && isNamedList(walker.currentBodyIndex(), access.value))
            tested = std::nullopt;
        bool mayGoByKind = (!tested || (isRowReadForEitherKind && !mayStoreAgainInIteration(access) && (types.isSubset(access.typeOfStored, TypeTable::int32) || canGuardAccess(access)))) && (asked == ArrayStorage::Double || types.isSubset(access.typeOfStored, TypeTable::int32)) && walker.isInvariantInSpecializedVersion(access.base) && (isRowReadForEitherKind || !textOfLoopsReadsListByKeyInTypedElements(walker.currentBodyIndex(), access.value));
        tested = tryHoistArrayGuardForAccess(walker, access.value, access.base, asked, mayGoByKind);
    }
    bool isCheckedInIteration = !isPush && access.base.isValue() && (!walker.isInvariantInSpecializedVersion(access.base) || isReadOffListInTypedElements(walker.currentBodyIndex(), access.value));
    if (!tested && !access.isOnlyAskedAbout && storesNumber && isCheckedInIteration && walker.canGuardAfterStores() && !isFreshPristineArray(walker, access.base)) {
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        Draft kind(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(kind);
        kind.setArrayOperand(access.base);
        std::optional<HoistedArrayGuard> askedForReads = findHoistedArrayGuard(walker, access.base);
        bool isWhole = !(types.at(access.typeOfStored).primitives & Primitive::FractionalNumber) && (askedForReads ? askedForReads->storage == ArrayStorage::Int32 : rowsHoldIntegers(walker, access.value));
        bool isExactDouble = !isWhole && askedForReads && askedForReads->storage == ArrayStorage::Double && !askedForReads->admitsInt32Storage;
        kind.setArrayStorage(isWhole ? ArrayStorage::Int32 : ArrayStorage::Double);
        kind.setAdmitsInt32Storage(!isWhole && !isExactDouble);
        kind.setIsWritable(true);
        if (!guardPristineArrayInIteration(walker, access.base, origin) || !continueOrRestartIteration(walker, walker.build(WTF::move(kind))))
            return true;
        tested = HoistedArrayGuard { access.base, isWhole ? ArrayStorage::Int32 : ArrayStorage::Double, !isWhole && !isExactDouble, true };
        walker.recordSpeculation();
        walker.count(StatisticID::ArraysCheckedAtStores);
    }
    bool goesByKind = tested && tested->isWritable && (tested->storage == ArrayStorage::Other || (tested->storage == ArrayStorage::Double && tested->admitsInt32Storage));
    bool storesNumberByKind = goesByKind && !storesByAnyKind && tested->admitsInt32Storage && !types.isSubset(access.typeOfStored, TypeTable::int32);
    if (storesNumberByKind && ((isPush && !access.isPushAtLength) || !canGuardAccess(access)))
        return false;
    if (!tested || !tested->isWritable || (!goesByKind && !hasBoxedElements && (!isNumericStorage(tested->storage) || tested->admitsInt32Storage)))
        return false;

    if (!access.isOnlyAskedAbout && !isPush && access.typeOfKey.isInteger())
        walker.noteLoweredCounter(access.value);
    bool mayAppend = access.isPushAtLength || (!isPush && mayAppendToLiteral(access, *tested));
    if (!access.isOnlyAskedAbout && rowAsksForWritableList && !isPush && !mayAppend && !notedCounterPosition(access, KnownNote::LoopAppendsToLiteral, tested->name))
        hoistArrayBoundsCheck(access);
    std::optional<PassedTest> inside = access.isOnlyAskedAbout ? std::nullopt : walker.findWhatHoldsPositionInsideList(access.base, access.key);
    bool isInside = !!inside;
    bool turnAsksForBounds = rowAsksForWritableList || (!isPush && !access.isOnlyAskedAbout && walker.depthOfCopies() && canGuardAccess(access));
    if (!isInside && !isArrayIndexInTypedElements(access.typeOfKey) && !mayAppend && (!turnAsksForBounds || keyIsLengthOfListItselfInTypedElements(access)))
        return false;
    bool fits = canStoreWithoutConversion(types, goesByKind ? ArrayStorage::Double : tested->storage, access.typeOfStored);
    bool isMovedOutOfDoubleStorage = !fits && (goesByKind || tested->storage == ArrayStorage::Double) && isReadOutOfDoubleStorageInTypedElements(walker, access.stored);
    if (isMovedOutOfDoubleStorage)
        fits = true;
    if (walker.shouldAssumeNoNaNStores() && (goesByKind || tested->storage == ArrayStorage::Double) && types.at(access.typeOfStored).isNumber())
        fits = true;
    fits |= storesByAnyKind;
    bool mayHoldOnlyInt32s = hasBoxedElements && tested->admitsInt32Storage && !isPresentInStoredArray(access);
    if ((!isInside || !fits || mayHoldOnlyInt32s || storesByAnyKind) && !canGuardAccess(access))
        return false;
    Opcode opcode = storesNumberByKind || (tested->storage == ArrayStorage::Double && !goesByKind) ? Opcode::PutDoubleIndex : Opcode::PutContiguousIndex;
    bool appendsToLiteral = !isInside && !storesNumberByKind && !storesByAnyKind && notedCounterPosition(access, KnownNote::LoopAppendsToLiteral, tested->name) && literalMayHoldStorageAskedInTypedElements(walker, *tested);
    bool asksForKind = storesNumberByKind || storesByAnyKind || mayHoldOnlyInt32s;
    mayAppend &= !isInside && (!asksForKind || keyIsLengthOfListItselfInTypedElements(access)) && !appendsToLiteral && !walker.shouldSkipLengthChecks();
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, appendsToLiteral || mayAppend ? Opcode::ExtendArrayLength : opcode, KnowledgeLevel::Proven);

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
    Reference lengthOfArray;
    std::optional<PassedTest> fromZeroUpOfAppend;
    bool extendsLength = appendsToLiteral && !turnStoresAgainBehindInTypedElements(access) && walker.canRestartIteration() && notedCounterPosition(access, KnownNote::LengthEqualsInitialCounter, tested->name);
    if (isPush && !extendsLength && !mayAppend)
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
        std::optional<PassedTest> fromZeroUp = isKeyNonNegative ? walker.findPassedTest(isKeyNonNegative) : walker.findNonNegativeProof(access.key);
        mayAppend &= !asksForKind || access.key == currentLength;
        mayAppend &= fromZeroUp && access.key.isValue() && currentLength.isValue() && !(walker.code().isConstant(access.key.id()) && walker.code().isConstant(currentLength.id()));
        if (access.isPushAtLength && (!mayAppend || access.key != currentLength))
            return false;
        if (mayAppend) {
            lengthOfArray = currentLength;
            fromZeroUpOfAppend = fromZeroUp;
        } else {
            Reference isKeyWithinLength = walker.build(WTF::move(lengthComparison));
            if (!continueOrRestartIteration(walker, isKeyWithinLength, true))
                return true;
            std::optional<PassedTest> below = walker.findPassedTest(isKeyWithinLength);
            if (below && fromZeroUp)
                inside = walker.boundsProofFromChecks(access.base, access.key, currentLength, *fromZeroUp, *below);
        }
    }
    Reference stored = access.stored;
    if (!fits && tested->storage != ArrayStorage::Int32)
        stored = hoistStoredNumber(access, origin, fits);
    if (opcode == Opcode::PutDoubleIndex && !storesNumberByKind && stored && !types.at(walker.typeOf(stored)).isNumber())
        return false;
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

    std::optional<PassedTest> capacityProof;
    if (extendsLength && !checkCapacityForExtension(access, *tested, origin, capacityProof))
        return true;
    bool extendsBehindKind = extendsLength && asksForKind;
    if (extendsLength && !extendsBehindKind && !buildLengthExtension(access, *tested, origin, capacityProof, inside))
        return true;
    Reference valueBeforeStore;
    bool appendIsUndoable = mayAppend && !goesByKind && isNumericStorage(tested->storage) && walker.canRecordUndoableStore() && (turnStoresAgainBehindInTypedElements(access) || readsNamesAfterStore(access));
    uint32_t undoableStoreBefore = walker.state().newestUndoableStore();
    if (mayAppend) {
    } else if (opcode == Opcode::PutDoubleIndex && !goesByKind && walker.canRecordUndoableStore() && (turnStoresAgainBehindInTypedElements(access) || readsNamesAfterStore(access) || readsRowsAfterStore(access))) {
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
    } else if (opcode == Opcode::PutContiguousIndex && !goesByKind && !isPush && !extendsLength && tested->storage == ArrayStorage::Int32 && walker.canRecordUndoableStore() && (turnStoresAgainBehindInTypedElements(access) || readsNamesAfterStore(access) || readsRowsAfterStore(access))) {
        valueBeforeStore = walker.state().element(access.base, access.key);
        if (!valueBeforeStore || !types.isSubset(walker.typeOf(valueBeforeStore), TypeTable::int32)) {
            Draft isPresent(Opcode::IsElementPresent, origin);
            walker.chooseConservativeFields(isPresent);
            isPresent.setArrayOperand(access.base);
            isPresent.setPositionOperand(access.key);
            isPresent.setKeyIsIntegral(true);
            isPresent.setArrayStorage(ArrayStorage::Int32);
            if (inside)
                isPresent.setIsInBounds(*inside);
            takeStorageAndLengthFromInFrontOfLoop(walker, isPresent, *tested);
            if (!continueOrRestartIteration(walker, walker.build(WTF::move(isPresent))))
                return true;
            Draft read(Opcode::GetOwnInt32Index, origin);
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

    bool appendsBehindKind = mayAppend && asksForKind;
    std::optional<PassedTest> roomAtEnd;
    if (appendsBehindKind && !buildRoomAtEnd(access, *tested, origin, *fromZeroUpOfAppend, roomAtEnd))
        return true;
    auto append = [&] {
        if (extendsBehindKind) {
            buildLengthExtension(access, *tested, origin, capacityProof, inside);
            return;
        }
        Draft extend(Opcode::ExtendArrayLength, origin);
        walker.chooseConservativeFields(extend);
        extend.setArrayOperand(access.base);
        extend.setPositionOperand(access.key);
        if (roomAtEnd)
            extend.restOn(*roomAtEnd);
        inside = walker.boundsProofAfterExtension(access.base, access.key, walker.build(WTF::move(extend)));
    };
    BlockID behindKinds;
    auto askKind = [&](ArrayStorage storage) {
        Draft kind(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(kind);
        kind.setArrayOperand(access.base);
        kind.setArrayStorage(storage);
        kind.setIsWritable(true);
        return walker.build(WTF::move(kind));
    };
    auto storeNumber = [&](TypeID number, Opcode opcodeOfStore) {
        if (walker.isUnreachable())
            return;
        const BodyIndex& index = walker.currentBodyIndex();
        LoopID loop = index.loopOf(index.blockOf(access.value.id()));
        bool isReadInPreheader = opcodeOfStore == Opcode::PutDoubleIndex && loop && !index.loop(loop).parent && stored == access.stored && walker.representationOf(stored) == Representation::JSValue && walker.isInvariantInSpecializedVersion(stored, Walker::IterationPurity::KnownEffectsOnly);
        Reference value = isReadInPreheader ? readAsNumber(access) : Reference();
        if (!isReadInPreheader) {
            Reference isNumber = buildTestOfNumber(walker, stored, number, origin);
            if (!isNumber || !continueOrRestartIteration(walker, isNumber))
                return;
            value = buildProofOfNumber(walker, stored, isNumber, origin);
        }
        if (value && opcodeOfStore == Opcode::PutDoubleIndex && walker.representationOf(value) == Representation::JSValue) {
            Draft unboxed(Opcode::Unbox, origin);
            unboxed.setOperand(0, value);
            unboxed.representation = Representation::Double;
            value = walker.build(WTF::move(unboxed));
        }
        if (value && opcodeOfStore == Opcode::PutDoubleIndex) {
            Draft isItself(Opcode::StrictEq, origin);
            walker.chooseConservativeFields(isItself);
            isItself.setLeftOperand(value);
            isItself.setRightOperand(value);
            isItself.setForm(operatorFormFor(walker, isItself));
            if (!continueOrRestartIteration(walker, walker.build(WTF::move(isItself))))
                return;
        }
        if ((appendsBehindKind || extendsBehindKind) && value)
            append();
        Draft whole = draftOfStoredElement(access, opcodeOfStore, origin);
        whole.setArrayOperand(access.base);
        whole.setPositionOperand(access.key);
        whole.setValueOperand(value);
        if (inside)
            whole.setIsInBounds(*inside);
        if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
            whole.setStorageOperand(pointer);
        if (value && walker.build(WTF::move(whole)) && !walker.isUnreachable())
            walker.jumpTo(behindKinds);
    };
    if (mayAppend && !appendsBehindKind && !buildAppendingArm(access, *tested, origin, opcode, stored, lengthOfArray, *fromZeroUpOfAppend, goesByKind, appendIsUndoable, behindKinds, inside))
        return true;
    if (mayAppend && !appendsBehindKind && (walker.isUnreachable() || access.key == lengthOfArray)) {
        walker.continueIn(behindKinds);
        if (appendIsUndoable && !walker.isUnreachable())
            walker.recordUndoableAppend(access.base, access.key, Reference::value(walker.code().at(behindKinds).parameters()[0]), lengthOfArray, tested->storage, undoableStoreBefore);
        noteStoredElementInTypedElements(walker, access.base, access.key, stored);
        walker.bind(access.value.id(), Reference());
        walker.letRestOfStretchRunAsWrittenBehindFirstStore(access.value.id());
        return true;
    }
    Reference heldWithin;
    if (appendIsUndoable) {
        if (tested->storage == ArrayStorage::Int32) {
            Draft isPresent(Opcode::IsElementPresent, origin);
            walker.chooseConservativeFields(isPresent);
            isPresent.setArrayOperand(access.base);
            isPresent.setPositionOperand(access.key);
            isPresent.setKeyIsIntegral(true);
            isPresent.setArrayStorage(ArrayStorage::Int32);
            if (inside)
                isPresent.setIsInBounds(*inside);
            if (!continueOrRestartIteration(walker, walker.build(WTF::move(isPresent))))
                return true;
        }
        Draft read(tested->storage == ArrayStorage::Int32 ? Opcode::GetOwnInt32Index : Opcode::GetOwnDoubleIndex, origin);
        walker.chooseConservativeFields(read);
        read.setArrayOperand(access.base);
        read.setPositionOperand(access.key);
        heldWithin = walker.build(WTF::move(read));
        if (!heldWithin)
            return true;
    }
    if (storesByAnyKind) {
        Reference holdsAnyValues = askKind(ArrayStorage::Contiguous);
        if (!holdsAnyValues)
            return true;
        BlockID anyValues = walker.addBlock();
        BlockID numbersAlone = walker.addBlock();
        behindKinds = walker.addBlock();
        walker.branch(holdsAnyValues, anyValues, numbersAlone);
        walker.continueIn(numbersAlone);
        Reference holdsDoubles = walker.isUnreachable() ? Reference() : askKind(ArrayStorage::Double);
        if (holdsDoubles) {
            BlockID doubles = walker.addBlock();
            BlockID int32s = walker.addBlock();
            walker.branch(holdsDoubles, doubles, int32s);
            walker.continueIn(int32s);
            storeNumber(TypeTable::int32, Opcode::PutContiguousIndex);
            walker.continueIn(doubles);
            storeNumber(TypeTable::number, Opcode::PutDoubleIndex);
        }
        if (walker.hasFailed())
            return true;
        walker.continueIn(anyValues);
        if (walker.isUnreachable()) {
            walker.continueIn(behindKinds);
            noteStoredElementInTypedElements(walker, access.base, access.key, stored);
            walker.bind(access.value.id(), Reference());
            walker.letRestOfStretchRunAsWrittenBehindFirstStore(access.value.id());
            return true;
        }
        walker.count(StatisticID::ValuesStoredByAnyKind);
    }
    if (storesNumberByKind) {
        Draft kind(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(kind);
        kind.setArrayOperand(access.base);
        kind.setArrayStorage(ArrayStorage::Double);
        kind.setIsWritable(true);
        Reference holdsDoubles = walker.build(WTF::move(kind));
        if (!holdsDoubles)
            return true;
        BlockID doubles = walker.addBlock();
        BlockID boxes = walker.addBlock(BlockFrequency::Unlikely);
        behindKinds = walker.addBlock();
        walker.branch(holdsDoubles, doubles, boxes);
        walker.continueIn(boxes);
        if (!walker.isUnreachable()) {
            Reference isInt32 = buildTestOfNumber(walker, stored, TypeTable::int32, origin);
            if (continueOrRestartIteration(walker, isInt32)) {
                Reference int32 = buildProofOfNumber(walker, stored, isInt32, origin);
                if ((appendsBehindKind || extendsBehindKind) && int32)
                    append();
                Draft whole = draftOfStoredElement(access, Opcode::PutContiguousIndex, origin);
                whole.setArrayOperand(access.base);
                whole.setPositionOperand(access.key);
                whole.setValueOperand(int32);
                if (inside)
                    whole.setIsInBounds(*inside);
                if (isNumericStorage(tested->storage))
                    whole.setHasNumericStorage(true);
                if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
                    whole.setStorageOperand(pointer);
                if (whole.valueOperand() && walker.build(WTF::move(whole)) && !walker.isUnreachable())
                    walker.jumpTo(behindKinds);
            }
        }
        if (walker.hasFailed())
            return true;
        walker.continueIn(doubles);
        if (walker.isUnreachable()) {
            walker.continueIn(behindKinds);
            noteStoredElementInTypedElements(walker, access.base, access.key, stored);
            walker.bind(access.value.id(), Reference());
            walker.letRestOfStretchRunAsWrittenBehindFirstStore(access.value.id());
            return true;
        }
        walker.count(StatisticID::NumbersStoredByKind);
    }
    if (mayHoldOnlyInt32s) {
        Draft kind(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(kind);
        kind.setArrayOperand(access.base);
        kind.setArrayStorage(ArrayStorage::Contiguous);
        kind.setIsWritable(true);
        Reference holdsAnyValues = walker.build(WTF::move(kind));
        if (!holdsAnyValues)
            return true;
        BlockID anyValues = walker.addBlock();
        BlockID int32sAlone = walker.addBlock();
        behindKinds = walker.addBlock();
        walker.branch(holdsAnyValues, anyValues, int32sAlone);
        walker.continueIn(int32sAlone);
        if (!walker.isUnreachable()) {
            Reference isInt32 = buildTestOfNumber(walker, stored, TypeTable::int32, origin);
            if (continueOrRestartIteration(walker, isInt32)) {
                Reference int32 = buildProofOfNumber(walker, stored, isInt32, origin);
                if ((appendsBehindKind || extendsBehindKind) && int32)
                    append();
                Draft whole = draftOfStoredElement(access, opcode, origin);
                whole.setArrayOperand(access.base);
                whole.setPositionOperand(access.key);
                whole.setValueOperand(int32);
                if (inside)
                    whole.setIsInBounds(*inside);
                if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
                    whole.setStorageOperand(pointer);
                if (whole.valueOperand() && walker.build(WTF::move(whole)) && !walker.isUnreachable())
                    walker.jumpTo(behindKinds);
            }
        }
        if (walker.hasFailed())
            return true;
        walker.continueIn(anyValues);
        if (walker.isUnreachable()) {
            walker.continueIn(behindKinds);
            noteStoredElementInTypedElements(walker, access.base, access.key, stored);
            walker.bind(access.value.id(), Reference());
            walker.letRestOfStretchRunAsWrittenBehindFirstStore(access.value.id());
            return true;
        }
        walker.count(StatisticID::BoxedStoresByStorage);
    }
    if (appendsBehindKind || extendsBehindKind)
        append();
    Draft put = draftOfStoredElement(access, opcode, origin);
    if (isPush)
        walker.count(StatisticID::PushesCompiledAsAppends);
    put.setArrayOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(stored);
    if (inside)
        put.setIsInBounds(*inside);
    if (goesByKind && !storesNumberByKind && !storesByAnyKind)
        put.setGoesByKindOfStorage(true);
    if (opcode == Opcode::PutContiguousIndex && isNumericStorage(tested->storage))
        put.setHasNumericStorage(true);
    if (valueBeforeStore && walker.canRecordUndoableStore())
        walker.recordUndoableElementStore(access.base, access.key, valueBeforeStore, tested->storage);
    if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
        put.setStorageOperand(pointer);
    if (isMovedOutOfDoubleStorage)
        walker.count(StatisticID::DoubleElementsCopiedUnchecked);
    Reference store = walker.build(WTF::move(put));
    walker.takePendingUndoableStore();
    if (behindKinds) {
        if (!walker.isUnreachable())
            walker.jumpTo(behindKinds, heldWithin ? singleElementSpan(heldWithin) : std::span<const Reference>());
        walker.continueIn(behindKinds);
        store = { };
        if (heldWithin && !walker.isUnreachable())
            walker.recordUndoableAppend(access.base, access.key, Reference::value(walker.code().at(behindKinds).parameters()[0]), lengthOfArray, tested->storage, undoableStoreBefore);
    }
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
    if (access.typeOfKey.isNumberWithoutFraction() && !access.typeOfKey.isInteger()) {
        bool walkStoodInFront = !walker.isUnreachable() && !walker.hasFailed();
        if (std::optional<AccessOfTypedElements> held = tryGuardIntegerKey(access))
            return buildArrayStoreOfTypedElements(*held);
        if (walkStoodInFront && (walker.isUnreachable() || walker.hasFailed()))
            return true;
    }
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
    requestStorageForStoredValue(access);
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

static bool accessesViewOfVariableAgain(const BodyIndex& index, const BodyValue& access)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
    static constexpr OpcodeSet calls = opcodesOfFamily(OperationFamily::Call) | OpcodeSet { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::CallDirect, Opcode::Await, Opcode::Yield };
    static constexpr uint32_t farthestAccess = 64;
    if (access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words())
        return false;
    auto variableOf = [&](ValueID object) -> VariableID {
        ValueID read = index.stripCasts(object);
        return read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable ? index.value(read).variable() : VariableID();
    };
    VariableID variable = variableOf(access.objectOperand());
    if (!variable)
        return false;
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t end = std::min(block.firstValue().index() + block.numberOfValues(), access.id().index() + farthestAccess);
    for (uint32_t number = access.id().index() + 1; number < end; ++number) {
        Opcode opcode = index.opcodeOf(ValueID(number));
        if (calls.contains(opcode) || (opcode == Opcode::PutModuleVariable && index.value(ValueID(number)).variable() == variable))
            return false;
        if (accessesByKey.contains(opcode) && variableOf(index.value(ValueID(number)).objectOperand()) == variable)
            return true;
    }
    return false;
}

static Reference lengthOfViewInTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
{
    Walker& walker = access.walker;
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::LengthOfList, access.base))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::RoomOfViewIsAsInFrontOfLoop, access.base))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    bool isAccessedAgain = walker.opensGuesses() && accessesViewOfVariableAgain(walker.currentBodyIndex(), access.value);
    if ((!walker.isInLoop() && !isAccessedAgain) || !walker.opensGuesses())
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
    bool isOfThisBody = written.index() < index.numberOfValues() && index.value(written).words() == access.value.words();
    bool isNamedAgain = isAccessedAgain || (isOfThisBody && index.useCount(access.value.objectOperand()) > 1);
    if (BlockID head = walker.unrolledLoopHeader(); head && isOfThisBody && !isNamedAgain)
        isNamedAgain = index.isInLoop(index.blockOf(written), index.loopOf(head)) && index.stripCasts(access.value.objectOperand()).index() < index.block(head).firstParameter().index();
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
    bool sharesVector = walker.comparesElementsOfViews() && walker.isInLoop();
    std::optional<uint64_t> noted = sharesVector && !walker.state().answers().isEmpty() ? findKnown(walker, KnownNote::VectorOfView, view) : std::nullopt;
    Reference built = noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
    if (!built) {
        Draft vector(Opcode::GetTypedArrayVector, access.origin());
        walker.chooseConservativeFields(vector);
        vector.setViewOperand(view);
        built = walker.build(WTF::move(vector));
        if (built && sharesVector && built.isValue() && !walker.isUnreachable())
            noteKnown(walker, KnownNote::VectorOfView, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
    }
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

static bool isConstantViewWithin(const Code& code, Reference view, int64_t length)
{
    if (!view || !view.isValue())
        return false;
    ValueID cell = underlyingCellOf(code, view.id());
    if (code.isConstant(cell))
        return false;
    const Operation& made = code.at(cell);
    if (made.opcode != Opcode::CallInline || made.inlineBuiltin() != InlineBuiltin::TypedArrayWithLength || !made.numberOfVariadicOperands())
        return false;
    ValueID written = made.variadicOperands()[0];
    return code.isConstant(written) && code.at(written).opcode == Opcode::Integer && code.at(written).integer() <= length;
}

static bool isKeyNearCounter(const BodyIndex& index, const BodyValue& read)
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::ToNumeric };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    if (read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    ValueID written = index.stripCasts(read.propertyOperand());
    ValueID key = written;
    while (key && !index.isParameter(key)) {
        BodyValue made = index.value(key);
        bool addsConstant = sums.contains(made.opcode()) && made.operand(1) && !index.isParameter(index.stripCasts(made.operand(1))) && index.opcodeOf(index.stripCasts(made.operand(1))) == Opcode::Integer;
        if (!addsConstant && !steps.contains(made.opcode()))
            return false;
        key = index.stripCasts(made.operand(0));
    }
    return key && key != written && index.isLoopHead(index.blockOf(key));
}

static Reference mostOfSmallRangeInTypedElements(Walker& walker, const BodyValue& read, Reference view, Reference position, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(position));
    if (!position.isValue() || !type.isInteger() || type.integers.minimum < 0 || type.integers.maximum > mostOfRangeAskedWholeInTypedElements || isKeyNearCounter(walker.currentBodyIndex(), read))
        return { };
    uint32_t least = walker.facts().linked().leastLengthOfViewsReadAt(walker.currentFunction(), read.id());
    if (isConstantViewWithin(walker.code(), view, type.integers.maximum) || (least && least <= type.integers.maximum))
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
    bool isReckonedWith = isElementUsedWithConstant(walker, access.value) || iterationComputesWithElement(walker, access.value);
    bool turnNeedsNumber = isReckonedWith || turnTakesWhatIsReadForKeyOrStoresItIntoViewInTypedElements(walker, access.value);
    std::optional<PassedTest> fromZeroUp = walker.findNonNegativeProof(access.key);
    if (!fromZeroUp)
        fromZeroUp = walker.findNonNegativeDifferenceTest(access.key);
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
    if (isReckonedWith)
        walker.count(StatisticID::ReadsGuardedForNumbers);
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
    Reference most = mostOfSmallRangeInTypedElements(walker, access.value, access.base, key, origin);
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
    Reference most = mostBelowWrittenBoundInTypedElements(walker, access.key, origin, WrittenMasks::AreTaken);
    if (!most && access.key.isConstant() && access.typeOfKey.integers.minimum >= 0)
        most = access.key;
    bool isLowered = !most;
    if (isLowered)
        most = mostUnderLoweredCounter(access, purity, origin);
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
        if (!length || !length.isValue() || isAboveEveryKey(walker, most, length))
            return;
        Draft test(Opcode::Less, origin);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(most);
        test.setRightOperand(length);
        test.setForm(operatorFormFor(walker, test));
        isBelow = walker.build(WTF::move(test));
    }
    if (!isBelow || !isBelow.isValue() || !hoistUpperBoundCheck(walker, isBelow, purity, isLowered))
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
    auto buildElement = [&](const std::optional<PassedTest>& inside) {
        Draft element = draftOfTypedElements(walker, read, Opcode::GetTypedArrayElement);
        element.setViewOperand(view);
        element.setPositionOperand(position);
        element.setTypedArrayType(typedArrayType);
        if (inside)
            element.setIsInsideView(*inside);
        element.setLevel(KnowledgeLevel::Proven);
        element.setKeyIsIntegral(true);
        nameStorageOfViewInTypedElements(access, element, typedArrayType);
        return walker.build(WTF::move(element));
    };
    walker.noteLoweredCounter(read);
    if (access.typeOfKey.isInteger() && walker.isUnderLoweredCounter(position)) {
        std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(view, position);
        if (!inside) {
            hoistTypedArrayBoundsCheck(access, typedArrayType);
            inside = walker.findWhatHoldsPositionInsideList(view, position);
        }
        if (inside)
            return buildElement(inside);
    }
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
    Reference most = readIsUpdated ? Reference() : mostOfSmallRangeInTypedElements(walker, read, view, position, origin);
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

    return buildElement(walker.boundsProofFromChecks(view, position, length, *fromZeroUp, *below));
}

static bool hasMaskedKey(const AccessOfTypedElements& access)
{
    if (!access.key.isValue() || !access.typeOfKey.isInteger() || access.typeOfKey.integers.minimum < 0)
        return false;
    const Code& code = access.walker.code();
    ValueID key = access.key.id();
    while (conversionsOfForm.contains(code.at(key).opcode))
        key = code.at(key).operand(0);
    return isMaskedKey(code, key, access.typeOfKey);
}

static bool isTruncatedSumTerm(const AccessOfTypedElements& access, ValueID sum)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!sums.contains(index.opcodeOf(sum)))
        return false;
    static constexpr unsigned mostSums = 3;
    TypeArena& types = walker.types();
    TypeID int32OrUndefined = types.withPrimitives(TypeTable::int32, Primitive::Undefined);
    auto isInt32OrUndefined = [&](auto& self, ValueID other, unsigned sumsLeft) -> bool {
        ValueID written = index.stripCasts(other);
        if (sumsLeft && written && !index.isParameter(written) && sums.contains(index.opcodeOf(written)) && index.hasOneUse(written) && index.blockOf(written) == index.blockOf(access.value.id()))
            return self(self, index.value(written).operand(0), sumsLeft - 1) && self(self, index.value(written).operand(1), sumsLeft - 1);
        Reference term = written ? walker.referenceBoundTo(written) : Reference();
        if (term && term.isRecord())
            return types.isSubset(walker.typeOf(term), int32OrUndefined);
        if (!written || index.isParameter(written) || index.blockOf(written) != index.blockOf(access.value.id()))
            return false;
        Opcode opcode = index.opcodeOf(written);
        if (allBitwiseOperators.contains(opcode) || opcode == Opcode::Integer)
            return true;
        ValueID view = opcode == Opcode::ElemGet ? index.stripCasts(index.value(written).objectOperand()) : ValueID();
        return view && view == index.stripCasts(access.value.objectOperand());
    };
    ValueID read = access.value.id();
    for (unsigned number = 0; number < mostSums; ++number) {
        bool isLeft = index.stripCasts(index.value(sum).operand(0)) == read;
        if (!isInt32OrUndefined(isInt32OrUndefined, index.value(sum).operand(isLeft), mostSums))
            return false;
        ValueID user = index.onlyUsingValue(sum);
        while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
            user = index.onlyUsingValue(user);
        if (!user)
            return false;
        if (allBitwiseOperators.contains(index.opcodeOf(user)))
            return true;
        if (!sums.contains(index.opcodeOf(user)) || index.blockOf(user) != index.blockOf(sum))
            return false;
        read = sum;
        sum = user;
    }
    return false;
}

static bool isIntegerReadAsDouble(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    static constexpr OpcodeSet operatorsOnDoubles = OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div } | allRelations;
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = access.value.id();
    if (!walker.has(Mechanism::UnboxedNumbers) || (!isInt(typedArrayType) && !isClamped(typedArrayType)) || !readsElementsInLineInTypedElements(typedArrayType))
        return false;
    if (read.index() >= index.numberOfValues() || index.value(read).words() != access.value.words())
        return false;
    ValueID user = index.onlyUsingValue(read);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    if (!user || !operatorsOnDoubles.contains(index.opcodeOf(user)) || isTruncatedSumTerm(access, user))
        return false;
    return everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing, false);
}

static bool buildViewReadOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetTypedArrayElement, KnowledgeLevel::Proven);
    if (bindNumberReadBehindTestsOfTypedElements(access))
        return true;
    Walker& walker = access.walker;
    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetTypedArrayElement);
    if (access.typeOfKey.isInteger())
        walker.noteLoweredCounter(access.value);
    std::optional<PassedTest> inside = access.typeOfKey.isInteger() ? walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key) : std::nullopt;
    if (!inside && (access.key.isConstant() || hasMaskedKey(access) || walker.isUnderLoweredCounter(access.key))) {
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
    if (!inside && isIntegerReadAsDouble(access, typedArrayType)) {
        read.representation = Representation::OptionalDouble;
        walker.count(StatisticID::ViewIntegersReadAsDoubles);
    }
    nameStorageOfViewInTypedElements(access, read, typedArrayType);
    walker.bind(access.value.id(), walker.build(WTF::move(read)));
    return true;
}

static KnowledgeLevel levelOfViewStoreOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    TypeArena& types = access.walker.types();
    TypeID storedID = access.typeOfStored;
    Type stored = types.at(storedID);
    bool convertsWithoutCode = !stored.isNever() && stored.isNumberLike();
    bool convertsInLine = convertsWithoutCode && readsElementsInLineInTypedElements(typedArrayType) && access.walker.has(Mechanism::UnboxedNumbers);
    bool fits = isBigInt(typedArrayType) ? !stored.isNever() && stored.hasOnlyPrimitives(Primitive::BigInt) : stored.isNumber() || convertsInLine;
    if (fits)
        return KnowledgeLevel::Proven;
    bool mayPassTests = readsElementsInLineInTypedElements(typedArrayType) && access.typeOfKey.isInteger() && types.overlap(storedID, TypeTable::number);
    return mayPassTests ? KnowledgeLevel::Expected : KnowledgeLevel::Unknown;
}

static bool onlyStoreUsesValue(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID stored = access.value.valueOperand();
    if (!stored || !index.hasOneUse(stored))
        return false;
    if (!index.isParameter(stored))
        return true;
    std::optional<Walker::CallOfCopy> copied = walker.callOfCopy();
    std::optional<uint32_t> parameter = copied && index.blockOf(stored) == entryBlock ? index.block(entryBlock).indexOfParameter(stored) : std::nullopt;
    const Function& callee = walker.program().at(walker.currentFunction());
    if (!parameter || callee.entryParameterKind(*parameter) != EntryParameter::Parameter)
        return false;
    std::span<const ValueID> handed = copied->index->value(copied->call).argumentsOperands();
    uint32_t position = *parameter - callee.firstParameterIndex();
    return position < handed.size() && handed[position] && copied->index->hasOneUse(handed[position]);
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
        Type typeOfStored = walker.types().at(access.typeOfStored);
        if (isInt(typedArrayType) && typeOfStored.isNumberOrNullish() && (!isClamped(typedArrayType) || typeOfStored.isInt32OrNullish())) {
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
    if (level == KnowledgeLevel::Proven && isInt(typedArrayType) && !isClamped(typedArrayType) && !isBigInt(typedArrayType) && stored.isValue() && onlyStoreUsesValue(access)) {
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        Reference bits = buildTruncatedSum(walker, stored, origin);
        if (walker.isUnreachable())
            return true;
        if (bits)
            stored = bits;
    }
    Draft put = draftOfTypedElements(walker, access.value, Opcode::PutTypedArrayElement);
    put.setViewOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(stored);
    put.setTypedArrayType(typedArrayType);
    if (level == KnowledgeLevel::Proven && access.typeOfKey.isInteger())
        walker.noteLoweredCounter(access.value);
    std::optional<PassedTest> inside = level == KnowledgeLevel::Proven ? walker.findWhatHoldsPositionInsideList(access.base, access.key) : std::nullopt;
    if (!inside && level == KnowledgeLevel::Proven && access.typeOfKey.isInteger())
        inside = walker.findWhatHoldsPositionBelowRoomOfView(access.base, access.key);
    if (!inside && level == KnowledgeLevel::Proven && (access.key.isConstant() || hasMaskedKey(access) || walker.isUnderLoweredCounter(access.key))) {
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
        if (Reference ofCharacter = buildLengthOfCharacterInSight(walker, base, walker.originOf(value))) {
            walker.bind(value.id(), ofCharacter);
            return true;
        }
        if (walker.hasFailed())
            return true;
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

static Reference keyReadBeforeLoop(Walker& walker, Reference key)
{
    std::optional<uint32_t> loop = walker.innermostOpenLoop();
    if (!loop || !key || !key.isValue() || walker.standsInFrontOfOpenLoop(*loop, key))
        return key;
    for (ValueID name = key.id(); walker.code().at(name).opcode == Opcode::Narrowed && !walker.code().at(name).narrowsHolderOfOperand() && !walker.code().at(name).narrowsOtherResultOfOperand();) {
        ValueID test = walker.code().at(name).testOperand();
        if (!test || walker.code().blockOf(test) != walker.headOfOpenLoop(*loop))
            return key;
        name = walker.code().at(name).valueOperand();
        Reference before = Reference::value(name);
        if (!isArrayIndexInTypedElements(walker.types().at(walker.code().at(name).type)) || walker.representationOf(before) != walker.representationOf(key) || walker.code().isConstant(name))
            return key;
        if (walker.standsInFrontOfOpenLoop(*loop, before))
            return before;
    }
    return key;
}

static bool readsElementOrLength(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    return opcode == Opcode::ElemGet || opcode == Opcode::ElemSet || (opcode == Opcode::PropGet && !value.isFreeName() && value.key() == walker.program().keyOf(BuiltinNames::length));
}

static bool buildNamedArrayAccess(Walker& walker, const BodyValue& value)
{
    ValueID object = value.objectOperand();
    Reference instance = walker.referenceOf(object);
    if (walker.types().isSubset(walker.typeOf(instance), walker.types().builtinClass(BuiltinClasses::Array)))
        return false;
    Reference name = arrayNameOfInstance(walker, instance, walker.originOf(value));
    if (walker.hasFailed())
        return true;
    if (!name || !name.isValue() || walker.isUnreachable())
        return false;
    Reference before = walker.referenceBoundTo(object);
    walker.bind(object, name);
    bool isBuilt = buildTypedElementAccess(walker, value);
    if (!walker.isUnreachable() && !walker.hasFailed())
        walker.bind(object, before);
    return isBuilt;
}

bool buildTypedElementAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (buildElementAccessUnderNumber(walker, value, inputs))
        return true;
    Opcode opcode = value.opcode();
    if (!opcodesOfTypedElements.contains(opcode))
        return false;
    if (!inputs.areOnlyTypes() && readsElementOrLength(walker, value) && isArraySubclassInstance(walker, inputs.typeOf(value.objectOperand())) && buildNamedArrayAccess(walker, value))
        return true;
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
    if (opcode == Opcode::ElemGet) {
        if (Reference key = isOnlyAskedAbout ? Reference() : keyReadBeforeLoop(walker, access.key); key && key != access.key) {
            access.key = key;
            access.typeOfKey = walker.types().at(walker.typeOf(key));
        }
        return buildReadOfTypedElements(access);
    }
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

Reference Walker::elementReadAhead(ValueID read) const
{
    if (m_versionedLoops.isEmpty() || m_versionedLoops.last().frameIndex != m_frames.size() - 1)
        return { };
    for (const auto& [ahead, element] : m_versionedLoops.last().elementsReadAhead) {
        if (ahead == read)
            return element;
    }
    return { };
}

void Walker::readElementsAhead(BlockID inputBlock)
{
    static constexpr OpcodeSet opcodesWithoutOtherEffects = allNumericOperators | allRelations | allEqualities | opcodesThatStateTypes
        | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::ElemGet, Opcode::ElemSet, Opcode::LoopHint, Opcode::ToBoolean, Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement, Opcode::Undefined };
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (versionedLoop.frameIndex != m_frames.size() - 1 || inputBlock == versionedLoop.inputHead)
        return;
    const BodyIndex& index = currentBodyIndex();
    BlockID inputHead = versionedLoop.inputHead;
    LoopID loop = index.loopOf(inputHead);
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(inputBlock);
    if (!edge || edge->from != inputHead || !index.isInLoop(inputBlock, loop))
        return;
    versionedLoop.elementsReadAhead.shrink(0);
    if (isUnreachable() || m_hasFailed || !has(Mechanism::UnboxedNumbers) || index.loop(loop).numberOfLoopsWithin != 1 || !index.holdsAnyOf(OpcodeSet { Opcode::ElemSet }) || !index.holdsAnyOf(allBitwiseOperators) || !canBuildIterationGuard())
        return;
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto isBoundWhereIterationBegins = [&](ValueID value) {
        return value.index() < firstValueOfLoop || (index.isParameter(value) && index.blockOf(value) == inputHead);
    };
    BackEdgeDominators everyIteration(index, loop);
    bool hasStored = false;
    Vector<ValueID, 2> reads;
    for (BlockID member : index.membersOf(loop)) {
        bool isInEveryIteration = everyIteration.contains(member);
        for (BodyValue operation : index.block(member).values()) {
            if (!opcodesWithoutOtherEffects.contains(operation.opcode()))
                return;
            if (operation.opcode() == Opcode::ElemSet) {
                std::optional<HoistedArrayGuard> stored = findHoistedArrayGuard(*this, operation);
                if (!stored || !stored->isWritable || stored->storage != ArrayStorage::Contiguous || stored->admitsInt32Storage)
                    return;
                hasStored = true;
            }
            bool isCandidate = hasStored && isInEveryIteration && operation.opcode() == Opcode::ElemGet && isBoundWhereIterationBegins(operation.objectOperand()) && isBoundWhereIterationBegins(operation.propertyOperand());
            if (isCandidate && findTruncatedUse(*this, operation.id()) == TruncatedUse::TermOfSums)
                reads.append(operation.id());
        }
    }
    for (ValueID read : reads) {
        BodyValue operation = index.value(read);
        std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(*this, operation);
        if (!tested || tested->isWritable || !isNumericStorage(tested->storage))
            continue;
        if (!buildTypedElementAccess(*this, operation) || isUnreachable() || m_hasFailed)
            return;
        Reference element = referenceBoundTo(read);
        if (element && element.isValue() && m_types.at(typeOf(element)).isInteger())
            m_versionedLoops.last().elementsReadAhead.append({ read, element });
    }
}

static bool loopFillsLiteral(const BodyIndex& index, ValueID literal)
{
    std::optional<BodyUser> first = index.firstUser(literal);
    while (first && first->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(first->value)))
        first = index.firstUser(first->value);
    if (!first)
        return false;
    LoopID loop = index.loopOf(first->kind == BodyUseKind::Operand ? index.blockOf(first->value) : first->block);
    if (!loop || index.isInLoop(index.blockOf(literal), loop))
        return false;
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            return operation.opcode() == Opcode::ElemSet && index.stripCasts(operation.objectOperand()) == literal && !index.isParameter(index.stripCasts(operation.valueOperand()));
        });
    });
}

bool buildLiteralLoopFills(Walker& walker, const BodyValue& literal)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    if (literal.id().index() >= index.numberOfValues() || index.value(literal.id()).words() != literal.words())
        return false;
    auto written = literal.as<Opcode::NewArray>();
    std::span<const ValueID> elements = written.elementsOperands();
    if (elements.empty() || !written.holes().empty())
        return false;
    bool someIsUnknown = false;
    for (ValueID element : elements) {
        Reference built = walker.referenceOf(element);
        TypeID proven = built ? walker.typeOf(built) : TypeID();
        if (!proven || isNeverType(proven) || !types.overlap(proven, TypeTable::number))
            return false;
        someIsUnknown |= !types.at(proven).isNumber();
    }
    if (!someIsUnknown || !loopFillsLiteral(index, literal.id()))
        return false;
    Vector<Reference, 4> values;
    for (ValueID element : elements)
        values.append(walker.referenceOf(element));
    Draft empty = walker.draftOf(literal);
    if (walker.hasFailed() || empty.storage() != AllocationStorage::Heap)
        return false;
    empty.setVariadicOperands({ });
    walker.chooseConservativeFields(empty);
    Origin origin = empty.origin();
    Reference list = walker.build(WTF::move(empty));
    if (!list)
        return true;
    for (Reference element : values) {
        Draft append(Opcode::AppendElement, origin);
        walker.chooseConservativeFields(append);
        append.setArrayOperand(list);
        append.setValueOperand(element);
        walker.build(WTF::move(append));
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
    walker.bind(literal.id(), list);
    return true;
}

constinit const OpcodeSet opcodesThatHearRaiseWherePushIsBuiltAsWrittenTakes { Opcode::Call };

bool hearRaiseWherePushIsBuiltAsWrittenOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.isInTrialVisitOfCurrentFrame() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop)
        return false;
    bool isInVersion = walker.isInSpecializedIteration();
    bool fillsLiteral = !isInVersion && walker.pushOfLoopFillsLiteral(index.loop(loop).head) && literalThatCallPushesOntoInEveryTurn(walker, call, index.loop(loop).head);
    if (!fillsLiteral && (!walker.isPushOntoLiteral(call) || (isInVersion && findListTestedForStoresInFrontOfLoop(walker, walker.referenceOf(index.stripCasts(call.as<Opcode::Call>().thisValueOperand()))))))
        return false;
    Draft raise(Opcode::ExtendArrayLength, walker.originOf(call));
    walker.chooseConservativeFields(raise);
    raise.setArrayOperand(walker.referenceOf(index.stripCasts(call.as<Opcode::Call>().thisValueOperand())));
    const Effects effects = declaredBoundOf(Opcode::ExtendArrayLength);
    if (walker.shouldAssumeLocalArrayReceivers()) [[unlikely]] {
        walker.recordTrialEffectsOfWriter(effects, Vector<uint32_t, 2> { });
        return true;
    }
    if (raise.arrayOperand()) {
        walker.recordTrialEffectsOfWriter(effects, objectsMadeHereThatAloneAreWritten(walker, raise, effects));
        return true;
    }
    walker.recordTrialEffects(declaredBoundOf(Opcode::ExtendArrayLength));
    return true;
}

static bool buildPushAtLength(Walker& walker, const BodyValue& call, Reference list)
{
    Reference pushed = walker.referenceOf(call.as<Opcode::Call>().argumentsOperands()[0]);
    if (!pushed || !walker.typeOf(pushed) || !walker.typeOf(list) || !findListTestedForStoresInFrontOfLoop(walker, list) || !callsArrayPush(walker, call, list))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID innermost = index.loopOf(index.blockOf(call.id()));
    for (LoopID loop = innermost; loop; loop = index.loop(loop).parent) {
        BlockID head = index.loop(loop).head;
        std::optional<uint64_t> filling = loop == innermost ? findKnown(walker, KnownNote::PushAppendsToLiteral, list, { }, head.index()) : std::nullopt;
        bool isFillingPush = filling && *filling == (call.id().index() | static_cast<uint64_t>(walker.currentFunction().index()) << 32);
        if (!isFillingPush && findKnown(walker, KnownNote::LengthEqualsInitialCounter, list, { }, head.index()))
            return false;
    }
    Draft read(Opcode::GetArrayLength, walker.originOf(call));
    walker.chooseConservativeFields(read);
    read.setArrayOperand(list);
    Reference length = walker.build(WTF::move(read));
    if (!length || walker.isUnreachable())
        return false;
    AccessOfTypedElements access { walker, call, list, length, pushed, walker.typeOf(list), walker.types().at(walker.typeOf(length)), walker.typeOf(pushed), false, ValueID(), true };
    if (!buildStoreBehindRowOfTypedElements(access))
        return false;
    if (walker.isUnreachable() || walker.currentBodyIndex().isUnused(call.id()))
        return true;
    Draft one(Opcode::Integer, walker.originOf(call));
    one.setInteger(1);
    if (Reference longer = buildIntegerOperator(walker, Opcode::Add, length, walker.constant(WTF::move(one)), walker.originOf(call)))
        walker.bind(call.id(), longer);
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
    auto buildAtLength = [&] {
        Reference untouched = untouchedLiteralOfPush(walker, call, loop);
        return untouched && buildPushAtLength(walker, call, untouched);
    };
    if (!noted)
        return buildAtLength();
    if (!noted || *noted >> 32 != walker.currentFunction().index() || !static_cast<uint32_t>(*noted) || static_cast<uint32_t>(*noted) > head.numberOfParameters())
        return false;
    ValueID counter = head.parameter(static_cast<uint32_t>(*noted) - 1);
    Reference key = walker.referenceOf(counter);
    Reference pushed = walker.referenceOf(call.as<Opcode::Call>().argumentsOperands()[0]);
    if (!key || !pushed || !walker.typeOf(key) || !walker.typeOf(pushed))
        return false;
    AccessOfTypedElements access { walker, call, list, key, pushed, walker.typeOf(list), walker.types().at(walker.typeOf(key)), walker.typeOf(pushed), false, counter };
    if (!buildStoreBehindRowOfTypedElements(access))
        return buildAtLength();
    if (walker.isUnreachable() || index.isUnused(call.id()))
        return true;
    Draft one(Opcode::Integer, walker.originOf(call));
    one.setInteger(1);
    if (Reference length = buildIntegerOperator(walker, Opcode::Add, key, walker.constant(WTF::move(one)), walker.originOf(call)))
        walker.bind(call.id(), length);
    return true;
}

static TypedArrayType onlyViewAmongExpectedObjects(Walker& walker, TypeID expected)
{
    TypeID present = walker.types().withoutNullish(expected);
    Type type = walker.types().at(present);
    if (!type.isObject() || type.hasUnboundedUnknown() || type.anyObject || type.anyFunction)
        return NotTypedArray;
    const BuiltinTable& table = walker.program().builtins();
    TypedArrayType only = NotTypedArray;
    for (const ClassPart& part : classPartsOf(walker.types(), type)) {
        TypedArrayType view = table.typedArrayTypeOf(part.builtinClass);
        if (!isTypedView(view) || (only != NotTypedArray && view != only))
            return NotTypedArray;
        only = view;
    }
    return only;
}

bool buildAccessByIndexOffExpectedViewOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    TypeID expected = findExpectation(walker, value.objectOperand()).type;
    if (!expected)
        return false;
    TypedArrayType view = onlyViewAmongExpectedObjects(walker, expected);
    if (elementsExpectedByTypedElements(view) == ExpectedElements::Anything)
        return false;
    Reference object = walker.referenceOf(value.objectOperand());
    Reference key = walker.referenceOf(value.propertyOperand());
    if (!object || !key)
        return false;
    AccessOfTypedElements access { walker, value, object, key, { }, walker.typeOf(object), walker.types().at(walker.typeOf(key)), { }, false };
    return buildAccessByIndexOfTypedElements(access, view);
}

namespace {

struct ConstantIndexRun {
    uint32_t begin { 0 };
    uint32_t end { 0 };
    int64_t greatestIndex { 0 };
    bool stores { false };
    Vector<int64_t, 8> indexesRead;
    Vector<ValueID, 8> operandsTested;
};

} // anonymous namespace

static constexpr uint32_t minAccessesInRun = 6;

static std::optional<int64_t> constantIndexIn(const BodyIndex& index, const BodyValue& access, ValueID base)
{
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet };
    if (!accesses.contains(access.opcode()) || index.stripCasts(access.objectOperand()) != base)
        return std::nullopt;
    ValueID key = index.stripCasts(access.propertyOperand());
    if (index.isParameter(key) || index.opcodeOf(key) != Opcode::Integer)
        return std::nullopt;
    int64_t place = index.value(key).as<Opcode::Integer>().integer();
    return place >= 0 && place <= std::numeric_limits<uint16_t>::max() ? std::optional(place) : std::nullopt;
}

static std::optional<ConstantIndexRun> findConstantIndexRun(Walker& walker, const BodyValue& firstRead)
{
    static constexpr OpcodeSet bitsThatGiveInt32 { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift };
    static constexpr OpcodeSet operators = allBitwiseOperators | OpcodeSet { Opcode::Add, Opcode::Sub };
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstRead.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    ValueID base = index.stripCasts(firstRead.objectOperand());
    ConstantIndexRun stretch { firstRead.id().index() - firstValue, 0, 0, false, { }, { } };
    Vector<ValueID, 32> numbers;
    Vector<int64_t, 8> indexesStored;
    uint32_t accesses = 0;
    auto isNumber = [&](ValueID operand) {
        ValueID made = index.stripCasts(operand);
        if (numbers.contains(made) || (!index.isParameter(made) && index.opcodeOf(made) == Opcode::Integer))
            return true;
        ValueID before = operand;
        while (!index.isParameter(before) && before.index() > firstRead.id().index() && opcodesThatStateTypes.contains(index.opcodeOf(before)))
            before = index.value(before).operand(0);
        Reference built = before.index() < firstRead.id().index() ? walker.referenceOf(before) : Reference();
        if (!built || !built.isRecord())
            return false;
        TypeID type = walker.typeOf(built);
        bool isBoxed = built.isValue() && walker.representationOf(built) == Representation::JSValue;
        if (!isBoxed)
            return walker.types().at(type).isNumber();
        if (!walker.types().overlap(type, TypeTable::int32))
            return false;
        if (!stretch.operandsTested.contains(before))
            stretch.operandsTested.append(before);
        return true;
    };
    for (uint32_t place = stretch.begin; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        Opcode opcode = operation.opcode();
        if (std::optional<int64_t> key = constantIndexIn(index, operation, base)) {
            if (opcode == Opcode::ElemSet) {
                ValueID stored = index.stripCasts(operation.valueOperand());
                if (!numbers.contains(stored) || (!bitsThatGiveInt32.contains(index.opcodeOf(stored)) && index.opcodeOf(stored) != Opcode::ElemGet))
                    break;
                stretch.stores = true;
                indexesStored.append(*key);
            } else {
                numbers.append(operation.id());
                if (!stretch.indexesRead.contains(*key) && !indexesStored.contains(*key))
                    stretch.indexesRead.append(*key);
            }
            stretch.greatestIndex = std::max(stretch.greatestIndex, *key);
            stretch.end = place + 1;
            ++accesses;
            continue;
        }
        if (opcodesThatStateTypes.contains(opcode) || primitiveConstants.contains(opcode))
            continue;
        if (!operators.contains(opcode))
            break;
        bool takesNumbers = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) { takesNumbers &= isNumber(operand); });
        if (!takesNumbers)
            break;
        numbers.append(operation.id());
    }
    return accesses >= minAccessesInRun ? std::optional(WTF::move(stretch)) : std::nullopt;
}

static bool buildConstantIndexRun(Walker& walker, const BodyValue& firstRead)
{
    if (firstRead.opcode() != Opcode::ElemGet || walker.isInLoop() || walker.isUnreachable() || walker.readStandsUnderHandler() || !walker.opensGuesses() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (firstRead.id().index() >= index.numberOfValues() || index.value(firstRead.id()).words() != firstRead.words())
        return false;
    TypeArena& types = walker.types();
    ValueID base = index.stripCasts(firstRead.objectOperand());
    Reference array = walker.referenceOf(firstRead.objectOperand());
    TypeID arrays = types.builtinClass(BuiltinClassID(BuiltinClasses::Array));
    if (!array || !array.isValue() || walker.representationOf(array) != Representation::JSValue || !types.isUninformative(walker.typeOf(array)) || !constantIndexIn(index, firstRead, base))
        return false;
    std::optional<ConstantIndexRun> stretch = findConstantIndexRun(walker, firstRead);
    if (!stretch)
        return false;
    BlockID inputBlock = index.blockOf(firstRead.id());
    uint32_t first = firstRead.id().index();
    uint32_t size = stretch->end - stretch->begin;
    Vector<uint32_t, 32> mentions;
    mentions.fill(0, size);
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand.index() >= first)
                ++mentions[operand.index() - first];
        });
    }
    Vector<ValueID, 8> valuesMentionedLater;
    for (uint32_t place = 0; place < size; ++place) {
        uint32_t uses = index.useCount(ValueID(first + place));
        if (uses == BodyIndex::maxCountedUses || uses > mentions[place])
            valuesMentionedLater.append(ValueID(first + place));
    }

    Origin origin = walker.originOf(firstRead);
    origin.isTailCall = false;
    BlockID otherBlock = walker.addBlock(BlockFrequency::Unlikely);
    auto passes = [&](Reference test, bool asksElement = false) {
        if (!test || !test.isValue() || walker.isUnreachable() || walker.hasFailed())
            return false;
        BlockID passed = walker.addBlock();
        if (asksElement)
            walker.branchOnTest(test, passed, otherBlock);
        else
            walker.branch(test, passed, otherBlock);
        walker.continueIn(passed);
        return !walker.isUnreachable() && !walker.hasFailed();
    };
    auto integer = [&](int64_t number) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(number);
        return walker.constant(WTF::move(written));
    };
    Vector<std::pair<int64_t, Reference>, 8> elements;
    Vector<Reference, 8> operandsProven;
    Reference list;
    auto buildChecks = [&] {
        Vector<Reference, 8> operandsAsked;
        for (ValueID operand : stretch->operandsTested) {
            Reference number = walker.referenceOf(operand);
            size_t asked = operandsAsked.find(number);
            operandsAsked.append(number);
            if (asked != notFound) {
                operandsProven.append(operandsProven[asked]);
                continue;
            }
            Reference isInt32 = buildTestOfNumber(walker, number, TypeTable::int32, origin);
            if (!passes(isInt32))
                return false;
            Reference proven = buildProofOfNumber(walker, number, isInt32, origin);
            if (proven && proven.isValue() && walker.representationOf(proven) == Representation::JSValue) {
                Draft unboxed(Opcode::Unbox, origin);
                unboxed.setOperand(0, proven);
                unboxed.representation = Representation::Int32;
                proven = walker.build(WTF::move(unboxed));
            }
            if (!proven)
                return false;
            operandsProven.append(proven);
        }
        Reference isArray = buildTestOfNumber(walker, array, arrays, origin);
        if (!passes(isArray))
            return false;
        list = buildProofOfNumber(walker, array, isArray, origin);
        if (!list || !passes(askWhetherListIsAsMade(walker, list, origin)) || !passes(askWhetherStorageOfListIsOfKind(walker, list, ArrayStorage::Int32, origin, stretch->stores)))
            return false;
        noteKindOfStorageThatTurnFound(walker, list, ArrayStorage::Int32, stretch->stores);
        Reference length = buildArrayLength(walker, list, origin);
        Reference isBelow = length && length.isValue() ? buildIntegerOperator(walker, Opcode::Less, integer(stretch->greatestIndex), length, origin) : Reference();
        if (!passes(isBelow))
            return false;
        Reference held = buildNarrowedInNumberForms(walker, length, isBelow, OperandIndex::Less::right, true, origin);
        if (!held || !held.isValue() || held == length)
            return false;
        noteKnown(walker, KnownNote::LengthOfList, list, { }, 0, placesUnderLengthsOfLists, held.bits());
        for (int64_t place : stretch->indexesRead) {
            Reference position = integer(place);
            std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position);
            if (!inside)
                return false;
            Draft isPresent(Opcode::IsElementPresent, origin);
            walker.chooseConservativeFields(isPresent);
            isPresent.setArrayOperand(list);
            isPresent.setPositionOperand(position);
            isPresent.setArrayStorage(ArrayStorage::Int32);
            isPresent.setKeyIsIntegral(true);
            isPresent.setIsInBounds(*inside);
            Reference test = walker.build(WTF::move(isPresent));
            if (!passes(test, true))
                return false;
            std::optional<PassedTest> present = walker.showElementPresentBehindTest(list, position, walker.findPassedTest(test));
            if (!present)
                return false;
            Draft read(Opcode::GetArrayElement, origin);
            walker.chooseConservativeFields(read);
            read.setArrayOperand(list);
            read.setPositionOperand(position);
            read.setArrayStorage(ArrayStorage::Int32);
            read.setLevel(KnowledgeLevel::Proven);
            read.setKeyIsIntegral(true);
            read.setIsPresent(*present);
            Reference element = walker.build(WTF::move(read));
            if (!element || !types.isSubset(walker.typeOf(element), TypeTable::int32))
                return false;
            elements.append({ place, element });
        }
        return true;
    };
    bool isChecked = buildChecks();
    if (walker.hasFailed())
        return true;
    if (!isChecked && !walker.isUnreachable())
        walker.jumpTo(otherBlock);
    BlockID blockBehindChecks = isChecked && !walker.isUnreachable() ? walker.currentBlock() : BlockID();
    BlockID meetingBlock = walker.addBlock();
    walker.recordSpeculation();

    Vector<Walker::JoinArm, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable())
            return;
        Walker::JoinArm arm { walker.currentBlock(), { } };
        for (ValueID mentioned : valuesMentionedLater) {
            Reference built = walker.referenceOf(mentioned);
            if (!built || !built.isRecord())
                return walker.fail("an arm of a stretch has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };
    walker.continueIn(otherBlock);
    {
        Walker::ClosedArm otherArm(walker);
        walker.walkStretch(inputBlock, stretch->begin, stretch->end);
        endArm();
    }
    if (walker.hasFailed())
        return true;
    if (blockBehindChecks) {
        walker.continueIn(blockBehindChecks);
        Vector<Reference, 8> operandsBefore;
        for (size_t place = 0; place < stretch->operandsTested.size(); ++place) {
            operandsBefore.append(walker.referenceBoundTo(stretch->operandsTested[place]));
            walker.bind(stretch->operandsTested[place], operandsProven[place]);
        }
        auto elementAt = [&](int64_t place) -> Reference& {
            for (auto& [index, element] : elements) {
                if (index == place)
                    return element;
            }
            elements.append({ place, Reference() });
            return elements.last().second;
        };
        for (uint32_t place = stretch->begin; place < stretch->end && !walker.isUnreachable() && !walker.hasFailed(); ++place) {
            BodyValue operation = index.value(ValueID(first + place - stretch->begin));
            std::optional<int64_t> key = constantIndexIn(index, operation, base);
            if (!key) {
                walker.walkStretch(inputBlock, place, place + 1);
                continue;
            }
            Reference& element = elementAt(*key);
            if (operation.opcode() == Opcode::ElemGet) {
                walker.bind(operation.id(), element);
                continue;
            }
            Reference stored = walker.referenceOf(operation.valueOperand());
            Reference position = integer(*key);
            std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position);
            if (!stored || !inside || !types.isSubset(walker.typeOf(stored), TypeTable::int32))
                return walker.fail("a stretch stores no int32"_s), true;
            element = stored;
            Draft put(Opcode::PutContiguousIndex, walker.originOf(operation));
            walker.chooseConservativeFields(put);
            put.setArrayOperand(list);
            put.setPositionOperand(position);
            put.setValueOperand(stored);
            put.setIsInBounds(*inside);
            put.setHasNumericStorage(true);
            walker.build(WTF::move(put));
            walker.bind(operation.id(), Reference());
        }
        endArm();
        for (size_t place = 0; place < stretch->operandsTested.size(); ++place)
            walker.bind(stretch->operandsTested[place], operandsBefore[place]);
    }
    if (walker.hasFailed())
        return true;
    walker.count(StatisticID::ArraysCheckedAtStores);
    for (uint32_t place = 0; place < size; ++place) {
        if (arms.isEmpty() || !valuesMentionedLater.contains(ValueID(first + place)))
            walker.bind(ValueID(first + place), Reference());
    }
    Vector<Reference, 8> met(valuesMentionedLater.size());
    walker.joinArms(arms.span(), meetingBlock, met.mutableSpan());
    for (size_t position = 0; position < met.size() && !arms.isEmpty() && !walker.hasFailed(); ++position)
        walker.bind(valuesMentionedLater[position], met[position]);
    return true;
}

bool buildReadByIndexOffAnyBaseOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    if (buildConstantIndexRun(walker, value))
        return true;
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
