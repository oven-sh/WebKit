/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessPlan.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTIRProgram.h"
#include "AOTMemoryLocation.h"
#include "AOTObjectWords.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeArena.h"
#include "GetterSetter.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSType.h"
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

// A store writes without asking whether the object has the slot.
static bool slotAccessFindsSlotInEveryReceiver(const FieldAccess& access)
{
    return slotAccessFindsSlot(access) && access.coverage == FieldAccess::Coverage::EveryMember;
}

// The back end adds to such a word by its low half: Machine::MemoryUpdate::LowHalf.
// A Load with an index below `firstNewOperation` was found, and may stand ahead of the proof: the Load of IsSlotFilled. It is not marked. Where a sum alone reads the word, a reload behind the proof is.
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

static const PlacedSlot& slotOfMemberWithField(const Program& program, const Plan& plan, LayoutID member, PropertyKeyID key)
{
    std::optional<FieldIndex> field = plan.slots().findField(program, member, key);
    RELEASE_ASSERT(field);
    return plan.slot(member, *field);
}

FieldAccess SlotAccessEmitter::fieldAccessOf(const ValueContext& value, bool isStore) const
{
    return fieldAccessFor(value->key(), value->receivers(), isStore);
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

// `if (this.cause) this.cause.code`, where the program makes no object of the class that `cause` is written as: the slot of `cause` proves the class, and is never filled.
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
    case Representation::LiteralString:
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

Word32 SlotAccessEmitter::distanceBehindLowestOffset(CellPointer cell, const FieldAccess& access)
{
    LayoutSetID setOfMembers = setOfMembersWithField(access);
    uint64_t keyAndMembers = static_cast<uint64_t>(access.key.bits()) << 32 | setOfMembers.bits();
    auto found = m_offsetsByShapeOfKeyAndMembers.find(keyAndMembers);
    size_t known = found == m_offsetsByShapeOfKeyAndMembers.end() ? notFound : found->value;
    if (known == notFound) {
        const ShapeTable& shapes = m_plan.shapes();
        Vector<DistanceOfShapes, 8> members;
        for (LayoutID layout : access.membersWithField) {
            ShapeRange range = shapes.shapesOf(layout);
            if (range.isEmpty())
                continue;
            uint32_t offset = slotOfMemberWithField(m_program, m_plan, layout, access.key).offset;
            RELEASE_ASSERT(offset >= access.slot.offset);
            members.append({ range, offset - access.slot.offset });
        }
        if (members.isEmpty())
            return word32(0);

        OffsetsByShape offsets { distancesByShape(members.span()) };
        if (!offsets.distances.table.isEmpty()) {
            offsets.constant = m_out.addConstantData(offsets.distances.table.span(), ConstantKind::OffsetTable);
            offsets.tableSize = safeCast<uint32_t>(offsets.distances.table.size());
            offsets.distances.table = { };
        }
        known = m_offsetsByShape.size();
        m_offsetsByShape.append(WTF::move(offsets));
        m_offsetsByShapeOfKeyAndMembers.add(keyAndMembers, safeCast<uint32_t>(known));
    }

    OffsetsByShape& offsets = m_offsetsByShape[known];
    ++offsets.numberOfSites;
    Word32 shapeWord = emitter<ShapeTestEmitter>().shapeWordOf(cell);
    return Word32(appendDistanceOfShape(m_out, m_heaps.AOTImage_tables, shapeWord.value(), offsets.distances, offsets.constant));
}

CellPointer SlotAccessEmitter::baseOfSlot(CellPointer cell, const FieldAccess& access)
{
    if (!access.findsOffsetByShape)
        return cell;
    return CellPointer(m_out.add(cell.value(), zeroExtended(distanceBehindLowestOffset(cell, access)).value()));
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

Operand SlotAccessEmitter::readSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool absentIsUndefined)
{
    if (value.representation() == Representation::Void)
        return { };
    if (access.coverage == FieldAccess::Coverage::NoMember && absentIsUndefined)
        return undefinedResult(value);
    if (!slotAccessFindsSlot(access))
        return resultWithoutSlot(value);
    if (access.slot.representation == SlotRepresentation::DeferredClosure)
        return failRead(value, InternalErrorKind::SlotAccessNotPlaced);

    // A result that is never undefined comes from a slot that is never absent.
    bool mayBeAbsent = absentIsUndefined && typeOf(value.id()).primitives & Primitive::Undefined;
    auto load = [&] {
        return loadSlotOrUndefined(value, baseOfSlot(cell, access), access);
    };
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        break;
    case FieldAccess::Coverage::SomeMembers:
    case FieldAccess::Coverage::EveryMemberHasSlot:
        if (mayBeAbsent)
            return access.coverage == FieldAccess::Coverage::EveryMemberHasSlot ? load() : readIfMemberHasField(value, cell, access, load);
        return inResultRepresentation(value, loadFilledSlot(value, baseOfSlot(cell, access), access));
    case FieldAccess::Coverage::EveryMember:
        return mayBeAbsent ? load() : inResultRepresentation(value, loadFilledSlot(value, baseOfSlot(cell, access), access));
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    define(value, noteIntegerAddedToInSlot(m_out, read, typeOf(value.id()), firstNewOperation, onlyUser && allSums.contains(opcodeOf(onlyUser))));
}

void SlotAccessEmitter::generateGetSlotOrUndefined(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateSlotRead(value);
        return;
    }
    define(value, readSlot(value, in<CellPointer>(value->objectOperand()), fieldAccessOf(value, false), true));
}

