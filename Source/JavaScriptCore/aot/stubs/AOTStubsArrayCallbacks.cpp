/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrayCallbacks.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeRecords.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyKeys.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "JSObject.h"
#include "JSType.h"
#include "PureNaN.h"
#include <array>
#include <bit>
#include <initializer_list>
#include <utility>

namespace JSC { namespace AOT {

namespace {

struct CallbackRegisters {
    GPRReg callback { argumentGPR(0) };
    GPRReg answer { argumentGPR(1) };
    GPRReg flags { argumentGPR(2) };
    GPRReg operand { argumentGPR(3) };
    GPRReg end;
    GPRReg cursor;
    GPRReg value;
    GPRReg methodWord;
    GPRReg scratchOfMethodWord;
    GPRReg place;
    GPRReg distance { InvalidGPRReg };

    static CallbackRegisters allocate(StubAssembler&);
};

struct CallbackExits {
    JumpList lookups;
    JumpList methodCalls;
    JumpList bailouts;
};

enum class Elements : uint8_t {
    Values,
    Doubles,
    FieldsByStructure,
    FieldsInCellByStructure,
    FieldsByShape,
};

enum class AsksOtherAnswers : bool { No, Yes };

enum class EmptyField : bool { IsLeftToTest, BailsOut };

struct ColdParts {
    Vector<std::pair<JumpList, Label>, 4> unanswered;
};

} // anonymous namespace

CallbackRegisters CallbackRegisters::allocate(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    CallbackRegisters result;
    result.methodWord = registers.word;
    result.scratchOfMethodWord = registers.scratch;
    result.value = jit.allocateScratch(StubRegister::T10);
#if CPU(X86_64)
    result.end = registers.word;
    result.cursor = registers.scratch;
    result.place = result.callback;
#else
    result.distance = argumentGPR(4);
    result.end = argumentGPR(5);
    result.cursor = argumentGPR(6);
    result.place = argumentGPR(7);
#endif
    RELEASE_ASSERT(noOverlap(jit.immediate(), thisGPR, result.callback, result.answer, result.flags, result.operand, result.end, result.cursor, result.value, instanceGPR));
    RELEASE_ASSERT(noOverlap(jit.immediate(), thisGPR, result.place, result.answer, result.flags, result.operand, result.end, result.cursor, result.value, instanceGPR));
    return result;
}

static CallbackSource sourceOf(unsigned number) { return static_cast<CallbackSource>(number); }

static void saveCallback(StubAssembler& jit, const CallbackRegisters& registers)
{
#if CPU(X86_64)
    jit.push(registers.answer);
    jit.push(registers.callback);
#else
    jit.move(registers.answer, registers.distance);
#endif
}

static void discardCallback([[maybe_unused]] StubAssembler& jit)
{
#if CPU(X86_64)
    jit.addPtr(TrustedImm32(2 * sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
}

static void restoreCallback([[maybe_unused]] StubAssembler& jit, [[maybe_unused]] const CallbackRegisters& registers)
{
#if CPU(X86_64)
    jit.pop(registers.callback);
    jit.pop(registers.answer);
#endif
}

static void loadMethodWord(StubAssembler& jit, GPRReg flags, GPRReg word)
{
    jit.urshift32(flags, TrustedImm32(ArrayCallbackFlags::methodShift), word);
    jit.load64(BaseIndex(instanceGPR, word, CCallHelpers::TimesEight, static_cast<int32_t>(AOT_INSTANCE_OFFSET(stubs.wordsOfCompiledMethodsOfArray))), word);
}

namespace {

struct ArrayCheck {
    Jump isOther;
    Label holdsValues;
};

} // anonymous namespace

static ArrayCheck checkArray(StubAssembler& jit, const CallbackRegisters& registers, CallbackExits& exits)
{
    static_assert(JSCell::cellStateOffset() == sizeof(uint64_t) - 1);
    exits.lookups.append(jit.branchIfNotCell(thisGPR));
    exits.lookups.append(branchIfNotPristine(jit, thisGPR));
    jit.load64(Address(thisGPR), registers.value);
    jit.xor64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader)), registers.value);
    jit.lshift64(TrustedImm32(8), registers.value);
    Jump isOther = jit.branchTest64(CCallHelpers::NonZero, registers.value);
    return { isOther, jit.label() };
}

static void loadShape(StubAssembler& jit, GPRReg shape)
{
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
}

static Jump checkOtherArray(StubAssembler& jit, const CallbackRegisters& registers, const ArrayCheck& check, CallbackSource source, bool takesDoubles, CallbackExits& exits)
{
    check.isOther.link(&jit);
    exits.lookups.append(jit.branchIfNotType(thisGPR, ArrayType));
    loadStructure(jit, thisGPR, registers.value);
    exits.lookups.append(branchIfNotOfThisRealm(jit, registers.value));
    exits.methodCalls.append(jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader))));
    loadShape(jit, registers.value);
    jit.branch32(CCallHelpers::Equal, registers.value, TrustedImm32(ContiguousShape)).linkTo(check.holdsValues, &jit);
    Jump holdsDoubles;
    if (source == CallbackSource::Element) {
        jit.branch32(CCallHelpers::Equal, registers.value, TrustedImm32(Int32Shape)).linkTo(check.holdsValues, &jit);
        if (takesDoubles)
            holdsDoubles = jit.branch32(CCallHelpers::Equal, registers.value, TrustedImm32(DoubleShape));
    }
    exits.methodCalls.append(jit.jump());
    return holdsDoubles;
}

static void loadCursorAndEnd(StubAssembler& jit, const CallbackRegisters& registers)
{
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), registers.cursor);
    jit.load32(Address(registers.cursor, Butterfly::offsetOfPublicLength()), registers.end);
    jit.getEffectiveAddress(BaseIndex(registers.cursor, registers.end, CCallHelpers::TimesEight), registers.end);
}

static Jump branchAtEnd(StubAssembler& jit, const CallbackRegisters& registers)
{
    return jit.branchPtr(CCallHelpers::AboveOrEqual, registers.cursor, registers.end);
}

#if CPU(X86_64)
static constexpr GPRReg scratchOfLoad = CCallHelpers::s_scratchRegister;
#else
static constexpr GPRReg scratchOfLoad = InvalidGPRReg;
#endif

static void bailOutUnlessCell(StubAssembler& jit, GPRReg value, JumpList& bailouts)
{
#if CPU(X86_64)
    constexpr GPRReg scratch = scratchOfLoad;
    constexpr int32_t leastAddressOfCell = 16;
    constexpr unsigned bitsOfAddress = 48;
    static_assert(static_cast<uint64_t>(JSValue::NumberTag) >> bitsOfAddress && JSValue::DoubleEncodeOffset >> bitsOfAddress && JSValue::ValueUndefined < leastAddressOfCell && JSValue::ValueTrue < leastAddressOfCell);
    jit.add64(TrustedImm32(-leastAddressOfCell), value, scratch);
    jit.urshift64(TrustedImm32(bitsOfAddress), scratch);
    bailouts.append(jit.branchTest64(CCallHelpers::NonZero, scratch));
#else
    bailouts.append(jit.branchIfNotCell(value));
    bailouts.append(jit.branchTest64(CCallHelpers::Zero, value));
#endif
}

static void loadPlaceByStructure(StubAssembler& jit, GPRReg index, GPRReg structure, GPRReg place, JumpList& unanswered)
{
    using Form = SiteAnswers::Form;
    static_assert(!SiteAnswers::bitsOf(SiteUse::Read, Form::OwnValueInStorage));
    constexpr ptrdiff_t offsetOfNewest = OBJECT_OFFSETOF(SiteAnswers, second);
    auto word = [&](ptrdiff_t offset) { return BaseIndex(instanceGPR, index, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offset)); };
    auto bitsOf = [](Form form) { return TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(SiteUse::Read, form))); };
    jit.load32(word(offsetOfNewest + OBJECT_OFFSETOF(SiteAnswers::Answer, key)), place);
    jit.xor32(structure, place);
    Jump isNoValue = jit.branchTest32(CCallHelpers::NonZero, place);
    jit.load32(word(offsetOfNewest + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), place);
    jit.signExtend32ToPtr(place, place);
    jit.lshift64(TrustedImm32(3), place);
    JumpList hasPlace;
    hasPlace.append(jit.jump());
    isNoValue.link(&jit);
    Jump isField = jit.branch32(CCallHelpers::Equal, place, bitsOf(Form::OwnFieldInCell));
    unanswered.append(branchUnlessIndexIsOfWideAnswers(jit, index, place));
    std::array<Jump, WideSiteAnswers::numberOfFurtherAnswers> hasFurtherAnswer;
    for (unsigned number = 0; number < hasFurtherAnswer.size(); ++number) {
        jit.load32(word(OBJECT_OFFSETOF(WideSiteAnswers, structuresOfFurtherAnswers) + number * sizeof(uint32_t)), place);
        hasFurtherAnswer[number] = jit.branch32(CCallHelpers::Equal, place, structure);
    }
    unanswered.append(jit.jump());
    for (unsigned number = 0; number < hasFurtherAnswer.size(); ++number) {
        hasFurtherAnswer[number].link(&jit);
        jit.load8(word(OBJECT_OFFSETOF(WideSiteAnswers, formsOfFurtherAnswersHeld) + number * sizeof(WideSiteAnswers::FormOfFurtherAnswer) + OBJECT_OFFSETOF(WideSiteAnswers::FormOfFurtherAnswer, bits)), place);
        Jump isFurtherValue = jit.branch32(CCallHelpers::Equal, place, bitsOf(Form::OwnValueInCell));
        unanswered.append(jit.branch32(CCallHelpers::NotEqual, place, bitsOf(Form::OwnFieldInCell)));
        isFurtherValue.link(&jit);
        jit.load32(word(OBJECT_OFFSETOF(WideSiteAnswers, operandsOfFurtherAnswers) + number * sizeof(uint32_t)), place);
        hasPlace.append(jit.jump());
    }

    isField.link(&jit);
    jit.load32(word(offsetOfNewest + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), place);
    hasPlace.link(&jit);
}

static Jump loadPlaceOfField(StubAssembler& jit, const CallbackRegisters& registers, JumpList& bailouts, bool mayBeEmpty = true)
{
    static_assert(InstanceLayout::sizeOfIndexOfSiteAnswers == sizeof(uint16_t));
    static_assert(!OBJECT_OFFSETOF(SiteAnswers::Answer, key) && OBJECT_OFFSETOF(SiteAnswers::Answer, operand) == sizeof(uint32_t));
    static_assert(SiteAnswers::mostCommonForm(SiteUse::Read) == SiteAnswers::Form::OwnValueInStorage && !SiteAnswers::bitsOf(SiteUse::Read, SiteAnswers::Form::OwnValueInStorage));
    auto loadAnswer = [&](ptrdiff_t offsetOfAnswer, GPRReg loaded) {
        jit.load64(BaseIndex(instanceGPR, registers.answer, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offsetOfAnswer)), loaded);
    };
    JumpList isUnanswered;
    jit.load16(BaseIndex(instanceGPR, registers.answer, CCallHelpers::TimesOne), registers.answer);
    Jump isEmpty = mayBeEmpty ? branchAtEnd(jit, registers) : Jump();
    jit.load64(Address(registers.cursor), registers.value);
    bailOutUnlessCell(jit, registers.value, bailouts);
    loadAnswer(OBJECT_OFFSETOF(SiteAnswers, first), registers.place);
    Jump hasOtherShape = jit.branch32(CCallHelpers::NotEqual, Address(registers.value, offsetOfShapeWord), registers.place);
    jit.zeroExtend32ToWord(registers.place, registers.answer);
    jit.urshift64(registers.place, TrustedImm32(32), registers.place);
    Jump hasShape = jit.jump();

    hasOtherShape.link(&jit);
    loadAnswer(OBJECT_OFFSETOF(SiteAnswers, second), registers.place);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, Address(registers.value, JSCell::structureIDOffset()), registers.place);
    jit.zeroExtend32ToWord(registers.place, registers.answer);
    jit.rshift64(registers.place, TrustedImm32(32), registers.place);
    jit.lshift64(TrustedImm32(3), registers.place);
    JumpList isOfStructure;
    isOfStructure.append(jit.jump());

    hasOtherStructure.link(&jit);
    jit.load32(Address(registers.value, JSCell::structureIDOffset()), registers.value);
    loadPlaceByStructure(jit, registers.answer, registers.value, registers.place, isUnanswered);
    jit.move(registers.value, registers.answer);
    jit.load64(Address(registers.cursor), registers.value);
    isOfStructure.append(jit.jump());
    isUnanswered.link(&jit);
    jit.load64(Address(registers.cursor), registers.value);
    if (mayBeEmpty)
        isEmpty.link(&jit);
    loadAnswer(OBJECT_OFFSETOF(SiteAnswers, second), registers.answer);
    jit.rshift64(registers.answer, TrustedImm32(32), registers.place);
    jit.lshift64(TrustedImm32(3), registers.place);
    Jump isOfCommonForm = jit.branchTest32(CCallHelpers::Zero, registers.answer, TrustedImm32(SiteAnswers::formMask));
    jit.move(TrustedImm32(0), registers.answer);
    isOfCommonForm.link(&jit);
    jit.zeroExtend32ToWord(registers.answer, registers.answer);
    isOfStructure.link(&jit);
    return hasShape;
}

static void advance(StubAssembler& jit, const CallbackRegisters& registers, Label next)
{
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), registers.cursor);
    jit.branchPtr(CCallHelpers::Below, registers.cursor, registers.end).linkTo(next, &jit);
}

static void loadValue(StubAssembler& jit, const CallbackRegisters& registers, Elements elements, ColdParts& coldParts, JumpList& bailouts, EmptyField emptyField = EmptyField::BailsOut, Jump hasObject = { })
{
    const GPRReg value = registers.value;
    jit.load64(Address(registers.cursor), value);
    if (elements == Elements::Values) {
        bailouts.append(jit.branchTest64(CCallHelpers::Zero, value));
        return;
    }
    const bool byShape = elements == Elements::FieldsByShape;
    bailOutUnlessCell(jit, value, bailouts);
    if (hasObject.isSet())
        hasObject.link(&jit);
    JumpList isUnanswered;
    isUnanswered.append(jit.branch32(CCallHelpers::NotEqual, Address(value, byShape ? offsetOfShapeWord : JSCell::structureIDOffset()), registers.answer));
    if (elements == Elements::FieldsByStructure) {
#if CPU(X86_64)
        const GPRReg storage = scratchOfLoad;
#else
        const OwnedScratchRegister ownedScratch(jit);
        const GPRReg storage = ownedScratch;
#endif
        jit.loadPtr(Address(value, JSObject::butterflyOffset()), storage);
        jit.moveConditionallyTest64(CCallHelpers::Signed, registers.place, registers.place, storage, value);
    }
    jit.load64(BaseIndex(value, registers.place, CCallHelpers::TimesOne), value);
    if (emptyField == EmptyField::BailsOut)
        bailouts.append(jit.branchTest64(CCallHelpers::Zero, value));
    coldParts.unanswered.append({ isUnanswered, jit.label() });
}

