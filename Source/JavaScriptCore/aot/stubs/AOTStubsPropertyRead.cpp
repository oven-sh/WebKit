/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTClassMemberTable.h"
#include "AOTClassPrototype.h"
#include "AOTCompiledStructure.h"
#include "AOTFieldTable.h"
#include "AOTImageText.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubsField.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "JSObject.h"
#include "PropertyTable.h"
#include <wtf/Box.h>

namespace JSC { namespace AOT {

static_assert(slotNameRegister == StubRegister::T9 && slotHeaderRegister == StubRegister::T12 && slotPayloadRegister == StubRegister::T13);
static_assert(!PackedSlot::offsetOfPayload() && PackedSlot::offsetOfLocation() == sizeof(uint32_t) && sizeof(PackedSlot) == sizeof(uint64_t));

static StubSignature signatureReturningValue(std::initializer_list<StubRegister> operands)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    for (StubRegister operand : operands)
        signature.addOperand(gprOf(operand));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfGetById(StubKey key)
{
    StubSignature signature = signatureReturningValue({ });
    signature.addOperand(key.operandRegister().value_or(gprOf(firstStubOperandRegister)));
    signature.boundImmediates = StubBoundImmediates::NamesRead;
    return signature;
}

StubSignature signatureOfGetByIdAfterFrontProbe(StubKey) { return signatureReturningValue({ firstStubOperandRegister, slotHeaderRegister }); }
StubSignature signatureOfGetByIdHashed(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfSilentSlot(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfNarrowCellInExtension(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfEmptySlot(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdBehindFieldTable(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfInById(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }

struct ReadProbeRegisters {
    GPRReg R0;
    GPRReg T9;
    GPRReg T10;
    GPRReg T11;
    GPRReg T12;
    GPRReg T13;
    GPRReg T14;
};

static ReadProbeRegisters allocateReadProbeRegisters(StubAssembler& jit)
{
    return {
        .R0 = jit.operand(0),
        .T9 = jit.immediate(),
        .T10 = jit.allocateScratch(StubRegister::T10),
        .T11 = jit.allocateScratch(StubRegister::T11),
        .T12 = jit.allocateScratch(StubRegister::T12),
        .T13 = jit.allocateScratch(StubRegister::T13),
        .T14 = jit.allocateScratch(StubRegister::T14),
    };
}

static void returnOwnInlineValue(StubAssembler& jit, GPRReg receiver, GPRReg cell)
{
    loadOwnInlineValue(jit, receiver, cell, jit.result());
    jit.returnToCaller();
}

#if !CPU(X86_64)
// Goes on where the filter of the receiver's Structure says that neither the receiver nor anything above it has the name.
static void branchUnlessFilterRulesOut(StubAssembler& jit, GPRReg structureID, GPRReg name, GPRReg filter, GPRReg scratch, JumpList& isUnknown)
{
    RELEASE_ASSERT(noOverlap(structureID, name, filter, scratch, instanceGPR));
    jit.urshift32(structureID, TrustedImm32(StructureRowWords::granuleShift), scratch);
    jit.and32(TrustedImm32(AbsenceFilter::numberOfFilters - 1), scratch);
    jit.lshift32(TrustedImm32(AbsenceFilter::sizeShift), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilters)), filter);
    jit.add64(scratch, filter);
    isUnknown.append(jit.branch32(CCallHelpers::NotEqual, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, structureID)), structureID));
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilterGeneration)), scratch);
    isUnknown.append(jit.branch32(CCallHelpers::NotEqual, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, generation)), scratch));
    jit.urshift32(name, TrustedImm32(AbsenceFilter::bitsOfWordShift), scratch);
    jit.and32(TrustedImm32(AbsenceFilter::numberOfWords - 1), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight, OBJECT_OFFSETOF(AbsenceFilter, names)), scratch);
    // A shift takes the low six bits of its count.
    jit.urshift64(name, scratch);
    isUnknown.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(1)));
}
#endif

#if CPU(X86_64)
static void returnAnswer(StubAssembler& jit, ObjectLookupQuestion question, bool isPresent)
{
    RELEASE_ASSERT(question != ObjectLookupQuestion::Read || !isPresent);
    if (question == ObjectLookupQuestion::Read)
        jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    else
        jit.move(TrustedImm32(isPresent), jit.result());
    jit.returnToCaller();
}