// Nothing writes the words of a GetterSetter once it is made, so what its getter and its setter are depends on the slot alone.
std::optional<CellPointer> SlotAccessEmitter::loadHalfOfAccessorInSlot(const ValueContext& value, ptrdiff_t offsetInGetterSetter)
{
    // No generic operation answers with a half of an accessor, so there is nothing to do where a guess fails.
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

// The function that stands for an absent setter ignores the call. Strict code calls the one that throws.
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
    // A read of a filled slot stands behind the test that says so, and only a receiver that has the field passes it.
    bool looksOutsideSlot = readsEmptySlotAsUndefined && mayHaveLeftDeferringSlot(value, access);
    auto load = [&] {
        return looksOutsideSlot ? loadDeferringSlotOrFieldOutsideIt(value, cell, access) : loadDeferringSlot(value, cell, access, readsEmptySlotAsUndefined);
    };
    bool testsForField = readsEmptySlotAsUndefined && access.coverage == FieldAccess::Coverage::SomeMembers;
    define(value, testsForField ? readIfMemberHasField(value, cell, access, load) : load());
}

// If every receiver defers a closure that captures the object, every one a closure that captures nothing, or every one a closure over the environment in its hidden field, a stub makes it as the allocation would have.
// `new Readable({ read() {} })`: the constructor reads `options.read` of every caller's literal, each with a function of its own.
CellPointer SlotAccessEmitter::makeDeferredClosure(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    static constexpr unsigned maxFunctionsDeferredAtOneRead = 8;
    Vector<ConstantEmitter::DeferredFunction, 4> functions;
    bool capturesObject = false;
    std::optional<uint32_t> offsetOfEnvironment;
    // Only an object that has the field comes here, unless the read takes the bytes of an object without it for an empty slot.
    bool findsSlotAtOneOffset = !access.findsOffsetByShape && !access.slot.isOutOfLine() && access.coverage != FieldAccess::Coverage::EveryMemberHasSlot;
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
    // The record of the layout names the function and says what the closure captures.
    auto makeInRunTime = [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::MaterializeDeferredClosure, { cell, identifierNumber(access.key) }, { .isOnRarePath = true });
    };
    if (areAlike && !functions.isEmpty())
        return emitter<ConstantEmitter>().makeClosureOfField(value, functions.span(), cell, access.slot.offset, capturesObject, offsetOfEnvironment, makeInRunTime);
    return makeInRunTime();
}

Operand SlotAccessEmitter::loadDeferringSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool readsEmptySlotAsUndefined)
{
    // The words for "not made yet" and "absent" come first. Behind them come null, undefined and the references, which are what a narrow reference holds.
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
        // One test serves whatever the slot holds: the two words that are no value are told apart on this side of it.
        return either<NarrowReference>(isZero(Word32(word.value())), makeClosure, [&] {
            return NarrowReference(m_out.constInt32(undefinedCell32));
        });
    });
    return decodeWordOfDeferringSlot(value, held);
}

