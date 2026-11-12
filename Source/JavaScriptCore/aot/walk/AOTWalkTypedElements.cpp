/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinPristine.h"
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
    bool storesNumberFromBefore { false };
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

static std::optional<int64_t> findConstantPosition(Walker& walker, Reference key)
{
    if (!key || !key.isConstant() || walker.code().at(key.id()).opcode != Opcode::Integer)
        return std::nullopt;
    return walker.code().at(key.id()).integer();
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
    auto readsNumberSlot = [&](ValueID ahead) {
        auto holdsNumber = [](Representation held) {
            return held == Representation::Double || held == Representation::OptionalDouble || held == Representation::Int32;
        };
        if (ahead && !index.isParameter(ahead) && index.opcodeOf(ahead) == Opcode::CheckInitialized)
            ahead = index.stripCasts(index.value(ahead).operand(0));
        if (!ahead || index.isParameter(ahead))
            return false;
        BodyValue operation = index.value(ahead);
        if (operation.opcode() == Opcode::GetEnvironmentField)
            return holdsNumber(representationOf(walker.plan().slot(operation.layout(), operation.field()).representation));
        if (operation.opcode() != Opcode::PropGet || operation.isFreeName() || operation.objectOperand().index() >= read.index())
            return false;
        Reference object = walker.referenceOf(operation.objectOperand());
        TypeID objects = object && object.isRecord() ? types.withOnlyObjects(walker.typeOf(object)) : TypeID();
        if (!objects || !types.at(objects).hasObjects() || !types.at(objects).hasOnlyLayouts())
            return false;
        return std::ranges::all_of(membersOf(types, types.at(objects).layouts), [&](LayoutID member) {
            std::optional<FieldReference> field = findField(walker.program(), member, operation.key());
            return field && holdsNumber(representationOf(walker.plan().slot(member, field->field).representation));
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
                    bool isNumberSlot = readsNumberSlot(readToCome);
                    if (isNumberSlot)
                        walker.count(StatisticID::ElementsReadRawBesideSlots);
                    standsBesideNumbers &= !index.isParameter(readToCome) && (isNumberSlot || operationsThatMakeNumbers.contains(index.opcodeOf(readToCome)) || (index.opcodeOf(readToCome) == Opcode::ElemGet && isNumberOrUndefined(walker.facts().linked().resultOfReadByKey(walker.currentFunction(), readToCome))));
                    return;
                }
                Reference other = walker.referenceOf(operand);
                standsBesideNumbers &= other && other.isRecord() && isNumberOrUndefined(walker.typeOf(other));
            });
        }
        return standsBesideNumbers;
    };
    if ((type.primitives & Primitive::FractionalNumber) && isDefaultedElement(walker, index, read)) {
        walker.exchangeResultOfReadBeingTyped(types.withPrimitives(proven, Primitive::Number));
        return unboxed;
    }
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

std::span<const Walker::ElementAccess> Walker::elementAccessesOf(ValueID array)
{
    const BodyIndex& index = currentBodyIndex();
    const Vector<ElementAccess>& found = m_elementAccesses.ensure(&index, [&] {
        Vector<ElementAccess> made;
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            BlockID block(number);
            if (!index.isReachable(block))
                continue;
            for (BodyValue operation : index.block(block).values()) {
                if (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet)
                    made.append({ index.stripCasts(operation.objectOperand()).index(), operation.id().index() });
            }
        }
        std::ranges::stable_sort(made, { }, &ElementAccess::array);
        return made;
    }).iterator->value;
    auto ofArray = std::ranges::equal_range(found, array.index(), { }, &ElementAccess::array);
    return { ofArray.begin(), ofArray.end() };
}

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
    auto isFieldOfNumbers = [&](const BodyValue& field) {
        ValueID holder = field.isFreeName() ? ValueID() : index.stripCasts(field.objectOperand());
        Reference object = holder && holder.index() < read.id().index() ? walker.referenceOf(holder) : Reference();
        TypeID typeOfObject = object && object.isRecord() ? walker.typeOf(object) : TypeID();
        return typeOfObject && areReceiversFilledAtAllReads(walker, typeOfObject, field.key()) && isNumberOrNothing(typeFoundInFilledSlot(walker, field, typeOfObject));
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
        if (opcode == Opcode::PropGet && isFieldOfNumbers(index.value(written))) {
            walker.count(StatisticID::FieldFactorsTakenForNumbers);
            continue;
        }
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
    if (uint32_t end = walker.endOfStretchBeingWalked(); (walker.standsInArmOfRowOfShapeTests() || walker.standsInClosedArm() || walker.isInArrayArms()) && end < blockOfRead.numberOfValues() && (!blockOfRead.holds(user) || user.index() - blockOfRead.firstValue().index() >= end))
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
        if (!walker.guardedReadArray() && !walker.isInArrayArms())
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
    static constexpr size_t mostCallsHandingOn = 3;
    if (!walker.callOfCopy() || walker.standsInArmOfRowOfShapeTests() || walker.guardedReadArray())
        return std::nullopt;
    auto behindStatedTypes = [](const BodyIndex& index, ValueID& name) {
        ValueID user = index.onlyUsingValue(name);
        for (; user && opcodesThatStateTypes.contains(index.opcodeOf(user)); user = index.onlyUsingValue(user))
            name = user;
        return user;
    };
    std::optional<Walker::CallOfCopy> copied;
    const BodyIndex* callee = &walker.currentBodyIndex();
    ValueID name = read.id();
    ValueID user = behindStatedTypes(*callee, name);
    size_t outward = 0;
    for (; !user && outward < mostCallsHandingOn; ++outward) {
        BodyTerminator end = callee->block(callee->blockOf(name)).terminator();
        if (!callee->hasOneUse(name) || (end.kind() != TerminatorKind::Return && (!outward || end.kind() != TerminatorKind::ReturnCall)) || end.operand(0) != name)
            return std::nullopt;
        copied = walker.callOfCopy(outward);
        if (!copied)
            return std::nullopt;
        callee = copied->index;
        name = copied->call;
        user = behindStatedTypes(*callee, name);
    }
    if (!user || !copied)
        return std::nullopt;
    const BodyIndex& index = *copied->index;
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    BodyValue operation = index.value(user);
    if (operation.opcode() == Opcode::BitNot)
        return copied;
    if (!arithmetic.contains(operation.opcode()) && !allBitwiseOperators.contains(operation.opcode()))
        return std::nullopt;
    if (operation.opcode() != Opcode::Add)
        return copied;
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    Reference built = other != name && other.index() < copied->call.index() ? walker.referenceInCaller(other, outward - 1) : Reference();
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
    else if (!walker.canRestartIteration() && walker.canGuardRecursion())
        walker.jumpTo(walker.elementGuardFallback());
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
    if (access.isOnlyAskedAbout)
        return access.walker.looksTakeFormsBehindLooksAsBuilt();
    return access.walker.stretchMayRunAsWrittenInstead() || access.walker.canBuildIterationGuard() || access.walker.canGuardAfterStores() || access.walker.canGuardRecursion();
}

static ValueID findRelationFalseForHole(Walker& walker, const BodyValue& read, Reference list)
{
    static constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.opcode() != Opcode::ElemGet || read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return { };
    ValueID user = index.onlyUsingValue(read.id());
    BodyBlock block = index.block(index.blockOf(read.id()));
    if (!user || !allRelations.contains(index.opcodeOf(user)) || !block.holds(user) || user.index() + 1 != block.endOfValues().index() || !index.hasOneUse(user))
        return { };
    BodyTerminator end = block.terminator();
    if (end.kind() != TerminatorKind::Branch || end.operand(0) != user)
        return { };
    auto isInteger = [&](ValueID operand) {
        if (operand.index() > read.id().index() && block.holds(operand))
            return index.opcodeOf(operand) == Opcode::Integer || steps.contains(index.opcodeOf(operand));
        Reference built = walker.referenceOf(operand);
        return built && walker.types().at(walker.typeOf(built)).isInteger();
    };
    auto readsSameList = [&](BodyValue operation) {
        return operation.opcode() == Opcode::ElemGet && operation.objectOperand().index() < read.id().index() && walker.referenceOf(operation.objectOperand()) == list && isInteger(operation.propertyOperand());
    };
    for (uint32_t between = read.id().index() + 1; between < user.index(); ++between) {
        BodyValue operation = index.value(ValueID(between));
        bool isStep = steps.contains(operation.opcode());
        for (uint32_t operand = 0; operand < (isStep ? numberOfNamedOperandsOf(operation.opcode()) : 0); ++operand)
            isStep &= isInteger(operation.operand(operand));
        if (!index.hasOneUse(ValueID(between)) || (operation.opcode() != Opcode::Integer && !isStep && !readsSameList(operation)))
            return { };
    }
    BodyValue relation = index.value(user);
    ValueID other = relation.operand(0) == read.id() ? relation.operand(1) : relation.operand(0);
    if (other == read.id())
        return { };
    if (other.index() < read.id().index()) {
        Reference built = walker.referenceOf(other);
        if (!built || !walker.types().at(walker.typeOf(built)).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
            return { };
    }
    for (ValueID argument : end.targets()[1].arguments()) {
        if (argument && argument.index() >= read.id().index() && block.holds(argument))
            return { };
    }
    return walker.canFollowNotTakenEdge(end) ? user : ValueID();
}

static void failRelationOnHole(Walker& walker, const BodyValue& read, ValueID relation)
{
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    walker.bind(read.id(), walker.constant(Draft(Opcode::Undefined, origin)));
    Draft fails(Opcode::Boolean, origin);
    fails.setBoolean(false);
    fails.representation = Representation::JSValue;
    walker.bind(relation, walker.constant(WTF::move(fails)));
    walker.followNotTakenEdge(walker.currentBodyIndex().terminator(walker.currentBodyIndex().blockOf(relation)));
    walker.bind(relation, Reference());
    walker.count(StatisticID::RelationsFailedByHoles);
}

bool guardElementPresenceInIteration(Walker& walker, Draft& read, Reference list, Reference position, const HoistedArrayGuard& tested, ValueID relationFailedByHole)
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
    if (!walker.isUnreachable() && relationFailedByHole)
        failRelationOnHole(walker, walker.currentBodyIndex().value(read.inputValueGiven), relationFailedByHole);
    else if (!walker.isUnreachable())
        jumpToGuardFallback(walker);
    walker.continueIn(found);
    walker.recordSpeculation();
    std::optional<PassedTest> present = walker.isUnreachable() || walker.hasFailed() ? std::nullopt : walker.showElementPresentBehindTest(list, position, walker.findPassedTest(test));
    if (!present)
        return false;
    noteKnown(walker, KnownNote::PositionFoundInsideList, list, position, 0, slotsUnderInBoundsIndices, test.bits());
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

static bool isRemainderByConstant(const Code& code, ValueID key, const Type& type)
{
    const Operation& operation = code.at(key);
    return operation.opcode == Opcode::Mod && code.isConstant(operation.operand(1)) && type.integers.maximum <= std::numeric_limits<uint16_t>::max();
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
    if (made.opcode != Opcode::Narrowed)
        return false;
    ValueID test = made.testOperand();
    if (made.isOnTakenEdge() && test && !code.isConstant(test) && made.operandIndex() <= OperandIndex::Less::right) {
        const Operation& relation = code.at(test);
        const OpcodeSet& bounds = made.operandIndex() == OperandIndex::Less::left ? upperBounds : lowerBounds;
        if (bounds.contains(relation.opcode) && code.isConstant(relation.operand(1 - made.operandIndex())))
            return true;
    }
    return depth < deepestOperator && isBelowConstantBound(code, made.valueOperand(), depth + 1);
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
    if ((masks == WrittenMasks::AreLeft || (!isMaskedKey(code, held, type) && !isRemainderByConstant(code, held, type))) && !isBelowConstantBound(code, held))
        return { };
    Draft most(Opcode::Integer, origin);
    most.setInteger(type.integers.maximum);
    return walker.constant(WTF::move(most));
}

int64_t tightenBoundOfSteppedCounter(Walker&, const BodyIndex&, Opcode relation, ValueID counter, int64_t bound);

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
    int64_t last = tightenBoundOfSteppedCounter(walker, index, index.opcodeOf(relation), value, index.value(bound).as<Opcode::Integer>().integer()) - (index.opcodeOf(relation) == Opcode::Less);
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
    if (!loop && walker.depthOfCopies() && walker.isInLoop())
        return true;
    uint32_t numberOfOperations = 0;
    for (BlockID member : loop ? index.membersOf(loop) : std::span<const BlockID>())
        numberOfOperations += index.block(member).numberOfValues();
    return numberOfOperations > maxUnrolledLoopSize;
}

static void hoistArrayBoundsCheck(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    Walker::IterationPurity purity = walker.isIterationEffectFree() ? Walker::IterationPurity::EffectFree : Walker::IterationPurity::KnownEffectsOnlyWithInlinees;
    if (!access.typeOfKey.isInteger() || !walker.isInvariantInSpecializedVersion(access.base, purity))
        return;
    bool isPreserved = walker.iterationsPreserveArrayGuards(access.base, purity) && walker.iterationsPreserveArrayLength(access.base, purity);
    if (!isPreserved && (access.value.opcode() != Opcode::ElemGet || !compareWithStoredArrays(walker, access.value, access.base)))
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
    bool isKeyItself = most && !isLowered && access.key.isValue() && walker.isInvariantInSpecializedVersion(access.key, purity);
    if (isKeyItself)
        most = access.key;
    if (!most)
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    bool isInBody = access.value.id().index() < index.numberOfValues() && index.value(access.value.id()).words() == access.value.words();
    if (std::optional<Range> overTurns = isInBody && !isKeyItself && walker.unrolledLoopHeader() ? indexRangeOverIterations(walker, index, access.value.propertyOperand()) : std::nullopt; overTurns && overTurns->maximum > access.typeOfKey.integers.maximum) {
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
    if (isKeyItself) {
        walker.state().answers().noteOutcome(isBelow, true);
        noteKnown(walker, KnownNote::Relation, access.key, length, 0, LocationMasks(), static_cast<uint64_t>(isBelow.bits()) << 32 | 1);
        noteKnown(walker, KnownNote::LengthOfList, access.base, { }, 0, placesUnderLengthsOfLists, length.bits());
        walker.recordSpeculation();
        walker.count(StatisticID::ArrayBoundsChecksHoisted);
        walker.count(StatisticID::InvariantKeyBoundsChecks);
        return;
    }
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

static Reference buildIntegerReadByStorage(const AccessOfTypedElements& access, const HoistedArrayGuard& tested, std::optional<PassedTest> inside, bool isIndex = false)
{
    Walker& walker = access.walker;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Representation form = isIndex ? Representation::Int32 : Representation::Int64;
    TypeID integers = isIndex ? TypeID(TypeTable::int32) : walker.types().integer(integersAddedWithoutRounding);
    auto buildInteger = [&](Opcode conversion, Reference number) {
        if (!number || !number.isValue() || walker.representationOf(number) == form)
            return number;
        Draft integer(conversion, origin);
        integer.setOperand(0, number);
        integer.representation = form;
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
        Reference isInteger = buildTestOfNumber(walker, element, integers, origin);
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

static bool mayTruncateRead(Walker&, const BodyValue& access);

static bool isPassedForBits(Walker& walker, const BodyIndex& index, ValueID passed)
{
    ValueID user = index.onlyUsingValue(passed);
    if (!user || index.opcodeOf(user) != Opcode::Call)
        return false;
    BodyValue call = index.value(user);
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), user);
    const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
    auto arguments = call.as<Opcode::Call>().argumentsOperands();
    size_t position = std::ranges::find(arguments, passed) - arguments.begin();
    if (!body || position >= arguments.size() || call.as<Opcode::Call>().thisValueOperand() == passed)
        return false;
    const Function& function = walker.program().at(callee);
    size_t parameter = function.firstParameterIndex() + position;
    BodyBlock entry = body->block(entryBlock);
    if (parameter >= entry.numberOfParameters() || function.entryParameterKind(parameter) != EntryParameter::Parameter || function.isCoroutine() || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount }))
        return false;
    ValueID taken = entry.parameter(parameter);
    uint32_t usesLeft = body->useCount(taken);
    if (!usesLeft || usesLeft >= BodyIndex::maxCountedUses)
        return false;
    for (uint32_t block = 0; block < body->numberOfBlocks() && usesLeft; ++block) {
        for (BodyValue operation : body->block(BlockID(block)).values()) {
            bool takesBits = allBitwiseOperators.contains(operation.opcode());
            bool isTaken = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                if (operand != taken)
                    return;
                isTaken = true;
                usesLeft -= takesBits;
            });
            if (isTaken && !takesBits)
                return false;
        }
    }
    return !usesLeft;
}

static bool holdsBitsAlone(Walker& walker, const BodyIndex& index, ValueID array)
{
    if (!array || index.isParameter(array))
        return false;
    Reference name = walker.referenceOf(array);
    if (!name || !name.isValue())
        return false;
    Opcode allocation = walker.code().at(underlyingCellOf(walker.code(), name.id())).opcode;
    bool isBuiltinCall = index.opcodeOf(array) == Opcode::Call && allocation == Opcode::NewArrayLike && !walker.facts().linked().calleeAt(walker.currentFunction(), array);
    if (!isBuiltinCall && (index.opcodeOf(array) != Opcode::NewArray || allocation != Opcode::NewArray))
        return false;
    uint32_t usesLeft = index.useCount(array);
    if (!usesLeft || usesLeft >= BodyIndex::maxCountedUses)
        return false;
    for (uint32_t block = 0; block < index.numberOfBlocks() && usesLeft; ++block) {
        for (BodyValue operation : index.block(BlockID(block)).values()) {
            uint32_t usesHere = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                usesHere += operand == array;
            });
            if (!usesHere)
                continue;
            if (usesHere != 1)
                return false;
            bool isRead = operation.opcode() == Opcode::ElemGet && operation.objectOperand() == array;
            bool isStore = operation.opcode() == Opcode::ElemSet && operation.objectOperand() == array;
            if (isRead && !index.isUnused(operation.id()) && findTruncatedUse(walker, operation.id()) != TruncatedUse::OperandOfBits && !isPassedForBits(walker, index, operation.id()))
                return false;
            if (!isRead && !isStore)
                return false;
            --usesLeft;
        }
    }
    return !usesLeft;
}

static ValueID findTruncatingStore(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = read.id();
    ValueID user = index.onlyUsingValue(written);
    for (; user && opcodesThatStateTypes.contains(index.opcodeOf(user)); user = index.onlyUsingValue(written))
        written = user;
    if (!user || index.opcodeOf(user) != Opcode::ElemSet)
        return { };
    BodyValue store = index.value(user);
    ValueID array = index.stripCasts(store.objectOperand());
    bool isStored = store.valueOperand() == written && array != index.stripCasts(read.objectOperand()) && index.loopOf(index.blockOf(user)) == index.loopOf(index.blockOf(read.id()));
    return isStored && holdsBitsAlone(walker, index, array) ? user : ValueID();
}

static TruncatedUse findTruncatedUseInLoop(Walker& walker, const BodyValue& read)
{
    TruncatedUse use = findTruncatedUse(walker, read.id());
    if (use != TruncatedUse::None || !mayTruncateRead(walker, read))
        return use;
    if (isTruncatedInLoop(walker.currentBodyIndex(), read.id()))
        return TruncatedUse::OperandOfBits;
    ValueID store = findTruncatingStore(walker, read);
    Reference array = store ? walker.referenceOf(walker.currentBodyIndex().value(store).objectOperand()) : Reference();
    std::optional<HoistedArrayGuard> stored = array && array.isValue() ? findListTestedForStoresInFrontOfLoop(walker, array) : std::nullopt;
    if (!stored || !stored->isWritable || stored->storage != ArrayStorage::Int32 || stored->admitsInt32Storage)
        return TruncatedUse::None;
    walker.count(StatisticID::HolesStoredAsZero);
    return TruncatedUse::OperandOfBits;
}

bool readsTruncatedElement(Walker& walker, const BodyValue& read)
{
    return walker.has(Mechanism::UnboxedNumbers) && mayTruncateRead(walker, read) && findTruncatedUseInLoop(walker, read) == TruncatedUse::OperandOfBits;
}

bool hasWideTruncatedReads(Walker& walker, BlockID inputHead, const LoopPreview& preview)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }) || !index.holdsAnyOf(allBitwiseOperators) || index.loop(loop).numberOfLoopsWithin != 1)
        return false;
    auto typeInPreview = [&](ValueID value) -> TypeID {
        if (value.index() >= preview.firstValue && value.index() - preview.firstValue < preview.typesOfValues.size())
            return preview.typesOfValues[value.index() - preview.firstValue];
        Reference built = walker.referenceOf(value);
        return built ? walker.typeOf(built) : TypeID();
    };
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue read) {
            TypeID key = read.opcode() == Opcode::ElemGet ? typeInPreview(read.propertyOperand()) : TypeID();
            return key && walker.types().at(key).isInteger() && !isArrayIndexInTypedElements(walker.types().at(key)) && readsTruncatedElement(walker, read);
        });
    });
}

static bool mayTruncateRead(Walker& walker, const BodyValue& access)
{
    static constexpr uint32_t maxShortLoopSize = 20;
    static constexpr OpcodeSet uncounted = primitiveConstants | opcodesThatStateTypes;
    static constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement, Opcode::ToNumeric };
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    if (!loop)
        return false;
    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!uncounted.contains(operation.opcode()) && ++numberOfOperations > maxShortLoopSize)
                return true;
        }
    }
    ValueID key = index.stripCasts(access.propertyOperand());
    while (key && !index.isParameter(key) && steps.contains(index.opcodeOf(key)))
        key = index.stripCasts(index.value(key).operand(0));
    return (key && !index.isParameter(key)) || findUpdatingStore(index, access.id());
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