// object has no lookup of its own, and structure is its Structure: what the Structure's property table lists is all that the object has under a name.
// The table is complete, so a name that is not in it is absent, and nothing has to have been asked before.
void findInPropertyTable(StubAssembler& jit, const ObjectLookupRegisters& r, JumpList& isAbsentHere, JumpList& isUnknown)
{
    constexpr unsigned maxProbes = 4;
    constexpr unsigned typeShift = 48;
    static_assert(sizeof(Structure::SeenProperties) == sizeof(uint64_t) && sizeof(CompactPropertyTableEntry) == sizeof(uint64_t) && PropertyTable::EmptyEntryIndex == 0);
    const GPRReg entry = r.structure;
    RELEASE_ASSERT(noOverlap(r.receiver, r.name, r.uid, r.object, r.structure, r.size, r.vector, r.index));

    // The Structure keeps the union of the bits of the names that it has seen, whether or not its table is there.
    jit.load64(Address(r.structure, Structure::seenPropertiesOffset()), r.index);
    jit.and64(r.uid, r.index);
    isAbsentHere.append(jit.branch64(CCallHelpers::NotEqual, r.index, r.uid));

    jit.loadPtr(Address(r.structure, Structure::propertyTableUnsafeOffset()), r.size);
    isUnknown.append(jit.branchTestPtr(CCallHelpers::Zero, r.size));
    jit.loadPtr(Address(r.size, PropertyTable::offsetOfIndexVector()), r.vector);
    isUnknown.append(jit.branchTestPtr(CCallHelpers::Zero, r.vector, TrustedImm32(PropertyTable::flagOfCompactIndexVector)));
    jit.load32(Address(r.size, PropertyTable::offsetOfIndexSize()), r.size);
    jit.load32(Address(r.uid, StringImpl::flagsOffset()), r.index);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), r.index);

    // PropertyTable::findImpl(). The address in vector is one too high, by the flag. The entries lie behind the index, and their numbers begin at one.
    JumpList isFound;
    for (unsigned probe = 0; probe < maxProbes; ++probe) {
        if (probe)
            jit.add32(TrustedImm32(probe), r.index);
        jit.add32(TrustedImm32(-1), r.size, entry);
        jit.and32(entry, r.index);
        jit.load8(BaseIndex(r.vector, r.index, CCallHelpers::TimesOne, -1), entry);
        isAbsentHere.append(jit.branchTest32(CCallHelpers::Zero, entry));
        jit.getEffectiveAddress(BaseIndex(r.vector, entry, CCallHelpers::TimesEight, -1 - static_cast<int32_t>(sizeof(CompactPropertyTableEntry))), entry);
        jit.load64(BaseIndex(entry, r.size, CCallHelpers::TimesOne), entry);
        // The name is in the low 48 bits. Above it: the offset, then the attributes.
        jit.xor64(r.uid, entry);
        jit.rotateRight64(TrustedImm32(typeShift), entry);
        isFound.append(jit.branch64(CCallHelpers::BelowOrEqual, entry, TrustedImm32(UINT16_MAX)));
    }
    isUnknown.append(jit.jump());

    isFound.link(&jit);
}

// A read returns the value of a data property and what a getter answers.
// The call of a getter takes the register of the uid. beforeGetter runs while it still has the uid, and cannotCallGetter is left for behind it.
template<typename Functor>
static void answerByPropertyTableAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, JumpList& isAbsentHere, JumpList& isUnknown, JumpList& cannotCallGetter, const Functor& beforeGetter)
{
    constexpr int32_t accessorOrCustom = static_cast<int32_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue);
    constexpr int32_t custom = static_cast<int32_t>(PropertyAttribute::CustomAccessorOrValue);
    static_assert(accessorOrCustom <= UINT8_MAX);
    const GPRReg entry = r.structure;

    findInPropertyTable(jit, r, isAbsentHere, isUnknown);
    if (question != ObjectLookupQuestion::Read) {
        returnAnswer(jit, question, true);
        return;
    }
    auto loadProperty = [&](GPRReg result) {
        jit.urshift32(TrustedImm32(8), entry);
        Jump isOutOfLine = jit.branch32(CCallHelpers::AboveOrEqual, entry, TrustedImm32(firstOutOfLineOffset));
        jit.load64(BaseIndex(r.object, entry, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()), result);
        Jump isLoaded = jit.jump();
        isOutOfLine.link(&jit);
        jit.loadPtr(Address(r.object, JSObject::butterflyOffset()), r.vector);
        jit.neg64(entry);
        jit.load64(BaseIndex(r.vector, entry, CCallHelpers::TimesEight, (firstOutOfLineOffset - 2) * static_cast<int32_t>(sizeof(EncodedJSValue))), result);
        isLoaded.link(&jit);
    };
    Jump isNoValue = jit.branchTest32(CCallHelpers::NonZero, entry, TrustedImm32(accessorOrCustom));
    loadProperty(jit.result());
    jit.returnToCaller();

    isNoValue.link(&jit);
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, entry, TrustedImm32(custom)));
    beforeGetter();
    const GPRReg getterSetter = r.uid;
    loadProperty(getterSetter);
    callGetterAndReturn(jit, r.receiver, getterSetter, cannotCallGetter);
}

// Leaves the number of the name in r.name. isNoNameOfImage: the image has no such name, so nothing that it declares has it. hasNoColumn: the tables of the image say nothing about the name.
static void loadNumberOfName(StubAssembler& jit, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isNoNameOfImage, JumpList& hasNoColumn)
{
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    static_assert(sizeof(UniquedStringImpl) == 24);
#endif
    switch (name) {
    case ObjectLookupName::IsGivenByNumber:
        hasNoColumn.append(jit.branch32(CCallHelpers::Above, r.name, TrustedImm32(FieldTableEntry::maxName)));
        return;
    case ObjectLookupName::IsGivenByUID:
        // The names of the image are atoms that lie in the order of their numbers. Text made at run time that equals one of them is that atom.
        jit.move(r.uid, r.name);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        jit.sub64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaders)), r.name);
        isNoNameOfImage.append(jit.branch64(CCallHelpers::AboveOrEqual, r.name, Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeadersCapacity))));
        jit.load32(Address(r.uid, OBJECT_OFFSETOF(LazyStringHeader, number)), r.name);
        hasNoColumn.append(jit.branch32(CCallHelpers::Above, r.name, TrustedImm32(FieldTableEntry::maxName)));
