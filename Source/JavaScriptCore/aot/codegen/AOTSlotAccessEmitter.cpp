/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTAccessPlan.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTCompileUnit.h"
#include "AOTEffectsNet.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTMemoryLocation.h"
#include "AOTObjectWords.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaleAnswers.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "GetterSetter.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSType.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static const PlacedSlot* extensionWordOfSlotAccess(const FieldAccess& access)
{
    return access.slot.isOutOfLine() ? &access.extensionWord : nullptr;
}

static MemoryLocation locationOfSlotAccess(const FieldAccess& access)
{
    return MemoryLocation::objectField(access.key);
}

static bool slotAccessFindsSlot(const FieldAccess& access)
{
    return access.coverage != FieldAccess::Coverage::NoMember && access.hasLocation && access.slot.hasStorage();
}

static bool slotAccessFindsSlotInEveryReceiver(const FieldAccess& access)
{
    return slotAccessFindsSlot(access) && access.coverage == FieldAccess::Coverage::EveryMember;
}

static Operand noteIntegerAddedToInSlot(Output& out, Operand read, const Type& proven, unsigned firstNewOperation, bool isReadBySumAlone)
{
    if (read.representation != Representation::Int64 || !proven.isInteger() || proven.integers.isEmpty() || !proven.integers.isSubsetOf({ minIntegerAddedTo, maxIntegerAddedTo }))
        return read;
    if (read.value->opcode() != Machine::Load || read.value->type() != Machine::Int64)
        return read;
    if (read.value->index() < firstNewOperation) {
        if (!isReadBySumAlone)
            return read;
        read.value = out.reload(read.value);
    }
    out.noteIntegerAddedTo(read.value);
    return read;
}

static ALWAYS_INLINE const PlacedSlot& slotOfMemberWithField(const Program& program, const Plan& plan, LayoutID member, PropertyKeyID key)
{
    std::optional<FieldIndex> field = plan.slots().findField(program, member, key);
    RELEASE_ASSERT(field);
    return plan.slot(member, *field);
}

