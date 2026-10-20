/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementCheck.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFunctionKinds.h"
#include "AOTRepresentation.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSlotPlanSteps.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

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

} // anonymous namespace

#if USE(BIGINT32)
static constexpr uint16_t primitivesAlwaysInCells = cellPrimitives & ~Primitive::BigInt;
#else
static constexpr uint16_t primitivesAlwaysInCells = cellPrimitives;
#endif

static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;
static constexpr uint16_t primitivesWithoutMarkers = Primitive::Defined & ~markerPrimitives;

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
static_assert(!representsAbsentAsZero(SlotRepresentation::DeferredClosure));
static_assert(traitsOf(SlotRepresentation::DeferredClosure).isNarrowReference);

static uint16_t markerPrimitivesOf(const SlotTraits& traits)
{
    uint16_t primitives = 0;
    if (traits.hasAbsent)
        primitives |= Primitive::Absent;
    if (traits.hasUndefined)
        primitives |= Primitive::Undefined;
    if (traits.hasNull)
        primitives |= Primitive::Null;
    return primitives;
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
    if (type.primitives & ~(storable.primitives | markerPrimitivesOf(traits)))
        return false;
    if (!type.integers.isSubsetOf(storable.integers))
        return false;
    if (type.opaque && !isStorable(*type.opaque, storable))
        return false;
    return storable.hasObjects || !(type.hasObjects() || type.environment);
}

