/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClassMemberTable.h"
#include "AOTClassPrototype.h"
#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTFieldTable.h"
#include "AOTImageText.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsField.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "JSObject.h"
#include "PropertyTable.h"
#include <bit>
#include <wtf/Box.h>

namespace JSC { namespace AOT {

static_assert(slotNameRegister == StubRegister::T9 && slotHeaderRegister == StubRegister::T12 && slotPayloadRegister == StubRegister::T13);
static_assert(!PackedSlot::offsetOfPayload() && PackedSlot::offsetOfLocation() == sizeof(uint32_t) && sizeof(PackedSlot) == sizeof(uint64_t));
static_assert(PackedSlot::offsetOfName() + sizeof(uint16_t) == sizeof(PackedSlot));

static StubSignature signatureReturningValue(std::initializer_list<StubRegister> operands)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    for (StubRegister operand : operands)
        signature.addOperand(gprOf(operand));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfGetById(StubKey)
{
    StubSignature signature = signatureReturningValue({ });
    signature.addOperand(gprOf(firstStubOperandRegister));
    return signature;
}

StubSignature signatureOfGetByIdKeepingRegisters(StubKey key) { return signatureOfGetById(key); }
StubSignature signatureOfGetByIdAfterFrontProbe(StubKey) { return signatureReturningValue({ firstStubOperandRegister, slotHeaderRegister }); }
StubSignature signatureOfGetByIdHashed(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdCheckingColumn(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfSymbolOfRealm(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfSilentSlot(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfNarrowCellInExtension(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfEmptySlot(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfFieldLeftOut(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfGetByIdOfCell(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfInById(StubKey)
{
#if CPU(X86_64)
    return signatureReturningValue({ firstStubOperandRegister }).fillingKeptRegisterInFront(X86Registers::edx);
#else
    return signatureReturningValue({ firstStubOperandRegister });
#endif
}

static void subtractWordAt(StubAssembler& jit, Address address, GPRReg value, GPRReg scratch)
{
#if CPU(X86_64)
    UNUSED_PARAM(scratch);
    jit.sub64(address, value);
#else
    jit.loadPtr(address, scratch);
    jit.sub64(scratch, value);
#endif
}

static void returnAnswerOfObjects(StubAssembler& jit, ObjectLookupQuestion question, bool isPresent)
{
    RELEASE_ASSERT(question != ObjectLookupQuestion::Read || !isPresent);
    if (question == ObjectLookupQuestion::Read)
        jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    else
        jit.move(TrustedImm32(isPresent), jit.result());
    jit.returnToCaller();
}

void findInPropertyTable(StubAssembler& jit, const ObjectLookupRegisters& r, JumpList& isAbsentHere, JumpList& isUnknown, EmptyWordsOfPropertyTable* emptyWords)
{
    static constexpr unsigned numberOfWordsBeforeLoop = 4;
    constexpr unsigned typeShift = 48;
    static_assert(sizeof(Structure::SeenProperties) == sizeof(uint64_t) && sizeof(CompactPropertyTableEntry) == sizeof(uint64_t) && PropertyTable::EmptyEntryIndex == 0);
    const GPRReg entry = r.structure;
    RELEASE_ASSERT(noOverlap(r.receiver, r.name, r.uid, r.object, r.structure, r.size, r.vector, r.index));

    auto search = [&](JumpList EmptyWordsOfPropertyTable::* words, auto loadWord, auto branchIfEntryHasName) {
        for (unsigned probe = 0; probe < numberOfWordsBeforeLoop; ++probe) {
            if (probe)
                jit.add32(TrustedImm32(probe), r.index);
            jit.add32(TrustedImm32(-1), r.size, entry);
            jit.and32(entry, r.index);
            loadWord(r.index);
            (emptyWords ? emptyWords->*words : isAbsentHere).append(jit.branchTest32(CCallHelpers::Zero, entry));
            branchIfEntryHasName(0);
        }
        const GPRReg mask = r.size;
        const GPRReg position = r.index;
        jit.sub32(TrustedImm32(1), mask);
        jit.lshift64(TrustedImm32(32), position);
        jit.add64(TrustedImm32(numberOfWordsBeforeLoop - 1), position);
        Label nextWord = jit.label();
        jit.add64(TrustedImm32(1), position);
        jit.lshift64(position, TrustedImm32(32), entry);
        jit.add64(entry, position);
        jit.urshift64(position, TrustedImm32(32), entry);
        jit.and32(mask, entry);
        loadWord(entry);
        Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, entry);
        branchIfEntryHasName(1);
        jit.jump().linkTo(nextWord, &jit);
        if (!emptyWords) {
            isAbsentHere.append(isEmpty);
            return;
        }
        isEmpty.link(&jit);
        jit.urshift64(TrustedImm32(32), position);
        jit.and32(mask, position);
        jit.add32(TrustedImm32(1), mask);
        (emptyWords->*words).append(jit.jump());
    };

    if (!emptyWords) {
        jit.load64(Address(r.structure, Structure::seenPropertiesOffset()), r.index);
        jit.and64(r.uid, r.index);
        isAbsentHere.append(jit.branch64(CCallHelpers::NotEqual, r.index, r.uid));
    }

    jit.loadPtr(Address(r.structure, Structure::propertyTableUnsafeOffset()), r.size);
    isUnknown.append(jit.branchTestPtr(CCallHelpers::Zero, r.size));
    jit.loadPtr(Address(r.size, PropertyTable::offsetOfIndexVector()), r.vector);
    jit.load32(Address(r.size, PropertyTable::offsetOfIndexSize()), r.size);
    jit.load32(Address(r.uid, StringImpl::flagsOffset()), r.index);
    Jump isNoSymbol = jit.branchTest32(CCallHelpers::Zero, r.index, TrustedImm32(StringImpl::flagIsSymbol()));
    jit.load32(Address(r.uid, OBJECT_OFFSETOF(SymbolImpl::StaticSymbolImpl, m_hashForSymbolShiftedWithFlagCount)), r.index);
    isNoSymbol.link(&jit);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), r.index);
    Jump isWide = jit.branchTestPtr(CCallHelpers::Zero, r.vector, TrustedImm32(PropertyTable::flagOfCompactIndexVector));

    JumpList isFound;
    search(&EmptyWordsOfPropertyTable::inCompactIndex, [&](GPRReg word) {
        jit.load8(BaseIndex(r.vector, word, CCallHelpers::TimesOne, -1), entry);
    }, [&](int32_t missingFromSize) {
        jit.getEffectiveAddress(BaseIndex(r.vector, entry, CCallHelpers::TimesEight, missingFromSize - 1 - static_cast<int32_t>(sizeof(CompactPropertyTableEntry))), entry);
        jit.load64(BaseIndex(entry, r.size, CCallHelpers::TimesOne), entry);
        jit.xor64(r.uid, entry);
        jit.rotateRight64(TrustedImm32(typeShift), entry);
        isFound.append(jit.branch64(CCallHelpers::BelowOrEqual, entry, TrustedImm32(UINT16_MAX)));
    });

    isWide.link(&jit);
    constexpr int32_t offsetOfOffsetInWideEntry = sizeof(UniquedStringImpl*);
    constexpr int32_t offsetOfAttributesInWideEntry = offsetOfOffsetInWideEntry + sizeof(PropertyOffset);
    constexpr PropertyOffset maxOffsetBesideAttributes = (1 << 23) - 1;
    static_assert(sizeof(PropertyTableEntry) == 16);
    {
        PropertyTableEntry sample(nullptr, 0x01020304, 0x7f);
        PropertyOffset offsetOfSample;
        uint8_t attributesOfSample;
        memcpy(&offsetOfSample, reinterpret_cast<const uint8_t*>(&sample) + offsetOfOffsetInWideEntry, sizeof(offsetOfSample));
        memcpy(&attributesOfSample, reinterpret_cast<const uint8_t*>(&sample) + offsetOfAttributesInWideEntry, sizeof(attributesOfSample));
        RELEASE_ASSERT(offsetOfSample == sample.offset() && attributesOfSample == sample.attributes());
    }
    JumpList isFoundInWideEntry;
    search(&EmptyWordsOfPropertyTable::inWideIndex, [&](GPRReg word) {
        jit.load32(BaseIndex(r.vector, word, CCallHelpers::TimesFour), entry);
    }, [&](int32_t missingFromSize) {
        jit.lshift64(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(sizeof(PropertyTableEntry)))), entry);
        jit.getEffectiveAddress(BaseIndex(entry, r.size, CCallHelpers::TimesFour, missingFromSize * static_cast<int32_t>(sizeof(uint32_t)) - static_cast<int32_t>(sizeof(PropertyTableEntry))), entry);
        jit.addPtr(r.vector, entry);
        isFoundInWideEntry.append(jit.branchPtr(CCallHelpers::Equal, Address(entry), r.uid));
    });

    isFoundInWideEntry.link(&jit);
    jit.load32(Address(entry, offsetOfOffsetInWideEntry), r.index);
    isUnknown.append(jit.branch32(CCallHelpers::Above, r.index, TrustedImm32(maxOffsetBesideAttributes)));
    jit.load8(Address(entry, offsetOfAttributesInWideEntry), entry);
    jit.lshift32(TrustedImm32(8), r.index);
    jit.or32(r.index, entry);

    isFound.link(&jit);
}

template<typename Functor>
static void answerByPropertyTableAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, JumpList& isAbsentHere, JumpList& isUnknown, JumpList& cannotCallGetter, const Functor& beforeGetter, const ScopedLambda<void()>* willReturnValue = nullptr)
{
    constexpr int32_t accessorOrCustom = static_cast<int32_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue);
    constexpr int32_t custom = static_cast<int32_t>(PropertyAttribute::CustomAccessorOrValue);
    static_assert(accessorOrCustom <= UINT8_MAX);
    const GPRReg entry = r.structure;

    findInPropertyTable(jit, r, isAbsentHere, isUnknown);
    if (question != ObjectLookupQuestion::Read) {
        returnAnswerOfObjects(jit, question, true);
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
    if (willReturnValue)
        (*willReturnValue)();
    loadProperty(jit.result());
    jit.returnToCaller();

    isNoValue.link(&jit);
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, entry, TrustedImm32(custom)));
    beforeGetter();
    const GPRReg getterSetter = r.uid;
    loadProperty(getterSetter);
    AllowMacroScratchRegisterUsage indexIsFree(jit);
    callGetterAndReturn(jit, r.receiver, getterSetter, cannotCallGetter);
}

static void loadNumberOfName(StubAssembler& jit, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isNoNameOfImage, JumpList& hasNoColumn)
{
    static_assert(sizeof(UniquedStringImpl) == 24);
    switch (name) {
    case ObjectLookupName::IsGivenByNumber:
        hasNoColumn.append(jit.branch32(CCallHelpers::Above, r.name, TrustedImm32(jit.options().maxNameInFieldTable)));
        return;
    case ObjectLookupName::IsGivenByUID:
        jit.move(r.uid, r.name);
        if (jit.options().isPositionIndependent) {
            subtractWordAt(jit, Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.headers)), r.name, r.index);
            isNoNameOfImage.append(jit.branch64(CCallHelpers::AboveOrEqual, r.name, Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.capacity))));
            jit.load32(Address(r.uid, OBJECT_OFFSETOF(LazyStringHeader, number)), r.name);
            hasNoColumn.append(jit.branch32(CCallHelpers::Above, r.name, TrustedImm32(jit.options().maxNameInFieldTable)));
        } else {
            subtractWordAt(jit, Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), r.name, r.index);
            isNoNameOfImage.append(jit.branch64(CCallHelpers::AboveOrEqual, r.name, Address(instanceGPR, AOT_INSTANCE_OFFSET(image.sizeOfProgramIdentifiers))));
            hasNoColumn.append(jit.branch64(CCallHelpers::Above, r.name, TrustedImm32(jit.options().maxNameInFieldTable * sizeof(UniquedStringImpl))));
            jit.urshift32(TrustedImm32(3), r.name);
            jit.mul32(TrustedImm32(static_cast<int32_t>(0xaaaaaaabu)), r.name, r.name);
        }
        isNoNameOfImage.append(jit.branchTest32(CCallHelpers::Zero, r.name));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void answerByClassMembersAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isNoClassPrototype, JumpList& isUnknown)
{
    const GPRReg entry = r.size;
    loadStructure(jit, r.object, r.structure);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.infoOfClassPrototypes)), r.index);
    isNoClassPrototype.append(jit.branchPtr(CCallHelpers::NotEqual, Address(r.structure, Structure::classInfoOffset()), r.index));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), r.index);
    isUnknown.append(jit.branchPtr(CCallHelpers::NotEqual, Address(r.structure, Structure::realmOffset()), r.index));
    isUnknown.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(r.object, JSObject::butterflyOffset())));
    loadNumberOfName(jit, r, name, isUnknown, isUnknown);
    jit.load32(Address(r.object, ClassPrototype::offsetOfMemberRow()), r.index);
    isUnknown.append(jit.branch32(CCallHelpers::Equal, r.index, TrustedImm32(static_cast<int32_t>(ClassRowEntry::noRow))));
    jit.add32(r.name, r.index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), r.vector);
    jit.load32(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour), entry);
    jit.urshift32(entry, TrustedImm32(FieldTableEntry::nameShift), r.vector);
    Jump isNotDeclared = jit.branch32(CCallHelpers::NotEqual, r.vector, r.name);
    if (question != ObjectLookupQuestion::Read)
        returnAnswerOfObjects(jit, question, true);
    else {
        RELEASE_ASSERT(!jit.options().useWideReferences);
        static_assert(!static_cast<unsigned>(ClassRowEntry::Form::Method) && static_cast<unsigned>(ClassRowEntry::Form::Accessor) == 1);
        const GPRReg number = r.index;
        const GPRReg cell = r.vector;
        const GPRReg getter = r.structure;
        auto loadClosure = [&](GPRReg result) {
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureCells)), cell);
            jit.load32(BaseIndex(cell, number, CCallHelpers::TimesFour), cell);
            isUnknown.append(jit.branchTest32(CCallHelpers::Zero, cell));
            jit.add32(TrustedImm32(1), cell);
            decodeCell32(jit, cell, result);
        };
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
        {
            AllowMacroScratchRegisterUsage indexIsFree(jit);
            callGetterFunctionAndReturn(jit, r.receiver, getter, isUnknown);
        }

        hasNoGetter.link(&jit);
        returnAnswerOfObjects(jit, question, false);
    }
    isNotDeclared.link(&jit);
}

