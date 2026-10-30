/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementCheck.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFrameAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTGuardFactDetails.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTOperationData.h"
#include "AOTPlacementSteps.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpRequests.h"
#include "AOTRepresentation.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(Value::opcodesWithAllocation == OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::MaterializeEnvironment });
static_assert(Value::opcodesWithGuardBit == OpcodeSet { Opcode::AreMembersUnchanged });
static_assert(Value::opcodesWithStaticsOffset == OpcodeSet { Opcode::NewArray, Opcode::NewRegExp, Opcode::SharedRegExpLiteral, Opcode::NewTemplateObject });
static_assert(Value::opcodesWithRequest == OpcodeSet { Opcode::TryPutByIds, Opcode::NewClass, Opcode::NewTemplateObject, Opcode::DirectEval, Opcode::Data, Opcode::NewSetFromData, Opcode::NewMapFromData, Opcode::CopyDataProperties, Opcode::CopyObject });

namespace {

struct PlacementCheckContext {
    bool expect(bool holds, PlacementRule rule, LayoutID layout = { }, FunctionID function = { }, ValueID value = { }) const
    {
        constexpr unsigned maxReportedFailures = 100;
        if (!holds && failures++ < maxReportedFailures)
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::PlacementCheckFailed, .function = function, .value = value, .layout = layout, .count = static_cast<uint32_t>(rule) });
        return holds;
    }

    template<typename ID>
    void forEach(size_t count, void (*check)(const PlacementCheckContext&, ID)) const
    {
        forEachIndexInParallel(loops, count, [&](unsigned index) {
            check(*this, ID(index));
        });
    }

    const Program& program;
    const PlacementInputs& inputs;
    const FlowResults& flow;
    const SlotPlan& slots;
    const Placement& placement;
    const ShapeTable& shapes;
    const StaticsPlan& statics;
    const SiteCaches& siteCaches;
    BuildContext& build;
    const ParallelLoops& loops;
    std::atomic<unsigned>& failures;
};

struct Extent {
    uint64_t first { 0 };
    uint64_t end { 0 };
};

struct StorableValues {
    uint16_t primitives { 0 };
    Range integers { Range::empty() };
    bool hasObjects { false };
    bool hasRawWords { false };
};