static int64_t distanceToGreatestKey(Walker& walker, const BodyValue& read)
{
    constexpr int64_t farthest = std::numeric_limits<uint16_t>::max();
    const BodyIndex& index = walker.currentBodyIndex();
    auto keyOf = [&](const BodyValue& access) -> std::pair<ValueID, int64_t> {
        ValueID key = index.stripCasts(access.propertyOperand());
        if (key && !index.isParameter(key) && index.opcodeOf(key) == Opcode::Add) {
            ValueID addend = index.stripCasts(index.value(key).operand(1));
            if (addend && !index.isParameter(addend) && index.opcodeOf(addend) == Opcode::Integer && index.value(addend).as<Opcode::Integer>().integer() >= 0 && index.value(addend).as<Opcode::Integer>().integer() <= farthest)
                return { index.stripCasts(index.value(key).operand(0)), index.value(addend).as<Opcode::Integer>().integer() };
        }
        return { key, 0 };
    };
    auto [counter, own] = keyOf(read);
    ValueID array = index.stripCasts(read.objectOperand());
    int64_t greatest = own;
    for (BodyValue other : index.block(index.blockOf(read.id())).values()) {
        if (other.opcode() != Opcode::ElemGet || other.id() <= read.id() || index.stripCasts(other.objectOperand()) != array)
            continue;
        if (auto [otherCounter, distance] = keyOf(other); counter && otherCounter == counter)
            greatest = std::max(greatest, distance);
    }
    return greatest - own;
}

static void noteLengthAbovePosition(Walker& walker, Reference length, Reference isBelow, Origin origin)
{
    if (!length.isValue() || !isBelow.isValue() || !walker.currentBodyIndex().opcodes().contains(Opcode::Mod))
        return;
    const Operation& test = walker.code().at(isBelow.id());
    Range positions = test.opcode == Opcode::Less ? walker.types().at(walker.code().at(test.leftOperand()).type).integers : Range::empty();
    if (positions.isEmpty() || positions.minimum < 0 || positions.minimum >= walker.types().at(walker.typeOf(length)).integers.maximum)
        return;
    Reference above = buildNarrowedInNumberForms(walker, length, isBelow, OperandIndex::Less::right, true, origin);
    if (above && above.isRecord() && above != length)
        noteKnown(walker, KnownNote::PositionBelowLength, length, { }, 0, placesUnderLengthsOfLists, above.bits());
}

static std::optional<PassedTest> guardKeyBelowLength(AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!canGuardAccess(access) || !access.base.isValue() || !access.key.isValue())
        return std::nullopt;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    std::optional<PassedTest> fromZeroUp = walker.findNonNegativeProof(access.key);
    static_assert(MAX_STORAGE_VECTOR_LENGTH <= static_cast<unsigned>(std::numeric_limits<int32_t>::max()));
    if (!fromZeroUp && walker.representationOf(access.key) == Representation::Int32 && access.typeOfKey.integers.isSubsetOf(Range::int32())) {
        Draft zero(Opcode::Integer, origin);
        zero.setInteger(0);
        Reference unsignedKey = buildIntegerOperator(walker, Opcode::URShift, access.key, walker.constant(WTF::move(zero)), origin);
        if (!unsignedKey || !unsignedKey.isValue())
            return std::nullopt;
        access.key = unsignedKey;
        access.typeOfKey = walker.types().at(walker.typeOf(unsignedKey));
        fromZeroUp = walker.findNonNegativeProof(access.key);
    }
    if (!fromZeroUp) {
        Draft zero(Opcode::Integer, origin);
        zero.setInteger(0);
        Reference isFromZeroUp = buildIntegerOperator(walker, Opcode::GreaterEq, access.key, walker.constant(WTF::move(zero)), origin);
        if (!isFromZeroUp || !isFromZeroUp.isValue() || walker.code().isConstant(isFromZeroUp.id()) || !continueOrRestartIteration(walker, isFromZeroUp, true))
            return std::nullopt;
        fromZeroUp = walker.findPassedTest(isFromZeroUp);
    }
    if (!fromZeroUp)
        return std::nullopt;
    Reference length = buildArrayLength(walker, access.base, origin);
    if (std::optional<uint64_t> noted = length ? findKnown(walker, KnownNote::PositionBelowLength, access.base, length) : std::nullopt) {
        std::optional<PassedTest> below = walker.findPassedTest(Reference::fromBits(static_cast<uint32_t>(*noted)));
        if (std::optional<PassedTest> inside = below ? walker.boundsProofFromChecks(access.base, access.key, length, *fromZeroUp, *below) : std::nullopt)
            return inside;
    }
    Reference greatest = access.key;
    if (int64_t distance = length && isReadOfBody(access) ? distanceToGreatestKey(walker, access.value) : 0) {
        Draft above(Opcode::Integer, origin);
        above.setInteger(distance);
        greatest = buildIntegerOperator(walker, Opcode::Add, access.key, walker.constant(WTF::move(above)), origin);
    }
    Reference test = length && greatest ? buildIntegerOperator(walker, Opcode::Less, greatest, length, origin) : Reference();
    if (!test || !test.isValue() || walker.code().isConstant(test.id()) || !continueOrRestartIteration(walker, test, true))
        return std::nullopt;
    std::optional<PassedTest> below = walker.findPassedTest(test);
    if (below && length.isRecord())
        noteKnown(walker, KnownNote::PositionBelowLength, access.base, length, 0, placesUnderLengthsOfLists, test.bits());
    if (below && length.isRecord())
        noteLengthAbovePosition(walker, length, test, origin);
    return below ? walker.boundsProofFromChecks(access.base, access.key, length, *fromZeroUp, *below) : std::nullopt;
}

static bool buildCarriedRead(const AccessOfTypedElements& access, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    if (!walker.mayCarryElements() || !isNumericStorage(tested.storage) || !access.typeOfKey.isInteger() || !access.base.isValue() || !walker.has(Mechanism::UnboxedNumbers) || !isReadOfBody(access))
        return false;
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    bool takesBitsAlone = findTruncatedUseInLoop(walker, access.value) == TruncatedUse::OperandOfBits;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    auto draftRead = [&](Reference position) {
        Draft read(Opcode::GetArrayElement, origin);
        walker.chooseConservativeFields(read);
        read.setArrayOperand(access.base);
        read.setPositionOperand(position);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        nameKindOfListTestedInFrontOfLoop(read, tested);
        return read;
    };
    auto convert = [&](Reference element, bool isInt32) -> Reference {
        bool isExact = !isInt32 || walker.types().isSubset(walker.typeOf(element), TypeTable::int32);
        if (!element || isExact || !takesBitsAlone)
            return isExact ? element : Reference();
        Draft conversion(Opcode::ToInt32, origin);
        conversion.setOperand(0, element);
        return walker.build(WTF::move(conversion));
    };
    bool mayBeInt32 = takesBitsAlone || (tested.storage == ArrayStorage::Int32 && !tested.admitsInt32Storage);
    Reference carried = walker.buildCarriedElement(access.value, access.base, purity, mayBeInt32, scopedLambda<Reference(Reference, bool, bool)>([&](Reference position, bool isInt32, bool isGuardedAtHeader) -> Reference {
        if (!isGuardedAtHeader) {
            Reference hasStorage;
            {
                Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
                hasStorage = askWhetherStorageOfListIsOfKind(walker, access.base, tested.storage, origin, false, tested.admitsInt32Storage);
            }
            if (!walker.tryHoistConditionalCheck(hasStorage, true, purity, Walker::GuardedIterations::Some))
                return { };
        }
        Reference isThere;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            Draft isPresent(Opcode::IsElementPresent, origin);
            walker.chooseConservativeFields(isPresent);
            isPresent.setArrayOperand(access.base);
            isPresent.setPositionOperand(position);
            isPresent.setArrayStorage(tested.storage);
            isPresent.setAdmitsInt32Storage(tested.admitsInt32Storage);
            isPresent.setKeyIsIntegral(true);
            isThere = walker.build(WTF::move(isPresent));
        }
        std::optional<PassedTest> present = isThere && isThere.isValue() ? walker.showElementPresentBehindTest(access.base, position, walker.tryHoistConditionalCheck(isThere, true, purity, Walker::GuardedIterations::Some)) : std::nullopt;
        if (!present)
            return { };
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft read = draftRead(position);
        read.setIsInBounds(*present);
        read.setIsPresent(*present);
        Reference element = walker.build(WTF::move(read));
        return element && element.isValue() ? convert(element, isInt32) : Reference();
    }));
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    if (!carried)
        return false;
    if (walker.checksProofs()) {
        WalkState believed = walker.state();
        Draft read = draftRead(access.key);
        if (takesBitsAlone && walker.types().isSubset(walker.typeOf(carried), TypeTable::int32)) {
            read.inputValueGiven = access.value.id();
            read.setReadsHoleAsNaN(true);
            Draft conversion(Opcode::ToInt32, origin);
            conversion.setOperand(0, walker.build(WTF::move(read)));
            read = WTF::move(conversion);
        }
        walker.checkThatMemoryAnswersAsBelieved(carried, WTF::move(read));
        walker.state() = WTF::move(believed);
    }
    if (access.key.isConstant())
        noteStoredElementInTypedElements(walker, access.base, access.key, carried);
    walker.bind(access.value.id(), carried);
    return true;
}

static bool continueIfKeyIsNonNegative(Walker&, Reference key, Origin);

static bool narrowKeyToIndex(AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Draft zero(Opcode::Integer, origin);
    Reference isNonNegative = buildIntegerOperator(walker, Opcode::GreaterEq, access.key, walker.constant(WTF::move(zero)), origin);
    if (!isNonNegative || !isNonNegative.isValue() || walker.code().isConstant(isNonNegative.id()) || !continueOrRestartIteration(walker, isNonNegative, true))
        return false;
    Reference index = buildNarrowedInNumberForms(walker, access.key, isNonNegative, OperandIndex::GreaterEq::left, true, origin);
    if (!index || !index.isRecord())
        return false;
    if (index.isValue() && index != access.key && !walker.state().checkedNameOf(access.key))
        walker.state().noteCheckedName(access.key, index);
    access.key = index;
    access.typeOfKey = walker.types().at(walker.typeOf(index));
    return isArrayIndexInTypedElements(access.typeOfKey);
}

static bool buildCarriedUpdate(const AccessOfTypedElements& access, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    ValueID store = findUpdatingStore(walker.currentBodyIndex(), access.value.id());
    std::optional<Walker::CarriedUpdate> last = walker.findCarriedUpdate(access.value.id(), store, access.base);
    if (!last)
        return false;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference isSameKey = buildIntegerOperator(walker, Opcode::StrictEq, access.key, last->key, origin);
    if (!isSameKey || !isSameKey.isValue() || walker.code().isConstant(isSameKey.id()) || walker.hasFailed())
        return false;
    BlockID sameKey = walker.addBlock();
    BlockID otherKey = walker.addBlock();
    walker.branch(isSameKey, sameKey, otherKey);
    Vector<WayInNumberForms, 2> ways;
    walker.continueIn(sameKey);
    if (!walker.isUnreachable()) {
        if (walker.checksProofs()) {
            WalkState believed = walker.state();
            Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
            read.setArrayOperand(access.base);
            read.setPositionOperand(access.key);
            read.setLevel(KnowledgeLevel::Proven);
            read.setKeyIsIntegral(true);
            read.setReadsHoleAsNaN(true);
            nameKindOfListTestedInFrontOfLoop(read, tested);
            Draft conversion(Opcode::ToInt32, origin);
            conversion.setOperand(0, walker.build(WTF::move(read)));
            walker.checkThatMemoryAnswersAsBelieved(last->bits, WTF::move(conversion));
            walker.state() = WTF::move(believed);
        }
        ways.append(WayInNumberForms { walker.currentBlock(), last->bits, { } });
    }
    walker.continueIn(otherKey);
    Reference read = walker.isUnreachable() || walker.hasFailed() ? Reference() : buildTruncatedRead(access, tested, std::nullopt);
    if (read && !walker.isUnreachable() && !walker.hasFailed())
        ways.append(WayInNumberForms { walker.currentBlock(), read, { } });
    if (walker.hasFailed())
        return true;
    if (Reference bits = meetInNumberForms(walker, walker.addBlock(), ways.span()))
        walker.bind(access.value.id(), bits);
    return true;
}

static std::optional<HoistedArrayGuard> findHoistedGuardOfArray(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(walker, access.value);
    return tested && hasKind(walker.types(), walker.typeOf(tested->name), OperandKind::Array) ? tested : std::nullopt;
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
    std::optional<HoistedArrayGuard> tested = level == KnowledgeLevel::Unknown ? std::nullopt : findHoistedGuardOfArray(access);
    std::optional<HoistedArrayGuard> rowHolder = !tested && level != KnowledgeLevel::Unknown && access.typeOfKey.isInteger() ? findRowHolderGuard(walker, access.value) : std::nullopt;
    std::optional<PassedTest> rowIsPresent = rowHolder && !access.isOnlyAskedAbout && !canGuardAccess(access) ? tryHoistElementPresenceCheck(walker, access.value, *rowHolder, access.base, access.key) : std::nullopt;
    if (rowHolder && (access.isOnlyAskedAbout || rowIsPresent || canGuardAccess(access)))
        tested = rowHolder;
    if (!tested && level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout)
        tested = tryHoistArrayGuardForAccess(walker, access.value, access.base);
    if (!tested && level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout)
        tested = tryGuardNumericArrayInIteration(walker, access.value, access.base);
    if (!tested && level != KnowledgeLevel::Unknown && !access.isOnlyAskedAbout && access.typeOfKey.isInteger())
        tested = tryGuardArrayInRecursion(walker, access.value, access.base);
    if (walkStoodInFront && (walker.isUnreachable() || walker.hasFailed()))
        return true;
    if (!tested && level == KnowledgeLevel::Expected && !access.isOnlyAskedAbout && buildReadBehindTestsInTurnOfTypedElements(access))
        return true;
    if (!tested && level == KnowledgeLevel::Expected && !access.isOnlyAskedAbout && access.typeOfKey.isInteger()) {
        tested = tryGuardInlineeArrayInIteration(walker, access.value, access.base);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
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
    if (tested && !tested->onlyHoldsRows && buildCarriedRead(access, *tested))
        return true;

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
    bool listHasNoHoles = listHasNoHolesByLink(access);
    read.setListHasNoHoles(listHasNoHoles);
    if (listHasNoHoles)
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
    bool isOfEitherNumericStorage = tested && !tested->onlyHoldsRows && tested->storage == ArrayStorage::Double && tested->admitsInt32Storage && !tested->written;
    bool isIndex = isArrayIndexInTypedElements(access.typeOfKey);
    bool readsUpdatedBits = tested && tested->isWritable && isReadOfBody(access) && canGuardAccess(access) && findUpdatingStore(walker.currentBodyIndex(), access.value.id());
    bool isOfInt32Storage = tested && tested->storage == ArrayStorage::Int32 && !tested->admitsInt32Storage && !tested->written;
    TruncatedUse truncatedUse = (isOfEitherNumericStorage || isOfInt32Storage) && !presentAtHead && access.typeOfKey.isInteger() && walker.isInLoop() && walker.has(Mechanism::UnboxedNumbers) && isReadOfBody(access) ? findTruncatedUseInLoop(walker, access.value) : TruncatedUse::None;
    bool isKnownInIteration = truncatedUse == TruncatedUse::OperandOfBits && (walker.state().element(access.base, access.key) || walker.state().elementWithNaNForHole(access.base, access.key));
    bool isTruncatedInLoop = truncatedUse == TruncatedUse::OperandOfBits && !isKnownInIteration && mayTruncateRead(walker, access.value);
    auto isWholeRemainderTerm = [&] {
        if (!isOfEitherNumericStorage || truncatedUse != TruncatedUse::None || !isReadOfBody(access) || !isTermOfRemainder(walker.currentBodyIndex(), access.value.id()))
            return false;
        TypeID seen = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), access.value.id());
        seen = seen ? walker.types().withoutPrimitives(seen, Primitive::Undefined | Primitive::Absent) : TypeID();
        return seen && !isNeverType(seen) && walker.types().at(seen).isNumberWithoutFraction();
    };
    if (isTruncatedInLoop && isOfInt32Storage && tested->isWritable)
        walker.count(StatisticID::TruncatedReadsOfWrittenArrays);
    AccessOfTypedElements guarded = access;
    if (isTruncatedInLoop && readsUpdatedBits && !inside && access.key.isValue() && !access.key.isConstant()) {
        isIndex = isIndex || narrowKeyToIndex(guarded);
        if (isIndex)
            walker.count(StatisticID::UpdatedBitsReadPastEnd);
        if (isIndex && !walker.isUnreachable() && !walker.hasFailed() && buildCarriedUpdate(guarded, *tested))
            return true;
    }
    if (isTruncatedInLoop && !isIndex && !inside && !walker.isUnreachable() && !walker.hasFailed())
        inside = guardKeyBelowLength(guarded);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    if ((truncatedUse == TruncatedUse::TermOfSums && isOfEitherNumericStorage && !tested->isWritable && isIndex && canGuardAccess(access)) || (isTruncatedInLoop && (isIndex || inside))) {
        Reference integer = truncatedUse == TruncatedUse::OperandOfBits ? buildTruncatedRead(guarded, *tested, inside) : buildIntegerReadByStorage(access, *tested, inside);
        if (integer || !walker.isUnreachable())
            walker.bind(access.value.id(), integer);
        return true;
    }
    if (isOfEitherNumericStorage && !tested->isWritable && !presentAtHead && isIndex && walker.isInLoop() && walker.has(Mechanism::UnboxedNumbers) && canGuardAccess(access) && isWholeRemainderTerm()) {
        Reference integer = buildIntegerReadByStorage(access, *tested, inside, true);
        if (integer || !walker.isUnreachable())
            walker.bind(access.value.id(), integer);
        return true;
    }
    bool mayBeIndex = isOfEitherNumericStorage && !tested->isWritable && truncatedUse == TruncatedUse::None && !presentAtHead && (isIndex || inside) && walker.isInLoop() && walker.has(Mechanism::UnboxedNumbers) && isReadOfBody(access) && canGuardAccess(access);
    if (mayBeIndex && isUsedForKey(walker, access.value.id(), KeyHolders::AnyValues)) {
        Reference integer = buildIntegerReadByStorage(access, *tested, inside, true);
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
    bool takesNaNForHole = tested && !tested->onlyHoldsRows && read.arrayStorage() == ArrayStorage::Double && (read.isInBounds() ? !read.admitsInt32Storage() || tested->turnsStoreIntoLists : tested->turnsStoreIntoLists && isArrayIndexInTypedElements(access.typeOfKey)) && everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing);
    if (!takesNaNForHole && tested && tested->turnsStoreIntoLists && !tested->isWritable && isNumericStorage(read.arrayStorage()) && onlyUserHoldsItAgainstNumberInTypedElements(walker, access.value))
        takesNaNForHole = true;
    if (takesNaNForHole && tested->turnsStoreIntoLists)
        walker.count(StatisticID::StoringLoopHoleChecksRemoved);
    bool leavesTurnAtHole = !presentAtHead && !takesNaNForHole && tested && canGuardAccess(access) && (tested->onlyHoldsRows || (walker.canGuardRecursion() && isReadOfBody(access) && isElementUsedNumerically(walker, walker.currentBodyIndex(), access.value.id())) || holeLeavesTurnBehindRow(walker, access.value, *tested));
    bool findsNothingOutsideList = read.isInBounds() || isArrayIndexInTypedElements(access.typeOfKey);
    ValueID relationFailedByHole = !leavesTurnAtHole && !presentAtHead && !takesNaNForHole && tested && tested->turnsStoreIntoLists && !tested->onlyHoldsRows && isNumericStorage(read.arrayStorage()) && !read.admitsInt32Storage() && findsNothingOutsideList && read.inputValueGiven == access.value.id() && walker.isInLoop() ? findRelationFalseForHole(walker, access.value, access.base) : ValueID();
    bool canTakeNaN = !leavesTurnAtHole && !relationFailedByHole && tested && !tested->onlyHoldsRows && findsNothingOutsideList && isNumericStorage(read.arrayStorage());
    std::optional<Walker::CallOfCopy> computingCaller = canTakeNaN ? findComputingCaller(walker, access.value) : std::nullopt;
    if (canTakeNaN && (computingCaller || everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing)))
        read.setReadsHoleAsNaN(true);
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    bool isNumberOrNaN = tested && !findsNothingOutsideList && !leavesTurnAtHole && isNumericStorage(read.arrayStorage()) && everyUserTakesUndefinedForNaNInTypedElements(walker, access.value, WhatStandsBesideRead::NumberOrNothing);
    bool takesNumber = !isNumericStorage(read.arrayStorage()) || read.readsHoleAsNaN() || leavesTurnAtHole || isNumberOrNaN;
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
    if (tested && !tested->isWritable && read.arrayStorage() == ArrayStorage::Double && read.admitsInt32Storage() && walker.isQuietVersionOfUnswitchLoop()) {
        if (!givesNaNForHole)
            walker.noteInt32Candidate();
        else if (!tested->turnsStoreIntoLists && access.base.isValue())
            walker.noteNumericRead(access.value.id(), access.base);
    }
    Reference element = leavesTurnAtHole || relationFailedByHole ? walker.state().element(access.base, access.key) : Reference();
    if (presentInFront) {
        if (std::exchange(read.testsPassed, std::nullopt))
            read.setIsInBounds(*presentInFront);
        read.setIsPresent(*presentInFront);
    }
    bool isReadInFront = presentInFront && !isNeverType(walker.resultTypeOf(read));
    if (tested && !isReadInFront)
        takeStorageAndLengthFromInFrontOfLoop(walker, read, *tested);
    bool findsElement = listHasNoHoles && read.isInBounds();
    std::optional<Draft> readAgain;
    if (givesNaNForHole && !isReadInFront) {
        element = walker.state().elementWithNaNForHole(access.base, access.key);
        if (element) {
            walker.count(StatisticID::NumericElementReadsEliminated);
            walker.noteReadAnsweredByHoleReadAsNaN(access.value.id(), element);
        }
    }
    if (!element || !walker.types().at(walker.typeOf(element)).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined)) {
        if ((leavesTurnAtHole || relationFailedByHole) && !isReadInFront && !guardElementPresenceInIteration(walker, read, access.base, access.key, *tested, relationFailedByHole))
            return true;
        if (read.isPresent())
            read.setListHasNoHoles(false);
        if (isReadInFront && walker.checksBounds())
            readAgain = read;
        Walker::SpecializedPreheaderScope preheaderScope(walker, isReadInFront, walker.arrayGuardPurity());
        if (isReadInFront && tested && tested->name == access.base && walker.isBuildingInsideLoop())
            takeStorageAndLengthFromInFrontOfLoop(walker, read, *tested);
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
    if (tested && !givesNaNForHole && isReadOfBody(access))
        walker.carryReadElement(access.value, access.base, element);
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
    if (element && element.isValue() && tested && tested->storage == ArrayStorage::Int32 && !tested->admitsInt32Storage && findsElement && walker.isInLoop() && isReadOfBody(access) && walker.representationOf(element) != Representation::Int32 && (isTermOfRemainder(walker.currentBodyIndex(), access.value.id()) || isCounterStartTerm(walker.currentBodyIndex(), access.value.id()))) {
        Draft conversion(Opcode::ToInt32, walker.originOf(access.value));
        conversion.setOperand(0, element);
        element = walker.build(WTF::move(conversion));
    }
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

static bool fieldHoldsFractionsByLink(const AccessOfTypedElements& access)
{
    static constexpr unsigned maxCalls = 4;
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!isReadOfBody(access))
        return false;
    auto fieldReadIn = [](const BodyIndex& body, const BodyValue& operation) {
        ValueID array = body.stripCasts(operation.objectOperand());
        return array && !body.isParameter(array) && body.opcodeOf(array) == Opcode::PropGet && !body.value(array).isFreeName() ? body.value(array).key() : PropertyKeyID();
    };
    PropertyKeyID field = fieldReadIn(index, access.value);
    if (!field)
        return false;
    auto readsFraction = [&](FunctionID function, const BodyIndex& body) {
        if (!body.opcodes().contains(Opcode::ElemGet))
            return false;
        for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
            for (BodyValue read : body.block(BlockID(number)).values()) {
                if (read.opcode() != Opcode::ElemGet || fieldReadIn(body, read) != field)
                    continue;
                TypeID seen = walker.facts().linked().resultOfReadByKey(function, read.id());
                if (seen && !isNeverType(seen) && (walker.types().at(seen).primitives & Primitive::FractionalNumber))
                    return true;
            }
        }
        return false;
    };
    if (readsFraction(walker.currentFunction(), index))
        return true;
    unsigned calls = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue call : index.block(BlockID(number)).values()) {
            if (call.opcode() != Opcode::Call)
                continue;
            FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id());
            const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
            if (body && readsFraction(callee, *body))
                return true;
            if (++calls == maxCalls)
                return false;
        }
    }
    return false;
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
    static constexpr unsigned maxJoinDepth = 3;
    auto isNeverNaN = [&](auto& isNeverNaN, ValueID value, unsigned depth) -> bool {
        ValueID held = behindNames(value);
        while (conversionsOfForm.contains(code.at(held).opcode))
            held = behindNames(code.at(held).operand(0));
        const Operation& chosen = code.at(held);
        if (isSuchRead(held) || (depth && walker.types().at(chosen.type).isInteger()))
            return true;
        BlockID join = chosen.opcode == Opcode::Parameter && depth < maxJoinDepth ? chosen.block() : BlockID();
        if (!join || code.at(join).isLoopHead || code.at(join).isEntry || !code.at(join).firstEdgeIn())
            return false;
        std::span<const ValueID> parameters = code.at(join).parameters();
        size_t position = std::ranges::find(parameters, held) - parameters.begin();
        bool isEveryArm = position < parameters.size();
        code.forEachIncomingEdgeUnsealed(join, [&](const Edge& edge) {
            isEveryArm = isEveryArm && position < edge.arguments().size() && isNeverNaN(isNeverNaN, edge.arguments()[position], depth + 1);
        });
        return isEveryArm;
    };
    return isNeverNaN(isNeverNaN, stored.id(), 0);
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
    for (ValueID next : walker.nextStoresUnderSameKey(first)) {
        BodyValue later = index.value(next);
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

static bool callsElementMethodAfterStore(const AccessOfTypedElements& access)
{
    static constexpr uint32_t maxOperations = 40;
    const Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    if (access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    return loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            if (operation.opcode() != Opcode::Call || operation.id().index() < access.value.id().index())
                return false;
            FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id());
            const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
            return body && !body->hasLoops() && body->holdsAnyOf(OpcodeSet { Opcode::ElemGet, Opcode::ElemSet }) && walker.facts().summaries().at(callee).numberOfOperations <= maxOperations;
        });
    });
}