LayoutSetID SlotAccessEmitter::layoutsThatTestAgainstEveryShapeLetThrough(ValueID name) const
{
    while (name) {
        const Operation& definition = m_code.at(name);
        if (definition.opcode != Opcode::Unbox && definition.opcode != Opcode::Narrowed)
            break;
        ValueID test = definition.opcode == Opcode::Narrowed ? definition.testOperand() : ValueID();
        if (test && definition.isOnTakenEdge() && !definition.narrowsHolderOfOperand() && !definition.narrowsOtherResultOfOperand()) {
            const Operation& asked = m_code.at(test);
            if (asked.opcode == Opcode::ShapeTest && m_code.isNeeded(test) && asked.objectOperand() == definition.valueOperand()) {
                Type known = typeOf(asked.objectOperand());
                if (asked.rejectsMarkedObjects() || asked.asksInFull() || !known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
                    return asked.layouts();
            }
        }
        name = definition.valueOperand();
    }
    return { };
}

FieldAccess SlotAccessEmitter::fieldAccessOf(const ValueContext& value, bool isStore) const
{
    FieldAccess access = fieldAccessFor(value->key(), value->receivers(), isStore);
    if (access.findsOffsetByShape)
        access.layoutsThatTestInCodeLetThrough = layoutsThatTestAgainstEveryShapeLetThrough(value->objectOperand());
    return access;
}

LayoutSetID SlotAccessEmitter::setOfMembersWithField(const FieldAccess& access)
{
    return types().layoutSets().ofSorted(access.membersWithField.span());
}

Operand SlotAccessEmitter::failRead(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

bool SlotAccessEmitter::receiversAreNeverAllocated(const ValueContext& value) const
{
    std::span<const LayoutID> receivers = membersOf(m_generator.inputs().types, value->receivers());
    return !receivers.empty() && std::ranges::none_of(receivers, [&](LayoutID receiver) {
        return m_plan.isInUse(receiver);
    });
}

Operand SlotAccessEmitter::resultWithoutSlot(const ValueContext& value)
{
    return receiversAreNeverAllocated(value) ? resultBehindTestThatNeverPasses(value) : failRead(value, InternalErrorKind::SlotAccessNotPlaced);
}

Operand SlotAccessEmitter::resultBehindTestThatNeverPasses(const ValueContext& value)
{
    m_out.unreachable();
    m_out.appendTo(newBlock());
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Operand SlotAccessEmitter::undefinedResult(const ValueContext& value)
{
    switch (value.representation()) {
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::JSValue:
        return emitter<ConstantEmitter>().constantOf(jsUndefined(), value.representation());
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    return failRead(value, InternalErrorKind::OpcodeWithoutCode);
}

Operand SlotAccessEmitter::inResultRepresentation(const ValueContext& value, Operand loaded)
{
    if (loaded.representation == value.representation() && loaded.representation != Representation::Unchosen)
        return loaded;
    return emitter<ConversionEmitter>().inRepresentation(value, loaded, typeOf(value.id()), value.representation());
}

static LValue appendDistanceByComparison(Output& out, LValue shapeWord, const DistancesByComparison& plan)
{
    LValue distance = out.constInt32(plan.comparesWithFirstShapes ? static_cast<int32_t>(plan.stretches[0].distance) : 0);
    for (const DistanceOfShapes& stretch : plan.stretches.span().subspan(plan.comparesWithFirstShapes ? 1 : 0)) {
        LValue isInStretch;
        if (plan.comparesWithFirstShapes)
            isInStretch = out.belowOrEqual(shapeWord, out.constInt32(static_cast<int32_t>(AOT::shapeWordOf(stretch.shapes.first))));
        else if (stretch.distance) {
            LValue distanceFromLast = out.add(shapeWord, out.constInt32(static_cast<int32_t>(stretch.shapes.end() - 1)));
            isInStretch = out.below(distanceFromLast, out.constInt32(static_cast<int32_t>(stretch.shapes.count)));
        } else
            continue;
        LValue distanceOfStretch = out.constInt32(static_cast<int32_t>(stretch.distance));
        distance = out.select(isInStretch, distanceOfStretch, distance);
    }
    return distance;
}

LValue appendDistanceOfShape(Output& out, const Machine::AbstractHeap& tables, LValue shapeWord, const DistancesByShape& distances, uint32_t constant)
{
    if (distances.form == DistanceForm::Comparisons)
        return appendDistanceByComparison(out, shapeWord, distances.comparisons);

    LValue index = distances.form == DistanceForm::HashedTable
        ? out.lShr(out.mul(shapeWord, out.constInt32(static_cast<int32_t>(distances.hashMultiplier))), static_cast<unsigned>(distances.hashShift))
        : out.add(shapeWord, out.constInt32(static_cast<int32_t>(distances.lastShape)));
    LValue table = out.relocatable(SymbolSpace::Constant, constant);
    auto loadElement = [&](LValue element, unsigned logBytes) {
        LValue extended = out.zeroExt(element, Machine::Int64);
        TypedPointer pointer = out.address(tables, out.add(table, logBytes ? out.shl(extended, logBytes) : extended));
        switch (logBytes) {
        case 0:
            return out.load8ZeroExt32(pointer);
        case 1:
            return out.load16ZeroExt32(pointer);
        case 2:
            return out.load32(pointer);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (distances.form != DistanceForm::PagedTable)
        return loadElement(index, distances.logBytesPerDistance);
    LValue entryOfPage = loadElement(out.lShr(index, static_cast<unsigned>(distances.logShapesPerPage)), distances.logBytesPerPageEntry);
    return loadElement(out.sub(entryOfPage, index), distances.logBytesPerDistance);
}

SlotAccessEmitter::~SlotAccessEmitter()
{
    constexpr size_t numberOfForms = 4;
    static_assert(!static_cast<size_t>(DistanceForm::Comparisons) && static_cast<size_t>(DistanceForm::FlatTable) == 1 && static_cast<size_t>(DistanceForm::HashedTable) == 2 && static_cast<size_t>(DistanceForm::PagedTable) == 3);
    static constexpr StatisticID sites[numberOfForms] { StatisticID::OffsetsFoundByComparisons, StatisticID::OffsetsFoundInFlatTables, StatisticID::OffsetsFoundInHashedTables, StatisticID::OffsetsFoundInPagedTables };
    static constexpr StatisticID bytes[numberOfForms] { StatisticID::None, StatisticID::BytesOfFlatOffsetTables, StatisticID::BytesOfHashedOffsetTables, StatisticID::BytesOfPagedOffsetTables };
    uint64_t numberOfSites[numberOfForms] { };
    uint64_t numberOfBytes[numberOfForms] { };
    for (const OffsetsByShape& offsets : m_offsetsByShape) {
        size_t form = static_cast<size_t>(offsets.distances.form);
        numberOfSites[form] += offsets.numberOfSites;
        numberOfBytes[form] += offsets.tableSize;
    }
    for (size_t form = 0; form < numberOfForms; ++form) {
        if (numberOfSites[form])
            build().statistics().count(sites[form], numberOfSites[form]);
        if (numberOfBytes[form])
            build().statistics().count(bytes[form], numberOfBytes[form]);
    }
}

std::optional<size_t> SlotAccessEmitter::findOffsetsByShape(const FieldAccess& access, LayoutSetID setOfMembers)
{
    uint64_t keyAndMembers = static_cast<uint64_t>(access.key.bits()) << 32 | setOfMembers.bits();
    auto found = m_offsetsByShapeOfKeyAndMembers.find(keyAndMembers);
    size_t known = found == m_offsetsByShapeOfKeyAndMembers.end() ? notFound : found->value;
    if (known == notFound) {
        const ShapeTable& shapes = m_plan.shapes();
        Vector<DistanceOfShapes, 8> members;
        members.reserveInitialCapacity(access.membersWithField.size());
        for (LayoutID layout : access.membersWithField) {
            ShapeRange range = shapes.shapesOf(layout);
            if (range.isEmpty())
                continue;
            uint32_t offset = slotOfMemberWithField(m_program, m_plan, layout, access.key).offset;
            RELEASE_ASSERT(offset >= access.slot.offset);
            members.append({ range, offset - access.slot.offset });
        }
        LayoutSetID membersLaidIn = setOfMembers;
        if (uint32_t word = build().options().understatedEffects) [[unlikely]] {
            UnderstatedEffects row = UnderstatedEffects::decode(word);
            bool strikesHighest = row.part == PartOfEffects::ShapesOfField;
            if ((strikesHighest || row.part == PartOfEffects::MiddleShapesOfField) && generator().unit().function.index() == row.row && members.size() > 1) {
                std::ranges::sort(members, { }, [](const DistanceOfShapes& member) {
                    return member.shapes.first;
                });
                members.removeAt(strikesHighest ? members.size() - 1 : members.size() / 2);
                membersLaidIn = { };
            }
        }
        if (members.isEmpty())
            return std::nullopt;

        OffsetsByShape offsets { distancesByShape(members.span()) };
        if (!offsets.distances.table.isEmpty()) {
            offsets.constant = m_out.addConstantData(offsets.distances.table.span(), ConstantKind::OffsetTable);
            offsets.tableSize = safeCast<uint32_t>(offsets.distances.table.size());
            offsets.distances.table = { };
        }
        offsets.membersLaidIn = membersLaidIn;
        known = m_offsetsByShape.size();
        m_offsetsByShape.append(WTF::move(offsets));
        m_offsetsByShapeOfKeyAndMembers.add(keyAndMembers, safeCast<uint32_t>(known));
    }
    return known;
}

Word32 SlotAccessEmitter::distanceBehindLowestOffset(CellPointer cell, const FieldAccess& access, LayoutSetID setOfMembers)
{
    std::optional<size_t> known = findOffsetsByShape(access, setOfMembers);
    if (!known)
        return word32(0);
    OffsetsByShape& offsets = m_offsetsByShape[*known];
    ++offsets.numberOfSites;
    bool testInCodeHasAsked = access.layoutsThatTestInCodeLetThrough && offsets.membersLaidIn && types().layoutSets().isSubset(access.layoutsThatTestInCodeLetThrough, offsets.membersLaidIn);
    Word32 shapeWord = emitter<ShapeTestEmitter>().shapeWordOf(cell);
    if (!testInCodeHasAsked && (offsets.distances.form == DistanceForm::FlatTable || offsets.distances.form == DistanceForm::PagedTable)) {
        const OperationContext* context = generator().contextBeingGenerated();
        RELEASE_ASSERT(context);
        emitter<ThrowEmitter>().requireOrCrash(*context, below(add(shapeWord, word32(offsets.distances.lastShape)), offsets.distances.numberOfShapes), std::nullopt, ProofSource::OtherOperation);
    }
    Word32 distance(appendDistanceOfShape(m_out, m_heaps.AOTImage_tables, shapeWord.value(), offsets.distances, offsets.constant));
    if (!testInCodeHasAsked && offsets.distances.form == DistanceForm::FlatTable)
        emitter<ThrowEmitter>().requireOrCrash(*generator().contextBeingGenerated(), notEqual(distance, word32(offsets.distances.distanceOfNoMember())), std::nullopt, ProofSource::OtherOperation);
    return distance;
}

CellPointer SlotAccessEmitter::baseOfSlot(CellPointer cell, const FieldAccess& access)
{
    if (!access.findsOffsetByShape)
        return cell;
    return CellPointer(m_out.add(cell.value(), zeroExtended(distanceBehindLowestOffset(cell, access)).value()));
}

CellPointer SlotAccessEmitter::baseOfSlot(CellPointer cell, const FieldAccess& access, LayoutSetID receivers)
{
    if (!access.findsOffsetByShape)
        return cell;
    return CellPointer(m_out.add(cell.value(), zeroExtended(distanceBehindLowestOffset(cell, access, membersWithFieldAmong(access, receivers))).value()));
}

std::optional<CellPointer> SlotAccessEmitter::baseOfSlotWhereOneTableTellsShapeAndDistance(CellPointer cell, const FieldAccess& access, LayoutSetID receivers, bool rejectsMarkedObjects, LBasicBlock whereShapeIsNoReceiver)
{
    if (!access.findsOffsetByShape)
        return std::nullopt;
    LayoutSetID members = membersWithFieldAmong(access, receivers);
    std::optional<size_t> known = members ? findOffsetsByShape(access, members) : std::nullopt;
    if (!known || m_offsetsByShape[*known].distances.form != DistanceForm::FlatTable || m_offsetsByShape[*known].membersLaidIn != members)
        return std::nullopt;
    OffsetsByShape& offsets = m_offsetsByShape[*known];
    ++offsets.numberOfSites;
    build().statistics().count(StatisticID::ReceiversCheckedByOffsetTables);
    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    Word32 place = rejectsMarkedObjects ? shapes.distanceOfUnmarkedObjectFromShape(cell, members, offsets.distances.lastShape) : add(shapes.shapeWordOf(cell), word32(offsets.distances.lastShape));
    branchUnless(below(place, offsets.distances.numberOfShapes), whereShapeIsNoReceiver);
    LValue placeInTable = zeroExtended(place).value();
    unsigned logBytes = offsets.distances.logBytesPerDistance;
    TypedPointer pointer = m_out.address(m_heaps.AOTImage_tables, m_out.add(m_out.relocatable(SymbolSpace::Constant, offsets.constant), logBytes ? m_out.shl(placeInTable, logBytes) : placeInTable));
    Word32 distance(logBytes ? m_out.load16ZeroExt32(pointer) : m_out.load8ZeroExt32(pointer));
    branchUnless(notEqual(distance, word32(offsets.distances.distanceOfNoMember())), whereShapeIsNoReceiver);
    return CellPointer(m_out.add(cell.value(), zeroExtended(distance).value()));
}

Operand SlotAccessEmitter::loadSlot(CellPointer cell, const FieldAccess& access)
{
    return emitter<SlotEmitter>().loadSlot(cell, access.slot, locationOfSlotAccess(access), extensionWordOfSlotAccess(access));
}

Operand SlotAccessEmitter::loadFilledSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    return emitter<SlotEmitter>().loadSlot(cell, access.slot, locationOfSlotAccess(access), extensionWordOfSlotAccess(access), typeOf(value.id()));
}

Operand SlotAccessEmitter::loadSlotOrUndefined(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    SlotRepresentation representation = access.slot.representation;
    SlotTraits traits = traitsOf(representation);
    if (!traits.hasAbsent)
        return inResultRepresentation(value, loadSlot(cell, access));
    bool checksWhatSlotsHold = sourcesOfProofsCheckedUnder(build().options().testing) & bitOfSourceOfProofs(ProofSource::Field);
    if (representation == SlotRepresentation::OptionalCell32 && value.representation() == Representation::Cell32 && !access.slot.isOutOfLine() && !checksWhatSlotsHold && generator().isReadAgainBehindItsOwnTruthAlone(value)) {
        build().statistics().count(StatisticID::EmptySlotConversionsSkipped);
        return loadSlot(cell, access);
    }
    bool holdsMarkedNumber = representation == SlotRepresentation::OptionalDouble || representation == SlotRepresentation::OptionalInt64;
    bool mayRecheckResults = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckWalkAtRunTime) || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly);
    if (holdsMarkedNumber && value.representation() == Typed::representationOf(representation) && !access.slot.isOutOfLine() && !checksWhatSlotsHold && !mayRecheckResults && generator().isGuardedByUndefinedTest(value)) {
        build().statistics().count(StatisticID::EmptySlotConversionsSkipped);
        return loadSlot(cell, access);
    }
    if (traits.hasUndefined)
        return inResultRepresentation(value, slots.loadSlotOrUndefined(cell, access.slot, locationOfSlotAccess(access), extensionWordOfSlotAccess(access)));

    Operand loaded = loadSlot(cell, access);
    LBasicBlock isFilled = newBlock();
    LBasicBlock isAbsent = newBlock();
    OperandJoin join(m_out, value.representation());
    branch(slots.isAbsent(loaded, representation), { unlikelyEdge(isAbsent), normalEdge(isFilled) });

    m_out.appendTo(isFilled);
    join.add(inResultRepresentation(value, loaded));

    m_out.appendTo(isAbsent);
    join.add(undefinedResult(value));
    return join.finish();
}

Operand SlotAccessEmitter::readIfMemberHasField(const ValueContext& value, CellPointer cell, const FieldAccess& access, NOESCAPE const Invocable<Operand()> auto& read)
{
    LBasicBlock hasField = newBlock();
    LBasicBlock hasNoField = newBlock();
    OperandJoin join(m_out, value.representation());
    emitter<ShapeTestEmitter>().branchOnShapeAmongReceivers(value, cell, setOfMembersWithField(access), { normalEdge(hasField), normalEdge(hasNoField) });

    m_out.appendTo(hasField);
    join.add(read());

    m_out.appendTo(hasNoField);
    join.add(undefinedResult(value));
    return join.finish();
}

static bool everyMemberWithFieldKeepsSlotFilled(const Program& program, const Facts& facts, const Plan& plan, const FieldAccess& access)
{
    return std::ranges::all_of(access.membersWithField, [&](LayoutID member) {
        FieldIndex field = *plan.slots().findField(program, member, access.key);
        const PlacedSlot& slot = plan.slot(member, field);
        const LinkedCell* cell = facts.linked().findCellOfField({ member, field });
        bool takesEveryValue = slot.representation == SlotRepresentation::JSValue && (!slot.type || program.at(slot.type).isAnything());
        return cell && cell->isFilledAtEveryRead && (takesEveryValue || (slot.type && slot.everyStoreFits));
    });
}

Operand SlotAccessEmitter::readSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool absentIsUndefined)
{
    if (value.representation() == Representation::Void)
        return { };
    if (access.coverage == FieldAccess::Coverage::NoMember && absentIsUndefined)
        return undefinedResult(value);
    if (!slotAccessFindsSlot(access))
        return resultWithoutSlot(value);
    if (access.slot.representation == SlotRepresentation::DeferredClosure || access.slot.holdsAccessor)
        return failRead(value, InternalErrorKind::SlotAccessNotPlaced);

    bool mayBeAbsent = absentIsUndefined && typeOf(value.id()).primitives & Primitive::Undefined;
    auto load = [&] {
        return loadSlotOrUndefined(value, baseOfSlot(cell, access), access);
    };
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        break;
    case FieldAccess::Coverage::SomeMembers:
        if (mayBeAbsent && !everyMemberWithFieldKeepsSlotFilled(m_program, m_facts, m_plan, access))
            return readIfMemberHasField(value, cell, access, load);
        if (mayBeAbsent) {
            return readIfMemberHasField(value, cell, access, [&] {
                CellPointer base = baseOfSlot(cell, access);
                if (sourcesOfProofsCheckedUnder(build().options().testing) & bitOfSourceOfProofs(ProofSource::Field))
                    emitter<ThrowEmitter>().requireOrCrash(value, emitter<SlotEmitter>().isFilled(base, access.slot, locationOfSlotAccess(access), extensionWordOfSlotAccess(access)), std::nullopt, ProofSource::Field);
                return inResultRepresentation(value, loadFilledSlot(value, base, access));
            });
        }
        return inResultRepresentation(value, loadFilledSlot(value, baseOfSlot(cell, access), access));
    case FieldAccess::Coverage::EveryMember:
        return mayBeAbsent ? load() : inResultRepresentation(value, loadFilledSlot(value, baseOfSlot(cell, access, value->receivers()), access));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE_INTO_OTHER_FILES void SlotAccessEmitter::askObjectAgainWhatReadAnswered(const ValueContext& value, CellPointer object, Operand read, bool readsEmptySlotAsUndefined)
{
    PropertyKeyID key = value->key();
    bool isInCheckedUnrolledLoop = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly) && generator().isLayingOutTurns();
    if ((!emitter<AccessEmitter>().verifiesFastPathAnswers(key) && !isInCheckedUnrolledLoop) || value.representation() == Representation::Void || m_program.at(key).kind == PropertyKeyKind::PrivateName)
        return;
    const Type& type = typeOf(value.id());
    if (type.primitives & (Primitive::Absent | Primitive::Unspecified))
        return;
    int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::TypedReadByName) | (readsEmptySlotAsUndefined ? emptySlotAnswersNothingByItself : 0));
    callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxCell(object), emitter<ConversionEmitter>().box(value, read, type), identifierNumber(key), word32(kindAndStub) });
}