#else
        jit.sub64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), r.name);
        isNoNameOfImage.append(jit.branch64(CCallHelpers::AboveOrEqual, r.name, Address(instanceGPR, AOT_INSTANCE_OFFSET(image.sizeOfProgramIdentifiers))));
        hasNoColumn.append(jit.branch64(CCallHelpers::Above, r.name, TrustedImm32(FieldTableEntry::maxName * sizeof(UniquedStringImpl))));
        jit.urshift32(TrustedImm32(3), r.name);
        jit.mul32(TrustedImm32(static_cast<int32_t>(0xaaaaaaabu)), r.name, r.name);
#endif
        isNoNameOfImage.append(jit.branchTest32(CCallHelpers::Zero, r.name));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// object stands for the prototype object of a class, which is as the class declares it for as long as it does. The row of the class is complete. Goes on where the class declares no such member.
static void answerByClassMembersAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isUnknown)
{
    const GPRReg entry = r.size;
    loadStructure(jit, r.object, r.structure);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.infoOfClassPrototypes)), r.index);
    isUnknown.append(jit.branchPtr(CCallHelpers::NotEqual, Address(r.structure, Structure::classInfoOffset()), r.index));
    // The closures of the members are those of the Instance of its realm.
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), r.index);
    isUnknown.append(jit.branchPtr(CCallHelpers::NotEqual, Address(r.structure, Structure::realmOffset()), r.index));
    isUnknown.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(r.object, JSObject::butterflyOffset())));
    // `constructor` is a member of every class, whether or not the image names it.
    loadNumberOfName(jit, r, name, isUnknown, isUnknown);
    jit.load32(Address(r.object, ClassPrototype::offsetOfMemberRow()), r.index);
    isUnknown.append(jit.branch32(CCallHelpers::Equal, r.index, TrustedImm32(static_cast<int32_t>(ClassRowEntry::noRow))));
    jit.add32(r.name, r.index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), r.vector);
    jit.load32(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour), entry);
    jit.urshift32(entry, TrustedImm32(FieldTableEntry::nameShift), r.vector);
    Jump isNotDeclared = jit.branch32(CCallHelpers::NotEqual, r.vector, r.name);
    if (question != ObjectLookupQuestion::Read)
        returnAnswer(jit, question, true);
    else {
        RELEASE_ASSERT(!jit.options().useWideReferences);
        static_assert(!static_cast<unsigned>(ClassRowEntry::Form::Method) && static_cast<unsigned>(ClassRowEntry::Form::Accessor) == 1);
        const GPRReg number = r.index;
        const GPRReg cell = r.vector;
        const GPRReg getter = r.structure;
        auto loadClosure = [&](GPRReg result) {
            // ClosureTable::closureIfCreated().
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureCells)), cell);
            jit.load32(BaseIndex(cell, number, CCallHelpers::TimesFour), cell);
            isUnknown.append(jit.branchTest32(CCallHelpers::Zero, cell));
            jit.add32(TrustedImm32(1), cell);
            decodeCell32(jit, cell, result);
        };
        // ClassRowEntry::numberIn().
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), r.vector);
        jit.load16(BaseIndex(r.vector, r.index, CCallHelpers::TimesTwo), number);
        jit.lshift32(TrustedImm32(FieldTableEntry::operandBits), number);
        jit.and32(TrustedImm32(FieldTableEntry::maxFormAndOperand), entry);
        Jump isAccessor = jit.branch32(CCallHelpers::AboveOrEqual, entry, TrustedImm32(static_cast<int32_t>(ClassRowEntry::Form::Accessor) << FieldTableEntry::formShift));
        jit.and32(TrustedImm32(FieldTableEntry::maxOperand), entry);
        jit.or32(entry, number);
        loadClosure(jit.result());
        jit.returnToCaller();

        isAccessor.link(&jit);
        jit.and32(TrustedImm32(FieldTableEntry::maxOperand), entry);
        jit.or32(entry, number);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.classAccessors)), r.vector);
        jit.load32(BaseIndex(r.vector, number, CCallHelpers::TimesEight, OBJECT_OFFSETOF(ClassAccessorData, getter)), number);
        Jump hasNoGetter = jit.branchTest32(CCallHelpers::Zero, number);
        loadClosure(getter);
        callGetterFunctionAndReturn(jit, r.receiver, getter, isUnknown);

        hasNoGetter.link(&jit);
        returnAnswer(jit, question, false);
    }
    isNotDeclared.link(&jit);
}

void locateFieldOfObject(StubAssembler& jit, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& hasNoField, JumpList& isUnknown)
{
    static_assert(sizeof(FieldTableEntry) == 1u << CCallHelpers::TimesFour && FieldTableEntry::nameShift == 16);
    loadNumberOfName(jit, r, name, hasNoField, isUnknown);
    jit.load32SignedExtendTo64(Address(r.object, offsetOfShapeWord), r.index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), r.vector);
    jit.load32(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour), r.index);
    jit.add32(r.name, r.index);
    jit.load16(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour, FieldTableEntry::nameShift / 8), r.vector);
    hasNoField.append(jit.branch32(CCallHelpers::NotEqual, r.vector, r.name));
}