static void readByOtherAnswer(StubAssembler& jit, const CallbackRegisters& registers, const Invocable<void(GPRReg, size_t)> auto& loadDistance, Label hasValue)
{
    const GPRReg value = registers.value;
#if CPU(X86_64)
    const GPRReg index = registers.answer;
    const GPRReg place = registers.place;
    jit.push(index);
    jit.push(place);
    auto restoreRegisters = [&] {
        jit.pop(place);
        jit.pop(index);
    };
    loadDistance(index, 2 * sizeof(CPURegister));
#else
    const GPRReg index = jit.reuseScratch(StubRegister::T11);
    const GPRReg place = jit.reuseScratch(StubRegister::T12);
    auto restoreRegisters = [] { };
    loadDistance(index, 0);
#endif
    auto word = [&](ptrdiff_t offset) { return BaseIndex(instanceGPR, index, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offset)); };
    JumpList unanswered;
    jit.load16(BaseIndex(instanceGPR, index, CCallHelpers::TimesOne), index);
    jit.load32(word(OBJECT_OFFSETOF(SiteAnswers, first) + OBJECT_OFFSETOF(SiteAnswers::Answer, key)), place);
    Jump hasOtherShape = jit.branch32(CCallHelpers::NotEqual, Address(value, offsetOfShapeWord), place);
    jit.load32(word(OBJECT_OFFSETOF(SiteAnswers, first) + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), place);
    Jump isInCell = jit.jump();
    hasOtherShape.link(&jit);
    jit.load32(Address(value, JSCell::structureIDOffset()), value);
    loadPlaceByStructure(jit, index, value, place, unanswered);
    jit.load64(Address(registers.cursor), value);
    Jump isInCellByStructure = jit.branchTest64(CCallHelpers::PositiveOrZero, place);
    jit.loadPtr(Address(value, JSObject::butterflyOffset()), value);
    isInCell.link(&jit);
    isInCellByStructure.link(&jit);
    jit.load64(BaseIndex(value, place, CCallHelpers::TimesOne), value);
    restoreRegisters();
    jit.branchTest64(CCallHelpers::NonZero, value).linkTo(hasValue, &jit);
    Jump isEmpty = jit.jump();
    unanswered.link(&jit);
    restoreRegisters();
    isEmpty.link(&jit);
    jit.load64(Address(registers.cursor), value);
}

static void emitColdParts(StubAssembler& jit, const CallbackRegisters& registers, ColdParts& coldParts, JumpList& bailouts, std::initializer_list<GPRReg> kept, const Invocable<void(GPRReg, size_t)> auto& loadDistance, const Invocable<void()> auto& restoreNumbers, AsksOtherAnswers asksOtherAnswers = AsksOtherAnswers::Yes)
{
    for (auto& [isUnanswered, hasValue] : coldParts.unanswered) {
        isUnanswered.link(&jit);
        if (asksOtherAnswers == AsksOtherAnswers::Yes)
            readByOtherAnswer(jit, registers, loadDistance, hasValue);
        jit.preserving(kept, [&] {
            loadDistance(gprOf(shortStubImmediateRegister), WTF::roundUpToMultipleOf<stackAlignmentBytes()>(kept.size() * sizeof(CPURegister)));
            jit.move(registers.value, gprOf(receiverAtSiteRegister));
            jit.move(registers.value, gprOf(firstStubOperandRegister));
            jit.callStub(readAtSiteRunningNoCode(ReceiverAtSite::IsCell, true));
            jit.move(GPRInfo::returnValueGPR, registers.value);
        });
        restoreNumbers();
        bailouts.append(jit.branchTest64(CCallHelpers::Zero, registers.value));
        jit.jump().linkTo(hasValue, &jit);
    }
    coldParts = { };
}

static void emitFramelessColdParts(StubAssembler& jit, const CallbackRegisters& registers, ColdParts& coldParts, JumpList& bailouts, std::initializer_list<GPRReg> kept, const Invocable<void()> auto& keepNumbers, const Invocable<void()> auto& restoreNumbers)
{
    for (auto& part : coldParts.unanswered) {
        part.first.link(&jit);
        auto loadDistanceWithoutFrame = [&](GPRReg distance, [[maybe_unused]] size_t bytesBelow) {
#if CPU(X86_64)
            jit.load64(Address(CCallHelpers::stackPointerRegister, sizeof(CPURegister) + bytesBelow), distance);
#else
            jit.move(registers.distance, distance);
#endif
        };
        readByOtherAnswer(jit, registers, loadDistanceWithoutFrame, part.second);
        keepNumbers();
        jit.emitFunctionPrologue();
        JumpList hasFrame;
        hasFrame.append(jit.jump());
        part.first = hasFrame;
    }
    auto loadDistance = [&](GPRReg distance, size_t) {
#if CPU(X86_64)
        jit.load64(Address(GPRInfo::callFrameRegister, 2 * sizeof(CPURegister)), distance);
#else
        jit.move(registers.distance, distance);
#endif
    };
    emitColdParts(jit, registers, coldParts, bailouts, kept, loadDistance, [&] {
        jit.emitFunctionEpilogueWithEmptyFrame();
        restoreNumbers();
    }, AsksOtherAnswers::No);
}

namespace {

class FieldPairReader {
public:
    FieldPairReader(StubAssembler& jit, const CallbackRegisters& registers, GPRReg placeOfSecond, GPRReg second, bool isNested)
        : m_jit(jit)
        , m_registers(registers)
        , m_placeOfSecond(placeOfSecond)
        , m_second(second)
        , m_isNested(isNested)
    {
    }

    JumpList loadPlaces(const Invocable<void(GPRReg)> auto& loadDistances, JumpList& bailouts);
    void loadFields(Elements, JumpList& bailouts);
    void emitColdParts(JumpList& bailouts, std::initializer_list<GPRReg> kept, const Invocable<void(GPRReg, size_t)> auto& loadDistances, const Invocable<void()> auto& enter, const Invocable<void()> auto& leave);
    ColdParts& coldParts() { return m_coldParts; }
    void useColdParts(Vector<std::pair<JumpList, Label>, 4>& parts) { m_unanswered = &parts; }
    void emitForEachForm(const Invocable<void(Elements)> auto& emit, JumpList& hasShape);

private:
    void loadObject(JumpList& bailouts);
    void loadAnswer(GPRReg index, ptrdiff_t offsetOfAnswer, GPRReg loaded)
    {
        m_jit.load64(BaseIndex(instanceGPR, index, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offsetOfAnswer)), loaded);
    }
    void loadIndices(const Invocable<void(GPRReg)> auto& loadDistances);
    void loadField(GPRReg place, GPRReg loaded, bool mayBeInStorage);
    JumpList loadPlacesOfFirstObject(const Invocable<void(GPRReg)> auto& loadDistances, JumpList& unanswered, JumpList& bailouts);
    JumpList loadPlacesOfNestedObjects(const Invocable<void(GPRReg)> auto& loadDistances, JumpList& unanswered, JumpList& bailouts);

    StubAssembler& m_jit;
    const CallbackRegisters& m_registers;
    const GPRReg m_placeOfSecond;
    const GPRReg m_second;
    const bool m_isNested;
    ColdParts m_coldParts;
    Vector<std::pair<JumpList, Label>, 4>* m_unanswered { &m_coldParts.unanswered };
};

void FieldPairReader::loadObject(JumpList& bailouts)
{
    m_jit.load64(Address(m_registers.cursor), m_registers.value);
    bailOutUnlessCell(m_jit, m_registers.value, bailouts);
}

void FieldPairReader::loadIndices(const Invocable<void(GPRReg)> auto& loadDistances)
{
    const GPRReg answer = m_registers.answer;
    loadDistances(m_second);
    m_jit.zeroExtend32ToWord(m_second, answer);
    m_jit.urshift64(TrustedImm32(32), m_second);
    m_jit.load16(BaseIndex(instanceGPR, answer, CCallHelpers::TimesOne), answer);
    m_jit.load16(BaseIndex(instanceGPR, m_second, CCallHelpers::TimesOne), m_second);
}

void FieldPairReader::loadField(GPRReg place, GPRReg loaded, bool mayBeInStorage)
{
    if (mayBeInStorage) {
        Jump isInCell = m_jit.branchTest64(CCallHelpers::PositiveOrZero, place);
        m_jit.loadPtr(Address(loaded, JSObject::butterflyOffset()), loaded);
        isInCell.link(&m_jit);
    }
    m_jit.load64(BaseIndex(loaded, place, CCallHelpers::TimesOne), loaded);
}

JumpList FieldPairReader::loadPlacesOfNestedObjects(const Invocable<void(GPRReg)> auto& loadDistances, JumpList& unanswered, JumpList& bailouts)
{
    StubAssembler& jit = m_jit;
    const GPRReg value = m_registers.value;
    const GPRReg place = m_registers.place;
    const GPRReg answer = m_registers.answer;
    auto loadPlaceByShape = [&](GPRReg index, GPRReg shape, GPRReg placeOfField, JumpList& hasOtherShape) {
        loadAnswer(index, OBJECT_OFFSETOF(SiteAnswers, first), placeOfField);
        hasOtherShape.append(jit.branch32(CCallHelpers::NotEqual, placeOfField, shape));
        jit.urshift64(TrustedImm32(32), placeOfField);
    };
    auto loadPlacesInForm = [&](bool byShape, JumpList& otherForm) {
        auto loadWord = [&](GPRReg loaded) { jit.load32(Address(value, byShape ? offsetOfShapeWord : JSCell::structureIDOffset()), loaded); };
        auto loadPlaceOf = [&](GPRReg index, GPRReg word, GPRReg placeOfField) {
            if (byShape)
                loadPlaceByShape(index, word, placeOfField, otherForm);
            else
                loadPlaceByStructure(jit, index, word, placeOfField, otherForm);
        };
        loadObject(bailouts);
        loadIndices(loadDistances);
        loadWord(m_placeOfSecond);
        loadPlaceOf(answer, m_placeOfSecond, place);
        jit.move(m_placeOfSecond, answer);
        loadField(place, value, !byShape);
        bailOutUnlessCell(jit, value, bailouts);
        loadWord(value);
        loadPlaceOf(m_second, value, m_placeOfSecond);
        jit.move(value, m_second);
    };
    JumpList hasOtherShape;
    loadPlacesInForm(true, hasOtherShape);
    JumpList hasShape;
    hasShape.append(jit.jump());
    hasOtherShape.link(&jit);
    loadPlacesInForm(false, unanswered);
    return hasShape;
}

JumpList FieldPairReader::loadPlacesOfFirstObject(const Invocable<void(GPRReg)> auto& loadDistances, JumpList& unanswered, JumpList& bailouts)
{
    if (m_isNested)
        return loadPlacesOfNestedObjects(loadDistances, unanswered, bailouts);
    StubAssembler& jit = m_jit;
    const GPRReg value = m_registers.value;
    const GPRReg place = m_registers.place;
    const GPRReg answer = m_registers.answer;
    loadObject(bailouts);
    loadIndices(loadDistances);
    jit.load32(Address(value, offsetOfShapeWord), m_placeOfSecond);
    loadAnswer(answer, OBJECT_OFFSETOF(SiteAnswers, first), place);
    JumpList hasOtherShape;
    hasOtherShape.append(jit.branch32(CCallHelpers::NotEqual, m_placeOfSecond, place));
    loadAnswer(m_second, OBJECT_OFFSETOF(SiteAnswers, first), m_second);
    hasOtherShape.append(jit.branch32(CCallHelpers::NotEqual, m_placeOfSecond, m_second));
    jit.move(m_placeOfSecond, answer);
    jit.urshift64(place, TrustedImm32(32), place);
    jit.urshift64(m_second, TrustedImm32(32), m_placeOfSecond);
    JumpList hasShape;
    hasShape.append(jit.jump());

    hasOtherShape.link(&jit);
    loadIndices(loadDistances);
    jit.load32(Address(value, JSCell::structureIDOffset()), value);
    JumpList isUnanswered;
    loadPlaceByStructure(jit, answer, value, place, isUnanswered);
    loadPlaceByStructure(jit, m_second, value, m_placeOfSecond, isUnanswered);
    jit.move(value, answer);
    Jump isOfStructure = jit.jump();
    isUnanswered.link(&jit);
    jit.load64(Address(m_registers.cursor), value);
    unanswered.append(jit.jump());
    isOfStructure.link(&jit);
    return hasShape;
}

JumpList FieldPairReader::loadPlaces(const Invocable<void(GPRReg)> auto& loadDistances, JumpList& bailouts)
{
    JumpList unanswered;
    JumpList hasShape = loadPlacesOfFirstObject(loadDistances, unanswered, bailouts);
    Jump isOfStructure = m_jit.jump();
    m_unanswered->append({ unanswered, m_jit.label() });
    hasShape.append(loadPlacesOfFirstObject(loadDistances, bailouts, bailouts));
    isOfStructure.link(&m_jit);
    return hasShape;
}

void FieldPairReader::emitForEachForm(const Invocable<void(Elements)> auto& emit, JumpList& hasShape)
{
    JumpList isInStorage;
    isInStorage.append(m_jit.branchTest64(CCallHelpers::Signed, m_registers.place));
    isInStorage.append(m_jit.branchTest64(CCallHelpers::Signed, m_placeOfSecond));
    emit(Elements::FieldsInCellByStructure);
    JumpList isDone;
    isDone.append(m_jit.jump());
    isInStorage.link(&m_jit);
    emit(Elements::FieldsByStructure);
    isDone.append(m_jit.jump());
    hasShape.link(&m_jit);
    emit(Elements::FieldsByShape);
    isDone.link(&m_jit);
}

void FieldPairReader::loadFields(Elements elements, JumpList& bailouts)
{
    StubAssembler& jit = m_jit;
    const GPRReg value = m_registers.value;
    const bool byShape = elements == Elements::FieldsByShape;
    const bool mayBeInStorage = elements == Elements::FieldsByStructure;
    loadObject(bailouts);
    const Address word(value, byShape ? offsetOfShapeWord : JSCell::structureIDOffset());
    JumpList isUnanswered;
    isUnanswered.append(jit.branch32(CCallHelpers::NotEqual, word, m_registers.answer));
    if (m_isNested) {
        loadField(m_registers.place, value, mayBeInStorage);
        bailOutUnlessCell(jit, value, bailouts);
        isUnanswered.append(jit.branch32(CCallHelpers::NotEqual, word, m_second));
        loadField(m_placeOfSecond, value, mayBeInStorage);
        bailouts.append(jit.branchTest64(CCallHelpers::Zero, value));
    } else {
        jit.move(value, m_second);
        loadField(m_placeOfSecond, m_second, mayBeInStorage);
        loadField(m_registers.place, value, mayBeInStorage);
    }
    m_unanswered->append({ isUnanswered, jit.label() });
}