void SlotAccessEmitter::askObjectWhatStandsInWayOfStore(const ValueContext& value, CellPointer object)
{
    PropertyKeyID key = value->key();
    if (value->isDefinition() || !emitter<AccessEmitter>().verifiesFastPathAnswers(key) || m_program.at(key).kind == PropertyKeyKind::PrivateName)
        return;
    int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::StoreIntoSlot));
    callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxCell(object), undefined(), identifierNumber(key), word32(kindAndStub) });
}

bool SlotAccessEmitter::readsNarrowInteger(const Operation& read) const
{
    if (read.opcode != Opcode::GetSlot || read.representation != Representation::OptionalInt64 || emittersCheckProofs())
        return false;
    FieldAccess access = fieldAccessFor(read.key(), read.receivers(), false);
    return slotAccessFindsSlot(access) && access.slot.representation == SlotRepresentation::OptionalInt32 && !access.slot.isOutOfLine();
}

void SlotAccessEmitter::generateGetSlot(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateSlotRead(value);
        return;
    }
    CellPointer object = in<CellPointer>(value->objectOperand());
    unsigned firstNewOperation = m_out.procedure().numberOfOperations();
    Operand read = readSlot(value, object, fieldAccessOf(value, false), false);
    ValueID onlyUser = m_code.onlyUserOf(value.id());
    define(value, read.representation == Representation::Int64 ? noteIntegerAddedToInSlot(m_out, read, typeOf(value.id()), firstNewOperation, onlyUser && allSums.contains(opcodeOf(onlyUser))) : read);
    askObjectAgainWhatReadAnswered(value, object, read, false);
}