bool canHold(const Program&, SlotRepresentation representation, const Type& type)
{
    SlotTraits traits = traitsOf(representation);
    switch (representation) {
    case SlotRepresentation::JSValue:
        return isStorable(type, traits, { .primitives = primitivesWithoutMarkers, .integers = Range::full(), .hasObjects = true });
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        return isStorable(type, traits, { .primitives = primitivesAlwaysInCells, .hasObjects = true });
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
    case SlotRepresentation::Int32OrDouble:
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

static LayoutStorage storageRequiredBy(const Layout& layout)
{
    bool mayBeAllocated = layout.flow.allocation != AllocationState::NeverAllocated;
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

class PlannedLayoutChecker {
public:
    PlannedLayoutChecker(const PlacementCheckContext&, LayoutID);

    void run();

private:
    void fail(PlacementRule rule) { m_context.failures.fail(rule, m_id); }
    bool isCompiledObject() const { return m_planned.storage == LayoutStorage::CompiledObject; }

    bool hasRequiredStorage() const;
    void checkBounds(const PlacedSlot&);
    void checkCellExtents();
    void checkExtensionExtents();
    void checkHiddenWords();
    void checkFieldRecords();
    void checkField(FieldIndex);
    bool canOmitAbsentState(const FieldFlow&) const;
    bool isCreatedWithoutKeys() const;
    bool isStorageAllowed(const PlacedSlot&, const Type&) const;
    bool hasRequiredWidth(FieldIndex, const PlacedSlot&) const;
    bool isDeferredClosureProven(const PlacedSlot&, const Field& declaration) const;

    const PlacementCheckContext& m_context;
    const Program& m_program;
    LayoutID m_id;
    const Layout& m_layout;
    const PlannedLayout& m_planned;
    bool m_usesWideReferences;
    bool m_isCoroutineFrame;
    bool m_keepsFieldsInCell;
    bool m_cannotReachUnknownJavaScript;
};

PlannedLayoutChecker::PlannedLayoutChecker(const PlacementCheckContext& context, LayoutID id)
    : m_context(context)
    , m_program(context.program)
    , m_id(id)
    , m_layout(context.program.at(id))
    , m_planned(context.inputs.slots.layout(id))
    , m_usesWideReferences(context.build.recordedOptions().useWideReferences())
    , m_isCoroutineFrame(m_layout.kind == LayoutKind::CoroutineFrame)
    , m_keepsFieldsInCell(m_isCoroutineFrame || context.inputs.frameAllocation.mayBeInFrame(id))
    , m_cannotReachUnknownJavaScript(everyAllocationClassCannot(context.program, context.inputs.flow, id, ObjectMutation::ReachUnknownJavaScript))
{
}

void PlannedLayoutChecker::run()
{
    if (!hasRequiredStorage()) {
        fail(PlacementRule::LayoutStorageWrong);
        return;
    }
    if (!m_planned.hasSlots())
        return;

    for (const PlacedSlot& slot : m_planned.slots) {
        if (slot.hasStorage())
            checkBounds(slot);
    }
    checkCellExtents();
    checkExtensionExtents();
    checkHiddenWords();
    checkFieldRecords();
    for (FieldIndex index = 0; index < m_planned.numberOfFields(); ++index) {
        if (m_planned.slots[index].hasStorage())
            checkField(index);
    }
    if (m_planned.keysAreNeverEnumerated && !keysAreNeverEnumerated(m_program, m_context.inputs.flow, m_id))
        fail(PlacementRule::ShapeVariantWrong);
}

bool PlannedLayoutChecker::hasRequiredStorage() const
{
    size_t numberOfRows = m_planned.hasSlots() ? m_layout.fields.size() + numberOfHiddenWords : 0;
    return m_planned.storage == storageRequiredBy(m_layout) && m_planned.slots.size() == numberOfRows;
}

void PlannedLayoutChecker::checkBounds(const PlacedSlot& slot)
{
    if (slot.offset % slot.width() || (slot.doubleOffsetInExtension && *slot.doubleOffsetInExtension % sizeOfOverflowArea))
        fail(PlacementRule::SlotAlignment);
    if (extentAt(slot.offset, slot.width()).end > (slot.isOutOfLine() ? m_planned.extensionSize : m_planned.size))
        fail(PlacementRule::SlotPastCellEnd);
}

void PlannedLayoutChecker::checkCellExtents()
{
    Vector<Extent, 32> extents;
    for (const PlacedSlot& slot : m_planned.slots) {
        if (slot.storage == SlotStorage::Inline)
            extents.append(extentAt(slot.offset, slot.width()));
    }
    if (auto offset = m_planned.isEscapedOffset)
        extents.append(extentAt(*offset, sizeof(bool)));
    uint64_t start = isCompiledObject() ? offsetOfFirstField : offsetOfFirstEnvironmentVariable;
    if (!areDisjoint(extents.mutableSpan(), start, m_planned.size)) {
        fail(PlacementRule::CellOverlap);
        return;
    }

    for (const ReservedBytes& reserved : m_planned.bytesKeptZero)
        extents.append(extentAt(reserved.offset, reserved.width));
    if (!areDisjoint(extents.mutableSpan(), start, m_planned.size))
        fail(PlacementRule::BytesKeptZeroNotFree);
}

void PlannedLayoutChecker::checkExtensionExtents()
{
    Vector<Extent, 8> extents;
    for (const PlacedSlot& slot : m_planned.slots) {
        if (slot.isOutOfLine())
            extents.append(extentAt(slot.offset, slot.width()));
        if (slot.hasStorage() && slot.doubleOffsetInExtension)
            extents.append(extentAt(*slot.doubleOffsetInExtension, sizeOfOverflowArea));
    }
    if (!(extents.isEmpty() ? !m_planned.extensionSize : areDisjoint(extents.mutableSpan(), offsetOfFirstExtensionSlot, m_planned.extensionSize)))
        fail(PlacementRule::ExtensionOverlap);
    if (m_planned.extensionSize > maxExtensionSize)
        fail(PlacementRule::SlotPastCellEnd);
}

void PlannedLayoutChecker::checkHiddenWords()
{
    constexpr OptionSet<ObjectMutation> mutationsCreatingExtraProperties { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::StoreOfOtherType };
    bool needsExtension = std::ranges::any_of(m_planned.slots, [](const PlacedSlot& slot) {
        return slot.hasStorage() && slot.needsExtension();
    });
    bool needsExtraPropertiesWord = isCompiledObject() && someAllocationClassMay(m_program, m_context.inputs.flow, m_id, mutationsCreatingExtraProperties);
    if (needsExtension != m_planned.hasExtension() || (needsExtraPropertiesWord && !m_planned.hasExtraPropertiesWord()))
        fail(PlacementRule::HiddenWordMissing);

    SlotRepresentation representation = hiddenReferenceRepresentation(m_usesWideReferences);
    for (const PlacedSlot& word : m_planned.slots.span().last(numberOfHiddenWords)) {
        if (word.hasStorage() && (word.storage != SlotStorage::Inline || word.representation != representation))
            fail(PlacementRule::HiddenWordMissing);
    }
}

void PlannedLayoutChecker::checkFieldRecords()
{
    uint32_t numberOfRecords = 0;
    bool areValid = true;
    for (FieldIndex index = 0; index < m_planned.slots.size(); ++index) {
        const PlacedSlot& slot = m_planned.slots[index];
        std::optional<uint32_t> expected;
        if (isCompiledObject() && index < m_planned.numberOfFields() && slot.hasStorage())
            expected = numberOfRecords++;
        if (slot.indexInLayoutRecord != expected)
            areValid = false;
    }
    if (!areValid || m_planned.numberOfRecordFields != numberOfRecords)
        fail(PlacementRule::FieldRecords);
}

void PlannedLayoutChecker::checkField(FieldIndex index)
{
    const PlacedSlot& slot = m_planned.slots[index];
    const Field& declaration = fieldAt(m_program, declarationOf(m_program, { m_id, index }));
    const Type& type = m_program.at(declaration.declared.type);

    if (!canHold(m_program, slot.representation, type))
        fail(PlacementRule::RepresentationCannotHoldType);
    if (!canLeaveSlot(slot.representation) && slot.representation != SlotRepresentation::DeferredClosure && !canOmitAbsentState(declaration.flow))
        fail(PlacementRule::AbsentStateMissing);
    if (!isStorageAllowed(slot, type))
        fail(PlacementRule::OutOfLineNotAllowed);
    if (!hasRequiredWidth(index, slot))
        fail(PlacementRule::WideReferenceMissing);
    bool needsDoubleArea = slot.representation == SlotRepresentation::Int32OrDouble;
    if (!isDeferredClosureProven(slot, declaration) || needsDoubleArea != slot.doubleOffsetInExtension.has_value())
        fail(PlacementRule::DeferredClosureNotProven);
}

// The run time allocates the object of a subclass and of a function called with new, and the stores of the constructor record the keys.
bool PlannedLayoutChecker::isCreatedWithoutKeys() const
{
    if (!m_layout.constructor || m_planned.keysAreNeverEnumerated)
        return false;
    return m_program.at(m_layout.constructor).kind != FunctionKind::BaseConstructor;
}

bool PlannedLayoutChecker::canOmitAbsentState(const FieldFlow& flow) const
{
    constexpr OptionSet<FieldStability> everyStability { FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };
    bool isAlwaysFilled = flow.occupancy == Occupancy::AlwaysFilled;
    switch (m_layout.kind) {
    case LayoutKind::CoroutineFrame:
        return true;
    case LayoutKind::Environment:
        return flow.location.initialization != InitializationState::Unknown && flow.stability.containsAll(everyStability);
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return isAlwaysFilled && flow.stability.containsAll(everyStability) && m_cannotReachUnknownJavaScript && !isCreatedWithoutKeys();
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PlannedLayoutChecker::isStorageAllowed(const PlacedSlot& slot, const Type& type) const
{
    if (!slot.isOutOfLine())
        return !slot.mayExistWithoutStorage;
    if (m_keepsFieldsInCell || !representsAbsentAsZero(slot.representation))
        return false;
    if (!slot.mayExistWithoutStorage)
        return true;
    return (type.primitives & Primitive::Undefined) && !mayHaveUnorderedShape(m_program, m_context.inputs.flow, m_id);
}

bool PlannedLayoutChecker::hasRequiredWidth(FieldIndex index, const PlacedSlot& slot) const
{
    if (m_keepsFieldsInCell && slot.representation == SlotRepresentation::Int32OrDouble)
        return false;
    if (!traitsOf(slot.representation).isNarrowReference)
        return true;
    if (m_usesWideReferences || m_isCoroutineFrame || m_planned.holdsWideReferences)
        return false;
    return !m_context.inputs.frameAllocation.mayHoldFramePointer(FieldReference { m_id, index });
}

bool PlannedLayoutChecker::isDeferredClosureProven(const PlacedSlot& slot, const Field& declaration) const
{
    if (slot.representation != SlotRepresentation::DeferredClosure)
        return !slot.deferredFunction && !slot.deferredClosureCapturesObject;
    const LocationFlow& location = declaration.flow.location;
    return location.deferral != ClosureDeferral::Unknown
        && slot.deferredFunction
        && slot.deferredFunction == location.deferredFunction
        && slot.deferredClosureCapturesObject == (location.deferral == ClosureDeferral::CapturesObject)
        && !declaration.flags.contains(FieldFlags::IsPrivate);
}

} // anonymous namespace

void checkSlotPlan(const PlacementCheckContext& context)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.program.layouts.size(), [&](unsigned index) {
        PlannedLayoutChecker(context, LayoutID(index)).run();
    });
}

static bool capturesOnlyClassEnvironment(const Program& program, const Function& constructor, const Function& method)
{
    if (method.captures.isEmpty())
        return true;
    if (method.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = method.captures[0].type;
    return captured && captured == constructor.captures[0].type && program.at(captured).environment;
}

static bool fitsClassRecord(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || !member.function || program.isIndexLike(member.key))
        return false;
    return capturesOnlyClassEnvironment(program, constructor, program.at(member.function));
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
    const Program& program = context.program;
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Instance || !layout.constructor)
        return;

    const ClassCreationPlan& plan = context.inputs.slots.classCreation(id);
    const Vector<ClassMember>& members = layout.classFlow.members;
    if (plan.members.size() != members.size()) {
        context.failures.fail(PlacementRule::MemberCreationOrder, id);
        return;
    }

    const Function& constructor = program.at(layout.constructor);
    bool isDefinedByNewClass = !layout.has(LayoutFlags::IsConstructorFunction);
    ClassSideFlags hasMemberAtDefinition;
    bool isOrderValid = true;
    for (size_t index = 0; index < members.size(); ++index) {
        const ClassMember& member = members[index];
        switch (plan.members[index]) {
        case MemberCreation::AtDefinition:
            hasMemberAtDefinition[member.side] = true;
            break;
        case MemberCreation::ByClassRecord:
            if (!isDefinedByNewClass || hasMemberAtDefinition[member.side] || !fitsClassRecord(program, constructor, member))
                isOrderValid = false;
            break;
        }
    }
    if (!isOrderValid)
        context.failures.fail(PlacementRule::MemberCreationOrder, id);

    bool mayCreateMembersLazily = isDefinedByNewClass && layout.classFlow.evaluations == ExecutionCount::AtMostOnce && !hasMemberAtDefinition.prototypeSide;
    bool isLazyCreationAllowed = (!plan.createsMembersLazily || mayCreateMembersLazily)
        && (!plan.createsClassObjectOnMutation || (plan.createsMembersLazily && !hasMemberAtDefinition.staticSide))
        && (!plan.createsPrototypeBeforeClassObject || (plan.createsMembersLazily && hasKnownBaseChain(program, id)));
    if (!isLazyCreationAllowed)
        context.failures.fail(PlacementRule::LazyCreationNotAllowed, id);
}

static void checkDefinedMethods(const PlacementCheckContext& context, FunctionID id)
{
    const Program& program = context.program;
    const Function& function = program.at(id);
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (value.opcode != Opcode::DefineMethod)
                continue;
            const Value& closure = function.at(definitionThroughUpcasts(function, value.operand(OperandIndex::DefineMethod::function)));
            if (closure.opcode != Opcode::NewClosure)
                continue;
            if (context.inputs.slots.creationOf(program, value.layout(), closure.function()) != MemberCreation::AtDefinition)
                context.failures.fail(PlacementRule::MemberCreationOrder, value.layout(), id, valueID);
        }
    }
}