void FieldPairReader::emitColdParts(JumpList& bailouts, std::initializer_list<GPRReg> kept, const Invocable<void(GPRReg, size_t)> auto& loadDistances, const Invocable<void()> auto& enter, const Invocable<void()> auto& leave)
{
    StubAssembler& jit = m_jit;
    const GPRReg value = m_registers.value;
    for (auto& [isUnanswered, hasFields] : *m_unanswered) {
        isUnanswered.link(&jit);
        enter();
        auto lookUp = [&](bool isSecond, GPRReg keptToo, GPRReg loaded) {
            Vector<GPRReg, 8> saved;
            saved.append(keptToo);
            for (GPRReg reg : kept)
                saved.append(reg);
            jit.preserving(saved.span(), [&] {
                const GPRReg distance = gprOf(shortStubImmediateRegister);
                jit.move(value, gprOf(receiverAtSiteRegister));
                jit.move(value, gprOf(firstStubOperandRegister));
                loadDistances(distance, WTF::roundUpToMultipleOf<stackAlignmentBytes()>(saved.size() * sizeof(CPURegister)));
                if (isSecond)
                    jit.urshift64(TrustedImm32(32), distance);
                else
                    jit.zeroExtend32ToWord(distance, distance);
                jit.callStub(readAtSiteRunningNoCode(ReceiverAtSite::IsCell, true));
                jit.move(GPRInfo::returnValueGPR, loaded);
            });
        };
        if (m_isNested) {
            jit.load64(Address(m_registers.cursor), value);
            lookUp(false, m_second, value);
            JumpList hasNoObject;
            hasNoObject.append(jit.branchTest64(CCallHelpers::Zero, value));
            hasNoObject.append(jit.branchIfNotCell(value));
            lookUp(true, m_second, value);
            Jump hasField = jit.jump();
            hasNoObject.link(&jit);
            jit.move(TrustedImm32(0), value);
            hasField.link(&jit);
            leave();
            bailouts.append(jit.branchTest64(CCallHelpers::Zero, value));
        } else {
            lookUp(true, value, m_second);
            lookUp(false, m_second, value);
            leave();
            bailouts.append(jit.branchTest64(CCallHelpers::Zero, value));
            bailouts.append(jit.branchTest64(CCallHelpers::Zero, m_second));
        }
        jit.jump().linkTo(hasFields, &jit);
    }
    m_unanswered->clear();
}

class ElementTester {
public:
    ElementTester(StubAssembler& jit, const CallbackRegisters& registers, CallbackSource source, ElementTest test)
        : m_jit(jit)
        , m_registers(registers)
        , m_test(test)
        , m_takesDoubles(source == CallbackSource::Element && test != ElementTest::IsTrue && test != ElementTest::StartsWith && test != ElementTest::HasType)
    {
        if (isRelation() || m_takesDoubles) {
            m_bound = jit.allocateScratch(FPRInfo::fpRegT0);
            m_number = jit.allocateScratch(FPRInfo::fpRegT1);
        }
    }

    bool isRelation() const { return m_test == ElementTest::IsLess || m_test == ElementTest::IsLessOrEqual; }
    bool takesDoubles() const { return m_takesDoubles; }
    bool keepsBound() const { return m_bound != InvalidFPRReg; }
    FPRReg bound() const { return m_bound; }
    FPRReg number() const { return m_number; }

    void prepareOperand(JumpList& cannotTest, bool amongDoubles = false);
    void prepareBound();
    void prepareForDoubles(JumpList& bailouts);
    void emitTest(JumpList& passes);
    void emitTestOfNumber(JumpList& passes, JumpList& bailouts);
    void finishTest(Label passed, Label failed) { m_coldTests.append({ std::exchange(m_isOther, { }), std::exchange(m_isComparedAsNumber, { }), passed, failed }); }
    void emitColdTests(JumpList& bailouts);

private:
    struct ColdTest {
        JumpList isOther;
        Jump isComparedAsNumber;
        Label passed;
        Label failed;
    };

    void testTypes(JumpList& passes);
    void emitColdTest(JumpList& passes, JumpList& fails, JumpList& bailouts);
    void emitComparisonOfStrings(JumpList& passes, JumpList& fails, JumpList& bailouts);

    StubAssembler& m_jit;
    Vector<ColdTest, 4> m_coldTests;
    JumpList m_isOther;
    Jump m_isComparedAsNumber;
    const CallbackRegisters& m_registers;
    const ElementTest m_test;
    const bool m_takesDoubles;
    FPRReg m_bound { InvalidFPRReg };
    FPRReg m_number { InvalidFPRReg };
};

} // anonymous namespace

void ElementTester::prepareOperand(JumpList& cannotTest, bool amongDoubles)
{
    StubAssembler& jit = m_jit;
    const GPRReg operand = m_registers.operand;
    const GPRReg value = m_registers.value;
    if (m_test == ElementTest::IsTrue || m_test == ElementTest::HasType)
        return;
    if (m_test == ElementTest::StartsWith) {
        cannotTest.append(jit.branchIfNotCell(operand));
        cannotTest.append(jit.branchTest64(CCallHelpers::Zero, operand));
        cannotTest.append(jit.branchIfNotType(operand, StringType));
        jit.loadPtr(Address(operand, JSString::offsetOfValue()), value);
        cannotTest.append(jit.branchIfRopeStringImpl(value));
        return;
    }
    Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, operand, numberTag);
    if (isRelation()) {
        cannotTest.append(jit.branchTest64(CCallHelpers::Zero, operand, numberTag));
        unboxDouble(jit, operand, value, m_bound);
        Jump isUnboxed = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(operand, m_bound);
        isUnboxed.link(&jit);
        return;
    }
    Jump isCell = jit.branchIfCell(operand);
    JumpList isPrepared;
    if (amongDoubles) {
        isPrepared.append(jit.branchTest64(CCallHelpers::Zero, operand, numberTag));
        unboxDouble(jit, operand, value, m_bound);
    } else
        cannotTest.append(jit.branchTest64(CCallHelpers::NonZero, operand, numberTag));
    isPrepared.append(jit.jump());
    isCell.link(&jit);
    cannotTest.append(jit.branchTest64(CCallHelpers::Zero, operand));
    Jump isNoString = jit.branchIfNotType(operand, StringType);
    jit.loadPtr(Address(operand, JSString::offsetOfValue()), value);
    cannotTest.append(jit.branchIfRopeStringImpl(value));
    isPrepared.append(jit.jump());
    isNoString.link(&jit);
    cannotTest.append(jit.branchIfType(operand, HeapBigIntType));
    isPrepared.append(jit.jump());
    isInt32.link(&jit);
    if (amongDoubles)
        jit.convertInt32ToDouble(operand, m_bound);
    isPrepared.link(&jit);
}

void ElementTester::prepareBound()
{
    Jump isInt32 = m_jit.branch64(CCallHelpers::AboveOrEqual, m_registers.operand, numberTag);
    Jump isNoNumber = m_jit.branchTest64(CCallHelpers::Zero, m_registers.operand, numberTag);
    m_jit.add64(numberTag, m_registers.operand);
    m_jit.move64ToDouble(m_registers.operand, m_bound);
    m_jit.sub64(numberTag, m_registers.operand);
    Jump isUnboxed = m_jit.jump();
    isInt32.link(&m_jit);
    m_jit.convertInt32ToDouble(m_registers.operand, m_bound);
    isUnboxed.link(&m_jit);
    isNoNumber.link(&m_jit);
}

void ElementTester::prepareForDoubles(JumpList& bailouts)
{
    if (!isRelation())
        bailouts.append(m_jit.branchTest64(CCallHelpers::Zero, m_registers.operand, numberTag));
}

void ElementTester::emitTestOfNumber(JumpList& passes, JumpList& bailouts)
{
    if (!isRelation()) {
        passes.append(m_jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, m_number, m_bound));
        return;
    }
    bool isLess = m_test == ElementTest::IsLess;
    passes.append(m_jit.branchDouble(isLess ? CCallHelpers::DoubleLessThanAndOrdered : CCallHelpers::DoubleLessThanOrEqualAndOrdered, m_number, m_bound));
    bailouts.append(m_jit.branchDouble(isLess ? CCallHelpers::DoubleLessThanOrUnordered : CCallHelpers::DoubleLessThanOrEqualOrUnordered, m_number, m_bound));
}

void ElementTester::emitComparisonOfStrings(JumpList& passes, JumpList& fails, JumpList& bailouts)
{
    StubAssembler& jit = m_jit;
    const GPRReg value = m_registers.value;
#if CPU(X86_64)
    const GPRReg compared = m_registers.answer;
    const GPRReg length = m_registers.place;
    const GPRReg character = m_registers.cursor;
    const GPRReg other = m_registers.end;
    for (GPRReg reg : { compared, length, character, other })
        jit.push(reg);
    auto restoreRegisters = [&] {
        for (GPRReg reg : { other, character, length, compared })
            jit.pop(reg);
    };
#else
    const GPRReg compared = jit.reuseScratch(StubRegister::T11);
    const GPRReg length = jit.reuseScratch(StubRegister::T12);
    const GPRReg character = jit.reuseScratch(StubRegister::T13);
    const GPRReg other = jit.reuseScratch(StubRegister::T14);
    auto restoreRegisters = [] { };
#endif
    JumpList areEqual;
    JumpList differ;
    JumpList isUnknown;
    jit.loadPtr(Address(m_registers.operand, JSString::offsetOfValue()), compared);
    areEqual.append(jit.branchPtr(CCallHelpers::Equal, compared, value));
    jit.load32(Address(compared, StringImpl::lengthMemoryOffset()), length);
    const bool comparesBeginning = m_test == ElementTest::StartsWith;
    differ.append(jit.branch32(comparesBeginning ? CCallHelpers::Below : CCallHelpers::NotEqual, Address(value, StringImpl::lengthMemoryOffset()), length));
    jit.load32(Address(compared, StringImpl::flagsOffset()), character);
    jit.load32(Address(value, StringImpl::flagsOffset()), other);
    jit.and32(other, character);
    if (!comparesBeginning)
        differ.append(jit.branchTest32(CCallHelpers::NonZero, character, TrustedImm32(StringImpl::flagIsAtom())));
    isUnknown.append(jit.branchTest32(CCallHelpers::Zero, character, TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(compared, StringImpl::dataOffset()), compared);
    jit.loadPtr(Address(value, StringImpl::dataOffset()), value);
    Label nextCharacter = jit.label();
    areEqual.append(jit.branchTest32(CCallHelpers::Zero, length));
    jit.sub32(TrustedImm32(1), length);
    jit.load8(BaseIndex(compared, length, CCallHelpers::TimesOne), character);
    jit.load8(BaseIndex(value, length, CCallHelpers::TimesOne), other);
    jit.branch32(CCallHelpers::Equal, character, other).linkTo(nextCharacter, &jit);

    differ.link(&jit);
    restoreRegisters();
    fails.append(jit.jump());
    areEqual.link(&jit);
    restoreRegisters();
    passes.append(jit.jump());
    isUnknown.link(&jit);
    restoreRegisters();
    bailouts.append(jit.jump());
}

void ElementTester::testTypes(JumpList& passes)
{
#if CPU(X86_64)
    const GPRReg types = m_registers.operand;
#else
    const GPRReg types = m_jit.reuseScratch(StubRegister::T11);
    m_jit.move(m_registers.operand, types);
#endif
    passes.append(branchTestBitAt(m_jit, CCallHelpers::NonZero, types, m_registers.value));
}

void ElementTester::emitTest(JumpList& passes)
{
    StubAssembler& jit = m_jit;
    const GPRReg operand = m_registers.operand;
    const GPRReg value = m_registers.value;
    if (m_test == ElementTest::IsTrue) {
        passes.append(jit.branch64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueTrue)));
        m_isOther.append(jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueFalse)));
        return;
    }
    if (m_test == ElementTest::StartsWith) {
        m_isOther.append(jit.jump());
        return;
    }
    if (m_test == ElementTest::HasType) {
        bailOutUnlessCell(jit, value, m_isOther);
        jit.load8(Address(value, JSCell::typeInfoTypeOffset()), value);
        Jump isPrimitive = jit.branch32(CCallHelpers::Below, value, TrustedImm32(ObjectType));
        jit.move(TrustedImm32(ElementTypes::bitOfObject), value);
        isPrimitive.link(&jit);
        testTypes(passes);
        return;
    }
    if (m_test == ElementTest::Equals)
        passes.append(jit.branch64(CCallHelpers::Equal, value, operand));
    m_isOther.append(jit.branch64(CCallHelpers::Below, value, numberTag));
    if (isRelation()) {
        m_isComparedAsNumber = jit.branch64(CCallHelpers::Below, operand, numberTag);
        passes.append(jit.branch32(m_test == ElementTest::IsLess ? CCallHelpers::LessThan : CCallHelpers::LessThanOrEqual, value, operand));
    }
}

void ElementTester::emitColdTests(JumpList& bailouts)
{
    for (ColdTest& test : m_coldTests) {
        Jump hasNumber;
        if (test.isComparedAsNumber.isSet()) {
            test.isComparedAsNumber.link(&m_jit);
            m_jit.convertInt32ToDouble(m_registers.value, m_number);
            hasNumber = m_jit.jump();
        }
        test.isOther.link(&m_jit);
        JumpList passes;
        JumpList fails;
        if (isRelation()) {
            bailouts.append(m_jit.branchTest64(CCallHelpers::Zero, m_registers.value, numberTag));
            unboxDouble(m_jit, m_registers.value, m_registers.value, m_number);
            hasNumber.link(&m_jit);
            emitTestOfNumber(passes, bailouts);
        } else {
            bailouts.append(m_jit.branchTest64(CCallHelpers::Zero, m_registers.value));
            emitColdTest(passes, fails, bailouts);
        }
        fails.link(&m_jit);
        m_jit.jump().linkTo(test.failed, &m_jit);
        passes.linkTo(test.passed, &m_jit);
    }
    m_coldTests.clear();
}

void ElementTester::emitColdTest(JumpList& passes, JumpList& fails, JumpList& bailouts)
{
    StubAssembler& jit = m_jit;
    const GPRReg operand = m_registers.operand;
    const GPRReg value = m_registers.value;
    if (m_test == ElementTest::IsTrue) {
        Jump isNoCell = jit.branchIfNotCell(value);
        static_assert(ObjectType > StringType && ObjectType > SymbolType && ObjectType > HeapBigIntType);
        passes.append(jit.branch8(CCallHelpers::AboveOrEqual, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(ObjectType)));
        bailouts.append(jit.branchIfNotType(value, StringType));
        jit.loadPtr(Address(value, JSString::offsetOfValue()), value);
        passes.append(jit.branchIfRopeStringImpl(value));
        passes.append(jit.branchTest32(CCallHelpers::NonZero, Address(value, StringImpl::lengthMemoryOffset())));
        fails.append(jit.jump());

        isNoCell.link(&jit);
        fails.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
        bailouts.append(jit.branch64(CCallHelpers::Below, value, numberTag));
        passes.append(jit.branchTest32(CCallHelpers::NonZero, value));
        return;
    }
    if (m_test == ElementTest::StartsWith) {
        bailouts.append(jit.branchIfNotCell(value));
        bailouts.append(jit.branchIfNotType(value, StringType));
        jit.loadPtr(Address(value, JSString::offsetOfValue()), value);
        bailouts.append(jit.branchIfRopeStringImpl(value));
        emitComparisonOfStrings(passes, fails, bailouts);
        return;
    }
    if (m_test == ElementTest::HasType) {
        Jump isNumber = jit.branchTest64(CCallHelpers::NonZero, value, numberTag);
        jit.add32(TrustedImm32(ElementTypes::shiftOfEncoded), value);
        testTypes(passes);
        fails.append(jit.jump());
        isNumber.link(&jit);
        passes.append(jit.branchTest32(CCallHelpers::Signed, operand));
        return;
    }
    RELEASE_ASSERT(m_test == ElementTest::Equals);
    Jump isCell = jit.branchIfCell(value);
    fails.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
    fails.append(jit.branch64(CCallHelpers::Below, operand, numberTag));
    bailouts.append(jit.jump());

    isCell.link(&jit);
    fails.append(jit.branchIfNotCell(operand));
    fails.append(jit.branchIfNotType(operand, StringType));
    fails.append(jit.branchIfNotType(value, StringType));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), value);
    bailouts.append(jit.branchIfRopeStringImpl(value));
    emitComparisonOfStrings(passes, fails, bailouts);
}