void SlotAccessEmitter::generateReadAboveEmptySlot(const ValueContext& value)
{
    AccessEmitter& accesses = emitter<AccessEmitter>();
    PropertyKeyID key = value->key();
    Boxed object = in<Boxed>(value->objectOperand());
    auto readPrototype = [&] {
        Boxed prototype = loadField<Boxed>(loadStructure(unboxCell(object)), m_heaps.Structure_prototype);
        LBasicBlock isObject = newBlock();
        LBasicBlock isNoObject = newBlock();
        Join<Boxed> join(m_out);
        branch(isCell(prototype), { normalEdge(isObject), rareEdge(isNoObject) });
        m_out.appendTo(isObject);
        join.add(accesses.getById(value, prototype, key));
        m_out.appendTo(isNoObject);
        join.add(accesses.getById(value, object, key));
        return join.finish();
    };
    auto looksAbove = [&] {
        Boxed found = generator().isInLoop(value.block()) && value.representation() != Representation::Void ? readPrototype() : accesses.getById(value, object, key);
        if (value.representation() == Representation::Void)
            return Operand();
        TypeID primitives = types().withoutObjects(typeIDOf(value.id()));
        if (!isNeverType(primitives))
            emitter<ThrowEmitter>().requireOrCrash(value, hasType(found, typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, primitives)), found, ProofSource::Field);
        return inResultRepresentation(value, found);
    };
    FieldAccess access = fieldAccessOf(value, false);
    if (value.representation() != Representation::Void && (!slotAccessFindsSlot(access) || access.coverage != FieldAccess::Coverage::EveryMember)) {
        define(value, resultWithoutSlot(value));
        return;
    }
    if (value.representation() == Representation::Void) {
        define(value, looksAbove());
        return;
    }
    CellPointer cell = unboxCell(object);
    LBasicBlock isEmpty = newBlock();
    OperandJoin join(m_out, value.representation());
    Operand loaded = emitter<SlotEmitter>().loadSlotOrLeaveIfEmpty(baseOfSlot(cell, access, value->receivers()), access.slot, locationOfSlotAccess(access), extensionWordOfSlotAccess(access), rareEdge(isEmpty));
    askObjectAgainWhatReadAnswered(value, cell, loaded, false);
    join.add(inResultRepresentation(value, loaded));

    m_out.appendTo(isEmpty);
    join.add(looksAbove());
    define(value, join.finish());
}

void SlotAccessEmitter::generateGetSlotOrUndefined(const ValueContext& value)
{
    if (value->looksAboveEmptySlot()) {
        generateReadAboveEmptySlot(value);
        return;
    }
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateSlotRead(value);
        return;
    }
    CellPointer object = in<CellPointer>(value->objectOperand());
    Operand read = readSlot(value, object, fieldAccessOf(value, false), true);
    askObjectAgainWhatReadAnswered(value, object, read, true);
    define(value, read);
}

std::optional<CellPointer> SlotAccessEmitter::loadHalfOfAccessorInSlot(const ValueContext& value, ptrdiff_t offsetInGetterSetter)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        define(value, failRead(value, InternalErrorKind::OpcodeWithoutCode));
        return std::nullopt;
    }
    if (value.representation() == Representation::Void) {
        define(value, { });
        return std::nullopt;
    }
    FieldAccess access = fieldAccessOf(value, false);
    if (!slotAccessFindsSlot(access)) {
        define(value, resultWithoutSlot(value));
        return std::nullopt;
    }
    MemoryLocation location = locationOfSlotAccess(access);
    CellPointer object = in<CellPointer>(value->objectOperand());
    Type anyObject = types().at(TypeTable::object);
    Operand held = emitter<SlotEmitter>().loadSlot(baseOfSlot(object, access), access.slot, location, extensionWordOfSlotAccess(access), anyObject);
    CellPointer accessor = held.is<CellPointer>() ? held.as<CellPointer>()
        : held.is<NarrowReference>() ? emitter<ConversionEmitter>().decode(held.as<NarrowReference>(), anyObject, Representation::Cell).as<CellPointer>()
        : unboxCell(held.as<Boxed>());
    return CellPointer(m_out.loadPtr(m_out.address(m_heaps.at(m_program, location), accessor.value(), offsetInGetterSetter)));
}

void SlotAccessEmitter::generateGetSlotGetter(const ValueContext& value)
{
    if (std::optional<CellPointer> getter = loadHalfOfAccessorInSlot(value, GetterSetter::offsetOfGetter()))
        define(value, inResultRepresentation(value, *getter));
}

void SlotAccessEmitter::generateGetSlotSetter(const ValueContext& value)
{
    std::optional<CellPointer> setter = loadHalfOfAccessorInSlot(value, GetterSetter::offsetOfSetter());
    if (!setter)
        return;
    if (value->isInStrictCode()) {
        CellPointer thrower(loadField<Pointer>(loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)), m_heaps.JSGlobalObject_nullSetterStrictFunction).value());
        setter = select(hasCellType(*setter, NullSetterFunctionType), thrower, *setter);
    }
    define(value, inResultRepresentation(value, *setter));
}

void SlotAccessEmitter::readDeferringSlot(const ValueContext& value, bool readsEmptySlotAsUndefined)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateSlotRead(value);
        return;
    }
    FieldAccess access = fieldAccessOf(value, false);
    CellPointer cell = in<CellPointer>(value->objectOperand());
    if (access.slot.representation != SlotRepresentation::DeferredClosure) {
        define(value, readSlot(value, cell, access, readsEmptySlotAsUndefined));
        return;
    }
    if (!slotAccessFindsSlot(access)) {
        define(value, resultWithoutSlot(value));
        return;
    }
    bool looksOutsideSlot = readsEmptySlotAsUndefined && mayHaveLeftDeferringSlot(value, access);
    auto load = [&] {
        return looksOutsideSlot ? loadDeferringSlotOrFieldOutsideIt(value, cell, access) : loadDeferringSlot(value, cell, access, readsEmptySlotAsUndefined);
    };
    bool testsForField = readsEmptySlotAsUndefined && access.coverage == FieldAccess::Coverage::SomeMembers;
    define(value, testsForField ? readIfMemberHasField(value, cell, access, load) : load());
}

CellPointer SlotAccessEmitter::makeDeferredClosure(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    static constexpr unsigned maxFunctionsDeferredAtOneRead = 8;
    Vector<ConstantEmitter::DeferredFunction, 4> functions;
    bool capturesObject = false;
    std::optional<uint32_t> offsetOfEnvironment;
    bool findsSlotAtOneOffset = !access.findsOffsetByShape && !access.slot.isOutOfLine();
    bool areAlike = findsSlotAtOneOffset && std::ranges::all_of(access.membersWithField, [&](LayoutID member) {
        const PlacedSlot& slot = slotOfMemberWithField(m_program, m_plan, member, access.key);
        std::optional<uint32_t> declared = m_program.at(member).findField(access.key);
        if (!slot.deferredFunction || (declared && !m_program.at(member).fields[*declared].form.isEmpty()))
            return false;
        if (slot.deferredClosureCapturesObject && slot.deferredClosureCapturesEnvironment)
            return false;
        if (!functions.isEmpty() && (capturesObject != slot.deferredClosureCapturesObject || !!offsetOfEnvironment != slot.deferredClosureCapturesEnvironment))
            return false;
        capturesObject = slot.deferredClosureCapturesObject;
        if (slot.deferredClosureCapturesEnvironment) {
            std::optional<FieldIndex> environmentField = m_plan.layout(member).environmentOfDeferredClosures;
            if (!environmentField)
                return false;
            const PlacedSlot& environment = m_plan.slot(member, *environmentField);
            if (environment.storage != SlotStorage::Inline || environment.representation != SlotRepresentation::Cell32 || offsetOfEnvironment.value_or(environment.offset) != environment.offset)
                return false;
            offsetOfEnvironment = environment.offset;
        }
        size_t known = functions.findIf([&](const ConstantEmitter::DeferredFunction& deferred) {
            return deferred.function == slot.deferredFunction;
        });
        if (known == notFound) {
            known = functions.size();
            functions.append({ slot.deferredFunction, { } });
        }
        functions[known].layouts.append(member);
        return functions.size() <= maxFunctionsDeferredAtOneRead;
    });
    auto makeInRunTime = [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::MaterializeDeferredClosure, { cell, identifierNumber(access.key) }, { .isOnRarePath = true });
    };
    if (areAlike && !functions.isEmpty())
        return emitter<ConstantEmitter>().makeClosureOfField(value, functions.span(), cell, access.slot.offset, capturesObject, offsetOfEnvironment, makeInRunTime);
    return makeInRunTime();
}

