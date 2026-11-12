/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTRegExpFlags.h"
#include "AOTSlotAccess.h"
#include "AOTSlotPlan.h"
#include "AOTStubCatalog.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsRegExp.h"
#include "JSCJSValueInlines.h"
#include "RememberedSlot.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr GPRReg registerOfValueInStraightAllocation = GPRInfo::argumentGPR1;
static_assert(!StraightEmitter::isResultRegister(registerOfValueInStraightAllocation));

static_assert([] {
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        if (bitsBeforeCreation(representation) && widthOf(representation) != sizeof(uint32_t) && widthOf(representation) != sizeof(uint64_t))
            return false;
    }
    return true;
}());

OpcodeSet StraightEmitter::opcodesOfAllocations()
{
    return {
        Opcode::NewObject, Opcode::NewDictionary, Opcode::FinishObject, Opcode::NewRegExp, Opcode::NewTemplateObject,
        Opcode::NewRuntimeError, Opcode::NewSuppressedError,
    };
}

static constexpr std::optional<size_t> cellSizeKnownToStraightAllocation(size_t bytes)
{
    if (bytes && bytes <= maxCellSizeInKey && !(bytes % cellSizeStep))
        return bytes;
    return std::nullopt;
}

static std::optional<uint64_t> bitsOfConstantInSlotOfStraightAllocation(SlotRepresentation representation, JSValue constant)
{
    if (representation == SlotRepresentation::JSValue)
        return static_cast<uint64_t>(JSValue::encode(constant));
    bool holdsInt32s = representation == SlotRepresentation::Int32 || representation == SlotRepresentation::OptionalInt32;
    if (!holdsInt32s && representation != SlotRepresentation::OptionalInt64 && representation != SlotRepresentation::OptionalDouble)
        return std::nullopt;
    if (constant.isUndefinedOrNull() && !traitsOf(representation).hasUndefined)
        return std::nullopt;
    if (constant.isUndefined())
        return traitsOf(representation).undefinedBits;
    if (constant.isNull())
        return traitsOf(representation).nullBits;
    uint64_t bits = 0;
    bool isWritten = constant.isInt32() ? tryWriteInt32(representation, &bits, constant.asInt32()) : constant.isDouble() && tryWriteDouble(representation, &bits, constant.asDouble());
    return isWritten ? std::optional(bits) : std::nullopt;
}

static bool allocationListsFieldInStraightForm(StraightEmitter& emitter, FieldReference field, ValueID value)
{
    const PlacedSlot& slot = emitter.plan().slot(field.layout, field.field);
    FieldFacts facts = emitter.facts().field(field);
    if (slot.storage != SlotStorage::Inline || slot.holdsAccessor || emitter.plan().layout(field.layout).holdsRawValueFromBirth(field.field))
        return false;
    if (facts.facts.contains(FieldFact::IsAccessor) || facts.slotType || slot.provenType)
        return false;
    if (!facts.formOfValue.isEmpty() && (emitter.index().isParameter(value) || emitter.index().opcodeOf(value) != Opcode::Absent))
        return false;
    if (slot.representation == SlotRepresentation::JSValue)
        return !slot.type;
    std::optional<JSValue> constant = emitter.constantOf(value);
    return constant && bitsOfConstantInSlotOfStraightAllocation(slot.representation, *constant);
}

static PropertyKeyID keyDefinedBehindStraightAllocation(StraightEmitter& emitter, FieldReference field)
{
    const Program& program = emitter.program();
    FieldFacts facts = emitter.facts().field(field);
    if (!facts.formOfValue.isEmpty())
        return { };
    if (field.field >= program.at(field.layout).fields.size())
        return facts.facts.contains(FieldFact::IsAccessor) ? PropertyKeyID() : emitter.plan().slots().keyOf(program, field);

    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    const Field& declaration = fieldAt(program, declarationOf(program, field));
    if (!declaration.form.isEmpty() || declaration.flags.containsAny(filledByAllocationAlone))
        return { };
    return declaration.key;
}

static StubKey stubThatDefinesBehindStraightAllocation(StraightEmitter& emitter, PropertyKeyID key)
{
    if (emitter.program().at(key).kind == PropertyKeyKind::CreatedSymbol && !emitter.plan().hasOneInstance())
        return putByIdOfSymbolOfRealm(PutMode::Define);
    return putByIdHashed(PutMode::Define);
}