static void branchOnFunctionObject(StubAssembler& jit, const ObjectLookupRegisters& r, JumpList& hasWhatPropertyTableLists, JumpList& hasNothing, JumpList& isNoFunctionObject, JumpList& isUnknown)
{
    static_assert(SlimClosureWithPropertiesType == SlimClosureType + 1 && CompiledClosureType > SlimClosureWithPropertiesType - SlimClosureType);
    FunctionKindData constructor { };
    constructor.isConstructor = 1;
    const GPRReg type = r.size;
    const GPRReg function = r.vector;
    const GPRReg kind = r.size;

    jit.load8(Address(r.object, JSCell::typeInfoTypeOffset()), type);
    Jump isClosure = jit.branch32(CCallHelpers::Equal, type, TrustedImm32(CompiledClosureType));
    jit.sub32(TrustedImm32(SlimClosureType), type);
    isNoFunctionObject.append(jit.branch32(CCallHelpers::Above, type, TrustedImm32(SlimClosureWithPropertiesType - SlimClosureType)));
    isClosure.link(&jit);
    for (ptrdiff_t offsetOfUID : { AOT_INSTANCE_OFFSET(propertyCaches.nameUID), AOT_INSTANCE_OFFSET(propertyCaches.lengthUID), AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID) })
        isUnknown.append(jit.branchPtr(CCallHelpers::Equal, Address(instanceGPR, static_cast<int32_t>(offsetOfUID)), r.uid));
    hasNothing.append(jit.branchTest32(CCallHelpers::Zero, type));
    hasWhatPropertyTableLists.append(jit.branch32(CCallHelpers::NotEqual, type, TrustedImm32(CompiledClosureType)));
    jit.load64(Address(r.object, offsetOfClosureEntry), function);
    extractBits(jit, function, ClosureWord::functionShift, ClosureWord::numberOfFunctionBits, function);
    loadKindOfClosureFunction(jit, function, kind);
    hasWhatPropertyTableLists.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(std::bit_cast<uint16_t>(constructor))));
    isUnknown.append(jit.jump());
}