Operand SlotAccessEmitter::loadDeferringSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool readsEmptySlotAsUndefined)
{
    static_assert(uncreatedDeferredClosure < absentDeferredClosure && absentDeferredClosure < nullCell32 && absentDeferredClosure < undefinedCell32);
    NarrowReference word(m_out.castToInt32(loadSlot(baseOfSlot(cell, access), access).as<Pointer>().value()));
    NarrowReference held = either<NarrowReference>(above(word, NarrowReference(m_out.constInt32(absentDeferredClosure))), [&] {
        return word;
    }, [&] {
        auto makeClosure = [&] {
            return compress(makeDeferredClosure(value, cell, access));
        };
        if (!readsEmptySlotAsUndefined)
            return makeClosure();
        return either<NarrowReference>(isZero(Word32(word.value())), makeClosure, [&] {
            return NarrowReference(m_out.constInt32(undefinedCell32));
        });
    });
    return decodeWordOfDeferringSlot(value, held);
}

bool SlotAccessEmitter::mayHaveLeftDeferringSlot(const ValueContext& value, const FieldAccess& access) const
{
    if (value.representation() != Representation::JSValue)
        return false;
    return std::ranges::any_of(access.membersWithField, [&](LayoutID member) {
        std::optional<FieldIndex> field = m_plan.slots().findField(m_program, member, access.key);
        return !field || !m_plan.slot(member, *field).everyStoreFits || !m_facts.field({ member, *field }).stability.contains(FieldStability::IsNeverRedefined);
    });
}

Operand SlotAccessEmitter::loadDeferringSlotOrFieldOutsideIt(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    NarrowReference word(m_out.castToInt32(loadSlot(baseOfSlot(cell, access), access).as<Pointer>().value()));
    Boxed result = either<Boxed>(above(word, NarrowReference(m_out.constInt32(absentDeferredClosure))), [&] {
        return decodeWordOfDeferringSlot(value, word).as<Boxed>();
    }, [&] {
        return either<Boxed>(isZero(Word32(word.value())), [&] {
            return decodeWordOfDeferringSlot(value, compress(makeDeferredClosure(value, cell, access))).as<Boxed>();
        }, [&] {
            return either<Boxed>(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordHasFieldOutsideSlot), [&] {
                return undefined();
            }, [&] {
                return callOperation<Boxed>(value, RuntimeOperation::GetFieldOutsideSlot, { cell, identifierNumber(access.key) }, { .isOnRarePath = true });
            });
        });
    });
    return result;
}

Operand SlotAccessEmitter::decodeWordOfDeferringSlot(const ValueContext& value, NarrowReference held)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (value.representation()) {
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
        return conversions.decode(held, typeOf(value.id()), value.representation());
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::RawWord:
        return inResultRepresentation(value, conversions.decode(held, typeOf(value.id()), Representation::JSValue));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotAccessEmitter::writeSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, ValueID stored)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    MemoryLocation location = locationOfSlotAccess(access);
    const PlacedSlot* extensionWord = extensionWordOfSlotAccess(access);
    CellPointer base = baseOfSlot(cell, access);
    bool defersClosure = access.slot.representation == SlotRepresentation::DeferredClosure;
    if (defersClosure && isDeferredClosure(stored)) {
        LValue uncreated = m_out.constInt32(uncreatedDeferredClosure);
        m_out.store32(uncreated, slots.addressOf(base, access.slot, location, extensionWord));
        return;
    }
    Operand operand = get(stored);
    Type storedType = typeOf(stored);
    if (defersClosure)
        m_out.store32(operand.as<NarrowReference>().value(), slots.addressOf(base, access.slot, location, extensionWord));
    else
        slots.storeSlot(base, access.slot, location, operand, extensionWord, storedType);
    barrierAfterStore(value, cell, storedType);
}

void SlotAccessEmitter::markCellIfMarkerReplacesRawValue(CellPointer cell, const FieldAccess& access, ValueID stored)
{
    if (!typeOf(stored).mayBeUndefinedOrNull())
        return;
    SlotEmitter& slots = emitter<SlotEmitter>();
    LayoutSetID members = setOfMembersWithField(access);
    if (slots.holdsRawValueFromBirthInSomeMember(members, access.key))
        slots.markCellIfMarkerIsStored(cell, stored, members);
}

void SlotAccessEmitter::generatePutSlot(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateSlotStore(value);
        return;
    }
    FieldAccess access = fieldAccessOf(value, true);
    if (!slotAccessFindsSlotInEveryReceiver(access)) {
        resultWithoutSlot(value);
        m_generator.noteThatOwnerIsRememberedBehind(value.id());
        return;
    }
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    askObjectWhatStandsInWayOfStore(value, cell);
    markCellIfMarkerReplacesRawValue(cell, access, stored);
    if (access.slot.isOutOfLine() && access.mayBeFilledWithoutStorage) {
        SlotEmitter& slots = emitter<SlotEmitter>();
        if (!needsStorageFor(access.slot, typeOf(stored))) {
            slots.storeIfExtensionHolds(cell, access.slot, locationOfSlotAccess(access), get(stored), access.extensionWord);
            return;
        }
        slots.ensureExtensionHoldsWithoutCollecting(value, cell, access.extensionWord, granuleBitOf(access.slot.offset), granuleBitsWithFieldsBehind(access));
    }
    writeSlot(value, cell, access, stored);
}

static bool everyMemberKeepsFieldAtSlotKey(const Program& program, const Plan& plan, const FieldAccess& access)
{
    static_assert(slotKeyFor(8, false) == 8);
    return std::ranges::all_of(access.membersWithField, [&](LayoutID member) {
        return slotKeyFor(slotOfMemberWithField(program, plan, member, access.key).offset, false) != noSlotKey;
    });
}

bool SlotAccessEmitter::tryPutSlotOrAddByStub(const ValueContext& value, CellPointer cell, const FieldAccess& access, ValueID stored)
{
    return tryPutSlotOrAddByStub(value, cell, access, get(stored));
}

bool SlotAccessEmitter::tryPutSlotOrAddByStub(const ValueContext& value, CellPointer cell, const FieldAccess& access, Operand stored)
{
    const PlacedSlot& slot = access.slot;
    SlotRepresentation representation = slot.representation;
    Operand operand = stored;
    Representation formOfStoredValue = representation == SlotRepresentation::DeferredClosure ? Representation::Cell32 : Typed::representationOf(representation);
    if (!mayBeCreatedByStore(representation) || operand.representation != formOfStoredValue)
        return false;
    if (representation == SlotRepresentation::Int32)
        operand = Operand(Representation::OptionalInt64, m_out.zeroExtPtr(operand.value));
    uint32_t slotKey = slotKeyFor(slot.offset, slot.isOutOfLine());
    if (slotKey == noSlotKey)
        return false;
    if (access.findsOffsetByShape) {
        if (slot.isOutOfLine() || !everyMemberKeepsFieldAtSlotKey(m_program, m_plan, access))
            return false;
        LValue keyOfThisObject = m_out.add(m_out.constInt64(slotKey), zeroExtended(distanceBehindLowestOffset(cell, access)).value());
        callStub(value, AOT::putSlotOrAddAtKeyInRegister(representation), { cell.value(), operand.value, keyOfThisObject }, { .accessesCollectorState = true });
        m_generator.noteThatOwnerIsRememberedBehind(value.id());
        return true;
    }
    if (slot.isOutOfLine() && !representsAbsentAsZero(representation))
        return false;
    PlaceOfCreatedSlot place = slot.isOutOfLine() ? PlaceOfCreatedSlot::OutOfLine : PlaceOfCreatedSlot::InCell;
    callStub(value, AOT::putSlotOrAdd(place, representation), { cell.value(), operand.value }, { .immediate = slotKey, .accessesCollectorState = true });
    m_generator.noteThatOwnerIsRememberedBehind(value.id());
    return true;
}

bool SlotAccessEmitter::storesIntoLateField(const ValueContext& value, const FieldAccess& access) const
{
    SlotRepresentation representation = access.slot.representation;
    bool isTakenByStub = representation != SlotRepresentation::DeferredClosure && representation != SlotRepresentation::Int32 && mayBeCreatedByStore(representation) && representsAbsentAsZero(representation);
    return access.slot.isOutOfLine() && isTakenByStub && !access.findsOffsetByShape && !value->isExpectedToCreate() && mayCreateListedField(access) && slotKeyFor(access.slot.offset, true) != noSlotKey;
}

