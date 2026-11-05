/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mechanism 2, elements: ElemGet, ElemSet, ElemHas and reads of `length` become one operation of the family ElementAccess, whose slow side lies inside its emitter. No block is added.
// At a suspension: nothing of its own. What it has noted of elements ends by the effects of the suspension, as at any call, but for what a proven store has put into a fresh object.
// At a throw: each form throws what the closed form throws, where it throws it.
// Withheld: Walker::copy() builds the closed forms.

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
#include "TypedArrayType.h"

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
// AOTWalkNumbers.cpp
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

// AppendElement has one form, which tests the storage for itself and takes the value in the form of its row: Walker::copy() builds it.
static constexpr OpcodeSet opcodesOfTypedElements { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::PropGet };

static constexpr Range arrayIndexesOfTypedElements { 0, static_cast<int64_t>(UINT32_MAX) - 1 };

namespace {

// Other: nothing here applies, and the next link of the chain may open a guess. Arguments: one that has been built. Only one that never is has a form of its own for its elements.
enum class KindOfBaseOfTypedElements : uint8_t { Other, Array, TypedArray, String, Arguments };

struct BaseOfTypedElements {
    KindOfBaseOfTypedElements kind { KindOfBaseOfTypedElements::Other };
    TypedArrayType typedArrayType { NotTypedArray };
};

struct AccessOfTypedElements {
    Walker& walker;
    const BodyValue& value;
    // Null where the access is only asked about: InputsOfTypedOperation::areOnlyTypes().
    Reference base;
    Reference key;
    // Of a store alone.
    Reference stored;
    TypeID typeOfBase;
    // A number.
    Type typeOfKey;
    TypeID typeOfStored;
    bool isOnlyAskedAbout { false };
};

} // anonymous namespace

static BuiltinTypeContext builtinContextOfTypedElements(Walker& walker)
{
    return { walker.program(), walker.types(), &walker.facts() };
}

// An instance of a class below Array is no such array: it is never pristine, and the forms that test for themselves turn it away every time.
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

// The origin of the operation being walked, and what is said of its result.
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

// The state knows a fresh object under the record that made it. Null: what is named is not fresh.
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

// The closed operation, which is also the failing side of a form that tests for itself, hands the object to a getter, a setter or a trap as `this`.
// In front of the operation, whose effects then end what was noted of the object.
static void markBaseAsPassedInTypedElements(Walker& walker, Reference base)
{
    walker.letLeave(base);
}

// A proven store runs no code and hands the object to nobody, so a fresh one stays fresh. No effects end what is known of it: WalkState::noteElement() ends what the store may have replaced.
// No value: what lies there now is not known.
static void noteStoredElementInTypedElements(Walker& walker, Reference base, Reference key, Reference stored)
{
    Reference object = freshObjectNamedInTypedElements(walker, base);
    walker.state().noteElement(object ? object : base, key, stored);
}

// Whoever asks ahead for the effects of what would be built is told the operation and its level: effectsOfTypedFormAhead().
static bool answerAheadOfTypedElements(const AccessOfTypedElements& access, Opcode opcode, KnowledgeLevel level)
{
    access.walker.noteFormAskedAhead(opcode, level);
    return true;
}

// ---- Arrays

// A hole and a position behind the end are answered by the prototypes, and so is a key that is no index: a negative one, one with a fraction.
// Proven: Facts says that nothing lies there, so the operation answers by itself and runs no code.
// Expected: the operation asks the array when it runs, finds an own element under an int32 by itself, and does the closed operation for whatever else it meets.
// Unknown: neither.
static KnowledgeLevel levelOfArrayAccessOfTypedElements(const AccessOfTypedElements& access)
{
    PristineAspectSet required { PristineAspects::Storage };
    if (!isArrayIndexInTypedElements(access.typeOfKey))
        required.add(PristineAspects::Names);
    if (pristineAspectsProvenBy(builtinContextOfTypedElements(access.walker), access.typeOfBase, BuiltinClasses::Array).containsAll(required))
        return KnowledgeLevel::Proven;
    return access.typeOfKey.isInteger() ? KnowledgeLevel::Expected : KnowledgeLevel::Unknown;
}