void checkClassCreation(const PlacementCheckContext& context)
{
    const ParallelLoops& loops = context.build.parallelLoops();
    forEachIndexInParallel(loops, context.program.layouts.size(), [&](unsigned index) {
        checkClass(context, LayoutID(index));
    });
    forEachIndexInParallel(loops, context.program.functions.size(), [&](unsigned index) {
        checkDefinedMethods(context, FunctionID(index));
    });
}

static bool isClassConstructor(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return true;
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canHaveSlimForm(const PlacementCheckContext& context, FunctionID id)
{
    const Function& function = context.program.at(id);
    return !function.hasAny({ FunctionFlags::IsGenerator, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee }) && !isClassConstructor(function.kind);
}

void checkClosureForms(const PlacementCheckContext& context)
{
    const Program& program = context.program;
    const SlotPlan& slots = context.inputs.slots;
    forEachIndexInParallel(context.build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        if (slots.closureFormOf(id) == ClosureForm::Slim && !canHaveSlimForm(context, id))
            context.failures.fail(PlacementRule::ClosureFormNotProven, { }, id);
        bool targetIsSlim = slots.closureFormOf(closureTargetOf(program, id)) == ClosureForm::Slim;
        if (static_cast<bool>(context.placement.functionKind(id).isSlim) != targetIsSlim)
            context.failures.fail(PlacementRule::ClosureFormNotProven, { }, id);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