void locateFieldOfObject(StubAssembler& jit, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& hasNoField, JumpList& isUnknown)
{
    static_assert(sizeof(FieldTableEntry) == 1u << CCallHelpers::TimesFour && FieldTableEntry::nameShift == 16);
    loadNumberOfName(jit, r, name, hasNoField, isUnknown);
    loadShapeWordAsIndex(jit, r.object, r.index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), r.vector);
    jit.load32(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour, offsetOfRowsByShapeIndex(jit)), r.index);
    jit.add32(r.name, r.index);
    jit.load16(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour, FieldTableEntry::nameShift / 8), r.vector);
    hasNoField.append(jit.branch32(CCallHelpers::NotEqual, r.vector, r.name));
}

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
    AllowMacroScratchRegisterUsage indexIsFree(jit);
    Jump isOutOfLine = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldIsOutOfLine));
    computeFieldAddress(jit, registers.field, false, exits.isAbsent);
    Label hasAddress = jit.label();
    if (question == ObjectLookupQuestion::Read) {
        Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldIsAccessor));
        readFieldAndReturn(jit, registers, exits);

        isAccessor.link(&jit);
        const GPRReg getterSetter = r.uid;
        loadGetterSetterInField(jit, registers.field.address, registers.field.representation, getterSetter, exits.isAbsent);
        branchUnlessAccessorPair(jit, getterSetter, isUnknown);
        callGetterAndReturn(jit, r.receiver, getterSetter, isUnknown);
    } else {
        JumpList isPresent;
        branchOnFieldPresence(jit, registers.field, registers.value, isPresent, exits.isAbsent);
        isPresent.link(&jit);
        returnAnswerOfObjects(jit, question, true);
    }

    isOutOfLine.link(&jit);
    jit.load32(Address(r.structure, CompiledStructure::offsetOfExtensionSlot()), registers.field.extensionWordOffset);
    jit.and32(TrustedImm32(SlotData::maxOffset), registers.field.extensionWordOffset);
    computeFieldAddress(jit, registers.field, true, exits.isAbsent);
    jit.jump().linkTo(hasAddress, &jit);

    isUnknown.append(exits.cannot);
    exits.isAbsent.link(&jit);
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldMayExistWithoutStorage)));
    loadIdentifier(jit, r.name, r.uid, isUnknown);
    hasNoField.link(&jit);
}

void answerFromObjectsAndReturn(StubAssembler& jit, ObjectLookupQuestion question, const ObjectLookupRegisters& r, ObjectLookupName name, JumpList& isUnknown, const ScopedLambda<void()>* willReturnValue)
{
    constexpr int32_t flagsOfOwnLookup = OverridesGetOwnPropertySlot | OverridesGetPrototype;
    static_assert((flagsOfOwnLookup | HasStaticPropertyTable) <= UINT8_MAX);
    JumpList isAbsentHere;
    JumpList isAbsentFromExtraProperties;
    JumpList extraPropertiesDoNotSay;
    const GPRReg objectWithExtraProperties = r.name;
    auto restoreName = [&] {
        if (name != ObjectLookupName::IsGivenByNumber)
            return;
        if (jit.options().isPositionIndependent) {
            jit.load32(Address(r.uid, OBJECT_OFFSETOF(LazyStringHeader, number)), r.name);
        } else {
            static_assert(sizeof(UniquedStringImpl) == 24);
            jit.move(r.uid, r.name);
            subtractWordAt(jit, Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), r.name, r.index);
            jit.urshift32(TrustedImm32(3), r.name);
            jit.mul32(TrustedImm32(static_cast<int32_t>(0xaaaaaaabu)), r.name, r.name);
        }
    };

    auto branchIfNameBeginsWithDigit = [&] {
        jit.loadPtr(Address(r.uid, StringImpl::dataOffset()), r.index);
        jit.load8(Address(r.index), r.index);
        jit.sub32(TrustedImm32('0'), r.index);
        return jit.branch32(CCallHelpers::BelowOrEqual, r.index, TrustedImm32(9));
    };

    isUnknown.append(jit.branchIfNotCell(r.receiver));
    isUnknown.append(jit.branchIfNotObject(r.receiver));
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, Address(r.uid, StringImpl::lengthMemoryOffset())));
    jit.move(r.receiver, r.object);

    Label askObject = jit.label();
    Jump hasOwnLookup = jit.branchTest8(CCallHelpers::NonZero, Address(r.object, JSCell::typeInfoFlagsOffset()), TrustedImm32(flagsOfOwnLookup));
    Label askPropertyTable = jit.label();
    Jump hasElements = jit.branchTest8(CCallHelpers::NonZero, Address(r.object, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask));
    Label askPropertyTableForNameThatSpellsNoIndex = jit.label();
    loadStructure(jit, r.object, r.structure);
    answerByPropertyTableAndReturn(jit, question, r, isAbsentHere, isUnknown, isUnknown, [] { }, willReturnValue);

    isAbsentHere.link(&jit);
    Jump hasNoStaticTable = jit.branchTest8(CCallHelpers::Zero, Address(r.object, JSCell::typeInfoFlagsOffset()), TrustedImm32(HasStaticPropertyTable));
    loadStructure(jit, r.object, r.structure);
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, Address(r.structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_staticPropertiesReifiedBits)));
    hasNoStaticTable.link(&jit);
    Label askObjectAbove = jit.label();
    if (question == ObjectLookupQuestion::HasOwn)
        returnAnswerOfObjects(jit, question, false);
    else {
        loadStructure(jit, r.object, r.structure);
        jit.load64(Address(r.structure, Structure::prototypeOffset()), r.object);
        isUnknown.append(jit.branchTest64(CCallHelpers::Zero, r.object));
        jit.branchIfCell(r.object).linkTo(askObject, &jit);
        returnAnswerOfObjects(jit, question, false);
    }

    hasOwnLookup.link(&jit);
    isUnknown.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsSymbol())));
    JumpList isNotCompiledObject = branchIfNotCompiledObjectOfThisImage(jit, r.object);
    answerByFieldRecordAndReturn(jit, question, r, name, isUnknown);
    jit.branchTest32(CCallHelpers::Zero, Address(r.object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)).linkTo(askObjectAbove, &jit);
    loadStructure(jit, r.object, r.structure);
    jit.load32(Address(r.structure, CompiledStructure::offsetOfExtraPropertiesSlot()), r.index);
    jit.and32(TrustedImm32(SlotData::maxOffset), r.index);
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, r.index));
    jit.move(r.object, objectWithExtraProperties);
    loadExtraProperties(jit, objectWithExtraProperties, r.index, r.object, extraPropertiesDoNotSay);
    extraPropertiesDoNotSay.append(jit.branch8(CCallHelpers::NotEqual, Address(r.object, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));
    Jump extraPropertiesHaveNoElements = jit.branchTest8(CCallHelpers::Zero, Address(r.object, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask));
    extraPropertiesDoNotSay.append(branchIfNameBeginsWithDigit());
    extraPropertiesHaveNoElements.link(&jit);
    loadStructure(jit, r.object, r.structure);
    answerByPropertyTableAndReturn(jit, question, r, isAbsentFromExtraProperties, extraPropertiesDoNotSay, isUnknown, restoreName);

    isAbsentFromExtraProperties.link(&jit);
    jit.move(objectWithExtraProperties, r.object);
    restoreName();
    jit.jump().linkTo(askObjectAbove, &jit);

    extraPropertiesDoNotSay.link(&jit);
    restoreName();
    isUnknown.append(jit.jump());

    hasElements.link(&jit);
    isUnknown.append(branchIfNameBeginsWithDigit());
    jit.jump().linkTo(askPropertyTableForNameThatSpellsNoIndex, &jit);

    isNotCompiledObject.link(&jit);
    if (jit.options().useWideReferences) {
        isUnknown.append(jit.jump());
        return;
    }
    JumpList hasWhatPropertyTableLists;
    JumpList hasNothing;
    JumpList isNoFunctionObject;
    branchOnFunctionObject(jit, r, hasWhatPropertyTableLists, hasNothing, isNoFunctionObject, isUnknown);
    hasWhatPropertyTableLists.linkTo(askPropertyTable, &jit);
    hasNothing.linkTo(askObjectAbove, &jit);

    isNoFunctionObject.link(&jit);
    JumpList isNoClassPrototype;
    answerByClassMembersAndReturn(jit, question, r, name, isNoClassPrototype, isUnknown);
    jit.jump().linkTo(askObjectAbove, &jit);

    isNoClassPrototype.link(&jit);
    isUnknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(r.object, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    jit.loadPtr(Address(r.structure, Structure::classInfoOffset()), r.index);
    jit.loadPtr(Address(r.index, OBJECT_OFFSETOF(ClassInfo, methodTable) + OBJECT_OFFSETOF(MethodTable, getOwnPropertySlot)), r.index);
    jit.branchPtr(CCallHelpers::Equal, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.lookupOfOrdinaryObjects)), r.index).linkTo(askPropertyTable, &jit);
    auto branchIfNamed = [&](ptrdiff_t offsetOfUID) {
        return jit.branchPtr(CCallHelpers::Equal, Address(instanceGPR, static_cast<int32_t>(offsetOfUID)), r.uid);
    };
    isUnknown.append(branchIfNamed(AOT_INSTANCE_OFFSET(propertyCaches.lengthUID)));

    Jump isNoArray = jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.lookupOfArrays)), r.index);
    isUnknown.append(branchIfNameBeginsWithDigit());
    jit.jump().linkTo(askPropertyTableForNameThatSpellsNoIndex, &jit);

    isNoArray.link(&jit);
    isUnknown.append(jit.branchPtr(CCallHelpers::NotEqual, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.lookupOfFunctions)), r.index));
    isUnknown.append(branchIfNamed(AOT_INSTANCE_OFFSET(propertyCaches.nameUID)));
    isUnknown.append(branchIfNamed(AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID)));
    jit.jump().linkTo(askPropertyTable, &jit);
}