static void loadNumberInDoubles(StubAssembler& jit, const CallbackRegisters& registers, FPRReg number, JumpList& bailouts)
{
    jit.loadDouble(Address(registers.cursor), number);
    bailouts.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, number, number));
}

static void callMethod(StubAssembler& jit, const CallbackRegisters& registers, CallbackExits& exits, bool mayPassInitialValue)
{
    const GPRReg word = registers.methodWord;
    const GPRReg scratch = registers.scratchOfMethodWord;
    exits.methodCalls.link(&jit);
    Jump onlyTries = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::onlyTries));
    loadMethodWord(jit, registers.flags, word);
    exits.lookups.append(jit.branchTest64(CCallHelpers::Zero, word));
    JumpList lendingLookups;
    Jump isNotLent = jit.branchTest32(CCallHelpers::Zero, registers.flags, TrustedImm32(ArrayCallbackFlags::callbackIsLent));
    lendingLookups.append(branchUnlessWordBorrows(jit, word, scratch, onlyFirstArgumentIsLent));
    isNotLent.link(&jit);
    auto enterMethod = [&](unsigned count) {
        if (count == 2)
            jit.move(registers.operand, argumentGPR(1));
        emitFillMissingArgumentRegisters(jit, count);
        jit.move(TrustedImm32(static_cast<int32_t>(count)), countGPR);
        toCodeAddress(jit, word, scratch, StackIsTested::No);
        jit.farJump(word, JSEntryPtrTag);
    };
    auto lookUpMethod = [&](unsigned lentArguments) {
        if (mayPassInitialValue) {
            Jump passesNoInitialValue = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::hasNoInitialValue));
            jit.move(registers.operand, argumentGPR(1));
            jit.jumpToStub(callMethodById(2, lentArguments));
            passesNoInitialValue.link(&jit);
        }
        jit.jumpToStub(callMethodById(1, lentArguments));
    };
    if (mayPassInitialValue) {
        Jump passesNoInitialValue = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::hasNoInitialValue));
        enterMethod(2);
        passesNoInitialValue.link(&jit);
    }
    enterMethod(1);

    exits.lookups.link(&jit);
    Jump onlyTriesToo = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::onlyTries));
    lendingLookups.append(jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::callbackIsLent)));
    lookUpMethod(0);
    lendingLookups.link(&jit);
    lookUpMethod(onlyFirstArgumentIsLent);

    onlyTries.link(&jit);
    onlyTriesToo.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(JSValue())), jit.result());
    jit.returnToCaller();
}

static void searchString(StubAssembler& jit, const CallbackRegisters& registers, Elements elements, JumpList& isFound, JumpList& isAtEnd, JumpList& valueSearches, JumpList& searchBailouts)
{
    const GPRReg value = registers.value;
#if CPU(X86_64)
    const GPRReg characters = thisGPR;
    const GPRReg length = jit.immediate();
    const GPRReg character = registers.operand;
    const GPRReg lastCharacter = registers.flags;
    const GPRReg position = registers.place;
    const std::array saved { characters, length, character, lastCharacter };
    for (GPRReg reg : saved)
        jit.push(reg);
    auto restoreRegisters = [&] {
        for (size_t index = saved.size(); index--;)
            jit.pop(saved[index]);
    };
    auto savePosition = [&] { jit.push(position); };
    auto restorePosition = [&] { jit.pop(position); };
#else
    const GPRReg characters = jit.reuseScratch(StubRegister::T11);
    const GPRReg length = jit.reuseScratch(StubRegister::T12);
    const GPRReg character = jit.reuseScratch(StubRegister::T13);
    const GPRReg lastCharacter = jit.reuseScratch(StubRegister::T14);
    const GPRReg position = jit.reuseScratch(StubRegister::T15);
    auto restoreRegisters = [] { };
    auto savePosition = [] { };
    auto restorePosition = [] { };
#endif
    JumpList isOtherString;
    JumpList bailouts;
    JumpList mismatches;
    ColdParts coldParts;
    jit.loadPtr(Address(registers.operand, JSString::offsetOfValue()), value);
    bailouts.append(jit.branchIfRopeStringImpl(value));
    isOtherString.append(jit.branchTest32(CCallHelpers::Zero, Address(value, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(value, StringImpl::lengthMemoryOffset()), length);
    isOtherString.append(jit.branchTest32(CCallHelpers::Zero, length));
    jit.loadPtr(Address(value, StringImpl::dataOffset()), characters);
    jit.load8(BaseIndex(characters, length, CCallHelpers::TimesOne, -1), lastCharacter);

    Label next = jit.label();
    loadValue(jit, registers, elements, coldParts, bailouts, EmptyField::IsLeftToTest);
    JumpList isNoCell;
    bailOutUnlessCell(jit, value, isNoCell);
    mismatches.append(jit.branchIfNotType(value, StringType));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), value);
    bailouts.append(jit.branchIfRopeStringImpl(value));
    mismatches.append(jit.branch32(CCallHelpers::NotEqual, Address(value, StringImpl::lengthMemoryOffset()), length));
    bailouts.append(jit.branchTest32(CCallHelpers::Zero, Address(value, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(value, StringImpl::dataOffset()), value);
    jit.load8(BaseIndex(value, length, CCallHelpers::TimesOne, -1), character);
    Jump endsAlike = jit.branch32(CCallHelpers::Equal, character, lastCharacter);
    mismatches.link(&jit);
    Label skip = jit.label();
    advance(jit, registers, next);
    restoreRegisters();
    isAtEnd.append(jit.jump());

    endsAlike.link(&jit);
    savePosition();
    jit.move(length, position);
    jit.sub32(TrustedImm32(1), position);
    Label nextCharacter = jit.label();
    Jump areEqual = jit.branchTest32(CCallHelpers::Zero, position);
    jit.sub32(TrustedImm32(1), position);
    jit.load8(BaseIndex(value, position, CCallHelpers::TimesOne), character);
    jit.load8(BaseIndex(characters, position, CCallHelpers::TimesOne), lastCharacter);
    jit.branch32(CCallHelpers::Equal, character, lastCharacter).linkTo(nextCharacter, &jit);
    restorePosition();
    jit.load8(BaseIndex(characters, length, CCallHelpers::TimesOne, -1), lastCharacter);
    jit.jump().linkTo(skip, &jit);

    areEqual.link(&jit);
    restorePosition();
    restoreRegisters();
    isFound.append(jit.jump());
    isNoCell.link(&jit);
    jit.branchTest64(CCallHelpers::NonZero, value).linkTo(skip, &jit);
    bailouts.append(jit.jump());
    for (auto& part : coldParts.unanswered)
        isOtherString.append(part.first);
    bailouts.link(&jit);
    restoreRegisters();
    searchBailouts.append(jit.jump());
    isOtherString.link(&jit);
    restoreRegisters();
    if (elements != Elements::Values)
        jit.load64(Address(registers.cursor), value);
    valueSearches.append(jit.jump());
}

static void searchBits(StubAssembler& jit, const CallbackRegisters& registers, FPRReg bound, JumpList& isFound, JumpList& isAtEnd, JumpList& searchesNumbers, JumpList& bailouts)
{
    const GPRReg bits = registers.answer;
    const GPRReg bitsOfHole = registers.place;
    searchesNumbers.append(jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated)));
    searchesNumbers.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, bound, bound));
    jit.moveDoubleTo64(bound, bits);
    jit.move(bits, registers.value);
    jit.lshift64(TrustedImm32(1), registers.value);
    searchesNumbers.append(jit.branchTest64(CCallHelpers::Zero, registers.value));
    jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), bitsOfHole);
    Label next = jit.label();
    jit.load64(Address(registers.cursor), registers.value);
    isFound.append(jit.branch64(CCallHelpers::Equal, registers.value, bits));
    bailouts.append(jit.branch64(CCallHelpers::Equal, registers.value, bitsOfHole));
    advance(jit, registers, next);
    isAtEnd.append(jit.jump());
}

static TrustedImm32 numberOfMethod(CompiledMethodOfArray method) { return TrustedImm32(static_cast<int32_t>(method)); }

StubSignature signatureOfSearchArray(StubKey) { return signatureOf(callMethodById(1)); }

void generateSearchArray(StubAssembler& jit)
{
    const CallbackSource source = sourceOf(jit.key().form() / numberOfElementTests);
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    ElementTester tester(jit, registers, source, static_cast<ElementTest>(jit.key().form() % numberOfElementTests));
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    tester.prepareOperand(exits.methodCalls);
    saveCallback(jit, registers);
    loadCursorAndEnd(jit, registers);
    JumpList isAtEnd;
    isAtEnd.append(branchAtEnd(jit, registers));
    Jump hasShape;
    if (source != CallbackSource::Element)
        hasShape = loadPlaceOfField(jit, registers, exits.bailouts, false);

    ColdParts coldParts;
    auto emitLoop = [&](Elements elements, bool findsFailed) {
        const bool amongDoubles = elements == Elements::Doubles;
        Jump hasObject = source == CallbackSource::Field ? jit.jump() : Jump();
        Label next = jit.label();
        JumpList passes;
        if (amongDoubles) {
            loadNumberInDoubles(jit, registers, tester.number(), exits.bailouts);
            tester.emitTestOfNumber(passes, exits.bailouts);
        } else {
            loadValue(jit, registers, elements, coldParts, exits.bailouts, EmptyField::IsLeftToTest, hasObject);
            tester.emitTest(passes);
        }
        Label failed = jit.label();
        Jump isFound = findsFailed ? jit.jump() : Jump();
        if (findsFailed)
            passes.link(&jit);
        Label passed = jit.label();
        advance(jit, registers, next);
        isAtEnd.append(jit.jump());
        if (findsFailed)
            isFound.link(&jit);
        else {
            passes.link(&jit);
            passed = jit.label();
        }
        if (!amongDoubles)
            tester.finishTest(passed, failed);
    };
    JumpList isFound;
    auto search = [&](Elements elements) {
        Jump findsFailed = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated));
        emitLoop(elements, false);
        isFound.append(jit.jump());
        findsFailed.link(&jit);
        emitLoop(elements, true);
        isFound.append(jit.jump());
    };
    auto returnMethodMatch = [&](CompiledMethodOfArray method) {
        static_assert(JSValue::ValueTrue == JSValue::ValueFalse + 1);
        jit.compare32(CCallHelpers::Equal, registers.flags, numberOfMethod(method), jit.result());
        jit.add32(TrustedImm32(JSValue::ValueFalse), jit.result());
        jit.returnToCaller();
    };
    auto takeMethodOfFlags = [&] {
        discardCallback(jit);
        jit.urshift32(TrustedImm32(ArrayCallbackFlags::methodShift), registers.flags);
    };
    auto returnFound = [&](bool amongDoubles) {
        takeMethodOfFlags();
        Jump findsElement = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::Find));
        Jump findsIndex = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::FindIndex));
        returnMethodMatch(CompiledMethodOfArray::Some);
        findsElement.link(&jit);
        if (amongDoubles) {
            jit.loadDouble(Address(registers.cursor), tester.number());
            jit.boxDouble(tester.number(), registers.value);
            jit.move(registers.value, jit.result());
        } else
            jit.load64(Address(registers.cursor), jit.result());
        jit.returnToCaller();
        findsIndex.link(&jit);
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), registers.value);
        jit.subPtr(registers.value, registers.cursor);
        jit.urshift64(TrustedImm32(3), registers.cursor);
        jit.or64(numberTag, registers.cursor);
        jit.move(registers.cursor, jit.result());
        jit.returnToCaller();
    };
    auto searchValues = [&](Elements elements) {
        if (static_cast<ElementTest>(jit.key().form() % numberOfElementTests) == ElementTest::Equals) {
            JumpList valueSearches;
            valueSearches.append(jit.branchIfNotCell(registers.operand));
            valueSearches.append(jit.branchIfNotType(registers.operand, StringType));
            valueSearches.append(jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated)));
            searchString(jit, registers, elements, isFound, isAtEnd, valueSearches, exits.bailouts);
            valueSearches.link(&jit);
        }
        search(elements);
    };
    if (hasShape.isSet()) {
        Jump isInStorage = jit.branchTest64(CCallHelpers::Signed, registers.place);
        searchValues(Elements::FieldsInCellByStructure);
        isInStorage.link(&jit);
        searchValues(Elements::FieldsByStructure);
        hasShape.link(&jit);
        searchValues(Elements::FieldsByShape);
    } else
        searchValues(Elements::Values);
    isFound.link(&jit);
    returnFound(false);
    Jump holdsDoubles = checkOtherArray(jit, registers, check, source, tester.takesDoubles(), exits);
    if (holdsDoubles.isSet()) {
        holdsDoubles.link(&jit);
        tester.prepareOperand(exits.methodCalls, true);
        saveCallback(jit, registers);
        tester.prepareForDoubles(exits.bailouts);
        loadCursorAndEnd(jit, registers);
        isAtEnd.append(branchAtEnd(jit, registers));
        isFound.clear();
        if (static_cast<ElementTest>(jit.key().form() % numberOfElementTests) == ElementTest::Equals) {
            JumpList searchesNumbers;
            searchBits(jit, registers, tester.bound(), isFound, isAtEnd, searchesNumbers, exits.bailouts);
            searchesNumbers.link(&jit);
        }
        search(Elements::Doubles);
        isFound.link(&jit);
        returnFound(true);
    }

    isAtEnd.link(&jit);
    takeMethodOfFlags();
    Jump findsNoElement = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::Find));
    Jump findsNoIndex = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::FindIndex));
    returnMethodMatch(CompiledMethodOfArray::Every);
    findsNoElement.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();
    findsNoIndex.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsNumber(-1))), jit.result());
    jit.returnToCaller();

    auto keepNumbers = [] { };
    auto restoreNumbers = [&] {
        if (tester.keepsBound())
            tester.prepareBound();
    };
#if CPU(X86_64)
    emitFramelessColdParts(jit, registers, coldParts, exits.bailouts, { thisGPR, registers.place, registers.flags, registers.operand, registers.end, registers.cursor, jit.immediate() }, keepNumbers, restoreNumbers);
#else
    emitFramelessColdParts(jit, registers, coldParts, exits.bailouts, { registers.callback, registers.flags, jit.immediate() }, keepNumbers, restoreNumbers);
#endif
    tester.emitColdTests(exits.bailouts);
    exits.bailouts.link(&jit);
    restoreCallback(jit, registers);
    callMethod(jit, registers, exits, false);
}

static constexpr int32_t mostElementsOnStack = 128;
static constexpr int32_t bytesOnStack = mostElementsOnStack * sizeof(EncodedJSValue);

static void takeCollectedElements(StubAssembler& jit, const CallbackRegisters& registers, GPRReg elements, GPRReg scratch)
{
    jit.loadPtr(Address(registers.value, JSObject::butterflyOffset()), elements);
    jit.move(CCallHelpers::stackPointerRegister, registers.cursor);
    Jump isEmpty = branchAtEnd(jit, registers);
    Label next = jit.label();
    jit.load64(Address(registers.cursor), scratch);
    jit.store64(scratch, Address(elements));
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
    advance(jit, registers, next);
    isEmpty.link(&jit);
    jit.addPtr(TrustedImm32(bytesOnStack), CCallHelpers::stackPointerRegister);
}