struct ClassSideFlags {
    bool& operator[](ClassSide side)
    {
        switch (side) {
        case ClassSide::Prototype:
            return prototypeSide;
        case ClassSide::Static:
            return staticSide;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool prototypeSide { false };
    bool staticSide { false };
};

struct BitSetUse {
    uint32_t firstWord { 0 };
    uint32_t numberOfWords { 0 };

    friend auto operator<=>(const BitSetUse&, const BitSetUse&) = default;
};

enum class SiteCellKind : uint8_t { ArrayProfile, RegExp, TemplateObject };

struct SiteCellUse {
    uint32_t offset { 0 };
    SiteCellKind kind { SiteCellKind::ArrayProfile };
    SourceSite site;
    SiteKey user;
    RegExpRecordID regExpRecord;
};

struct EntriesWithPlacedFields {
    OpcodeSet opcodes;
    TerminatorKindSet terminators;
};

struct SiteCacheUse {
    SiteCacheID cache;
    SiteWeight weight { 0 };
    SiteKey user;
};

} // anonymous namespace

static constexpr uint32_t sizeOfStaticsWord = sizeof(uint64_t);

ASCIILiteral nameOf(PlacementRule rule)
{
    switch (rule) {
    case PlacementRule::None:
        return "None"_s;
#define AOT_PLACEMENT_RULE_NAME(name) \
    case PlacementRule::name: \
        return #name ""_s;
    FOR_EACH_AOT_PLACEMENT_RULE(AOT_PLACEMENT_RULE_NAME)
#undef AOT_PLACEMENT_RULE_NAME
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Extent extentAt(uint32_t offset, uint32_t size) { return { offset, static_cast<uint64_t>(offset) + size }; }

static bool areDisjoint(std::span<Extent> extents, uint64_t start, uint64_t limit)
{
    AOT::sort(extents, { }, &Extent::first);
    uint64_t end = start;
    for (auto& extent : extents) {
        if (extent.first < end)
            return false;
        end = extent.end;
    }
    return end <= limit;
}

#if USE(BIGINT32)
static constexpr uint16_t checkedPrimitivesAlwaysInCells = cellPrimitives & ~Primitive::BigInt;
#else
static constexpr uint16_t checkedPrimitivesAlwaysInCells = cellPrimitives;
#endif
static constexpr uint16_t primitivesWithoutMarkers = Primitive::Defined & ~(Primitive::UndefinedOrNull | Primitive::Absent);

static constexpr Range integerRangeOf(SlotTraits traits)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(traits.width < sizeof(int64_t));
    int64_t numberOfValues = 1ll << (8 * traits.width);
    if (traits.valueKind == SlotValueKind::UnsignedInteger)
        return { 0, numberOfValues - 1 };
    return { -(numberOfValues / 2), numberOfValues / 2 - 1 };
}

static_assert(integerRangeOf(traitsOf(SlotRepresentation::Int32)) == Range::int32());
static_assert(integerRangeOf(traitsOf(SlotRepresentation::Uint32)) == Range::uint32());
static_assert(canLeaveSlot(SlotRepresentation::DeferredClosure) && !representsAbsentAsZero(SlotRepresentation::DeferredClosure));
static_assert(traitsOf(SlotRepresentation::DeferredClosure).isNarrowReference);

static uint16_t markerPrimitivesOf(const SlotTraits& traits)
{
    return (traits.hasAbsent ? Primitive::Absent : 0) | (traits.hasUndefined ? Primitive::Undefined : 0) | (traits.hasNull ? Primitive::Null : 0);
}

static bool isStorable(OpaqueKind kind, const StorableValues& storable)
{
    Representation representation = representationOf(kind);
    if (representation == Representation::Void)
        return true;
    if (representation == Representation::RawWord)
        return storable.hasRawWords;
    return holdsCells(representation) && storable.hasObjects;
}

static bool isStorable(const Type& type, const SlotTraits& traits, const StorableValues& storable)
{
    if (type.primitives & ~(storable.primitives | markerPrimitivesOf(traits)) || !type.integers.isSubsetOf(storable.integers))
        return false;
    if (type.opaque && !isStorable(*type.opaque, storable))
        return false;
    return storable.hasObjects || !(type.hasObjects() || type.environment);
}

static bool canHoldType(SlotRepresentation representation, const Type& type)
{
    SlotTraits traits = traitsOf(representation);
    switch (representation) {
    case SlotRepresentation::JSValue:
        return isStorable(type, traits, { .primitives = primitivesWithoutMarkers, .integers = Range::full(), .hasObjects = true });
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        return isStorable(type, traits, { .primitives = checkedPrimitivesAlwaysInCells, .hasObjects = true });
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
        return isStorable(type, traits, { .primitives = Primitive::Integer, .integers = integerRangeOf(traits) });
    case SlotRepresentation::Int64:
        return isStorable(type, traits, { .primitives = Primitive::Integer, .integers = Range::full(), .hasRawWords = true });
    case SlotRepresentation::OptionalInt64:
        return isStorable(type, traits, { .primitives = Primitive::Integer, .integers = Range::full() });
    case SlotRepresentation::OptionalInt32:
        return isStorable(type, traits, { .primitives = Primitive::Integer, .integers = { minInlineInt32, INT32_MAX } });
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        return isStorable(type, traits, { .primitives = Primitive::Number, .integers = Range::full() });
    case SlotRepresentation::Float:
        return isStorable(type, traits, { });
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
        return isStorable(type, traits, { .primitives = Primitive::Boolean });
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        return isStorable(type, traits, { .primitives = Primitive::AnyString });
    case SlotRepresentation::DeferredClosure:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LayoutStorage storageRequiredBy(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    bool mayBeAllocated = layout.flow.allocation != AllocationState::NeverAllocated && !builtinBaseClassOf(program, id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return mayBeAllocated || declaresPrivateName(layout) ? LayoutStorage::CompiledObject : LayoutStorage::None;
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        return mayBeAllocated ? LayoutStorage::Environment : LayoutStorage::None;
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return LayoutStorage::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct PlannedLayoutCheck : PlacementCheckContext {
    bool expect(bool holds, PlacementRule rule) const { return PlacementCheckContext::expect(holds, rule, id); }

    void run() const
    {
        size_t numberOfRows = planned.hasSlots() ? layout.fields.size() + numberOfHiddenWords : 0;
        if (!expect(planned.storage == storageRequiredBy(program, id) && planned.slots.size() == numberOfRows, PlacementRule::LayoutStorageWrong) || !planned.hasSlots())
            return;

        for (const PlacedSlot& slot : planned.slots) {
            if (!slot.hasStorage())
                continue;
            expect(!(slot.offset % slot.width()), PlacementRule::SlotAlignment);
            expect(extentAt(slot.offset, slot.width()).end <= (slot.isOutOfLine() ? planned.extensionSize : planned.size), PlacementRule::SlotPastCellEnd);
        }
        checkCellExtents();
        checkExtensionExtents();
        checkHiddenWords();
        checkFieldRecords();
        for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
            if (planned.slots[index].hasStorage())
                checkField(index);
        }
        expect(!planned.keysAreNeverEnumerated || keysAreNeverEnumerated(program, flow, id), PlacementRule::ShapeVariantWrong);
    }

    void checkCellExtents() const
    {
        Vector<Extent, 32> extents;
        for (const PlacedSlot& slot : planned.slots) {
            if (slot.storage == SlotStorage::Inline)
                extents.append(extentAt(slot.offset, slot.width()));
        }
        if (auto offset = planned.isEscapedOffset)
            extents.append(extentAt(*offset, sizeof(bool)));
        uint64_t start = isCompiledObject ? offsetOfFirstField : offsetOfFirstEnvironmentVariable;
        if (!expect(areDisjoint(extents.mutableSpan(), start, planned.size), PlacementRule::CellOverlap))
            return;
        for (const ReservedBytes& reserved : planned.bytesKeptZero)
            extents.append(extentAt(reserved.offset, reserved.width));
        expect(areDisjoint(extents.mutableSpan(), start, planned.size), PlacementRule::BytesKeptZeroNotFree);
    }

    void checkExtensionExtents() const
    {
        Vector<Extent, 8> extents;
        for (const PlacedSlot& slot : planned.slots) {
            if (slot.isOutOfLine())
                extents.append(extentAt(slot.offset, slot.width()));
        }
        expect(extents.isEmpty() ? !planned.extensionSize : areDisjoint(extents.mutableSpan(), offsetOfFirstExtensionSlot, planned.extensionSize), PlacementRule::ExtensionOverlap);
        expect(planned.extensionSize <= maxExtensionSize, PlacementRule::SlotPastCellEnd);
    }

    void checkHiddenWords() const
    {
        constexpr OptionSet<ObjectMutation> mutationsCreatingExtraProperties { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::StoreOfOtherType };
        bool needsExtension = std::ranges::any_of(planned.slots, &PlacedSlot::isOutOfLine);
        bool needsExtraPropertiesWord = isCompiledObject && someAllocationClassMay(program, flow, id, mutationsCreatingExtraProperties);
        expect(needsExtension == planned.hasExtension() && (!needsExtraPropertiesWord || planned.hasExtraPropertiesWord()), PlacementRule::HiddenWordMissing);

        SlotRepresentation representation = hiddenReferenceRepresentation(usesWideReferences);
        for (const PlacedSlot& word : planned.slots.span().last(numberOfHiddenWords))
            expect(!word.hasStorage() || (word.storage == SlotStorage::Inline && word.representation == representation), PlacementRule::HiddenWordMissing);
    }

    void checkFieldRecords() const
    {
        uint32_t numberOfRecords = 0;
        bool areValid = true;
        for (FieldIndex index = 0; index < planned.slots.size(); ++index) {
            std::optional<uint32_t> expected;
            if (isCompiledObject && index < planned.numberOfFields() && planned.slots[index].hasStorage())
                expected = numberOfRecords++;
            areValid &= planned.slots[index].indexInLayoutRecord == expected;
        }
        expect(areValid && planned.numberOfRecordFields == numberOfRecords, PlacementRule::FieldRecords);
    }

    void checkField(FieldIndex index) const
    {
        const PlacedSlot& slot = planned.slots[index];
        const Field& declaration = fieldAt(program, declarationOf(program, { id, index }));
        const Type& type = program.at(slotTypeOf(program, declaration));

        expect(canHoldType(slot.representation, slotHoldsUndefinedAndNull(program, { id, index }) ? type.withPrimitives(Primitive::UndefinedOrNull) : type), PlacementRule::RepresentationCannotHoldType);
        expect(canLeaveSlot(slot.representation) || canOmitAbsentState(declaration.flow), PlacementRule::AbsentStateMissing);
        expect(isStorageAllowed(slot, type), PlacementRule::OutOfLineNotAllowed);
        expect(hasRequiredWidth(index, slot), PlacementRule::WideReferenceMissing);
        expect(isDeferredClosureProven(slot, declaration), PlacementRule::DeferredClosureNotProven);
    }

    // The run time allocates the object of a subclass and of a function called with new, and the stores of the constructor record the keys.
    bool isCreatedWithoutKeys() const
    {
        return layout.constructor && !planned.keysAreNeverEnumerated && program.at(layout.constructor).kind != FunctionKind::BaseConstructor;
    }

    bool canOmitAbsentState(const FieldFlow& fieldFlow) const
    {
        constexpr OptionSet<FieldStability> everyStability { FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };
        switch (layout.kind) {
        case LayoutKind::CoroutineFrame:
            return true;
        case LayoutKind::Environment:
            return fieldFlow.location.initialization != InitializationState::Unknown && fieldFlow.stability.containsAll(everyStability);
        case LayoutKind::Record:
        case LayoutKind::Instance:
        case LayoutKind::Container:
        case LayoutKind::BuiltinResult:
            return fieldFlow.occupancy == Occupancy::AlwaysFilled && fieldFlow.stability.containsAll(everyStability) && cannotReachUnknownJavaScript && !isCreatedWithoutKeys();
        case LayoutKind::Closure:
        case LayoutKind::ModuleNamespace:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isStorageAllowed(const PlacedSlot& slot, const Type& type) const
    {
        if (!slot.isOutOfLine())
            return !slot.mayExistWithoutStorage;
        if (isCoroutineFrame || inputs.frameAllocation.mayBeInFrame(id) || !representsAbsentAsZero(slot.representation))
            return false;
        return !slot.mayExistWithoutStorage || ((type.primitives & Primitive::Undefined) && !mayHaveUnorderedShape(program, flow, id));
    }

    bool hasRequiredWidth(FieldIndex index, const PlacedSlot& slot) const
    {
        if (!traitsOf(slot.representation).isNarrowReference)
            return true;
        return !usesWideReferences && !isCoroutineFrame && !planned.holdsWideReferences && !inputs.frameAllocation.mayHoldFramePointer(FieldReference { id, index });
    }

    bool holdsClosureEnvironmentInCell() const
    {
        std::optional<uint32_t> field = layout.flow.closureEnvironmentField;
        if (!field || *field >= planned.numberOfFields())
            return false;
        return planned.slots[*field].storage == SlotStorage::Inline && planned.slots[*field].representation == SlotRepresentation::Cell32;
    }

    bool isDeferredClosureProven(const PlacedSlot& slot, const Field& declaration) const
    {
        if (slot.representation != SlotRepresentation::DeferredClosure)
            return !slot.deferredFunction && !slot.deferredClosureCapturesObject && !slot.deferredClosureCapturesEnvironment;
        const LocationFlow& location = declaration.flow.location;
        return location.deferral != ClosureDeferral::Unknown
            && slot.deferredFunction
            && slot.deferredFunction == location.deferredFunction
            && slot.deferredClosureCapturesObject == (location.deferral == ClosureDeferral::CapturesObject)
            && slot.deferredClosureCapturesEnvironment == (location.deferral == ClosureDeferral::CapturesEnvironment)
            && (!slot.deferredClosureCapturesEnvironment || holdsClosureEnvironmentInCell())
            && !declaration.flags.contains(FieldFlags::IsPrivate);
    }

    LayoutID id;
    const Layout& layout { program.at(id) };
    const PlannedLayout& planned { slots.layout(id) };
    bool isCompiledObject { planned.storage == LayoutStorage::CompiledObject };
    bool usesWideReferences { build.recordedOptions().useWideReferences() };
    bool isCoroutineFrame { layout.kind == LayoutKind::CoroutineFrame };
    bool cannotReachUnknownJavaScript { everyAllocationClassCannot(program, flow, id, ObjectMutation::ReachUnknownJavaScript) };
};

} // anonymous namespace

static void checkPlannedLayout(const PlacementCheckContext& context, LayoutID id)
{
    PlannedLayoutCheck { context, id }.run();
}

static bool capturesOnlyClassEnvironment(const Program& program, const Function& constructor, const Function& method)
{
    if (method.captures.isEmpty())
        return true;
    if (method.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = method.captures[0].written();
    return captured && captured == constructor.captures[0].written() && program.at(captured).environment;
}

static bool hasKnownBaseChain(const Program& program, LayoutID id)
{
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(id);
        switch (layout.flow.baseState) {
        case BaseState::Unknown:
        case BaseState::BuiltinClass:
            return false;
        case BaseState::None:
            return !layout.has(LayoutFlags::HasHeritage);
        case BaseState::Layout:
            id = layout.flow.base;
            break;
        }
    }
    return false;
}

static void checkClass(const PlacementCheckContext& context, LayoutID id)
{
    const Layout& layout = context.program.at(id);
    if (layout.kind != LayoutKind::Instance || !layout.constructor)
        return;

    const ClassCreationPlan& plan = context.slots.classCreation(id);
    const Vector<ClassMember>& members = layout.classFlow.members;
    if (!context.expect(plan.members.size() == members.size(), PlacementRule::MemberCreationOrder, id))
        return;

    bool isDefinedByNewClass = !layout.has(LayoutFlags::IsConstructorFunction);
    ClassSideFlags hasMemberAtDefinition;
    bool isOrderValid = true;
    for (size_t index = 0; index < members.size(); ++index) {
        const ClassMember& member = members[index];
        switch (plan.members[index]) {
        case MemberCreation::AtDefinition:
            hasMemberAtDefinition[member.side] = true;
            break;
        case MemberCreation::ByClassRecord: {
            bool fitsClassRecord = member.key && member.function && !context.program.isIndexLike(member.key) && capturesOnlyClassEnvironment(context.program, context.program.at(layout.constructor), context.program.at(member.function));
            isOrderValid &= isDefinedByNewClass && !hasMemberAtDefinition[member.side] && fitsClassRecord;
            break;
        }
        }
    }
    context.expect(isOrderValid, PlacementRule::MemberCreationOrder, id);

    bool mayCreateMembersLazily = isDefinedByNewClass && layout.classFlow.evaluations == ExecutionCount::AtMostOnce && !hasMemberAtDefinition.prototypeSide;
    bool isLazyCreationAllowed = (!plan.createsMembersLazily || mayCreateMembersLazily)
        && (!plan.createsClassObjectOnMutation || (plan.createsMembersLazily && !hasMemberAtDefinition.staticSide))
        && (!plan.createsPrototypeBeforeClassObject || (plan.createsMembersLazily && hasKnownBaseChain(context.program, id)));
    context.expect(isLazyCreationAllowed, PlacementRule::LazyCreationNotAllowed, id);
}

static void checkDefinedMethods(const PlacementCheckContext& context, FunctionID id)
{
    const Function& function = context.program.at(id);
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (value.opcode != Opcode::DefineMethod)
                continue;
            const Value& closure = function.at(definitionThroughUpcasts(function, value.operand(OperandIndex::DefineMethod::function)));
            if (closure.opcode == Opcode::NewClosure)
                context.expect(context.slots.creationOf(context.program, value.layout(), closure.function()) == MemberCreation::AtDefinition, PlacementRule::MemberCreationOrder, value.layout(), id, valueID);
        }
    }
}

static void checkClosureForm(const PlacementCheckContext& context, FunctionID id)
{
    const Function& function = context.program.at(id);
    bool canHaveSlimForm = !function.hasAny({ FunctionFlags::IsGenerator, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee }) && !isClassConstructorOf(function.kind);
    context.expect(context.slots.closureFormOf(id) != ClosureForm::Slim || canHaveSlimForm, PlacementRule::ClosureFormNotProven, { }, id);
    bool targetIsSlim = context.slots.closureFormOf(closureTargetOf(context.program, id)) == ClosureForm::Slim;
    context.expect(static_cast<bool>(context.placement.functionKind(id).isSlim) == targetIsSlim, PlacementRule::ClosureFormNotProven, { }, id);
}

static bool isInterned(const Program& program, LayoutSetID set)
{
    return set && set.index() < program.types.numberOfLayoutSets();
}

static LayoutID firstMemberOf(const Program& program, LayoutSetID set)
{
    auto& members = program.at(set).written();
    return members.isEmpty() ? LayoutID() : members.first();
}

static uint32_t maxShapesOf(const PlacementCheckContext& context, LayoutID id)
{
    switch (context.slots.storageOf(id)) {
    case LayoutStorage::CompiledObject:
        return 2;
    case LayoutStorage::Environment:
        return 1;
    case LayoutStorage::None:
    case LayoutStorage::OrdinaryObject:
        return id == context.placement.closureLayout() ? 1 : 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool tryMarkShapeRange(const PlacementCheckContext& context, LayoutID id, BitVector& numbersInRanges)
{
    ShapeRange range = context.shapes.shapesOf(id);
    bool isCompiledObject = context.slots.storageOf(id) == LayoutStorage::CompiledObject;
    uint32_t maxShapes = maxShapesOf(context, id);
    if (range.isEmpty())
        return !maxShapes;
    if (range.count > maxShapes || !range.first || static_cast<uint64_t>(range.first) + range.count - 1 > context.shapes.numberOfShapes())
        return false;
    bool isValid = isCompiledObject ? range.end() <= context.shapes.firstEnvironmentShape() : range.first >= context.shapes.firstEnvironmentShape();
    for (uint32_t number = range.first; number < range.end(); ++number) {
        if (context.shapes.shape(number).layout != id || numbersInRanges.quickSet(number))
            isValid = false;
    }
    return isValid;
}

static bool checkShapeNumbers(const PlacementCheckContext& context)
{
    bool areValid = true;
    BitVector numbersInRanges;
    numbersInRanges.ensureSize(static_cast<size_t>(context.shapes.numberOfShapes()) + 1);
    for (unsigned i = 0; i < context.program.layouts.size(); ++i)
        areValid &= context.expect(tryMarkShapeRange(context, LayoutID(i), numbersInRanges), PlacementRule::ShapeNumbers, LayoutID(i));
    for (uint32_t number = context.shapes.shapesBelow() + 1; number <= context.shapes.numberOfShapes(); ++number) {
        bool isAssigned = context.shapes.isAssigned(number);
        areValid &= context.expect(isAssigned == numbersInRanges.quickGet(number) && context.shapes.shapeCountInRange(number, number) == static_cast<uint32_t>(isAssigned), PlacementRule::ShapeNumbers, context.shapes.shape(number).layout);
    }
    return areValid;
}

static void checkShapeVariants(const PlacementCheckContext& context, LayoutID id)
{
    ShapeRange range = context.shapes.shapesOf(id);
    if (range.isEmpty())
        return;
    auto expect = [&](bool holds) {
        return context.expect(holds, PlacementRule::ShapeVariantWrong, id);
    };
    expect(range.count != 2 || (!context.shapes.shape(range.first).keysAreNeverEnumerated && context.shapes.shape(range.first + 1).keysAreNeverEnumerated));
    expect(!context.placement.layout(id).keysAreNeverEnumerated || context.shapes.shape(range.first).keysAreNeverEnumerated);
    for (AllocationClassID allocationClassID : context.program.at(id).flow.allocationClasses) {
        uint32_t number = context.shapes.shapeOf(id, allocationClassID);
        if (!expect(number >= range.first && number < range.end()))
            continue;
        const PlacedShape& shape = context.shapes.shape(number);
        const AllocationClass& allocationClass = context.flow.at(allocationClassID);
        expect((!shape.keysAreNeverEnumerated || allocationClass.keys == KeyEnumeration::NeverEnumerated) && allocationClass.impossibleMutations.containsAll(shape.impossibleMutations));
    }
}

static bool hasExtentOf(const ShapeTable& shapes, const ShapeTest& test, std::span<const uint32_t> ascendingShapes)
{
    auto spansShapes = [&] {
        if (ascendingShapes.empty() || !ascendingShapes.front() || ascendingShapes.back() > shapes.numberOfShapes())
            return false;
        return test.first == ascendingShapes.front() && test.count == ascendingShapes.back() - ascendingShapes.front() + 1;
    };
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
    case ShapeTestForm::AlwaysPasses:
        return true;
    case ShapeTestForm::OneShape:
        return spansShapes() && test.count == 1;
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
    case ShapeTestForm::BitSet:
        return spansShapes();
    case ShapeTestForm::ImmediateBits:
        return spansShapes() && test.count <= immediateBitsWindow;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<size_t> numberOfPassingShapes(const ShapeTable& shapes, const ShapeTest& test, std::span<const uint32_t> testedShapes)
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return 0;
    case ShapeTestForm::AlwaysPasses:
        return shapes.shapeCountInRange(1, shapes.numberOfShapes());
    case ShapeTestForm::OneShape:
    case ShapeTestForm::Range:
        return shapes.shapeCountInRange(test.first, test.last());
    case ShapeTestForm::UpTo:
        return shapes.shapeCountInRange(1, test.last());
    case ShapeTestForm::From:
        return shapes.shapeCountInRange(test.first, shapes.numberOfShapes());
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier: {
        uint32_t block = (test.last() - 1) / shapesPerBlock;
        bool isTestable = block < maxTestableBlocks && shapes.numberOfShapes() <= maxShapesForBlockTests;
        if (!isTestable || test.blockByte != static_cast<uint8_t>(shapeWordOf(test.last()) >> 8))
            return std::nullopt;
        return shapes.shapeCountInRange(test.form == ShapeTestForm::Block ? block * shapesPerBlock + 1 : 1, std::min((block + 1) * shapesPerBlock, shapes.numberOfShapes()));
    }
    case ShapeTestForm::ImmediateBits: {
        size_t numberPassing = 0;
        for (uint32_t distanceFromLast = 0; distanceFromLast < test.count; ++distanceFromLast)
            numberPassing += (test.bits >> distanceFromLast & 1) && shapes.isAssigned(test.last() - distanceFromLast);
        return numberPassing;
    }
    case ShapeTestForm::BitSet:
        return testedShapes.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkBitsInTable(const PlacementCheckContext& context, const ShapeTest& test, std::span<const uint32_t> passing, LayoutID subject, Vector<BitSetUse>& bitSets)
{
    std::span<const uint64_t> table = context.shapes.shapeSetWords();
    uint32_t firstWord = PagedBitSet::firstWordAt(test.bitSetOffset);
    PagedBitSet::Entries entries = PagedBitSet::entriesAt(test.bitSetOffset);
    uint64_t numberOfWords = 0;
    size_t numberOfMembers = 0;
    if (entries == PagedBitSet::Entries::Hashed) {
        bool isInTable = firstWord < table.size() && PagedBitSet::shiftIn(table[firstWord]) && PagedBitSet::shiftIn(table[firstWord]) <= 31 && firstWord + PagedBitSet::numberOfWordsOfHashedSet(PagedBitSet::shiftIn(table[firstWord])) <= table.size();
        if (!context.expect(isInTable, PlacementRule::BitSetsOverlap, subject))
            return;
        uint64_t head = table[firstWord];
        numberOfWords = PagedBitSet::numberOfWordsOfHashedSet(PagedBitSet::shiftIn(head));
        std::span<const PagedBitSet::Slot> slots = spanReinterpretCast<const PagedBitSet::Slot>(table.subspan(firstWord + 1, numberOfWords - 1)).first(PagedBitSet::numberOfSlots(PagedBitSet::shiftIn(head)));
        for (uint32_t slot = 0; slot < slots.size(); ++slot)
            numberOfMembers += PagedBitSet::slotOf(slots[slot], PagedBitSet::multiplierIn(head), PagedBitSet::shiftIn(head)) == slot;
    } else {
        uint64_t numberOfPages = numberOfPagesInShapeSet(test.count);
        numberOfWords = PagedBitSet::numberOfEntryWords(numberOfPages, entries);
        if (!context.expect(firstWord + numberOfWords <= table.size(), PlacementRule::BitSetsOverlap, subject))
            return;
        for (uint64_t page = 0; page < numberOfPages; ++page) {
            uint64_t word = PagedBitSet::wordOfPage(table.subspan(firstWord).data(), entries, page);
            if (!context.expect(firstWord + word < table.size(), PlacementRule::BitSetsOverlap, subject))
                return;
            numberOfWords = std::max<uint64_t>(numberOfWords, word + 1);
            numberOfMembers += std::popcount(table[firstWord + word]);
        }
    }
    bool holdsEveryMember = std::ranges::all_of(passing, [&](uint32_t shape) {
        return PagedBitSet::contains(table.subspan(firstWord).data(), entries, test.last() - shape);
    });
    context.expect(holdsEveryMember, PlacementRule::MemberFailsTest, subject);
    context.expect(numberOfMembers == passing.size(), PlacementRule::TestPassesOthers, subject);
    bitSets.append(BitSetUse { firstWord, static_cast<uint32_t>(numberOfWords) });
}

static void forEachKnownShape(std::span<const uint32_t> passing, std::span<const uint32_t> known, NOESCAPE const Invocable<void(uint32_t shape, bool shouldPass)> auto& functor)
{
    size_t next = 0;
    for (uint32_t shape : known) {
        while (next < passing.size() && passing[next] < shape)
            ++next;
        functor(shape, next < passing.size() && passing[next] == shape);
    }
}

static PlacementRule findKnownShapeFailure(const ShapeTable& shapes, const ShapeTest& test, std::span<const uint32_t> passing, std::span<const uint32_t> known)
{
    Vector<uint32_t, 16> passingKnownShapes;
    forEachKnownShape(passing, known, [&](uint32_t shape, bool shouldPass) {
        if (shouldPass)
            passingKnownShapes.append(shape);
    });
    if (!hasExtentOf(shapes, test, passingKnownShapes.span()) && !hasExtentOf(shapes, test, passing))
        return PlacementRule::TestPassesOthers;
    PlacementRule failure = PlacementRule::None;
    forEachKnownShape(passing, known, [&](uint32_t shape, bool shouldPass) {
        if (passes(test, shape, passing) != shouldPass)
            failure = shouldPass ? PlacementRule::MemberFailsTest : PlacementRule::TestPassesOthers;
    });
    return failure;
}

static PlacementRule findKnownSetFailure(const PlacementCheckContext& context, const ShapeTest& test, LayoutSetID tested, LayoutSetID known)
{
    // An object of a member that exists without a shape, such as a module namespace object, is none of the set's member shapes.
    auto all = context.program.at(known).all();
    bool listsEveryObjectByShape = all && std::ranges::all_of(*all, [&](LayoutID member) {
        return context.slots.holdsCompiledObjects(member) || context.program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
    if (!listsEveryObjectByShape)
        return test == context.shapes.testOf(tested) ? PlacementRule::None : PlacementRule::TestFormNeedsKnownShapes;
    return findKnownShapeFailure(context.shapes, test, context.shapes.memberShapes(tested), context.shapes.memberShapes(known));
}

static void checkSetTest(const PlacementCheckContext& context, LayoutSetID set, Vector<BitSetUse>& bitSets)
{
    LayoutID subject = firstMemberOf(context.program, set);
    if (!context.expect(context.shapes.hasTestOf(set), PlacementRule::MemberShapesWrong, subject))
        return;
    Vector<uint32_t, 16> expectedShapes;
    for (LayoutID member : context.program.at(set).written()) {
        if (context.slots.storageOf(member) != LayoutStorage::CompiledObject)
            continue;
        ShapeRange range = context.shapes.shapesOf(member);
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            expectedShapes.append(shape);
    }
    AOT::sort(expectedShapes);
    auto passing = context.shapes.memberShapes(set);
    context.expect(std::ranges::adjacent_find(expectedShapes) == expectedShapes.end() && std::ranges::equal(passing, expectedShapes), PlacementRule::MemberShapesWrong, subject);

    const ShapeTest& test = context.shapes.testOf(set);
    context.expect(holdsForAnyCell(test.form), PlacementRule::TestFormNeedsKnownShapes, subject);
    if (!context.expect(hasExtentOf(context.shapes, test, passing), PlacementRule::TestPassesOthers, subject))
        return;
    bool everyMemberPasses = std::ranges::all_of(passing, [&](uint32_t shape) {
        return passes(test, shape, passing);
    });
    context.expect(everyMemberPasses, PlacementRule::MemberFailsTest, subject);
    context.expect(numberOfPassingShapes(context.shapes, test, passing) == passing.size(), PlacementRule::TestPassesOthers, subject);
    if (test.bitSetOffset != noBitSet)
        checkBitsInTable(context, test, passing, subject, bitSets);
}

static void checkTestWithinKnownSet(const PlacementCheckContext& context, std::span<const ShapeTestWithinKnownSet> entries, size_t index)
{
    auto& entry = entries[index];
    if (!context.expect(isInterned(context.program, entry.tested) && isInterned(context.program, entry.known), PlacementRule::MemberShapesWrong))
        return;
    LayoutID subject = firstMemberOf(context.program, entry.tested);
    context.expect(!index || std::pair { entries[index - 1].tested, entries[index - 1].known } < std::pair { entry.tested, entry.known }, PlacementRule::MembersNotSorted, subject);
    PlacementRule failure = findKnownSetFailure(context, entry.test, entry.tested, entry.known);
    context.expect(failure == PlacementRule::None, failure, subject);
}

static void checkShapes(const PlacementCheckContext& context)
{
    if (!checkShapeNumbers(context))
        return;
    context.forEach(context.program.layouts.size(), checkShapeVariants);
    Vector<BitSetUse> uses = collectInParallel<BitSetUse>(context.loops, context.program.types.numberOfLayoutSets(), [&](unsigned i, Vector<BitSetUse>& bitSets) {
        checkSetTest(context, LayoutSetID(i), bitSets);
    });
    sortAndRemoveDuplicates(context.loops, uses);
    for (size_t i = 1; i < uses.size(); ++i)
        context.expect(static_cast<uint64_t>(uses[i - 1].firstWord) + uses[i - 1].numberOfWords <= uses[i].firstWord, PlacementRule::BitSetsOverlap);
    auto entries = context.shapes.testsWithinKnownSets();
    forEachIndexInParallel(context.loops, entries.size(), [&](unsigned i) {
        checkTestWithinKnownSet(context, entries, i);
    });
}

static SlotAccessKey keyOf(const FieldAccess& access)
{
    return { access.key, access.receivers, access.isStore };
}

static bool isCoverageValid(const Program& program, const FieldAccess& access, const SlotAgreement& agreement, std::span<const LayoutID> membersWithSlot)
{
    if (agreement.groups.isEmpty())
        return access.coverage == FieldAccess::Coverage::NoMember && !access.slot.hasStorage();
    if (!isInterned(program, access.membersWithField) || !std::ranges::equal(program.at(access.membersWithField).written(), membersWithSlot))
        return false;
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        return false;
    case FieldAccess::Coverage::SomeMembers:
        return !access.isStore && !agreement.coversEveryMember();
    case FieldAccess::Coverage::EveryMemberHasSlot:
        return !access.isStore && agreement.membersWithoutSlotKeepBytesZero;
    case FieldAccess::Coverage::EveryMember:
        return agreement.coversEveryMember();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAccessTestValid(const PlacementCheckContext& context, const FieldAccess& access)
{
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        return access.membersWithFieldTest.form == ShapeTestForm::NeverPasses;
    case FieldAccess::Coverage::EveryMemberHasSlot:
    case FieldAccess::Coverage::EveryMember:
        return access.membersWithFieldTest.form == ShapeTestForm::AlwaysPasses;
    case FieldAccess::Coverage::SomeMembers:
        return isInterned(context.program, access.membersWithField) && findKnownSetFailure(context, access.membersWithFieldTest, access.membersWithField, access.receivers) == PlacementRule::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool areContentsValid(const Program& program, const FieldAccess& access, std::span<const LayoutID> membersWithSlot)
{
    if (!access.contents)
        return false;
    const Type& contents = program.at(access.contents);
    bool maySeeEmptySlot = access.coverage != FieldAccess::Coverage::EveryMember || access.slot.isOutOfLine();
    for (LayoutID member : membersWithSlot) {
        FieldReference field { member, *program.at(member).findField(access.key) };
        const Type& held = program.at(slotTypeOf(program, fieldAt(program, declarationOf(program, field))));
        if (!(access.isStore ? contents.isSubsetOf(held, program) : held.isSubsetOf(contents, program)))
            return false;
        if (flowOf(program, field).occupancy != Occupancy::AlwaysFilled && canLeaveSlot(access.slot.representation))
            maySeeEmptySlot = true;
    }
    if (access.isStore)
        return true;
    if (access.slot.mayExistWithoutStorage && !(contents.primitives & Primitive::Undefined))
        return false;
    return !maySeeEmptySlot || contents.primitives & Primitive::Absent;
}

static bool hasStorageRuleOfMembers(const PlacementCheckContext& context, const FieldAccess& access, std::span<const LayoutID> membersWithSlot)
{
    if (membersWithSlot.empty())
        return !access.slot.mayExistWithoutStorage;
    auto mayExistWithoutStorage = [&](LayoutID member) {
        return context.slots.slot(member, *context.program.at(member).findField(access.key)).mayExistWithoutStorage;
    };
    bool expected = access.isStore ? std::ranges::all_of(membersWithSlot, mayExistWithoutStorage) : std::ranges::any_of(membersWithSlot, mayExistWithoutStorage);
    return access.slot.mayExistWithoutStorage == expected;
}

static void checkFieldAccess(const PlacementCheckContext& context, FieldAccessID id)
{
    const FieldAccess& access = context.placement.fieldAccess(id);
    if (!context.expect(isInterned(context.program, access.receivers), PlacementRule::AccessKey))
        return;
    auto expect = [&, subject = firstMemberOf(context.program, access.receivers)](bool holds, PlacementRule rule) {
        return context.expect(holds, rule, subject);
    };
    expect(!id.index() || keyOf(context.placement.fieldAccess(FieldAccessID(id.index() - 1))) < keyOf(access), PlacementRule::AccessKey);

    const SlotAgreement& agreement = context.slots.agreementOf(context.program, access.key, access.receivers);
    if (!expect(access.findsOffsetByShape == agreement.hasOffsetByShape(access.isStore) && (agreement.groups.size() <= 1 || access.findsOffsetByShape), PlacementRule::AccessLocationDiffers))
        return;
    std::span<const LayoutID> membersWithSlot;
    SlotGroup group;
    if (!agreement.groups.isEmpty()) {
        group = access.findsOffsetByShape ? agreement.groupOfAllOffsets() : agreement.groups[0];
        membersWithSlot = group.members.span();
        bool extensionWordsAgree = !group.slot.needsExtension() || access.extensionWord.hasLocationOf(group.extensionWord);
        expect(access.slot.hasLocationOf(group.slot) && extensionWordsAgree, PlacementRule::AccessLocationDiffers);
    }
    expect(hasStorageRuleOfMembers(context, access, membersWithSlot), PlacementRule::OutOfLineNotAllowed);
    expect(isCoverageValid(context.program, access, agreement, membersWithSlot), PlacementRule::CoverageWrong);
    expect(isAccessTestValid(context, access), PlacementRule::AccessTestWrong);
    expect(areContentsValid(context.program, access, membersWithSlot), PlacementRule::ContentsTooNarrow);
}

static bool isOrderWordValid(const Placement& placement, const PlannedLayout& planned, const PlacedAllocation& allocation, std::span<const FieldIndex> storedFields)
{
    uint32_t word = allocation.orderWord;
    if (planned.storage != LayoutStorage::CompiledObject)
        return !word;
    if (placement.shapes().shape(allocation.shape).keysAreNeverEnumerated)
        return word == unorderedOrderWord;
    if (storedFields.empty())
        return !word;
    auto orders = placement.keyOrders().of(allocation.layout);
    if (word & ~orderAndCountMask || countOf(word) != storedFields.size() || orderOf(word) >= orders.size())
        return false;
    return spanHasPrefix(orders[orderOf(word)].span(), storedFields);
}

// A store cannot tell whether it creates a field whose slot has no absent state, so the allocation lists the field.
static bool listsFieldsThatNoStoreLists(const PlacementCheckContext& context, const PlannedLayout& planned, const PlacedAllocation& allocation)
{
    if (planned.storage != LayoutStorage::CompiledObject || !context.program.at(allocation.layout).constructor || context.shapes.shape(allocation.shape).keysAreNeverEnumerated)
        return true;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].hasStorage() && !canLeaveSlot(planned.slots[field].representation) && !allocation.initialFields.contains(field))
            return false;
    }
    return true;
}

static void checkAllocation(const PlacementCheckContext& context, AllocationID id)
{
    const PlacedAllocation& allocation = context.placement.allocation(id);
    const PlannedLayout& planned = context.slots.layout(allocation.layout);
    auto expect = [&](bool holds, PlacementRule rule) {
        return context.expect(holds, rule, allocation.layout);
    };
    ShapeRange range = context.shapes.shapesOf(allocation.layout);
    if (!expect(!range.isEmpty(), PlacementRule::AllocationOfUnplacedLayout))
        return;
    bool isClosure = allocation.layout == context.placement.closureLayout();
    bool isSized = isClosure ? allocation.size >= Closure::allocationSize(allocation.numberOfCaptures) && allocation.initialFields.isEmpty() : allocation.size == planned.size;
    expect(isSized && !(allocation.size % 16), PlacementRule::AllocationSize);
    Vector<FieldIndex, 16> storedFields;
    for (FieldIndex field : allocation.initialFields) {
        if (!expect(field < planned.numberOfFields(), PlacementRule::InitialFieldWithoutStorage) || !planned.slots[field].hasStorage())
            continue;
        storedFields.append(field);
        expect(!planned.slots[field].isOutOfLine() || planned.slots[field].mayExistWithoutStorage, PlacementRule::InitialFieldWithoutStorage);
    }
    if (!expect(allocation.shape >= range.first && allocation.shape < range.end() && context.shapes.isAssigned(allocation.shape), PlacementRule::AllocationShape))
        return;
    expect(isOrderWordValid(context.placement, planned, allocation, storedFields.span()), PlacementRule::AllocationOrderWord);
    expect(listsFieldsThatNoStoreLists(context, planned, allocation), PlacementRule::AbsentStateMissing);
}

static void checkKeyOrders(const PlacementCheckContext& context, LayoutID id)
{
    const PlannedLayout& planned = context.slots.layout(id);
    auto orders = context.placement.keyOrders().of(id);
    context.expect(orders.size() <= maxKeyOrdersPerLayout && (!planned.keysAreNeverEnumerated || orders.empty()), PlacementRule::KeyOrderInvalid, id);
    for (size_t i = 0; i < orders.size(); ++i) {
        auto order = orders[i].span();
        Vector<FieldIndex, 16> sorted(order);
        AOT::sort(sorted);
        bool everyFieldHasStorage = std::ranges::all_of(order, [&](FieldIndex field) {
            return field < planned.numberOfFields() && planned.slots[field].hasStorage();
        });
        context.expect(std::ranges::adjacent_find(sorted) == sorted.end() && everyFieldHasStorage, PlacementRule::KeyOrderInvalid, id);
        context.expect(i + 1 == orders.size() || (std::ranges::lexicographical_compare(order, orders[i + 1]) && !spanHasPrefix(orders[i + 1].span(), order)), PlacementRule::KeyOrderInvalid, id);
    }
}

static bool isContinuationValid(const KeyOrders& keyOrders, const PlacedFieldCreation& creation, KeyOrderContinuation continuation)
{
    auto orders = keyOrders.of(creation.layout);
    auto keysBehind = [&](uint32_t orderWord) -> std::optional<std::span<const FieldIndex>> {
        if (orderWord & ~orderAndCountMask)
            return std::nullopt;
        if (!countOf(orderWord))
            return orderOf(orderWord) ? std::nullopt : std::optional(std::span<const FieldIndex> { });
        if (orderOf(orderWord) >= orders.size() || countOf(orderWord) > orders[orderOf(orderWord)].size())
            return std::nullopt;
        return orders[orderOf(orderWord)].span().first(countOf(orderWord));
    };
    auto before = keysBehind(continuation.orderWordBefore);
    auto after = keysBehind(continuation.orderWordAfter);
    if (!before || !after || after->size() != before->size() + creation.fields.size())
        return false;
    if (keyOrders.findOrderWord(creation.layout, *before) != continuation.orderWordBefore || keyOrders.findOrderWord(creation.layout, *after) != continuation.orderWordAfter)
        return false;
    return spanHasPrefix(*after, *before) && std::ranges::equal(after->subspan(before->size()), creation.fields);
}

static bool isEncodingAllowed(const Variable& variable, const PlacedVariable& placed, bool isDescribed)
{
    bool readsAsDeclaredFunction = variable.initialization == VariableInitialization::Function && placed.function == variable.function;
    switch (placed.storage) {
    case VariableStorage::None:
        return true;
    case VariableStorage::ClosureTable:
        return placed.encoding == VariableEncoding::FunctionAsZero && readsAsDeclaredFunction && variable.flow.stores == StoreCount::Never;
    case VariableStorage::Statics:
        break;
    }
    switch (placed.encoding) {
    case VariableEncoding::Plain:
        return variable.initialization != VariableInitialization::Function;
    case VariableEncoding::UndefinedAsZero:
        return variable.initialization == VariableInitialization::Undefined && placed.representation == SlotRepresentation::JSValue && !isDescribed;
    case VariableEncoding::FunctionAsZero:
        return readsAsDeclaredFunction;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasValidInitialWord(const Variable& variable, const PlacedVariable& placed)
{
    SlotTraits traits = traitsOf(placed.representation);
    bool isStoredBeforeEveryRead = variable.flow.initialization != InitializationState::Unknown;
    if (variable.owner && isStoredBeforeEveryRead && placed.encoding == VariableEncoding::Plain)
        return traits.hasAbsent && placed.initialWord == traits.absentBits;
    bool mayBeReadBeforeStore = placed.encoding == VariableEncoding::Plain && !isStoredBeforeEveryRead;
    if (!placed.initialWord && !mayBeReadBeforeStore)
        return true;
    switch (variable.initialization) {
    case VariableInitialization::Uninitialized:
        return traits.hasAbsent && placed.initialWord == traits.absentBits;
    case VariableInitialization::Undefined:
        return traits.hasUndefined && placed.initialWord == traits.undefinedBits;
    case VariableInitialization::Function:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkVariable(const PlacementCheckContext& context, VariableID id, bool isDescribed)
{
    const Variable& variable = context.program.at(id);
    const PlacedVariable& placed = context.placement.variable(id);
    context.expect(isEncodingAllowed(variable, placed, isDescribed), PlacementRule::EncodingNotAllowed, variable.owner);
    if (!placed.hasWordInStatics())
        return;
    bool isFilledAtProgramEntry = placed.region == StaticsRegion::FilledAtProgramEntry;
    bool mayBeFilledAtProgramEntry = placed.initialWord && !context.slots.variable(id).isUsedBeforeProgramEntry;
    context.expect(isFilledAtProgramEntry == (placed.offset < context.statics.sizeFilledAtProgramEntry) && (!isFilledAtProgramEntry || mayBeFilledAtProgramEntry), PlacementRule::VariableRegion, variable.owner);
    std::optional<uint32_t> group = context.statics.groupFilledWhenEntered(variable.module);
    uint32_t word = placed.offset / sizeOfStaticsWord;
    bool liesAmongWordsOfFiles = word - context.statics.firstWordFilledWithFiles < context.statics.endWordFilledWithFiles - context.statics.firstWordFilledWithFiles;
    bool liesAmongWordsOfItsFile = group && word - context.statics.groupsOfFilesFilledWhenEntered[*group].firstWord < context.statics.groupsOfFilesFilledWhenEntered[*group].numberOfWords;
    context.expect(liesAmongWordsOfFiles == (placed.region == StaticsRegion::FilledWithFile) && liesAmongWordsOfFiles == (liesAmongWordsOfItsFile && placed.initialWord), PlacementRule::VariableRegion, variable.owner);
    context.expect(hasValidInitialWord(variable, placed), PlacementRule::VariableLacksInitialValue, variable.owner);
    const Type& stored = context.program.at(context.program.provenTypeOf(variable.declared));
    context.expect(canHoldType(placed.representation, stored) && (!variable.owner || canLeaveSlot(placed.representation)), PlacementRule::RepresentationCannotHoldType, variable.owner);
}

static void checkEvalBindings(const PlacementCheckContext& context)
{
    const Type& anything = context.program.at(TypeTable::anything);
    for (const EvalScope& scope : context.program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!isAssignableBinding(binding.kind) || binding.kind == EvalBindingKind::MappedArgument)
                continue;
            if (binding.variable)
                context.expect(canHoldType(context.placement.variable(binding.variable).representation, anything), PlacementRule::RepresentationCannotHoldType);
            else if (binding.layout && binding.field)
                context.expect(canHoldType(context.slots.slot(binding.layout, *binding.field).representation, anything), PlacementRule::RepresentationCannotHoldType, binding.layout);
        }
    }
}

static void checkStaticsExtents(const PlacementCheckContext& context)
{
    Vector<Extent> extents;
    bool areAligned = true;
    auto add = [&](uint32_t offset, uint32_t width) {
        extents.append(extentAt(offset, width));
        areAligned &= !(offset % width);
    };
    for (auto& variable : context.statics.variables) {
        if (variable.hasWordInStatics())
            add(variable.offset, widthOf(variable.representation));
    }
    for (auto offset : context.statics.moduleValueOffsets) {
        if (offset)
            add(*offset, sizeOfStaticsWord);
    }
    for (auto& cell : context.statics.moduleBuiltinCells)
        add(cell.offset, sizeOfStaticsWord);
    for (auto offset : context.statics.moduleNamespaceOffsets) {
        if (offset)
            add(*offset, sizeOfStaticsWord);
    }
    for (auto offset : context.statics.freeCellOffsets) {
        if (offset)
            add(*offset, sizeOfStaticsWord);
    }
    if (!context.expect(areAligned && areDisjoint(extents.mutableSpan(), 0, context.statics.size), PlacementRule::VariableOverlap))
        return;
    Vector<uint32_t> guardWordOffsets = context.placement.guardBits().bits.map([](const PlacedGuardBit& bit) {
        return bit.wordOffset;
    });
    sortAndRemoveDuplicates(context.loops, guardWordOffsets);
    for (uint32_t offset : guardWordOffsets)
        add(offset, sizeOfStaticsWord);
    context.expect(areAligned && areDisjoint(extents.mutableSpan(), 0, context.statics.size), PlacementRule::GuardWordOverlap);
}

static bool isRoot(const StaticsPlan& statics, uint32_t offset)
{
    return std::ranges::binary_search(statics.roots, offset);
}

static bool isClearedEachCollection(const StaticsPlan& statics, uint64_t offset)
{
    return std::ranges::any_of(statics.clearedEachCollection, [&](const StaticsRange& range) {
        return offset >= range.offset && offset + sizeOfStaticsWord <= static_cast<uint64_t>(range.offset) + range.size;
    });
}

static void checkRoots(const PlacementCheckContext& context)
{
    auto roots = context.statics.roots.span();
    for (size_t i = 0; i < roots.size(); ++i)
        context.expect((!i || roots[i - 1] < roots[i]) && !(roots[i] % sizeOfStaticsWord) && static_cast<uint64_t>(roots[i]) + sizeOfStaticsWord <= context.statics.size, PlacementRule::RootMissing);
    for (auto& variable : context.statics.variables) {
        if (!variable.hasWordInStatics())
            continue;
        SlotTraits traits = traitsOf(variable.representation);
        context.expect(!traits.holdsCell || traits.isNarrowReference || isRoot(context.statics, variable.offset), PlacementRule::RootMissing);
        // A root is visited as a word of eight bytes, and a narrow reference is half of one.
        context.expect(!traits.isNarrowReference || !isRoot(context.statics, variable.offset & ~(sizeOfStaticsWord - 1)), PlacementRule::RootMissing);
    }
    for (auto offset : context.statics.moduleValueOffsets)
        context.expect(!offset || isRoot(context.statics, *offset), PlacementRule::RootMissing);
    for (auto& cell : context.statics.moduleBuiltinCells)
        context.expect(isRoot(context.statics, cell.offset), PlacementRule::RootMissing);
    for (auto offset : context.statics.moduleNamespaceOffsets)
        context.expect(!offset || isRoot(context.statics, *offset), PlacementRule::RootMissing);
}

static void checkClearedRanges(const PlacementCheckContext& context)
{
    for (auto& range : context.statics.clearedEachCollection) {
        uint64_t end = static_cast<uint64_t>(range.offset) + range.size;
        auto firstRoot = std::ranges::lower_bound(context.statics.roots, range.offset);
        bool containsRoot = firstRoot != context.statics.roots.end() && *firstRoot < end;
        context.expect(range.offset >= context.statics.sizeFilledAtProgramEntry && end <= context.statics.size && !containsRoot, PlacementRule::ClearedRangeIsFilledAtProgramEntry);
    }
    for (unsigned i = 0; i < context.statics.freeCellOffsets.size(); ++i) {
        auto offset = context.statics.freeCellOffsets[i];
        context.expect(!offset || isClearedEachCollection(context.statics, *offset), PlacementRule::ClearedRangeIsFilledAtProgramEntry, LayoutID(i));
    }
}

static void checkStatics(const PlacementCheckContext& context)
{
    if (!context.expect(context.statics.variables.size() == context.program.variables.size() && context.statics.moduleValueOffsets.size() == context.program.modules.size(), PlacementRule::VariableWithoutStorage))
        return;
    const BitVector described = findVariablesDescribedToRunTime(context.program, context.flow);
    forEachIndexInParallel(context.loops, context.program.variables.size(), [&](unsigned i) {
        checkVariable(context, VariableID(i), described.quickGet(i));
    });
    checkStaticsExtents(context);
    checkRoots(context);
    checkClearedRanges(context);
}

static std::tuple<LayoutID, ClassSide, MemberGuardKind, PropertyKeyID> keyOf(const PlacedGuardBit& bit)
{
    return { bit.classLayout, bit.side, bit.kind, bit.key };
}

static bool isValidBitOfSet(const Program& program, const GuardBitTable& table, size_t index)
{
    const PlacedGuardBit& bit = table.bits[index];
    GuardBitID set(index);
    std::span<const GuardBitID> members = table.membersOf(set);
    if (bit.classLayout || bit.key || members.size() < 2)
        return false;
    if (std::ranges::any_of(members, [&](GuardBitID member) { return member.index() >= table.numberOfClassBits; }) || bit.holdsWhileClear != setHoldsWhileClear(program, table, members))
        return false;
    bool areAscending = std::ranges::adjacent_find(members, [](GuardBitID a, GuardBitID b) { return !(a < b); }) == members.end();
    bool isListedUnderMembers = std::ranges::all_of(members, [&](GuardBitID member) {
        return std::ranges::binary_search(table.setsContaining(member), set);
    });
    bool comesBehindPreviousSet = index == table.numberOfClassBits || std::ranges::lexicographical_compare(table.membersOf(GuardBitID(index - 1)), members);
    return areAscending && isListedUnderMembers && comesBehindPreviousSet;
}

static bool dividesIntoGroups(std::span<const uint32_t> firstOfGroups, size_t numberOfGroups, size_t numberOfEntries)
{
    return firstOfGroups.size() == numberOfGroups + 1 && !firstOfGroups.front() && firstOfGroups.back() == numberOfEntries && std::ranges::is_sorted(firstOfGroups);
}

static void checkGuardBits(const PlacementCheckContext& context)
{
    const GuardBitTable& table = context.placement.guardBits();
    bool hasListsOfSets = table.numberOfClassBits <= table.bits.size() && table.membersOfSets.size() == table.setsOfClassBits.size()
        && dividesIntoGroups(table.firstMemberOfSet.span(), table.bits.size() - table.numberOfClassBits, table.membersOfSets.size())
        && dividesIntoGroups(table.firstSetOfClassBit.span(), table.numberOfClassBits, table.setsOfClassBits.size());
    if (!context.expect(hasListsOfSets, PlacementRule::GuardBitsNotDistinct))
        return;
    auto bits = table.bits.span();
    Vector<uint32_t> wordOffsets;
    uint64_t usedBits = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        auto& bit = bits[i];
        // The bits of one side of a class fill words that follow each other: the run time finds them from the first. The bits of sets fill word after word.
        bool isOnSideOfPrevious = i && bits[i - 1].classLayout == bit.classLayout && bits[i - 1].side == bit.side;
        bool startsWord = !isOnSideOfPrevious || !(bit.bit % MemberGuardData::bitsPerWord);
        bool followsWordsOfSide = !bit.classLayout || !bit.bit || (isOnSideOfPrevious && bit.bit == bits[i - 1].bit + 1 && (!startsWord || bit.wordOffset == bits[i - 1].wordOffset + sizeof(uint64_t)));
        if (startsWord) {
            wordOffsets.append(bit.wordOffset);
            usedBits = 0;
        }
        bool hasOwnBit = (bit.classLayout || bit.bit < MemberGuardData::bitsPerWord) && followsWordsOfSide && !(usedBits & bit.mask());
        bool hasWordOfNeighbors = bit.wordOffset && bit.wordOffset == wordOffsets.last();
        bool hasSenseOfClass = !bit.classLayout || bit.holdsWhileClear == context.slots.classCreation(bit.classLayout).guardsHoldWhileClear;
        bool isValidBitOfClass = bit.classLayout && (!i || keyOf(bits[i - 1]) < keyOf(bit)) && !!bit.key == (bit.kind == MemberGuardKind::Member || bit.kind == MemberGuardKind::NotHidden || bit.kind == MemberGuardKind::PassedByStores);
        bool isValid = hasSenseOfClass && (i < table.numberOfClassBits ? isValidBitOfClass : isValidBitOfSet(context.program, table, i));
        context.expect(hasOwnBit && hasWordOfNeighbors && isValid, PlacementRule::GuardBitsNotDistinct, bit.classLayout);
        usedBits |= bit.mask();
    }
    AOT::sort(wordOffsets);
    context.expect(std::ranges::adjacent_find(wordOffsets) == wordOffsets.end(), PlacementRule::GuardBitsNotDistinct);
}

static bool hasPlacedField(std::span<const PayloadFieldInfo> fields)
{
    return std::ranges::any_of(fields, [](const PayloadFieldInfo& field) {
        return field.firstForm == IRForm::Placed;
    });
}

static EntriesWithPlacedFields findEntriesWithPlacedFields()
{
    EntriesWithPlacedFields entries;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (hasPlacedField(payloadFieldsOf(opcode)))
            entries.opcodes.add(opcode);
    }
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        if (hasPlacedField(payloadFieldsOf(kind)))
            entries.terminators.add(kind);
    }
    return entries;
}

static SiteCellKind siteCellKindOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::NewArray:
        return SiteCellKind::ArrayProfile;
    case Opcode::NewRegExp:
    case Opcode::SharedRegExpLiteral:
        return SiteCellKind::RegExp;
    case Opcode::NewTemplateObject:
        return SiteCellKind::TemplateObject;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool slotAdmits(const Program& program, bool holdsUndefinedAndNull, const Type& contents, const Type& stored)
{
    if (!holdsUndefinedAndNull || !(stored.primitives & Primitive::UndefinedOrNull & ~contents.primitives))
        return stored.isSubsetOf(contents, program);
    return stored.isSubsetOf(contents.withPrimitives(Primitive::UndefinedOrNull), program);
}

namespace {

struct SiteCheck : PlacementCheckContext {
    bool expect(bool holds, PlacementRule rule, ValueID value = { }) const { return PlacementCheckContext::expect(holds, rule, { }, functionID, value); }

    void run()
    {
        for (const Block& block : function.blocks) {
            for (ValueID value : block.values)
                checkSite(value);
            expect(!placedFields.terminators.contains(block.terminator.kind), PlacementRule::SiteRefersElsewhere);
            forEachVariable(block.terminator, [&](VariableID variable) {
                expect(placement.variable(variable).hasWordInStatics(), PlacementRule::VariableWithoutStorage);
            });
        }
        AOT::sort(closuresStoredInDeferredSlots);
        for (ValueID closure : deferredClosures)
            expect(std::ranges::binary_search(closuresStoredInDeferredSlots, closure), PlacementRule::DeferredClosureStore, closure);
    }

    FunctionAnalyses& ensureAnalyses()
    {
        if (!analyses)
            analyses.emplace(AnalysisContext { build, program, &flow, nullptr, functionID }, function);
        return *analyses;
    }

    const FieldAccess* accessAt(FieldAccessID id) const
    {
        return id && id.index() < placement.numberOfFieldAccesses() ? &placement.fieldAccess(id) : nullptr;
    }

    const PlacedSlot* slotOf(LayoutID member, PropertyKeyID key) const
    {
        std::optional<uint32_t> field = program.at(member).findField(key);
        return field ? &slots.slot(member, *field) : nullptr;
    }

    void checkSite(ValueID id)
    {
        const Value& value = function.at(id);
        if (placedFields.opcodes.contains(value.opcode)) {
            for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
                if (field.firstForm == IRForm::Placed)
                    checkPlacedField(id, value, field.kind);
            }
        }
        if (Value::opcodesWithPlacedKeys.contains(value.opcode))
            checkPlacedKeys(id, value);
        if (Value::opcodesWithFieldAccess.contains(value.opcode) && isSlotStore(value.opcode))
            checkAccessStore(id, value.fieldAccess(), value.operand(*storedOf(value.opcode)));
        if (value.opcode == Opcode::NewObject)
            checkInitialValues(id, value);
        if (value.opcode == Opcode::NewArray)
            checkElements(id, value);
        if (value.opcode == Opcode::GetSlot)
            checkReadOfFilledSlot(id, value);
        if (value.opcode == Opcode::PutSlot)
            checkStoreWithoutAllocation(id, value);
        if (value.opcode == Opcode::PrivateDefine && slots.layout(value.layout()).hasSlots()) {
            // The run time throws where the private field exists already, and a slot without an absent state always looks filled.
            const PlacedSlot& slot = slots.slot(value.layout(), value.field());
            expect(!slot.hasStorage() || canLeaveSlot(slot.representation), PlacementRule::AbsentStateMissing, id);
        }
        if (value.opcode == Opcode::NewClosure)
            checkClosureForm(id, value);
        expect(value.opcode != Opcode::GetEncodedModuleVariable || placement.variable(value.variable()).encoding == VariableEncoding::UndefinedAsZero, PlacementRule::EncodingNotAllowed, id);
    }

    void expectWord(ValueID id, bool hasWord, bool needsWord, bool isRight, PlacementRule ruleOfWrongWord = PlacementRule::SiteRefersElsewhere)
    {
        if (hasWord)
            expect(isRight, ruleOfWrongWord, id);
        else
            expect(!needsWord, PlacementRule::PayloadMissing, id);
    }

    void checkPlacedField(ValueID id, const Value& value, PayloadKind kind)
    {
        switch (kind) {
        case PayloadKind::FieldAccess:
            return checkFieldAccess(id, value.fieldAccess(), { value.key(), value.receivers(), isSlotStore(value.opcode) });
        case PayloadKind::Allocation: {
            AllocationID allocation = value.allocation();
            bool allocatesByRecord = value.opcode != Opcode::NewClosure || value.closureForm() == ClosureForm::Full;
            return expectWord(id, !!allocation, allocatesByRecord, allocation && allocatesByRecord && allocation.index() < placement.numberOfAllocations() && isAllocationOf(placement.allocation(allocation), value));
        }
        case PayloadKind::RegExpRecord: {
            const RegExpRequests& requests = inputs.regExpRequests;
            RegExpRecordID record = value.regExpRecord();
            bool isRight = record && record.index() < requests.all().size() && requests.at(record).pattern == value.pattern() && requests.at(record).flags == ImageRegExp::significantFlags(toYarrFlags(value.regExpFlags()));
            return expectWord(id, !!record, true, isRight, PlacementRule::RegExpRecordWrong);
        }
        case PayloadKind::Request:
            return checkRequest(id, value);
        case PayloadKind::StaticsOffset:
            return checkStaticsOffset(id, value);
        case PayloadKind::OrderWord: {
            std::optional<uint32_t> expected = placement.keyOrders().findOrderWord(value.layout(), value.fields());
            std::optional<OrderWord> word = value.orderWord();
            return expectWord(id, !!word, !!expected, word && expected == word->value());
        }
        case PayloadKind::GuardBit: {
            GuardBitID expected = placement.guardBits().find(value.conditions());
            return expectWord(id, !!value.guardBit(), !!expected, value.guardBit() == expected);
        }
        case PayloadKind::GlobalBinding:
            return expectWord(id, !!value.binding(), true, true);
        case PayloadKind::SiteCache:
            return;
        case PayloadKind::String:
        case PayloadKind::PropertyKey:
        case PayloadKind::Function:
        case PayloadKind::Layout:
        case PayloadKind::Field:
        case PayloadKind::Variable:
        case PayloadKind::Module:
        case PayloadKind::ModuleFailure:
        case PayloadKind::Type:
        case PayloadKind::TypeSite:
        case PayloadKind::EvalScope:
        case PayloadKind::Data:
        case PayloadKind::ExpectedResult:
        case PayloadKind::Builtin:
        case PayloadKind::Enum:
        case PayloadKind::Flags:
        case PayloadKind::Bool:
        case PayloadKind::UInt32:
        case PayloadKind::Int64:
        case PayloadKind::Double:
        case PayloadKind::External:
        case PayloadKind::Record:
        case PayloadKind::Variant:
        case PayloadKind::List:
        case PayloadKind::LayoutSet:
        case PayloadKind::CalleeSet:
        case PayloadKind::AccessSite:
        case PayloadKind::AllocationSite:
        case PayloadKind::AllocationClass:
        case PayloadKind::Origin:
        case PayloadKind::Opcode:
            expect(false, PlacementRule::SiteRefersElsewhere, id);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void checkFieldAccess(ValueID id, FieldAccessID accessID, const SlotAccessKey& key)
    {
        const FieldAccess* access = accessAt(accessID);
        expectWord(id, !!accessID, true, access && keyOf(*access) == key);
    }

    void checkPlacedKeys(ValueID id, const Value& value)
    {
        PayloadListView<PlacedKey> placedKeys = value.placedKeys();
        std::span<const ValueID> values = value.valuesOperands();
        if (!expect(placedKeys.size() == values.size(), PlacementRule::SiteRefersElsewhere, id))
            return;
        for (size_t index = 0; index < placedKeys.size(); ++index) {
            PlacedKey placedKey = placedKeys[index];
            checkFieldAccess(id, placedKey.fieldAccess, { placedKey.key, value.receivers(), true });
            checkAccessStore(id, placedKey.fieldAccess, values[index]);
        }
    }

    bool isAllocationOf(const PlacedAllocation& allocation, const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::NewClosure:
            return allocation.layout == placement.closureLayout() && allocation.numberOfCaptures >= value.capturesOperands().size();
        case Opcode::NewObject:
            return allocation.layout == value.layout()
                && std::ranges::equal(allocation.initialFields, value.fields())
                && allocation.shape == shapes.shapeOf(value.layout(), value.allocationClass());
        case Opcode::MaterializeEnvironment:
            return allocation.layout == value.layout() && allocation.initialFields.isEmpty();
        default:
            return false;
        }
    }

    // A second cell would be a second function object with a prototype object of its own, and instances inherit from the one of the closure table.
    void checkClosureForm(ValueID id, const Value& closure)
    {
        ClosureForm form = closure.closureForm();
        expect(form == ClosureForm::Singleton || placement.closureIdentityOf(closure.function()) != ClosureIdentity::TableCell, PlacementRule::ClosureFormNotProven, id);
        if (form == ClosureForm::Deferred)
            deferredClosures.append(id);
        else
            expect((form == ClosureForm::Singleton) == namesClosureOfTable(program, closure), PlacementRule::ClosureFormNotProven, id);
    }

    bool isDataRequestOf(const DataRecord& record, const Value& value) const
    {
        if (record.data != value.data() || (record.sharedStorageOffset || record.node) != (value.encoding() == DataEncoding::CopyOnWriteArray))
            return false;
        if (!!record.cellOffset != (value.encoding() == DataEncoding::MadeOnce) || (record.cellOffset && !isRoot(statics, *record.cellOffset)))
            return false;
        if (record.node)
            return !record.sharedStorageOffset && isStorageOfArray(program.at(record.data).root.as<DataNode::Array>());
        return !record.sharedStorageOffset || isRoot(statics, *record.sharedStorageOffset);
    }

    bool isRequestOf(const RequestRecord& request, const Value& value) const
    {
        Opcode opcode = value.opcode;
        return WTF::switchOn(request,
            [&](const NewClassRecord& record) { return opcode == Opcode::NewClass && record.classLayout == value.layout(); },
            [&](const TemplateObjectRecord& record) { return opcode == Opcode::NewTemplateObject && std::ranges::equal(record.parts, value.templateStrings()); },
            [&](const DirectEvalRecord& record) { return opcode == Opcode::DirectEval && record.scope == value.evalScope() && record.module == program.sourceFunctionOf(function, value).module; },
            [&](const DataRecord& record) { return opcode == Opcode::Data && isDataRequestOf(record, value); },
            [&](const CollectionDataRecord& record) { return (opcode == Opcode::NewSetFromData || opcode == Opcode::NewMapFromData) && record.data == value.data() && record.hasValues == (opcode == Opcode::NewMapFromData); });
    }

    void checkRequest(ValueID id, const Value& value)
    {
        std::optional<Request> request = value.request();
        if (namesPropertyRun(value.opcode)) {
            std::span<const PlacedPropertyRun> runs = placement.propertyRuns().runs();
            expect(!request || (request->value() < runs.size() && std::ranges::equal(runs[request->value()].keys, keysOfPropertyRun(value))), PlacementRule::PropertyRunWrong, id);
            return;
        }
        const RequestRecord* record = request ? placement.requests().recordAt(*request) : nullptr;
        expectWord(id, !!request, true, record && isRequestOf(*record, value), PlacementRule::RequestWrong);
    }

    bool isSiteCell(uint32_t offset, SiteCellKind kind) const
    {
        if (offset % sizeOfStaticsWord || offset >= statics.size)
            return false;
        switch (kind) {
        case SiteCellKind::ArrayProfile:
            return !isRoot(statics, offset) && isClearedEachCollection(statics, static_cast<uint64_t>(offset) + statics.distanceFromArrayProfileToLastArray);
        case SiteCellKind::RegExp:
        case SiteCellKind::TemplateObject:
            return isRoot(statics, offset);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void checkStaticsOffset(ValueID id, const Value& value)
    {
        SiteCellKind kind = siteCellKindOf(value.opcode);
        bool hasCell = kind != SiteCellKind::ArrayProfile || value.literalStorage() == ArrayLiteralStorage::Profiled;
        std::optional<StaticsOffset> offset = value.staticsOffset();
        bool isRight = offset && hasCell && isSiteCell(offset->value(), kind);
        expectWord(id, !!offset, hasCell, isRight);
        if (isRight)
            siteCells.append({ offset->value(), kind, sourceSiteOf(program, functionID, value), { functionID, id }, kind == SiteCellKind::RegExp ? value.regExpRecord() : RegExpRecordID() });
    }

    bool isUsedOnlyByStore(ValueID stored, ValueID closure)
    {
        const UseLists& uses = ensureAnalyses().useLists();
        for (ValueID id = stored; id != closure; id = function.at(id).operand(0)) {
            if (uses.useCount(id) != 1)
                return false;
        }
        return uses.useCount(closure) == 1;
    }

    void checkStoredValue(ValueID site, ValueID stored, SlotRepresentation representation, bool holdsUndefinedAndNull, const Type& contents, const Invocable<bool(FunctionID)> auto& isDeferredFunction)
    {
        expect(slotAdmits(program, holdsUndefinedAndNull, contents, program.typeOf(function, stored)), PlacementRule::StoredTypeTooWide, site);
        if (representation != SlotRepresentation::DeferredClosure)
            return;

        ValueID definition = definitionThroughUpcasts(function, stored);
        const Value& closure = function.at(definition);
        if (closure.opcode != Opcode::NewClosure || closure.closureForm() != ClosureForm::Deferred)
            expect(!(program.typeOf(function, stored).primitives & ~Primitive::UndefinedOrNull), PlacementRule::DeferredClosureStore, site);
        else if (expect(isDeferredFunction(closure.function()) && isUsedOnlyByStore(stored, definition), PlacementRule::DeferredClosureStore, site))
            closuresStoredInDeferredSlots.append(definition);
    }

    void checkAccessStore(ValueID id, FieldAccessID accessID, ValueID stored)
    {
        const FieldAccess* access = accessAt(accessID);
        if (!access || !access->slot.hasStorage())
            return;
        bool holdUndefinedAndNull = true;
        for (LayoutID member : program.at(access->membersWithField).completeMembers()) {
            std::optional<uint32_t> field = program.at(member).findField(access->key);
            holdUndefinedAndNull &= field && slotHoldsUndefinedAndNull(program, { member, *field });
            expect(!field || !fieldAt(program, { member, *field }).form.contains(FieldForm::IsAccessor), PlacementRule::ValueInAccessorSlot, id);
        }
        checkStoredValue(id, stored, access->slot.representation, holdUndefinedAndNull, program.at(access->contents), [&](FunctionID deferred) {
            return std::ranges::all_of(program.at(access->membersWithField).completeMembers(), [&](LayoutID member) {
                const PlacedSlot* slot = slotOf(member, access->key);
                return slot && slot->deferredFunction == deferred;
            });
        });
    }

    void checkInitialValues(ValueID id, const Value& value)
    {
        LayoutID layout = value.layout();
        std::span<const uint32_t> fields = value.fields();
        std::span<const ValueID> values = value.valuesOperands();
        if (!expect(fields.size() == values.size(), PlacementRule::SiteRefersElsewhere, id))
            return;
        size_t numberOfFields = program.at(layout).fields.size();
        bool listsClosureOverEnvironment = false;
        for (size_t index = 0; index < fields.size(); ++index) {
            const PlacedSlot& slot = slots.slot(layout, fields[index]);
            if (fields[index] >= numberOfFields || !slot.hasStorage())
                continue;
            const Field& declaration = fieldAt(program, declarationOf(program, { layout, fields[index] }));
            expect(!values[index] || !declaration.form.contains(FieldForm::IsAccessor), PlacementRule::ValueInAccessorSlot, id);
            checkStoredValue(id, values[index], slot.representation, slotHoldsUndefinedAndNull(program, { layout, fields[index] }), program.at(slotTypeOf(program, declaration)), [&](FunctionID deferred) {
                return slot.deferredFunction == deferred;
            });
            listsClosureOverEnvironment |= slot.deferredClosureCapturesEnvironment;
        }
        std::optional<uint32_t> environmentField = program.at(layout).flow.closureEnvironmentField;
        expect(!listsClosureOverEnvironment || (environmentField && std::ranges::find(fields, *environmentField) != fields.end()), PlacementRule::DeferredClosureStore, id);
    }

    void checkElements(ValueID id, const Value& literal)
    {
        if (!holdsOnlyNumbers(literal.arrayStorage()))
            return;
        const Type& contents = program.at(literal.contents());
        expect(literal.contentsAreProven() && canStoreWithoutConversion(literal.arrayStorage(), contents), PlacementRule::StoredTypeTooWide, id);
        for (ValueID element : literal.elementsOperands())
            expect(program.typeOf(function, element).isSubsetOf(contents, program), PlacementRule::StoredTypeTooWide, id);
    }

    void checkReadOfFilledSlot(ValueID id, const Value& value)
    {
        const FieldAccess* access = accessAt(value.fieldAccess());
        if (!access)
            return;
        // A load from an Extension that is missing or too short answers absent, and code generation reads that as undefined.
        bool mayReadAbsent = !access->slot.isOutOfLine() && (program.at(access->contents).primitives & Primitive::Absent);
        if (access->coverage == FieldAccess::Coverage::EveryMember && !mayReadAbsent)
            return;
        bool isAlwaysFilled = std::ranges::all_of(program.at(access->receivers).completeMembers(), [&](LayoutID member) {
            std::optional<uint32_t> field = program.at(member).findField(access->key);
            return field && flowOf(program, { member, *field }).occupancy == Occupancy::AlwaysFilled;
        });
        expect(!isAlwaysFilled, PlacementRule::ContentsTooNarrow, id);
    }

    // PutSlot declares no collection. A filled slot has storage unless the object may list the field without it: the row says so, and the store then gets the granule without a collection.
    void checkStoreWithoutAllocation(ValueID id, const Value& value)
    {
        const FieldAccess* access = accessAt(value.fieldAccess());
        if (!access || !access->slot.hasStorage())
            return;
        bool mayLackStorage = std::ranges::any_of(program.at(access->membersWithField).completeMembers(), [&](LayoutID member) {
            const PlacedSlot* slot = slotOf(member, access->key);
            return slot && slot->mayExistWithoutStorage;
        });
        expect(!access->slot.isOutOfLine() || (access->mayBeFilledWithoutStorage == mayLackStorage && guardProvesSlotFilled(ensureAnalyses(), value)), PlacementRule::OutOfLineNotAllowed, id);
    }

    const EntriesWithPlacedFields& placedFields;
    FunctionID functionID;
    Vector<SiteCellUse>& siteCells;
    const Function& function { program.at(functionID) };
    std::optional<FunctionAnalyses> analyses { };
    Vector<ValueID> closuresStoredInDeferredSlots { };
    Vector<ValueID> deferredClosures { };
};

} // anonymous namespace

static void checkSiteCells(const PlacementCheckContext& context, Vector<SiteCellUse>&& uses)
{
    auto cellOf = [](const SiteCellUse& use) {
        return std::tuple { use.kind, use.site };
    };
    auto expect = [&](bool holds, PlacementRule rule, const SiteCellUse& use) {
        context.expect(holds, rule, { }, use.user.function, use.user.value);
    };
    auto templateStringsOf = [&](const SiteCellUse& use) {
        return context.program.at(use.user.function).at(use.user.value).templateStrings();
    };

    parallelSort(context.loops, uses.mutableSpan(), [&](const SiteCellUse& a, const SiteCellUse& b) {
        return std::tuple { a.offset, cellOf(a), a.user } < std::tuple { b.offset, cellOf(b), b.user };
    });
    for (size_t index = 1; index < uses.size(); ++index)
        expect(uses[index].offset != uses[index - 1].offset || cellOf(uses[index]) == cellOf(uses[index - 1]), PlacementRule::SiteRefersElsewhere, uses[index]);

    parallelSort(context.loops, uses.mutableSpan(), [&](const SiteCellUse& a, const SiteCellUse& b) {
        return std::tuple { cellOf(a), a.offset, a.user } < std::tuple { cellOf(b), b.offset, b.user };
    });
    for (size_t index = 1; index < uses.size(); ++index) {
        if (cellOf(uses[index]) != cellOf(uses[index - 1]))
            continue;
        expect(uses[index].offset == uses[index - 1].offset, PlacementRule::SiteRefersElsewhere, uses[index]);
        expect(uses[index].regExpRecord == uses[index - 1].regExpRecord, PlacementRule::RegExpRecordWrong, uses[index]);
        expect(uses[index].kind != SiteCellKind::TemplateObject || std::ranges::equal(templateStringsOf(uses[index]), templateStringsOf(uses[index - 1])), PlacementRule::SiteRefersElsewhere, uses[index]);
    }
}

static void checkDataObject(const PlacementCheckContext& context, const DataNode::Object& object)
{
    if (!object.layout || context.slots.storageOf(object.layout) != LayoutStorage::CompiledObject)
        return;
    Vector<FieldIndex, 16> fields;
    for (const DataMember& member : object.members) {
        if (std::optional<uint32_t> field = context.program.at(object.layout).findField(member.key))
            fields.append(*field);
    }
    ShapeRange range = context.shapes.shapesOf(object.layout);
    bool hasAllocation = false;
    for (uint32_t shape = range.first; shape < range.end(); ++shape)
        hasAllocation |= !!context.placement.allocations().find(object.layout, shape, fields.span());
    context.expect(fitsReadOnlyEncoding(context.program, object) && hasAllocation, PlacementRule::DataNotPlaced, object.layout);
}

static void checkDataTree(const PlacementCheckContext& context, const DataNode& root)
{
    Vector<const DataNode*, 16> worklist { &root };
    while (!worklist.isEmpty()) {
        const DataNode& node = *worklist.takeLast();
        switch (node.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
            continue;
        case DataNode::Kind::Array:
            for (const DataNode& element : node.as<DataNode::Array>().elements)
                worklist.append(&element);
            continue;
        case DataNode::Kind::Object:
            checkDataObject(context, node.as<DataNode::Object>());
            for (const DataMember& member : node.as<DataNode::Object>().members)
                worklist.append(&member.value);
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static void checkSites(const PlacementCheckContext& context)
{
    EntriesWithPlacedFields placedFields = findEntriesWithPlacedFields();
    Vector<SiteCellUse> siteCells = collectInParallel<SiteCellUse>(context.loops, context.program.functions.size(), [&](unsigned index, Vector<SiteCellUse>& found) {
        if (context.program.functions[index].hasBody())
            SiteCheck { context, placedFields, FunctionID(index), found }.run();
    });
    checkSiteCells(context, WTF::move(siteCells));

    std::span<const RequestRecord> records = context.placement.requests().records();
    forEachIndexInParallel(context.loops, records.size(), [&](unsigned index) {
        DataID data = WTF::switchOn(records[index],
            [](const NewClassRecord&) { return DataID(); },
            [](const TemplateObjectRecord&) { return DataID(); },
            [](const DirectEvalRecord&) { return DataID(); },
            [](const DataRecord& record) { return record.data; },
            [](const CollectionDataRecord& record) { return record.data; });
        if (data)
            checkDataTree(context, context.program.at(data).root);
    });
}

static void collectSiteCacheUses(const PlacementCheckContext& context, FunctionID id, Vector<SiteCacheUse>& uses)
{
    const Function& function = context.program.at(id);
    std::optional<FunctionAnalyses> analyses;
    std::optional<FunctionSiteWeights> weights;
    auto ensureAnalyses = [&]() -> FunctionAnalyses& {
        if (!analyses)
            analyses.emplace(AnalysisContext { context.build, context.program, &context.flow, nullptr, id }, function);
        return *analyses;
    };
    auto weightOf = [&](BlockID block) {
        if (!weights)
            weights.emplace(context.program, context.inputs.heat, id, ensureAnalyses());
        return weights->weightOf(block);
    };

    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID valueID : function.at(block).values) {
            const Value& value = function.at(valueID);
            if (!Value::opcodesWithSiteCache.contains(value.opcode))
                continue;
            unsigned numberUsed = numberOfSiteCachesUsedBy(context.program, context.placement, function, value);
            if (numberUsed && !mayReadSiteCachesAgain(ensureAnalyses(), block))
                numberUsed = 0;
            unsigned numberSet = 0;
            forEachSiteCache(value, [&](SiteCacheID cache) {
                ++numberSet;
                uses.append({ cache, weightOf(block), { id, valueID } });
            });
            context.expect(numberSet <= numberUsed, PlacementRule::SiteCacheNotUsed, { }, id, valueID);
            for (unsigned number = numberSet; number < numberUsed; ++number)
                uses.append({ SiteCacheID(), weightOf(block), { id, valueID } });
        }
    }
}

static void checkSiteCaches(const PlacementCheckContext& context)
{
    Vector<SiteCacheUse> uses = collectInParallel<SiteCacheUse>(context.loops, context.program.functions.size(), [&](unsigned index, Vector<SiteCacheUse>& found) {
        collectSiteCacheUses(context, FunctionID(index), found);
    });
    parallelSort(context.loops, uses.mutableSpan(), [](const SiteCacheUse& a, const SiteCacheUse& b) {
        return std::tuple { a.cache, a.user } < std::tuple { b.cache, b.user };
    });
    auto expect = [&](bool holds, PlacementRule rule, const SiteCacheUse& use) {
        context.expect(holds, rule, { }, use.user.function, use.user.value);
    };
    auto positionOf = [&](const SiteCacheUse& use) {
        return siteCachePositionOf(context.program, use.user.function, use.user.value);
    };

    size_t numberOfPlacedCaches = std::ranges::partition_point(uses, [](const SiteCacheUse& use) {
        return !!use.cache;
    }) - uses.begin();
    std::span<const SiteCacheUse> placed = uses.span().first(numberOfPlacedCaches);
    std::span<const SiteCacheUse> sitesWithoutCache = uses.span().subspan(numberOfPlacedCaches);

    for (size_t index = 0; index < placed.size(); ++index) {
        expect(placed[index].cache.index() == (index ? placed[index - 1].cache.index() + 1 : 0), PlacementRule::SiteCacheNotDistinct, placed[index]);
        expect(!index || !(positionOf(placed[index]) < positionOf(placed[index - 1])), PlacementRule::SiteCacheOrder, placed[index]);
    }
    context.expect(placed.size() == context.siteCaches.count(), PlacementRule::SiteCacheNotDistinct);

    if (sitesWithoutCache.empty())
        return;
    expect(context.siteCaches.count() == context.build.limit(LimitID::MaxSiteCaches), PlacementRule::SiteCacheOrder, sitesWithoutCache.front());
    SiteWeight lowestPlacedWeight = placed.empty() ? 0 : std::ranges::min(placed, { }, [](const SiteCacheUse& use) { return use.weight.predicted(); }).weight;
    for (const SiteCacheUse& use : sitesWithoutCache)
        expect(placed.empty() || (use.weight <= lowestPlacedWeight).predicted(), PlacementRule::SiteCacheOrder, use);
}

void checkPlacement(const Program& program, const PlacementInputs& inputs, const Placement& placement, const SiteCaches& siteCaches, BuildContext& build)
{
    if (build.diagnostics().hasErrors())
        return;
    std::atomic<unsigned> failures { 0 };
    PlacementCheckContext context { program, inputs, inputs.flow, inputs.slots, placement, placement.shapes(), placement.statics(), siteCaches, build, build.parallelLoops(), failures };
    context.forEach(program.layouts.size(), checkPlannedLayout);
    context.forEach(program.layouts.size(), checkClass);
    context.forEach(program.functions.size(), checkDefinedMethods);
    context.forEach(program.functions.size(), checkClosureForm);
    checkShapes(context);
    context.forEach(placement.numberOfFieldAccesses(), checkFieldAccess);
    context.forEach(placement.numberOfAllocations(), checkAllocation);
    context.forEach(program.layouts.size(), checkKeyOrders);
    for (const PlacedFieldCreation& creation : placement.keyOrders().fieldCreations) {
        bool isValid = !creation.fields.isEmpty() && std::ranges::all_of(creation.continuations, [&](KeyOrderContinuation continuation) {
            return isContinuationValid(placement.keyOrders(), creation, continuation);
        });
        context.expect(isValid, PlacementRule::KeyOrderInvalid, creation.layout);
    }
    checkStatics(context);
    checkEvalBindings(context);
    checkGuardBits(context);
    if (failures.load(std::memory_order_relaxed))
        return;
    checkSites(context);
    if (!failures.load(std::memory_order_relaxed))
        checkSiteCaches(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