static void refName(StubAssembler& jit, GPRReg uid, GPRReg scratch, GPRReg otherScratch)
{
    constexpr int32_t increment = 2;
    static_assert(StringImpl::lengthMemoryOffset() == sizeof(uint32_t) && StringImpl::refCountOfStaticString() == 1);
    {
        Ref<StringImpl> sample = StringImpl::create("sample"_s);
        uint32_t count;
        memcpy(&count, sample.ptr(), sizeof(count));
        RELEASE_ASSERT(sample->hasOneRef() && count == increment);
    }
    const Address count(uid);
    Jump isStatic = jit.branchTest32(CCallHelpers::NonZero, count, TrustedImm32(StringImpl::refCountOfStaticString()));
#if CPU(X86_64)
    UNUSED_PARAM(scratch);
    UNUSED_PARAM(otherScratch);
    jit.atomicAdd32(TrustedImm32(increment), count);
#else
    Label again = jit.label();
    jit.loadLink32(count, scratch);
    jit.add32(TrustedImm32(increment), scratch);
    jit.storeCond32(scratch, count, otherScratch);
    jit.branchTest32(CCallHelpers::NonZero, otherScratch).linkTo(again, &jit);
#endif
    isStatic.link(&jit);
}

static void addToDictionary(StubAssembler& jit, const ObjectLookupRegisters& r, EmptyWordsOfPropertyTable& emptyWords, GPRReg spare, JumpList& cannot)
{
    constexpr int32_t dictionaryKinds = Structure::s_dictionaryKindMask << Structure::s_dictionaryKindShift;
    constexpr int32_t bitsThatForbid = Structure::s_mayBePrototypeBits | Structure::s_didPreventExtensionsBits;
    constexpr int32_t overridesPut = OverridesPut >> numberOfInlineBits;
    constexpr unsigned offsetShiftInCompactEntry = 56;
    static_assert(sizeof(TypeInfo::OutOfLineTypeFlags) == sizeof(uint16_t) && overridesPut <= INT16_MAX && initialOutOfLineCapacity == 4 && outOfLineGrowthFactor == 2);
    static_assert(sizeof(std::unique_ptr<Vector<PropertyOffset>>) == sizeof(void*));
    {
        CompactPropertyTableEntry sample(nullptr, 0x7f, 0);
        uint64_t bits;
        memcpy(&bits, &sample, sizeof(bits));
        RELEASE_ASSERT(bits == static_cast<uint64_t>(0x7f) << offsetShiftInCompactEntry);
    }
    const GPRReg owner = r.object;
    const GPRReg structure = r.structure;
    const GPRReg word = r.index;
    const GPRReg entries = r.vector;
    const GPRReg offset = r.size;
    const GPRReg scratch = r.name;
    const GPRReg table = r.name;
    const GPRReg numberOfEntry = spare;
    RELEASE_ASSERT(noOverlap(spare, r.receiver, r.name, r.uid, r.object, r.structure, r.size, r.vector, r.index));

    emptyWords.inCompactIndex.link(&jit);
    jit.getEffectiveAddress(BaseIndex(r.vector, r.index, CCallHelpers::TimesOne, -1), word);
    jit.getEffectiveAddress(BaseIndex(r.vector, r.size, CCallHelpers::TimesOne, -1), entries);
    Jump hasAddresses = jit.jump();

    emptyWords.inWideIndex.link(&jit);
    jit.getEffectiveAddress(BaseIndex(r.vector, r.index, CCallHelpers::TimesFour), word);
    jit.getEffectiveAddress(BaseIndex(r.vector, r.size, CCallHelpers::TimesFour), entries);
    hasAddresses.link(&jit);

    jit.load32(Address(r.uid, StringImpl::flagsOffset()), scratch);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), scratch);
    cannot.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(StringHasher::computeLiteralHashAndMaskTop8Bits("then"_s)))));
    cannot.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(StringHasher::computeLiteralHashAndMaskTop8Bits("__proto__"_s)))));
    jit.loadPtr(Address(r.uid, StringImpl::dataOffset()), scratch);
    jit.load8(Address(scratch), scratch);
    jit.sub32(TrustedImm32('0'), scratch);
    cannot.append(jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(9)));

    cannot.append(jit.branchPtr(CCallHelpers::NotEqual, owner, r.receiver));
    loadStructure(jit, owner, structure);
    cannot.append(jit.branchTest32(CCallHelpers::Zero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(dictionaryKinds)));
    cannot.append(jit.branchTest32(CCallHelpers::Zero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_isPinnedPropertyTableBits)));
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(bitsThatForbid)));
    const GPRReg objectAbove = scratch;
    Label askObjectAbove = jit.label();
    jit.load64(Address(structure, Structure::prototypeOffset()), objectAbove);
    cannot.append(jit.branchTest64(CCallHelpers::Zero, objectAbove));
    Jump isTop = jit.branch64(CCallHelpers::Equal, objectAbove, TrustedImm32(JSValue::ValueNull));
    cannot.append(jit.branchTest8(CCallHelpers::NonZero, Address(objectAbove, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    loadStructure(jit, objectAbove, structure);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits)));
    cannot.append(jit.branchTest16(CCallHelpers::NonZero, Address(structure, Structure::outOfLineTypeFlagsOffset()), TrustedImm32(overridesPut)));
    jit.jump().linkTo(askObjectAbove, &jit);

    isTop.link(&jit);
    loadStructure(jit, owner, structure);

    const GPRReg room = r.size;
    jit.loadPtr(Address(structure, Structure::propertyTableUnsafeOffset()), table);
    jit.urshift32(TrustedImm32(1), room);
    jit.sub32(Address(table, PropertyTable::offsetOfKeyCount()), room);
    jit.sub32(Address(table, PropertyTable::offsetOfDeletedCount()), room);
    cannot.append(jit.branch32(CCallHelpers::LessThanOrEqual, room, TrustedImm32(0)));
    const GPRReg deletedOffsets = r.size;
    jit.loadPtr(Address(table, PropertyTable::offsetOfDeletedOffsets()), deletedOffsets);
    Jump hasNoDeletedOffsets = jit.branchTestPtr(CCallHelpers::Zero, deletedOffsets);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(deletedOffsets, Vector<PropertyOffset>::sizeMemoryOffset())));
    hasNoDeletedOffsets.link(&jit);
    jit.load32(Address(table, PropertyTable::offsetOfKeyCount()), offset);
    jit.load8(Address(structure, Structure::inlineCapacityOffset()), scratch);
    Jump isInCell = jit.branch32(CCallHelpers::Below, offset, scratch);
    jit.sub32(scratch, offset);
    cannot.append(jit.branchTest32(CCallHelpers::Zero, offset));
    Jump hasRoom = jit.branch32(CCallHelpers::Below, offset, TrustedImm32(initialOutOfLineCapacity));
    jit.sub32(offset, TrustedImm32(1), scratch);
    cannot.append(jit.branchTest32(CCallHelpers::Zero, scratch, offset));
    hasRoom.link(&jit);
    jit.add32(TrustedImm32(firstOutOfLineOffset), offset);
    isInCell.link(&jit);

    auto takeNextEntry = [&] {
        jit.load32(Address(table, PropertyTable::offsetOfDeletedCount()), numberOfEntry);
        jit.add32(Address(table, PropertyTable::offsetOfKeyCount()), numberOfEntry);
        jit.add32(TrustedImm32(1), Address(table, PropertyTable::offsetOfKeyCount()));
    };
    jit.loadPtr(Address(structure, Structure::propertyTableUnsafeOffset()), table);
    Jump isWide = jit.branchTest32(CCallHelpers::Zero, Address(table, PropertyTable::offsetOfIndexVector()), TrustedImm32(PropertyTable::flagOfCompactIndexVector));
    cannot.append(jit.branch32(CCallHelpers::Above, offset, TrustedImm32(UINT8_MAX)));
    takeNextEntry();
    jit.move(offset, scratch);
    jit.lshift64(TrustedImm32(offsetShiftInCompactEntry), scratch);
    jit.or64(r.uid, scratch);
    jit.store64(scratch, BaseIndex(entries, numberOfEntry, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), numberOfEntry);
    jit.store8(numberOfEntry, Address(word));
    Jump isListed = jit.jump();

    isWide.link(&jit);
    cannot.append(jit.branch32(CCallHelpers::Above, offset, TrustedImm32(Structure::greatestMaxOffsetOutsideRareData)));
    takeNextEntry();
    static_assert(sizeof(PropertyTableEntry) == 2 * sizeof(uint64_t));
    jit.add32(numberOfEntry, numberOfEntry, scratch);
    jit.storePtr(r.uid, BaseIndex(entries, scratch, CCallHelpers::TimesEight));
    jit.store64(offset, BaseIndex(entries, scratch, CCallHelpers::TimesEight, sizeof(UniquedStringImpl*)));
    jit.add32(TrustedImm32(1), numberOfEntry);
    jit.store32(numberOfEntry, Address(word));
    isListed.link(&jit);

    jit.store16(offset, Address(structure, Structure::maxOffsetOffset()));
    jit.load64(Address(structure, Structure::seenPropertiesOffset()), scratch);
    jit.or64(r.uid, scratch);
    jit.store64(scratch, Address(structure, Structure::seenPropertiesOffset()));
    jit.load32(Address(r.uid, StringImpl::flagsOffset()), scratch);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), scratch);
    jit.xor32(Address(structure, Structure::propertyHashOffset()), scratch);
    jit.store32(scratch, Address(structure, Structure::propertyHashOffset()));
    refName(jit, r.uid, scratch, numberOfEntry);
    jit.move(offset, r.structure);
}