static void countCollectedElements(StubAssembler& jit, const CallbackRegisters& registers, GPRReg elements, GPRReg count)
{
    jit.move(elements, registers.end);
    jit.move(elements, count);
    jit.move(CCallHelpers::stackPointerRegister, registers.cursor);
    jit.subPtr(registers.cursor, count);
    jit.urshift64(TrustedImm32(3), count);
}

StubSignature signatureOfSearchFieldPairs(StubKey) { return signatureOf(callMethodById(1)); }

void generateSearchFieldPairs(StubAssembler& jit)
{
    const FieldPairTest test = static_cast<FieldPairTest>(jit.key().form());
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg otherNumber = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg second = gprOf(StubRegister::T9);
    const GPRReg placeOfSecond = registers.operand;
    const GPRReg value = registers.value;
    RELEASE_ASSERT(second == jit.immediate());
#if CPU(X86_64)
    constexpr size_t wordsBelowDistances = 3;
    auto save = [&] {
        saveCallback(jit, registers);
        jit.push(jit.immediate());
        jit.push(jit.immediate());
    };
    auto restore = [&] {
        jit.pop(jit.immediate());
        jit.pop(jit.immediate());
        restoreCallback(jit, registers);
    };
    auto loadDistances = [&](GPRReg distances) { jit.load64(Address(CCallHelpers::stackPointerRegister, wordsBelowDistances * sizeof(CPURegister)), distances); };
    auto loadDistancesInFrame = [&](GPRReg distances, size_t) { jit.load64(Address(GPRInfo::callFrameRegister, (1 + wordsBelowDistances) * sizeof(CPURegister)), distances); };
#else
    const GPRReg immediate = registers.scratchOfMethodWord;
    auto save = [&] {
        saveCallback(jit, registers);
        jit.move(jit.immediate(), immediate);
    };
    auto restore = [&] {
        jit.move(immediate, jit.immediate());
        restoreCallback(jit, registers);
    };
    auto loadDistances = [&](GPRReg distances) { jit.move(registers.distance, distances); };
    auto loadDistancesInFrame = [&](GPRReg distances, size_t) { jit.move(registers.distance, distances); };
#endif
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    save();
    loadCursorAndEnd(jit, registers);
    exits.bailouts.append(branchAtEnd(jit, registers));

    struct ColdTest {
        JumpList isOther;
        Label passed;
        Label failed;
    };
    Vector<ColdTest, 5> coldTests;
    FieldPairReader reader(jit, registers, placeOfSecond, second, false);
    JumpList isAtEnd;
    auto emitLoop = [&](Elements elements, bool findsFailed) {
        Label next = jit.label();
        reader.loadFields(elements, exits.bailouts);
        JumpList isOther;
        isOther.append(jit.branch64(CCallHelpers::Below, value, numberTag));
        isOther.append(jit.branch64(CCallHelpers::Below, second, numberTag));
        Jump passes = jit.branch32(test == FieldPairTest::Equals ? CCallHelpers::Equal : test == FieldPairTest::IsLess ? CCallHelpers::LessThan : CCallHelpers::LessThanOrEqual, value, second);
        Label failed = jit.label();
        Jump isFound = findsFailed ? jit.jump() : Jump();
        if (findsFailed)
            passes.link(&jit);
        Label passed = jit.label();
        advance(jit, registers, next);
        isAtEnd.append(jit.jump());
        if (findsFailed)
            isFound.link(&jit);
        else {
            passes.link(&jit);
            passed = jit.label();
        }
        coldTests.append({ isOther, passed, failed });
    };
    JumpList hasShape = reader.loadPlaces(loadDistances, exits.bailouts);
    reader.emitForEachForm([&](Elements elements) {
        Jump findsFailed = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated));
        emitLoop(elements, false);
        Jump isFound = jit.jump();
        findsFailed.link(&jit);
        emitLoop(elements, true);
        isFound.link(&jit);
    }, hasShape);

    auto returnMethodMatch = [&](CompiledMethodOfArray method) {
        jit.compare32(CCallHelpers::Equal, registers.flags, numberOfMethod(method), jit.result());
        jit.add32(TrustedImm32(JSValue::ValueFalse), jit.result());
        jit.returnToCaller();
    };
    auto takeMethodOfFlags = [&] {
        restore();
        jit.urshift32(TrustedImm32(ArrayCallbackFlags::methodShift), registers.flags);
    };
    takeMethodOfFlags();
    Jump findsElement = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::Find));
    Jump findsIndex = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::FindIndex));
    returnMethodMatch(CompiledMethodOfArray::Some);
    findsElement.link(&jit);
    jit.load64(Address(registers.cursor), jit.result());
    jit.returnToCaller();
    findsIndex.link(&jit);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), value);
    jit.subPtr(value, registers.cursor);
    jit.urshift64(TrustedImm32(3), registers.cursor);
    jit.or64(numberTag, registers.cursor);
    jit.move(registers.cursor, jit.result());
    jit.returnToCaller();

    isAtEnd.link(&jit);
    takeMethodOfFlags();
    Jump findsNoElement = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::Find));
    Jump findsNoIndex = jit.branch32(CCallHelpers::Equal, registers.flags, numberOfMethod(CompiledMethodOfArray::FindIndex));
    returnMethodMatch(CompiledMethodOfArray::Every);
    findsNoElement.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();
    findsNoIndex.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsNumber(-1))), jit.result());
    jit.returnToCaller();

    auto unbox = [&](GPRReg boxed, FPRReg unboxed) {
        Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, boxed, numberTag);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, boxed, numberTag));
        unboxDouble(jit, boxed, boxed, unboxed);
        Jump isUnboxed = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(boxed, unboxed);
        isUnboxed.link(&jit);
    };
    for (ColdTest& coldTest : coldTests) {
        coldTest.isOther.link(&jit);
        unbox(value, number);
        unbox(second, otherNumber);
        auto condition = test == FieldPairTest::Equals ? CCallHelpers::DoubleEqualAndOrdered : test == FieldPairTest::IsLess ? CCallHelpers::DoubleLessThanAndOrdered : CCallHelpers::DoubleLessThanOrEqualAndOrdered;
        jit.branchDouble(condition, number, otherNumber).linkTo(coldTest.passed, &jit);
        jit.jump().linkTo(coldTest.failed, &jit);
    }
#if CPU(X86_64)
    const std::initializer_list<GPRReg> kept { thisGPR, registers.place, placeOfSecond, registers.flags, registers.end, registers.cursor };
#else
    const std::initializer_list<GPRReg> kept { registers.callback, registers.flags, immediate };
#endif
    reader.emitColdParts(exits.bailouts, kept, loadDistancesInFrame, [&] { jit.emitFunctionPrologue(); }, [&] { jit.emitFunctionEpilogueWithEmptyFrame(); });
    checkOtherArray(jit, registers, check, CallbackSource::Field, false, exits);
    exits.bailouts.link(&jit);
    restore();
    callMethod(jit, registers, exits, false);
}

StubSignature signatureOfFoldArray(StubKey) { return signatureOf(callMethodById(1)); }

void generateFoldArray(StubAssembler& jit)
{
    const CallbackSource source = sourceOf(jit.key().form() / 3);
    const NumberFold fold = static_cast<NumberFold>(jit.key().form() % 3);
    const bool isElement = source == CallbackSource::Element;
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg operand = jit.allocateScratch(FPRInfo::fpRegT1);
#if CPU(X86_64)
    const GPRReg sum = registers.operand;
    auto saveOperand = [&] {
        jit.push(registers.operand);
        jit.push(registers.operand);
    };
    auto restoreOperand = [&] {
        jit.pop(registers.operand);
        jit.pop(registers.operand);
    };
#else
    const GPRReg sum = jit.allocateScratch(StubRegister::T15);
    auto saveOperand = [&] { jit.move(registers.operand, sum); };
    auto restoreOperand = [] { };
#endif
    const GPRReg value = registers.value;
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    saveOperand();
    saveCallback(jit, registers);
    loadCursorAndEnd(jit, registers);
    Jump hasShape;
    if (!isElement)
        hasShape = loadPlaceOfField(jit, registers, exits.bailouts);

    JumpList isAtEndOfIntegers;
    JumpList isAtEndOfNumbers;
    ColdParts coldPartsOfIntegers;
    ColdParts coldPartsOfNumbers;
    auto foldNumber = [&](Label next) {
        JumpList continues;
        if (fold == NumberFold::Sum)
            jit.addDouble(operand, number);
        else {
            bool isGreatest = fold == NumberFold::Greatest;
            continues.append(jit.branchDouble(isGreatest ? CCallHelpers::DoubleLessThanAndOrdered : CCallHelpers::DoubleGreaterThanAndOrdered, operand, number));
            Jump replaces = jit.branchDouble(isGreatest ? CCallHelpers::DoubleGreaterThanAndOrdered : CCallHelpers::DoubleLessThanAndOrdered, operand, number);
            exits.bailouts.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, operand, number));
            jit.moveZeroToDouble(operand);
            continues.append(jit.branchDouble(CCallHelpers::DoubleNotEqualAndOrdered, operand, number));
            exits.bailouts.append(jit.jump());
            replaces.link(&jit);
            jit.moveDouble(operand, number);
        }
        continues.link(&jit);
        advance(jit, registers, next);
        isAtEndOfNumbers.append(jit.jump());
    };
    auto convertSum = [&](GPRReg scratch) {
        if (fold == NumberFold::Sum) {
            jit.rshift64(sum, TrustedImm32(53), scratch);
            jit.add64(TrustedImm32(1), scratch);
            exits.bailouts.append(jit.branch64(CCallHelpers::Above, scratch, TrustedImm32(1)));
        }
        jit.convertInt64ToDouble(sum, number);
    };

    auto foldValues = [&](Elements elements) {
        if (isElement) {
            Jump hasInitialValue = jit.branchTest32(CCallHelpers::Zero, registers.flags, TrustedImm32(ArrayCallbackFlags::hasNoInitialValue));
            exits.bailouts.append(branchAtEnd(jit, registers));
            loadValue(jit, registers, elements, coldPartsOfIntegers, exits.bailouts, EmptyField::IsLeftToTest);
            jit.move(value, sum);
            jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), registers.cursor);
            hasInitialValue.link(&jit);
        }
        Jump beginsWithInt32 = jit.branch64(CCallHelpers::AboveOrEqual, sum, numberTag);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, sum, numberTag));
        unboxDouble(jit, sum, sum, number);
        Jump entersNumbers = jit.jump();
        beginsWithInt32.link(&jit);
        jit.signExtend32ToPtr(sum, sum);

        isAtEndOfIntegers.append(branchAtEnd(jit, registers));
        Jump hasObject = isElement ? Jump() : jit.jump();
        Label nextInteger = jit.label();
        loadValue(jit, registers, elements, coldPartsOfIntegers, exits.bailouts, EmptyField::IsLeftToTest, hasObject);
        Jump isNoInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
        jit.signExtend32ToPtr(value, value);
        if (fold == NumberFold::Sum)
            jit.add64(value, sum);
        else {
            Jump keeps = jit.branch64(fold == NumberFold::Greatest ? CCallHelpers::LessThanOrEqual : CCallHelpers::GreaterThanOrEqual, value, sum);
            jit.move(value, sum);
            keeps.link(&jit);
        }
        advance(jit, registers, nextInteger);
        isAtEndOfIntegers.append(jit.jump());

        isNoInt32.link(&jit);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
        jit.move64ToDouble(value, operand);
        convertSum(value);
        jit.moveDoubleTo64(operand, value);
        Jump hasDouble = jit.jump();

        entersNumbers.link(&jit);
        isAtEndOfNumbers.append(branchAtEnd(jit, registers));
        Label nextNumber = jit.label();
        loadValue(jit, registers, elements, coldPartsOfNumbers, exits.bailouts, EmptyField::IsLeftToTest);
        Jump isOtherThanInt32 = jit.branch64(CCallHelpers::Below, value, numberTag);
        jit.convertInt32ToDouble(value, operand);
        Jump hasOperand = jit.jump();
        isOtherThanInt32.link(&jit);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
        hasDouble.link(&jit);
        unboxDouble(jit, value, value, operand);
        hasOperand.link(&jit);
        foldNumber(nextNumber);
    };
    if (hasShape.isSet()) {
        Jump isInStorage = jit.branchTest64(CCallHelpers::Signed, registers.place);
        foldValues(Elements::FieldsInCellByStructure);
        isInStorage.link(&jit);
        foldValues(Elements::FieldsByStructure);
        hasShape.link(&jit);
        foldValues(Elements::FieldsByShape);
    } else
        foldValues(Elements::Values);

    Jump holdsDoubles = checkOtherArray(jit, registers, check, source, isElement, exits);
    if (isElement) {
        holdsDoubles.link(&jit);
        saveOperand();
        saveCallback(jit, registers);
        loadCursorAndEnd(jit, registers);
        Jump hasInitialValue = jit.branchTest32(CCallHelpers::Zero, registers.flags, TrustedImm32(ArrayCallbackFlags::hasNoInitialValue));
        exits.bailouts.append(branchAtEnd(jit, registers));
        loadNumberInDoubles(jit, registers, number, exits.bailouts);
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), registers.cursor);
        Jump hasNumber = jit.jump();
        hasInitialValue.link(&jit);
        Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, sum, numberTag);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, sum, numberTag));
        unboxDouble(jit, sum, sum, number);
        Jump isUnboxed = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(sum, number);
        isUnboxed.link(&jit);
        hasNumber.link(&jit);
        isAtEndOfNumbers.append(branchAtEnd(jit, registers));
        Label nextDouble = jit.label();
        loadNumberInDoubles(jit, registers, operand, exits.bailouts);
        foldNumber(nextDouble);
    }

    auto returnValue = [&] {
#if CPU(X86_64)
        jit.addPtr(TrustedImm32(4 * sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
        jit.move(value, jit.result());
        jit.returnToCaller();
    };
    isAtEndOfIntegers.link(&jit);
    jit.signExtend32ToPtr(sum, value);
    Jump isWide = jit.branch64(CCallHelpers::NotEqual, value, sum);
    jit.zeroExtend32ToWord(sum, value);
    jit.or64(numberTag, value);
    returnValue();
    isWide.link(&jit);
    convertSum(value);
    isAtEndOfNumbers.link(&jit);
    jit.purifyNaN(number, operand);
    jit.boxDouble(operand, value);
    returnValue();

#if CPU(X86_64)
    const std::initializer_list<GPRReg> kept { thisGPR, registers.place, registers.flags, sum, registers.end, registers.cursor, jit.immediate() };
#else
    const std::initializer_list<GPRReg> kept { registers.callback, registers.flags, sum, jit.immediate() };
#endif
    emitFramelessColdParts(jit, registers, coldPartsOfIntegers, exits.bailouts, kept, [] { }, [] { });
    emitFramelessColdParts(jit, registers, coldPartsOfNumbers, exits.bailouts, kept, [&] { jit.moveDoubleTo64(number, sum); }, [&] { jit.move64ToDouble(sum, number); });
    exits.bailouts.link(&jit);
    restoreCallback(jit, registers);
    restoreOperand();
    callMethod(jit, registers, exits, true);
}

static void checkSpecies(StubAssembler& jit, const CallbackRegisters& registers, JumpList& otherSpecies)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), registers.value);
    otherSpecies.append(branchIfWatchpointSetIsNotWatched(jit, Address(registers.value, OBJECT_OFFSETOF(JSGlobalObject, m_arraySpeciesWatchpointSet)), registers.value));
}