void SlotAccessEmitter::storeIntoLateField(const ValueContext& value, CellPointer cell, const FieldAccess& access, const MemoryLocation& location, Operand operand, const Type& storedType, NOESCAPE const ScopedLambda<void()>& askBeforeCreation)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    LBasicBlock lacksGranule = newBlock();
    LBasicBlock creates = newBlock();
    LBasicBlock done = newBlock();
    ScopeFork fork = slots.storeIntoFilledSlot(cell, access.slot, location, operand, storedType, access.extensionWord, rareEdge(lacksGranule), rareEdge(creates));
    barrierAfterStore(value, cell, storedType);
    m_out.jump(done);

    m_out.appendTo(lacksGranule);
    slots.reserveGranules(value, cell, access.extensionWord, granuleBitsWithLaterStores(value, access), emitter<BarrierEmitter>().barrierOfExtension(value), true);
    m_out.jump(creates);

    m_out.appendTo(creates);
    askBeforeCreation();
    callStub(value, AOT::putSlotOrAdd(PlaceOfCreatedSlot::OutOfLine, access.slot.representation), { cell.value(), operand.value }, { .immediate = slotKeyFor(access.slot.offset, true), .isOnRarePath = true, .accessesCollectorState = true });
    m_out.jump(done);

    m_out.appendToJoin(done, fork);
}

bool SlotAccessEmitter::nextStoreWritesOrderWordAtOnce(const ValueContext& value) const
{
    std::span<const ValueID> behind = value.operationsBehind();
    if (behind.empty())
        return false;
    const Operation& next = operationAt(behind.front());
    if (next.opcode != Opcode::PutSlotOrAdd || next.objectOperand() != value->objectOperand() || !next.orderWordAfter() || next.level() == KnowledgeLevel::Expected)
        return false;
    FieldAccess access = fieldAccessFor(next.key(), next.receivers(), true);
    return slotAccessFindsSlotInEveryReceiver(access) && mayCreateListedField(access) && (!access.slot.isOutOfLine() || !needsStorageFor(access.slot, typeOf(next.valueOperand())));
}

bool SlotAccessEmitter::mayBeFirstStore(const Operation& store) const
{
    if (store.opcode != Opcode::PutSlotOrAdd || !store.orderWordAfter() || !store.isDefinition() || store.level() != KnowledgeLevel::Proven || m_generator.isDeferredClosure(store.valueOperand()))
        return false;
    FieldAccess access = fieldAccessFor(store.key(), store.receivers(), true);
    return slotAccessFindsSlotInEveryReceiver(access) && mayCreateListedField(access) && access.slot.storage == SlotStorage::Inline && !access.findsOffsetByShape && access.slot.representation != SlotRepresentation::DeferredClosure;
}

void SlotAccessEmitter::generatePutSlotOrAdd(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateSlotStore(value);
        return;
    }
    FieldAccess access = fieldAccessOf(value, true);
    if (!slotAccessFindsSlotInEveryReceiver(access)) {
        resultWithoutSlot(value);
        m_generator.noteThatOwnerIsRememberedBehind(value.id());
        return;
    }

    SlotEmitter& slots = emitter<SlotEmitter>();
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    MemoryLocation location = locationOfSlotAccess(access);
    bool needsStorage = !access.slot.isOutOfLine() || !access.slot.mayExistWithoutStorage || needsStorageFor(access.slot, typeOf(stored));
    askObjectWhatStandsInWayOfStore(value, cell);
    markCellIfMarkerReplacesRawValue(cell, access, stored);
    BlockFrequency frequency = generator().code().at(value.block()).frequency;
    Place coldPlace = frequency == BlockFrequency::Rare ? Place::RareBlock : frequency == BlockFrequency::Unlikely ? Place::UnlikelyBlock : Place::Anywhere;
    StateOfKnowledge slotState = access.slot.isOutOfLine() ? StateOfKnowledge::FieldInExtension : StateOfKnowledge::Always;
    bool isWrittenInLine = coldPlace == Place::Anywhere && generator().takesOpenForm(value, slotState);
    if (coldPlace != Place::Anywhere)
        m_out.noteForm(Form::Closed, slotState, coldPlace);
    if (!isWrittenInLine && needsStorage && mayCreateListedField(access) && !value->orderWordAfter() && !isDeferredClosure(stored) && tryPutSlotOrAddByStub(value, cell, access, stored))
        return;
    if (needsStorage && isWrittenInLine && !value->orderWordAfter() && !isDeferredClosure(stored) && storesIntoLateField(value, access) && get(stored).representation == Typed::representationOf(access.slot.representation)) {
        storeIntoLateField(value, cell, access, location, get(stored), typeOf(stored), scopedLambda<void()>([] { }));
        return;
    }
    if (access.slot.isOutOfLine() && needsStorage)
        slots.ensureExtensionHolds(value, cell, access.extensionWord, granuleBitOf(access.slot.offset), granuleBitsWithLaterStores(value, access), emitter<BarrierEmitter>().barrierOfExtension(value));

    auto store = [&] {
        if (needsStorage)
            writeSlot(value, cell, access, stored);
        else
            slots.storeIfExtensionHolds(baseOfSlot(cell, access), access.slot, location, get(stored), access.extensionWord);
    };
    if (!mayCreateListedField(access)) {
        store();
        return;
    }

    if (std::optional<OrderWord> orderWordAfter = value->orderWordAfter()) {
        if (!generator().laterStoreWritesOrderWord(value.id()) && !nextStoreWritesOrderWordAtOnce(value))
            storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(orderWordAfter->value())));
        if (needsStorage || !access.slot.isLeftOutByMakers)
            store();
        else if (emittersCheckProofs())
            emitter<ThrowEmitter>().requireOrCrash(value, logicalNot(slots.holdsValue(baseOfSlot(cell, access), access.slot, location, extensionWordOfSlotAccess(access))), std::nullopt, ProofSource::RememberedFact);
        if (ValueID allocation = generator().takeAllocationFilledBy(value.id()))
            emitter<BarrierEmitter>().fenceAfterFirstStores(allocation);
        return;
    }

    LayoutSetID membersWithField = setOfMembersWithField(access);
    auto listFieldIfMissing = [&] {
        LBasicBlock creates = newBlock();
        LBasicBlock done = newBlock();
        CellPointer base = baseOfSlot(cell, access);
        Condition isMissing = needsStorage
            ? slots.holdsAbsent(slots.addressOf(base, access.slot, location, extensionWordOfSlotAccess(access)), access.slot.representation)
            : logicalNot(slots.holdsValue(base, access.slot, location, extensionWordOfSlotAccess(access)));
        BlockEdge toCreation = edgeToCreation(creates, value->isExpectedToCreate());
        branch(isMissing, { toCreation, normalEdge(done) });
        m_out.appendTo(creates);
        didCreateField(value, cell, access, toCreation.isRare());
        m_out.jump(done);
        m_out.appendTo(done);
    };

    listCreatedFields(cell, membersWithField, singleElementSpan(access.key), value->isExpectedToCreate(), listFieldIfMissing);
    store();
}

std::optional<FieldAccess> SlotAccessEmitter::fieldCreatedByDefinition(const Operation& definition) const
{
    auto typeOfPart = [&](ValueID part) {
        return part ? typeIDOf(part) : TypeID();
    };
    AccessContext context { m_program, WalkFacts(m_facts), m_plan, m_generator.inputs().types, m_id };
    return fieldCreatedInSlotBy(context, {
        typeIDOf(definition.objectOperand()), typeIDOf(definition.computedKeyOperand()),
        typeOfPart(definition.valueOperand()), typeOfPart(definition.getterOperand()), typeOfPart(definition.setterOperand()), definition.descriptorFlags()
    });
}