static bool mayRestartAfterStore(const AccessOfTypedElements& access)
{
    return turnStoresAgainBehindInTypedElements(access) || readsNamesAfterStore(access) || readsRowsAfterStore(access) || callsElementMethodAfterStore(access);
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
    if (opcode == Opcode::ResultAt) {
        auto result = index.value(made).as<Opcode::ResultAt>();
        ValueID step = result.resultsOperand();
        return result.index() == 1 && step && !index.isParameter(step) && index.opcodeOf(step) == Opcode::IteratorResume;
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

static bool callsMethodNamed(const BodyIndex& index, const BodyValue& call, PropertyKeyID key, size_t numberOfArguments)
{
    if (call.opcode() != Opcode::Call)
        return false;
    auto called = call.as<Opcode::Call>();
    ValueID member = called.thisValueOperand() && called.argumentsOperands().size() == numberOfArguments ? index.stripCasts(called.calleeOperand()) : ValueID();
    return member && !index.isParameter(member) && index.opcodeOf(member) == Opcode::PropGet && !index.value(member).isFreeName() && index.value(member).key() == key;
}

static bool isDeclaredWithoutNumbers(Walker& walker, const BodyIndex& index, ValueID value)
{
    for (ValueID cast = value; cast && !index.isParameter(cast) && opcodesThatStateTypes.contains(index.opcodeOf(cast)); cast = index.value(cast).operand(0)) {
        TypeID written = index.opcodeOf(cast) == Opcode::Convert ? index.value(cast).as<Opcode::Convert>().declared().written() : TypeID();
        if (written && !walker.types().isUninformative(written) && !(walker.types().at(written).primitives & Primitive::Number))
            return true;
    }
    return false;
}

static bool isNumberByText(Walker& walker, const BodyIndex& index, ValueID value, ValueID assumed = { }, unsigned stepsLeft = 4)
{
    ValueID made = value ? index.stripCasts(value) : ValueID();
    if (!made || !stepsLeft)
        return false;
    if (made == assumed)
        return true;
    auto isNumber = [&](ValueID operand) { return isNumberByText(walker, index, operand, assumed, stepsLeft - 1); };
    if (index.isParameter(made)) {
        BlockID head = index.blockOf(made);
        if (assumed || !index.isLoopHead(head) || index.isEnteredByHandlerEdge(head))
            return false;
        uint32_t position = made.index() - index.block(head).firstParameter().index();
        return std::ranges::all_of(walker.incomingEdgesOf(index, head), [&](const BodyIncomingEdge& edge) {
            return position < edge.target.arguments().size() && isNumberByText(walker, index, edge.target.arguments()[position], made, stepsLeft - 1);
        });
    }
    switch (Opcode opcode = index.opcodeOf(made)) {
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::ToNumber:
        return true;
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::Negate:
        return isNumber(index.value(made).operand(0));
    case Opcode::Add:
        return isNumber(index.value(made).operand(0)) && isNumber(index.value(made).operand(1));
    default:
        return (allNumericOperators.contains(opcode) || allBitwiseOperators.contains(opcode)) && numberOfNamedOperandsOf(opcode) == 2 && (isNumber(index.value(made).operand(0)) || isNumber(index.value(made).operand(1)));
    }
}

static bool pushesOntoUntouchedLiteral(Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    if (!callsMethodNamed(index, call, walker.program().keyOf(BuiltinNames::push), 1))
        return false;
    auto called = call.as<Opcode::Call>();
    ValueID literal = index.stripCasts(called.thisValueOperand());
    auto loopsReadNumbers = [&] {
        TypeID read = walker.literalElementTypeInLoops(literal);
        return read && (walker.types().at(read).primitives & Primitive::Number) && walker.types().at(read).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined | Primitive::Absent);
    };
    if (index.isParameter(literal) || (index.opcodeOf(literal) == Opcode::NewArray && !std::ranges::all_of(index.value(literal).as<Opcode::NewArray>().elementsOperands(), [&](ValueID element) {
        return isNumberByText(walker, index, element);
    }) && !loopsReadNumbers()))
        return false;
    return mayBeNumberByText(index, called.argumentsOperands()[0]) && !isDeclaredWithoutNumbers(walker, index, called.argumentsOperands()[0]) && isUntouchedLiteral(index, called.thisValueOperand(), index.loopOf(index.blockOf(call.id())));
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
    static constexpr OpcodeSet elementReads { Opcode::ElemGet, Opcode::IteratorResume };
    if (!index.opcodes().contains(Opcode::Call) || !index.holdsAnyOf(elementReads))
        return { };
    Vector<LoopID, 4> loopsThatRead;
    Vector<ValueID, 4> iteratedObjects;
    auto isIteratedValue = [&](ValueID value) { return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::ResultAt; };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (elementReads.contains(operation.opcode()))
                loopsThatRead.appendIfNotContains(index.loopOf(member));
            else if (operation.opcode() == Opcode::Call && pushesOntoUntouchedLiteral(*this, index, operation))
                pushes->append(operation.id().index());
            else if (operation.opcode() == Opcode::PropGet && !operation.isFreeName()) {
                if (ValueID object = index.stripCasts(operation.as<Opcode::PropGet>().objectOperand()); isIteratedValue(object))
                    iteratedObjects.appendIfNotContains(object);
            }
        }
    }
    pushes->removeAllMatching([&](uint32_t call) {
        return !loopsThatRead.contains(index.loopOf(index.blockOf(ValueID(call)))) || iteratedObjects.contains(index.stripCasts(index.value(ValueID(call)).as<Opcode::Call>().argumentsOperands()[0]));
    });
    std::ranges::sort(*pushes);
    return pushes->span();
}

bool Walker::loopsPushNumbersOnto(ValueID literal)
{
    const BodyIndex& index = currentBodyIndex();
    const Vector<ValueID>& literals = m_literalsPushedOntoByLoops.ensure(&index, [&] {
        Vector<ValueID> found;
        if (!index.numberOfLoops() || !index.holdsAnyOf(OpcodeSet { Opcode::NewArray }) || !index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }))
            return found;
        for (uint32_t number = 0; number < index.numberOfLoops(); ++number) {
            for (uint32_t call : pushesOntoLiteralsOf(LoopID(number)))
                found.appendIfNotContains(index.stripCasts(index.value(ValueID(call)).as<Opcode::Call>().thisValueOperand()));
        }
        return found;
    }).iterator->value;
    return literals.contains(literal);
}

bool Walker::isPushOntoLiteral(const BodyValue& call)
{
    const BodyIndex& index = currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words() || !callsMethodNamed(index, call, program().keyOf(BuiltinNames::push), 1))
        return false;
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    return loop && std::ranges::binary_search(pushesOntoLiteralsOf(loop), call.id().index());
}

bool Walker::isOutermostPushPopLoop(LoopID loop)
{
    static constexpr OpcodeSet otherCalls { Opcode::CallDirect, Opcode::CallList, Opcode::Construct, Opcode::SuperConstruct };
    static constexpr OpcodeSet numbersOrElements { Opcode::ElemGet, Opcode::ResultAt, Opcode::Add };
    const BodyIndex& index = currentBodyIndex();
    auto mayPushNumber = [&](const BodyValue& push) {
        ValueID pushed = index.stripCasts(push.as<Opcode::Call>().argumentsOperands()[0]);
        if (!pushed || index.isParameter(pushed) || numbersOrElements.contains(index.opcodeOf(pushed)))
            return true;
        TypeID type = widestTypeOfOperation(*this, index.value(pushed));
        type = type ? m_types.withoutPrimitives(type, Primitive::Undefined) : TypeID();
        return type && !isNeverType(type) && m_types.at(type).isNumber();
    };
    Vector<std::optional<bool>>& ofLoops = m_outermostPushPopLoops.ensure(&index, [&] {
        Vector<std::optional<bool>> none;
        none.grow(index.numberOfLoops());
        return none;
    }).iterator->value;
    std::optional<bool>& answer = ofLoops[loop.index()];
    if (!answer) {
        answer = !index.loop(loop).parent && std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
                if (operation.opcode() != Opcode::Call)
                    return !otherCalls.contains(operation.opcode());
                if (callsMethodNamed(index, operation, program().keyOf(BuiltinNames::push), 1))
                    return mayPushNumber(operation);
                return callsMethodNamed(index, operation, program().keyOf(BuiltinNames::pop), 0) || isMathCall(*this, index, operation);
            });
        });
    }
    return *answer;
}

bool Walker::isPopFromLiteral(const BodyValue& call)
{
    const BodyIndex& index = currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words() || !callsMethodNamed(index, call, program().keyOf(BuiltinNames::pop), 0))
        return false;
    ValueID literal = index.stripCasts(call.as<Opcode::Call>().thisValueOperand());
    auto pushesOntoLiteral = [&](LoopID loop) {
        return std::ranges::any_of(pushesOntoLiteralsOf(loop), [&](uint32_t push) {
            return index.stripCasts(index.value(ValueID(push)).as<Opcode::Call>().thisValueOperand()) == literal;
        });
    };
    LoopID around = index.loopOf(index.blockOf(call.id()));
    if (!around)
        return false;
    for (LoopID loop = around; loop; loop = index.loop(loop).parent) {
        if (pushesOntoLiteral(loop))
            return true;
    }
    static constexpr OpcodeSet literals { Opcode::NewArray, Opcode::Data };
    if (index.isParameter(literal) || !literals.contains(index.opcodeOf(literal)) || index.isInLoop(index.blockOf(literal), around))
        return false;
    for (uint32_t loop = 0; loop < index.numberOfLoops(); ++loop) {
        if (pushesOntoLiteral(LoopID(loop)))
            return true;
    }
    return false;
}

Vector<ValueID, 4> Walker::nextStoresUnderSameKey(const BodyValue& first)
{
    const BodyIndex& index = currentBodyIndex();
    auto blockAndKeyOf = [&](const BodyValue& store) {
        ValueID key = index.stripCasts(store.propertyOperand());
        return static_cast<uint64_t>(index.blockOf(store.id()).index()) << 32 | (key ? key.index() : std::numeric_limits<uint32_t>::max());
    };
    auto placeOf = [](const StoreUnderKey& store) { return std::pair { store.blockAndKey, store.store }; };
    auto holderOf = [&](const StoreUnderKey& store) {
        ValueID holder = index.stripCasts(index.value(ValueID(store.store)).objectOperand());
        if (holder && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::CheckInitialized)
            holder = index.stripCasts(index.value(holder).operand(0));
        if (holder && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::GetModuleVariable)
            return uint64_t { 1 } << 32 | index.value(holder).variable().index();
        return static_cast<uint64_t>(holder ? holder.index() : store.store);
    };
    const Vector<StoreUnderKey>& stores = m_storesByKeyOfBlocks.ensure(&index, [&] {
        Vector<StoreUnderKey> found;
        if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemSet }))
            return found;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            for (BodyValue operation : index.block(BlockID(block)).values()) {
                if (operation.opcode() == Opcode::ElemSet)
                    found.append(StoreUnderKey { blockAndKeyOf(operation), operation.id().index() });
            }
        }
        std::ranges::sort(found, { }, placeOf);
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> newestOfHolders;
        for (size_t place = 0; place < found.size(); ++place) {
            if (place && found[place].blockAndKey != found[place - 1].blockAndKey)
                newestOfHolders.clear();
            uint32_t& newest = newestOfHolders.add(holderOf(found[place]), 0).iterator->value;
            found[place].previousOfHolder = std::exchange(newest, found[place].store);
        }
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> holders;
        for (size_t place = found.size(); place--;) {
            if (place + 1 < found.size() && found[place].blockAndKey != found[place + 1].blockAndKey)
                holders.clear();
            found[place].numberOfHoldersAfter = holders.size();
            holders.add(holderOf(found[place]));
        }
        return found;
    }).iterator->value;
    uint64_t asked = blockAndKeyOf(first);
    Vector<ValueID, 4> next;
    auto store = std::ranges::lower_bound(stores, std::pair { asked, first.id().index() }, { }, placeOf);
    if (store == stores.end() || store->store != first.id().index())
        return next;
    for (size_t wanted = store->numberOfHoldersAfter; next.size() < wanted;) {
        ++store;
        if (store->previousOfHolder <= first.id().index())
            next.append(ValueID(store->store));
    }
    return next;
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

static bool isLiteralFilledByEarlierLoop(Walker& walker, const BodyIndex& index, ValueID array, LoopID loop)
{
    static constexpr OpcodeSet literals { Opcode::NewArray, Opcode::Data };
    ValueID literal = index.stripCasts(array);
    if (!literal || index.isParameter(literal) || !literals.contains(index.opcodeOf(literal)) || index.isInLoop(index.blockOf(literal), loop))
        return false;
    std::optional<BodyUser> first = index.firstUser(literal);
    while (first && first->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(first->value)))
        first = index.firstUser(first->value);
    if (!first || first->kind != BodyUseKind::Operand || index.opcodeOf(first->value) != Opcode::ElemSet)
        return false;
    BodyValue store = index.value(first->value);
    return index.stripCasts(store.objectOperand()) == literal && index.stripCasts(store.valueOperand()) != literal && storesUnderSteppedCounter(walker, index, store);
}

static bool mayAppendToLiteral(const AccessOfTypedElements& access, const HoistedArrayGuard& tested)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!tested.name.isValue() || access.value.id().index() >= index.numberOfValues() || index.value(access.value.id()).words() != access.value.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.value.id()));
    if (loop && storesFromInitialLength(walker, index, access.value))
        return true;
    if (loop && storesFromRowLength(walker, index, access.value)) {
        walker.count(StatisticID::RowStoresMaybeAppending);
        return true;
    }
    if (loop && findUpdatedRead(index, access.value))
        return true;
    bool isUntouched = false;
    for (LoopID outer = loop; outer && !isUntouched; outer = index.loop(outer).parent)
        isUntouched = isUntouchedLiteral(index, access.value.objectOperand(), outer);
    if (!isUntouched && !(loop && isLiteralFilledByEarlierLoop(walker, index, access.value.objectOperand(), loop)))
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

static Reference finalLengthInLoop(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = isReadOfBody(access) ? index.loopOf(index.blockOf(access.value.id())) : LoopID();
    BlockID head = loop ? index.loop(loop).head : BlockID();
    ValueID counter = head && access.value.opcode() == Opcode::ElemSet ? index.stripCasts(access.value.propertyOperand()) : ValueID();
    if (!counter || !index.isParameter(counter) || index.blockOf(counter) != head || index.terminator(head).kind() != TerminatorKind::Branch)
        return { };
    static constexpr OpcodeSet opcodesOfSuchLoops = allNumericOperators | allRelations | allEqualities | opcodesThatStateTypes
        | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::ElemGet, Opcode::ElemSet, Opcode::LoopHint, Opcode::ToBoolean, Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement, Opcode::Undefined };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (member != head && !index.isInLoop(target.block(), loop))
                return { };
        }
        auto canBeInSuchLoop = [&](BodyValue operation) {
            return opcodesOfSuchLoops.contains(operation.opcode()) || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length));
        };
        if (!std::ranges::all_of(index.block(member).values(), canBeInSuchLoop))
            return { };
    }
    auto isSteppedBy = [&](Opcode step, ValueID parameter) {
        uint32_t position = parameter.index() - index.block(head).firstParameter().index();
        return std::ranges::all_of(walker.incomingEdgesOf(index, head), [&](const BodyIncomingEdge& edge) {
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID next = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
            return !index.isInLoop(edge.from, loop) || (next && !index.isParameter(next) && index.opcodeOf(next) == step && index.stripCasts(index.value(next).operand(0)) == parameter);
        });
    };
    if (!isSteppedBy(Opcode::Increment, counter) || !BackEdgeDominators(index, loop).contains(index.blockOf(access.value.id())))
        return { };
    BodyTargets::Iterator target = index.terminator(head).targets().begin();
    ValueID asked = index.terminator(head).as<TerminatorKind::Branch>().conditionOperand();
    if (!index.isInLoop((*target).block(), loop) || !asked || index.isParameter(asked) || index.blockOf(asked) != head)
        return { };
    BodyValue test = index.value(asked);
    auto integerBoundTo = [&](ValueID value) {
        Reference bound = walker.referenceOf(value);
        return bound && bound.isRecord() && walker.types().at(walker.typeOf(bound)).isInteger() ? bound : Reference();
    };
    if (test.opcode() == Opcode::Less)
        return index.stripCasts(test.operand(0)) == counter ? integerBoundTo(test.operand(1)) : Reference();
    if (test.opcode() == Opcode::LessEq) {
        Reference last = index.stripCasts(test.operand(0)) == counter ? integerBoundTo(test.operand(1)) : Reference();
        if (!last)
            return { };
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        Draft one(Opcode::Integer, origin);
        one.setInteger(1);
        return buildIntegerOperator(walker, Opcode::Add, last, walker.constant(WTF::move(one)), origin);
    }
    ValueID left = test.opcode() == Opcode::ToBoolean ? index.stripCasts(test.operand(0)) : ValueID();
    if (!left || left == counter || !index.isParameter(left) || index.blockOf(left) != head)
        return { };
    if (!isSteppedBy(Opcode::Decrement, left))
        return { };
    Reference iterationsLeft = integerBoundTo(left);
    if (!iterationsLeft || walker.types().at(walker.typeOf(iterationsLeft)).integers.minimum < 1)
        return { };
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    return buildIntegerOperator(walker, Opcode::Add, access.key, iterationsLeft, origin);
}