void replaceOwnValueAndReturn(StubAssembler& jit, SlotOperation operation, const ObjectLookupRegisters& r, ObjectLookupName name, GPRReg value, JumpList& cannot, GPRReg spare, const ScopedLambda<void()>* willReplaceValue)
{
    constexpr uint32_t bitsThatForbid = Structure::s_mayBePrototypeBits | Structure::s_hasImmutablePropertiesBits | Structure::s_hasListedImmutablePropertiesBits | Structure::s_isWatchingReplacementBits;
    const int32_t attributesThatForbid = operation == SlotOperation::Define ? UINT8_MAX : static_cast<int32_t>(PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
    const GPRReg owner = r.object;
    const GPRReg offset = r.structure;
    const GPRReg storage = r.vector;
    RELEASE_ASSERT(attributesThatForbid <= UINT8_MAX && noOverlap(value, r.receiver, r.name, r.uid, r.object, r.structure, r.size, r.vector, r.index));

    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.uid, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsSymbol())));
    cannot.append(jit.branchTest32(CCallHelpers::Zero, Address(r.uid, StringImpl::lengthMemoryOffset())));
    jit.move(r.receiver, owner);
    Jump isOrdinary = jit.branch8(CCallHelpers::Equal, Address(owner, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType));

    JumpList hasNoField;
    cannot.append(branchIfNotCompiledObjectOfThisImage(jit, owner));
    cannot.append(jit.branchTest32(CCallHelpers::Zero, Address(owner, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    locateFieldOfObject(jit, r, name, hasNoField, cannot);
    cannot.append(jit.jump());
    hasNoField.link(&jit);
    loadStructure(jit, owner, r.structure);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    jit.load32(Address(r.structure, CompiledStructure::offsetOfExtraPropertiesSlot()), r.index);
    jit.and32(TrustedImm32(SlotData::maxOffset), r.index);
    cannot.append(jit.branchTest32(CCallHelpers::Zero, r.index));
    loadExtraProperties(jit, r.receiver, r.index, owner, cannot);
    cannot.append(jit.branch8(CCallHelpers::NotEqual, Address(owner, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));

    isOrdinary.link(&jit);
    loadStructure(jit, owner, r.structure);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.structure, Structure::bitFieldOffset()), TrustedImm32(static_cast<int32_t>(bitsThatForbid))));
    const bool addsToDictionaries = spare != InvalidGPRReg && name == ObjectLookupName::IsGivenByUID;
    EmptyWordsOfPropertyTable emptyWords;
    findInPropertyTable(jit, r, cannot, cannot, addsToDictionaries ? &emptyWords : nullptr);
    cannot.append(jit.branchTest32(CCallHelpers::NonZero, offset, TrustedImm32(attributesThatForbid)));
    if (willReplaceValue)
        (*willReplaceValue)();
    jit.urshift32(TrustedImm32(8), offset);
    Label storeAtOffset = jit.label();
    Jump isOutOfLine = jit.branch32(CCallHelpers::AboveOrEqual, offset, TrustedImm32(firstOutOfLineOffset));
    jit.store64(value, BaseIndex(owner, offset, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    returnAfterStoring(jit, owner, value);

    isOutOfLine.link(&jit);
    jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
    jit.neg64(offset);
    jit.store64(value, BaseIndex(storage, offset, CCallHelpers::TimesEight, (firstOutOfLineOffset - 2) * static_cast<int32_t>(sizeof(EncodedJSValue))));
    returnAfterStoring(jit, owner, value);

    if (addsToDictionaries) {
        RELEASE_ASSERT(spare != value && operation != SlotOperation::Define);
        addToDictionary(jit, r, emptyWords, spare, cannot);
        jit.jump().linkTo(storeAtOffset, &jit);
    }
}

void countMissOfPair(StubAssembler& jit, GPRReg structureID, GPRReg uid, GPRReg counter, JumpList& isDueForFiling)
{
    static_assert(hasOneBitSet(MissesOfPairs::numberOfCounters) && hasOneBitSet(MissesOfPairs::missesBetweenFilings) && hasOneBitSet(MissesOfPairs::missesBetweenFilingsOfDictionaries));
    static_assert(MissesOfPairs::missesBetweenFilings < MissesOfPairs::missesBetweenFilingsOfDictionaries && MissesOfPairs::missesBetweenFilingsOfDictionaries <= UINT8_MAX + 1);
    RELEASE_ASSERT(noOverlap(structureID, uid, counter, instanceGPR));
    jit.mul32(TrustedImm32(static_cast<int32_t>(HashedSlot::structureMultiplier)), structureID, counter);
    jit.urshift32(TrustedImm32(HashedSlot::structureHashShift), counter);
    jit.getEffectiveAddress(BaseIndex(counter, uid, static_cast<CCallHelpers::Scale>(HashedSlot::uidHashShift)), counter);
    jit.urshift32(TrustedImm32(HashedSlot::sizeShift), counter);
    jit.and32(TrustedImm32(MissesOfPairs::numberOfCounters - 1), counter);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.missesOfPairs)), counter);
    jit.add8(TrustedImm32(1), Address(counter));
    JumpList isNotDue;
    isNotDue.append(jit.branchTest8(CCallHelpers::NonZero, Address(counter), TrustedImm32(MissesOfPairs::missesBetweenFilings - 1)));
    Jump isDueForAnyStructure = jit.branchTest8(CCallHelpers::Zero, Address(counter), TrustedImm32(MissesOfPairs::missesBetweenFilingsOfDictionaries - 1));
    jit.move(structureID, counter);
    decodeStructureID(jit, counter);
    isNotDue.append(jit.branchTest32(CCallHelpers::NonZero, Address(counter, Structure::bitFieldOffset()), TrustedImm32(Structure::s_dictionaryKindMask << Structure::s_dictionaryKindShift)));
    isDueForAnyStructure.link(&jit);
    jit.store8(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.missIsDueForFiling)));
    isDueForFiling.append(jit.jump());
    isNotDue.link(&jit);
}

static_assert(!Slot::kindFlagOf(Slot::Kind::Value) && !Slot::kindFlagOf(Slot::Kind::Absent) && !Slot::kindFlagOf(Slot::Kind::ExtraProperty));

static constexpr int32_t biasedBitsOfFrontCell(Slot::Kind kind, uint8_t flags)
{
    return (static_cast<uint8_t>(kind) | flags) ^ PackedSlot::ownInlineValueBits;
}