// A field of a compiled object in whatever form its slot has, by the record of the field. Goes on where the object has no property of the name in a slot.
static void answerByFieldRecordAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isUnknown)
{
    constexpr unsigned fieldSizeShift = WTF::fastLog2(static_cast<unsigned>(sizeof(FieldData)));
    static_assert(sizeof(FieldData) == 1u << fieldSizeShift && sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo);
    const GPRReg flags = r.size;
    JumpList hasNoField;
    auto inField = [&](ptrdiff_t offset) {
        return Address(r.index, static_cast<int32_t>(sizeof(LayoutData) + offset));
    };

    locateFieldOfObject(jit, r, name, hasNoField, isUnknown);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), r.vector);
    jit.load16(BaseIndex(r.vector, r.index, CCallHelpers::TimesTwo), r.index);
    jit.and32(TrustedImm32(FieldRecordIndex::indexMask), r.index);
    jit.lshift32(TrustedImm32(fieldSizeShift), r.index);
    loadStructure(jit, r.object, r.structure);
    jit.loadPtr(Address(r.structure, CompiledStructure::offsetOfLayout()), r.vector);
    jit.addPtr(r.vector, r.index);
    jit.load16(inField(OBJECT_OFFSETOF(FieldData, flags)), flags);
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldIsPrivate)));

    const FieldReadRegisters registers {
        .field = {
            .object = r.object,
            .slotData = r.uid,
            .extensionWordOffset = r.structure,
            .address = r.structure,
            .representation = r.vector,
        },
        .value = r.uid,
        .result = jit.result(),
        .number = jit.allocateScratch(FPRInfo::fpRegT0),
        .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
    };
    FieldReadExits exits;
    jit.load32(inField(OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
    Jump isOutOfLine = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldIsOutOfLine));
    computeFieldAddress(jit, registers.field, false, exits.isAbsent);
    Label hasAddress = jit.label();
    if (question == ObjectLookupQuestion::Read) {
        Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldIsAccessor));
        readFieldAndReturn(jit, registers, exits);

        isAccessor.link(&jit);
        const GPRReg getterSetter = r.uid;
        loadGetterSetterInField(jit, registers.field.address, registers.field.representation, getterSetter, exits.isAbsent);
        callGetterAndReturn(jit, r.receiver, getterSetter, isUnknown);
    } else {
        JumpList isPresent;
        branchOnFieldPresence(jit, registers.field, registers.value, isPresent, exits.isAbsent);
        isPresent.link(&jit);
        returnAnswer(jit, question, true);
    }

    isOutOfLine.link(&jit);
    jit.load32(Address(r.structure, CompiledStructure::offsetOfExtensionSlot()), registers.field.extensionWordOffset);
    jit.and32(TrustedImm32(SlotData::maxOffset), registers.field.extensionWordOffset);
    computeFieldAddress(jit, registers.field, true, exits.isAbsent);
    jit.jump().linkTo(hasAddress, &jit);

    isUnknown.append(exits.cannot);
    exits.isAbsent.link(&jit);
    // Only the key order says whether such a field exists where its slot is empty.
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldMayExistWithoutStorage)));
    loadIdentifier(jit, r.name, r.uid, isUnknown);
    hasNoField.link(&jit);
}

void answerFromObjectsAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isUnknown)
{
    constexpr int32_t flagsOfOwnLookup = OverridesGetOwnPropertySlot | OverridesGetPrototype | HasStaticPropertyTable;
    static_assert(flagsOfOwnLookup <= UINT8_MAX);
    JumpList isAbsentHere;
    JumpList isAbsentFromExtraProperties;
    JumpList extraPropertiesDoNotSay;
    const GPRReg objectWithExtraProperties = r.name;
    // The name is no symbol, so its number is its place among the atoms of the image.
    auto restoreName = [&] {
        if (name != ObjectLookupName::IsGivenByNumber)
            return;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        jit.load32(Address(r.uid, OBJECT_OFFSETOF(LazyStringHeader, number)), r.name);
#else
        static_assert(sizeof(UniquedStringImpl) == 24);
        jit.move(r.uid, r.name);
        jit.sub64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), r.name);
        jit.urshift32(TrustedImm32(3), r.name);
        jit.mul32(TrustedImm32(static_cast<int32_t>(0xaaaaaaabu)), r.name, r.name);