static Reference grownCapacity(const AccessOfTypedElements& access, Origin origin)
{
    Walker& walker = access.walker;
    if (Reference final = finalLengthInLoop(access)) {
        walker.count(StatisticID::CapacitiesReservedFromSourceArrays);
        return final;
    }
    auto integer = [&](int64_t number) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(number);
        return walker.constant(WTF::move(written));
    };
    return buildIntegerOperator(walker, Opcode::Add, buildIntegerOperator(walker, Opcode::Add, access.key, buildIntegerOperator(walker, Opcode::URShift, access.key, integer(1), origin), origin), integer(4), origin);
}

static void readLengthThroughStorageInFrontOfLoop(Walker& walker, Draft& length, const HoistedArrayGuard& tested, Origin origin)
{
    if (Reference pointer = storageOfListFromInFrontOfLoop(walker, tested, origin))
        length.setStorageOperand(pointer);
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
    auto [capacity, isWithin] = compareWithCapacity();
    if (!isWithin || !isWithin.isValue() || walker.isUnreachable())
        return false;
    BlockID hasRoom = walker.addBlock();
    BlockID isFull = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isWithin, hasRoom, isFull, true);
    walker.continueIn(isFull);
    bool runsAgain = false;
    if (!walker.isUnreachable()) {
        Draft grow(Opcode::ReserveArrayCapacity, origin);
        walker.chooseConservativeFields(grow);
        grow.setArrayOperand(tested.name);
        grow.setCapacityOperand(grownCapacity(access, origin));
        runsAgain = walker.buildGrowthOfList(WTF::move(grow));
        if (!runsAgain)
            walker.jumpTo(hasRoom);
        else if (Reference isWithinGrown = compareWithCapacity().second; isWithinGrown && continueOrRestartIteration(walker, isWithinGrown, true))
            walker.runIterationAgainBehindReads();
    }
    walker.continueIn(hasRoom);
    if (walker.isUnreachable() || walker.hasFailed())
        return false;
    if (!runsAgain) {
        std::tie(capacity, isWithin) = compareWithCapacity();
        if (!isWithin || !continueOrRestartIteration(walker, isWithin, true))
            return false;
    }
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
    auto readCapacity = [&] {
        Draft capacity(Opcode::GetArrayCapacity, origin);
        walker.chooseConservativeFields(capacity);
        capacity.setArrayOperand(tested.name);
        return walker.build(WTF::move(capacity));
    };
    auto positionBelow = [&](Reference bound, Reference isBelow) -> std::optional<PassedTest> {
        std::optional<PassedTest> below = isBelow ? walker.findPassedTest(isBelow) : std::nullopt;
        return below ? walker.boundsProofFromChecks(access.base, access.key, bound, fromZeroUp, *below) : std::nullopt;
    };
    auto append = [&](Reference capacity, Reference isWithinCapacity, Reference pointer) {
        Draft extend(Opcode::ExtendArrayLength, origin);
        walker.chooseConservativeFields(extend);
        extend.setArrayOperand(access.base);
        extend.setPositionOperand(access.key);
        if (pointer)
            extend.setStorageOperand(pointer);
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
        if (pointer)
            put.setStorageOperand(pointer);
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
    if (walker.shouldSkipCapacityChecks() && !walker.isUnreachable())
        append({ }, { }, { });
    else if (!walker.isUnreachable()) {
        Reference capacity = readCapacity();
        Reference isWithinCapacity = capacity ? buildIntegerOperator(walker, Opcode::Less, access.key, capacity, origin) : Reference();
        if (isWithinCapacity && isWithinCapacity.isValue() && !walker.isUnreachable()) {
            BlockID hasRoom = walker.addBlock();
            BlockID isFull = walker.addBlock(BlockFrequency::Rare);
            walker.branch(isWithinCapacity, hasRoom, isFull, true);
            walker.continueIn(isFull);
            Draft grow(Opcode::ReserveArrayCapacity, origin);
            walker.chooseConservativeFields(grow);
            grow.setArrayOperand(tested.name);
            grow.setCapacityOperand(grownCapacity(access, origin));
            bool runsAgain = walker.buildGrowthOfList(WTF::move(grow));
            Reference grown = readCapacity();
            Reference isWithinGrown = grown ? buildIntegerOperator(walker, Opcode::Less, access.key, grown, origin) : Reference();
            if (isWithinGrown && continueOrRestartIteration(walker, isWithinGrown, true)) {
                if (runsAgain)
                    walker.runIterationAgainBehindReads();
                else
                    append(grown, isWithinGrown, { });
            }
            walker.continueIn(hasRoom);
            append(capacity, isWithinCapacity, walker.isUnreachable() ? Reference() : storageOfListFromInFrontOfLoop(walker, tested, origin));
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

static bool storesNumberFromBeforeCallers(const AccessOfTypedElements& access, const std::optional<HoistedArrayGuard>& tested)
{
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    if (tested || !walker.depthOfCopies() || access.counterThatPushGoesBy || access.isPushAtLength || !access.stored || !access.stored.isValue() || !access.typeOfStored || !types.overlap(access.typeOfStored, TypeTable::number))
        return false;
    if (walker.representationOf(access.stored) != Representation::JSValue || !walker.isInvariantInSpecializedVersion(access.stored, Walker::IterationPurity::KnownEffectsOnly) || !canGuardAccess(access))
        return false;
    const BodyIndex* index = &walker.currentBodyIndex();
    FunctionID function = walker.currentFunction();
    ValueID passed = index->stripCasts(access.value.valueOperand());
    for (size_t outward = 0; passed && index->isParameter(passed) && index->blockOf(passed) == entryBlock; ++outward) {
        if (types.at(index->value(passed).parameterType()).isNumber())
            return true;
        std::optional<Walker::CallOfCopy> copied = walker.callOfCopy(outward);
        std::optional<uint32_t> parameter = copied ? index->block(entryBlock).indexOfParameter(passed) : std::nullopt;
        const Function& callee = walker.program().at(function);
        if (!parameter || callee.entryParameterKind(*parameter) != EntryParameter::Parameter)
            return false;
        std::span<const ValueID> handed = copied->index->value(copied->call).argumentsOperands();
        uint32_t position = *parameter - callee.firstParameterIndex();
        if (position >= handed.size() || !handed[position])
            return false;
        index = copied->index;
        function = copied->caller;
        passed = index->stripCasts(handed[position]);
    }
    return false;
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
    std::optional<HoistedArrayGuard> tested = isPush ? findListTestedForStoresInFrontOfLoop(walker, access.base) : findHoistedGuardOfArray(access);
    bool hasBoxedElements = tested && tested->isWritable && tested->storage == ArrayStorage::Contiguous;
    bool storesByAnyKind = tested && tested->isWritable && tested->storage == ArrayStorage::Other && tested->admitsInt32Storage && (!isPush || access.isPushAtLength) && access.typeOfStored && !types.isSubset(access.typeOfStored, TypeTable::int32);
    bool storesInt32AmongBoxes = storesNumber && hasBoxedElements && !isPush && !tested->admitsInt32Storage && types.isSubset(access.typeOfStored, TypeTable::int32);
    if (storesNumber ? hasBoxedElements && !storesInt32AmongBoxes : (isPush && !access.isPushAtLength) || !access.typeOfStored || (types.at(access.typeOfStored).primitives & Primitive::Absent))
        return false;
    bool rowAsksForWritableList = tested && tested->isWritable;
    if (!storesNumber && !hasBoxedElements && !storesByAnyKind) {
        if (storesNumberFromBeforeCallers(access, tested)) {
            AccessOfTypedElements number = access;
            number.typeOfStored = TypeTable::number;
            number.storesNumberFromBefore = true;
            if (!access.isOnlyAskedAbout) {
                number.stored = readAsNumber(access);
                walker.count(StatisticID::CallerNumbersStoredByKind);
            }
            return number.stored && buildStoreBehindRowOfTypedElements(number);
        }
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
        bool isReadForDoubles = tested && tested->storage == ArrayStorage::Double && !tested->admitsInt32Storage;
        ArrayStorage asked = isReadForDoubles || (types.at(access.typeOfStored).primitives & Primitive::FractionalNumber) ? ArrayStorage::Double : ArrayStorage::Int32;
        if (!tested && asked == ArrayStorage::Int32 && fieldHoldsFractionsByLink(access)) {
            asked = ArrayStorage::Double;
            walker.count(StatisticID::FractionFieldsStoredAsDoubles);
        }
        bool isRowReadForEitherKind = tested && tested->storage == ArrayStorage::Double && tested->admitsInt32Storage && (isReadOffListInTypedElements(walker.currentBodyIndex(), access.value) || isNamedList(walker.currentBodyIndex(), access.value));
        if (tested && tested->storage == ArrayStorage::Other && isNamedList(walker.currentBodyIndex(), access.value))
            tested = std::nullopt;
        bool mayGoByKind = (!tested || (isRowReadForEitherKind && !mayStoreAgainInIteration(access) && (types.isSubset(access.typeOfStored, TypeTable::int32) || canGuardAccess(access)))) && (asked == ArrayStorage::Double || types.isSubset(access.typeOfStored, TypeTable::int32)) && walker.isInvariantInSpecializedVersion(access.base) && (isRowReadForEitherKind || access.storesNumberFromBefore || !textOfLoopsReadsListByKeyInTypedElements(walker.currentBodyIndex(), access.value));
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
        noteKindOfStorageThatTurnFound(walker, access.base, tested->storage, true, tested->admitsInt32Storage, true);
        walker.count(StatisticID::ArrayChecksSharedByIterations);
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
    bool turnAsksForBounds = rowAsksForWritableList || (!isPush && !access.isOnlyAskedAbout && (walker.depthOfCopies() || access.typeOfKey.integers.isSubsetOf(Range::int32())) && canGuardAccess(access));
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
    if (asksForKind && mayAppend && !isInside && !isPush && !appendsToLiteral && !access.isOnlyAskedAbout && access.base.isValue() && access.key.isValue() && isReadOfBody(access) && canGuardAccess(access)
        && (storesFromInitialLength(walker, walker.currentBodyIndex(), access.value) || storesFromRowLength(walker, walker.currentBodyIndex(), access.value, false))) {
        Origin origin = walker.originOf(access.value);
        origin.isTailCall = false;
        Reference length = buildArrayLength(walker, access.base, origin);
        Reference isAtEnd = length ? buildIntegerOperator(walker, Opcode::StrictEq, access.key, length, origin) : Reference();
        if (!isAtEnd || !continueOrRestartIteration(walker, isAtEnd, true))
            return true;
        AccessOfTypedElements atEnd = access;
        atEnd.key = length;
        atEnd.typeOfKey = types.at(walker.typeOf(length));
        atEnd.isPushAtLength = true;
        return buildStoreBehindRowOfTypedElements(atEnd);
    }
    bool mustUndoExtension = appendsToLiteral && turnStoresAgainBehindInTypedElements(access);
    bool canUndoExtension = !goesByKind && isNumericStorage(tested->storage) && !access.isOnlyAskedAbout && !walker.unrolledLoopHeader() && walker.canRecordUndoableStore();
    bool canExtend = !mustUndoExtension || canUndoExtension;
    mayAppend &= !isInside && (!asksForKind || keyIsLengthOfListItselfInTypedElements(access)) && (!appendsToLiteral || !canExtend) && !walker.shouldSkipLengthChecks();
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
    bool extendsLength = appendsToLiteral && canExtend && walker.canRestartIteration() && notedCounterPosition(access, KnownNote::LengthEqualsInitialCounter, tested->name);
    if (isPush && !extendsLength && !mayAppend)
        return false;
    if (!isInside && !extendsLength && !walker.shouldSkipLengthChecks()) {
        Draft length(Opcode::GetArrayLength, origin);
        walker.chooseConservativeFields(length);
        length.setArrayOperand(tested->name);
        if (mayAppend)
            readLengthThroughStorageInFrontOfLoop(walker, length, *tested, origin);
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
            if (below && fromZeroUp) {
                inside = walker.boundsProofFromChecks(access.base, access.key, currentLength, *fromZeroUp, *below);
                noteLengthAbovePosition(walker, currentLength, isKeyWithinLength, origin);
            }
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
    uint32_t undoableStoreBefore = walker.state().newestUndoableStore();
    bool extendsBehindKind = extendsLength && asksForKind;
    if (extendsLength && !extendsBehindKind && !buildLengthExtension(access, *tested, origin, capacityProof, inside))
        return true;
    Reference valueBeforeStore;
    Reference wordBeforeStores;
    bool isRecorded = !mayAppend && !isPush && !extendsLength && opcode == Opcode::PutDoubleIndex && walker.canRecordUndoableStore() && mayRestartAfterStore(access) && walker.isElementStoreRecorded(access.base, access.key);
    bool appendIsUndoable = mayAppend && !goesByKind && isNumericStorage(tested->storage) && walker.canRecordUndoableStore() && (turnStoresAgainBehindInTypedElements(access) || readsNamesAfterStore(access));
    if (mayAppend) {
    } else if (isRecorded)
        walker.count(StatisticID::RepeatedElementStoresRecordedOnce);
    else if (opcode == Opcode::PutDoubleIndex && !goesByKind && walker.canRecordUndoableStore() && mayRestartAfterStore(access)) {
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
    } else if (opcode == Opcode::PutContiguousIndex && !isPush && !extendsLength && !storesByAnyKind && tested->storage == (goesByKind ? ArrayStorage::Double : ArrayStorage::Int32) && walker.canRecordUndoableStore() && mayRestartAfterStore(access)) {
        valueBeforeStore = goesByKind ? Reference() : walker.state().element(access.base, access.key);
        if (!valueBeforeStore || !types.isSubset(walker.typeOf(valueBeforeStore), TypeTable::int32)) {
            bool isNumberOnSomePaths = !goesByKind && walker.isOnSomePathsOfIteration() && findKnown(walker, KnownNote::PositionFoundInsideList, access.base, access.key);
            if (isNumberOnSomePaths) {
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
            } else
                walker.count(StatisticID::UndoableStoresOfElementWords);
            Draft read(isNumberOnSomePaths ? Opcode::GetOwnInt32Index : Opcode::GetElementWord, origin);
            walker.chooseConservativeFields(read);
            read.setArrayOperand(access.base);
            read.setPositionOperand(access.key);
            if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
                read.setStorageOperand(pointer);
            valueBeforeStore = walker.build(WTF::move(read));
            if (!valueBeforeStore)
                return true;
        }
    } else if (storesNumberByKind && !isPush && !extendsLength && walker.canRecordUndoableStore() && mayRestartAfterStore(access)) {
        Draft read(Opcode::GetElementWord, origin);
        walker.chooseConservativeFields(read);
        read.setArrayOperand(access.base);
        read.setPositionOperand(access.key);
        read.setIsOfAnyStorage(tested->storage == ArrayStorage::Other);
        if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
            read.setStorageOperand(pointer);
        wordBeforeStores = walker.build(WTF::move(read));
        if (!wordBeforeStores)
            return true;
        walker.count(StatisticID::UndoableStoresByStorage);
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
        if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
            extend.setStorageOperand(pointer);
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
        if (isReadOfBody(access))
            walker.carryStoredElement(access.value, access.base, stored);
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
            if (isRecorded && !walker.isUnreachable())
                walker.restoreUndoableStores(undoableStoreBefore);
            if (wordBeforeStores && !walker.isUnreachable())
                walker.recordUndoableWordStore(access.base, access.key, wordBeforeStores, tested->storage, undoableStoreBefore);
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
    if (isRecorded && !storesNumberByKind)
        walker.reuseNewestUndoableStore();
    else if (valueBeforeStore && walker.canRecordUndoableStore())
        walker.recordUndoableElementStore(access.base, access.key, valueBeforeStore, tested->storage);
    if (Reference pointer = storageOfListFromInFrontOfLoop(walker, *tested, origin))
        put.setStorageOperand(pointer);
    if (isMovedOutOfDoubleStorage)
        walker.count(StatisticID::DoubleElementsCopiedUnchecked);
    std::optional<int64_t> storedConstant = inside && !isPush && !behindKinds && !freshObjectNamedInTypedElements(walker, access.base) ? findConstantPosition(walker, access.key) : std::nullopt;
    std::optional<uint32_t> loopKeepingElements = storedConstant ? walker.loopKeepingElements(access.base) : std::nullopt;
    Reference store;
    {
        SetForScope keepsElements(walker.arrayKeepingElements(), storedConstant ? access.base : Reference());
        store = walker.build(WTF::move(put));
    }
    if (storedConstant && !walker.isUnreachable()) {
        walker.state().endElementAt(access.base, *storedConstant);
        if (loopKeepingElements)
            walker.keepCarriedElements(*loopKeepingElements, access.base, *storedConstant);
    }
    walker.takePendingUndoableStore();
    if (extendsLength && mustUndoExtension && !walker.isUnreachable())
        walker.recordUndoableAppend(access.base, access.key, placeholderForAppend(walker, tested->storage, origin), access.key, tested->storage, undoableStoreBefore);
    if (behindKinds) {
        if (!walker.isUnreachable())
            walker.jumpTo(behindKinds, heldWithin ? singleElementSpan(heldWithin) : std::span<const Reference>());
        walker.continueIn(behindKinds);
        store = { };
        if (heldWithin && !walker.isUnreachable())
            walker.recordUndoableAppend(access.base, access.key, Reference::value(walker.code().at(behindKinds).parameters()[0]), lengthOfArray, tested->storage, undoableStoreBefore);
        if (isRecorded && storesNumberByKind && !walker.isUnreachable())
            walker.restoreUndoableStores(undoableStoreBefore);
        if (wordBeforeStores && !walker.isUnreachable())
            walker.recordUndoableWordStore(access.base, access.key, wordBeforeStores, tested->storage, undoableStoreBefore);
    }
    noteStoredElementInTypedElements(walker, access.base, access.key, stored);
    if (isReadOfBody(access))
        walker.carryStoredElement(access.value, access.base, stored);
    if (ValueID read = isReadOfBody(access) && !isPush ? findUpdatedRead(walker.currentBodyIndex(), access.value) : ValueID())
        walker.carryUpdate(read, access.value.id(), access.base, access.key, stored);
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

struct ViewHolder {
    VariableID variable;
    ValueID environmentOrCapture;
    LayoutID layout;
    uint32_t field { 0 };

    explicit operator bool() const { return variable || environmentOrCapture; }
    bool operator==(const ViewHolder&) const = default;
};

static ViewHolder findViewHolder(const Walker& walker, const BodyIndex& index, ValueID object)
{
    ValueID read = index.stripCasts(object);
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.stripCasts(index.value(read).operand(0));
    if (!read)
        return { };
    if (index.isParameter(read)) {
        std::optional<uint32_t> parameter = index.blockOf(read) == entryBlock ? index.block(entryBlock).indexOfParameter(read) : std::nullopt;
        bool isCapture = parameter && *parameter < walker.program().at(walker.currentFunction()).captures.size();
        return isCapture ? ViewHolder { VariableID(), read, LayoutID(), *parameter } : ViewHolder();
    }
    if (index.opcodeOf(read) == Opcode::GetModuleVariable)
        return { index.value(read).variable(), ValueID(), LayoutID(), 0 };
    if (index.opcodeOf(read) != Opcode::GetEnvironmentField)
        return { };
    auto captured = index.value(read).as<Opcode::GetEnvironmentField>();
    return { VariableID(), index.stripCasts(captured.environmentOperand()), captured.layout(), captured.field() };
}

static bool isStoreIntoHolder(const BodyValue& store, const ViewHolder& holder)
{
    if (store.opcode() == Opcode::PutModuleVariable)
        return holder.variable && store.variable() == holder.variable;
    if (store.opcode() != Opcode::PutEnvironmentField || !holder.layout)
        return false;
    auto captured = store.as<Opcode::PutEnvironmentField>();
    return captured.layout() == holder.layout && captured.field() == holder.field;
}

static bool accessesViewOfVariableAgain(const Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
    static constexpr OpcodeSet calls = opcodesOfFamily(OperationFamily::Call) | OpcodeSet { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::CallDirect, Opcode::Await, Opcode::Yield };
    static constexpr uint32_t farthestAccess = 64;
    if (access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words())
        return false;
    ViewHolder holder = findViewHolder(walker, index, access.objectOperand());
    if (!holder)
        return false;
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t end = std::min(block.firstValue().index() + block.numberOfValues(), access.id().index() + farthestAccess);
    for (uint32_t number = access.id().index() + 1; number < end; ++number) {
        Opcode opcode = index.opcodeOf(ValueID(number));
        if (calls.contains(opcode) || isStoreIntoHolder(index.value(ValueID(number)), holder))
            return false;
        if (accessesByKey.contains(opcode) && findViewHolder(walker, index, index.value(ValueID(number)).objectOperand()) == holder)
            return true;
    }
    return false;
}

static VariableID findMirroringVariable(Walker& walker, Reference view)
{
    if (!view || !view.isValue())
        return { };
    const Code& code = walker.code();
    ValueID read = underlyingCellOf(code, view.id());
    if (code.isConstant(read) || code.at(read).opcode != Opcode::GetModuleVariable || code.at(read).returnsRawSlot())
        return { };
    VariableID variable = code.at(read).variable();
    if (!walker.plan().variable(variable).hasViewMirror || walker.plan().deferredDefinitions().findDeferred(variable))
        return { };
    Reference held = walker.state().variable(variable);
    return held && held.isValue() && underlyingCellOf(code, held.id()) == read ? variable : VariableID();
}

struct CheckedView {
    VariableID variable;
    Reference cell;

    explicit operator bool() const { return variable || cell; }
};

static CheckedView findCheckedView(Walker& walker, Reference view)
{
    if (VariableID variable = findMirroringVariable(walker, view))
        return { variable, Reference() };
    if (!view || !view.isValue())
        return { };
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, view.id());
    return { VariableID(), code.isConstant(cell) ? Reference() : Reference::value(cell) };
}

static Reference cappedLengthOfView(Walker& walker, Reference view, TypedArrayType typedArrayType, Origin origin)
{
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::CappedLengthOfView, view))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    Draft length(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(view);
    length.setTypedArrayType(typedArrayType);
    length.setIsCapped(true);
    Reference built = walker.build(WTF::move(length));
    if (!built || !built.isValue() || walker.isUnreachable())
        return { };
    noteKnown(walker, KnownNote::CappedLengthOfView, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
    return built;
}

static Reference findMirroredOfBuffer(const Walker& walker, KnownNote note, Reference view, VariableID first)
{
    std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, note, { }, { }, first.index());
    if (!noted)
        return { };
    Reference found = Reference::fromBits(static_cast<uint32_t>(*noted));
    Reference held = Reference::value(underlyingCellOf(walker.code(), view.id()));
    std::optional<uint32_t> innermost = walker.innermostOpenLoop();
    for (uint32_t loop = 0; innermost && loop <= *innermost; ++loop) {
        if (walker.standsInFrontOfOpenLoop(loop, found) && !walker.standsInFrontOfOpenLoop(loop, held))
            return { };
    }
    return found;
}

static void noteMirroredOfBuffer(Walker& walker, KnownNote note, VariableID first, const PlannedVariable& planned, Reference built, uint32_t numberOfValuesBefore)
{
    if (built.id().index() < numberOfValuesBefore)
        return;
    LocationMasks places(AbstractHeapSet { AbstractHeap::TypedArrayStorage });
    places.add(AbstractHeap::ModuleVariables, planned.locationsOfViewsOfBuffer);
    noteKnown(walker, note, { }, { }, first.index(), places, built.bits());
}

static Reference mirroredLengthOfView(Walker& walker, Reference view, VariableID variable, TypedArrayType typedArrayType, Origin origin)
{
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::MirroredLengthOfView, view))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    Draft length(Opcode::GetMirroredLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(Reference::value(underlyingCellOf(walker.code(), view.id())));
    length.setVariable(variable);
    length.setTypedArrayType(typedArrayType);
    Reference built = walker.build(WTF::move(length));
    if (!built || !built.isValue() || walker.isUnreachable())
        return { };
    noteKnown(walker, KnownNote::MirroredLengthOfView, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
    return built;
}

static Reference mirroredLengthOfBuffer(Walker& walker, Reference view, VariableID variable, TypedArrayType typedArrayType, Origin origin)
{
    const PlannedVariable& planned = walker.plan().variable(variable);
    VariableID first = planned.firstViewOfElementSize;
    if (!first)
        return mirroredLengthOfView(walker, view, variable, typedArrayType, origin);
    if (Reference found = findMirroredOfBuffer(walker, KnownNote::MirroredLengthOfBuffer, view, first))
        return found;
    Draft length(Opcode::GetMirroredLength, origin);
    walker.chooseConservativeFields(length);
    static constexpr std::array typesByLogElementSize { TypeInt8, TypeInt16, TypeInt32, TypeFloat64 };
    length.setViewOperand(walker.constant(Draft(Opcode::Undefined, origin)));
    length.setVariable(first);
    length.setTypedArrayType(typesByLogElementSize[logElementSize(typedArrayType)]);
    length.setIsOfBuffer(true);
    uint32_t numberOfValuesBefore = walker.code().numberOfValues();
    Reference built = walker.build(WTF::move(length));
    if (!built || !built.isValue() || walker.isUnreachable())
        return { };
    noteMirroredOfBuffer(walker, KnownNote::MirroredLengthOfBuffer, first, planned, built, numberOfValuesBefore);
    return built;
}

static Reference mirroredVectorOfView(Walker& walker, Reference view, VariableID variable, bool isBelowLengthOfBuffer, Origin origin)
{
    const PlannedVariable& planned = walker.plan().variable(variable);
    VariableID first = isBelowLengthOfBuffer ? planned.firstViewOfBuffer : VariableID();
    Reference found;
    if (first)
        found = findMirroredOfBuffer(walker, KnownNote::MirroredVectorOfBuffer, view, first);
    else if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::MirroredVectorOfView, view))
        found = Reference::fromBits(static_cast<uint32_t>(*noted));
    if (found)
        return found;
    Draft vector(Opcode::GetMirroredVector, origin);
    walker.chooseConservativeFields(vector);
    vector.setViewOperand(Reference::value(underlyingCellOf(walker.code(), view.id())));
    vector.setVariable(first ? first : variable);
    uint32_t numberOfValuesBefore = walker.code().numberOfValues();
    Reference built = walker.build(WTF::move(vector));
    if (!built || !built.isValue() || walker.isUnreachable())
        return { };
    if (first)
        noteMirroredOfBuffer(walker, KnownNote::MirroredVectorOfBuffer, first, planned, built, numberOfValuesBefore);
    else
        noteKnown(walker, KnownNote::MirroredVectorOfView, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
    return built;
}

static bool hasCappedLength(const Walker& walker, Reference view)
{
    return !walker.state().answers().isEmpty() && findKnown(walker, KnownNote::CappedLengthOfView, view);
}

static Reference lengthForOneAccess(Walker& walker, Reference view, const CheckedView& checked, TypedArrayType typedArrayType, Origin origin)
{
    if (checked.variable)
        return mirroredLengthOfBuffer(walker, view, checked.variable, typedArrayType, origin);
    bool loopReadsItOnce = walker.isInLoop() && walker.loopPreservesViews();
    if (hasCappedLength(walker, view) || (loopReadsItOnce && !logElementSize(typedArrayType)))
        return cappedLengthOfView(walker, view, typedArrayType, origin);
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::LengthOfList, view))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    if (loopReadsItOnce)
        return cappedLengthOfView(walker, view, typedArrayType, origin);
    Draft length(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(view);
    length.setTypedArrayType(typedArrayType);
    Reference built = walker.build(WTF::move(length));
    if (!built || !built.isValue() || walker.isUnreachable())
        return { };
    noteKnown(walker, KnownNote::LengthOfList, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
    return built;
}

static void hoistViewMemoryTest(Walker& walker, Reference view, Origin origin)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    if (!walker.isInLoop() || !walker.canRestartIteration() || !view.isValue() || !viewMayFollowSharedMemoryThatGrows(walker, view) || !walker.isInvariantInSpecializedVersion(view, purity) || !walker.iterationsPreserveTypedArrays(purity) || walker.leavesOutQuestionThatEveryTurnWouldAsk(Walker::QuestionIsAsked::Anywhere))
        return;
    const BuiltinTable& builtins = walker.program().builtins();
    Type type = walker.types().at(walker.typeOf(view));
    const ClassPart* part = type.isObject() ? singleBuiltinClassOf(walker.types(), type) : nullptr;
    if (!part || !isTypedView(builtins.typedArrayTypeOf(part->builtinClass)) || pristineTestOf(builtins, part->builtinClass) != PristineTest::MayPass)
        return;
    origin.isTailCall = false;
    Reference test;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft draft(Opcode::IsPristine, origin);
        walker.chooseConservativeFields(draft);
        draft.setValueOperand(view);
        draft.setBuiltinClass(part->builtinClass);
        draft.setAspects({ PristineAspects::Storage });
        test = walker.build(WTF::move(draft));
    }
    if (!test || !test.isValue() || walker.hasFailed() || walker.code().isConstant(test.id()) || !walker.tryBranchInPreheader(test, true, purity))
        return;
    walker.recordSpeculation();
    walker.count(StatisticID::ViewMemoryTestsHoisted);
    noteThatViewKeepsItsLength(walker, view, test);
}

static Reference lengthOfViewInTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
{
    Walker& walker = access.walker;
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::LengthOfList, access.base))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    if (std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::RoomOfViewIsAsInFrontOfLoop, access.base))
        return Reference::fromBits(static_cast<uint32_t>(*noted));
    bool isAccessedAgain = walker.opensGuesses() && accessesViewOfVariableAgain(walker, access.value);
    if ((!walker.isInLoop() && !isAccessedAgain) || !walker.opensGuesses())
        return { };
    hoistViewMemoryTest(walker, access.base, origin);
    if (walker.hasFailed() || walker.isUnreachable())
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