void readByLoadedFrontCell(StubAssembler& jit, const LoadedFrontCell& r, GPRReg result, const ScopedLambda<void()>& finish)
{
    const GPRReg kind = r.scratch;
    const GPRReg wordIndex = r.scratch;
    const GPRReg storage = r.secondScratch;
    RELEASE_ASSERT(noOverlap(r.receiver, r.cell, r.scratch, r.secondScratch));
    JumpList needsHandler;
    auto answerWhereKindIs = [&](Slot::Kind kindOfAnswer, const auto& load) {
        const int32_t biasedKind = biasedBitsOfFrontCell(kindOfAnswer, 0) & PackedSlot::kindMask;
        Jump isOfOtherKind = biasedKind ? jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(biasedKind)) : jit.branchTest32(CCallHelpers::NonZero, kind);
        load();
        finish();
        isOfOtherKind.link(&jit);
    };
    auto answerExtraProperty = [&] {
        answerWhereKindIs(Slot::Kind::ExtraProperty, [&] {
            loadExtraPropertyByFrontCell(jit, r.cell, r.receiver, storage, wordIndex, result, needsHandler);
        });
    };
    auto answerValue = [&] {
        answerWhereKindIs(Slot::Kind::Value, [&] {
            loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, r.cell, InvalidGPRReg), r.receiver, storage, wordIndex, result);
        });
    };
    auto answerAbsence = [&] {
        answerWhereKindIs(Slot::Kind::Absent, [&] {
            jit.move(TrustedImm64(JSValue::ValueUndefined), result);
        });
    };

    extractBits(jit, r.cell, 8 * PackedSlot::offsetOfBiasedKindAndFlags(), PackedSlot::kindBits, kind);
    switch (r.receiverIs) {
    case ReceiverOfFrontProbe::IsCompiledObject:
        answerExtraProperty();
        answerValue();
        answerAbsence();
        break;
    case ReceiverOfFrontProbe::IsAnyCell:
        answerValue();
        answerAbsence();
        answerExtraProperty();
        break;
    }
    needsHandler.link(&jit);
}

void readByFrontTable(StubAssembler& jit, const FrontTableRead& r, GPRReg result, JumpList& hasOtherName, std::optional<StubKey> entersBehindAnswer)
{
#if CPU(X86_64)
    const GPRReg plane = r.scratch;
    const GPRReg index = r.structureID;
    const GPRReg word = r.cell;
    RELEASE_ASSERT(noOverlap(r.receiver, r.column, plane, index, word, cellCageBaseGPR) && noOverlap(result, r.column, plane, index, word));
    auto inCell = [=](ptrdiff_t offset) {
        return BaseIndex(plane, index, CCallHelpers::TimesEight, static_cast<int32_t>(offset));
    };
    struct Answer {
        Jump isAsked;
        Function<void(StubAssembler&, JumpList& needsHandler)> load;
    };
    Vector<Answer, 5> answers;
    auto answerWhereCellIs = [&](Slot::Kind kind, uint8_t flags, Function<void(StubAssembler&, JumpList&)>&& load) {
        answers.append(Answer { jit.branch8(CCallHelpers::Equal, inCell(PackedSlot::offsetOfBiasedKindAndFlags()), TrustedImm32(biasedBitsOfFrontCell(kind, flags))), WTF::move(load) });
    };
    auto answerExtraProperty = [&](bool isOutOfLine) {
        answerWhereCellIs(Slot::Kind::ExtraProperty, isOutOfLine ? PackedSlot::isOwn | PackedSlot::isOutOfLine : PackedSlot::isOwn, [=](StubAssembler& jit, JumpList& needsHandler) {
            loadExtraPropertyByFrontCell(jit, inCell(0), isOutOfLine, r.receiver, word, result, needsHandler);
        });
    };
    auto answerValueInCellOfHolder = [&] {
        answerWhereCellIs(Slot::Kind::Value, 0, [=](StubAssembler& jit, JumpList&) {
            jit.load8(inCell(PackedSlot::offsetOfLocation()), word);
            jit.add32(inCell(PackedSlot::offsetOfPayload()), word);
            if (r.filesInAccessTable) {
                const GPRReg holder = index;
                jit.load32(inCell(PackedSlot::offsetOfPayload()), holder);
                jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, holder, CCallHelpers::TimesEight), holder);
                jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, word, CCallHelpers::TimesEight), word);
                fileValueAboveInAccessTable(jit, { r.receiver, r.column, plane, holder }, word);
                jit.load64(Address(word), result);
                return;
            }
            jit.load64(BaseIndex(cellCageBaseGPR, word, CCallHelpers::TimesEight), result);
        });
    };
    auto answerValueInStorageOfHolder = [&] {
        answerWhereCellIs(Slot::Kind::Value, PackedSlot::isOutOfLine, [=](StubAssembler& jit, JumpList&) {
            if (r.filesInAccessTable) {
                jit.load64(inCell(0), word);
                makeAnswerInStorageOfHolder(jit, word, word);
                fileValueInStorageOfHolderInAccessTable(jit, { r.receiver, r.column, plane, index }, word);
                loadValueInStorageOfHolderByAnswer(jit, word, plane, result);
                return;
            }
            jit.load32(inCell(PackedSlot::offsetOfPayload()), word);
            jit.loadPtr(BaseIndex(cellCageBaseGPR, word, CCallHelpers::TimesEight, JSObject::butterflyOffset()), word);
            jit.load8SignedExtendTo64(inCell(PackedSlot::offsetOfLocation()), index);
            jit.load64(BaseIndex(word, index, CCallHelpers::TimesEight), result);
        });
    };
    auto answerValueInOwnCell = [&] {
        answerWhereCellIs(Slot::Kind::Value, PackedSlot::isOwn, [=](StubAssembler& jit, JumpList&) {
            jit.load32(inCell(PackedSlot::offsetOfPayload()), word);
            if (r.filesInAccessTable) {
                jit.lshift32(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(sizeof(EncodedJSValue)))), word);
                fileOwnValueOfOrdinaryCellInAccessTable(jit, { r.receiver, r.column, plane, index }, word);
                jit.load64(BaseIndex(r.receiver, word, CCallHelpers::TimesOne), result);
                return;
            }
            jit.load64(BaseIndex(r.receiver, word, CCallHelpers::TimesEight), result);
        });
    };
    auto answerValueInOwnStorage = [&] {
        answerWhereCellIs(Slot::Kind::Value, PackedSlot::isOwn | PackedSlot::isOutOfLine, [=](StubAssembler& jit, JumpList&) {
            jit.load32SignedExtendTo64(inCell(PackedSlot::offsetOfPayload()), word);
            if (r.filesInAccessTable) {
                makeAnswerInOwnStorage(jit, word);
                fileOwnValueInStorageInAccessTable(jit, { r.receiver, r.column, plane, index }, word);
                loadValueInOwnStorageByAnswer(jit, r.receiver, word, plane, result);
                return;
            }
            jit.loadPtr(Address(r.receiver, JSObject::butterflyOffset()), plane);
            jit.load64(BaseIndex(plane, word, CCallHelpers::TimesEight), result);
        });
    };
    auto answerAbsence = [&] {
        answerWhereCellIs(Slot::Kind::Absent, 0, [=](StubAssembler& jit, JumpList&) {
            if (r.filesInAccessTable)
                fileAbsenceInAccessTable(jit, { r.receiver, r.column, plane, index }, word);
            jit.move(TrustedImm64(JSValue::ValueUndefined), result);
        });
    };

    locateFrontCell(jit, SlotAccess::Read, index, r.column, plane);
    jit.m_assembler.cmpw_rm(r.column, static_cast<int32_t>(PackedSlot::offsetOfName()), plane, index, CCallHelpers::TimesEight);
    hasOtherName.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE)));
    switch (r.receiverIs) {
    case ReceiverOfFrontProbe::IsCompiledObject:
        answerExtraProperty(false);
        answerValueInCellOfHolder();
        answerAbsence();
        answerValueInStorageOfHolder();
        answerExtraProperty(true);
        break;
    case ReceiverOfFrontProbe::IsAnyCell:
        answerValueInOwnCell();
        answerAbsence();
        answerValueInOwnStorage();
        answerValueInCellOfHolder();
        answerValueInStorageOfHolder();
        break;
    }
    Label asksHandler = jit.label();
    for (Answer& answer : answers) {
        jit.addOutOfLinePart([isAsked = answer.isAsked, load = WTF::move(answer.load), asksHandler, entersBehindAnswer](StubAssembler& jit) {
            isAsked.link(&jit);
            JumpList needsHandler;
            load(jit, needsHandler);
            if (entersBehindAnswer)
                jit.jumpToStub(*entersBehindAnswer);
            else
                jit.returnToCaller();
            needsHandler.linkTo(asksHandler, &jit);
        });
    }
    jit.load64(inCell(0), word);