CellPointer SlotAccessEmitter::newGetterSetter(const ValueContext& context, ValueID getterID, ValueID setterID)
{
    auto callRunTime = [&] {
        CellPointer none(m_out.intPtrZero);
        return callOperation<CellPointer>(context, RuntimeOperation::NewGetterSetter, { getterID ? in<CellPointer>(getterID) : none, setterID ? in<CellPointer>(setterID) : none });
    };
    if (!generator().paysForCodeInLine(context.block()))
        return callRunTime();

    const Machine::AbstractHeap& allocatorState = m_heaps.entire(AbstractHeap::AllocatorState);
    const Machine::AbstractHeap& fields = m_heaps.entire(AbstractHeap::ObjectFields);
    LBasicBlock hasNoCell = newBlock();
    Join<CellPointer> join(m_out);
    CellPointer getter = getterID ? in<CellPointer>(getterID) : CellPointer(m_out.loadPtr(instanceField(allocatorState, AOT_INSTANCE_OFFSET(allocation.getterOfAccessorWithoutGetter))));
    CellPointer setter = setterID ? in<CellPointer>(setterID) : CellPointer(m_out.loadPtr(instanceField(allocatorState, AOT_INSTANCE_OFFSET(allocation.setterOfAccessorWithoutSetter))));
    {
        CodeGenerator::InitializationScope scope(generator());
        CellPointer pair = emitter<CellAllocationEmitter>().allocateCellWithHeader(InlineAllocation::GetterSetter, hasNoCell);
        m_out.storePtr(getter.value(), m_out.address(fields, pair.value(), GetterSetter::offsetOfGetter()));
        m_out.storePtr(setter.value(), m_out.address(fields, pair.value(), GetterSetter::offsetOfSetter()));
        emitter<BarrierEmitter>().fenceAfterInitialization();
        join.add(pair);
    }
    m_out.appendTo(hasNoCell);
    join.add(callRunTime());
    return join.finish();
}

SlotAccessEmitter::CreationByDefinition SlotAccessEmitter::tryCreateFieldByDefinition(const ValueContext& definition, CellPointer cell)
{
    std::optional<FieldAccess> access = fieldCreatedByDefinition(definition.value());
    std::optional<OrderWord> orderWordAfter = definition->orderWordAfter();
    if (!access) {
        if (orderWordAfter)
            fail(definition, InternalErrorKind::SlotAccessNotPlaced);
        return { };
    }
    PropertyKeyID key = access->key;
    ValueID stored = definition->valueOperand();
    LBasicBlock leftToRunTime = nullptr;

    SlotEmitter& slots = emitter<SlotEmitter>();
    MemoryLocation location = locationOfSlotAccess(*access);
    auto goOnIf = [&](Condition condition) {
        if (!leftToRunTime)
            leftToRunTime = newBlock();
        LBasicBlock next = newBlock();
        branch(condition, { normalEdge(next), unlikelyEdge(leftToRunTime) });
        m_out.appendTo(next);
    };

    bool listsKey = mayCreateListedField(*access);
    std::optional<KeyOrderStep> step;
    auto hasNoFlags = [&] {
        return hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), ~orderAndCountMask);
    };
    if (!orderWordAfter) {
        if (listsKey)
            step = testLikeliestKeyOrderStep(cell, setOfMembersWithField(*access), singleElementSpan(key));
        goOnIf(hasNoFlags());
        if (listsKey)
            goOnIf(notEqual(bitAnd(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordOrderMask), orderStoredOutOfLine));
    }
    auto isNoPrototype = [&] {
        return hasNoBits(loadField<Word32>(loadStructure(cell), m_heaps.Structure_bitField), Structure::s_mayBePrototypeBits);
    };
    if (!orderWordAfter)
        goOnIf(isNoPrototype());
    if (access->slot.isOutOfLine())
        slots.ensureExtensionHolds(definition, cell, access->extensionWord, granuleBitOf(access->slot.offset), granuleBitsWithFieldsBehind(*access), OwnerBarrier::Emitted);
    auto lacksField = [&] {
        return slots.holdsAbsent(slots.addressOf(cell, access->slot, location, extensionWordOfSlotAccess(*access)), access->slot.representation);
    };
    if (!orderWordAfter)
        goOnIf(lacksField());

    Type storedType = stored ? typeOf(stored) : types().at(TypeTable::object);
    Operand operand = stored ? get(stored) : Operand(newGetterSetter(definition, definition->getterOperand(), definition->setterOperand()));
    operand = emitter<ConversionEmitter>().inRepresentation(definition, operand, storedType, Typed::representationOf(access->slot.representation));
    if (orderWordAfter)
        storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(orderWordAfter->value())));
    else if (listsKey) {
        takeKeyOrderStepOr(cell, step, [&] {
            didCreateField(definition, cell, *access);
        });
    }
    slots.storeSlot(cell, access->slot, location, operand, extensionWordOfSlotAccess(*access), storedType);
    if (storedType.hasCells())
        emitter<BarrierEmitter>().barrier(definition, cell);
    return { true, leftToRunTime };
}

bool SlotAccessEmitter::mayCreateListedField(const FieldAccess& access) const
{
    return m_plan.slots().storeMayListKey(access);
}

static std::optional<KeyOrderContinuation> firstContinuationCreatingSlots(const Program& program, const Plan& plan, LayoutID member, std::span<const PropertyKeyID> createdKeys)
{
    Vector<FieldIndex, 4> fields;
    for (PropertyKeyID key : createdKeys) {
        std::optional<FieldIndex> field = plan.slots().findField(program, member, key);
        if (!field || !plan.slot(member, *field).hasStorage())
            return std::nullopt;
        fields.append(*field);
    }
    return plan.keyOrders().firstContinuationCreating(member, fields.span());
}

uint64_t SlotAccessEmitter::granuleBitsWithFieldsBehind(const FieldAccess& access) const
{
    RELEASE_ASSERT(access.slot.isOutOfLine());
    const KeyOrders& orders = m_plan.keyOrders();
    std::optional<uint64_t> bitsBehind;
    for (LayoutID member : access.membersWithField) {
        std::optional<KeyOrderContinuation> continuation = firstContinuationCreatingSlots(m_program, m_plan, member, singleElementSpan(access.key));
        const PlannedLayout& layout = m_plan.layout(member);
        bool hasExtensionWordOfAccess = layout.slots[layout.extensionWordIndex()].hasLocationOf(access.extensionWord);
        uint64_t bitsOfMember = hasExtensionWordOfAccess ? layout.granulesOfFirstExtension : 0;
        if (continuation && hasExtensionWordOfAccess) {
            uint32_t orderWordAfter = continuation->orderWordAfter;
            std::span<const FieldIndex> order = orders.order(member, orderOf(orderWordAfter));
            std::span<const KeyOrderSource> sources = orders.sourcesOfFields(member, orderOf(orderWordAfter));
            for (size_t index = countOf(orderWordAfter); index < order.size() && sources[index] != KeyOrderSource::Declaration; ++index) {
                const PlacedSlot& later = m_plan.slot(member, order[index]);
                if (later.isOutOfLine())
                    bitsOfMember |= granuleBitOf(later.offset);
            }
        }
        bitsBehind = bitsBehind.value_or(bitsOfMember) & bitsOfMember;
        if (!*bitsBehind)
            break;
    }
    return granuleBitOf(access.slot.offset) | bitsBehind.value_or(0);
}

uint64_t SlotAccessEmitter::granuleBitsWithLaterStores(const ValueContext& value, const FieldAccess& access) const
{
    uint64_t stored = granuleBitOf(access.slot.offset);
    for (ValueID id : value.operationsBehind()) {
        const Operation& next = operationAt(id);
        if (next.traits().mayCallUnknownCode())
            break;
        if (next.opcode != Opcode::PutSlotOrAdd)
            continue;
        if (next.objectOperand() != value->objectOperand() || next.receivers() != value->receivers())
            break;
        FieldAccess later = fieldAccessFor(next.key(), next.receivers(), true);
        if (!slotAccessFindsSlotInEveryReceiver(later) || later.findsOffsetByShape)
            break;
        if (!later.slot.isOutOfLine() || (later.slot.mayExistWithoutStorage && !needsStorageFor(later.slot, typeOf(next.valueOperand()))))
            continue;
        uint64_t granuleBit = granuleBitOf(later.slot.offset);
        if (stored & granuleBit)
            break;
        stored |= granuleBit;
    }
    return granuleBitsWithFieldsBehind(access) | stored;
}

static std::optional<KeyOrderContinuation> findSlotContinuationOfEveryMember(const Program& program, const Plan& plan, std::span<const LayoutID> members, std::span<const PropertyKeyID> createdKeys)
{
    std::optional<KeyOrderContinuation> common;
    for (LayoutID member : members) {
        if (plan.shapes().shapesOf(member).count != 1)
            return std::nullopt;
        std::optional<KeyOrderContinuation> continuation = firstContinuationCreatingSlots(program, plan, member, createdKeys);
        if (!continuation || (common && *common != *continuation))
            return std::nullopt;
        common = continuation;
    }
    return common;
}