// Nothing but this code has the list, and it is as it was made: it has no accessor among its elements, and what stands above it holds nothing under a number. A read hands it to nobody, so it stays fresh.
static KnowledgeLevel levelOfArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    if ((access.base && isFreshListAsMade(access.walker, access.base)) || readsOffListThatStaysAsMadeByLink(access.walker, access.value))
        return KnowledgeLevel::Proven;
    return levelOfArrayAccessOfTypedElements(access);
}

// A guess, for the forms that test the kind that they name. Other: they go by the kind that they find.
// `function first(nodes: Node[]) { return nodes[0]; }`: a test for Array proves nothing of the elements. What is written of them stands in what is expected of the value, as findListsReadInLoop() asks it.
static ArrayStorage storageDeclaredForTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    TypeArena& types = walker.types();
    ArrayStorage declared = arrayStorageDeclaredBy(walker.program(), types, access.typeOfBase).predicted();
    if (declared != ArrayStorage::Other || access.isOnlyAskedAbout)
        return declared;
    TypeID expected = findExpectation(walker, access.value.objectOperand()).type;
    ArrayStorage written = expected ? arrayStorageDeclaredBy(walker.program(), types, types.withoutNullish(expected)).predicted() : ArrayStorage::Other;
    // A read that names no kind finds an int32 in storage that holds boxed values by one test. Named, the kind is asked for behind storage for doubles.
    return written == ArrayStorage::Int32 && access.value.opcode() != Opcode::ElemSet ? ArrayStorage::Other : written;
}

// A value that the kind refuses gives the array another kind, and the test would fail from then on. A value that holds a cell ends up in storage for any value.
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

// Of the key nothing is proven, or only that it is a number. A list is nearly always read under an int32: the read asks the key for its tag, finds an own element by itself, and does the closed operation for whatever else it meets:
// a number in another form, a text, a symbol, an object.
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

// Tests on the way here have found storage that holds numbers alone and an element at the position, the element was read, and nothing has written the list since. openGuessAtElementOfNumbers().
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

// Of a read that names no kind of storage. Unchosen: the form that the rules give it, a box. build() keeps a form that the draft brings, where the proven type allows it.
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
    // Int32 are added and compared as int32, out of their boxes: by way of doubles `list[i] > greatest` takes 30 instructions a turn for 22, and a sum of elements loses its integer form.
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
    // `line += list[i] + ","`: the writer of texts asks a box for its tag, and writes an int32 without a call.
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

// `t += list[i]`: arithmetic makes NaN of undefined, so whoever reads for it alone may take a hole for NaN. The other operand is a number: beside a text undefined is spelled out.
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
    // Both operands may be the one read, and what stands behind the read in the text is not built yet.
    Reference built = other != name ? walker.referenceOf(other) : Reference();
    return built && walker.types().at(walker.typeOf(built)).isNumber();
}

static bool buildArrayReadOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    if (!access.isOnlyAskedAbout && bindNumberReadBehindTestsOfTypedElements(access))
        return true;
    KnowledgeLevel level = levelOfArrayReadOfTypedElements(access);
    // What a row in front of a loop has shown of the list. Asked ahead it answers the same, so a look at a loop has to ask whether the loop leaves lists as they are: LooksAtLoops::preview().
    std::optional<ListTestedInFrontOfLoop> tested = level == KnowledgeLevel::Unknown ? std::nullopt : findListTestedInFrontOfLoop(walker, access.value);
    if (tested)
        level = KnowledgeLevel::Proven;
    if (level == KnowledgeLevel::Unknown)
        return buildArrayReadUnderAnyKeyOfTypedElements(access);
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetArrayElement, level);

    // A proven read takes the kind that it names for granted. Numbers are read out of Int32 storage and out of Double storage alike.
    ArrayStorage storage = level == KnowledgeLevel::Expected ? storageDeclaredForTypedElements(access) : ArrayStorage::Other;
    if (storage == ArrayStorage::Int32)
        storage = ArrayStorage::Double;

    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetArrayElement);
    read.setArrayOperand(access.base);
    read.setPositionOperand(access.key);
    read.setArrayStorage(storage);
    read.setAdmitsInt32Storage(storage == ArrayStorage::Double);
    read.setLevel(level);
    // A read that names a kind has the form of that: behind a row in front of the loop, or where the kind is declared.
    if (!tested && !hasArrayStorageForm(storage))
        read.representation = formOfNumberThatLinkHasTypedOfTypedElements(access);
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    read.setIsInBounds(positionIsKnownToLieInsideList(walker, access.base, access.key));
    if (tested)
        nameKindOfListTestedInFrontOfLoop(read, *tested);
    if (tested && read.isInBounds() && holdsOnlyNumbers(read.arrayStorage()) && onlyUserTakesUndefinedForNaNInTypedElements(walker, access.value))
        read.setReadsHoleAsNaN(true);
    if (tested)
        takeStorageAndLengthFromInFrontOfLoop(walker, read, *tested);
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    bool givesNaNForHole = read.readsHoleAsNaN();
    Reference element = walker.build(WTF::move(read));
    // Of a fresh object the state takes a new note for a store, and drops what it holds under every other position.
    // `t += list[i]; if (list[i] === undefined) ..`: what gives NaN for a hole answers its one user alone. Whoever reads the element again reads undefined there.
    if (element && !givesNaNForHole && level == KnowledgeLevel::Proven && isArrayIndexInTypedElements(access.typeOfKey) && !freshObjectNamedInTypedElements(walker, access.base))
        walker.state().noteElement(access.base, access.key, element);
    walker.bind(access.value.id(), element);
    return true;
}