#else
    auto finish = [&] {
        if (entersBehindAnswer)
            jit.jumpToStub(*entersBehindAnswer);
        else
            jit.returnToCaller();
    };
    const AccessTableRegisters inAccessTable { r.receiver, r.column, r.structureID, r.secondScratch };
    loadFrontCell(jit, SlotAccess::Read, { r.structureID, r.column, r.secondScratch, r.cell, r.scratch });
    if (r.receiverIs == ReceiverOfFrontProbe::IsAnyCell) {
        Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, r.column, r.cell, r.scratch);
        if (r.filesInAccessTable) {
            const GPRReg offsetInCell = r.scratch;
            jit.zeroExtend32ToWord(r.cell, offsetInCell);
            jit.lshift32(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(sizeof(EncodedJSValue)))), offsetInCell);
            fileOwnValueOfOrdinaryCellInAccessTable(jit, inAccessTable, offsetInCell);
        }
        loadOwnInlineValue(jit, r.receiver, r.cell, result);
        finish();
        isNotOwnInlineValue.link(&jit);
    }
    hasOtherName.append(branchIfCellNameDiffers(jit, r.column, r.cell, r.scratch));
    if (r.filesInAccessTable) {
        const GPRReg kindAndFlags = r.scratch;
        const GPRReg answer = r.scratch;
        auto answerWhereCellIs = [&](Slot::Kind kind, uint8_t flags, const auto& fileAndLoad) {
            Jump isSomethingElse = jit.branch32(CCallHelpers::NotEqual, kindAndFlags, TrustedImm32(biasedBitsOfFrontCell(kind, flags)));
            fileAndLoad();
            finish();
            isSomethingElse.link(&jit);
        };
        extractBits(jit, r.cell, 8 * PackedSlot::offsetOfBiasedKindAndFlags(), 8, kindAndFlags);
        answerWhereCellIs(Slot::Kind::Value, 0, [&] {
            const GPRReg holder = inAccessTable.place;
            extractBits(jit, r.cell, locationShiftInCell, 8, answer);
            jit.add32(r.cell, answer);
            jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, answer, CCallHelpers::TimesEight), answer);
            jit.zeroExtend32ToWord(r.cell, holder);
            jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, holder, CCallHelpers::TimesEight), holder);
            fileValueAboveInAccessTable(jit, inAccessTable, answer);
            jit.load64(Address(answer), result);
        });
        answerWhereCellIs(Slot::Kind::Absent, 0, [&] {
            fileAbsenceInAccessTable(jit, inAccessTable, answer);
            jit.move(TrustedImm64(JSValue::ValueUndefined), result);
        });
        answerWhereCellIs(Slot::Kind::Value, PackedSlot::isOutOfLine, [&] {
            makeAnswerInStorageOfHolder(jit, r.cell, answer);
            fileValueInStorageOfHolderInAccessTable(jit, inAccessTable, answer);
            loadValueInStorageOfHolderByAnswer(jit, answer, inAccessTable.place, result);
        });
        if (r.receiverIs == ReceiverOfFrontProbe::IsAnyCell) {
            answerWhereCellIs(Slot::Kind::Value, PackedSlot::isOwn | PackedSlot::isOutOfLine, [&] {
                jit.signExtend32ToPtr(r.cell, answer);
                makeAnswerInOwnStorage(jit, answer);
                fileOwnValueInStorageInAccessTable(jit, inAccessTable, answer);
                loadValueInOwnStorageByAnswer(jit, r.receiver, answer, inAccessTable.place, result);
            });
        }
    }
    readByLoadedFrontCell(jit, { r.receiver, r.cell, r.scratch, r.structureID, r.receiverIs }, result, finish);
#endif
}

void generateGetByIdKeepingRegisters(StubAssembler& jit)
{
#if CPU(X86_64)
    const GPRReg R0 = jit.operand(0);
    const GPRReg name = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T10);
    const GPRReg cell = jit.allocateScratch(StubRegister::T11);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    const GPRReg header = gprOf(StubRegister::T12);
    RELEASE_ASSERT(R0 == jit.result() && noOverlap(R0, name, scratch, cell, row, header));
    auto callAndReturn = [&](const ScopedLambda<void()>& emitCall) {
        jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true, .registersThatCalleeLoses = registersThatWaysOnwardOfGetByIdLose }, emitCall);
    };
    Jump isNotCell = jit.branchIfNotCell(R0);
    JumpList isNotInAccessTable;
    JumpList isNoFieldOfCompiledObject;
    JumpList hasOtherName;
    JumpList isAnsweredByHandler;
    const bool nameHasColumn = jit.key().form() == static_cast<uint8_t>(NameAtSite::HasColumn);
    FieldTableProbe probe { R0, name, row, cell, nameHasColumn, true, false, jit.key().callerFrame() };
    probe.isNotCompiledObject = &isNoFieldOfCompiledObject;
    probe.waysOnwardAreCalls = true;
    returnFieldByTable(jit, probe, isNoFieldOfCompiledObject);
    isNoFieldOfCompiledObject.link(&jit);
    returnValueByAccessTable(jit, { { R0, name, cell, scratch }, row, InvalidGPRReg, true }, isNotInAccessTable);
    auto askFrontTable = [&](ReceiverOfFrontProbe receiverIs) {
        jit.load32(Address(R0, JSCell::structureIDOffset()), cell);
        readByFrontTable(jit, { R0, name, cell, row, scratch, InvalidGPRReg, receiverIs, true }, R0, hasOtherName);
        isAnsweredByHandler.append(jit.jump());
    };
    isNotInAccessTable.link(&jit);
    if (!nameHasColumn)
        hasOtherName.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    Jump isNotCompiledObject = jit.branchTest8(CCallHelpers::Zero, Address(R0, offsetOfShapeWord + 3), TrustedImm32(compiledObjectShapeBit >> 24));
    askFrontTable(ReceiverOfFrontProbe::IsCompiledObject);
    isNotCompiledObject.link(&jit);
    askFrontTable(ReceiverOfFrontProbe::IsAnyCell);

    isAnsweredByHandler.link(&jit);
    callAndReturn(scopedLambda<void()>([&] {
        jit.move(row, header);
        callSlotHandler(jit, slotHandlerTable(SlotOperation::Read, SlotOrigin::ByIdInStubFrame, SlotForm::PackedSlot), header, scratch);
    }));

    hasOtherName.link(&jit);
    callAndReturn(scopedLambda<void()>([&] {
        jit.callStub(getByIdHashed().withCallerFrame(CallerFrame::StubFrame));
    }));

    isNotCell.link(&jit);
    callAndReturn(scopedLambda<void()>([&] {
        jit.callStub((nameHasColumn ? getById() : getByIdCheckingColumn()).withCallerFrame(CallerFrame::StubFrame));
    }));
#else
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