static bool isBelowMirroredLength(const Walker& walker, const Draft& access)
{
    const GroundsOfPosition* grounds = access.testsPassed && access.testsPassed->groundsOfPosition() ? &*access.testsPassed->groundsOfPosition() : nullptr;
    return grounds && grounds->length && !walker.code().isConstant(grounds->length) && walker.code().at(grounds->length).opcode == Opcode::GetMirroredLength;
}

static bool isBelowLengthOfBuffer(const Walker& walker, const Draft& access)
{
    return isBelowMirroredLength(walker, access) && walker.code().at(access.testsPassed->groundsOfPosition()->length).isOfBuffer();
}

static void nameStorageOfViewInTypedElements(const AccessOfTypedElements& written, Draft& access, TypedArrayType typedArrayType)
{
    Walker& walker = written.walker;
    Reference view = written.base;
    if (access.level() != KnowledgeLevel::Proven || !readsElementsInLineInTypedElements(typedArrayType))
        return;
    VariableID mirroring = isTypedView(typedArrayType) ? findMirroringVariable(walker, view) : VariableID();
    bool isLengthKnown = !walker.state().answers().isEmpty() && (findKnown(walker, KnownNote::LengthOfList, view) || findKnown(walker, KnownNote::RoomOfViewIsAsInFrontOfLoop, view));
    if (mirroring && (access.isInsideView() ? isBelowMirroredLength(walker, access) : !isLengthKnown)) {
        Reference length = access.isInsideView() ? Reference() : mirroredLengthOfView(walker, view, mirroring, typedArrayType, access.origin());
        if (!access.isInsideView() && !length)
            return;
        Reference built = mirroredVectorOfView(walker, view, mirroring, access.isInsideView() && isBelowLengthOfBuffer(walker, access), access.origin());
        if (!built)
            return;
        access.setVectorOperand(built);
        if (length)
            access.setLengthOperands(singleElementSpan(length));
        access.setIsMirroredView(true);
        return;
    }
    Reference length = access.isInsideView() ? Reference() : lengthOfViewInTypedElements(written, typedArrayType, access.origin());
    if (!access.isInsideView() && (!length || walker.isUnreachable()))
        return;
    bool sharesVector = (walker.comparesElementsOfViews() && walker.isInLoop()) || walker.isOnPathOfCapturedViews();
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

static bool isIntegerViewType(const Walker& walker, TypeID proven)
{
    if (!proven)
        return false;
    Type type = walker.program().at(proven);
    type.primitives &= ~Primitive::Absent;
    const ClassPart* onlyClass = type.primitives ? nullptr : singleBuiltinClassOf(TypeReader(walker.program()), type);
    TypedArrayType kind = onlyClass ? walker.program().builtins().typedArrayTypeOf(onlyClass->builtinClass) : NotTypedArray;
    return isTypedView(kind) && hasInlineElementAccess(kind) && isInt(kind) && !isBigInt(kind);
}

enum class HeldViews : bool { MirroredAlone, All };

static bool accessesCheckedView(const Walker& walker, const BodyIndex& index, const BodyValue& operation, HeldViews views)
{
    if (operation.opcode() != Opcode::ElemGet && operation.opcode() != Opcode::ElemSet)
        return false;
    ValueID read = index.stripCasts(operation.objectOperand());
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable)
        return walker.plan().variable(index.value(read).variable()).hasViewMirror;
    ViewHolder holder = read && views == HeldViews::All ? findViewHolder(walker, index, read) : ViewHolder();
    if (!holder.environmentOrCapture)
        return false;
    const LinkTables& linked = walker.facts().linked();
    return isIntegerViewType(walker, holder.layout ? linked.provenOfField({ holder.layout, holder.field }) : linked.capture(walker.currentFunction(), holder.field).proven);
}

static bool mayBeOnInBoundsPath(const Walker& walker, const BodyIndex& index, const BodyValue& operation, HeldViews views)
{
    static constexpr OpcodeSet plainOperations = constantsInFormOfUses | inputIdentityOpcodes | allNumericOperators | allRelations | allEqualities
        | OpcodeSet { Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::RequireObjectCoercible, Opcode::CheckInitialized, Opcode::ToBoolean, Opcode::LogicalNot };
    if (plainOperations.contains(operation.opcode()) || accessesCheckedView(walker, index, operation, views) || (views == HeldViews::All && operation.opcode() == Opcode::GetEnvironmentField))
        return true;
    if (operation.opcode() != Opcode::Call)
        return false;
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id());
    ValueID read = callee ? ValueID() : index.stripCasts(operation.calleeOperand());
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable) {
        VariableID variable = index.value(read).variable();
        TypeID held = walker.facts().beforeEvaluation().typeOf(variable);
        if (!held && variable.index() < walker.facts().linked().variables.size())
            held = walker.facts().linked().variable(variable).proven;
        if (Type type = walker.types().at(held ? held : TypeID(TypeTable::anything)); held && type.isObject() && type.isJavaScriptValue())
            callee = singleFunctionOf(walker.types(), type);
    }
    return callee && isSmallLeaf(walker, callee, operation.argumentsOperands().size());
}

static bool isKeyedByCounter(const BodyIndex& index, const BodyValue& access)
{
    ValueID key = index.stripCasts(access.propertyOperand());
    while (key && !index.isParameter(key) && (index.opcodeOf(key) == Opcode::Add || index.opcodeOf(key) == Opcode::Sub || index.opcodeOf(key) == Opcode::Mul || index.opcodeOf(key) == Opcode::LShift))
        key = index.stripCasts(index.value(key).operand(0));
    if (!key || !index.isParameter(key) || !index.isLoopHead(index.blockOf(key)))
        return false;
    for (uint32_t number = key.index() + 1; number < index.numberOfValues(); ++number) {
        ValueID step(number);
        if (index.isParameter(step))
            continue;
        Opcode opcode = index.opcodeOf(step);
        bool stepsByOne = opcode == Opcode::Increment || opcode == Opcode::Decrement;
        bool adds = opcode == Opcode::Add || opcode == Opcode::Sub;
        if ((!stepsByOne && !adds) || index.stripCasts(index.value(step).operand(0)) != key)
            continue;
        std::optional<BodyUser> user = index.onlyUser(step);
        if (stepsByOne || (user && user->kind == BodyUseKind::EdgeArgument))
            return true;
    }
    return false;
}

struct KeyOffset {
    ValueID root;
    uint32_t shift { 0 };
    int64_t offset { 0 };
};

static std::optional<int64_t> findIntegerLiteral(const BodyIndex& index, ValueID value)
{
    value = index.stripCasts(value);
    if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::Integer)
        return std::nullopt;
    return index.value(value).integer();
}

static std::optional<KeyOffset> findKeyOffset(const BodyIndex& index, ValueID position, HeldViews views)
{
    static constexpr int64_t greatestOffset = 1 << 16;
    KeyOffset found { index.stripCasts(position) };
    if (found.root && !index.isParameter(found.root) && index.opcodeOf(found.root) == Opcode::RShift) {
        std::optional<int64_t> shift = findIntegerLiteral(index, index.value(found.root).operand(1));
        if (!shift || *shift < 1 || *shift > 3)
            return std::nullopt;
        found.shift = static_cast<uint32_t>(*shift);
        found.root = index.stripCasts(index.value(found.root).operand(0));
    }
    for (ValueID sumOrRoot = found.root; sumOrRoot && !index.isParameter(sumOrRoot);) {
        BodyValue sum = index.value(sumOrRoot);
        if (views == HeldViews::All && sum.opcode() == Opcode::BitOr && findIntegerLiteral(index, sum.operand(1)) == 0) {
            sumOrRoot = index.stripCasts(sum.operand(0));
            continue;
        }
        if (sum.opcode() != Opcode::Add && sum.opcode() != Opcode::Sub)
            break;
        std::optional<int64_t> added = findIntegerLiteral(index, sum.operand(1));
        if (!added || *added < -greatestOffset || *added > greatestOffset)
            break;
        found.offset += sum.opcode() == Opcode::Add ? *added : -*added;
        found.root = sumOrRoot = index.stripCasts(sum.operand(0));
    }
    if (!found.root || found.offset < -greatestOffset || found.offset > greatestOffset || found.offset % (1 << found.shift))
        return std::nullopt;
    found.offset /= 1 << found.shift;
    return found;
}

static unsigned countNeighborAccesses(const Walker& walker, const BodyIndex& index, const BodyValue& access)
{
    std::optional<KeyOffset> position = findKeyOffset(index, access.propertyOperand(), HeldViews::All);
    if (!position)
        return 0;
    ViewHolder holder = findViewHolder(walker, index, access.objectOperand());
    BodyBlock block = index.block(index.blockOf(access.id()));
    unsigned numberOfAccesses = 0;
    for (uint32_t number = access.id().index() + 1; number < block.endOfValues().index(); ++number) {
        BodyValue operation = index.value(ValueID(number));
        if (!mayBeOnInBoundsPath(walker, index, operation, HeldViews::All))
            break;
        if (!accessesCheckedView(walker, index, operation, HeldViews::All) || findViewHolder(walker, index, operation.objectOperand()) != holder)
            continue;
        std::optional<KeyOffset> other = findKeyOffset(index, operation.propertyOperand(), HeldViews::All);
        numberOfAccesses += other && other->root == position->root && other->shift == position->shift && other->offset != position->offset;
    }
    return numberOfAccesses;
}

bool prefersInBoundsPath(Walker& walker, const BodyValue& access)
{
    static constexpr unsigned fewestNeighbors = 3;
    const BodyIndex& index = walker.currentBodyIndex();
    if (access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words())
        return false;
    if (!accessesCheckedView(walker, index, access, HeldViews::All) || findViewHolder(walker, index, access.objectOperand()).variable)
        return false;
    return walker.isOnPathOfCapturedViews() || countNeighborAccesses(walker, index, access) >= fewestNeighbors;
}

static bool endsWithIntegerTest(const BodyIndex& index, const BodyBlock& block, uint32_t limit, uint32_t& end)
{
    static constexpr OpcodeSet tests = allRelations | allEqualities | constantsInFormOfUses | inputIdentityOpcodes | OpcodeSet { Opcode::URShift, Opcode::ToBoolean, Opcode::LogicalNot };
    if (block.terminator().kind() != TerminatorKind::Branch)
        return false;
    uint32_t first = block.firstValue().index();
    uint32_t place = block.numberOfValues();
    bool hasRelation = false;
    for (; place > end && tests.contains(index.opcodeOf(ValueID(first + place - 1))); --place) {
        BodyValue operation = index.value(ValueID(first + place - 1));
        hasRelation |= allRelations.contains(operation.opcode()) || allEqualities.contains(operation.opcode());
        bool readsElement = false;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            ValueID read = operand ? index.stripCasts(operand) : ValueID();
            readsElement |= read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::ElemGet;
        });
        if (readsElement)
            return false;
    }
    if (hasRelation)
        end = std::min(place, limit);
    return hasRelation;
}

static bool isCalculatedFromFractions(Walker& walker, const BodyIndex& index, ValueID value, unsigned depth = 0)
{
    static constexpr unsigned maxDepth = 4;
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Negate };
    if (!value || index.isParameter(value) || depth == maxDepth || !arithmetic.contains(index.opcodeOf(value)))
        return false;
    bool found = false;
    index.value(value).forEachOperand([&](ValueID operand, uint32_t) {
        operand = index.stripCasts(operand);
        if (found || !operand || index.isParameter(operand))
            return;
        if (accessesCheckedView(walker, index, index.value(operand), HeldViews::MirroredAlone)) {
            VariableID variable = index.value(index.stripCasts(index.value(operand).objectOperand())).variable();
            TypeID held = walker.facts().beforeEvaluation().typeOf(variable);
            if (!held && variable.index() < walker.facts().linked().variables.size())
                held = walker.facts().linked().variable(variable).proven;
            BaseOfTypedElements base = classifyBaseOfTypedElements(walker, held ? walker.types().withoutNullish(held) : TypeID(TypeTable::anything));
            found = base.kind == KindOfBaseOfTypedElements::TypedArray && isFloat(base.typedArrayType);
        } else
            found = isCalculatedFromFractions(walker, index, operand, depth + 1);
    });
    return found;
}