static void defineBehindStraightAllocation(StraightEmitter& emitter, const BodyValue& value, std::span<const PropertyKeyID> keys, std::span<const ValueID> values)
{
    CCallHelpers& jit = emitter.jit();
    CCallHelpers::Address object = emitter.wordOfScratchArea(0);
    jit.store64(emitter.resultOfCall(), object);
    GPRReg result = emitter.takeResultRegister();
    for (size_t index = 0; index < keys.size(); ++index) {
        if (index)
            jit.load64(object, result);
        emitter.callStub(value, stubThatDefinesBehindStraightAllocation(emitter, keys[index]), { StraightArgument::inRegister(result), values[index] }, { .numberedImmediate = NumberedImmediate { emitter.mentionOfName(keys[index]) } });
    }
    jit.load64(object, result);
}

static bool isZeroWhileEmptyInStraightAllocation(const PlacedSlot& slot)
{
    return slot.storage != SlotStorage::Inline || !bitsBeforeCreation(slot.representation);
}

static void storeWhatEmptySlotsHoldInStraightAllocation(StraightEmitter& emitter, const PlannedLayout& planned, std::span<const FieldIndex> listed)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg cell = emitter.resultOfCall();
    std::optional<uint64_t> bitsInRegister;
    for (FieldIndex field = 0; field < planned.slots.size(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (isZeroWhileEmptyInStraightAllocation(slot) || std::ranges::contains(listed, field))
            continue;
        uint64_t bits = bitsBeforeCreation(slot.representation);
        CCallHelpers::Address address(cell, static_cast<int32_t>(slot.offset));
        if (widthOf(slot.representation) == sizeof(uint32_t)) {
            jit.store32(CCallHelpers::TrustedImm32(static_cast<int32_t>(bits)), address);
            continue;
        }
        if (bitsInRegister != bits) {
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(bits)), registerOfValueInStraightAllocation);
            bitsInRegister = bits;
        }
        jit.store64(registerOfValueInStraightAllocation, address);
    }
}