void generateGetById(StubAssembler& jit)
{
    const CallerFrame callerFrame = jit.key().callerFrame();
    const SlotOrigin origin = callerFrame == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    const SlotHandlerTable packedTable = slotHandlerTable(SlotOperation::Read, origin, SlotForm::PackedSlot);
    const SlotHandlerTable unpackedTable = slotHandlerTable(SlotOperation::Read, origin, SlotForm::Slot);
    const GPRReg R0 = jit.operand(0);
    const GPRReg name = jit.immediate();
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T12 = jit.allocateScratch(StubRegister::T12);
    const GPRReg T13 = jit.allocateScratch(StubRegister::T13);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    Box<JumpList> miss = Box<JumpList>::create();
    Label behindFieldTable;

    const GPRReg cell = T12;
    JumpList hasOtherName;
#if CPU(ARM64)
    auto returnToCaller = [&] {
        jit.returnToCaller();
    };
#endif
    Label start = jit.label();
    loadStructureIDOfReceiver(jit, R0, T11, *miss, [&] {
        JumpList isNotInAccessTable;
        JumpList isNoFieldOfCompiledObject;
        jit.bindEntry(getByIdOfCell().withCallerFrame(callerFrame));
        FieldTableProbe probe { R0, name, T14, cell, true, true, false, callerFrame };
        probe.isNotCompiledObject = &isNoFieldOfCompiledObject;
        returnFieldByTable(jit, probe, isNoFieldOfCompiledObject);
        isNoFieldOfCompiledObject.link(&jit);
        returnValueByAccessTable(jit, { { R0, name, T11, T12 }, T14, T10 }, isNotInAccessTable);
        isNotInAccessTable.link(&jit);
        Jump isNotCompiledObject = jit.branchTest8(CCallHelpers::Zero, Address(R0, offsetOfShapeWord + 3), TrustedImm32(compiledObjectShapeBit >> 24));
        behindFieldTable = jit.label();
        jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
        readByFrontTable(jit, { R0, name, T11, cell, T10, T13, ReceiverOfFrontProbe::IsCompiledObject, true }, R0, hasOtherName);
        jumpToSlotHandler(jit, packedTable, cell, T10);

        isNotCompiledObject.link(&jit);
    });
    readByFrontTable(jit, { R0, name, T11, cell, T10, T13, ReceiverOfFrontProbe::IsAnyCell, true }, R0, hasOtherName);
    jumpToSlotHandler(jit, packedTable, cell, T10);

    jit.bindEntry(getByIdAfterFrontProbe().withCallerFrame(callerFrame));
    hasOtherName.append(branchIfCellNameDiffers(jit, name, cell, T10));
#if CPU(ARM64)
    readByLoadedFrontCell(jit, { R0, cell, T10, T11, ReceiverOfFrontProbe::IsAnyCell }, R0, returnToCaller);
#endif
    jumpToSlotHandler(jit, packedTable, cell, T10);

    const GPRReg uid = T10;
    const GPRReg structureID = T11;
    const GPRReg header = T12;
    const GPRReg payload = T13;
    const GPRReg entry = T14;
    jit.bindEntry(getByIdCheckingColumn().withCallerFrame(callerFrame));
    jit.branch32(CCallHelpers::BelowOrEqual, name, TrustedImm32(RowDisplacementTable::maxColumn)).linkTo(start, &jit);
    hasOtherName.link(&jit);
    jit.bindEntry(getByIdHashed().withCallerFrame(callerFrame));
    loadStructureIDOfReceiver(jit, R0, structureID, *miss);
    loadIdentifier(jit, name, uid, *miss);
    JumpList isInNoTable;
    findHashedSlot(jit, SlotAccess::Read, { structureID, uid, entry, header, payload, payload }, isInNoTable);
    jumpToSlotHandler(jit, unpackedTable, header, uid);

    isInNoTable.link(&jit);
    countMissOfPair(jit, structureID, uid, header, *miss);
    JumpList isNotAnsweredByLayout;
    returnFieldByTable(jit, { R0, name, header, payload, false, false, true }, isNotAnsweredByLayout);
    isNotAnsweredByLayout.link(&jit);
    answerFromObjectsAndReturn(jit, ObjectLookupQuestion::Read, { R0, name, uid, header, structureID, payload, entry, IndexRegisterOfObjectLookup(jit) }, ObjectLookupName::IsGivenByNumber, *miss);

    JumpList isNotAnswered;
    answerMissingField(jit, callerFrame, R0, name, T10, T11, T12, T13, isNotAnswered);
    isNotAnswered.linkTo(behindFieldTable, &jit);

    jit.bindEntry(getByIdOfSymbolOfRealm().withCallerFrame(callerFrame));
    if (jit.options().hasOneInstance)
        jit.breakpoint();
    else {
        loadStructureIDOfReceiver(jit, R0, structureID, *miss, [&] {
            JumpList looksUpByUID;
            loadStructure(jit, R0, T10);
            looksUpByUID.append(branchIfNotOfThisRealm(jit, T10));
            returnFieldByTable(jit, { R0, name, T14, T12, false, true, false, callerFrame }, looksUpByUID);
            looksUpByUID.link(&jit);
        });
        loadProgramSymbol(jit, name, uid, *miss);
        findHashedSlot(jit, SlotAccess::Read, { structureID, uid, entry, header, payload, payload }, *miss);
        jumpToSlotHandler(jit, unpackedTable, header, uid);
    }
    jit.addOutOfLinePart([miss, unpackedTable](StubAssembler& jit) {
        linkToSlotExit(jit, *miss, unpackedTable, SlotExit::Miss);
    });
}

static Entry cachingOperationByNameFor(PresenceQuestion question)
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
    const GPRReg R0 = jit.operand(0);
    const GPRReg name = jit.immediate();
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg structureID = jit.allocateScratch(StubRegister::T11);
    const GPRReg T12 = jit.allocateScratch(StubRegister::T12);
    const GPRReg T13 = jit.allocateScratch(StubRegister::T13);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    JumpList isNotInFront;
    JumpList miss;
    JumpList isNotInFieldTable;
    JumpList keyOrderSays;
    miss.append(jit.branchIfNotCell(R0));
    Label asksCell = jit.label();
    auto emitPresenceBySlot = [&](const ProbedSlot& probed, JumpList& isUnknown) {
        Jump isNotField = branchUnlessSlotNamesField(jit, probed, T10);
        returnFieldPresence(jit, CachedSlot(probed.form, Slot::Kind::Field, probed.header, probed.payload), { R0, T10, structureID, structureID, T14 }, miss, &keyOrderSays);

        isNotField.link(&jit);
        returnPresenceBySlot(jit, question, probed, R0, asksCell, isUnknown);
    };

    auto emitReturnFalse = [&] {
        jit.move(TrustedImm32(0), R0);
        jit.returnToCaller();
    };
    const ScopedLambda<void()> returnFalse = emitReturnFalse;
    readFieldByTable(jit, { R0, name, T12, T13, false, true, true }, T12, isNotInFieldTable, [&] {
        jit.move(TrustedImm32(1), R0);
        jit.returnToCaller();
    }, &returnFalse);
    isNotInFieldTable.link(&jit);
    miss.append(jit.branchIfNotObject(R0));
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    isNotInFront.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    loadFrontCell(jit, SlotAccess::Read, { structureID, name, T13, T12, T10 });
    isNotInFront.append(branchIfCellNameDiffers(jit, name, T12, T10));
    emitPresenceBySlot({ SlotForm::PackedSlot, T12 }, isNotInFront);

    isNotInFront.link(&jit);
    loadIdentifier(jit, name, T10, miss);
    JumpList isInNoTable;
    findHashedSlot(jit, SlotAccess::Read, { structureID, T10, T14, T12, T13, T13 }, isInNoTable);
    JumpList slotDoesNotSay;
    emitPresenceBySlot({ SlotForm::Slot, T12, T13 }, slotDoesNotSay);
    slotDoesNotSay.link(&jit);
    if (question == PresenceQuestion::In) {
        miss.append(branchIfSlotKindDiffers(jit, { SlotForm::Slot, T12, T13 }, Slot::Kind::Uncacheable, T10));
        jit.linkToStub(jit.branchTest64(CCallHelpers::NonZero, T13), throughProxy(TrapOfProxy::Has));
    }
    miss.append(jit.jump());

    isInNoTable.link(&jit);
    countMissOfPair(jit, structureID, T10, T12, miss);
    answerFromObjectsAndReturn(jit, question == PresenceQuestion::In ? ObjectLookupQuestion::In : ObjectLookupQuestion::HasOwn, { R0, name, T10, T12, structureID, T13, T14, IndexRegisterOfObjectLookup(jit) }, ObjectLookupName::IsGivenByNumber, miss);

    miss.link(&jit);
    jit.exitOnMiss(cachingOperationByNameFor(question), { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = R0, .thirdArgumentIn = name });

    keyOrderSays.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTHasExtraProperty, { .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = R0, .thirdArgumentIn = name });
}

StubSignature signatureOfReadFieldInExtension(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(firstStubOperandRegister) });
}

void generateReadFieldInExtension(StubAssembler& jit)
{
    static_assert(offsetOfFirstExtensionSlot == extensionGranuleSize && extensionGranuleSize == sizeof(uint64_t) && maxExtensionGranules == 64);
    static_assert(std::endian::native == std::endian::little);
    const SlotInGranule slot = static_cast<SlotInGranule>(jit.key().form());
    const unsigned granule = jit.key().count();
    const GPRReg object = jit.operand(0);
    const GPRReg extension = jit.result();
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg granuleBits = ownedScratch;
    RELEASE_ASSERT(granuleBits != object && granuleBits != extension);
    JumpList hasNoStorage;
    auto returnWordAt = [&](auto address) {
        switch (widthOf(slot)) {
        case 1:
            jit.load8(address, jit.result());
            break;
        case 4:
            jit.load32(address, jit.result());
            break;
        case 8:
            jit.load64(address, jit.result());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        jit.returnToCaller();
    };

    if (jit.options().useWideReferences) {
        jit.load64(Address(object, offsetOfFirstField), extension);
        hasNoStorage.append(jit.branchTest64(CCallHelpers::Zero, extension));
    } else {
        jit.load32(Address(object, offsetOfFirstField), extension);
        jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, extension, static_cast<CCallHelpers::Scale>(cell32Shift), offsetOfEmptyExtensionInCage), extension);
    }
    if (!granule) {
        hasNoStorage.append(jit.branchTest8(CCallHelpers::Zero, Address(extension, offsetOfExtensionGranuleBits), TrustedImm32(1)));
        returnWordAt(Address(extension, offsetOfFirstExtensionSlot + offsetInGranuleOf(slot)));
    } else {
        jit.load64(Address(extension, offsetOfExtensionGranuleBits), granuleBits);
        if (unsigned laterGranules = maxExtensionGranules - 1 - granule)
            jit.lshift64(TrustedImm32(laterGranules), granuleBits);
        hasNoStorage.append(jit.branchTest64(CCallHelpers::PositiveOrZero, granuleBits));
        jit.countPopulation64(granuleBits, granuleBits, CCallHelpers::fpTempRegister);
        returnWordAt(BaseIndex(extension, granuleBits, CCallHelpers::TimesEight, offsetInGranuleOf(slot)));
    }

    hasNoStorage.link(&jit);
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