static constexpr int32_t mostElementsOfNewArray = 1 << 16;

static std::array<GPRReg, 5> preservedRegisters(StubAssembler& jit, const CallbackRegisters& registers) { return { thisGPR, registers.callback, registers.flags, jit.immediate(), registers.answer }; }

static void loadPreservedDistance(StubAssembler& jit, const std::array<GPRReg, 5>& preserved, GPRReg distance, size_t bytesBelow)
{
    jit.load64(Address(CCallHelpers::stackPointerRegister, bytesBelow + (preserved.size() - 1) * sizeof(CPURegister)), distance);
}

static void makeArray(StubAssembler& jit, const CallbackRegisters& registers, ArrayElementKind kind, std::initializer_list<GPRReg> kept)
{
    jit.preserving(kept, [&] {
        jit.callStub(newArrayOfLength(kind));
        jit.move(GPRInfo::returnValueGPR, registers.value);
    });
}

static void replaceByLength(StubAssembler& jit, GPRReg value, GPRReg scratch, JumpList& bailouts)
{
    bailouts.append(jit.branchIfNotCell(value));
    Jump isString = jit.branchIfType(value, StringType);
    bailouts.append(jit.branchIfNotType(value, ArrayType));
    jit.loadPtr(Address(value, JSObject::butterflyOffset()), value);
    bailouts.append(jit.branchTestPtr(CCallHelpers::Zero, value));
    jit.load32(Address(value, Butterfly::offsetOfPublicLength()), value);
    bailouts.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(0)));
    Jump hasLengthOfArray = jit.jump();
    isString.link(&jit);
    jit.loadPtr(Address(value, JSString::offsetOfValue()), scratch);
    Jump isRope = jit.branchIfRopeStringImpl(scratch);
    jit.load32(Address(scratch, StringImpl::lengthMemoryOffset()), value);
    Jump hasLength = jit.jump();
    isRope.link(&jit);
    jit.load32(Address(value, JSRopeString::offsetOfLength()), value);
    hasLength.link(&jit);
    hasLengthOfArray.link(&jit);
    jit.or64(numberTag, value);
}

StubSignature signatureOfMapArrayToField(StubKey) { return signatureOf(callMethodById(1)); }

void generateMapArrayToField(StubAssembler& jit)
{
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const GPRReg length = gprOf(StubRegister::T9);
    const GPRReg made = thisGPR;
    const GPRReg elements = registers.operand;
    const FieldMap map = static_cast<FieldMap>(jit.key().form());
    const CallbackSource source = map == FieldMap::LengthOfElement ? CallbackSource::Element : CallbackSource::Field;
    const bool isNested = map == FieldMap::FieldOfField;
    const FPRReg header = isNested ? jit.allocateScratch(FPRInfo::fpRegT0) : InvalidFPRReg;
    const bool measures = map == FieldMap::LengthOfElement || map == FieldMap::LengthOfField;
    RELEASE_ASSERT(length == jit.immediate());
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    checkSpecies(jit, registers, exits.methodCalls);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), registers.value);
    jit.load32(Address(registers.value, Butterfly::offsetOfPublicLength()), registers.value);
    jit.sub32(TrustedImm32(1), registers.value);
    exits.methodCalls.append(jit.branch32(CCallHelpers::AboveOrEqual, registers.value, TrustedImm32(mostElementsOfNewArray)));

    jit.emitFunctionPrologue();
    const std::array preserved = preservedRegisters(jit, registers);
    ColdParts coldParts;
    FieldPairReader reader(jit, registers, registers.flags, length, true);
    jit.preserving(std::span<const GPRReg>(preserved), [&] {
        JumpList isDone;
        auto loadElement = [&](Elements elementsRead) {
            if (isNested)
                reader.loadFields(elementsRead, exits.bailouts);
            else
                loadValue(jit, registers, elementsRead, coldParts, exits.bailouts);
        };
        auto loadResult = [&](Elements elementsRead) {
            loadElement(elementsRead);
            if (measures)
                replaceByLength(jit, registers.value, length, exits.bailouts);
        };
        auto mapElements = [&](Elements elementsRead) {
            loadResult(elementsRead);
            if (isNested)
                jit.move(length, elements);
            jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), length);
            jit.load32(Address(length, Butterfly::offsetOfPublicLength()), length);
            if (isNested) {
                makeArray(jit, registers, ArrayElementKind::Int32, { thisGPR, registers.answer, registers.place, registers.flags, elements });
                jit.move(elements, length);
            } else
                makeArray(jit, registers, ArrayElementKind::Int32, { thisGPR, registers.answer, registers.place });
            loadCursorAndEnd(jit, registers);
            jit.move(registers.value, made);
            jit.loadPtr(Address(made, JSObject::butterflyOffset()), elements);
            auto storeElement = [&](Label next) {
                jit.store64(registers.value, Address(elements));
                jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
                advance(jit, registers, next);
                isDone.append(jit.jump());
            };
            Label nextInteger = jit.label();
            loadResult(elementsRead);
            if (measures) {
                storeElement(nextInteger);
                return;
            }
            Jump isNoInt32 = jit.branch64(CCallHelpers::Below, registers.value, numberTag);
            storeElement(nextInteger);
            isNoInt32.link(&jit);
            if (isNested) {
                jit.loadDouble(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader)), header);
                jit.storeDouble(header, Address(made));
            } else {
                jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader)), length);
                jit.store64(length, Address(made));
            }
            Jump hasValue = jit.jump();
            Label nextValue = jit.label();
            loadElement(elementsRead);
            hasValue.link(&jit);
            storeElement(nextValue);
        };
        loadCursorAndEnd(jit, registers);
        if (isNested) {
            JumpList hasShape = reader.loadPlaces([&](GPRReg distances) { loadPreservedDistance(jit, preserved, distances, 0); }, exits.bailouts);
            mapElements(Elements::FieldsByStructure);
            hasShape.link(&jit);
            mapElements(Elements::FieldsByShape);
        } else if (source == CallbackSource::Element)
            mapElements(Elements::Values);
        else {
            Jump hasShape = loadPlaceOfField(jit, registers, exits.bailouts, false);
            mapElements(Elements::FieldsByStructure);
            hasShape.link(&jit);
            mapElements(Elements::FieldsByShape);
        }

        isDone.link(&jit);
        jit.move(made, registers.value);
        Jump isMade = jit.jump();
#if CPU(X86_64)
        const std::initializer_list<GPRReg> kept { made, registers.place, elements, registers.end, registers.cursor };
#else
        const std::initializer_list<GPRReg> kept { made };
#endif
        auto loadDistance = [&](GPRReg distance, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distance, bytesBelow); };
#if CPU(X86_64)
        const std::initializer_list<GPRReg> keptWhereNested { made, registers.place, registers.flags, elements, registers.end, registers.cursor };
#else
        const std::initializer_list<GPRReg> keptWhereNested { made, registers.flags };
#endif
        if (isNested)
            reader.emitColdParts(exits.bailouts, keptWhereNested, loadDistance, [] { }, [] { });
        emitColdParts(jit, registers, coldParts, exits.bailouts, kept, loadDistance, [] { });
        exits.bailouts.link(&jit);
        jit.move(TrustedImm32(0), registers.value);
        isMade.link(&jit);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, registers.value));
    jit.move(registers.value, jit.result());
    jit.returnToCaller();
    checkOtherArray(jit, registers, check, source, false, exits);
    callMethod(jit, registers, exits, false);
}

StubSignature signatureOfMapArrayToTest(StubKey) { return signatureOf(callMethodById(1)); }

void generateMapArrayToTest(StubAssembler& jit)
{
    const CallbackSource source = sourceOf(jit.key().form() / numberOfElementTests);
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    ElementTester tester(jit, registers, source, static_cast<ElementTest>(jit.key().form() % numberOfElementTests));
    const GPRReg length = gprOf(StubRegister::T9);
    const GPRReg negation = length;
    const GPRReg made = thisGPR;
    const GPRReg elements = registers.flags;
    const GPRReg value = registers.value;
    RELEASE_ASSERT(length == jit.immediate());
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    checkSpecies(jit, registers, exits.methodCalls);
    tester.prepareOperand(exits.methodCalls);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), value);
    jit.load32(Address(value, Butterfly::offsetOfPublicLength()), value);
    jit.sub32(TrustedImm32(1), value);
    exits.methodCalls.append(jit.branch32(CCallHelpers::AboveOrEqual, value, TrustedImm32(mostElementsOfNewArray)));

    jit.emitFunctionPrologue();
    const std::array preserved = preservedRegisters(jit, registers);
    ColdParts coldParts;
    jit.preserving(std::span<const GPRReg>(preserved), [&] {
        JumpList isDone;
        auto restoreNumbers = [&] {
            if (tester.keepsBound())
                tester.prepareBound();
        };
        auto mapElements = [&](Elements elementsRead) {
            jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), length);
            jit.load32(Address(length, Butterfly::offsetOfPublicLength()), length);
            makeArray(jit, registers, ArrayElementKind::Contiguous, { thisGPR, registers.answer, registers.place, registers.flags, registers.operand });
            restoreNumbers();
            loadCursorAndEnd(jit, registers);
            jit.move(registers.flags, negation);
            jit.and32(TrustedImm32(ArrayCallbackFlags::testIsNegated), negation);
            jit.move(value, made);
            jit.loadPtr(Address(made, JSObject::butterflyOffset()), elements);
            Label next = jit.label();
            loadValue(jit, registers, elementsRead, coldParts, exits.bailouts, EmptyField::IsLeftToTest);
            JumpList passes;
            tester.emitTest(passes);
            Label failed = jit.label();
            jit.move(TrustedImm32(JSValue::ValueFalse), value);
            Label hasResult = jit.label();
            jit.xor32(negation, value);
            jit.store64(value, Address(elements));
            jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
            advance(jit, registers, next);
            isDone.append(jit.jump());
            passes.link(&jit);
            tester.finishTest(jit.label(), failed);
            jit.move(TrustedImm32(JSValue::ValueTrue), value);
            jit.jump().linkTo(hasResult, &jit);
        };
        static_assert((JSValue::ValueTrue ^ ArrayCallbackFlags::testIsNegated) == JSValue::ValueFalse);
        if (source == CallbackSource::Element)
            mapElements(Elements::Values);
        else {
            loadCursorAndEnd(jit, registers);
            Jump hasShape = loadPlaceOfField(jit, registers, exits.bailouts, false);
            mapElements(Elements::FieldsByStructure);
            hasShape.link(&jit);
            mapElements(Elements::FieldsByShape);
        }

        isDone.link(&jit);
        jit.move(made, value);
        Jump isMade = jit.jump();
#if CPU(X86_64)
        const std::initializer_list<GPRReg> kept { made, registers.place, elements, registers.operand, registers.end, registers.cursor, negation };
#else
        const std::initializer_list<GPRReg> kept { made, elements, negation };
#endif
        emitColdParts(jit, registers, coldParts, exits.bailouts, kept, [&](GPRReg distance, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distance, bytesBelow); }, restoreNumbers);
        tester.emitColdTests(exits.bailouts);
        exits.bailouts.link(&jit);
        jit.move(TrustedImm32(0), value);
        isMade.link(&jit);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, value));
    jit.move(value, jit.result());
    jit.returnToCaller();
    checkOtherArray(jit, registers, check, source, false, exits);
    callMethod(jit, registers, exits, false);
}

static void emitOperationOnFieldPair(StubAssembler& jit, FieldPairOperation operation, FPRReg other, FPRReg number)
{
    switch (operation) {
    case FieldPairOperation::Sum:
        jit.addDouble(other, number);
        return;
    case FieldPairOperation::Difference:
        jit.subDouble(other, number);
        return;
    case FieldPairOperation::Product:
        jit.mulDouble(other, number);
        return;
    case FieldPairOperation::FieldOfField:
        return;
    }
}

StubSignature signatureOfMapArrayToNumber(StubKey) { return signatureOf(callMethodById(1)); }

void generateMapArrayToNumber(StubAssembler& jit)
{
    const CallbackSource source = sourceOf(jit.key().form() / numberOfNumberMaps);
    const NumberMap map = static_cast<NumberMap>(jit.key().form() % numberOfNumberMaps);
    const bool multiplies = map == NumberMap::Product;
    const bool divides = map == NumberMap::Quotient;
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const FPRReg operand = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg length = gprOf(StubRegister::T9);
    const GPRReg made = thisGPR;
    const GPRReg elements = registers.flags;
    const GPRReg value = registers.value;
    RELEASE_ASSERT(length == jit.immediate());
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    auto checkOperands = [&] {
        checkSpecies(jit, registers, exits.methodCalls);
        exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, registers.operand, numberTag));
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), value);
        jit.load32(Address(value, Butterfly::offsetOfPublicLength()), value);
        jit.sub32(TrustedImm32(1), value);
        exits.methodCalls.append(jit.branch32(CCallHelpers::AboveOrEqual, value, TrustedImm32(mostElementsOfNewArray)));
    };

    auto unboxOperand = [&] {
        Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, registers.operand, numberTag);
        jit.add64(numberTag, registers.operand);
        jit.move64ToDouble(registers.operand, operand);
        jit.sub64(numberTag, registers.operand);
        Jump isUnboxed = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(registers.operand, operand);
        isUnboxed.link(&jit);
    };
    auto beginArray = [&](ArrayElementKind kind) {
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), length);
        jit.load32(Address(length, Butterfly::offsetOfPublicLength()), length);
        makeArray(jit, registers, kind, { thisGPR, registers.answer, registers.place, registers.operand });
        loadCursorAndEnd(jit, registers);
        jit.move(value, made);
        jit.loadPtr(Address(made, JSObject::butterflyOffset()), elements);
    };
    JumpList isMade;
    auto mapInFrame = [&](bool amongDoubles) {
        exits.bailouts.clear();
        jit.emitFunctionPrologue();
        const std::array preserved = preservedRegisters(jit, registers);
        ColdParts coldParts;
        jit.preserving(std::span<const GPRReg>(preserved), [&] {
            JumpList isDone;
            auto mapElements = [&](Elements elementsRead) {
                JumpList hasNoInt32;
                Jump beginsWithDouble;
                if (!amongDoubles && !divides) {
                    loadValue(jit, registers, elementsRead, coldParts, exits.bailouts, EmptyField::IsLeftToTest);
                    Jump beginsWithInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
                    exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
                    beginsWithDouble = jit.jump();
                    beginsWithInt32.link(&jit);
                    Jump usesDouble = jit.branch64(CCallHelpers::Below, registers.operand, numberTag);
                    beginArray(ArrayElementKind::Int32);
                    Label nextInteger = jit.label();
                    loadValue(jit, registers, elementsRead, coldParts, exits.bailouts, EmptyField::IsLeftToTest);
                    hasNoInt32.append(jit.branch64(CCallHelpers::Below, value, numberTag));
                    Jump isZero;
                    if (multiplies) {
                        hasNoInt32.append(jit.branchMul32(CCallHelpers::Overflow, registers.operand, value));
                        isZero = jit.branchTest32(CCallHelpers::Zero, value);
                    } else
                        hasNoInt32.append(jit.branchAdd32(CCallHelpers::Overflow, registers.operand, value));
                    Label hasInt32 = jit.label();
                    jit.zeroExtend32ToWord(value, value);
                    jit.or64(numberTag, value);
                    jit.store64(value, Address(elements));
                    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
                    advance(jit, registers, nextInteger);
                    isDone.append(jit.jump());
                    if (multiplies) {
                        isZero.link(&jit);
                        jit.branch32(CCallHelpers::GreaterThan, registers.operand, TrustedImm32(0)).linkTo(hasInt32, &jit);
                    }
                    hasNoInt32.link(&jit);
                    jit.load64(Address(CCallHelpers::stackPointerRegister), thisGPR);
                    usesDouble.link(&jit);
                    beginsWithDouble.link(&jit);
                }
                beginArray(ArrayElementKind::Double);
                unboxOperand();
                Label nextNumber = jit.label();
                if (amongDoubles)
                    loadNumberInDoubles(jit, registers, number, exits.bailouts);
                else {
                    loadValue(jit, registers, elementsRead, coldParts, exits.bailouts, EmptyField::IsLeftToTest);
                    Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, value, numberTag);
                    exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
                    unboxDouble(jit, value, value, number);
                    Jump isUnboxed = jit.jump();
                    isInt32.link(&jit);
                    jit.convertInt32ToDouble(value, number);
                    isUnboxed.link(&jit);
                }
                if (divides)
                    jit.divDouble(operand, number);
                else if (multiplies)
                    jit.mulDouble(operand, number);
                else
                    jit.addDouble(operand, number);
                exits.bailouts.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, number, number));
                jit.storeDouble(number, Address(elements));
                jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
                advance(jit, registers, nextNumber);
                isDone.append(jit.jump());
            };
            loadCursorAndEnd(jit, registers);
            if (source == CallbackSource::Element)
                mapElements(amongDoubles ? Elements::Doubles : Elements::Values);
            else {
                Jump hasShape = loadPlaceOfField(jit, registers, exits.bailouts, false);
                mapElements(Elements::FieldsByStructure);
                hasShape.link(&jit);
                mapElements(Elements::FieldsByShape);
            }

            isDone.link(&jit);
            jit.move(made, value);
            Jump hasArray = jit.jump();