static void generateNewObject(StraightEmitter& emitter, const BodyValue& value)
{
    auto operation = value.as<Opcode::NewObject>();
    const Program& program = emitter.program();
    const Plan& plan = emitter.plan();
    LayoutID layout = operation.layout();
    const PlannedLayout& planned = plan.layout(layout);
    bool hasSlots = planned.hasSlots();
    bool isEnvironment = program.at(layout).isEnvironment();
    ValueID prototype = operation.prototypeOperand();
    std::span<const FieldIndex> fieldsAsWritten = operation.fields();
    std::span<const ValueID> valuesAsWritten = operation.valuesOperands();
    if (fieldsAsWritten.size() != valuesAsWritten.size() || planned.cellsAreHandedBack) {
        emitter.fail(value);
        return;
    }

    BitVector listedWithValue;
    for (size_t place = 0; place < fieldsAsWritten.size(); ++place) {
        if (valuesAsWritten[place] && (emitter.index().isParameter(valuesAsWritten[place]) || emitter.index().opcodeOf(valuesAsWritten[place]) != Opcode::Absent))
            listedWithValue.set(fieldsAsWritten[place]);
    }
    bool listsWhatIsHeldFilled = std::ranges::all_of(fieldsHeldFilledInSlots(program, emitter.facts().linked(), plan.slots(), layout), [&](FieldIndex field) {
        return listedWithValue.get(field);
    });
    if (!listsWhatIsHeldFilled) {
        emitter.fail(value);
        return;
    }

    Vector<FieldIndex, 16> fields;
    Vector<ValueID, 16> values;
    for (size_t index = 0; index < fieldsAsWritten.size(); ++index) {
        if (hasSlots && !plan.slot(layout, fieldsAsWritten[index]).hasStorage())
            continue;
        fields.append(fieldsAsWritten[index]);
        values.append(valuesAsWritten[index]);
    }

    size_t numberListed = 0;
    std::optional<PlacedAllocation> placed;
    if (hasSlots) {
        while (numberListed < fields.size() && allocationListsFieldInStraightForm(emitter, { layout, fields[numberListed] }, values[numberListed]))
            ++numberListed;
        if (isEnvironment)
            placed = plan.environmentAllocation(layout);
        else {
            placed = plan.allocation(program, layout, fields.span().first(numberListed));
            if (!placed) {
                numberListed = plan.keyOrders().lengthOfLongestBeginningWithOrderWord(layout, fields.span().first(numberListed));
                placed = plan.allocation(program, layout, fields.span().first(numberListed));
            }
        }
    }
    bool canBeMade = hasSlots ? placed && !placed->isBornUnfinished && (prototype || program.at(layout).kind != LayoutKind::Instance || plan.slots().hasDirectInstances(layout)) : program.at(layout).kind == LayoutKind::Record;
    if (!canBeMade || (isEnvironment && numberListed != fields.size()) || (prototype && (!hasSlots || isEnvironment || !fields.isEmpty()))) {
        emitter.fail(value);
        return;
    }

    Vector<PropertyKeyID, 16> keysDefinedBehind;
    for (size_t index = numberListed; index < fields.size(); ++index) {
        PropertyKeyID key = keyDefinedBehindStraightAllocation(emitter, { layout, fields[index] });
        if (!key || program.nameClassOf(key) == NameClass::Index || (hasSlots && !mayBeCreatedByStore(plan.slot(layout, fields[index]).representation))) {
            emitter.fail(value);
            return;
        }
        keysDefinedBehind.append(key);
    }

    CCallHelpers& jit = emitter.jit();
    if (!hasSlots)
        emitter.callStub(value, AOT::newDictionary(), { });
    else {
        std::span<const FieldIndex> listed = fields.span().first(numberListed);
        StraightArgument row = StraightArgument::number(emitter.mentions().allocation(*placed, isEnvironment ? std::span<const FieldIndex>() : listed));
        std::optional<size_t> cellSize = cellSizeKnownToStraightAllocation(placed->size);
        if (prototype) {
            StubKey stub = AOT::allocateInstance(cellSize);
            emitter.callStub(value, stub, { row, prototype });
            if (!std::ranges::all_of(planned.slots, isZeroWhileEmptyInStraightAllocation)) {
                CCallHelpers::Jump isWhole = jit.branchTest64(CCallHelpers::Zero, emitter.catalog().summaryOf(stub).signature.secondResult.gpr());
                storeWhatEmptySlotsHoldInStraightAllocation(emitter, planned, listed);
                isWhole.link(&jit);
            }
        } else {
            CellInitialization initialization = isObjectLayout(program.at(layout).kind) ? CellInitialization::HeaderAndWordsAndZeros : CellInitialization::HeaderAndZeros;
            emitter.callStub(value, AOT::allocateCell(initialization, cellSize), { row });
            GPRReg cell = emitter.resultOfCall();
            for (size_t index = 0; index < numberListed; ++index) {
                const PlacedSlot& slot = plan.slot(layout, fields[index]);
                CCallHelpers::Address address(cell, static_cast<int32_t>(slot.offset));
                if (std::optional<JSValue> constant = emitter.constantOf(values[index])) {
                    uint64_t bits = *bitsOfConstantInSlotOfStraightAllocation(slot.representation, *constant);
                    if (slot.width() == sizeof(uint32_t))
                        jit.store32(CCallHelpers::TrustedImm32(static_cast<int32_t>(bits)), address);
                    else
                        jit.store64(CCallHelpers::TrustedImm64(static_cast<int64_t>(bits)), address);
                    continue;
                }
                emitter.load(values[index], registerOfValueInStraightAllocation);
                jit.store64(registerOfValueInStraightAllocation, address);
            }
            storeWhatEmptySlotsHoldInStraightAllocation(emitter, planned, listed);
        }
        jit.storeFence();
    }
    if (!keysDefinedBehind.isEmpty())
        defineBehindStraightAllocation(emitter, value, keysDefinedBehind.span(), values.span().subspan(numberListed));
    emitter.define(value);
}