std::optional<KeyOrderStep> SlotAccessEmitter::testLikeliestKeyOrderStep(CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys)
{
    const ShapeTable& shapes = m_plan.shapes();
    std::span<const LayoutID> members = membersOf(types(), receivers);
    if (std::optional<KeyOrderContinuation> common = findSlotContinuationOfEveryMember(m_program, m_plan, members, createdKeys))
        return KeyOrderStep { equal(emitter<ShapeTestEmitter>().orderWordOf(cell), static_cast<int32_t>(common->orderWordBefore)), word32(static_cast<int32_t>(common->orderWordAfter)) };
    static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && !(offsetOfOrderWord % sizeof(uint64_t)) && std::endian::native == std::endian::little);
    auto isOfProgram = [&](LayoutID member) {
        FunctionID constructor = m_program.at(member).constructor;
        return constructor && !isLibraryCode(m_program, m_program.at(constructor));
    };
    Vector<LayoutID, 8> likeliestFirst;
    likeliestFirst.append(members);
    if (isLibraryCode(m_program, m_program.at(m_unit.function)))
        std::ranges::stable_partition(likeliestFirst, isOfProgram);
    for (LayoutID member : likeliestFirst) {
        std::optional<KeyOrderContinuation> continuation = firstContinuationCreatingSlots(m_program, m_plan, member, createdKeys);
        if (!continuation)
            continue;
        uint64_t wordsBefore = static_cast<uint64_t>(AOT::shapeWordOf(shapes.generalShapeOf(member))) << 32 | continuation->orderWordBefore;
        Integer words(m_out.load64(m_out.address(m_heaps.AOTObject_orderWord, cell.value(), offsetOfOrderWord)));
        if (m_plan.layout(member).mayBearMark)
            words = bitOr(words, integer(static_cast<int64_t>(orderAndShapeWordsAreUnmarked)));
        return KeyOrderStep { equal(words, integer(static_cast<int64_t>(wordsBefore))), word32(static_cast<int32_t>(continuation->orderWordAfter)) };
    }
    return std::nullopt;
}

std::optional<KeyOrderStep> SlotAccessEmitter::testNextKeyOrderStep(CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys)
{
    std::span<const LayoutID> members = membersOf(types(), receivers);
    bool isCalledInLoops = m_generator.runsRepeatedly(m_generator.blockBeingGenerated()) || m_facts.linked().functionsCalledInLoops.get(m_unit.function.index());
    if (!isCalledInLoops || members.size() != 1 || createdKeys.size() != 1 || m_plan.shapes().shapesOf(members[0]).count != 1)
        return std::nullopt;
    std::optional<FieldIndex> field = m_plan.slots().findField(m_program, members[0], createdKeys.front());
    std::optional<KeyOrderContinuation> next = field && m_plan.slot(members[0], *field).hasStorage() ? m_plan.keyOrders().secondContinuationCreating(members[0], *field) : std::nullopt;
    if (!next)
        return std::nullopt;
    return KeyOrderStep { equal(emitter<ShapeTestEmitter>().orderWordOf(cell), static_cast<int32_t>(next->orderWordBefore)), word32(static_cast<int32_t>(next->orderWordAfter)) };
}

BlockEdge SlotAccessEmitter::edgeToCreation(LBasicBlock creates, bool isExpectedToCreate)
{
    if (isExpectedToCreate)
        return unlikelyEdge(creates);
    return rareEdge(creates);
}

void SlotAccessEmitter::didCreateField(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool isOnRarePath)
{
    const PlacedSlot& slot = access.slot;
    uint32_t slotKey = slotKeyFor(slot.offset, slot.isOutOfLine());
    if (access.findsOffsetByShape) {
        if (!everyMemberKeepsFieldAtSlotKey(m_program, m_plan, access)) {
            callVoidOperation(value, RuntimeOperation::DidCreateField, { cell, identifierNumber(access.key) }, { .isOnRarePath = isOnRarePath });
            return;
        }
        LValue lowestKey = m_out.constInt64(slotKey);
        callStub(value, AOT::didCreateField(), { cell.value(), m_out.add(lowestKey, zeroExtended(distanceBehindLowestOffset(cell, access)).value()) });
        return;
    }
    if (slotKey == noSlotKey) {
        callVoidOperation(value, RuntimeOperation::DidCreateField, { cell, identifierNumber(access.key) }, { .isOnRarePath = isOnRarePath });
        return;
    }
    if (std::optional<uint64_t> wordsBefore = wordsAfterComputedKeys(value, access)) {
        listFieldAfterComputedKeys(value, cell, slotKey, *wordsBefore);
        return;
    }
    callStub(value, AOT::didCreateFieldOfSlot(), { cell.value() }, { .immediate = slotKey });
}

void SlotAccessEmitter::noteComputedKey(ValueID object)
{
    m_numberOfComputedKeys = m_objectWithComputedKeys == object ? m_numberOfComputedKeys + 1 : 1;
    m_objectWithComputedKeys = object;
}

std::optional<uint64_t> SlotAccessEmitter::wordsAfterComputedKeys(const ValueContext& value, const FieldAccess& access)
{
    if (value->opcode != Opcode::PutSlotOrAdd || !m_generator.isInLoop(value.block()) || m_objectWithComputedKeys != value->objectOperand() || operationAt(m_objectWithComputedKeys).opcode != Opcode::NewObject)
        return std::nullopt;
    std::span<const LayoutID> members = membersOf(types(), setOfMembersWithField(access));
    if (members.size() != 1 || m_numberOfComputedKeys >= CompiledStructure::unknownNumberOfExtraKeys)
        return std::nullopt;
    std::optional<KeyOrderContinuation> continuation = firstContinuationCreatingSlots(m_program, m_plan, members[0], singleElementSpan(access.key));
    if (!continuation)
        return std::nullopt;
    return static_cast<uint64_t>(AOT::shapeWordOf(m_plan.shapes().generalShapeOf(members[0]))) << 32 | continuation->orderWordBefore | orderWordHasExtraProperties;
}

void SlotAccessEmitter::listFieldAfterComputedKeys(const ValueContext& value, CellPointer cell, uint32_t slotKey, uint64_t wordsBefore)
{
    LBasicBlock isNotRemembered = newBlock();
    LBasicBlock done = newBlock();
    auto continueIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        branch(condition, { normalEdge(next), unlikelyEdge(isNotRemembered) });
        m_out.appendTo(next);
    };
    uint64_t keys = keysOfRunBesideExtraKeys(slotKey, m_numberOfComputedKeys);
    ptrdiff_t run = static_cast<ptrdiff_t>(indexOfKeyOrderRun(wordsBefore, slotKey) ^ numberOfKeyOrderRuns / 2) << logSizeOfKeyOrderRun;
    auto words = [&](LValue base, const Machine::AbstractHeap& heap, ptrdiff_t offset) {
        return Integer(m_out.load64(m_out.address(heap, base, offset)));
    };
    continueIf(equal(words(cell.value(), m_heaps.AOTObject_orderWord, offsetOfOrderWord), integer(static_cast<int64_t>(wordsBefore))));
    Word32 numberOfExtraKeys(m_out.load16ZeroExt32(m_out.address(loadStructure(cell).value(), m_heaps.AOTCompiledStructure_numberOfExtraKeys)));
    continueIf(equal(numberOfExtraKeys, word32(static_cast<int32_t>(m_numberOfComputedKeys))));
    LValue runs = loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.keyOrderRuns)).value();
    continueIf(equal(words(runs, m_heaps.AOTInstance_keyOrderRuns, run + KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), integer(static_cast<int64_t>(wordsBefore))));
    continueIf(equal(words(runs, m_heaps.AOTInstance_keyOrderRuns, run + KeyOrderRun::offsetOfSlotKeys()), integer(static_cast<int64_t>(keys))));
    storeField(cell, m_heaps.AOTObject_orderWord, Word32(m_out.load32(m_out.address(m_heaps.AOTInstance_keyOrderRuns, runs, run + KeyOrderRun::offsetOfOrderAndCountAfter()))));
    m_out.jump(done);
    m_out.appendTo(isNotRemembered);
    callStub(value, AOT::didCreateFieldOfSlot(), { cell.value() }, { .immediate = slotKey });
    m_out.jump(done);
    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