// A store of what the slot cannot hold and a definition with other attributes move the field to the extra properties, and leave the slot absent.
bool SlotAccessEmitter::mayHaveLeftDeferringSlot(const ValueContext& value, const FieldAccess& access) const
{
    // A result in any other form is proven to be what the slot holds, or undefined.
    if (value.representation() != Representation::JSValue)
        return false;
    return std::ranges::any_of(access.membersWithField, [&](LayoutID member) {
        std::optional<FieldIndex> field = m_plan.slots().findField(m_program, member, access.key);
        return !field || !m_facts.field({ member, *field }).stability.containsAll({ FieldStability::IsNeverStoredWithOtherType, FieldStability::IsNeverRedefined });
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
    case Representation::LiteralString:
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
    // The next read of the slot makes the closure.
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
    barrierAfterStore(value, cell, operand, storedType);
}

void SlotAccessEmitter::markCellIfMarkerReplacesRawValue(CellPointer cell, const FieldAccess& access, ValueID stored)
{
    if (!typeOf(stored).mayBeUndefinedOrNull())
        return;
    SlotEmitter& slots = emitter<SlotEmitter>();
    if (slots.holdsRawValueFromBirthInSomeMember(setOfMembersWithField(access), access.key))
        slots.markCellIfMarkerIsStored(cell, stored);
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
        return;
    }
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    markCellIfMarkerReplacesRawValue(cell, access, stored);
    // Plan puts a slot out of line only where every PutSlot finds it filled, so the Extension is there and holds its granule, unless the object lists the field without storage.
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

// The key of a slot in the cell is its offset.
static bool everyMemberKeepsFieldAtSlotKey(const Program& program, const Plan& plan, const FieldAccess& access)
{
    static_assert(slotKeyFor(8, false) == 8);
    return std::ranges::all_of(access.membersWithField, [&](LayoutID member) {
        return slotKeyFor(slotOfMemberWithField(program, plan, member, access.key).offset, false) != noSlotKey;
    });
}

// One stub asks whether the field is missing, gives a slot out of line its granule, lists the key, stores and runs the barrier.
bool SlotAccessEmitter::tryPutSlotOrAddByStub(const ValueContext& value, CellPointer cell, const FieldAccess& access, ValueID stored)
{
    const PlacedSlot& slot = access.slot;
    SlotRepresentation representation = slot.representation;
    Operand operand = get(stored);
    Representation formOfStoredValue = representation == SlotRepresentation::DeferredClosure ? Representation::Cell32 : Typed::representationOf(representation);
    if (!canLeaveSlot(representation) || operand.representation != formOfStoredValue)
        return false;
    uint32_t slotKey = slotKeyFor(slot.offset, slot.isOutOfLine());
    if (slotKey == noSlotKey)
        return false;
    if (access.findsOffsetByShape) {
        if (slot.isOutOfLine() || !everyMemberKeepsFieldAtSlotKey(m_program, m_plan, access))
            return false;
        LValue keyOfThisObject = m_out.add(m_out.constInt64(slotKey), zeroExtended(distanceBehindLowestOffset(cell, access)).value());
        callStub(value, AOT::putSlotOrAddAtKeyInRegister(representation), { cell.value(), operand.value, keyOfThisObject }, { .accessesCollectorState = true });
        return true;
    }
    // The stub takes the zeros of a new granule for a slot that is empty.
    if (slot.isOutOfLine() && !representsAbsentAsZero(representation))
        return false;
    PlaceOfCreatedSlot place = slot.isOutOfLine() ? PlaceOfCreatedSlot::OutOfLine : PlaceOfCreatedSlot::InCell;
    callStub(value, AOT::putSlotOrAdd(place, representation), { cell.value(), operand.value }, { .immediate = slotKey, .accessesCollectorState = true });
    return true;
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
        return;
    }

    SlotEmitter& slots = emitter<SlotEmitter>();
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    MemoryLocation location = locationOfSlotAccess(access);
    bool needsStorage = needsStorageFor(access.slot, typeOf(stored));
    markCellIfMarkerReplacesRawValue(cell, access, stored);
    bool isWrittenInLine = generator().takesOpenForm(value);
    if (!isWrittenInLine && needsStorage && mayCreateListedField(access) && !value->orderWordAfter() && !isDeferredClosure(stored) && tryPutSlotOrAddByStub(value, cell, access, stored))
        return;
    if (access.slot.isOutOfLine() && needsStorage)
        slots.ensureExtensionHolds(value, cell, access.extensionWord, granuleBitOf(access.slot.offset), granuleBitsWithFieldsBehind(access), emitter<BarrierEmitter>().barrierOfExtension(value));

    auto store = [&] {
        if (needsStorage)
            writeSlot(value, cell, access, stored);
        else if (!value->hasNoExtension())
            slots.storeIfExtensionHolds(baseOfSlot(cell, access), access.slot, location, get(stored), access.extensionWord);
    };
    if (!mayCreateListedField(access)) {
        store();
        return;
    }

    // Of a cell whose order word the walk knows: it holds every property in its slot and does not list the key, and this is its word once it does.
    if (std::optional<OrderWord> orderWordAfter = value->orderWordAfter()) {
        storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(orderWordAfter->value())));
        store();
        return;
    }

    LayoutSetID membersWithField = setOfMembersWithField(access);
    auto listFieldIfMissing = [&] {
        LBasicBlock creates = newBlock();
        LBasicBlock done = newBlock();
        // A field without storage may be listed already. The run time passes over a key that the object lists.
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

    // The key is listed first, while the slot still says whether the field is missing.
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
    // Of a cell whose order word the walk knows: it is extensible, holds every property in its slot and lacks the field, and this is its word once it lists the key.
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

    // An order word without flags says that the object is extensible and holds every property in its slot.
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
    // The run time notifies the caches about objects that inherit from a prototype.
    auto isNoPrototype = [&] {
        return hasNoBits(loadField<Word32>(loadStructure(cell), m_heaps.Structure_bitField), Structure::s_mayBePrototypeBits);
    };
    if (!definition->objectIsNoPrototype())
        goOnIf(isNoPrototype());
    if (access->slot.isOutOfLine())
        slots.ensureExtensionHolds(definition, cell, access->extensionWord, granuleBitOf(access->slot.offset), granuleBitsWithFieldsBehind(*access), OwnerBarrier::Emitted);
    auto lacksField = [&] {
        return slots.holdsAbsent(slots.addressOf(cell, access->slot, location, extensionWordOfSlotAccess(*access)), access->slot.representation);
    };
    if (!orderWordAfter)
        goOnIf(lacksField());

    // Everything that allocates comes first, so nothing separates the stores from their barrier.
    Type storedType = stored ? typeOf(stored) : types().at(TypeTable::object);
    Operand operand = stored ? get(stored) : Operand(newGetterSetter(definition, definition->getterOperand(), definition->setterOperand()));
    operand = emitter<ConversionEmitter>().inRepresentation(definition, operand, storedType, Typed::representationOf(access->slot.representation));
    // The key is listed while the slot still says that the field is missing.
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

// The places in the orders of the member where the created fields come next. None where the member lacks one of the fields.
static KeyOrders::Continuations continuationsCreatingSlots(const Program& program, const Plan& plan, LayoutID member, std::span<const PropertyKeyID> createdKeys)
{
    Vector<FieldIndex, 4> fields;
    for (PropertyKeyID key : createdKeys) {
        std::optional<FieldIndex> field = plan.slots().findField(program, member, key);
        if (!field || !plan.slot(member, *field).hasStorage())
            return { };
        fields.append(*field);
    }
    return plan.keyOrders().continuationsCreating(member, fields.span());
}

uint64_t SlotAccessEmitter::granuleBitsWithFieldsBehind(const FieldAccess& access) const
{
    RELEASE_ASSERT(access.slot.isOutOfLine());
    const KeyOrders& orders = m_plan.keyOrders();
    // What every member is likely to need: a granule that an object never fills is room lost.
    std::optional<uint64_t> bitsBehind;
    for (LayoutID member : access.membersWithField) {
        KeyOrders::Continuations continuations = continuationsCreatingSlots(m_program, m_plan, member, singleElementSpan(access.key));
        const PlannedLayout& layout = m_plan.layout(member);
        bool hasExtensionWordOfAccess = layout.slots[layout.extensionWordIndex()].hasLocationOf(access.extensionWord);
        // The store may be the first that asks for an Extension.
        uint64_t bitsOfMember = hasExtensionWordOfAccess ? layout.granulesOfFirstExtension : 0;
        if (!continuations.isEmpty() && hasExtensionWordOfAccess) {
            uint32_t orderWordAfter = continuations[0].orderWordAfter;
            std::span<const FieldIndex> order = orders.order(member, orderOf(orderWordAfter));
            std::span<const KeyOrderSource> sources = orders.sourcesOfFields(member, orderOf(orderWordAfter));
            // From the first field on that stands there only because the layout declares it, nobody has seen an object get the fields one behind the other.
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

// Where every member lists the created fields at the same place of an order with the same number, the order word alone says that the step applies.
static std::optional<KeyOrderContinuation> findSlotContinuationOfEveryMember(const Program& program, const Plan& plan, std::span<const LayoutID> members, std::span<const PropertyKeyID> createdKeys)
{
    std::optional<KeyOrderContinuation> common;
    for (LayoutID member : members) {
        // An order is numbered within its shape.
        if (plan.layout(member).keysAreNeverEnumerated || plan.shapes().shapesOf(member).count != 1)
            return std::nullopt;
        KeyOrders::Continuations continuations = continuationsCreatingSlots(program, plan, member, createdKeys);
        if (continuations.isEmpty() || (common && *common != continuations[0]))
            return std::nullopt;
        common = continuations[0];
    }
    return common;
}

// One comparison, with the whole word: an object that is not extensible, or that has properties outside its slots, is left to the run time.
// Every other step is the stub's, which remembers what the slot keys of a call made of the two words: KeyOrderRun.
std::optional<KeyOrderStep> SlotAccessEmitter::testLikeliestKeyOrderStep(CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys)
{
    const ShapeTable& shapes = m_plan.shapes();
    std::span<const LayoutID> members = membersOf(types(), receivers);
    if (std::optional<KeyOrderContinuation> common = findSlotContinuationOfEveryMember(m_program, m_plan, members, createdKeys))
        return KeyOrderStep { equal(emitter<ShapeTestEmitter>().orderWordOf(cell), static_cast<int32_t>(common->orderWordBefore)), word32(static_cast<int32_t>(common->orderWordAfter)) };
    // An order is numbered within its shape, and the two words lie side by side.
    static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && !(offsetOfOrderWord % sizeof(uint64_t)) && std::endian::native == std::endian::little);
    for (LayoutID member : members) {
        if (m_plan.layout(member).keysAreNeverEnumerated)
            continue;
        KeyOrders::Continuations continuations = continuationsCreatingSlots(m_program, m_plan, member, createdKeys);
        if (continuations.isEmpty())
            continue;
        uint64_t wordsBefore = static_cast<uint64_t>(AOT::shapeWordOf(shapes.generalShapeOf(member))) << 32 | continuations[0].orderWordBefore;
        Integer words(m_out.load64(m_out.address(m_heaps.AOTObject_orderWord, cell.value(), offsetOfOrderWord)));
        return KeyOrderStep { equal(words, integer(static_cast<int64_t>(wordsBefore))), word32(static_cast<int32_t>(continuations[0].orderWordAfter)) };
    }
    return std::nullopt;
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
        callStub(value, AOT::didCreateField(CreatedFields::One), { cell.value(), m_out.add(lowestKey, zeroExtended(distanceBehindLowestOffset(cell, access)).value()) });
        return;
    }
    if (slotKey == noSlotKey) {
        callVoidOperation(value, RuntimeOperation::DidCreateField, { cell, identifierNumber(access.key) }, { .isOnRarePath = isOnRarePath });
        return;
    }
    // The slot key says where the slot is. Every caller has seen to the granule of a slot out of line that needs one.
    callStub(value, AOT::didCreateFieldOfSlot(CreatedFields::One), { cell.value() }, { .immediate = slotKey });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