bool tryOpenInBoundsPath(Walker& walker, const BodyValue& access)
{
    if (walker.inBoundsPath() && walker.inBoundsPath()->frameIndex == walker.currentFrameIndex())
        return false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || walker.standsInClosedArm() || walker.isNestedTooDeeply() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!accessesCheckedView(walker, index, access, HeldViews::All) || walker.code().at(walker.currentBlock()).frequency != BlockFrequency::Normal)
        return false;
    bool isMirrored = !!findViewHolder(walker, index, access.objectOperand()).variable;
    if (walker.canRestartIteration() && (isMirrored || !prefersInBoundsPath(walker, access)))
        return false;
    HeldViews views = isMirrored ? HeldViews::MirroredAlone : HeldViews::All;
    if (walker.isInLoop() && walker.loopPreservesViews() && isKeyedByCounter(index, access))
        return false;
    BodyBlock block = index.block(index.blockOf(access.id()));
    if (walker.inputBlockBeingWalked() != block.id() || block.hasHandler())
        return false;
    uint32_t first = block.firstValue().index();
    uint32_t begin = access.id().index() - first;
    uint32_t limit = limitOfStretchOfArms(walker, block);
    uint32_t end = begin;
    unsigned numberOfAccesses = 0;
    uint32_t place = begin;
    for (; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(first + place));
        if (!mayBeOnInBoundsPath(walker, index, operation, views))
            break;
        if (accessesCheckedView(walker, index, operation, views)) {
            ++numberOfAccesses;
            end = place + 1;
        }
    }
    static constexpr unsigned fewestAccessesOutsideLoops = 2;
    if (numberOfAccesses < (walker.isInLoop() ? 1 : fewestAccessesOutsideLoops))
        return false;
    if (!isMirrored && walker.isInLoop())
        walker.noteInBoundsPath();
    bool mayTakeBranch = place == limit && limit == block.numberOfValues() && walker.isInLoop() && walker.armsMayTakeBranchOf(block.id(), limit, Reference()) && (isMirrored || walker.canSplitBranch(block.id(), limit));
    if (place == limit && (isMirrored || mayTakeBranch || !endsWithIntegerTest(index, block, limit, end)))
        end = limit;

    bool takesBranch = mayTakeBranch && end == block.numberOfValues();
    BodyTerminator terminator = block.terminator();
    if (ValueID condition = isMirrored && end == block.numberOfValues() && terminator.isConditionalBranch() ? terminator.operand(0) : ValueID(); condition && condition.index() == first + end - 1 && allRelations.contains(index.opcodeOf(condition))) {
        auto isNumber = [&](ValueID operand) {
            operand = index.stripCasts(operand);
            return index.isParameter(operand) || operand.index() < first || index.opcodeOf(operand) == Opcode::Integer || index.opcodeOf(operand) == Opcode::Double || isCalculatedFromFractions(walker, index, operand);
        };
        BodyValue relation = index.value(condition);
        bool comparesFractions = isCalculatedFromFractions(walker, index, index.stripCasts(relation.operand(0))) || isCalculatedFromFractions(walker, index, index.stripCasts(relation.operand(1)));
        if (comparesFractions && isNumber(relation.operand(0)) && isNumber(relation.operand(1)) && !(takesBranch && walker.canThreadBranchAfter(relation))) {
            --end;
            takesBranch = false;
        }
    }
    if (isMirrored && walker.isInLoop())
        walker.noteComparisonOfElementsOfViews();
    BlockID meeting = walker.addBlock();
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end);
    Walker::ArmsThatMeet arms;
    auto endPath = [&] {
        if (takesBranch)
            walker.threadBranchOf(block.id());
        walker.listArm(stretch, arms);
    };
    Walker::InBoundsPath path { walker.currentFrameIndex(), block.id(), begin, end, &stretch };
    path.isOfCapturedViews = !isMirrored;
    {
        SetForScope pathScope(walker.inBoundsPath(), &path);
        walker.walkStretch(block.id(), begin, end);
        if (!walker.hasFailed())
            endPath();
    }
    if (walker.hasFailed())
        return true;
    if (!path.exits.isEmpty()) {
        walker.count(StatisticID::InBoundsPathsOpened);
        walker.noteInBoundsPath();
        std::ranges::stable_sort(path.exits, { }, &Walker::InBoundsPath::Exit::begin);
        Walker::ClosedArm closedArm(walker);
        SetForScope boundsExitScope(walker.closedArmIsBoundsExit(), !isMirrored);
        SetForScope coldCopyScope(walker.coldCopyDepth(), walker.coldCopyDepth() + 1);
        Vector<std::pair<ValueID, Reference>, 8> savedBindings;
        auto bindOnSlowPath = [&](ValueID value, Reference reference) {
            if (value.index() < first + begin && std::ranges::none_of(savedBindings, [&](auto& bound) { return bound.first == value; }))
                savedBindings.append({ value, walker.referenceOf(value) });
            walker.bind(value, reference);
        };
        auto forgetOtherPaths = [&](uint32_t placeOfExit) {
            for (uint32_t place = begin; place < placeOfExit; ++place)
                walker.bind(ValueID(first + place), Reference());
            for (auto [value, bound] : savedBindings)
                walker.bind(value, bound);
        };
        BlockID chain;
        for (size_t number = 0; number < path.exits.size() && !walker.hasFailed();) {
            const Walker::InBoundsPath::Exit& exit = path.exits[number];
            Vector<Walker::JoinArm, 2> ways;
            auto listWay = [&] {
                Walker::JoinArm way { walker.currentBlock(), { } };
                for (auto [value, built] : exit.needed)
                    way.values.append(walker.referenceOf(value));
                ways.append(WTF::move(way));
            };
            if (chain) {
                walker.continueIn(chain);
                listWay();
            }
            for (; number < path.exits.size() && path.exits[number].begin == exit.begin; ++number) {
                walker.continueIn(path.exits[number].slowPath);
                if (walker.isUnreachable())
                    continue;
                forgetOtherPaths(exit.begin);
                for (auto [value, built] : path.exits[number].needed) {
                    if (built)
                        bindOnSlowPath(value, built);
                }
                for (auto [value, built] : path.exits[number].needed) {
                    if (built)
                        continue;
                    bindOnSlowPath(value, Reference());
                    if (index.opcodeOf(value) == Opcode::GetModuleVariable)
                        walker.state().noteVariable(index.value(value).variable(), Reference());
                    walker.repeatOperation(block.id(), value.index() - first);
                }
                if (walker.hasFailed() || walker.isUnreachable())
                    continue;
                listWay();
            }
            if (ways.isEmpty())
                continue;
            if (ways.size() > 1) {
                Vector<Reference, 8> met(exit.needed.size());
                if (!walker.joinArms(ways.span(), walker.addBlock(BlockFrequency::Rare), met.mutableSpan()))
                    break;
                forgetOtherPaths(exit.begin);
                for (size_t position = 0; position < met.size(); ++position)
                    walker.bind(exit.needed[position].first, met[position]);
            } else {
                walker.continueIn(ways[0].last);
                forgetOtherPaths(exit.begin);
                for (size_t position = 0; position < exit.needed.size(); ++position)
                    walker.bind(exit.needed[position].first, ways[0].values[position]);
            }
            walker.walkStretch(block.id(), exit.begin, number < path.exits.size() ? path.exits[number].begin : end);
            chain = walker.isUnreachable() ? BlockID() : walker.currentBlock();
        }
        if (chain && !walker.hasFailed()) {
            walker.continueIn(chain);
            endPath();
        }
        for (auto [value, bound] : savedBindings)
            walker.bind(value, bound);
    }
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

static bool isRepeatableOnSlowPath(Opcode opcode)
{
    return opcode == Opcode::GetModuleVariable || opcode == Opcode::CheckInitialized || allNumericOperators.contains(opcode);
}

static bool isUnchangedOnSlowPath(Walker& walker, const BodyValue& operation, Reference built, std::span<const std::pair<ValueID, Reference>> needed)
{
    const Code& code = walker.code();
    if (operation.opcode() == Opcode::GetModuleVariable) {
        Reference held = walker.state().variable(operation.variable());
        return held && held.isValue() && underlyingCellOf(code, held.id()) == underlyingCellOf(code, built.id());
    }
    if (operation.opcode() == Opcode::CheckInitialized)
        return std::ranges::any_of(needed, [&](const auto& other) { return other.first == operation.operand(0) && !other.second; });
    TypeArena& types = walker.types();
    bool isPure = types.at(walker.typeOf(built)).isNumber();
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        Reference value = operand ? walker.referenceOf(operand) : Reference();
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        isPure = isPure && type && types.isSubset(types.withoutPrimitives(type, Primitive::Undefined), TypeTable::number);
    });
    return isPure;
}

static BlockID findBoundsExit(Walker& walker, const BodyValue& access)
{
    Walker::InBoundsPath* path = walker.inBoundsPath();
    if (!path || path->frameIndex != walker.currentFrameIndex() || walker.standsInClosedArm() || walker.isUnreachable() || walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare)
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words() || index.blockOf(access.id()) != path->inputBlock)
        return { };
    uint32_t first = index.block(path->inputBlock).firstValue().index();
    uint32_t place = access.id().index() - first;
    if (place < path->begin || place >= path->end)
        return { };
    Walker::InBoundsPath::Exit exit { place, BlockID() };
    BitVector needed(path->end);
    auto need = [&](ValueID value) {
        if (value && value.index() >= first && value.index() < first + place)
            needed.quickSet(value.index() - first);
    };
    for (ValueID value : path->stretch->mentionedBehind)
        need(value);
    for (uint32_t later = path->end; later--;) {
        if (later >= place || (needed.quickGet(later) && (later >= path->begin || isRepeatableOnSlowPath(index.opcodeOf(ValueID(first + later))))))
            index.value(ValueID(first + later)).forEachOperand([&](ValueID operand, uint32_t) { need(operand); });
    }
    for (size_t earlier : needed) {
        ValueID value(static_cast<uint32_t>(first + earlier));
        Reference built = walker.referenceOf(value);
        if (!built || !built.isRecord())
            continue;
        bool isRepeated = isRepeatableOnSlowPath(index.opcodeOf(value));
        if (!isRepeated && earlier < path->begin)
            continue;
        exit.needed.append({ value, isRepeated && isUnchangedOnSlowPath(walker, index.value(value), built, exit.needed.span()) ? Reference() : built });
    }
    if (!path->exits.isEmpty() && path->exits.last().begin == place && path->exits.last().needed == exit.needed)
        return path->exits.last().slowPath;
    exit.slowPath = walker.addBlock(BlockFrequency::Rare);
    path->exits.append(WTF::move(exit));
    return path->exits.last().slowPath;
}

void checkInBoundsOperands(Walker& walker, const BodyValue& operation)
{
    TypeArena& types = walker.types();
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        Reference value = operand && !walker.isUnreachable() && !walker.hasFailed() ? walker.referenceOf(operand) : Reference();
        TypeID type = value && value.isValue() ? walker.typeOf(value) : TypeID();
        if (!type || walker.representationOf(value) != Representation::OptionalInt64 || !(types.at(type).primitives & Primitive::Undefined))
            return;
        TypeID defined = types.withoutPrimitives(type, Primitive::Undefined);
        BlockID exit = types.isSubset(defined, TypeTable::int32) && !isNeverType(defined) ? findBoundsExit(walker, operation) : BlockID();
        if (!exit)
            return;
        Origin origin = walker.originOf(operation);
        origin.isTailCall = false;
        Reference test = buildTestOfNumber(walker, value, defined, origin);
        if (!test || !test.isValue() || walker.isUnreachable())
            return;
        BlockID passes = walker.addBlock();
        walker.branch(test, passes, exit);
        walker.continueIn(passes);
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        Reference proven = buildProofOfNumber(walker, value, test, origin);
        while (Reference checked = walker.state().checkedNameOf(value))
            value = checked;
        if (proven && proven.isValue() && proven != value)
            walker.state().noteCheckedName(value, proven);
        walker.recordSpeculation();
    });
}

static Reference buildOffsetPosition(Walker& walker, Reference first, int64_t offset, Origin origin)
{
    if (!offset)
        return first;
    Draft written(Opcode::Integer, origin);
    written.setInteger(offset);
    return buildIntegerOperator(walker, Opcode::Add, first, walker.constant(WTF::move(written)), origin);
}

std::span<const Walker::OffsetsFromRoot> Walker::offsetsFromRootsOf(const BodyIndex& index)
{
    return m_offsetsFromRoots.ensure(&index, [&] {
        Vector<OffsetsFromRoot> found;
        for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
            if (index.isParameter(ValueID(number)))
                continue;
            BodyValue access = index.value(ValueID(number));
            if (!accessesCheckedView(*this, index, access, HeldViews::All))
                continue;
            ViewHolder holder = findViewHolder(*this, index, access.objectOperand());
            std::optional<KeyOffset> position = holder.variable ? std::nullopt : findKeyOffset(index, access.propertyOperand(), HeldViews::All);
            if (!position)
                continue;
            auto known = std::ranges::find_if(found, [&](const OffsetsFromRoot& other) {
                return other.root == position->root && other.shift == position->shift && other.environmentOrCapture == holder.environmentOrCapture && other.layout == holder.layout && other.field == holder.field;
            });
            if (known != found.end())
                known->offsets = known->offsets.unionWith(Range::one(position->offset));
            else
                found.append({ position->root, position->shift, holder.environmentOrCapture, holder.layout, holder.field, Range::one(position->offset) });
        }
        return found;
    }).iterator->value.span();
}

static Reference cappedLengthAfterOffset(Walker& walker, Reference view, TypedArrayType typedArrayType, uint32_t offset, Origin origin)
{
    Draft length(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(view);
    length.setTypedArrayType(typedArrayType);
    length.setOffset(offset);
    length.setIsCapped(true);
    Reference built = walker.build(WTF::move(length));
    return built && built.isValue() && !walker.isUnreachable() ? built : Reference();
}

static std::optional<PassedTest> checkRelation(Walker& walker, Opcode relation, Reference left, Reference right, BlockID exit, Origin origin)
{
    if (!left || !right || walker.isUnreachable())
        return std::nullopt;
    Draft test(relation, origin);
    walker.chooseConservativeFields(test);
    test.setLeftOperand(left);
    test.setRightOperand(right);
    test.setForm(operatorFormFor(walker, test));
    return walker.continueIfCheckPasses(walker.build(WTF::move(test)), exit);
}

static std::optional<PassedTest> checkBelowLength(Walker& walker, Reference position, Reference length, BlockID exit, Origin origin)
{
    if (!position || !length || walker.isUnreachable())
        return std::nullopt;
    bool remembersChecks = walker.isOnPathOfCapturedViews();
    if (std::optional<uint64_t> noted = !remembersChecks || walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::CheckedBelowLength, position, length)) {
        if (std::optional<PassedTest> passed = walker.findPassedTest(Reference::fromBits(static_cast<uint32_t>(*noted))))
            return passed;
    }
    Draft test(Opcode::IsBelowLength, origin);
    walker.chooseConservativeFields(test);
    test.setPositionOperand(position);
    test.setLengthOperand(length);
    Reference isBelow = walker.build(WTF::move(test));
    std::optional<PassedTest> passed = walker.continueIfCheckPasses(isBelow, exit);
    if (remembersChecks && passed && isBelow.isValue() && position.isValue())
        noteKnown(walker, KnownNote::CheckedBelowLength, position, length, 0, LocationMasks(AbstractHeapSet { }), isBelow.bits());
    return passed;
}

static Reference buildZero(Walker& walker, Origin origin)
{
    Draft zero(Opcode::Integer, origin);
    zero.setInteger(0);
    return walker.constant(WTF::move(zero));
}

static bool mirrorOneLength(const Walker& walker, VariableID one, VariableID other)
{
    if (one == other)
        return true;
    VariableID first = one && other ? walker.plan().variable(one).firstViewOfElementSize : VariableID();
    return first && first == walker.plan().variable(other).firstViewOfElementSize;
}

static bool mirrorOneBuffer(const Walker& walker, VariableID one, VariableID other)
{
    VariableID first = walker.plan().variable(one).firstViewOfBuffer;
    return one == other || (first && first == walker.plan().variable(other).firstViewOfBuffer);
}

static Reference narrowedToIndex(Walker& walker, Reference least, const PassedTest& fromZeroUp, Origin origin)
{
    TypeArena& types = walker.types();
    if (!least || !least.isValue() || fromZeroUp.sides().size() != 1 || !types.isSubset(walker.typeOf(least), TypeTable::int32))
        return { };
    const PassedTest::Side& side = fromZeroUp.sides().front();
    const Operation& test = walker.code().at(side.test);
    static_assert(!OperandIndex::GreaterEq::left && !OperandIndex::IsBelowLength::position);
    if (!side.testResult || (test.opcode != Opcode::GreaterEq && test.opcode != Opcode::IsBelowLength) || test.operand(0) != least.id())
        return { };
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(least);
    proof.setTestOperand(Reference::value(side.test));
    proof.setOperandIndex(0);
    proof.setIsOnTakenEdge(true);
    Reference index = walker.build(WTF::move(proof));
    return index && index.isValue() && types.at(walker.typeOf(index)).integers.minimum >= 0 ? index : Reference();
}

static std::optional<PassedTest> checkAccessGroup(AccessOfTypedElements& access, TypedArrayType typedArrayType, const CheckedView& view, BlockID exit, Origin origin)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    Walker::InBoundsPath& path = *walker.inBoundsPath();
    HeldViews views = path.isOfCapturedViews ? HeldViews::All : HeldViews::MirroredAlone;
    std::optional<KeyOffset> position = findKeyOffset(index, access.value.propertyOperand(), views);
    Reference root = position ? walker.referenceOf(position->root) : Reference();
    if (!root || !root.isValue() || position->shift > logElementSize(typedArrayType) || !walker.types().isSubset(walker.typeOf(root), TypeTable::int32))
        return std::nullopt;
    auto stillHolds = [&](const Walker::InBoundsPath::Group& group) {
        return walker.stillHolds(*group.below) && walker.stillHolds(*group.fromZeroUp);
    };
    auto proofFrom = [&](const Walker::InBoundsPath::Group& group) -> std::optional<PassedTest> {
        Reference key = buildOffsetPosition(walker, group.first, position->offset - group.offsetOfFirst, origin);
        if (!key || walker.isUnreachable())
            return std::nullopt;
        std::optional<PassedTest> inside = walker.boundsProofFromChecks(access.base, key, group.restOfLength ? group.restOfLength : group.length, *group.fromZeroUp, *group.below);
        if (inside)
            access.key = key;
        return inside;
    };
    auto lengthOfGroup = [&] {
        return view.variable ? mirroredLengthOfBuffer(walker, access.base, view.variable, typedArrayType, origin) : cappedLengthOfView(walker, access.base, typedArrayType, origin);
    };
    for (const Walker::InBoundsPath::Group& group : path.groups) {
        bool containsAccess = group.viewCell == view.cell && mirrorOneLength(walker, group.variable, view.variable) && group.root == position->root && group.rootHeld == root && group.shift == position->shift && group.offsets.contains(position->offset);
        if (!containsAccess || !stillHolds(group) || group.length != lengthOfGroup())
            continue;
        if (std::optional<PassedTest> inside = proofFrom(group))
            return inside;
    }
    uint32_t firstOfBlock = index.block(path.inputBlock).firstValue().index();
    Range offsets = Range::one(position->offset);
    unsigned numberOfAccesses = 1;
    ViewHolder holder = findViewHolder(walker, index, access.value.objectOperand());
    for (uint32_t later = access.value.id().index() + 1; later < firstOfBlock + path.end; ++later) {
        BodyValue operation = index.value(ValueID(later));
        if (isStoreIntoHolder(operation, holder) || (view.variable && operation.opcode() == Opcode::PutModuleVariable && mirrorOneBuffer(walker, operation.variable(), view.variable)))
            break;
        if (!accessesCheckedView(walker, index, operation, views))
            continue;
        ViewHolder otherHolder = findViewHolder(walker, index, operation.objectOperand());
        if (view.variable ? !mirrorOneLength(walker, otherHolder.variable, view.variable) : otherHolder != holder)
            continue;
        std::optional<KeyOffset> other = findKeyOffset(index, operation.propertyOperand(), views);
        if (other && other->root == position->root && other->shift == position->shift) {
            offsets = offsets.unionWith(Range::one(other->offset));
            ++numberOfAccesses;
        }
    }
    if (offsets.minimum == offsets.maximum && !position->offset && !position->shift)
        return std::nullopt;
    static constexpr unsigned fewestAccessesOfCapturedView = 3;
    bool readsLengthOnce = hasCappedLength(walker, access.base) || (walker.isInLoop() && walker.loopPreservesViews());
    if (!view.variable && numberOfAccesses < fewestAccessesOfCapturedView && !readsLengthOnce)
        return std::nullopt;
    if (!view.variable) {
        for (const Walker::OffsetsFromRoot& known : walker.offsetsFromRootsOf(index)) {
            if (known.root == position->root && known.shift == position->shift && known.environmentOrCapture == holder.environmentOrCapture && known.layout == holder.layout && known.field == holder.field)
                offsets = offsets.unionWith({ std::min(offsets.minimum, std::max<int64_t>(known.offsets.minimum, 0)), std::max(offsets.maximum, known.offsets.maximum) });
        }
    }
    if (!view.variable && offsets.isSingleValue())
        return std::nullopt;
    Reference length = lengthOfGroup();
    if (!length || walker.isUnreachable())
        return std::nullopt;
    Reference first = root;
    if (position->shift) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(position->shift);
        first = buildIntegerOperator(walker, Opcode::RShift, root, walker.constant(WTF::move(written)), origin);
        if (!first || walker.isUnreachable())
            return std::nullopt;
    }
    Walker::InBoundsPath::Group group { view.variable, view.cell, position->root, root, position->shift, first, 0, length, offsets };
    if (!view.variable) {
        Reference rest = cappedLengthAfterOffset(walker, access.base, typedArrayType, static_cast<uint32_t>(offsets.maximum - offsets.minimum), origin);
        Reference least = rest ? buildOffsetPosition(walker, first, offsets.minimum, origin) : Reference();
        if (!least || walker.isUnreachable())
            return std::nullopt;
        while (Reference checked = walker.state().checkedNameOf(least))
            least = checked;
        BlockID whereAsked = walker.currentBlock();
        group.below = checkBelowLength(walker, least, rest, exit, origin);
        if (!group.below || walker.isUnreachable())
            return std::nullopt;
        if (Reference narrowed = walker.currentBlock() != whereAsked ? narrowedToIndex(walker, least, *group.below, origin) : Reference()) {
            if (narrowed != least)
                walker.state().noteCheckedName(least, narrowed);
            least = narrowed;
            walker.count(StatisticID::GroupsIndexedByLeastPosition);
        }
        group.first = least;
        group.offsetOfFirst = offsets.minimum;
        group.restOfLength = rest;
        group.fromZeroUp = group.below;
        walker.recordSpeculation();
        walker.count(StatisticID::ViewAccessGroupsChecked);
        path.groups.append(group);
        return proofFrom(group);
    }
    for (const Walker::InBoundsPath::Group& other : path.groups) {
        if (other.root == group.root && other.rootHeld == root && other.shift == group.shift && other.offsets.minimum <= offsets.minimum && stillHolds(other)) {
            group.fromZeroUp = other.fromZeroUp;
            group.first = other.first;
            group.offsetOfFirst = other.offsetOfFirst;
        }
    }
    if (!group.fromZeroUp && !offsets.isSingleValue()) {
        Reference least = buildOffsetPosition(walker, first, offsets.minimum, origin);
        BlockID whereSignIsAsked = walker.currentBlock();
        group.fromZeroUp = least ? walker.findNonNegativeProof(least) : std::nullopt;
        if (!group.fromZeroUp)
            group.fromZeroUp = checkRelation(walker, Opcode::GreaterEq, least, buildZero(walker, origin), exit, origin);
        if (!group.fromZeroUp || walker.isUnreachable())
            return std::nullopt;
        if (Reference index = walker.currentBlock() != whereSignIsAsked ? narrowedToIndex(walker, least, *group.fromZeroUp, origin) : Reference()) {
            group.first = index;
            group.offsetOfFirst = offsets.minimum;
            walker.count(StatisticID::GroupsIndexedByLeastPosition);
        }
    }
    group.below = checkBelowLength(walker, buildOffsetPosition(walker, group.first, offsets.maximum - group.offsetOfFirst, origin), length, exit, origin);
    if (!group.below || walker.isUnreachable())
        return std::nullopt;
    if (!group.fromZeroUp)
        group.fromZeroUp = group.below;
    walker.recordSpeculation();
    if (offsets.minimum != offsets.maximum)
        walker.count(StatisticID::ViewAccessGroupsChecked);
    path.groups.append(group);
    return proofFrom(group);
}