#endif
    };

    isUnknown.append(jit.branchIfNotCell(r.receiver));
    isUnknown.append(jit.branchIfNotObject(r.receiver));
    // A symbol has its hash elsewhere.
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsSymbol())));
    // The empty name is the one name of the image whose uid is no atom of the image, so neither can be had from the other.
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, Address(r.uid, StringImpl::lengthMemoryOffset())));
    jit.move(r.receiver, r.object);

    Label askObject = jit.label();
    Jump hasOwnLookup = jit.branchTest8(CCallHelpers::NonZero, Address(r.object, JSCell::typeInfoFlagsOffset()), TrustedImm32(flagsOfOwnLookup));
    // A name may spell an index, and an element is in no property table.
    isUnknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(r.object, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
    loadStructure(jit, r.object, r.structure);
    answerByPropertyTableAndReturn(jit, question, r, isAbsentHere, isUnknown, isUnknown, [] { });

    isAbsentHere.link(&jit);
    Label askObjectAbove = jit.label();
    if (question == ObjectLookupQuestion::HasOwn)
        returnAnswer(jit, question, false);
    else {
        loadStructure(jit, r.object, r.structure);
        jit.load64(Address(r.structure, Structure::prototypeOffset()), r.object);
        // Empty where each object has its prototype in a slot of its own.
        isUnknown.append(jit.branchTest64(CCallHelpers::Zero, r.object));
        jit.branchIfCell(r.object).linkTo(askObject, &jit);
        returnAnswer(jit, question, false);
    }

    hasOwnLookup.link(&jit);
    Jump isNotCompiledObject = jit.branch8(CCallHelpers::NotEqual, Address(r.object, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledObjectType));
    answerByFieldRecordAndReturn(jit, question, r, name, isUnknown);
    jit.branchTest32(CCallHelpers::Zero, Address(r.object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)).linkTo(askObjectAbove, &jit);
    loadStructure(jit, r.object, r.structure);
    jit.load32(Address(r.structure, CompiledStructure::offsetOfExtraPropertiesSlot()), r.index);
    jit.and32(TrustedImm32(SlotData::maxOffset), r.index);
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, r.index));
    // The search takes the register of the object. The number of the name can be had again.
    jit.move(r.object, objectWithExtraProperties);
    loadExtraProperties(jit, objectWithExtraProperties, r.index, r.object, extraPropertiesDoNotSay);
    // In their place may be the record of what is still to be made.
    extraPropertiesDoNotSay.append(jit.branch8(CCallHelpers::NotEqual, Address(r.object, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));
    extraPropertiesDoNotSay.append(jit.branchTest8(CCallHelpers::NonZero, Address(r.object, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
    loadStructure(jit, r.object, r.structure);
    answerByPropertyTableAndReturn(jit, question, r, isAbsentFromExtraProperties, extraPropertiesDoNotSay, isUnknown, restoreName);

    isAbsentFromExtraProperties.link(&jit);
    jit.move(objectWithExtraProperties, r.object);
    restoreName();
    jit.jump().linkTo(askObjectAbove, &jit);

    extraPropertiesDoNotSay.link(&jit);
    restoreName();
    isUnknown.append(jit.jump());

    isNotCompiledObject.link(&jit);
    if (jit.options().useWideReferences) {
        isUnknown.append(jit.jump());
        return;
    }
    answerByClassMembersAndReturn(jit, question, r, name, isUnknown);
    jit.jump().linkTo(askObjectAbove, &jit);
}

void replaceOwnValueAndReturn(StubAssembler& jit, SlotOperation operation, const ObjectLookupRegisters& r, ObjectLookupName name, GPRReg value, JumpList& cannot)
{
    constexpr uint32_t bitsThatForbid = Structure::s_hasImmutablePropertiesBits | Structure::s_hasListedImmutablePropertiesBits | Structure::s_isWatchingReplacementBits;
    // A definition gives the property the attributes of a plain one, so it replaces the value alone only where the property has them.
    const int32_t attributesThatForbid = operation == SlotOperation::Define ? UINT8_MAX : static_cast<int32_t>(PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
    const GPRReg owner = r.object;
    const GPRReg offset = r.structure;
    const GPRReg storage = r.vector;
    RELEASE_ASSERT(attributesThatForbid <= UINT8_MAX && noOverlap(value, r.receiver, r.name, r.uid, r.object, r.structure, r.size, r.vector, r.index));

    // A symbol has its hash elsewhere.
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsSymbol())));
    // The uid of the empty name is no atom of the image, so it does not say whether a layout has a field of that name.
    cannot.append(jit.branchTest32(CCallHelpers::Zero, Address(r.uid, StringImpl::lengthMemoryOffset())));
    jit.move(r.receiver, owner);
    Jump isOrdinary = jit.branch8(CCallHelpers::Equal, Address(owner, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType));

    // Object::tryStoreExtraProperty(): what a compiled object has beside its fields lies in an ordinary object of its own.
    JumpList hasNoField;
    cannot.append(jit.branch8(CCallHelpers::NotEqual, Address(owner, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledObjectType)));
    cannot.append(jit.branchTest32(CCallHelpers::Zero, Address(owner, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    locateFieldOfObject(jit, r, name, hasNoField, cannot);
    cannot.append(jit.jump());
    hasNoField.link(&jit);
    loadStructure(jit, owner, r.structure);
    jit.load32(Address(r.structure, CompiledStructure::offsetOfExtraPropertiesSlot()), r.index);
    jit.and32(TrustedImm32(SlotData::maxOffset), r.index);
    cannot.append(jit.branchTest32(CCallHelpers::Zero, r.index));
    loadExtraProperties(jit, r.receiver, r.index, owner, cannot);
    // In their place may be the record of what is still to be made.
    cannot.append(jit.branch8(CCallHelpers::NotEqual, Address(owner, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));

    isOrdinary.link(&jit);
    loadStructure(jit, owner, r.structure);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.structure, Structure::bitFieldOffset()), TrustedImm32(static_cast<int32_t>(bitsThatForbid))));
    findInPropertyTable(jit, r, cannot, cannot);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, offset, TrustedImm32(attributesThatForbid)));
    jit.urshift32(TrustedImm32(8), offset);
    Jump isOutOfLine = jit.branch32(CCallHelpers::AboveOrEqual, offset, TrustedImm32(firstOutOfLineOffset));
    jit.store64(value, BaseIndex(owner, offset, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    returnAfterStoring(jit, owner, value);

    isOutOfLine.link(&jit);
    jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
    jit.neg64(offset);
    jit.store64(value, BaseIndex(storage, offset, CCallHelpers::TimesEight, (firstOutOfLineOffset - 2) * static_cast<int32_t>(sizeof(EncodedJSValue))));
    returnAfterStoring(jit, owner, value);
}

void countMissOfPair(StubAssembler& jit, GPRReg structureID, GPRReg uid, GPRReg counter, JumpList& isDueForFiling)
{
    static_assert(hasOneBitSet(MissesOfPairs::numberOfCounters) && hasOneBitSet(MissesOfPairs::missesBetweenFilings) && MissesOfPairs::missesBetweenFilings <= UINT8_MAX + 1);
    RELEASE_ASSERT(noOverlap(structureID, uid, counter, instanceGPR));
    jit.mul32(TrustedImm32(static_cast<int32_t>(HashedSlot::structureMultiplier)), structureID, counter);
    jit.urshift32(TrustedImm32(HashedSlot::structureHashShift), counter);
    jit.x86Lea32(BaseIndex(counter, uid, static_cast<CCallHelpers::Scale>(HashedSlot::uidHashShift)), counter);
    jit.urshift32(TrustedImm32(HashedSlot::sizeShift), counter);
    jit.and32(TrustedImm32(MissesOfPairs::numberOfCounters - 1), counter);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.missesOfPairs)), counter);
    jit.add8(TrustedImm32(1), Address(counter));
    Jump isNotDue = jit.branchTest8(CCallHelpers::NonZero, Address(counter), TrustedImm32(MissesOfPairs::missesBetweenFilings - 1));
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.missIsDueForFiling)));
    isDueForFiling.append(jit.jump());
    isNotDue.link(&jit);
}
#endif