static void generateNewRegExp(StraightEmitter& emitter, const BodyValue& value)
{
    if (emitter.plan().deferredDefinitions().findMadeBy(emitter.function(), value.id())) {
        emitter.fail(value);
        return;
    }
    auto operation = value.as<Opcode::NewRegExp>();
    OptionSet<Yarr::Flags> flags = toYarrFlags(operation.regExpFlags());
    StraightArgument record = StraightArgument::number(emitter.mentions().regExp(operation.pattern(), flags));
    emitter.callOperation(value, RuntimeOperation::NewRegExpLiteral, { record, StraightArgument::word(flags.toRaw()), StraightArgument::word(noSharedRegExpOffset) });
    emitter.define(value);
}

static void generateNewTemplateObject(StraightEmitter& emitter, const BodyValue& value)
{
    Wire::SourceLocation written = emitter.program().at(emitter.function()).locationAt(value.position());
    if (!hasModule(written)) {
        emitter.fail(value);
        return;
    }

    Vector<uint32_t, 8> cookedAndRawStrings;
    for (TemplateString part : value.as<Opcode::NewTemplateObject>().templateStrings()) {
        cookedAndRawStrings.append(part.cooked ? part.cooked.index() : Wire::none);
        cookedAndRawStrings.append(part.raw.index());
    }
    MentionList& mentions = emitter.mentions();
    uint32_t request = mentions.request(RequestKind::TemplateObject, cookedAndRawStrings.span());
    uint32_t keyOfSiteCell[] { written.module, written.position };
    CodeSymbol siteCell { SymbolSpace::InstanceWord, mentions.templateObjectCell(written) };
    CodeSymbol cardOfSiteCell { SymbolSpace::InstanceWord, mentions.cardOfWord(NumberSpace::TemplateObjectCell, keyOfSiteCell) };

    CCallHelpers& jit = emitter.jit();
    RelocationRecorder& relocations = emitter.relocations();
    GPRReg result = emitter.takeResultRegister();
    relocations.loadInstanceWord(siteCell, InstanceWordWidth::Word64, instanceGPR, result);
    CCallHelpers::Jump isMade = jit.branchTest64(CCallHelpers::NonZero, result);
    emitter.callOperation(value, RuntimeOperation::NewTemplateObject, { StraightArgument::number(request), StraightArgument::word(written.module), StraightArgument::number(mentions.templateObjectCell(written)) });
    jit.storeFence();
    relocations.storeInstanceWord(siteCell, InstanceWordWidth::Word64, emitter.resultOfCall(), instanceGPR);
    jit.move(CCallHelpers::TrustedImm32(RememberedSlotSet::writtenCard), registerOfValueInStraightAllocation);
    relocations.storeInstanceWord(cardOfSiteCell, InstanceWordWidth::Byte, registerOfValueInStraightAllocation, instanceGPR);
    isMade.link(&jit);
    emitter.define(value, result);
}

static void generateNewRuntimeError(StraightEmitter& emitter, const BodyValue& value)
{
    auto operation = value.as<Opcode::NewRuntimeError>();
    StraightArgument errorKind = StraightArgument::word(static_cast<uint32_t>(operation.runtimeError()));
    if (StringID name = operation.name())
        emitter.callOperation(value, RuntimeOperation::NewRuntimeErrorWithName, { errorKind, StraightArgument::number(emitter.mentions().add(NumberSpace::ShownString, name)) }, { .capturesStackTrace = true });
    else
        emitter.callOperation(value, RuntimeOperation::NewRuntimeError, { errorKind }, { .capturesStackTrace = true });
    emitter.define(value);
}

void StraightEmitter::generateAllocation(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::NewObject:
        generateNewObject(*this, value);
        return;
    case Opcode::NewDictionary:
        callStub(value, AOT::newDictionary(), { });
        define(value);
        return;
    case Opcode::FinishObject:
        defineSameAs(value, value.as<Opcode::FinishObject>().objectOperand());
        return;
    case Opcode::NewRegExp:
        generateNewRegExp(*this, value);
        return;
    case Opcode::NewTemplateObject:
        generateNewTemplateObject(*this, value);
        return;
    case Opcode::NewRuntimeError:
        generateNewRuntimeError(*this, value);
        return;
    case Opcode::NewSuppressedError: {
        auto operation = value.as<Opcode::NewSuppressedError>();
        callOperation(value, RuntimeOperation::NewSuppressedError, { operation.errorOperand(), operation.suppressedOperand() }, { .capturesStackTrace = true });
        define(value);
        return;
    }
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