#if CPU(X86_64)
            const std::initializer_list<GPRReg> kept { made, registers.place, elements, registers.operand, registers.end, registers.cursor };
#else
            const std::initializer_list<GPRReg> kept { elements };
#endif
            emitColdParts(jit, registers, coldParts, exits.bailouts, kept, [&](GPRReg distance, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distance, bytesBelow); }, unboxOperand);
            exits.bailouts.link(&jit);
            jit.move(TrustedImm32(0), value);
            hasArray.link(&jit);
        });
        jit.emitFunctionEpilogueWithEmptyFrame();
        exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, value));
        isMade.append(jit.jump());
    };
    checkOperands();
    mapInFrame(false);
    Jump holdsDoubles = checkOtherArray(jit, registers, check, source, source == CallbackSource::Element, exits);
    if (holdsDoubles.isSet()) {
        holdsDoubles.link(&jit);
        checkOperands();
        mapInFrame(true);
    }
    isMade.link(&jit);
    jit.move(value, jit.result());
    jit.returnToCaller();
    callMethod(jit, registers, exits, false);
}

StubSignature signatureOfMapFieldPairs(StubKey) { return signatureOf(callMethodById(1)); }

void generateMapFieldPairs(StubAssembler& jit)
{
    const FieldPairOperation operation = static_cast<FieldPairOperation>(jit.key().form());
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg otherNumber = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg second = gprOf(StubRegister::T9);
    const GPRReg made = thisGPR;
    const GPRReg elements = registers.flags;
    const GPRReg placeOfSecond = registers.operand;
    const GPRReg value = registers.value;
    RELEASE_ASSERT(second == jit.immediate());
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    checkSpecies(jit, registers, exits.methodCalls);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), value);
    jit.load32(Address(value, Butterfly::offsetOfPublicLength()), value);
    jit.sub32(TrustedImm32(1), value);
    exits.methodCalls.append(jit.branch32(CCallHelpers::AboveOrEqual, value, TrustedImm32(mostElementsOfNewArray)));

    jit.emitFunctionPrologue();
    const std::array preserved = preservedRegisters(jit, registers);
    FieldPairReader reader(jit, registers, placeOfSecond, second, false);
    jit.preserving(std::span<const GPRReg>(preserved), [&] {
        JumpList isDone;
        auto beginArray = [&](ArrayElementKind kind) {
            jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), second);
            jit.load32(Address(second, Butterfly::offsetOfPublicLength()), second);
            makeArray(jit, registers, kind, { thisGPR, registers.answer, registers.place, placeOfSecond });
            loadCursorAndEnd(jit, registers);
            jit.move(value, made);
            jit.loadPtr(Address(made, JSObject::butterflyOffset()), elements);
        };
        auto unbox = [&](GPRReg boxed, FPRReg unboxed) {
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, boxed, numberTag);
            exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, boxed, numberTag));
            unboxDouble(jit, boxed, boxed, unboxed);
            Jump isUnboxed = jit.jump();
            isInt32.link(&jit);
            jit.convertInt32ToDouble(boxed, unboxed);
            isUnboxed.link(&jit);
        };
        auto mapElements = [&](Elements elementsRead) {
            JumpList hasNoInt32;
            JumpList beginsWithDouble;
            reader.loadFields(elementsRead, exits.bailouts);
            exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, value, numberTag));
            exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, second, numberTag));
            beginsWithDouble.append(jit.branch64(CCallHelpers::Below, value, numberTag));
            beginsWithDouble.append(jit.branch64(CCallHelpers::Below, second, numberTag));
            beginArray(ArrayElementKind::Int32);
            Label nextInteger = jit.label();
            reader.loadFields(elementsRead, exits.bailouts);
            hasNoInt32.append(jit.branch64(CCallHelpers::Below, value, numberTag));
            hasNoInt32.append(jit.branch64(CCallHelpers::Below, second, numberTag));
            switch (operation) {
            case FieldPairOperation::Sum:
                hasNoInt32.append(jit.branchAdd32(CCallHelpers::Overflow, second, value));
                break;
            case FieldPairOperation::Difference:
                hasNoInt32.append(jit.branchSub32(CCallHelpers::Overflow, second, value));
                break;
            case FieldPairOperation::Product:
                hasNoInt32.append(jit.branchMul32(CCallHelpers::Overflow, second, value));
                hasNoInt32.append(jit.branchTest32(CCallHelpers::Zero, value));
                break;
            case FieldPairOperation::FieldOfField:
                RELEASE_ASSERT_NOT_REACHED();
            }
            jit.zeroExtend32ToWord(value, value);
            jit.or64(numberTag, value);
            jit.store64(value, Address(elements));
            jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
            advance(jit, registers, nextInteger);
            isDone.append(jit.jump());

            hasNoInt32.link(&jit);
            jit.load64(Address(CCallHelpers::stackPointerRegister), thisGPR);
            beginsWithDouble.link(&jit);
            beginArray(ArrayElementKind::Double);
            Label nextNumber = jit.label();
            reader.loadFields(elementsRead, exits.bailouts);
            unbox(value, number);
            unbox(second, otherNumber);
            emitOperationOnFieldPair(jit, operation, otherNumber, number);
            exits.bailouts.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, number, number));
            jit.storeDouble(number, Address(elements));
            jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
            advance(jit, registers, nextNumber);
            isDone.append(jit.jump());
        };
        loadCursorAndEnd(jit, registers);
        JumpList hasShape = reader.loadPlaces([&](GPRReg distances) { loadPreservedDistance(jit, preserved, distances, 0); }, exits.bailouts);
        reader.emitForEachForm(mapElements, hasShape);

        isDone.link(&jit);
        jit.move(made, value);
        Jump hasArray = jit.jump();
#if CPU(X86_64)
        const std::initializer_list<GPRReg> kept { made, registers.place, placeOfSecond, elements, registers.end, registers.cursor };
#else
        const std::initializer_list<GPRReg> kept { elements };
#endif
        reader.emitColdParts(exits.bailouts, kept, [&](GPRReg distances, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distances, bytesBelow); }, [] { }, [] { });
        exits.bailouts.link(&jit);
        jit.move(TrustedImm32(0), value);
        hasArray.link(&jit);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, value));
    jit.move(value, jit.result());
    jit.returnToCaller();
    checkOtherArray(jit, registers, check, CallbackSource::Field, false, exits);
    callMethod(jit, registers, exits, false);
}

StubSignature signatureOfFoldFieldPairs(StubKey) { return signatureOf(callMethodById(1)); }

void generateFoldFieldPairs(StubAssembler& jit)
{
    const FieldPairOperation operation = static_cast<FieldPairOperation>(jit.key().form());
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg addend = jit.allocateScratch(FPRInfo::fpRegT1);
    const FPRReg otherNumber = jit.allocateScratch(FPRInfo::fpRegT2);
    const GPRReg second = gprOf(StubRegister::T9);
    const GPRReg value = registers.value;
    RELEASE_ASSERT(second == jit.immediate());
#if CPU(X86_64)
    const GPRReg sum = registers.operand;
    const GPRReg placeOfSecond = registers.flags;
    constexpr size_t wordsBelowDistances = 3;
    auto save = [&] {
        jit.push(registers.operand);
        jit.push(registers.operand);
        saveCallback(jit, registers);
        jit.push(jit.immediate());
        jit.push(registers.flags);
    };
    auto restore = [&] {
        jit.pop(registers.flags);
        jit.pop(jit.immediate());
        restoreCallback(jit, registers);
        jit.pop(registers.operand);
        jit.pop(registers.operand);
    };
    auto loadDistances = [&](GPRReg distances) { jit.load64(Address(CCallHelpers::stackPointerRegister, wordsBelowDistances * sizeof(CPURegister)), distances); };
    auto loadDistancesInFrame = [&](GPRReg distances, size_t) { jit.load64(Address(GPRInfo::callFrameRegister, (1 + wordsBelowDistances) * sizeof(CPURegister)), distances); };
#else
    const GPRReg sum = jit.allocateScratch(StubRegister::T15);
    const GPRReg placeOfSecond = registers.methodWord;
    const GPRReg immediate = registers.scratchOfMethodWord;
    auto save = [&] {
        jit.move(registers.operand, sum);
        saveCallback(jit, registers);
        jit.move(jit.immediate(), immediate);
    };
    auto restore = [&] {
        jit.move(immediate, jit.immediate());
        restoreCallback(jit, registers);
    };
    auto loadDistances = [&](GPRReg distances) { jit.move(registers.distance, distances); };
    auto loadDistancesInFrame = [&](GPRReg distances, size_t) { jit.move(registers.distance, distances); };
#endif
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    save();
    loadCursorAndEnd(jit, registers);
    exits.bailouts.append(branchAtEnd(jit, registers));

    const bool isNested = operation == FieldPairOperation::FieldOfField;
    FieldPairReader reader(jit, registers, placeOfSecond, second, isNested);
    Vector<std::pair<JumpList, Label>, 4> coldPartsOfIntegers;
    Vector<std::pair<JumpList, Label>, 4> coldPartsOfNumbers;
    JumpList isAtEndOfIntegers;
    JumpList isAtEndOfNumbers;
    auto convertSum = [&] {
        jit.rshift64(sum, TrustedImm32(53), value);
        jit.add64(TrustedImm32(1), value);
        exits.bailouts.append(jit.branch64(CCallHelpers::Above, value, TrustedImm32(1)));
        jit.convertInt64ToDouble(sum, number);
    };
    auto unbox = [&](GPRReg boxed, FPRReg unboxed) {
        Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, boxed, numberTag);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, boxed, numberTag));
        unboxDouble(jit, boxed, boxed, unboxed);
        Jump isUnboxed = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(boxed, unboxed);
        isUnboxed.link(&jit);
    };
    auto foldElements = [&](Elements elementsRead) {
        Jump beginsWithInt32 = jit.branch64(CCallHelpers::AboveOrEqual, sum, numberTag);
        exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, sum, numberTag));
        unboxDouble(jit, sum, sum, number);
        Jump entersNumbers = jit.jump();
        beginsWithInt32.link(&jit);
        jit.signExtend32ToPtr(sum, sum);

        reader.useColdParts(coldPartsOfIntegers);
        Label nextInteger = jit.label();
        reader.loadFields(elementsRead, exits.bailouts);
        JumpList hasNoInt32;
        hasNoInt32.append(jit.branch64(CCallHelpers::Below, value, numberTag));
        if (!isNested)
            hasNoInt32.append(jit.branch64(CCallHelpers::Below, second, numberTag));
        switch (operation) {
        case FieldPairOperation::Sum:
            hasNoInt32.append(jit.branchAdd32(CCallHelpers::Overflow, second, value));
            break;
        case FieldPairOperation::Difference:
            hasNoInt32.append(jit.branchSub32(CCallHelpers::Overflow, second, value));
            break;
        case FieldPairOperation::Product:
            hasNoInt32.append(jit.branchMul32(CCallHelpers::Overflow, second, value));
            break;
        case FieldPairOperation::FieldOfField:
            break;
        }
        jit.signExtend32ToPtr(value, value);
        jit.add64(value, sum);
        advance(jit, registers, nextInteger);
        isAtEndOfIntegers.append(jit.jump());

        hasNoInt32.link(&jit);
        convertSum();
        entersNumbers.link(&jit);
        reader.useColdParts(coldPartsOfNumbers);
        Label nextNumber = jit.label();
        reader.loadFields(elementsRead, exits.bailouts);
        unbox(value, addend);
        if (!isNested)
            unbox(second, otherNumber);
        emitOperationOnFieldPair(jit, operation, otherNumber, addend);
        jit.addDouble(addend, number);
        advance(jit, registers, nextNumber);
        isAtEndOfNumbers.append(jit.jump());
    };
    reader.useColdParts(coldPartsOfIntegers);
    JumpList hasShape = reader.loadPlaces(loadDistances, exits.bailouts);
    reader.emitForEachForm(foldElements, hasShape);

    auto returnValue = [&] {
        restore();
        jit.move(value, jit.result());
        jit.returnToCaller();
    };
    isAtEndOfIntegers.link(&jit);
    jit.signExtend32ToPtr(sum, value);
    Jump isWide = jit.branch64(CCallHelpers::NotEqual, value, sum);
    jit.zeroExtend32ToWord(sum, value);
    jit.or64(numberTag, value);
    returnValue();
    isWide.link(&jit);
    convertSum();
    isAtEndOfNumbers.link(&jit);
    jit.purifyNaN(number, addend);
    jit.boxDouble(addend, value);
    returnValue();

#if CPU(X86_64)
    const std::initializer_list<GPRReg> kept { thisGPR, registers.place, placeOfSecond, sum, registers.end, registers.cursor };
#else
    const std::initializer_list<GPRReg> kept { registers.callback, registers.flags, sum, placeOfSecond, immediate };