// The twin of buildArrayReadUnderAnyKeyOfTypedElements(). `marks[count] = on`, `count` a variable that closures share: the store asks the key for its tag, replaces an element inside the length by itself,
// and does the closed store, whole and once, for whatever else it meets: an object among the keys is made a key of behind the value, as the closed store does it.
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

static bool buildArrayStoreOfTypedElements(const AccessOfTypedElements& access)
{
    Walker& walker = access.walker;
    KnowledgeLevel level = levelOfArrayAccessOfTypedElements(access);
    if (level == KnowledgeLevel::Unknown)
        return buildArrayStoreUnderAnyKeyOfTypedElements(access);
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::PutArrayElement, level);

    // At either level the store tests the kind that it names before it writes.
    ECMAMode mode = access.value.mode();
    Draft put = draftOfTypedElements(walker, access.value, Opcode::PutArrayElement);
    put.setArrayOperand(access.base);
    put.setPositionOperand(access.key);
    put.setValueOperand(access.stored);
    put.setArrayStorage(storageGuessedForStoreOfTypedElements(access));
    put.setMode(mode);
    put.setLevel(level);
    put.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    // `for (..; i < list.length; ..) list[i] = ..`: the store that tests its own guess looks at no length. A proven store that says so tests nothing at all: storeInsideLength().
    put.setIsInBounds(level == KnowledgeLevel::Expected && positionIsKnownToLieInsideList(walker, access.base, access.key));
    if (level == KnowledgeLevel::Expected)
        markBaseAsPassedInTypedElements(walker, access.base);
    Reference store = walker.build(WTF::move(put));
    // Sloppy code passes by a store that a frozen array refuses.
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

// ---- Typed arrays

static void nameVectorOfViewInTypedElements(Walker& walker, Draft& access, Reference view, TypedArrayType typedArrayType)
{
    if (!access.isInsideView() || !hasInlineElementAccess(typedArrayType) || walker.testsEveryPosition())
        return;
    Draft vector(Opcode::GetTypedArrayVector, access.origin());
    walker.chooseConservativeFields(vector);
    vector.setViewOperand(view);
    if (Reference built = walker.build(WTF::move(vector)))
        access.setVectorOperand(built);
}

// A view answers for every number by itself: what is no index inside it is undefined, whatever the prototypes hold.
static bool buildViewReadOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    if (access.isOnlyAskedAbout)
        return answerAheadOfTypedElements(access, Opcode::GetTypedArrayElement, KnowledgeLevel::Proven);
    Walker& walker = access.walker;
    Draft read = draftOfTypedElements(walker, access.value, Opcode::GetTypedArrayElement);
    read.setViewOperand(access.base);
    read.setPositionOperand(access.key);
    read.setTypedArrayType(typedArrayType);
    // `if (i < view.length) view[i]`: the position is an index below the length that the view has where the walk stands.
    read.setIsInsideView(positionIsKnownToLieInsideList(walker, access.base, access.key));
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(access.typeOfKey.isNumberWithoutFraction());
    nameVectorOfViewInTypedElements(walker, read, access.base, typedArrayType);
    walker.bind(access.value.id(), walker.build(WTF::move(read)));
    return true;
}