struct SumBehindShift {
    Reference left;
    Reference right;
    int64_t addedToRight { 0 };
    int64_t shiftOfLeft { 0 };
    int64_t shift { 0 };
};

static std::optional<SumBehindShift> findSumBehindShift(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    Walker& walker = access.walker;
    const BodyIndex& index = walker.currentBodyIndex();
    auto definitionOf = [&](ValueID value) -> ValueID {
        value = index.stripCasts(value);
        while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::BitOr && findIntegerLiteral(index, index.value(value).operand(1)) == 0)
            value = index.stripCasts(index.value(value).operand(0));
        return value && !index.isParameter(value) ? value : ValueID();
    };
    ValueID written = definitionOf(access.value.propertyOperand());
    std::optional<int64_t> shift = written && index.opcodeOf(written) == Opcode::RShift ? findIntegerLiteral(index, index.value(written).operand(1)) : std::nullopt;
    ValueID sum = shift && *shift >= 1 && *shift <= static_cast<int64_t>(logElementSize(typedArrayType)) ? definitionOf(index.value(written).operand(0)) : ValueID();
    int64_t added = 0;
    while (sum && index.opcodeOf(sum) == Opcode::Add) {
        std::optional<int64_t> literal = findIntegerLiteral(index, index.value(sum).operand(1));
        if (!literal || std::abs(added + *literal) > std::numeric_limits<int16_t>::max())
            break;
        added += *literal;
        sum = definitionOf(index.value(sum).operand(0));
    }
    if (!sum || index.opcodeOf(sum) != Opcode::Add)
        return std::nullopt;
    auto isInt32 = [&](Reference value) { return value && value.isRecord() && walker.types().isSubset(walker.typeOf(value), TypeTable::int32); };
    for (uint32_t place : { 0, 1 }) {
        ValueID shifted = definitionOf(index.value(sum).operand(place));
        std::optional<int64_t> distance = shifted && index.opcodeOf(shifted) == Opcode::LShift ? findIntegerLiteral(index, index.value(shifted).operand(1)) : std::nullopt;
        if (!distance || *distance < *shift || *distance > 31)
            continue;
        SumBehindShift found { walker.referenceOf(index.value(shifted).operand(0)), walker.referenceOf(index.value(sum).operand(1 - place)), added, *distance - *shift, *shift };
        if (isInt32(found.left) && isInt32(found.right))
            return found;
    }
    return std::nullopt;
}

static Reference buildSumBehindShift(Walker& walker, const SumBehindShift& sum, Origin origin)
{
    auto literal = [&](int64_t value) {
        Draft integer(Opcode::Integer, origin);
        integer.setInteger(value);
        return walker.constant(WTF::move(integer));
    };
    Reference left = sum.shiftOfLeft ? buildIntegerOperator(walker, Opcode::LShift, sum.left, literal(sum.shiftOfLeft), origin) : sum.left;
    Reference right = sum.right;
    for (auto [opcode, operand] : { std::pair { Opcode::Add, sum.addedToRight }, std::pair { Opcode::BitOr, int64_t { 0 } } }) {
        if (sum.addedToRight && left && right && !walker.isUnreachable())
            right = buildIntegerOperator(walker, opcode, right, literal(operand), origin);
    }
    right = left && right && !walker.isUnreachable() ? buildIntegerOperator(walker, Opcode::RShift, right, literal(sum.shift), origin) : Reference();
    Reference position = right && !walker.isUnreachable() ? buildIntegerOperator(walker, Opcode::Add, left, right, origin) : Reference();
    position = position && !walker.isUnreachable() ? buildIntegerOperator(walker, Opcode::BitOr, position, literal(0), origin) : Reference();
    return position && !walker.isUnreachable() ? position : Reference();
}

static std::optional<PassedTest> checkPositionInBounds(AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
{
    Walker& walker = access.walker;
    if (!walker.inBoundsPath() || !access.typeOfKey.isInteger() || !isTypedView(typedArrayType) || !readsElementsInLineInTypedElements(typedArrayType) || isBigInt(typedArrayType))
        return std::nullopt;
    CheckedView view = findCheckedView(walker, access.base);
    if (!view.variable && (!walker.isOnPathOfCapturedViews() || !accessesCheckedView(walker, walker.currentBodyIndex(), access.value, HeldViews::All)))
        return std::nullopt;
    BlockID exit = view ? findBoundsExit(walker, access.value) : BlockID();
    if (!exit)
        return std::nullopt;
    origin.isTailCall = false;
    if (std::optional<PassedTest> inside = checkAccessGroup(access, typedArrayType, view, exit, origin); inside || walker.isUnreachable())
        return inside;
    Walker::InBoundsPath::CheckedAccess& checked = walker.inBoundsPath()->checked;
    if (checked.access != access.value.id() || checked.key != access.key || !walker.stillHolds(*checked.below)) {
        std::optional<SumBehindShift> sum = findSumBehindShift(access, typedArrayType);
        bool isBelowCappedLength = sum && (view.variable || hasCappedLength(walker, access.base) || (walker.isInLoop() && walker.loopPreservesViews()));
        Reference length = isBelowCappedLength && !view.variable ? cappedLengthOfView(walker, access.base, typedArrayType, origin) : lengthForOneAccess(walker, access.base, view, typedArrayType, origin);
        if (!length || walker.isUnreachable())
            return std::nullopt;
        Reference position = isBelowCappedLength ? buildSumBehindShift(walker, *sum, origin) : access.key;
        if (!position || walker.isUnreachable())
            return std::nullopt;
        std::optional<PassedTest> below = checkBelowLength(walker, position, length, exit, origin);
        if (!below || walker.isUnreachable())
            return std::nullopt;
        walker.recordSpeculation();
        if (isBelowCappedLength)
            walker.count(StatisticID::PositionsSummedBehindShift);
        checked = { access.value.id(), access.key, position, length, below };
    }
    access.key = checked.position;
    return walker.boundsProofFromChecks(access.base, access.key, checked.length, *checked.below, *checked.below);
}

bool isProvenByViewMirror(Walker& walker, const BodyValue& written, Reference receiver, TypeID typeOfView)
{
    Reference key = walker.referenceOf(written.propertyOperand());
    if (!key || !key.isRecord() || !findMirroringVariable(walker, receiver))
        return false;
    BaseOfTypedElements base = classifyBaseOfTypedElements(walker, typeOfView);
    if (base.kind != KindOfBaseOfTypedElements::TypedArray)
        return false;
    AccessOfTypedElements access { walker, written, receiver, key, { }, typeOfView, walker.types().at(walker.typeOf(key)), { }, false };
    return checkPositionInBounds(access, base.typedArrayType, walker.originOf(written)).has_value();
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

static bool isMaskedByUnknownBits(const Code& code, ValueID position)
{
    while (!code.isConstant(position) && conversionsOfForm.contains(code.at(position).opcode))
        position = code.at(position).operand(0);
    if (code.isConstant(position) || code.at(position).opcode != Opcode::BitAnd)
        return false;
    const Operation& masked = code.at(position);
    return !code.isConstant(masked.operand(0)) && !code.isConstant(masked.operand(1));
}

static Reference mostOfSmallRangeInTypedElements(Walker& walker, const BodyValue& read, Reference view, Reference position, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(position));
    if (!position.isValue() || !type.isInteger() || type.integers.minimum < 0 || type.integers.maximum > mostOfRangeAskedWholeInTypedElements || isKeyNearCounter(walker.currentBodyIndex(), read))
        return { };
    if (isMaskedByUnknownBits(walker.code(), position.id())) {
        walker.count(StatisticID::UnknownMaskKeysChecked);
        return { };
    }
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

static bool readsCapturedIntegerView(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    return isReadOfBody(access) && accessesCheckedView(access.walker, index, access.value, HeldViews::All) && !findViewHolder(access.walker, index, access.value.objectOperand()).variable;
}

static bool usesCappedLength(const Walker& walker, Reference view)
{
    return hasCappedLength(walker, view) || (walker.isInLoop() && walker.loopPreservesViews());
}

static std::optional<PassedTest> tryGuardIndexInBounds(AccessOfTypedElements& access, TypedArrayType typedArrayType, Origin origin)
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
    if (!fromZeroUp && readsCapturedIntegerView(access)) {
        bool isCapped = usesCappedLength(walker, access.base);
        std::optional<SumBehindShift> sum = isCapped ? findSumBehindShift(access, typedArrayType) : std::nullopt;
        if (isCapped)
            length = cappedLengthOfView(walker, access.base, typedArrayType, origin);
        Reference position = !length || walker.isUnreachable() ? Reference() : sum ? buildSumBehindShift(walker, *sum, origin) : access.key;
        if (!position || walker.isUnreachable())
            return std::nullopt;
        std::optional<uint64_t> noted = walker.state().answers().isEmpty() ? std::nullopt : findKnown(walker, KnownNote::CheckedBelowLength, position, length);
        Reference isBelowLength = noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
        if (!isBelowLength) {
            Draft isBelow(Opcode::IsBelowLength, origin);
            walker.chooseConservativeFields(isBelow);
            isBelow.setPositionOperand(position);
            isBelow.setLengthOperand(length);
            isBelowLength = walker.build(WTF::move(isBelow));
        }
        std::optional<PassedTest> below = continueOrFallBack(isBelowLength);
        if (!below)
            return std::nullopt;
        if (position.isValue())
            noteKnown(walker, KnownNote::CheckedBelowLength, position, length, 0, LocationMasks(AbstractHeapSet { }), isBelowLength.bits());
        if (sum)
            walker.count(StatisticID::PositionsSummedBehindShift);
        walker.count(StatisticID::PositionsCheckedAtOnce);
        std::optional<PassedTest> inside = walker.boundsProofFromChecks(access.base, position, length, *below, *below);
        if (inside)
            access.key = position;
        return inside;
    }
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

bool isViewOfSeveralKinds(Walker& walker, TypeID typeOfView)
{
    if (!typeOfView || !walker.types().at(typeOfView).hasOnlyClassObjects())
        return false;
    BaseOfTypedElements base = classifyBaseOfTypedElements(walker, typeOfView);
    return base.kind == KindOfBaseOfTypedElements::TypedArray && base.typedArrayType == NotTypedArray;
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
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnlyWithInlinees;
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
    auto isKnownToFail = [&](Reference test) {
        return !walker.isUnreachable() && walker.findPassedTest(test, false);
    };
    auto readOutOfBounds = [&] {
        walker.count(StatisticID::TypedArrayReadsFolded);
        return walker.constant(Draft(Opcode::Undefined, origin));
    };
    std::optional<PassedTest> fromZeroUp = walker.findNonNegativeProof(position);
    if (!fromZeroUp && !readIsUpdated && readsCapturedIntegerView(access)) {
        Reference bound = usesCappedLength(walker, view) ? cappedLengthOfView(walker, view, typedArrayType, origin) : length;
        if (!bound || walker.isUnreachable())
            return { };
        Draft isBelow(Opcode::IsBelowLength, origin);
        walker.chooseConservativeFields(isBelow);
        isBelow.setPositionOperand(position);
        isBelow.setLengthOperand(bound);
        Reference isBelowBound = walker.build(WTF::move(isBelow));
        if (isKnownToFail(isBelowBound))
            return readOutOfBounds();
        std::optional<PassedTest> below = walker.continueIfCheckPasses(isBelowBound, whereItLiesOutside);
        if (!below)
            return { };
        walker.count(StatisticID::PositionsCheckedAtOnce);
        return buildElement(walker.boundsProofFromChecks(view, position, bound, *below, *below));
    }
    if (!fromZeroUp) {
        Draft zero(Opcode::Integer, origin);
        zero.setInteger(0);
        Reference isNotNegative = buildIntegerOperator(walker, Opcode::GreaterEq, position, walker.constant(WTF::move(zero)), origin);
        if (isKnownToFail(isNotNegative))
            return readOutOfBounds();
        fromZeroUp = walker.continueIfCheckPasses(isNotNegative, whereItLiesOutside);
        if (!fromZeroUp)
            return { };
    }
    Reference most = readIsUpdated ? Reference() : mostOfSmallRangeInTypedElements(walker, read, view, position, origin);
    if (most && !viewIsUsedElsewhereInTypedElements(walker, read))
        most = { };
    Reference test = buildIntegerOperator(walker, Opcode::Less, most ? most : position, length, origin);
    if (most && isKnownToFail(test)) {
        most = { };
        test = buildIntegerOperator(walker, Opcode::Less, position, length, origin);
    }
    if (isKnownToFail(test))
        return readOutOfBounds();
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

static constexpr uint32_t noConstantIndex = UINT32_MAX;

struct KeyOfViewElement {
    Reference view;
    Reference key;
    uint32_t index { noConstantIndex };
};

static std::optional<KeyOfViewElement> keyOfViewElement(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!access.base.isValue() || !access.key || !access.typeOfKey.isInteger())
        return std::nullopt;
    KeyOfViewElement element { access.base, { } };
    Range range = access.typeOfKey.integers;
    if (range.isSingleValue()) {
        if (range.minimum < 0 || range.minimum >= noConstantIndex)
            return std::nullopt;
        element.index = static_cast<uint32_t>(range.minimum);
        return element;
    }
    if (!access.key.isValue() || !isArrayIndexInTypedElements(access.typeOfKey))
        return std::nullopt;
    element.key = access.key;
    return element;
}

static bool isExactIntegerOfView(TypedArrayType typedArrayType, const Type& stored)
{
    if (!stored.isInteger())
        return false;
    if (typedArrayType == TypeFloat32)
        return stored.integers.isSubsetOf({ -(int64_t { 1 } << 24), int64_t { 1 } << 24 });
    if (typedArrayType == TypeFloat16)
        return stored.integers.isSubsetOf({ -(int64_t { 1 } << 11), int64_t { 1 } << 11 });
    return true;
}

static void noteViewElement(Walker& walker, const KeyOfViewElement& element, Reference value)
{
    LocationMasks places(AbstractHeapSet { AbstractHeap::TypedArrayStorage, AbstractHeap::TypedArrayElements });
    noteKnown(walker, KnownNote::ElementOfView, element.view, element.key, element.index, places, uint64_t(element.index) << 32 | value.bits());
}

static Vector<uint64_t, 16> viewElementsBesideStore(Walker& walker, const KeyOfViewElement& stored)
{
    static constexpr size_t mostElements = 64;
    if (stored.index == noConstantIndex)
        return { };
    Vector<uint64_t, 16> kept = listKnown(walker, KnownNote::ElementOfView, stored.view);
    kept.removeAllMatching([&](uint64_t noted) {
        uint32_t index = static_cast<uint32_t>(noted >> 32);
        return index == noConstantIndex || index == stored.index;
    });
    if (kept.size() > mostElements)
        kept.shrink(mostElements);
    return kept;
}

static Reference valueReadBehindStore(Walker& walker, Reference value, TypedArrayType typedArrayType, Origin origin)
{
    TypeArena& types = walker.types();
    Type type = types.at(walker.typeOf(value));
    if (typedArrayType == TypeFloat64)
        return type.isNumber() ? value : Reference();
    if (!type.isInteger() || !isInt(typedArrayType) || isBigInt(typedArrayType) || isClamped(typedArrayType))
        return { };
    TypeID element = elementTypeOf(typedArrayType);
    if (types.isSubset(walker.typeOf(value), element))
        return value;
    if (!type.integers.isSubsetOf(Range::int32()))
        return { };
    auto integer = [&](int32_t written) {
        Draft draft(Opcode::Integer, origin);
        draft.setInteger(written);
        return walker.constant(WTF::move(draft));
    };
    if (typedArrayType == TypeUint32)
        return buildIntegerOperator(walker, Opcode::URShift, value, integer(0), origin);
    if (typedArrayType == TypeUint8 || typedArrayType == TypeUint16)
        return buildIntegerOperator(walker, Opcode::BitAnd, value, integer(typedArrayType == TypeUint8 ? 0xFF : 0xFFFF), origin);
    return { };
}

static bool bindKnownViewElement(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    Walker& walker = access.walker;
    std::optional<KeyOfViewElement> element = keyOfViewElement(access);
    std::optional<uint64_t> known = element ? findKnown(walker, KnownNote::ElementOfView, element->view, element->key, element->index) : std::nullopt;
    if (!known)
        return false;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Reference value = Reference::fromBits(static_cast<uint32_t>(*known));
    if (!walker.types().isSubset(walker.typeOf(value), elementTypeInTypedElements(typedArrayType)))
        value = valueReadBehindStore(walker, value, typedArrayType, origin);
    if (!value)
        return false;
    if (walker.checksProofs() && isViewOverUnsharedMemory(walker, access.base)) {
        Draft read = draftOfTypedElements(walker, access.value, Opcode::GetTypedArrayElement);
        read.setViewOperand(access.base);
        read.setPositionOperand(access.key);
        read.setTypedArrayType(typedArrayType);
        if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(access.base, access.key))
            read.setIsInsideView(*inside);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
        WalkState believed = walker.state();
        if (read.isInsideView())
            walker.checkThatMemoryAnswersAsBelieved(value, WTF::move(read));
        walker.state() = WTF::move(believed);
    }
    walker.count(StatisticID::ViewElementsReused);
    walker.bind(access.value.id(), value);
    return true;
}

static bool buildViewReadOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetTypedArrayElement, KnowledgeLevel::Proven);
    if (Reference element = access.walker.boundElementReadBy(access.value, access.base)) {
        access.walker.bind(access.value.id(), element);
        return true;
    }
    if (bindNumberReadBehindTestsOfTypedElements(access) || bindKnownViewElement(access, typedArrayType))
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
    bool isOnPath = walker.isOnPathOfCapturedViews();
    AccessOfTypedElements checkedAccess = access;
    if (!inside && !isOnPath && walker.isIterationEffectFree())
        inside = tryGuardIndexInBounds(checkedAccess, typedArrayType, read.origin());
    if (!inside)
        inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    if (!inside) {
        hoistTypedArrayBoundsCheck(access, typedArrayType);
        inside = walker.findWhatHoldsPositionInsideList(access.base, access.key);
    }
    if (!inside && isOnPath)
        inside = checkPositionInBounds(checkedAccess, typedArrayType, read.origin());
    if (!inside && !walker.isUnreachable())
        inside = tryGuardIndexInBounds(checkedAccess, typedArrayType, read.origin());
    if (!inside && !isOnPath)
        inside = checkPositionInBounds(checkedAccess, typedArrayType, read.origin());
    if (walker.isUnreachable()) {
        walker.bind(access.value.id(), Reference());
        return true;
    }
    if (!inside)
        inside = holdPositionBelowLengthOrStopInTypedElements(access, typedArrayType, read.origin());
    read.setViewOperand(access.base);
    read.setPositionOperand(checkedAccess.key);
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
    Reference built = walker.build(WTF::move(read));
    walker.bind(access.value.id(), built);
    if (std::optional<KeyOfViewElement> element = inside && built && built.isValue() && typedArrayType != NotTypedArray && !walker.isUnreachable() ? keyOfViewElement(access) : std::nullopt; element && walker.types().at(walker.typeOf(built)).isNumber())
        noteViewElement(walker, *element, built);
    return true;
}

static KnowledgeLevel levelOfViewStoreOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    TypeArena& types = access.walker.types();
    TypeID storedID = access.typeOfStored;
    Type stored = types.at(storedID);
    static constexpr uint16_t convertedInLoops = Primitive::Number | Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::AnyString;
    bool isConvertedInLoop = access.walker.isInLoop() && stored.hasOnlyPrimitives(convertedInLoops) && types.overlap(storedID, TypeTable::number);
    bool convertsWithoutCode = !stored.isNever() && (stored.isNumberLike() || isConvertedInLoop);
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
    if (!inside && level == KnowledgeLevel::Proven) {
        AccessOfTypedElements checkedAccess = access;
        inside = checkPositionInBounds(checkedAccess, typedArrayType, put.origin());
        put.setPositionOperand(checkedAccess.key);
    }
    if (walker.isUnreachable()) {
        walker.bind(access.value.id(), Reference());
        return true;
    }
    if (inside)
        put.setIsInsideView(*inside);
    put.setMode(mode);
    put.setLevel(level);
    put.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    nameStorageOfViewInTypedElements(access, put, typedArrayType);
    std::optional<KeyOfViewElement> element = inside && level == KnowledgeLevel::Proven && typedArrayType != NotTypedArray ? keyOfViewElement(access) : std::nullopt;
    Vector<uint64_t, 16> kept;
    if (element)
        kept = viewElementsBesideStore(walker, *element);
    walker.bind(access.value.id(), walker.build(WTF::move(put)));
    if (element && !walker.isUnreachable() && !walker.hasFailed()) {
        for (uint64_t noted : kept)
            noteViewElement(walker, { element->view, { }, static_cast<uint32_t>(noted >> 32) }, Reference::fromBits(static_cast<uint32_t>(noted)));
        TypeID typeOfValue = stored ? walker.typeOf(stored) : TypeID();
        if (typeOfValue && (isExactIntegerOfView(typedArrayType, walker.types().at(typeOfValue)) || typedArrayType == TypeFloat64) && walker.types().at(typeOfValue).isNumber())
            noteViewElement(walker, *element, stored);
    }
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