struct GetByIdBody {
    StubAssembler& jit;
    ReadProbeRegisters registers;
    CallerFrame callerFrame;
    SlotHandlerTable packedTable;
    SlotHandlerTable unpackedTable;
    Box<JumpList> miss;
    // Where the receiver is a cell that the table of fields does not answer for.
    Label behindFieldTable { };
};

static JumpList emitFrontProbe(GetByIdBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.registers.R0;
    const GPRReg name = body.registers.T9;
    const GPRReg T10 = body.registers.T10;
    const GPRReg T11 = body.registers.T11;
    const GPRReg cell = body.registers.T12;
    const GPRReg T13 = body.registers.T13;
    loadStructureIDOfReceiver(jit, R0, T11, *body.miss, [&] {
        JumpList isNotInFieldTable;
        returnFieldByTable(jit, { R0, name, T11, cell, std::nullopt, true, true, false, false, body.callerFrame }, isNotInFieldTable);
        isNotInFieldTable.link(&jit);
        body.behindFieldTable = jit.label();
        jit.bindEntry(getByIdBehindFieldTable().withCallerFrame(body.callerFrame));
    });
    JumpList hasOtherName;
#if CPU(X86_64)
    UNUSED_PARAM(T13);
    locateFrontCell(jit, SlotAccess::Read, T11, name, T10);
    returnByFrontCell(jit, R0, name, T10, T11, cell, hasOtherName);
    jumpToSlotHandler(jit, body.packedTable, cell, T10);
#else
    loadFrontCell(jit, SlotAccess::Read, { T11, name, T13, cell, T10 });
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, name, cell, T10);
    returnOwnInlineValue(jit, R0, cell);

    isNotOwnInlineValue.link(&jit);
#endif
    jit.bindEntry(getByIdAfterFrontProbe().withCallerFrame(body.callerFrame));
    hasOtherName.append(branchIfCellNameDiffers(jit, name, cell, T10));
#if CPU(ARM64)
    Jump isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::Value, T10);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), R0, T13, T11, R0);
    jit.returnToCaller();

    isNotValue.link(&jit);
#endif
    jumpToSlotHandler(jit, body.packedTable, cell, T10);
    return hasOtherName;
}

static void emitHashedProbe(GetByIdBody& body, JumpList hasOtherNameInFront)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.registers.R0;
    const GPRReg name = body.registers.T9;
    const GPRReg uid = body.registers.T10;
    const GPRReg structureID = body.registers.T11;
    const GPRReg header = body.registers.T12;
    const GPRReg payload = body.registers.T13;
    const GPRReg entry = body.registers.T14;
    hasOtherNameInFront.link(&jit);
    jit.bindEntry(getByIdHashed().withCallerFrame(body.callerFrame));
    loadStructureIDOfReceiver(jit, R0, structureID, *body.miss);
    loadIdentifier(jit, name, uid, *body.miss);
    JumpList isInNoTable;
    findHashedSlot(jit, SlotAccess::Read, { structureID, uid, entry, header, payload, payload }, isInNoTable);
    jumpToSlotHandler(jit, body.unpackedTable, header, uid);

    // No table has the pair: mostly because it is asked for the first time. What follows costs a read that hits a table nothing.
    isInNoTable.link(&jit);
#if CPU(X86_64)
    // What answers from here on files nothing, so a pair that comes again and again has to leave for the operation that files: one miss in MissesOfPairs::missesBetweenFilings.
    constexpr bool leavesNowAndThen = false;
    countMissOfPair(jit, structureID, uid, header, *body.miss);
#else
    constexpr bool leavesNowAndThen = true;
#endif
    JumpList isNotAnsweredByLayout;
    returnFieldByTable(jit, { R0, name, header, payload, std::nullopt, false, false, true, leavesNowAndThen }, isNotAnsweredByLayout);
    isNotAnsweredByLayout.link(&jit);