// The value is converted before the position is looked at, and an object among the values runs code. Proven: it is what the view holds, so nothing is converted.
// Expected: the operation asks whether it is a number, and does the closed operation where it is none.
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
    nameVectorOfViewInTypedElements(walker, put, access.base, typedArrayType);
    // Nothing is ever noted of the elements of a view: it holds the value as converted, in bytes that another view may share.
    walker.bind(access.value.id(), walker.build(WTF::move(put)));
    return true;
}

// ---- Whatever else is proven to have elements: a string, an arguments object, an array or a view whose own form does not apply

static ExpectedElements elementsExpectedByTypedElements(TypedArrayType typedArrayType)
{
    if (typedArrayType == TypeInt32)
        return ExpectedElements::Int32Array;
    if (typedArrayType == TypeUint8)
        return ExpectedElements::Uint8Array;
    return ExpectedElements::Anything;
}

// The closed operation with the key as an integer in a register. Its stub looks for a character of a string and for an element in storage before it makes a key of the integer.
// Whoever asks ahead asks for a typed form, and this is none.
static bool buildAccessByIndexOfTypedElements(const AccessOfTypedElements& access, TypedArrayType typedArrayType)
{
    if (!access.typeOfKey.isNumberWithoutFraction() || access.isOnlyAskedAbout)
        return false;

    Walker& walker = access.walker;
    Draft draft = walker.draftOf(access.value);
    walker.chooseConservativeFields(draft);
    draft.setKeyIsIntegral(true);
    draft.setExpectedElements(elementsExpectedByTypedElements(typedArrayType));
    markBaseAsPassedInTypedElements(walker, access.base);
    walker.bind(access.value.id(), walker.build(WTF::move(draft)));
    return true;
}

// `arguments[i]` while nothing but this code has the object: the element is what the caller handed over, a parameter or an element of the list behind the parameters. Nothing that is built names the object.
// readLeavesFreshObjectAsItIs() promises a look at a loop whatever is taken here.
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
    // The index may name a parameter: each is asked for by its position, and what is none of them lies in the list.
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

// ---- Lengths

// The length of an array and of a string is a property of their own that nobody can replace, and that of an array is read the same way whatever its storage.
// That of a view is a getter above it, which holds only while the view and what stands above it are pristine. That of an arguments object is a property like any other.
static bool buildLengthOfTypedElements(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.isFreeName() || value.key() != walker.program().keyOf(BuiltinNames::length))
        return false;

    TypeArena& types = walker.types();
    TypeID typeOfBase = inputs.typeOf(value.objectOperand());
    bool isOnlyAskedAbout = inputs.areOnlyTypes();
    Reference base = isOnlyAskedAbout ? Reference() : walker.referenceOf(value.objectOperand());
    // Whoever asks ahead goes on with the type of what would be built: findStretchOfGuess().
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
        // Or the state has seen the view as its class makes it, and nothing replaces the getter.
        bool isSeenToFindGetter = onlyClass && ((!isOnlyAskedAbout && findConstantMemberReadOff(walker, base, value.key())) || readsOffEntryParameterThatStaysAsMade(walker, value));
        if (!onlyClass || (!isSeenToFindGetter && !pristineAspectsProvenBy(builtinContextOfTypedElements(walker), typeOfBase, onlyClass->builtinClass).contains(PristineAspects::Names)))
            return false;
        if (isOnlyAskedAbout)
            return answerAhead(Opcode::GetTypedArrayLength);
        Draft length = draftOfTypedElements(walker, value, Opcode::GetTypedArrayLength);
        length.setViewOperand(base);
        length.setTypedArrayType(classified.typedArrayType);
        Reference built = walker.build(WTF::move(length));
        walker.bind(value.id(), built);
        // Until something may write the storage of views: a buffer that is resized, handed away or detached.
        if (built && built.isValue() && !walker.isUnreachable())
            noteKnown(walker, KnownNote::LengthOfList, base, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
        return true;
    }
    case KindOfBaseOfTypedElements::Arguments:
        // It is the count that the caller handed over, until something stores to the property.
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