static bool branchReadsTestedElement(const AccessOfTypedElements& access)
{
    const BodyIndex& index = access.walker.currentBodyIndex();
    ValueID base = index.stripCasts(access.value.objectOperand());
    ValueID key = index.stripCasts(access.value.propertyOperand());
    BodyTerminator branch = index.terminator(index.blockOf(access.value.id()));
    if (branch.kind() != TerminatorKind::Branch)
        return false;
    return std::ranges::any_of(branch.targets(), [&](BodyTarget target) {
        return std::ranges::any_of(index.block(target.block()).values(), [&](BodyValue read) {
            return read.opcode() == Opcode::ElemGet && index.stripCasts(read.objectOperand()) == base && index.stripCasts(read.propertyOperand()) == key;
        });
    });
}

static bool buildReadElementPresenceTest(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (access.isOnlyAskedAbout || !walker.isInLoop() || !walker.opensGuesses() || !walker.has(Mechanism::Folding) || !access.base || !access.base.isValue() || !access.key || !access.typeOfKey.isInteger())
        return false;
    if (!walker.types().at(access.typeOfBase).isObject() || walker.hasUninlinedCallInIterations() || !branchReadsTestedElement(access))
        return false;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Draft lookup(Opcode::GetElementInStorageOrAbsent, origin);
    lookup.setObjectOperand(access.base);
    lookup.setPositionOperand(access.key);
    lookup.setValueType(TypeTable::anythingOrAbsent);
    Reference found = walker.build(WTF::move(lookup));
    if (!found || !found.isValue() || walker.isUnreachable())
        return true;
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(found);
    question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    Reference isAbsent = walker.build(WTF::move(question));
    if (!isAbsent || walker.isUnreachable())
        return true;
    walker.recordSpeculation();
    walker.count(StatisticID::ElementsReadByPresenceTests);
    ValueID written = access.value.id();
    BlockID asWritten = walker.addBlock();
    BlockID present = walker.addBlock();
    BlockID meeting = walker.addBlock();
    Vector<Walker::JoinArm, 2> arms;
    walker.branch(isAbsent, asWritten, present);
    walker.continueIn(asWritten);
    Draft test = walker.draftOf(access.value);
    walker.chooseConservativeFields(test);
    Reference tested = walker.hasFailed() ? Reference() : walker.buildWithInlineLookup(WTF::move(test));
    if (walker.hasFailed())
        return true;
    Draft none(Opcode::Absent, origin);
    if (tested && !walker.isUnreachable())
        arms.append(Walker::JoinArm { walker.currentBlock(), { tested, walker.constant(WTF::move(none)) } });
    walker.continueIn(present);
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(found);
    narrowed.setTestOperand(isAbsent);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(false);
    Reference element = walker.build(WTF::move(narrowed));
    Draft answer(Opcode::Boolean, origin);
    answer.setBoolean(true);
    if (element && !walker.isUnreachable())
        arms.append(Walker::JoinArm { walker.currentBlock(), { walker.constant(WTF::move(answer)), element } });
    std::array<Reference, 2> met;
    if (arms.isEmpty() || walker.hasFailed() || !walker.joinArms(arms.span(), meeting, met))
        return true;
    walker.bind(written, met[0]);
    noteKnown(walker, KnownNote::ElementOfPresenceTest, access.base, access.key, 0, readsOfOwnKeyLookup(walker.program()), met[1].bits(), true);
    return true;
}

static bool bindElementOfPresenceTest(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (access.isOnlyAskedAbout || !access.base || !access.base.isValue() || !access.key)
        return false;
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ElementOfPresenceTest, access.base, access.key);
    Reference element = noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
    if (!element || !walker.types().isSubset(walker.types().withoutPrimitives(walker.typeOf(element), Primitive::Absent), walker.resultTypeOf(walker.draftOf(access.value))))
        return false;
    if (!(walker.types().at(walker.typeOf(element)).primitives & Primitive::Absent)) {
        walker.bind(access.value.id(), element);
        return true;
    }
    if (!walker.opensGuesses())
        return false;
    Origin origin = walker.originOf(access.value);
    origin.isTailCall = false;
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(element);
    question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    Reference isAbsent = walker.build(WTF::move(question));
    if (!isAbsent || walker.isUnreachable())
        return true;
    ValueID written = access.value.id();
    BlockID asWritten = walker.addBlock();
    BlockID present = walker.addBlock();
    BlockID meeting = walker.addBlock();
    Vector<Walker::JoinArm, 2> arms;
    walker.branch(isAbsent, asWritten, present);
    walker.continueIn(asWritten);
    Draft read = walker.draftOf(access.value);
    walker.chooseConservativeFields(read);
    Reference readAsWritten = walker.hasFailed() ? Reference() : walker.buildWithInlineLookup(WTF::move(read));
    if (walker.hasFailed())
        return true;
    if (readAsWritten && !walker.isUnreachable())
        arms.append(Walker::JoinArm { walker.currentBlock(), { readAsWritten } });
    walker.continueIn(present);
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(element);
    narrowed.setTestOperand(isAbsent);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(false);
    Reference found = walker.build(WTF::move(narrowed));
    if (found && !walker.isUnreachable())
        arms.append(Walker::JoinArm { walker.currentBlock(), { found } });
    std::array<Reference, 1> met;
    if (!arms.isEmpty() && !walker.hasFailed() && walker.joinArms(arms.span(), meeting, met))
        walker.bind(written, met[0]);
    return true;
}

static Reference stringOfStringObjectForTypedElements(Walker& walker, Reference object)
{
    if (!object || !object.isValue())
        return { };
    const Operation& made = walker.code().at(underlyingCellOf(walker.code(), object.id()));
    if (made.opcode != Opcode::CallBuiltin || made.builtinCallForm() != BuiltinCallForm::Whole || made.argumentsOperands().size() != 1)
        return { };
    const BuiltinTable& builtins = walker.program().builtins();
    const BuiltinOverload& row = builtins.at(made.overload());
    if (row.mode != BuiltinCallMode::Construct || builtins.canonicalID(row.builtin) != builtins.canonicalID(Builtins::String))
        return { };
    Reference string = referenceToRecordForStrings(walker, made.argumentsOperands()[0]);
    return walker.types().at(walker.typeOf(string)).isAnyString() ? string : Reference();
}

static bool buildLengthOfStringObjectForTypedElements(Walker& walker, const BodyValue& value, Reference object)
{
    Reference string = stringOfStringObjectForTypedElements(walker, object);
    if (!string)
        return false;
    walker.count(StatisticID::LengthsOfStringObjectsRead);
    Reference length = buildLengthOfCharacterInSight(walker, string, walker.originOf(value));
    if (!length && !walker.hasFailed())
        length = buildLengthOfIntegerString(walker, string, walker.originOf(value));
    if (!length && !walker.hasFailed()) {
        Draft read = draftOfTypedElements(walker, value, Opcode::GetStringLength);
        read.setStringOperand(string);
        length = walker.build(WTF::move(read));
    }
    if (length)
        walker.bind(value.id(), length);
    return true;
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
    if (!isOnlyAskedAbout && buildLengthOfStringObjectForTypedElements(walker, value, base))
        return true;
    if (hasKind(types, typeOfBase, OperandKind::Array)) {
        if (isOnlyAskedAbout)
            return answerAhead(Opcode::GetArrayLength);
        std::optional<uint32_t> known = lengthOfFreshList(walker, base);
        if (!known)
            known = lengthOfListMadeInSight(walker, base);
        if (!known && walker.has(Mechanism::Folding) && walker.facts().linked().nobodyChangesReceiverAt(walker.currentFunction(), value.id())) {
            if (WrittenListOfVariable written = writtenListReadBy(walker, value.objectOperand())) {
                known = written.length;
                checkLengthOfListMadeInSight(walker, base, written.length, walker.originOf(value));
            }
        }
        if (known) {
            Draft constant(Opcode::Integer, walker.originOf(value));
            constant.setInteger(*known);
            walker.bind(value.id(), walker.constant(WTF::move(constant)));
            return true;
        }
        if (std::optional<uint64_t> stored = findKnown(walker, KnownNote::StoredLengthOfList, base)) {
            walker.bind(value.id(), Reference::fromBits(static_cast<uint32_t>(*stored)));
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
        Vector<ValueID, 2> joinsThatFailWhereReaderFails;
        if (Reference ofParts = walker.hasFailed() ? Reference() : buildLengthOnlyJoin(walker, value.objectOperand(), walker.originOf(value), joinsThatFailWhereReaderFails)) {
            walker.bind(value.id(), ofParts);
            for (ValueID join : joinsThatFailWhereReaderFails)
                walker.code().letJoinFailWhereItsReaderFails(join);
            return true;
        }
        if (Reference ofInteger = buildLengthOfIntegerString(walker, base, walker.originOf(value))) {
            walker.bind(value.id(), ofInteger);
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
    Reference built = walker.build(WTF::move(store));
    walker.bind(value.id(), built);
    if (mode == ECMAMode::Strict && built && built.isValue() && !walker.isUnreachable() && !walker.hasFailed()) {
        ValueID length = walker.code().at(built.id()).lengthOperand();
        noteKnown(walker, KnownNote::StoredLengthOfList, list, { }, 0, placesUnderLengthsOfLists, (walker.code().isConstant(length) ? Reference::constant(length) : Reference::value(length)).bits());
    }
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

static bool bindElementOfConstantArray(Walker& walker, const BodyValue& read)
{
    TypeArena& types = walker.types();
    Reference base = walker.referenceOf(read.objectOperand());
    Reference key = walker.referenceOf(read.propertyOperand());
    if (!base || !base.isValue() || !key || !walker.has(Mechanism::Folding) || walker.isUnreachable() || !hasKind(types, walker.typeOf(base), OperandKind::Array))
        return false;
    Type typeOfKey = types.at(walker.typeOf(key));
    if (!typeOfKey.isInteger() || !typeOfKey.integers.isSingleValue() || !walker.facts().linked().nobodyChangesReceiverAt(walker.currentFunction(), read.id()))
        return false;
    int64_t position = typeOfKey.integers.minimum;
    WrittenListOfVariable written = position >= 0 && position < UINT32_MAX ? writtenListReadBy(walker, read.objectOperand()) : WrittenListOfVariable();
    Reference element = written && position < written.length ? elementOfConstantArray(walker, written.data, static_cast<uint32_t>(position)) : Reference();
    if (!element || !types.at(walker.typeOf(element)).isNumber())
        return false;
    if (walker.checksProofs()) {
        Draft readAgain = draftOfTypedElements(walker, read, Opcode::GetArrayElement);
        readAgain.setArrayOperand(base);
        readAgain.setPositionOperand(key);
        WalkState believed = walker.state();
        walker.checkThatMemoryAnswersAsBelieved(element, WTF::move(readAgain));
        walker.state() = WTF::move(believed);
    }
    walker.count(StatisticID::ConstantArrayElementsFolded);
    walker.bind(read.id(), element);
    return true;
}

bool buildTypedElementAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.opcode() == Opcode::ElemGet && !inputs.areOnlyTypes() && bindElementOfConstantArray(walker, value))
        return true;
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
        return bindElementOfPresenceTest(access) || buildReadOfTypedElements(access);
    }
    if (opcode == Opcode::ElemHas)
        return buildTestOfTypedElements(access) || buildReadElementPresenceTest(access);
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
    if (!walker.has(Mechanism::TypedOperations) || !walker.isInTrialVisit() || walker.isUnreachable())
        return false;
    bool isInVersion = walker.isInSpecializedIteration();
    Reference poppedFrom = fieldArrayPoppedFrom(walker, call);
    if (Reference list = poppedFrom ? poppedFrom : fieldArrayPushedOnto(walker, call); list && !(isInVersion && findListTestedForStoresInFrontOfLoop(walker, list))) {
        Opcode opcode = poppedFrom ? Opcode::ShortenArray : Opcode::ExtendArrayLength;
        Draft raise(opcode, walker.originOf(call));
        walker.chooseConservativeFields(raise);
        raise.setOperand(0, list);
        const Effects effects = declaredBoundOf(opcode);
        walker.recordTrialEffectsOfWriter(effects, objectsMadeHereThatAloneAreWritten(walker, raise, effects));
        return true;
    }
    if (!walker.isInTrialVisitOfCurrentFrame())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return false;
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop)
        return false;
    bool fillsLiteral = !isInVersion && walker.pushOfLoopFillsLiteral(index.loop(loop).head) && literalThatCallPushesOntoInEveryTurn(walker, call, index.loop(loop).head);
    bool isPop = !fillsLiteral && walker.isPopFromLiteral(call);
    if (!fillsLiteral && ((!isPop && !walker.isPushOntoLiteral(call)) || (isInVersion && findListTestedForStoresInFrontOfLoop(walker, walker.referenceOf(index.stripCasts(call.as<Opcode::Call>().thisValueOperand()))))))
        return false;
    Opcode opcode = isPop ? Opcode::ShortenArray : Opcode::ExtendArrayLength;
    Draft raise(opcode, walker.originOf(call));
    walker.chooseConservativeFields(raise);
    raise.setOperand(0, walker.referenceOf(index.stripCasts(call.as<Opcode::Call>().thisValueOperand())));
    const Effects effects = declaredBoundOf(opcode);
    if (walker.shouldAssumeLocalArrayReceivers()) [[unlikely]] {
        walker.recordTrialEffectsOfWriter(effects, Vector<uint32_t, 2> { });
        return true;
    }
    if (raise.operand(0)) {
        walker.recordTrialEffectsOfWriter(effects, objectsMadeHereThatAloneAreWritten(walker, raise, effects));
        return true;
    }
    walker.recordTrialEffects(effects);
    return true;
}

static bool buildPushAtLength(Walker& walker, const BodyValue& call, Reference list)
{
    Reference pushed = walker.referenceOf(call.as<Opcode::Call>().argumentsOperands()[0]);
    std::optional<HoistedArrayGuard> tested = pushed && walker.typeOf(pushed) && walker.typeOf(list) ? findListTestedForStoresInFrontOfLoop(walker, list) : std::nullopt;
    if (!tested || !callsArrayPush(walker, call, list))
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
    if (walker.someCallerLoop([&](BlockID head) { return !!findKnown(walker, KnownNote::LengthEqualsInitialCounter, list, { }, head.index()); }))
        return false;
    Draft read(Opcode::GetArrayLength, walker.originOf(call));
    walker.chooseConservativeFields(read);
    read.setArrayOperand(list);
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    readLengthThroughStorageInFrontOfLoop(walker, read, *tested, origin);
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

static bool buildPopFromList(Walker& walker, const BodyValue& call, Reference list)
{
    std::optional<HoistedArrayGuard> tested = list && list.isValue() ? findListTestedForStoresInFrontOfLoop(walker, list) : std::nullopt;
    if (!tested || !tested->isWritable || !isNumericStorage(tested->storage) || tested->admitsInt32Storage || !walker.canRestartIteration() || !callsArrayMethod(walker, call, list, BuiltinNames::pop, Builtins::ArrayPrototypePop))
        return false;
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    auto comparison = [&](Opcode opcode, Reference left, Reference right) {
        Draft compared(opcode, origin);
        walker.chooseConservativeFields(compared);
        compared.setLeftOperand(left);
        compared.setRightOperand(right);
        compared.setForm(operatorFormFor(walker, compared));
        return walker.build(WTF::move(compared));
    };
    auto integer = [&](int64_t number) {
        Draft constant(Opcode::Integer, origin);
        constant.setInteger(number);
        return walker.constant(WTF::move(constant));
    };
    Draft asked(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(asked);
    asked.setArrayOperand(list);
    Reference length = walker.build(WTF::move(asked));
    Reference last = length ? buildIntegerOperator(walker, Opcode::Sub, length, integer(1), origin) : Reference();
    if (!last || walker.isUnreachable())
        return false;
    std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, last);
    if (!inside) {
        Reference isNotEmpty = comparison(Opcode::GreaterEq, last, integer(0));
        if (!continueOrRestartIteration(walker, isNotEmpty, true))
            return true;
        Reference isBelow = comparison(Opcode::Less, last, length);
        if (!continueOrRestartIteration(walker, isBelow, true))
            return true;
        std::optional<PassedTest> fromZeroUp = walker.findPassedTest(isNotEmpty);
        std::optional<PassedTest> below = walker.findPassedTest(isBelow);
        if (!fromZeroUp || !below)
            return false;
        inside = walker.boundsProofFromChecks(list, last, length, *fromZeroUp, *below);
        if (!inside)
            return false;
    }

    Reference element = walker.state().element(list, last);
    if (!element || !walker.types().isSubset(walker.typeOf(element), tested->storage == ArrayStorage::Int32 ? TypeTable::int32 : TypeTable::number)) {
        Draft isPresent(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(isPresent);
        isPresent.setArrayOperand(list);
        isPresent.setPositionOperand(last);
        isPresent.setKeyIsIntegral(true);
        isPresent.setIsInBounds(*inside);
        nameKindOfListTestedInFrontOfLoop(isPresent, *tested);
        Reference holdsElement = walker.build(WTF::move(isPresent));
        if (!continueOrRestartIteration(walker, holdsElement, true))
            return true;
        std::optional<PassedTest> present = walker.showElementPresentBehindTest(list, last, walker.findPassedTest(holdsElement));
        if (!present)
            return false;
        Draft read(Opcode::GetArrayElement, origin);
        walker.chooseConservativeFields(read);
        read.setArrayOperand(list);
        read.setPositionOperand(last);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        read.setIsPresent(*present);
        nameKindOfListTestedInFrontOfLoop(read, *tested);
        element = walker.build(WTF::move(read));
        if (!element || walker.isUnreachable())
            return true;
    }

    Draft shorten(Opcode::ShortenArray, origin);
    walker.chooseConservativeFields(shorten);
    shorten.setArrayOperand(list);
    shorten.setPositionOperand(last);
    shorten.setArrayStorage(tested->storage);
    shorten.restOn(*inside);
    walker.build(WTF::move(shorten));
    walker.bind(call.id(), element);
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
    if (Reference list = fieldArrayPushedOnto(walker, call))
        return buildPushAtLength(walker, call, list);
    if (Reference list = fieldArrayPoppedFrom(walker, call))
        return buildPopFromList(walker, call, list);
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop)
        return false;
    if (walker.isPopFromLiteral(call))
        return buildPopFromList(walker, call, walker.referenceOf(index.stripCasts(call.as<Opcode::Call>().thisValueOperand())));
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
    if (builtinClassBelowType(walker, present))
        return typedArrayTypeOf(builtinContextOfTypedElements(walker), present);
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
    bool expectsCharacters = value.opcode() == Opcode::ElemGet && isProvenString(walker, expected);
    if (!expectsCharacters && elementsExpectedByTypedElements(view) == ExpectedElements::Anything)
        return false;
    Reference object = walker.referenceOf(value.objectOperand());
    Reference key = walker.referenceOf(value.propertyOperand());
    if (!object || !key)
        return false;
    if (expectsCharacters) {
        Type typeOfKey = walker.types().at(walker.typeOf(key));
        bool mayBeIndex = typeOfKey.isJavaScriptValue() && typeOfKey.primitives & Primitive::Integer;
        if (!mayBeIndex || walker.isUnreachable())
            return false;
        Draft draft = walker.draftOf(value);
        walker.chooseConservativeFields(draft);
        draft.setKeyIsIntegral(typeOfKey.isNumberWithoutFraction());
        draft.setExpectedElements(ExpectedElements::Characters);
        walker.count(StatisticID::CharactersExpectedUnderKeys);
        walker.bind(value.id(), walker.build(WTF::move(draft)));
        return true;
    }
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
    ArrayStorage storage { ArrayStorage::Int32 };
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
    bool computes = false;
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
        computes = true;
    }
    if (accesses < minAccessesInRun)
        return std::nullopt;
    if (computes || stretch.stores || !isReachedByLoops(walker) || predictedStorageOfUnbuiltList(walker, base) != ArrayStorage::Double)
        return stretch;

    stretch.storage = ArrayStorage::Double;
    for (uint32_t place = stretch.end; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        if (opcodesThatStateTypes.contains(operation.opcode()) || primitiveConstants.contains(operation.opcode()))
            continue;
        if (operation.opcode() == Opcode::Call) {
            auto arguments = operation.as<Opcode::Call>().argumentsOperands();
            if (arguments.size() >= minAccessesInRun && std::ranges::all_of(arguments, [&](ValueID argument) { return numbers.contains(index.stripCasts(argument)); }))
                stretch.end = place + 1;
        }
        break;
    }
    return stretch;
}

static bool buildConstantIndexRun(Walker& walker, const BodyValue& firstRead)
{
    if (firstRead.opcode() != Opcode::ElemGet || walker.isInLoop() || walker.isUnreachable() || walker.readStandsUnderHandler() || !walker.opensGuesses() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (firstRead.id().index() >= index.numberOfValues() || index.value(firstRead.id()).words() != firstRead.words() || index.useCount(firstRead.objectOperand()) < minAccessesInRun)
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
        if (!list || !passes(askWhetherListIsAsMade(walker, list, origin)) || !passes(askWhetherStorageOfListIsOfKind(walker, list, stretch->storage, origin, stretch->stores)))
            return false;
        noteKindOfStorageThatTurnFound(walker, list, stretch->storage, stretch->stores);
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
            isPresent.setArrayStorage(stretch->storage);
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
            read.setArrayStorage(stretch->storage);
            read.setLevel(KnowledgeLevel::Proven);
            read.setKeyIsIntegral(true);
            read.setIsPresent(*present);
            Reference element = walker.build(WTF::move(read));
            if (!element || !types.isSubset(walker.typeOf(element), stretch->storage == ArrayStorage::Double ? TypeID(TypeTable::number) : TypeID(TypeTable::int32)))
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
    walker.count(stretch->storage == ArrayStorage::Double ? StatisticID::ArraysGuardedForReads : StatisticID::ArraysCheckedAtStores);
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