#if !CPU(X86_64)
    const GPRReg filter = header;
    branchUnlessFilterRulesOut(jit, structureID, name, filter, payload, *body.miss);
    static_assert(hasOneBitSet(AbsenceFilter::readsBetweenMisses) && leavesNowAndThen);
    jit.add32(TrustedImm32(1), Address(filter, OBJECT_OFFSETOF(AbsenceFilter, readsAnswered)));
    body.miss->append(jit.branchTest32(CCallHelpers::Zero, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, readsAnswered)), TrustedImm32(AbsenceFilter::readsBetweenMisses - 1)));
    jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
    jit.returnToCaller();
#else
    // Each object says by the bits of the names that its Structure has seen, or by its row, that it has no such name: that costs less than a filter has to be filled for.
    answerFromObjectsAndReturn(jit, ObjectLookupQuestion::Read, { R0, name, uid, header, structureID, payload, entry, jit.scratchRegister() }, ObjectLookupName::IsGivenByNumber, *body.miss);
#endif
}

#if CPU(X86_64)
static void emitAlternateReceiverProbe(StubAssembler& jit)
{
    const CallerFrame callerFrame = jit.key().callerFrame();
    const GPRReg receiver = jit.operand(0);
    const GPRReg R0 = jit.result();
    const GPRReg name = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::A1);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    const GPRReg T12 = receiver == gprOf(StubRegister::T12) ? jit.clobberOperand(0) : jit.allocateScratch(StubRegister::T12);
    const GPRReg cell = T11;
    const SlotOrigin origin = callerFrame == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    RELEASE_ASSERT(noOverlap(receiver, name, scratch, T11, row));
    Jump isNotCell = jit.branchIfNotCell(receiver);
    JumpList isNotInFieldTable;
    JumpList hasOtherName;
    returnFieldByTable(jit, { receiver, name, row, cell, std::nullopt, true, true, false, false, callerFrame }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    jit.load32(Address(receiver, JSCell::structureIDOffset()), cell);
    locateFrontCell(jit, SlotAccess::Read, cell, name, scratch);
    returnByFrontCell(jit, receiver, name, scratch, cell, row, hasOtherName);
    jit.move(receiver, R0);
    jit.move(row, T12);
    jumpToSlotHandler(jit, slotHandlerTable(SlotOperation::Read, origin, SlotForm::PackedSlot), T12, scratch);

    hasOtherName.link(&jit);
    jit.move(receiver, R0);
    jit.jumpToStub(getByIdHashed().withCallerFrame(callerFrame));

    isNotCell.link(&jit);
    jit.move(receiver, R0);
    jit.jumpToStub(getById().withCallerFrame(callerFrame));
}
#endif

static void emitBoundNameProbe(StubAssembler& jit, uint32_t number)
{
    RELEASE_ASSERT(RowDisplacementTable::hasColumn(number));
    const CallerFrame callerFrame = jit.key().callerFrame();
    const GPRReg R0 = jit.operand(0);
    const GPRReg T9 = jit.reuseScratch(StubRegister::T9);
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T12 = jit.allocateScratch(StubRegister::T12);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    auto inCell = [&](ptrdiff_t offset) { return Address(row, static_cast<int32_t>(number * sizeof(PackedSlot) + offset)); };
    JumpList isNotInFieldTable;
#if CPU(X86_64)
    const uint32_t upperHalf = PackedSlot::upperHalfForOwnInlineValue(static_cast<uint16_t>(number));
    Jump isNotCell = jit.branchIfNotCell(R0);
    returnFieldByTable(jit, { R0, InvalidGPRReg, T11, T12, number, true, true, false, false, callerFrame }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    loadFrontRowOfCell(jit, SlotAccess::Read, R0, row, T10);
    Jump isNotOwnInlineValue = jit.branch32(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), TrustedImm32(static_cast<int32_t>(upperHalf)));
    jit.load32(inCell(PackedSlot::offsetOfPayload()), row);
    jit.load64(BaseIndex(R0, row, CCallHelpers::TimesEight), R0);
    jit.returnToCaller();

    isNotOwnInlineValue.link(&jit);
    constexpr uint32_t biasedBitsOfAbsent = static_cast<uint8_t>(Slot::Kind::Absent) ^ PackedSlot::ownInlineValueBits;
    Jump isNotAbsent = jit.branch32(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), TrustedImm32(static_cast<int32_t>(upperHalf | biasedBitsOfAbsent << 8)));
    jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
    jit.returnToCaller();

    isNotAbsent.link(&jit);
    jit.load64(inCell(0), T12);
    jit.move(TrustedImm32(number), T9);
    jit.jumpToStub(getByIdAfterFrontProbe().withCallerFrame(callerFrame));

    isNotCell.link(&jit);
    jit.move(TrustedImm32(number), T9);
    jit.jumpToStub(getById().withCallerFrame(callerFrame));
#else
    returnFieldByTable(jit, { R0, InvalidGPRReg, T11, T12, number }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    jit.move(TrustedImm32(number), T9);
    jit.linkToStub(jit.branchIfNotCell(R0), getById().withCallerFrame(callerFrame));
    jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
    loadFrontRow(jit, SlotAccess::Read, T11, row, T10);
    jit.load64(inCell(0), T12);
    jit.linkToStub(branchUnlessOwnInlineValue(jit, T9, T12, T10), getByIdAfterFrontProbe().withCallerFrame(callerFrame));
    returnOwnInlineValue(jit, R0, T12);
#endif
}

static bool tryEmitSpecializedProbe(StubAssembler& jit)
{
    if (std::optional<uint32_t> number = jit.key().immediate()) {
        emitBoundNameProbe(jit, *number);
        return true;
    }
#if CPU(X86_64)
    if (jit.key().operandRegister()) {
        emitAlternateReceiverProbe(jit);
        return true;
    }
#endif
    return false;
}