#endif
    auto enter = [&] { jit.emitFunctionPrologue(); };
    auto leave = [&] { jit.emitFunctionEpilogueWithEmptyFrame(); };
    reader.useColdParts(coldPartsOfIntegers);
    reader.emitColdParts(exits.bailouts, kept, loadDistancesInFrame, enter, leave);
    reader.useColdParts(coldPartsOfNumbers);
    reader.emitColdParts(exits.bailouts, kept, loadDistancesInFrame, [&] {
        jit.moveDoubleTo64(number, sum);
        enter();
    }, [&] {
        leave();
        jit.move64ToDouble(sum, number);
    });
    checkOtherArray(jit, registers, check, CallbackSource::Field, false, exits);
    exits.bailouts.link(&jit);
    restore();
    callMethod(jit, registers, exits, true);
}

StubSignature signatureOfFilterArray(StubKey) { return signatureOf(callMethodById(1)); }

void generateFilterArray(StubAssembler& jit)
{
    const CallbackSource source = sourceOf(jit.key().form() / numberOfElementTests);
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    ElementTester tester(jit, registers, source, static_cast<ElementTest>(jit.key().form() % numberOfElementTests));
    const GPRReg count = gprOf(StubRegister::T9);
    const GPRReg made = count;
    const GPRReg elements = thisGPR;
    RELEASE_ASSERT(count == jit.immediate());
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    checkSpecies(jit, registers, exits.methodCalls);
    tester.prepareOperand(exits.methodCalls);

    constexpr uint32_t storesElements = ArrayCallbackFlags::hasNoInitialValue;
    JumpList isMade;
    auto filter = [&](bool amongDoubles) {
        exits.bailouts.clear();
        jit.emitFunctionPrologue();
        const std::array preserved = preservedRegisters(jit, registers);
        ColdParts coldParts;
        ColdParts coldPartsOfShort;
        JumpList bailoutsOfShort;
        JumpList isCollected;
        jit.preserving(std::span<const GPRReg>(preserved), [&] {
            if (amongDoubles)
                tester.prepareForDoubles(exits.bailouts);
            loadCursorAndEnd(jit, registers);
            JumpList isDone;
            auto collect = [&](Elements elementsRead, bool keepsFailed) {
                Label next = jit.label();
                loadValue(jit, registers, elementsRead, coldPartsOfShort, bailoutsOfShort, EmptyField::IsLeftToTest);
                JumpList passes;
                tester.emitTest(passes);
                Label failed = jit.label();
                Label keeps = failed;
                Label skips = failed;
                auto keepElement = [&] {
                    jit.load64(Address(registers.cursor), registers.value);
                    jit.store64(registers.value, Address(elements));
                    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
                };
                if (keepsFailed)
                    keepElement();
                skips = keepsFailed ? jit.label() : failed;
                advance(jit, registers, next);
                isCollected.append(jit.jump());
                passes.link(&jit);
                if (!keepsFailed) {
                    keeps = jit.label();
                    keepElement();
                }
                tester.finishTest(keepsFailed ? skips : keeps, failed);
                jit.jump().linkTo(skips, &jit);
            };
            auto emitPasses = [&](Elements elementsRead) {
                if (!amongDoubles) {
                    jit.move(registers.end, registers.value);
                    jit.subPtr(registers.cursor, registers.value);
                    jit.sub64(TrustedImm32(1), registers.value);
                    Jump isLong = jit.branch64(CCallHelpers::AboveOrEqual, registers.value, TrustedImm32(bytesOnStack));
                    jit.subPtr(TrustedImm32(bytesOnStack), CCallHelpers::stackPointerRegister);
                    jit.move(CCallHelpers::stackPointerRegister, elements);
                    Jump keepsFailed = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated));
                    collect(elementsRead, false);
                    keepsFailed.link(&jit);
                    collect(elementsRead, true);
                    tester.emitColdTests(bailoutsOfShort);
                    isLong.link(&jit);
                }
                jit.move(TrustedImm32(0), count);
                Label begins = jit.label();
                JumpList isAtEnd;
                isAtEnd.append(branchAtEnd(jit, registers));
                Label next = jit.label();
                JumpList passes;
                if (amongDoubles) {
                    loadNumberInDoubles(jit, registers, tester.number(), exits.bailouts);
                    tester.emitTestOfNumber(passes, exits.bailouts);
                } else {
                    loadValue(jit, registers, elementsRead, coldParts, exits.bailouts);
                    tester.emitTest(passes);
                }
                Label failed = jit.label();
                Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated));
                Label skip = jit.label();
                advance(jit, registers, next);
                isAtEnd.append(jit.jump());
                passes.link(&jit);
                if (!amongDoubles)
                    tester.finishTest(jit.label(), failed);
                jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated)).linkTo(skip, &jit);
                isTaken.link(&jit);
                Jump stores = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(storesElements));
                jit.add32(TrustedImm32(1), count);
                jit.jump().linkTo(skip, &jit);
                stores.link(&jit);
                jit.load64(Address(registers.cursor), registers.value);
                jit.store64(registers.value, Address(elements));
                jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
                jit.jump().linkTo(skip, &jit);

                isAtEnd.link(&jit);
                isDone.append(jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(storesElements)));
                exits.bailouts.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(mostElementsOfNewArray)));
                const std::initializer_list<GPRReg> kept { thisGPR, registers.answer, registers.place, registers.flags, registers.operand };
                if (amongDoubles)
                    makeArray(jit, registers, ArrayElementKind::Double, kept);
                else {
                    Jump holdsObjects;
                    if (source == CallbackSource::Element) {
                        loadShape(jit, registers.value);
                        holdsObjects = jit.branch32(CCallHelpers::NotEqual, registers.value, TrustedImm32(Int32Shape));
                        makeArray(jit, registers, ArrayElementKind::Int32, kept);
                    }
                    Jump isOfIntegers = source == CallbackSource::Element ? jit.jump() : Jump();
                    if (holdsObjects.isSet())
                        holdsObjects.link(&jit);
                    makeArray(jit, registers, ArrayElementKind::Contiguous, kept);
                    if (isOfIntegers.isSet())
                        isOfIntegers.link(&jit);
                }
                loadCursorAndEnd(jit, registers);
                jit.move(registers.value, made);
                if (tester.keepsBound())
                    tester.prepareBound();
                jit.loadPtr(Address(made, JSObject::butterflyOffset()), elements);
                jit.or32(TrustedImm32(storesElements), registers.flags);
                jit.jump().linkTo(begins, &jit);
                tester.emitColdTests(exits.bailouts);
            };
            if (source == CallbackSource::Element)
                emitPasses(amongDoubles ? Elements::Doubles : Elements::Values);
            else {
                Jump hasShape = loadPlaceOfField(jit, registers, exits.bailouts);
                emitPasses(Elements::FieldsByStructure);
                hasShape.link(&jit);
                emitPasses(Elements::FieldsByShape);
            }

            isDone.link(&jit);
            jit.move(made, registers.value);
            JumpList hasArray;
            hasArray.append(jit.jump());
#if CPU(X86_64)
            const std::initializer_list<GPRReg> keptAtSite { thisGPR, registers.place, registers.flags, registers.operand, registers.end, registers.cursor, count };
#else
            const std::initializer_list<GPRReg> keptAtSite { registers.flags, count };
#endif
            auto restoreNumbers = [&] {
                if (tester.keepsBound())
                    tester.prepareBound();
            };
            emitColdParts(jit, registers, coldParts, exits.bailouts, keptAtSite, [&](GPRReg distance, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distance, bytesBelow); }, restoreNumbers);
            if (!amongDoubles) {
                isCollected.link(&jit);
                countCollectedElements(jit, registers, elements, count);
                Jump holdsObjects;
                if (source == CallbackSource::Element) {
                    jit.loadPtr(Address(CCallHelpers::stackPointerRegister, bytesOnStack), thisGPR);
                    loadShape(jit, registers.value);
                    holdsObjects = jit.branch32(CCallHelpers::NotEqual, registers.value, TrustedImm32(Int32Shape));
                    makeArray(jit, registers, ArrayElementKind::Int32, { registers.end });
                }
                Jump isOfIntegers = source == CallbackSource::Element ? jit.jump() : Jump();
                if (holdsObjects.isSet())
                    holdsObjects.link(&jit);
                makeArray(jit, registers, ArrayElementKind::Contiguous, { registers.end });
                if (isOfIntegers.isSet())
                    isOfIntegers.link(&jit);
                takeCollectedElements(jit, registers, elements, count);
                hasArray.append(jit.jump());
#if CPU(X86_64)
                const std::initializer_list<GPRReg> keptOfShort { thisGPR, registers.place, registers.flags, registers.operand, registers.end, registers.cursor };
#else
                const std::initializer_list<GPRReg> keptOfShort { thisGPR, registers.flags };
#endif
                emitColdParts(jit, registers, coldPartsOfShort, bailoutsOfShort, keptOfShort, [&](GPRReg distance, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distance, bytesBelow + bytesOnStack); }, restoreNumbers);
                bailoutsOfShort.link(&jit);
                jit.addPtr(TrustedImm32(bytesOnStack), CCallHelpers::stackPointerRegister);
            }
            exits.bailouts.link(&jit);
            jit.move(TrustedImm32(0), registers.value);
            hasArray.link(&jit);
        });
        jit.emitFunctionEpilogueWithEmptyFrame();
        exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, registers.value));
        isMade.append(jit.jump());
    };
    filter(false);
    Jump holdsDoubles = checkOtherArray(jit, registers, check, source, tester.takesDoubles(), exits);
    if (holdsDoubles.isSet()) {
        holdsDoubles.link(&jit);
        checkSpecies(jit, registers, exits.methodCalls);
        tester.prepareOperand(exits.methodCalls, true);
        filter(true);
    }
    isMade.link(&jit);
    jit.move(registers.value, jit.result());
    jit.returnToCaller();
    callMethod(jit, registers, exits, false);
}

StubSignature signatureOfFilterFieldPairs(StubKey) { return signatureOf(callMethodById(1)); }

void generateFilterFieldPairs(StubAssembler& jit)
{
    const FieldPairTest test = static_cast<FieldPairTest>(jit.key().form());
    const CallbackRegisters registers = CallbackRegisters::allocate(jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg otherNumber = jit.allocateScratch(FPRInfo::fpRegT1);
    const GPRReg second = gprOf(StubRegister::T9);
    const GPRReg placeOfSecond = registers.operand;
    const GPRReg elements = thisGPR;
    const GPRReg value = registers.value;
    RELEASE_ASSERT(second == jit.immediate());
    CallbackExits exits;
    const ArrayCheck check = checkArray(jit, registers, exits);
    checkSpecies(jit, registers, exits.methodCalls);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), value);
    jit.load32(Address(value, Butterfly::offsetOfPublicLength()), value);
    jit.sub32(TrustedImm32(1), value);
    exits.methodCalls.append(jit.branch32(CCallHelpers::AboveOrEqual, value, TrustedImm32(mostElementsOnStack)));

    jit.emitFunctionPrologue();
    const std::array preserved = preservedRegisters(jit, registers);
    FieldPairReader reader(jit, registers, placeOfSecond, second, false);
    jit.preserving(std::span<const GPRReg>(preserved), [&] {
        struct ColdTest {
            JumpList isOther;
            Label passed;
            Label failed;
        };
        Vector<ColdTest, 6> coldTests;
        JumpList isCollected;
        auto collect = [&](Elements elementsRead, bool keepsFailed) {
            Label next = jit.label();
            reader.loadFields(elementsRead, exits.bailouts);
            JumpList isOther;
            isOther.append(jit.branch64(CCallHelpers::Below, value, numberTag));
            isOther.append(jit.branch64(CCallHelpers::Below, second, numberTag));
            Jump passes = jit.branch32(test == FieldPairTest::Equals ? CCallHelpers::Equal : test == FieldPairTest::IsLess ? CCallHelpers::LessThan : CCallHelpers::LessThanOrEqual, value, second);
            Label failed = jit.label();
            auto keepElement = [&] {
                jit.load64(Address(registers.cursor), value);
                jit.store64(value, Address(elements));
                jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), elements);
            };
            if (keepsFailed)
                keepElement();
            Label skips = jit.label();
            advance(jit, registers, next);
            isCollected.append(jit.jump());
            passes.link(&jit);
            Label passed = jit.label();
            if (!keepsFailed)
                keepElement();
            jit.jump().linkTo(skips, &jit);
            coldTests.append({ isOther, passed, failed });
        };
        auto loadDistances = [&](GPRReg distances, size_t bytesBelow) { loadPreservedDistance(jit, preserved, distances, bytesBelow + bytesOnStack); };
        loadCursorAndEnd(jit, registers);
        jit.subPtr(TrustedImm32(bytesOnStack), CCallHelpers::stackPointerRegister);
        jit.move(CCallHelpers::stackPointerRegister, elements);
        JumpList hasShape = reader.loadPlaces([&](GPRReg distances) { loadDistances(distances, 0); }, exits.bailouts);
        reader.emitForEachForm([&](Elements elementsRead) {
            Jump keepsFailed = jit.branchTest32(CCallHelpers::NonZero, registers.flags, TrustedImm32(ArrayCallbackFlags::testIsNegated));
            collect(elementsRead, false);
            keepsFailed.link(&jit);
            collect(elementsRead, true);
        }, hasShape);

        isCollected.link(&jit);
        countCollectedElements(jit, registers, elements, second);
        makeArray(jit, registers, ArrayElementKind::Contiguous, { registers.end });
        takeCollectedElements(jit, registers, elements, second);
        Jump hasArray = jit.jump();
        auto unbox = [&](GPRReg boxed, FPRReg unboxed) {
            Jump isInt32 = jit.branch64(CCallHelpers::AboveOrEqual, boxed, numberTag);
            exits.bailouts.append(jit.branchTest64(CCallHelpers::Zero, boxed, numberTag));
            unboxDouble(jit, boxed, boxed, unboxed);
            Jump isUnboxed = jit.jump();
            isInt32.link(&jit);
            jit.convertInt32ToDouble(boxed, unboxed);
            isUnboxed.link(&jit);
        };
        for (ColdTest& coldTest : coldTests) {
            coldTest.isOther.link(&jit);
            unbox(value, number);
            unbox(second, otherNumber);
            auto condition = test == FieldPairTest::Equals ? CCallHelpers::DoubleEqualAndOrdered : test == FieldPairTest::IsLess ? CCallHelpers::DoubleLessThanAndOrdered : CCallHelpers::DoubleLessThanOrEqualAndOrdered;
            jit.branchDouble(condition, number, otherNumber).linkTo(coldTest.passed, &jit);
            jit.jump().linkTo(coldTest.failed, &jit);
        }
#if CPU(X86_64)
        const std::initializer_list<GPRReg> kept { elements, registers.place, placeOfSecond, registers.flags, registers.end, registers.cursor };
#else
        const std::initializer_list<GPRReg> kept { elements, registers.flags };
#endif
        reader.emitColdParts(exits.bailouts, kept, loadDistances, [] { }, [] { });
        exits.bailouts.link(&jit);
        jit.addPtr(TrustedImm32(bytesOnStack), CCallHelpers::stackPointerRegister);
        jit.move(TrustedImm32(0), value);
        hasArray.link(&jit);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    exits.methodCalls.append(jit.branchTest64(CCallHelpers::Zero, value));
    jit.move(value, jit.result());
    jit.returnToCaller();
    checkOtherArray(jit, registers, check, CallbackSource::Field, false, exits);
    callMethod(jit, registers, exits, false);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