void generateGetById(StubAssembler& jit)
{
    if (tryEmitSpecializedProbe(jit))
        return;
    const CallerFrame callerFrame = jit.key().callerFrame();
    const SlotOrigin origin = callerFrame == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    GetByIdBody body {
        jit,
        allocateReadProbeRegisters(jit),
        callerFrame,
        slotHandlerTable(SlotOperation::Read, origin, SlotForm::PackedSlot),
        slotHandlerTable(SlotOperation::Read, origin, SlotForm::Slot),
        Box<JumpList>::create(),
    };
    JumpList hasOtherNameInFront = emitFrontProbe(body);
    emitHashedProbe(body, hasOtherNameInFront);
    JumpList isNotAnswered;
    answerMissingField(jit, callerFrame, body.registers.R0, body.registers.T9, body.registers.T10, body.registers.T11, body.registers.T12, isNotAnswered);
    isNotAnswered.linkTo(body.behindFieldTable, &jit);
    // loadStructureIDOfReceiver adds to the list in an out-of-line part, which runs after this function has returned.
    jit.addOutOfLinePart([miss = body.miss, table = body.unpackedTable](StubAssembler& jit) {
        linkToSlotExit(jit, *miss, table, SlotExit::Miss);
    });
}

static void emitPresenceBySlot(StubAssembler& jit, PresenceQuestion question, const ProbedSlot& probed, const ReadProbeRegisters& registers, JumpList& isUnknown, JumpList& miss)
{
    const GPRReg R0 = registers.R0;
    Jump isNotField = branchUnlessSlotNamesField(jit, probed, registers.T10);
    returnFieldPresence(jit, CachedSlot(probed.form, Slot::Kind::Field, probed.header, probed.payload), { R0, registers.T10, registers.T11, registers.T11, registers.T14 }, miss);

    isNotField.link(&jit);
    returnPresenceBySlot(jit, question, probed, R0, isUnknown);
}

static Entry cachingOperationFor(PresenceQuestion question)
{
    switch (question) {
    case PresenceQuestion::In:
        return Entry::operationAOTInByIdAndCache;
    case PresenceQuestion::HasOwn:
        return Entry::operationAOTHasOwnByIdAndCache;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateInById(StubAssembler& jit)
{
    const PresenceQuestion question = static_cast<PresenceQuestion>(jit.key().form());
    const ReadProbeRegisters registers = allocateReadProbeRegisters(jit);
    const GPRReg R0 = registers.R0;
    const GPRReg name = registers.T9;
    const GPRReg T10 = registers.T10;
    const GPRReg structureID = registers.T11;
    const GPRReg T12 = registers.T12;
    const GPRReg T13 = registers.T13;
    const GPRReg T14 = registers.T14;
    JumpList isNotInFront;
    JumpList miss;
#if !CPU(X86_64)
    JumpList isNotRuledOut;
#endif
    JumpList isNotInFieldTable;
    miss.append(jit.branchIfNotCell(R0));
    auto emitReturnFalse = [&] {
        jit.move(TrustedImm32(0), R0);
        jit.returnToCaller();
    };
    const ScopedLambda<void()> returnFalse = emitReturnFalse;
    readFieldByTable(jit, { R0, name, T12, T13, std::nullopt, false, true, true }, T12, isNotInFieldTable, [&] {
        jit.move(TrustedImm32(1), R0);
        jit.returnToCaller();
    }, &returnFalse);
    isNotInFieldTable.link(&jit);
    miss.append(jit.branchIfNotObject(R0));
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
#if !CPU(X86_64)
    // What the filter answers never leaves for the operation that files, so no table would ever hold it: the filter is asked first.
    branchUnlessFilterRulesOut(jit, structureID, name, T12, T13, isNotRuledOut);
    jit.move(TrustedImm32(0), R0);
    jit.returnToCaller();

    isNotRuledOut.link(&jit);
#endif
    isNotInFront.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    loadFrontCell(jit, SlotAccess::Read, { structureID, name, T13, T12, T10 });
    isNotInFront.append(branchIfCellNameDiffers(jit, name, T12, T10));
    // A reserved cell has the name and no slot, and a slot that does not pack is in the hashed table.
    emitPresenceBySlot(jit, question, { SlotForm::PackedSlot, T12 }, registers, isNotInFront, miss);

    isNotInFront.link(&jit);
    loadIdentifier(jit, name, T10, miss);
#if CPU(X86_64)
    JumpList isInNoTable;
#else
    JumpList& isInNoTable = miss;
#endif
    findHashedSlot(jit, SlotAccess::Read, { structureID, T10, T14, T12, T13, T13 }, isInNoTable);
    emitPresenceBySlot(jit, question, { SlotForm::Slot, T12, T13 }, registers, miss, miss);
#if CPU(X86_64)

    isInNoTable.link(&jit);
    countMissOfPair(jit, structureID, T10, T12, miss);
    answerFromObjectsAndReturn(jit, question == PresenceQuestion::In ? ObjectLookupQuestion::In : ObjectLookupQuestion::HasOwn, { R0, name, T10, T12, structureID, T13, T14, jit.scratchRegister() }, ObjectLookupName::IsGivenByNumber, miss);
#endif

    miss.link(&jit);
    jit.exitOnMiss(cachingOperationFor(question), { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = R0, .thirdArgumentIn = name });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
