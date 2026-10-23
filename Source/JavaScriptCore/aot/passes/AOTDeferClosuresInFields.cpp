/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeferClosuresInFields.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIR.h"
#include "AOTIRBuilder.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>
#include <wtf/FastMalloc.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::GetSlot::object == OperandIndex::GetDeferredClosureSlot::object && OperandIndex::GetSlot::guard == OperandIndex::GetDeferredClosureSlot::guard);
static_assert(OperandIndex::GetSlotOrUndefined::object == OperandIndex::GetDeferredClosureSlot::object && OperandIndex::GetSlotOrUndefined::guard == OperandIndex::GetDeferredClosureSlot::guard);
static_assert(OperandIndex::GetSlotOrUndefined::object == OperandIndex::GetDeferredClosureSlotOrUndefined::object && OperandIndex::GetSlotOrUndefined::guard == OperandIndex::GetDeferredClosureSlotOrUndefined::guard);

static constexpr size_t maxWindowSize = 512;
static constexpr unsigned maxCallDepth = 4;
static constexpr uint32_t noCalleeValue = UINT32_MAX;
static constexpr OptionSet<FieldStability> stabilityOfFilledSlot { FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };

namespace {

using ClosureFieldRow = uint32_t;

class FieldRows {
public:
    explicit FieldRows(const Program& program)
    {
        m_firstRows.reserveInitialCapacity(program.layouts.size() + 1);
        ClosureFieldRow next = 0;
        for (const Layout& layout : program.layouts) {
            m_firstRows.append(next);
            next += layout.fields.size();
        }
        m_firstRows.append(next);
    }

    ClosureFieldRow rowOf(LayoutID layout, uint32_t field) const { return m_firstRows[layout.index()] + field; }
    uint32_t numberOfFields(LayoutID layout) const { return m_firstRows[layout.index() + 1] - m_firstRows[layout.index()]; }
    size_t count() const { return m_firstRows.last(); }

private:
    Vector<ClosureFieldRow> m_firstRows;
};

enum class Rejection : uint8_t {
    None,
    NotInitializedByEveryAllocation,
    AllocationMaySelectOtherClass,
    InitializedWithSeveralFunctions,
    StoredAfterInitialization,
    ReadWhereItMayHaveLeftSlot,
    CapturesSeveralEnvironments,
    HoldsOtherThanObjects,
    IsPartOfConstantData,
};
constexpr size_t numberOfRejections = 10;

struct FieldState {
    bool canDefer() const { return function && rejection == Rejection::None; }

    FunctionID function;
    uint32_t numberOfInitializingAllocations { 0 };
    Rejection rejection { Rejection::None };
};

// `object.key = NewClosure function(object)`, or one that captures nothing, anywhere in the program. Nothing but the store uses the closure.
struct ClosureStore {
    FunctionID function;
    BlockID block;
    ValueID store;
    ValueID closure;
    LayoutID layout;
    uint32_t field { 0 };
    FunctionID closureFunction;
};

struct ValueReference {
    explicit operator bool() const { return !!value; }

    FunctionID function;
    ValueID value;

    friend bool operator==(const ValueReference&, const ValueReference&) = default;
    friend auto operator<=>(const ValueReference&, const ValueReference&) = default;
};

struct FieldInitializer {
    uint32_t field { 0 };
    ValueReference store;
    ValueID value;
    uint32_t calleeValue { noCalleeValue };
    FunctionID closureFunction;
    ValueID closure;
    ValueID environment;
    bool isInCallee { false };
    bool isAvailableAtAllocation { false };
};

struct EnteredCall {
    ValueReference call;
    FunctionID callee;
    bool passesOwnReceiver { false };
};

struct ClosureFieldFreshObject {
    FunctionID function;
    BlockID block;
    ValueID allocation;
    LayoutID layout;
    Vector<FieldInitializer> initializers;
    Vector<Value> calleeValues;
    Vector<EnteredCall> enteredCalls;
};

struct ScannedFunction {
    Vector<ClosureFieldFreshObject> objects;
    Vector<ClosureStore> closureStores;
};

struct PrologueValue {
    enum class Kind : uint8_t { Unknown, Parameter, Constant, Closure };

    Kind kind { Kind::Unknown };
    uint32_t parameterIndex { 0 };
    ValueID value;
};

struct PrologueStep {
    bool isCall() const { return !!callee; }

    ValueID operation;
    LayoutID layout;
    uint32_t field { 0 };
    PrologueValue stored;
    FunctionID callee;
    Vector<PrologueValue, 4> arguments;
};

struct CalleePrologue {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CalleePrologue);

    FunctionID function;
    Vector<PrologueStep> steps;
    bool isWholeBody { false };
};

struct ResolvedValue {
    enum class Kind : uint8_t { Unknown, Local, Constant };

    Kind kind { Kind::Unknown };
    ValueID local;
    ValueReference constant;
};

enum class Fold : uint8_t { None, Value, Placeholder };

struct ClosureEnvironmentField {
    LayoutID layout;
    uint32_t field { 0 };
    TypeID type;
    bool isNew { false };
};

struct CallSiteState {
    ValueReference call;
    FunctionID callee;
    bool passesOwnReceiver { false };
    uint32_t numberOfWindows { 0 };
};

struct CalleeState {
    FunctionID function;
    uint32_t numberOfEntrySites { 0 };
    uint32_t numberOfEnteredSites { 0 };
    uint32_t numberOfWindows { 0 };
    bool isEnteredOnlyFromWindows { false };
};

struct CalleeStoreState {
    ValueReference store;
    uint32_t numberOfWindows { 0 };
    uint32_t numberOfFolds { 0 };
    bool canLeaveCallee { false };
};

enum class ClosureFieldAccessKind : uint8_t { SlotRead, SlotStore, PresenceTest, NamedAccess, KeyOrderTest, FunctionEntry, Allocation };

struct FoundAccess {
    ValueReference reference;
    ClosureFieldAccessKind kind { ClosureFieldAccessKind::SlotRead };
};

struct FunctionEdits {
    FunctionID function;
    std::span<const ClosureFieldFreshObject> objects;
    std::span<const ValueReference> reads;
    std::span<const ValueReference> namedAccesses;
    std::span<const ValueReference> keyOrderTests;
    std::span<const ValueReference> removedStores;
    std::span<const ClosureStore> deferredStores;
};

struct EditCounts {
    uint64_t closures { 0 };
    uint64_t stores { 0 };
};

} // anonymous namespace

static ASCIILiteral describe(Rejection rejection)
{
    switch (rejection) {
    case Rejection::None:
        return "deferred"_s;
    case Rejection::NotInitializedByEveryAllocation:
        return "rejected: an allocation does not initialize the field with the closure"_s;
    case Rejection::AllocationMaySelectOtherClass:
        return "rejected: an allocation takes a prototype that may select another class"_s;
    case Rejection::InitializedWithSeveralFunctions:
        return "rejected: allocations initialize the field with closures of different functions"_s;
    case Rejection::StoredAfterInitialization:
        return "rejected: a store outside of object initialization writes the field"_s;
    case Rejection::ReadWhereItMayHaveLeftSlot:
        return "rejected: a read counts on a slot that takes every value"_s;
    case Rejection::CapturesSeveralEnvironments:
        return "rejected: the closures of one object capture different environments"_s;
    case Rejection::HoldsOtherThanObjects:
        return "rejected: the field may get a value that is neither an object nor undefined nor null"_s;
    case Rejection::IsPartOfConstantData:
        return "rejected: constant data gives the field a value other than undefined and null"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct DeferrableLayouts {
    // A store may leave its closure unmade in an object of such a layout.
    BitVector all;
    // The operations of the program make every object of such a layout, and each of them makes that layout and no other.
    BitVector allocatedOnlyByOperations;
};

} // anonymous namespace

static DeferrableLayouts findDeferrableLayouts(const Program& program)
{
    BitVector isBase(program.layouts.size());
    BitVector isFilledByOthers(program.layouts.size());
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.flow.baseState == BaseState::Layout)
            isBase.quickSet(layout.flow.base.index());
        bool isObject = layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance;
        if (!isObject || layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse) || layout.flow.baseState == BaseState::BuiltinClass)
            isFilledByOthers.quickSet(index);
    }
    // A class has the slots of the classes above it.
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (size_t index : isFilledByOthers) {
            const Layout& layout = program.layouts[index];
            if (layout.flow.baseState == BaseState::Layout)
                didChange |= !isFilledByOthers.quickSet(layout.flow.base.index());
        }
    }

    DeferrableLayouts result { BitVector(program.layouts.size()), BitVector(program.layouts.size()) };
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (isFilledByOthers.quickGet(index) || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        result.all.quickSet(index);
        if (isBase.quickGet(index) || layout.flow.baseState == BaseState::Layout)
            continue;
        if (layout.constructor && program.at(layout.constructor).kind != FunctionKind::BaseConstructor)
            continue;
        result.allocatedOnlyByOperations.quickSet(index);
    }
    return result;
}

// A class and the classes below it have one slot for a field, and the field of the class that declares it speaks for all of them.
static bool canDeferClosure(const Program& program, FieldReference reference)
{
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsPrivate, FieldFlags::IsImmutable, FieldFlags::IsBrand, FieldFlags::IsHidden, FieldFlags::IsRemovedAsUnread };
    const Field& field = fieldAt(program, declarationOf(program, reference));
    return field.key && !field.flags.containsAny(excludedFlags) && field.form.isEmpty() && !program.isIndexLike(field.key) && !isRemovedField(program, reference);
}

static bool canBeCreatedOnDemand(const Program& program, const Value& closure)
{
    const Function& callee = program.at(closure.function());
    // A plain function may be constructed. It is a function object like any other once something reads the field, and nothing constructs what it has not read.
    bool isConstructorOfClass = callee.constructedClass || callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
    if (isConstructorOfClass || callee.captures.size() > 1 || namesClosureOfTable(program, closure))
        return false;
    if (!callee.captures.isEmpty()) {
        const Type& captured = program.at(callee.captures[0].type);
        if (captured.environment && captured.primitives)
            return false;
    }
    return closure.storage() == AllocationStorage::Heap;
}

static ClosureDeferral deferralFor(const Program& program, FunctionID function)
{
    const Function& callee = program.at(function);
    if (callee.captures.isEmpty())
        return ClosureDeferral::CapturesNothing;
    return program.at(callee.captures[0].type).environment ? ClosureDeferral::CapturesEnvironment : ClosureDeferral::CapturesObject;
}

static bool createsValueWithoutRunningCode(const Value& value)
{
    if (isRematerializable(value) || isCastWithoutTest(value.opcode) || value.opcode == Opcode::AllGuards)
        return true;
    return interpretedOf(value.opcode) == Interpretation::Allocation && declaredBoundOf(value.opcode).isUnobservable();
}

static bool isFieldStore(Opcode opcode)
{
    return opcode == Opcode::PutSlot || opcode == Opcode::PutSlotOrAdd;
}

static void countAllocation(std::span<uint32_t> allocationsOfLayouts, LayoutID layout)
{
    WTF::atomicExchangeAdd(&allocationsOfLayouts[layout.index()], 1u, std::memory_order_relaxed);
}

static std::optional<Value> tryCreatePlaceholder(const Type& type)
{
    if (type.primitives & Primitive::Undefined)
        return IRBuilder::make(Opcode::Undefined);
    if (type.primitives & Primitive::Null)
        return IRBuilder::make(Opcode::Null);
    if (!(type.primitives & Primitive::Integer) || type.integers.isEmpty())
        return std::nullopt;
    Value integer = IRBuilder::make(Opcode::Integer);
    integer.setInteger(std::clamp<int64_t>(0, type.integers.minimum, type.integers.maximum));
    return integer;
}

static std::optional<Value> tryCreatePlaceholder(const Program& program, FieldReference reference)
{
    const Field& field = fieldAt(program, declarationOf(program, reference));
    if (!canBeStoredBehindAllocation(field))
        return std::nullopt;
    return tryCreatePlaceholder(program.at(slotTypeOf(program, field)));
}

// One closure for one object.
static ValueID findClosureUsedOnce(const Program& program, const Function& function, FunctionAnalyses& analyses, BlockID block, ValueID stored)
{
    const UseLists& uses = analyses.useLists();
    ValueID id = definitionThroughUpcasts(function, stored);
    const Value& closure = function.at(id);
    if (closure.opcode != Opcode::NewClosure || !canBeCreatedOnDemand(program, closure))
        return { };

    for (ValueID cast = stored; cast != id; cast = function.at(cast).operand(0)) {
        if (uses.useCount(cast) != 1)
            return { };
    }
    if (uses.useCount(id) != 1)
        return { };
    ValueLocation location = uses.locationOf(id);
    if (location.placement != ValuePlacement::InBlock)
        return { };
    if (location.block == block)
        return id;
    bool userRunsOnceForEachClosure = stored == id && !analyses.isInCycle(block) && analyses.dominators().dominates(location.block, block);
    return userRunsOnceForEachClosure ? id : ValueID();
}

bool canLeaveClosureUnmade(FunctionAnalyses& analyses, BlockID block, ValueID object, ValueID given)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    ValueID id = findClosureUsedOnce(program, function, analyses, block, given);
    if (!id)
        return false;
    std::span<const ValueID> captures = function.at(id).capturesOperands();
    if (captures.empty())
        return true;
    return sameObjectThroughCasts(function, captures[0]) == object || program.typeOf(function, captures[0]).environment;
}

static void forEachFinalInitializer(const ClosureFieldFreshObject& object, const Invocable<void(size_t, const FieldInitializer&)> auto& functor)
{
    Vector<uint32_t, 16> visitedFields;
    for (size_t index = object.initializers.size(); index--;) {
        const FieldInitializer& initializer = object.initializers[index];
        if (visitedFields.contains(initializer.field))
            continue;
        visitedFields.append(initializer.field);
        functor(index, initializer);
    }
}

template<typename Entries, typename Entry, typename Key>
static auto& findSorted(Entries& entries, const Key& key, Key Entry::* member)
{
    auto found = std::ranges::lower_bound(entries, key, { }, member);
    RELEASE_ASSERT(found != entries.end() && (*found).*member == key);
    return *found;
}

template<typename Edit>
static std::span<const Edit> takeEditsOf(FunctionID function, std::span<const Edit>& remaining)
{
    size_t count = std::ranges::find_if(remaining, [&](const Edit& edit) {
        return edit.function != function;
    }) - remaining.begin();
    std::span<const Edit> taken = remaining.first(count);
    remaining = remaining.subspan(count);
    return taken;
}

namespace {

class InitializationWindow {
public:
    explicit InitializationWindow(const Function& function)
        : m_function(function)
    {
    }

    void begin(ValueID object)
    {
        size_t numberOfValues = m_function.values.size();
        m_contains.ensureSize(numberOfValues);
        m_isObject.ensureSize(numberOfValues);
        m_mayHoldObject.ensureSize(numberOfValues);
        m_canBeHoisted.ensureSize(numberOfValues);
        for (ValueID id : m_values) {
            m_contains.quickClear(id.index());
            m_isObject.quickClear(id.index());
            m_mayHoldObject.quickClear(id.index());
            m_canBeHoisted.quickClear(id.index());
        }
        m_values.shrink(0);
        m_hasSkippedOperation = false;

        m_values.append(object);
        m_contains.quickSet(object.index());
        m_isObject.quickSet(object.index());
        m_mayHoldObject.quickSet(object.index());
    }

    bool contains(ValueID id) const { return m_contains.quickGet(id.index()); }
    bool isObject(ValueID id) const { return id && m_isObject.quickGet(id.index()); }
    bool mayHoldObject(ValueID id) const { return id && m_mayHoldObject.quickGet(id.index()); }
    bool isAvailableAtAllocation(ValueID id) const { return !contains(id) || m_canBeHoisted.quickGet(id.index()); }

    bool usesObject(const Value& value) const
    {
        bool result = false;
        value.forEachChild([&](ValueID operand) {
            result |= mayHoldObject(operand);
        });
        return result;
    }

    void addCreatedValue(ValueID id, const Value& value)
    {
        bool usesOnlyObject = true;
        bool holdsObject = false;
        bool canBeHoisted = !m_hasSkippedOperation || (value.opcode != Opcode::NewObject && declaredBoundOf(value.opcode).reads.containsOnly(bookkeepingHeaps));
        value.forEachChild([&](ValueID operand) {
            usesOnlyObject &= isObject(operand);
            holdsObject |= mayHoldObject(operand);
            canBeHoisted &= isAvailableAtAllocation(operand);
        });
        m_values.append(id);
        m_contains.quickSet(id.index());
        if (usesOnlyObject && (isCastWithoutTest(value.opcode) || value.opcode == Opcode::AllGuards))
            m_isObject.quickSet(id.index());
        if (holdsObject)
            m_mayHoldObject.quickSet(id.index());
        if (canBeHoisted)
            m_canBeHoisted.quickSet(id.index());
    }

    void addSkippedOperation(ValueID id)
    {
        m_values.append(id);
        m_contains.quickSet(id.index());
        m_hasSkippedOperation = true;
    }

private:
    const Function& m_function;
    Vector<ValueID, 32> m_values;
    BitVector m_contains;
    BitVector m_isObject;
    BitVector m_mayHoldObject;
    BitVector m_canBeHoisted;
    bool m_hasSkippedOperation { false };
};

} // anonymous namespace

static LayoutID layoutInitializedBy(const Program& program, const InitializationWindow& window, const Value& store)
{
    if (store.level() != KnowledgeLevel::Proven || (store.guardOperand() && !window.isObject(store.guardOperand())))
        return { };
    const LayoutSet& receivers = program.at(store.receivers());
    return receivers.isComplete && receivers.members.size() == 1 ? receivers.members[0] : LayoutID();
}

static bool passesObjectOnlyAsReceiver(const Program& program, const InitializationWindow& window, const Value& call)
{
    const Function& callee = program.at(call.function());
    std::optional<unsigned> receiverIndex = callee.thisParameterIndex();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!receiverIndex || arguments.size() != callee.numberOfEntryParameters() || !window.isObject(arguments[*receiverIndex]))
        return false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (index != *receiverIndex && window.mayHoldObject(arguments[index]))
            return false;
    }
    return true;
}

namespace {

class PrologueFinder {
public:
    PrologueFinder(const Program& program, BuildContext& build, const DeferrableLayouts& deferrableLayouts, FunctionID id)
        : m_program(program)
        , m_build(build)
        , m_deferrableLayouts(deferrableLayouts)
        , m_id(id)
        , m_function(program.at(id))
        , m_window(m_function)
    {
    }

    std::unique_ptr<CalleePrologue> run()
    {
        if (!isEnteredOnlyByDirectCalls())
            return nullptr;

        const Block& entry = m_function.at(entryBlock);
        m_window.begin(entry.parameters[*m_function.thisParameterIndex()]);
        auto prologue = makeUnique<CalleePrologue>();
        prologue->function = m_id;

        for (ValueID id : entry.values.span().first(std::min(entry.values.size(), maxWindowSize))) {
            const Value& value = m_function.at(id);
            std::optional<PrologueStep> step;
            if (isFieldStore(value.opcode) && m_window.isObject(value.objectOperand()))
                step = describeStore(id, value);
            else if (value.opcode == Opcode::CallDirect && m_window.usesObject(value)) {
                step = describeCall(id, value);
                m_window.addSkippedOperation(id);
            } else if (createsValueWithoutRunningCode(value)) {
                m_window.addCreatedValue(id, value);
                continue;
            } else if (!m_window.usesObject(value)) {
                m_window.addSkippedOperation(id);
                continue;
            }
            if (!step)
                return prologue;
            prologue->steps.append(WTF::move(*step));
        }

        const Terminator& terminator = entry.terminator;
        prologue->isWholeBody = entry.values.size() <= maxWindowSize && terminator.kind == TerminatorKind::Return && !m_window.mayHoldObject(terminator.valueOperand());
        return prologue;
    }

private:
    bool isEnteredOnlyByDirectCalls() const
    {
        if (!m_function.hasBody() || !m_function.thisParameterIndex() || !m_build.isEnabled(PassID::DeferClosuresInFields, m_id))
            return false;
        if (m_function.at(entryBlock).parameters.size() != m_function.numberOfEntryParameters())
            return false;
        return m_function.hasOnlyDirectCallers() || m_function.kind == FunctionKind::Initializer;
    }

    FunctionAnalyses& analyses()
    {
        if (!m_analyses)
            m_analyses.emplace(AnalysisContext { m_build, m_program, nullptr, nullptr, m_id }, m_function);
        return *m_analyses;
    }

    const UseLists& useLists() { return analyses().useLists(); }

    PrologueValue describeValue(ValueID id) const
    {
        if (!id)
            return { };
        ValueID definition = definitionThroughUpcasts(m_function, id);
        const Value& value = m_function.at(definition);
        if (isRematerializable(value))
            return { PrologueValue::Kind::Constant, 0, definition };
        if (value.opcode != Opcode::Parameter)
            return { };
        size_t index = m_function.at(entryBlock).parameters.find(definition);
        if (index == notFound)
            return { };
        return { PrologueValue::Kind::Parameter, static_cast<uint32_t>(index), definition };
    }

    PrologueValue describeStoredValue(ValueID stored)
    {
        if (m_function.at(definitionThroughUpcasts(m_function, stored)).opcode != Opcode::NewClosure)
            return describeValue(stored);

        ValueID id = findClosureUsedOnce(m_program, m_function, analyses(), entryBlock, stored);
        if (!id)
            return { };
        const Value& closure = m_function.at(id);
        std::span<const ValueID> captures = closure.capturesOperands();
        if (closure.isMadeOnDemand() || captures.size() != m_program.at(closure.function()).captures.size())
            return { };
        bool capturesReceiver = !captures.empty() && m_window.isObject(captures[0]) && deferralFor(m_program, closure.function()) == ClosureDeferral::CapturesObject;
        if (!captures.empty() && !capturesReceiver)
            return { };
        return { PrologueValue::Kind::Closure, 0, id };
    }

    std::optional<PrologueStep> describeStore(ValueID id, const Value& store)
    {
        LayoutID layout = layoutInitializedBy(m_program, m_window, store);
        if (!layout || !m_deferrableLayouts.allocatedOnlyByOperations.quickGet(layout.index()))
            return std::nullopt;
        std::optional<uint32_t> field = m_program.at(layout).findField(store.key());
        if (!field)
            return std::nullopt;
        return PrologueStep { .operation = id, .layout = layout, .field = *field, .stored = describeStoredValue(store.valueOperand()) };
    }

    std::optional<PrologueStep> describeCall(ValueID id, const Value& call) const
    {
        if (!passesObjectOnlyAsReceiver(m_program, m_window, call))
            return std::nullopt;
        PrologueStep step { .operation = id, .callee = call.function() };
        for (ValueID argument : call.argumentsOperands())
            step.arguments.append(describeValue(argument));
        return step;
    }

    const Program& m_program;
    BuildContext& m_build;
    const DeferrableLayouts& m_deferrableLayouts;
    FunctionID m_id;
    const Function& m_function;
    InitializationWindow m_window;
    std::optional<FunctionAnalyses> m_analyses;
};

class FreshObjectScanner {
public:
    FreshObjectScanner(const Program& program, BuildContext& build, const DeferrableLayouts& deferrableLayouts, FunctionID id)
        : m_program(program)
        , m_build(build)
        , m_deferrableLayouts(deferrableLayouts)
        , m_id(id)
        , m_function(program.at(id))
        , m_window(m_function)
    {
    }

    void run(std::span<uint32_t> allocationsOfLayouts, std::span<uint32_t> allocationsUnderPrototypes, ScannedFunction& found)
    {
        bool isEnabled = m_build.isEnabled(PassID::DeferClosuresInFields, m_id);
        forEachListedValue(m_program, m_id, scannedOpcodes(), [&](BlockID block, uint32_t index, ValueID id, const Value& value) {
            if (isFieldStore(value.opcode)) {
                if (isEnabled)
                    describeStore(block, id, value, found.closureStores);
                return;
            }
            if ((value.opcode != Opcode::NewObject && value.opcode != Opcode::CloneAs) || !m_deferrableLayouts.allocatedOnlyByOperations.quickGet(value.layout().index()))
                return;
            countAllocation(allocationsOfLayouts, value.layout());
            if (mayAllocateOtherLayout(m_program, value)) {
                countAllocation(allocationsUnderPrototypes, value.layout());
                return;
            }
            if (!isEnabled || value.opcode != Opcode::NewObject)
                return;
            ClosureFieldFreshObject object = describe(block, index);
            bool isInitializedWithClosure = object.initializers.containsIf([](const FieldInitializer& initializer) {
                return !!initializer.closureFunction;
            });
            if (isInitializedWithClosure)
                found.objects.append(WTF::move(object));
        });
    }

    // The opcodes for which run() does something.
    static const OpcodeSet& scannedOpcodes()
    {
        static const OpcodeSet opcodes = [] {
            OpcodeSet result { Opcode::NewObject, Opcode::CloneAs };
            for (Opcode opcode : allMembersOf<Opcode>) {
                if (isFieldStore(opcode))
                    result.add(opcode);
            }
            return result;
        }();
        return opcodes;
    }

private:
    struct CachedPrologue {
        FunctionID function;
        std::unique_ptr<CalleePrologue> prologue;
    };

    FunctionAnalyses& analyses()
    {
        if (!m_analyses)
            m_analyses.emplace(AnalysisContext { m_build, m_program, nullptr, nullptr, m_id }, m_function);
        return *m_analyses;
    }

    const UseLists& useLists() { return analyses().useLists(); }

    const CalleePrologue* findPrologue(FunctionID callee)
    {
        size_t index = m_prologues.findIf([&](const CachedPrologue& cached) {
            return cached.function == callee;
        });
        if (index == notFound) {
            index = m_prologues.size();
            m_prologues.append({ callee, PrologueFinder(m_program, m_build, m_deferrableLayouts, callee).run() });
        }
        return m_prologues[index].prologue.get();
    }

    void describeStore(BlockID block, ValueID id, const Value& store, Vector<ClosureStore>& found)
    {
        ValueID stored = store.valueOperand();
        if (m_function.at(definitionThroughUpcasts(m_function, stored)).opcode != Opcode::NewClosure)
            return;
        // Code that only expects the layout of its objects reads their members by name as well, and a read by name makes the closure in the run time, which costs as much as forty closures made beside the allocation.
        if (store.level() != KnowledgeLevel::Proven)
            return;
        std::optional<FieldReference> declaration = findDeclarationInEveryReceiver(store);
        if (!declaration || !canDeferClosure(m_program, *declaration))
            return;
        ValueID closureID = findClosureUsedOnce(m_program, m_function, analyses(), block, stored);
        if (!closureID)
            return;

        const Value& closure = m_function.at(closureID);
        std::span<const ValueID> captures = closure.capturesOperands();
        if (closure.isMadeOnDemand() || captures.size() != m_program.at(closure.function()).captures.size())
            return;
        switch (deferralFor(m_program, closure.function())) {
        case ClosureDeferral::Unknown:
            RELEASE_ASSERT_NOT_REACHED();
        case ClosureDeferral::CapturesNothing:
            break;
        case ClosureDeferral::CapturesObject:
            if (sameObjectThroughCasts(m_function, captures[0]) != sameObjectThroughCasts(m_function, store.objectOperand()))
                return;
            break;
        // Only an allocation can fill the hidden field that keeps the environment.
        case ClosureDeferral::CapturesEnvironment:
            return;
        }
        found.append({ m_id, block, id, closureID, declaration->layout, declaration->field, closure.function() });
    }

    std::optional<FieldReference> findDeclarationInEveryReceiver(const Value& store) const
    {
        const LayoutSet& receivers = m_program.at(store.receivers());
        if (!receivers.isComplete || receivers.members.isEmpty())
            return std::nullopt;
        std::optional<FieldReference> declaration;
        for (LayoutID member : receivers.members) {
            std::optional<uint32_t> field = m_program.at(member).findField(store.key());
            if (!field || !m_deferrableLayouts.all.quickGet(member.index()))
                return std::nullopt;
            FieldReference declarationOfMember = declarationOf(m_program, { member, *field });
            if (declaration && *declaration != declarationOfMember)
                return std::nullopt;
            declaration = declarationOfMember;
        }
        return m_deferrableLayouts.all.quickGet(declaration->layout.index()) ? declaration : std::nullopt;
    }

    static ValueID findListedEnvironment(const Layout& layout, const Value& allocation)
    {
        if (!layout.flow.closureEnvironmentField)
            return { };
        std::span<const uint32_t> fields = allocation.fields();
        size_t index = std::ranges::find(fields, *layout.flow.closureEnvironmentField) - fields.begin();
        return index < fields.size() ? allocation.valuesOperands()[index] : ValueID();
    }

    void describeClosure(const ClosureFieldFreshObject& object, ValueID listedEnvironment, FieldInitializer& initializer)
    {
        if (m_function.at(definitionThroughUpcasts(m_function, initializer.value)).opcode != Opcode::NewClosure || !canDeferClosure(m_program, { object.layout, initializer.field }))
            return;
        ValueID id = findClosureUsedOnce(m_program, m_function, analyses(), object.block, initializer.value);
        if (!id)
            return;

        const Value& closure = m_function.at(id);
        ClosureDeferral deferral = deferralFor(m_program, closure.function());
        ValueID captured = listedEnvironment;
        if (!closure.isMadeOnDemand()) {
            std::span<const ValueID> captures = closure.capturesOperands();
            if (captures.size() != m_program.at(closure.function()).captures.size())
                return;
            captured = captures.empty() ? ValueID() : captures[0];
        }

        switch (deferral) {
        case ClosureDeferral::Unknown:
            RELEASE_ASSERT_NOT_REACHED();
        case ClosureDeferral::CapturesNothing:
            break;
        case ClosureDeferral::CapturesObject:
            if (!closure.isMadeOnDemand() && !m_window.isObject(captured))
                return;
            break;
        case ClosureDeferral::CapturesEnvironment:
            if (!captured || m_window.contains(captured))
                return;
            initializer.environment = captured;
            break;
        }
        initializer.closure = id;
        initializer.closureFunction = closure.function();
    }

    void addInitializer(ClosureFieldFreshObject& object, ValueID listedEnvironment, uint32_t field, ValueID store, ValueID value)
    {
        FieldInitializer initializer { .field = field, .value = value, .isAvailableAtAllocation = !store || m_window.isAvailableAtAllocation(value) };
        if (store)
            initializer.store = { m_id, store };
        describeClosure(object, listedEnvironment, initializer);
        object.initializers.append(initializer);
    }

    void addCalleeInitializer(ClosureFieldFreshObject& object, const CalleePrologue& prologue, const PrologueStep& step, const ResolvedValue& stored)
    {
        FieldInitializer initializer { .field = step.field, .store = { prologue.function, step.operation }, .isInCallee = true };
        auto copyFromCallee = [&](ValueReference reference) {
            initializer.calleeValue = static_cast<uint32_t>(object.calleeValues.size());
            object.calleeValues.append(m_program.at(reference.function).at(reference.value));
        };

        if (step.stored.kind == PrologueValue::Kind::Closure) {
            if (canDeferClosure(m_program, { object.layout, step.field })) {
                copyFromCallee({ prologue.function, step.stored.value });
                initializer.closureFunction = object.calleeValues.last().function();
            }
        } else {
            switch (stored.kind) {
            case ResolvedValue::Kind::Unknown:
                break;
            case ResolvedValue::Kind::Local:
                initializer.value = stored.local;
                initializer.isAvailableAtAllocation = m_window.isAvailableAtAllocation(stored.local);
                break;
            case ResolvedValue::Kind::Constant:
                copyFromCallee(stored.constant);
                initializer.isAvailableAtAllocation = true;
                break;
            }
        }
        object.initializers.append(initializer);
    }

    bool walkPrologue(ClosureFieldFreshObject& object, const EnteredCall& call, const CalleePrologue& prologue, std::span<const ResolvedValue> arguments, unsigned depth)
    {
        object.enteredCalls.append(call);
        auto resolve = [&](const PrologueValue& value) -> ResolvedValue {
            switch (value.kind) {
            case PrologueValue::Kind::Unknown:
            case PrologueValue::Kind::Closure:
                return { };
            case PrologueValue::Kind::Parameter:
                return arguments[value.parameterIndex];
            case PrologueValue::Kind::Constant:
                return { ResolvedValue::Kind::Constant, { }, { prologue.function, value.value } };
            }
            RELEASE_ASSERT_NOT_REACHED();
        };

        for (const PrologueStep& step : prologue.steps) {
            if (!step.isCall()) {
                if (step.layout != object.layout)
                    return false;
                addCalleeInitializer(object, prologue, step, resolve(step.stored));
                continue;
            }
            const CalleePrologue* nested = depth < maxCallDepth ? findPrologue(step.callee) : nullptr;
            if (!nested)
                return false;
            Vector<ResolvedValue, 8> nestedArguments(step.arguments.size(), [&](size_t index) {
                return resolve(step.arguments[index]);
            });
            if (!walkPrologue(object, { { prologue.function, step.operation }, step.callee, true }, *nested, nestedArguments.span(), depth + 1))
                return false;
        }
        return prologue.isWholeBody;
    }

    bool enterCall(ClosureFieldFreshObject& object, ValueID id, const Value& call)
    {
        if (!passesObjectOnlyAsReceiver(m_program, m_window, call))
            return false;
        const CalleePrologue* prologue = findPrologue(call.function());
        if (!prologue)
            return false;
        std::span<const ValueID> operands = call.argumentsOperands();
        Vector<ResolvedValue, 8> arguments(operands.size(), [&](size_t index) {
            return operands[index] ? ResolvedValue { ResolvedValue::Kind::Local, operands[index], { } } : ResolvedValue();
        });
        return walkPrologue(object, { { m_id, id }, call.function(), false }, *prologue, arguments.span(), 0);
    }

    ClosureFieldFreshObject describe(BlockID blockID, size_t allocationIndex)
    {
        const BlockValues& values = m_function.at(blockID).values;
        ValueID allocationID = values[allocationIndex];
        const Value& allocation = m_function.at(allocationID);
        LayoutID layout = allocation.layout();
        ClosureFieldFreshObject object { m_id, blockID, allocationID, layout, { }, { }, { } };
        m_window.begin(allocationID);

        ValueID listedEnvironment = findListedEnvironment(m_program.at(layout), allocation);
        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> listedValues = allocation.valuesOperands();
        for (size_t index = 0; index < fields.size(); ++index)
            addInitializer(object, listedEnvironment, fields[index], { }, listedValues[index]);

        size_t end = std::min(values.size(), allocationIndex + 1 + maxWindowSize);
        for (size_t index = allocationIndex + 1; index < end; ++index) {
            ValueID id = values[index];
            const Value& value = m_function.at(id);
            if (isFieldStore(value.opcode) && m_window.isObject(value.objectOperand())) {
                std::optional<uint32_t> field;
                if (layoutInitializedBy(m_program, m_window, value) == layout)
                    field = m_program.at(layout).findField(value.key());
                if (!field)
                    break;
                addInitializer(object, listedEnvironment, *field, id, value.valueOperand());
                continue;
            }
            if (value.opcode == Opcode::CallDirect && m_window.usesObject(value)) {
                if (!enterCall(object, id, value))
                    break;
                m_window.addSkippedOperation(id);
                continue;
            }
            if (createsValueWithoutRunningCode(value)) {
                m_window.addCreatedValue(id, value);
                continue;
            }
            if (m_window.usesObject(value))
                break;
            m_window.addSkippedOperation(id);
        }
        return object;
    }

    const Program& m_program;
    BuildContext& m_build;
    const DeferrableLayouts& m_deferrableLayouts;
    FunctionID m_id;
    const Function& m_function;
    InitializationWindow m_window;
    std::optional<FunctionAnalyses> m_analyses;
    Vector<CachedPrologue, 2> m_prologues;
};

class DeferralAnalysis {
public:
    DeferralAnalysis(const Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
        , m_rows(program)
        , m_largeFunctions(largeFunctions(program, build.parallelLoops()))
    {
    }

    void run();

    bool defersAnything() const { return m_numberOfDeferringFields; }
    uint64_t numberOfDeferringFields() const { return m_numberOfDeferringFields; }
    std::span<const ClosureFieldFreshObject> objects() const LIFETIME_BOUND { return m_objects.span(); }
    std::span<const ValueReference> reads() const LIFETIME_BOUND { return m_reads.span(); }
    std::span<const ValueReference> namedAccesses() const LIFETIME_BOUND { return m_namedAccesses.span(); }
    std::span<const ValueReference> keyOrderTests() const LIFETIME_BOUND { return m_keyOrderTests.span(); }
    std::span<const ValueReference> removedStores() const LIFETIME_BOUND { return m_removedStores.span(); }
    std::span<const ClosureStore> deferredStores() const LIFETIME_BOUND { return m_closureStores.span(); }
    std::span<const FieldReference> candidates() const LIFETIME_BOUND { return m_candidates.span(); }
    std::span<const ClosureEnvironmentField> environmentFields() const LIFETIME_BOUND { return m_environmentFields.span(); }

    const FieldState& stateOf(LayoutID layout, uint32_t field) const LIFETIME_BOUND { return m_states[rowOf(layout, field)]; }
    bool defers(const ClosureFieldFreshObject&, const FieldInitializer&) const;
    bool defers(const ClosureStore&) const;
    bool defersAnyClosure(const ClosureFieldFreshObject&) const;
    bool isFilledByEveryAllocation(FieldReference) const;
    bool mayFindSlotEmpty(const Value& read) const;
    bool findsClosureOverReceiver(const Value& read) const;
    Vector<Fold, 16> selectFolds(const ClosureFieldFreshObject&) const;
    const ClosureEnvironmentField* findEnvironmentField(LayoutID) const LIFETIME_BOUND;
    ValueID environmentOf(const ClosureFieldFreshObject&) const;

    void dump(PrintStream&) const;

private:
    void findObjects();
    void findCandidates();
    void findEnteredCalls();
    void findAccesses();
    void findCalleesEnteredOnlyFromWindows();
    void findCalleeStores();
    void finish();

    const Value& valueAt(const ValueReference& reference) const LIFETIME_BOUND { return m_program.at(reference.function).at(reference.value); }
    ClosureFieldRow rowOf(LayoutID layout, uint32_t field) const
    {
        FieldReference declaration = declarationOf(m_program, { layout, field });
        return m_rows.rowOf(declaration.layout, declaration.field);
    }
    std::optional<ClosureFieldAccessKind> classifyAccess(const Function&, const Value&) const;
    bool entersCalleeOfWindow(const Value&) const;
    bool namesCandidate(const Value& access) const;
    bool namesDeferringField(const Value& access) const;
    bool becomesAlwaysFilled(FieldReference) const;

    bool keepsEnvironment(const FieldState&) const;
    bool mayBeOtherThanObject(FunctionID, ValueID) const;
    bool mayBeOtherThanObject(const ClosureFieldFreshObject&, const FieldInitializer&) const;

    void reject(ClosureFieldRow, Rejection);
    void rejectFieldsNamedBy(const Value& access, Rejection);
    void rejectFieldsOfConstantData();
    void rejectFieldsStoredOutsideInitialization();
    void rejectFieldsGivenOtherThanObjects();
    void findFieldsGivenOtherClosures();
    void noteOtherClosureIn(ClosureFieldRow);
    bool storesClosureOverReceiver(const Function&, const Value& store, FunctionID closureFunction) const;
    bool rejectFieldsListedWithOtherThanObjects(const ClosureFieldFreshObject&, const Vector<Fold, 16>&);
    bool rejectFieldsReadWhereTheyMayHaveLeftTheSlot();
    bool rejectFieldsOverEnvironments(LayoutID);
    bool countFolds();
    bool keepStoresNotFoldedByEveryWindow();
    bool rejectFieldsNotInitializedByEveryAllocation();
    bool rejectFieldsOverSeveralEnvironments();

    void forEachCandidateNamedBy(const Value& access, const Invocable<void(ClosureFieldRow)> auto& functor) const
    {
        for (LayoutID member : m_program.at(access.receivers()).members) {
            std::optional<uint32_t> field = m_program.at(member).findField(access.key());
            if (field && m_states[rowOf(member, *field)].function)
                functor(rowOf(member, *field));
        }
    }

    void forEachCandidateReadByName(const Function& function, const Value& access, const Invocable<void(FieldReference)> auto& functor) const
    {
        if (!access.numberOfOperands() || !access.operand(0))
            return;
        for (LayoutID member : membersOf(m_program, m_program.typeOf(function, access.operand(0)))) {
            std::optional<uint32_t> field = m_program.at(member).findField(access.key());
            if (field && m_states[rowOf(member, *field)].function)
                functor({ member, *field });
        }
    }

    const Program& m_program;
    BuildContext& m_build;
    FieldRows m_rows;
    Vector<uint32_t> m_largeFunctions;
    Vector<uint32_t> m_allocationsOfLayouts;
    Vector<uint32_t> m_allocationsUnderPrototypes;
    Vector<uint32_t> m_dataMembersOfFields;
    Vector<ClosureFieldFreshObject> m_objects;
    Vector<ClosureStore> m_closureStores;
    Vector<FieldState> m_states;
    Vector<FieldReference> m_candidates;
    BitVector m_keysOfCandidates;
    BitVector m_layoutsWithCandidates;
    BitVector m_layoutsAllocatedOnlyByOperations;
    BitVector m_layoutsWithEnvironmentCandidates;
    BitVector m_mayHoldOtherClosure;
    BitVector m_isCalleeOfWindow;
    Vector<CallSiteState> m_callSites;
    Vector<CalleeState> m_callees;
    Vector<CalleeStoreState> m_calleeStores;
    Vector<ValueReference> m_functionEntries;
    Vector<ValueReference> m_stores;
    Vector<ValueReference> m_initializingStores;
    Vector<ValueReference> m_allocations;
    Vector<ValueReference> m_reads;
    Vector<ValueReference> m_presenceTests;
    Vector<ValueReference> m_namedAccesses;
    Vector<ValueReference> m_keyOrderTests;
    Vector<ValueReference> m_removedStores;
    Vector<ClosureEnvironmentField> m_environmentFields;
    uint64_t m_numberOfDeferringFields { 0 };
};

void DeferralAnalysis::findObjects()
{
    DeferrableLayouts deferrableLayouts = findDeferrableLayouts(m_program);
    m_allocationsOfLayouts.fill(0, m_program.layouts.size());
    m_allocationsUnderPrototypes.fill(0, m_program.layouts.size());
    m_dataMembersOfFields.fill(0, m_rows.count());
    std::span<uint32_t> allocationsOfLayouts = m_allocationsOfLayouts.mutableSpan();
    std::span<uint32_t> allocationsUnderPrototypes = m_allocationsUnderPrototypes.mutableSpan();
    std::span<uint32_t> dataMembersOfFields = m_dataMembersOfFields.mutableSpan();
    const ParallelLoops& loops = m_build.parallelLoops();
    forEachIndexInParallel(loops, m_program.data.size(), [&](unsigned index) {
        forEachDataNodeInPreorder(m_program.data[index].root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object || !object->layout)
                return;
            countAllocation(allocationsOfLayouts, object->layout);
            for (const DataMember& member : object->members) {
                if (member.value.is<DataNode::Undefined>() || member.value.is<DataNode::Null>())
                    continue;
                if (std::optional<uint32_t> field = m_program.at(object->layout).findField(member.key))
                    WTF::atomicExchangeAdd(&dataMembersOfFields[rowOf(object->layout, *field)], 1u, std::memory_order_relaxed);
            }
        });
    });
    Vector<ScannedFunction> scanned = collectInParallel<ScannedFunction>(loops, m_program.functions.size(), m_largeFunctions.span(), [&](unsigned index, Vector<ScannedFunction>& found) {
        if (!mayListValueWith(m_program, FunctionID(index), FreshObjectScanner::scannedOpcodes()) || !m_program.functions[index].hasBody())
            return;
        ScannedFunction function;
        FreshObjectScanner(m_program, m_build, deferrableLayouts, FunctionID(index)).run(allocationsOfLayouts, allocationsUnderPrototypes, function);
        if (!function.objects.isEmpty() || !function.closureStores.isEmpty())
            found.append(WTF::move(function));
    });
    for (ScannedFunction& function : scanned) {
        for (ClosureFieldFreshObject& object : function.objects)
            m_objects.append(WTF::move(object));
        m_closureStores.appendVector(function.closureStores);
    }
    m_layoutsAllocatedOnlyByOperations = WTF::move(deferrableLayouts.allocatedOnlyByOperations);
}

void DeferralAnalysis::reject(ClosureFieldRow row, Rejection rejection)
{
    if (m_states[row].rejection == Rejection::None)
        m_states[row].rejection = rejection;
}

// A slot leaves the closures of one function unmade.
void DeferralAnalysis::findCandidates()
{
    struct Place {
        FieldReference field;
        FunctionID function;
        ValueReference operation;

        bool operator==(const Place&) const = default;
        auto operator<=>(const Place&) const = default;
    };
    Vector<Place> places;
    for (const ClosureFieldFreshObject& object : m_objects) {
        forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
            if (initializer.closureFunction)
                places.append({ { object.layout, initializer.field }, initializer.closureFunction, initializer.store ? initializer.store : ValueReference { object.function, object.allocation } });
        });
    }
    for (const ClosureStore& store : m_closureStores)
        places.append({ { store.layout, store.field }, store.closureFunction, { store.function, store.store } });
    AOT::sort(places);
    removeRepeatedElements(places);

    using Maker = std::pair<LayoutID, FunctionID>;
    Vector<Maker> makers = places.map([](const Place& place) {
        return Maker { place.field.layout, place.operation.function };
    });
    AOT::sort(makers);
    auto numberOfPlacesOfMaker = [&](const Place& place) {
        return std::ranges::equal_range(makers, Maker { place.field.layout, place.operation.function }).size();
    };

    m_states.fill({ }, m_rows.count());
    m_keysOfCandidates.ensureSize(m_program.types.numberOfPropertyKeys());
    m_layoutsWithCandidates.ensureSize(m_program.layouts.size());
    m_layoutsWithEnvironmentCandidates.ensureSize(m_program.layouts.size());
    for (size_t first = 0; first < places.size();) {
        FieldReference field = places[first].field;
        FunctionID chosen;
        std::pair<size_t, size_t> weightOfChosen;
        bool chosenKeepsEnvironment = true;
        size_t numberOfFunctions = 0;
        size_t next = first;
        while (next < places.size() && places[next].field == field) {
            FunctionID function = places[next].function;
            std::pair<size_t, size_t> weight;
            for (; next < places.size() && places[next].field == field && places[next].function == function; ++next) {
                ++weight.first;
                weight.second = std::max(weight.second, numberOfPlacesOfMaker(places[next]));
            }
            ++numberOfFunctions;
            bool keepsEnvironment = deferralFor(m_program, function) == ClosureDeferral::CapturesEnvironment;
            bool isBetter = !chosen || (chosenKeepsEnvironment && !keepsEnvironment) || (chosenKeepsEnvironment == keepsEnvironment && weight > weightOfChosen);
            if (!isBetter)
                continue;
            chosen = function;
            weightOfChosen = weight;
            chosenKeepsEnvironment = keepsEnvironment;
        }
        first = next;

        ClosureFieldRow row = rowOf(field.layout, field.field);
        m_states[row].function = chosen;
        m_candidates.append(field);
        m_keysOfCandidates.quickSet(fieldAt(m_program, field).key.index());
        if (!chosenKeepsEnvironment)
            continue;
        m_layoutsWithEnvironmentCandidates.quickSet(field.layout.index());
        if (numberOfFunctions > 1)
            reject(row, Rejection::InitializedWithSeveralFunctions);
    }

    for (size_t index = 0; index < m_program.layouts.size(); ++index) {
        for (uint32_t field = 0; field < m_rows.numberOfFields(LayoutID(index)); ++field) {
            if (m_states[rowOf(LayoutID(index), field)].function)
                m_layoutsWithCandidates.quickSet(index);
        }
    }
}

void DeferralAnalysis::findEnteredCalls()
{
    Vector<EnteredCall> calls;
    for (const ClosureFieldFreshObject& object : m_objects)
        calls.appendVector(object.enteredCalls);
    if (calls.isEmpty())
        return;

    AOT::sort(calls, { }, &EnteredCall::call);
    for (const EnteredCall& call : calls) {
        if (m_callSites.isEmpty() || m_callSites.last().call != call.call)
            m_callSites.append({ call.call, call.callee, call.passesOwnReceiver, 0 });
        ++m_callSites.last().numberOfWindows;
    }

    m_isCalleeOfWindow.ensureSize(m_program.functions.size());
    for (const CallSiteState& site : m_callSites) {
        if (!m_isCalleeOfWindow.quickSet(site.callee.index()))
            m_callees.append({ .function = site.callee });
    }
    AOT::sort(m_callees, { }, &CalleeState::function);
    for (const CallSiteState& site : m_callSites) {
        CalleeState& callee = findSorted(m_callees, site.callee, &CalleeState::function);
        ++callee.numberOfEnteredSites;
        callee.numberOfWindows += site.numberOfWindows;
    }
}

bool DeferralAnalysis::entersCalleeOfWindow(const Value& value) const
{
    FunctionID function = value.function();
    if (m_callees.isEmpty() || !function || !m_isCalleeOfWindow.get(function.index()))
        return false;
    bool entersFunction = false;
    forEachFunctionReference(value, [&](FunctionID, FunctionReferenceKind kind) {
        entersFunction |= keepsFunctionAlive(kind);
    });
    return entersFunction;
}

bool DeferralAnalysis::namesCandidate(const Value& access) const
{
    bool result = false;
    forEachCandidateNamedBy(access, [&](ClosureFieldRow) {
        result = true;
    });
    return result;
}

std::optional<ClosureFieldAccessKind> DeferralAnalysis::classifyAccess(const Function& function, const Value& value) const
{
    if (Value::opcodesWithFunction.contains(value.opcode))
        return entersCalleeOfWindow(value) ? std::optional(ClosureFieldAccessKind::FunctionEntry) : std::nullopt;
    if (value.opcode == Opcode::HasKeyOrder)
        return m_layoutsWithEnvironmentCandidates.quickGet(value.layout().index()) ? std::optional(ClosureFieldAccessKind::KeyOrderTest) : std::nullopt;
    if (value.opcode == Opcode::NewObject || value.opcode == Opcode::CloneAs)
        return m_layoutsWithCandidates.quickGet(value.layout().index()) ? std::optional(ClosureFieldAccessKind::Allocation) : std::nullopt;
    bool isTestInFrontOfReads = value.opcode == Opcode::IsSlotFilled && !value.accessSite();
    if (!Value::opcodesWithKey.contains(value.opcode) || isTestInFrontOfReads || value.opcode == Opcode::GetDeferredClosureSlot || value.opcode == Opcode::GetDeferredClosureSlotOrUndefined)
        return std::nullopt;

    PropertyKeyID key = value.key();
    if (!key || TypeTable::isProvisional(key) || !m_keysOfCandidates.quickGet(key.index()))
        return std::nullopt;
    if (value.opcode == Opcode::IsSlotFilled)
        return namesCandidate(value) ? std::optional(ClosureFieldAccessKind::PresenceTest) : std::nullopt;
    if (Value::opcodesWithFieldAccess.contains(value.opcode)) {
        if (!namesCandidate(value))
            return std::nullopt;
        return storedOf(value.opcode) ? ClosureFieldAccessKind::SlotStore : ClosureFieldAccessKind::SlotRead;
    }

    bool readsCandidate = false;
    forEachCandidateReadByName(function, value, [&](FieldReference) {
        readsCandidate = true;
    });
    return readsCandidate ? std::optional(ClosureFieldAccessKind::NamedAccess) : std::nullopt;
}

void DeferralAnalysis::findAccesses()
{
    for (const ClosureFieldFreshObject& object : m_objects) {
        for (const FieldInitializer& initializer : object.initializers) {
            if (initializer.store)
                m_initializingStores.append(initializer.store);
        }
    }
    AOT::sort(m_initializingStores);

    Vector<FoundAccess> accesses = collectInParallel<FoundAccess>(m_build.parallelLoops(), m_program.functions.size(), m_largeFunctions.span(), [&](unsigned index, Vector<FoundAccess>& found) {
        // classifyAccess() answers for no other operation.
        constexpr OpcodeSet classifiedOpcodes = Value::opcodesWithFunction | Value::opcodesWithKey | OpcodeSet { Opcode::HasKeyOrder, Opcode::NewObject, Opcode::CloneAs };
        const Function& function = m_program.functions[index];
        // It answers for no operation all of whose names are refused here. Whoever adds a case to it that goes by something else widens this.
        auto mayBeClassified = [&](NameKind kind, uint32_t name) {
            switch (kind) {
            case NameKind::Key:
                return m_keysOfCandidates.get(name);
            case NameKind::Function:
                return m_isCalleeOfWindow.get(name);
            case NameKind::Layout:
                return m_layoutsWithCandidates.get(name) || m_layoutsWithEnvironmentCandidates.get(name);
            case NameKind::Variable:
                return false;
            }
            RELEASE_ASSERT_NOT_REACHED();
        };
        forEachListedValueNaming(m_program, FunctionID(index), classifiedOpcodes, mayBeClassified, [&](BlockID, ValueID id, const Value& value) {
            if (std::optional<ClosureFieldAccessKind> kind = classifyAccess(function, value))
                found.append({ { FunctionID(index), id }, *kind });
        });
    });

    for (const FoundAccess& access : accesses) {
        switch (access.kind) {
        case ClosureFieldAccessKind::SlotRead:
            m_reads.append(access.reference);
            break;
        case ClosureFieldAccessKind::SlotStore:
            m_stores.append(access.reference);
            break;
        case ClosureFieldAccessKind::PresenceTest:
            m_presenceTests.append(access.reference);
            break;
        case ClosureFieldAccessKind::Allocation:
            m_allocations.append(access.reference);
            break;
        case ClosureFieldAccessKind::NamedAccess:
            m_namedAccesses.append(access.reference);
            break;
        case ClosureFieldAccessKind::KeyOrderTest:
            m_keyOrderTests.append(access.reference);
            break;
        case ClosureFieldAccessKind::FunctionEntry:
            m_functionEntries.append(access.reference);
            break;
        }
    }
}

void DeferralAnalysis::findCalleesEnteredOnlyFromWindows()
{
    for (const ValueReference& entry : m_functionEntries)
        ++findSorted(m_callees, valueAt(entry).function(), &CalleeState::function).numberOfEntrySites;
    for (CalleeState& callee : m_callees)
        callee.isEnteredOnlyFromWindows = callee.numberOfEntrySites == callee.numberOfEnteredSites;

    bool didChange = true;
    while (didChange) {
        didChange = false;
        for (const CallSiteState& site : m_callSites) {
            if (!site.passesOwnReceiver)
                continue;
            CalleeState& callee = findSorted(m_callees, site.callee, &CalleeState::function);
            const CalleeState& caller = findSorted(m_callees, site.call.function, &CalleeState::function);
            if (!callee.isEnteredOnlyFromWindows || (caller.isEnteredOnlyFromWindows && site.numberOfWindows == caller.numberOfWindows))
                continue;
            callee.isEnteredOnlyFromWindows = false;
            didChange = true;
        }
    }
}

void DeferralAnalysis::findCalleeStores()
{
    Vector<ValueReference> stores;
    for (const ClosureFieldFreshObject& object : m_objects) {
        for (const FieldInitializer& initializer : object.initializers) {
            if (initializer.isInCallee)
                stores.append(initializer.store);
        }
    }
    AOT::sort(stores);
    for (const ValueReference& store : stores) {
        if (m_calleeStores.isEmpty() || m_calleeStores.last().store != store)
            m_calleeStores.append({ .store = store });
        ++m_calleeStores.last().numberOfWindows;
    }
    for (CalleeStoreState& store : m_calleeStores) {
        const CalleeState& callee = findSorted(m_callees, store.store.function, &CalleeState::function);
        store.canLeaveCallee = callee.isEnteredOnlyFromWindows && store.numberOfWindows == callee.numberOfWindows;
    }
}

void DeferralAnalysis::rejectFieldsNamedBy(const Value& access, Rejection rejection)
{
    forEachCandidateNamedBy(access, [&](ClosureFieldRow row) {
        reject(row, rejection);
    });
}

// The hidden field that keeps the environment is filled by allocations alone, so every allocation has to list the closure and nothing else may come into the field.
bool DeferralAnalysis::keepsEnvironment(const FieldState& state) const
{
    return deferralFor(m_program, state.function) == ClosureDeferral::CapturesEnvironment;
}

// The slot has a word for a closure that is not made, one for an absent field, and what a narrow reference holds: undefined, null and objects.
static bool mayBeOtherThanObjectOrNullish(const Type& type)
{
    return type.primitives & ~Primitive::UndefinedOrNull;
}

bool DeferralAnalysis::mayBeOtherThanObject(FunctionID function, ValueID value) const
{
    return !value || mayBeOtherThanObjectOrNullish(m_program.typeOf(m_program.at(function), value));
}

bool DeferralAnalysis::mayBeOtherThanObject(const ClosureFieldFreshObject& object, const FieldInitializer& initializer) const
{
    if (initializer.calleeValue != noCalleeValue)
        return mayBeOtherThanObjectOrNullish(m_program.at(object.calleeValues[initializer.calleeValue].type));
    return mayBeOtherThanObject(object.function, initializer.value);
}

void DeferralAnalysis::rejectFieldsOfConstantData()
{
    for (FieldReference candidate : m_candidates) {
        ClosureFieldRow row = rowOf(candidate.layout, candidate.field);
        if (m_dataMembersOfFields[row])
            reject(row, Rejection::IsPartOfConstantData);
    }
}

void DeferralAnalysis::rejectFieldsStoredOutsideInitialization()
{
    for (const ValueReference& store : m_stores) {
        if (std::ranges::binary_search(m_initializingStores, store))
            continue;
        forEachCandidateNamedBy(valueAt(store), [&](ClosureFieldRow row) {
            if (keepsEnvironment(m_states[row]))
                reject(row, Rejection::StoredAfterInitialization);
        });
    }
}

// What the slot cannot hold takes the property out of its slot.
void DeferralAnalysis::rejectFieldsGivenOtherThanObjects()
{
    for (FieldReference candidate : m_candidates) {
        const LocationFlow& location = flowOf(m_program, candidate).location;
        bool everyWriterIsFound = location.writers == WriterState::AllKnown && location.storeNaming == StoreNaming::EveryStoreNamesLocation;
        if (isNeverMovedOutByStore(m_program, candidate) && !everyWriterIsFound && mayBeOtherThanObjectOrNullish(m_program.at(slotTypeOf(m_program, fieldAt(m_program, candidate)))))
            reject(rowOf(candidate.layout, candidate.field), Rejection::HoldsOtherThanObjects);
    }
    for (const ValueReference& reference : m_stores) {
        const Value& store = valueAt(reference);
        if (mayBeOtherThanObject(reference.function, store.operand(*storedOf(store.opcode))))
            rejectFieldsNamedBy(store, Rejection::HoldsOtherThanObjects);
    }
    for (const ValueReference& reference : m_namedAccesses) {
        const Value& access = valueAt(reference);
        std::optional<uint8_t> stored = storedOf(access.opcode);
        if (!stored || !mayBeOtherThanObject(reference.function, access.operand(*stored)))
            continue;
        forEachCandidateReadByName(m_program.at(reference.function), access, [&](FieldReference field) {
            if (isNeverMovedOutByStore(m_program, field))
                reject(rowOf(field.layout, field.field), Rejection::HoldsOtherThanObjects);
        });
    }

    Vector<ValueReference> describedAllocations = m_objects.map([](const ClosureFieldFreshObject& object) {
        return ValueReference { object.function, object.allocation };
    });
    AOT::sort(describedAllocations);
    for (const ValueReference& reference : m_allocations) {
        if (std::ranges::binary_search(describedAllocations, reference))
            continue;
        const Value& allocation = valueAt(reference);
        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> values = allocation.variadicOperands();
        for (size_t index = 0; index < fields.size(); ++index) {
            ClosureFieldRow row = rowOf(allocation.layout(), fields[index]);
            if (!m_states[row].function)
                continue;
            noteOtherClosureIn(row);
            if (mayBeOtherThanObject(reference.function, values[index]))
                reject(row, Rejection::HoldsOtherThanObjects);
        }
    }
}

void DeferralAnalysis::noteOtherClosureIn(ClosureFieldRow row)
{
    m_mayHoldOtherClosure.quickSet(row);
}

// What is undefined or null is no closure over anything.
bool DeferralAnalysis::storesClosureOverReceiver(const Function& function, const Value& store, FunctionID closureFunction) const
{
    ValueID stored = store.operand(*storedOf(store.opcode));
    if (stored && m_program.typeOf(function, stored).isUndefinedOrNull())
        return true;
    const Value& closure = function.at(definitionThroughUpcasts(function, stored));
    if (closure.opcode != Opcode::NewClosure || closure.function() != closureFunction || closure.isMadeOnDemand())
        return false;
    std::span<const ValueID> captures = closure.capturesOperands();
    return captures.size() == 1 && sameObjectThroughCasts(function, captures[0]) == sameObjectThroughCasts(function, store.operand(0));
}

// Where every closure that comes into the field is made over the object that holds the field, whoever wants what the closure captures has it at hand: the object.
void DeferralAnalysis::findFieldsGivenOtherClosures()
{
    m_mayHoldOtherClosure.ensureSize(m_rows.count());
    for (const ValueReference& reference : m_stores) {
        const Value& store = valueAt(reference);
        forEachCandidateNamedBy(store, [&](ClosureFieldRow row) {
            if (!storesClosureOverReceiver(m_program.at(reference.function), store, m_states[row].function))
                noteOtherClosureIn(row);
        });
    }
    for (const ValueReference& reference : m_namedAccesses) {
        const Value& access = valueAt(reference);
        if (!storedOf(access.opcode))
            continue;
        forEachCandidateReadByName(m_program.at(reference.function), access, [&](FieldReference field) {
            ClosureFieldRow row = rowOf(field.layout, field.field);
            if (!storesClosureOverReceiver(m_program.at(reference.function), access, m_states[row].function))
                noteOtherClosureIn(row);
        });
    }
    for (const ValueReference& reference : m_allocations) {
        const Value& allocation = valueAt(reference);
        if (allocation.opcode == Opcode::NewObject)
            continue;
        for (uint32_t field = 0; field < m_rows.numberOfFields(allocation.layout()); ++field)
            noteOtherClosureIn(rowOf(allocation.layout(), field));
    }
}

bool DeferralAnalysis::findsClosureOverReceiver(const Value& read) const
{
    auto& members = m_program.at(read.receivers()).members;
    return !members.isEmpty() && std::ranges::all_of(members, [&](LayoutID member) {
        std::optional<uint32_t> field = m_program.at(member).findField(read.key());
        if (!field)
            return false;
        const FieldState& state = stateOf(member, *field);
        const LocationFlow& location = flowOf(m_program, { member, *field }).location;
        return state.canDefer()
            && deferralFor(m_program, state.function) == ClosureDeferral::CapturesObject
            && !m_mayHoldOtherClosure.quickGet(rowOf(member, *field))
            && location.writers == WriterState::AllKnown
            && location.storeNaming == StoreNaming::EveryStoreNamesLocation;
    });
}

bool DeferralAnalysis::rejectFieldsListedWithOtherThanObjects(const ClosureFieldFreshObject& object, const Vector<Fold, 16>& folds)
{
    bool isRewritten = defersAnyClosure(object);
    bool didReject = false;
    Vector<uint32_t, 16> visitedFields;
    for (size_t index = object.initializers.size(); index--;) {
        const FieldInitializer& initializer = object.initializers[index];
        bool isListed = isRewritten ? folds[index] != Fold::None : !initializer.store;
        if (!isListed || visitedFields.contains(initializer.field))
            continue;
        visitedFields.append(initializer.field);
        if (!stateOf(object.layout, initializer.field).canDefer() || defers(object, initializer))
            continue;
        noteOtherClosureIn(rowOf(object.layout, initializer.field));
        if (!mayBeOtherThanObject(object, initializer))
            continue;
        reject(rowOf(object.layout, initializer.field), Rejection::HoldsOtherThanObjects);
        didReject = true;
    }
    return didReject;
}

bool DeferralAnalysis::defers(const ClosureFieldFreshObject& object, const FieldInitializer& initializer) const
{
    const FieldState& state = stateOf(object.layout, initializer.field);
    return initializer.closureFunction && state.canDefer() && state.function == initializer.closureFunction;
}

bool DeferralAnalysis::defers(const ClosureStore& store) const
{
    const FieldState& state = stateOf(store.layout, store.field);
    return state.canDefer() && state.function == store.closureFunction;
}

bool DeferralAnalysis::isFilledByEveryAllocation(FieldReference reference) const
{
    uint32_t numberOfAllocations = m_allocationsOfLayouts[reference.layout.index()];
    return numberOfAllocations && m_layoutsAllocatedOnlyByOperations.quickGet(reference.layout.index()) && stateOf(reference.layout, reference.field).numberOfInitializingAllocations == numberOfAllocations;
}

bool DeferralAnalysis::defersAnyClosure(const ClosureFieldFreshObject& object) const
{
    return std::ranges::any_of(object.initializers, [&](const FieldInitializer& initializer) {
        return defers(object, initializer);
    });
}

Vector<Fold, 16> DeferralAnalysis::selectFolds(const ClosureFieldFreshObject& object) const
{
    Vector<Fold, 16> folds(FillWith { }, object.initializers.size(), Fold::None);
    if (!defersAnyClosure(object))
        return folds;

    Vector<uint32_t, 16> listedFields;
    Vector<uint32_t, 4> fieldsWithRemainingStores;
    bool canListMoreFields = true;
    for (size_t index = 0; index < object.initializers.size(); ++index) {
        const FieldInitializer& initializer = object.initializers[index];
        bool isListed = listedFields.contains(initializer.field);
        bool canFold = !initializer.store
            || ((initializer.isAvailableAtAllocation || defers(object, initializer))
                && !fieldsWithRemainingStores.contains(initializer.field)
                && (isListed || canListMoreFields)
                && (!initializer.isInCallee || findSorted(m_calleeStores, initializer.store, &CalleeStoreState::store).canLeaveCallee));
        if (canFold) {
            folds[index] = Fold::Value;
            if (!isListed)
                listedFields.append(initializer.field);
            continue;
        }

        fieldsWithRemainingStores.append(initializer.field);
        if (isListed)
            continue;
        if (canListMoreFields && !stateOf(object.layout, initializer.field).canDefer() && tryCreatePlaceholder(m_program, { object.layout, initializer.field })) {
            folds[index] = Fold::Placeholder;
            listedFields.append(initializer.field);
        } else
            canListMoreFields = false;
    }

    for (size_t index = folds.size(); index-- && folds[index] != Fold::Value;)
        folds[index] = Fold::None;
    return folds;
}

bool DeferralAnalysis::countFolds()
{
    for (FieldReference candidate : m_candidates)
        m_states[rowOf(candidate.layout, candidate.field)].numberOfInitializingAllocations = 0;
    for (CalleeStoreState& store : m_calleeStores)
        store.numberOfFolds = 0;

    bool didReject = false;
    for (const ClosureFieldFreshObject& object : m_objects) {
        Vector<Fold, 16> folds = selectFolds(object);
        didReject |= rejectFieldsListedWithOtherThanObjects(object, folds);
        for (size_t index = 0; index < folds.size(); ++index) {
            if (folds[index] == Fold::Value && object.initializers[index].isInCallee)
                ++findSorted(m_calleeStores, object.initializers[index].store, &CalleeStoreState::store).numberOfFolds;
        }
        forEachFinalInitializer(object, [&](size_t index, const FieldInitializer& initializer) {
            if (folds[index] == Fold::Value && defers(object, initializer))
                ++m_states[rowOf(object.layout, initializer.field)].numberOfInitializingAllocations;
        });
    }
    return didReject;
}

bool DeferralAnalysis::keepStoresNotFoldedByEveryWindow()
{
    bool didChange = false;
    for (CalleeStoreState& store : m_calleeStores) {
        if (!store.canLeaveCallee || store.numberOfFolds == store.numberOfWindows)
            continue;
        store.canLeaveCallee = false;
        didChange = true;
    }
    return didChange;
}

bool DeferralAnalysis::rejectFieldsNotInitializedByEveryAllocation()
{
    bool didReject = false;
    for (FieldReference candidate : m_candidates) {
        FieldState& state = m_states[rowOf(candidate.layout, candidate.field)];
        uint32_t numberOfAllocations = m_allocationsOfLayouts[candidate.layout.index()];
        if (!state.canDefer() || !keepsEnvironment(state) || state.numberOfInitializingAllocations == numberOfAllocations)
            continue;
        bool isInitializedByAllOthers = state.numberOfInitializingAllocations + m_allocationsUnderPrototypes[candidate.layout.index()] == numberOfAllocations;
        state.rejection = isInitializedByAllOthers ? Rejection::AllocationMaySelectOtherClass : Rejection::NotInitializedByEveryAllocation;
        didReject = true;
    }
    return didReject;
}

// A read takes an empty slot for an absent property behind a test that no field of the object lies outside its slot, or without one where the property stays in its slot (staysInSlot()).
bool DeferralAnalysis::rejectFieldsReadWhereTheyMayHaveLeftTheSlot()
{
    auto staysInSlot = [&](const Value& access) {
        return std::ranges::all_of(m_program.at(access.receivers()).members, [&](LayoutID member) {
            std::optional<uint32_t> field = m_program.at(member).findField(access.key());
            if (!field)
                return true;
            FieldReference reference { member, *field };
            return isNeverMovedOutByStore(m_program, reference) || !slotTakesEveryValue(m_program, fieldAt(m_program, declarationOf(m_program, reference)));
        });
    };
    bool didReject = false;
    for (const ValueReference& reference : m_reads) {
        const Value& read = valueAt(reference);
        if (read.opcode != Opcode::GetSlotOrUndefined || !namesDeferringField(read))
            continue;
        // A read that gives any value looks among the extra properties where it finds the slot empty and the object says that a field lies outside its slot.
        if (staysInSlot(read) || read.type == TypeTable::anything)
            continue;
        rejectFieldsNamedBy(read, Rejection::ReadWhereItMayHaveLeftSlot);
        didReject = true;
    }
    for (const ValueReference& reference : m_presenceTests) {
        const Value& test = valueAt(reference);
        if (!namesDeferringField(test) || staysInSlot(test))
            continue;
        rejectFieldsNamedBy(test, Rejection::ReadWhereItMayHaveLeftSlot);
        didReject = true;
    }
    return didReject;
}

// A read that gives undefined for an empty slot has no guard that says the slot is filled.
bool DeferralAnalysis::mayFindSlotEmpty(const Value& read) const
{
    if (read.opcode != Opcode::GetSlotOrUndefined)
        return false;
    return !std::ranges::all_of(m_program.at(read.receivers()).members, [&](LayoutID member) {
        std::optional<uint32_t> field = m_program.at(member).findField(read.key());
        if (!field)
            return false;
        const FieldFlow& flow = flowOf(m_program, { member, *field });
        bool isFilledWithClosure = stateOf(member, *field).canDefer() && isFilledByEveryAllocation({ member, *field });
        return flow.stability.containsAll(stabilityOfFilledSlot) && (isFilledWithClosure || flow.occupancy == Occupancy::AlwaysFilled);
    });
}

bool DeferralAnalysis::rejectFieldsOverEnvironments(LayoutID layout)
{
    bool didReject = false;
    for (uint32_t field = 0; field < m_rows.numberOfFields(layout); ++field) {
        FieldState& state = m_states[rowOf(layout, field)];
        if (!state.canDefer() || deferralFor(m_program, state.function) != ClosureDeferral::CapturesEnvironment)
            continue;
        state.rejection = Rejection::CapturesSeveralEnvironments;
        didReject = true;
    }
    return didReject;
}

bool DeferralAnalysis::rejectFieldsOverSeveralEnvironments()
{
    bool didReject = false;
    for (const ClosureFieldFreshObject& object : m_objects) {
        if (!m_layoutsWithEnvironmentCandidates.quickGet(object.layout.index()))
            continue;
        ValueID environment;
        bool hasSeveralEnvironments = false;
        forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
            if (!initializer.environment || !defers(object, initializer))
                return;
            hasSeveralEnvironments |= environment && environment != initializer.environment;
            environment = initializer.environment;
        });
        if (hasSeveralEnvironments)
            didReject |= rejectFieldsOverEnvironments(object.layout);
    }

    for (size_t index : m_layoutsWithEnvironmentCandidates) {
        LayoutID id(index);
        const Layout& layout = m_program.at(id);
        TypeID type;
        if (layout.flow.closureEnvironmentField)
            type = layout.fields[*layout.flow.closureEnvironmentField].declared.type;
        bool hasSeveralTypes = false;
        for (uint32_t field = 0; field < m_rows.numberOfFields(id); ++field) {
            const FieldState& state = stateOf(id, field);
            if (!state.canDefer() || deferralFor(m_program, state.function) != ClosureDeferral::CapturesEnvironment)
                continue;
            TypeID captured = m_program.at(state.function).captures[0].type;
            hasSeveralTypes |= type && type != captured;
            type = captured;
        }
        if (hasSeveralTypes)
            didReject |= rejectFieldsOverEnvironments(id);
    }
    return didReject;
}

bool DeferralAnalysis::namesDeferringField(const Value& access) const
{
    bool result = false;
    forEachCandidateNamedBy(access, [&](ClosureFieldRow row) {
        result |= m_states[row].canDefer();
    });
    return result;
}

bool DeferralAnalysis::becomesAlwaysFilled(FieldReference reference) const
{
    const FieldFlow& flow = flowOf(m_program, reference);
    return stateOf(reference.layout, reference.field).canDefer() && isFilledByEveryAllocation(reference) && flow.occupancy != Occupancy::AlwaysFilled && flow.stability.containsAll(stabilityOfFilledSlot);
}

const ClosureEnvironmentField* DeferralAnalysis::findEnvironmentField(LayoutID layout) const
{
    auto found = std::ranges::lower_bound(m_environmentFields, layout, { }, &ClosureEnvironmentField::layout);
    return found != m_environmentFields.end() && found->layout == layout ? &*found : nullptr;
}

ValueID DeferralAnalysis::environmentOf(const ClosureFieldFreshObject& object) const
{
    ValueID environment;
    forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
        if (initializer.environment && defers(object, initializer))
            environment = initializer.environment;
    });
    return environment;
}

void DeferralAnalysis::finish()
{
    for (FieldReference candidate : m_candidates) {
        const FieldState& state = stateOf(candidate.layout, candidate.field);
        if (!state.canDefer())
            continue;
        ++m_numberOfDeferringFields;
        if (deferralFor(m_program, state.function) != ClosureDeferral::CapturesEnvironment)
            continue;
        const Layout& layout = m_program.at(candidate.layout);
        std::optional<uint32_t> existingField = layout.flow.closureEnvironmentField;
        m_environmentFields.append({ candidate.layout, existingField.value_or(static_cast<uint32_t>(layout.fields.size())), m_program.at(state.function).captures[0].type, !existingField });
    }
    AOT::sort(m_environmentFields, { }, &ClosureEnvironmentField::layout);
    removeRepeatedElements(m_environmentFields, [](const ClosureEnvironmentField& a, const ClosureEnvironmentField& b) {
        return a.layout == b.layout;
    });

    for (const CalleeStoreState& store : m_calleeStores) {
        if (store.canLeaveCallee)
            m_removedStores.append(store.store);
    }
    m_objects.removeAllMatching([&](const ClosureFieldFreshObject& object) {
        return !defersAnyClosure(object);
    });

    Vector<ValueReference> storesListedByAllocations = m_removedStores;
    for (const ClosureFieldFreshObject& object : m_objects) {
        Vector<Fold, 16> folds = selectFolds(object);
        for (size_t index = 0; index < folds.size(); ++index) {
            if (folds[index] == Fold::Value && object.initializers[index].store)
                storesListedByAllocations.append(object.initializers[index].store);
        }
    }
    AOT::sort(storesListedByAllocations);
    m_closureStores.removeAllMatching([&](const ClosureStore& store) {
        return !defers(store) || std::ranges::binary_search(storesListedByAllocations, ValueReference { store.function, store.store });
    });
    m_reads.removeAllMatching([&](const ValueReference& reference) {
        return !namesDeferringField(valueAt(reference));
    });
    m_namedAccesses.removeAllMatching([&](const ValueReference& reference) {
        bool isNarrowed = false;
        forEachCandidateReadByName(m_program.at(reference.function), valueAt(reference), [&](FieldReference field) {
            isNarrowed |= becomesAlwaysFilled(field);
        });
        return !isNarrowed;
    });
    m_keyOrderTests.removeAllMatching([&](const ValueReference& reference) {
        const ClosureEnvironmentField* field = findEnvironmentField(valueAt(reference).layout());
        return !field || !field->isNew;
    });
}

void DeferralAnalysis::run()
{
    findObjects();
    if (m_objects.isEmpty() && m_closureStores.isEmpty())
        return;
    findCandidates();
    findEnteredCalls();
    findAccesses();
    findCalleesEnteredOnlyFromWindows();
    findCalleeStores();
    rejectFieldsOfConstantData();
    rejectFieldsStoredOutsideInitialization();
    findFieldsGivenOtherClosures();
    rejectFieldsGivenOtherThanObjects();

    bool didChange = true;
    while (didChange) {
        didChange = countFolds();
        didChange |= keepStoresNotFoldedByEveryWindow();
        didChange |= rejectFieldsNotInitializedByEveryAllocation();
        didChange |= rejectFieldsReadWhereTheyMayHaveLeftTheSlot();
        didChange |= rejectFieldsOverSeveralEnvironments();
    }
    finish();
}

void DeferralAnalysis::dump(PrintStream& out) const
{
    std::array<uint64_t, numberOfRejections> countsByRejection { };
    uint64_t capturingNothing = 0;
    uint64_t capturingObject = 0;
    uint64_t capturingEnvironment = 0;
    for (FieldReference candidate : m_candidates) {
        const FieldState& state = stateOf(candidate.layout, candidate.field);
        ++countsByRejection[static_cast<size_t>(state.rejection)];
        if (!state.canDefer()) {
            out.println("closure field L", candidate.layout.index(), " field ", candidate.field, ": ", describe(state.rejection));
            continue;
        }
        switch (deferralFor(m_program, state.function)) {
        case ClosureDeferral::Unknown:
            RELEASE_ASSERT_NOT_REACHED();
        case ClosureDeferral::CapturesNothing:
            ++capturingNothing;
            break;
        case ClosureDeferral::CapturesObject:
            ++capturingObject;
            break;
        case ClosureDeferral::CapturesEnvironment:
            ++capturingEnvironment;
            break;
        }
    }
    for (size_t index = 0; index < numberOfRejections; ++index)
        out.println("closure fields: ", countsByRejection[index], " ", describe(static_cast<Rejection>(index)));
    out.println("deferred closure fields: ", capturingNothing, " capture nothing, ", capturingObject, " capture the object, ", capturingEnvironment, " capture an environment");
    out.println("direct calls followed during object initialization: ", m_callSites.size(), ", stores moved from callees into allocations: ", m_removedStores.size(), ", stores that leave their closure unmade: ", m_closureStores.size());
}

class ClosureFieldRewriter {
public:
    ClosureFieldRewriter(const AnalysisContext& context, const DeferralAnalysis& analysis, Function& function, TypeArena& types)
        : m_context(context)
        , m_analysis(analysis)
        , m_function(function)
        , m_types(types)
        , m_builder(context, function, types)
    {
    }

    EditCounts run(const FunctionEdits& edits)
    {
        Vector<ValueID>& narrowedValues = m_narrowedValues;
        for (const ValueReference& reference : edits.reads) {
            if (tryTakeCaptureFromReceiver(reference.value))
                continue;
            Value& read = m_function.edit(reference.value);
            TypeID previousType = read.type;
            becomeReadOfDeferredClosure(read);
            if (read.type != previousType)
                narrowedValues.append(reference.value);
        }
        for (const ValueReference& reference : edits.namedAccesses)
            narrowedValues.append(reference.value);
        for (const ValueReference& reference : edits.keyOrderTests)
            prependEnvironmentField(m_function.edit(reference.value));
        for (const ClosureFieldFreshObject& object : edits.objects)
            rewrite(object);
        removeStores(edits.removedStores);
        for (const ClosureStore& store : edits.deferredStores)
            deferClosureOf(store);

        if (!narrowedValues.isEmpty()) {
            FunctionAnalyses analyses(m_context, m_function);
            narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, analyses, m_types, m_context.build }, narrowedValues.span());
        }
        if (m_didDetachValues)
            compactFunction(m_function);
        return m_counts;
    }

private:
    // A call that is resolved reads the closure only for what it captures, which is the object at hand. A read that nothing else uses goes, and with it the closure that it would make.
    bool tryTakeCaptureFromReceiver(ValueID id)
    {
        const Value& read = m_function.at(id);
        if (!m_analysis.findsClosureOverReceiver(read))
            return false;
        if (!m_analyses)
            m_analyses.emplace(m_context, m_function);
        const UseLists& uses = m_analyses->useLists();
        ValueID receiver = read.objectOperand();
        bool hasOtherUses = false;
        for (const Use& use : uses.usesOf(id)) {
            bool isTaken = use.kind == Use::Kind::Operand
                && m_function.at(use.user).opcode == Opcode::GetCapture
                && m_context.program.typeOf(m_function, receiver).isSubsetOf(m_context.program.typeOf(m_function, use.user), m_context.program);
            if (!isTaken) {
                hasOtherUses = true;
                continue;
            }
            // The object at hand may be known better than what the function says it captures.
            becomeUpcast(m_function, use.user, receiver);
            Value& capture = m_function.edit(use.user);
            TypeID previousType = capture.type;
            capture.type = resultTypeOf(m_context.program, m_function, capture, m_types);
            if (capture.type != previousType)
                m_narrowedValues.append(use.user);
        }
        if (hasOtherUses)
            return false;
        m_function.edit(uses.blockOf(id), BlockEdit::Values).values.removeFirst(id);
        m_didDetachValues = true;
        return true;
    }

    void becomeReadOfDeferredClosure(Value& read)
    {
        Value previous = read;
        // The read that takes a filled slot has a guard that says so, or every receiver keeps the slot filled.
        bool mayFindSlotEmpty = m_analysis.mayFindSlotEmpty(previous);
        read.become(mayFindSlotEmpty ? Opcode::GetDeferredClosureSlotOrUndefined : Opcode::GetDeferredClosureSlot);
        read.setObjectOperand(previous.objectOperand());
        read.setGuardOperand(previous.guardOperand());
        read.setKey(previous.key());
        read.setReceivers(previous.receivers());
        read.setLevel(previous.level());
        if (previous.accessSite())
            read.setAccessSite(previous.accessSite());
        if (!mayFindSlotEmpty)
            read.setHoldsDeclaredType(previous.opcode == Opcode::GetSlot && previous.holdsDeclaredType());
        read.type = resultTypeOf(m_context.program, m_function, read, m_types);
        read.setExpected(expectedTypeOf(m_context.program, m_function, read, m_types));
    }

    void prependEnvironmentField(Value& test)
    {
        Vector<uint32_t, 16> fields;
        fields.append(m_analysis.findEnvironmentField(test.layout())->field);
        fields.append(test.fields());
        test.setFields(fields.span());
    }

    void hoist(ValueID id)
    {
        if (!m_isInWindow.quickGet(id.index()) || m_isHoisted.quickSet(id.index()))
            return;
        RELEASE_ASSERT(createsValueWithoutRunningCode(m_function.at(id)));
        m_function.at(id).forEachChild([&](ValueID operand) {
            hoist(operand);
        });
    }

    void takeOutOfItsBlock(ValueID id)
    {
        if (!m_analyses)
            m_analyses.emplace(m_context, m_function);
        bool wasListed = m_function.edit(m_analyses->useLists().blockOf(id), BlockEdit::Values).values.removeFirst(id);
        RELEASE_ASSERT(wasListed);
    }

    size_t deferClosure(const BlockValues& values, const FieldInitializer& initializer, Vector<ValueID, 8>& createdValues)
    {
        Value& closure = m_function.edit(initializer.closure);
        closure.setCapturesOperands({ });
        closure.setIsMadeOnDemand(true);

        size_t firstIndex = values.size();
        if (!values.contains(initializer.closure)) {
            RELEASE_ASSERT(initializer.value == initializer.closure);
            takeOutOfItsBlock(initializer.closure);
            createdValues.append(initializer.closure);
            return firstIndex;
        }
        for (ValueID id = initializer.value;; id = m_function.at(id).operand(0)) {
            m_isHoisted.quickSet(id.index());
            firstIndex = std::min(firstIndex, values.find(id));
            if (id == initializer.closure)
                break;
        }
        return firstIndex;
    }

    ValueID createValue(Value&& value, Vector<ValueID, 8>& createdValues)
    {
        createdValues.append(m_builder.addDetached(WTF::move(value)));
        return createdValues.last();
    }

    void rewrite(const ClosureFieldFreshObject& object)
    {
        const Program& program = m_context.program;
        Vector<Fold, 16> folds = m_analysis.selectFolds(object);
        Vector<uint32_t, 16> fields;
        Vector<size_t, 16> sources;
        for (size_t index = 0; index < folds.size(); ++index) {
            if (folds[index] == Fold::None)
                continue;
            size_t position = fields.find(object.initializers[index].field);
            if (position == notFound) {
                fields.append(object.initializers[index].field);
                sources.append(index);
            } else
                sources[position] = index;
        }

        BlockValues& values = m_function.edit(object.block, BlockEdit::Values).values;
        size_t allocationIndex = values.find(object.allocation);
        RELEASE_ASSERT(allocationIndex != notFound);
        size_t begin = allocationIndex;
        size_t end = allocationIndex + 1;

        m_isInWindow.ensureSize(m_function.values.size());
        m_isHoisted.ensureSize(m_function.values.size());
        m_isRemoved.ensureSize(m_function.values.size());
        for (size_t index = 0; index < folds.size(); ++index) {
            const FieldInitializer& initializer = object.initializers[index];
            if (folds[index] != Fold::Value || !initializer.store)
                continue;
            ++m_counts.stores;
            ValueID lastValueInWindow = initializer.isInCallee ? initializer.value : initializer.store.value;
            size_t position = lastValueInWindow ? values.find(lastValueInWindow) : notFound;
            if (position != notFound)
                end = std::max(end, position + 1);
            if (initializer.isInCallee)
                continue;
            m_isRemoved.quickSet(initializer.store.value.index());
            m_didDetachValues = true;
        }
        for (size_t index = allocationIndex + 1; index < end; ++index)
            m_isInWindow.quickSet(values[index].index());

        m_builder.setOrigin(m_function.at(object.allocation).origin());
        Vector<ValueID, 16> listedValues;
        Vector<ValueID, 8> createdValues;
        for (size_t index : sources) {
            const FieldInitializer& initializer = object.initializers[index];
            if (folds[index] == Fold::Placeholder) {
                listedValues.append(createValue(*tryCreatePlaceholder(program, { object.layout, initializer.field }), createdValues));
                continue;
            }

            bool isDeferred = m_analysis.defers(object, initializer);
            m_counts.closures += isDeferred;
            if (initializer.calleeValue != noCalleeValue) {
                Value copy = object.calleeValues[initializer.calleeValue];
                RELEASE_ASSERT(isDeferred == (copy.opcode == Opcode::NewClosure));
                if (isDeferred) {
                    copy.setCapturesOperands({ });
                    copy.setIsMadeOnDemand(true);
                }
                listedValues.append(createValue(WTF::move(copy), createdValues));
                continue;
            }
            if (isDeferred)
                begin = std::min(begin, deferClosure(values, initializer, createdValues));
            else
                hoist(initializer.value);
            listedValues.append(initializer.value);
        }

        const ClosureEnvironmentField* environmentField = m_analysis.findEnvironmentField(object.layout);
        if (environmentField && !fields.contains(environmentField->field)) {
            ValueID environment = m_analysis.environmentOf(object);
            RELEASE_ASSERT(environment);
            fields.insert(0, environmentField->field);
            listedValues.insert(0, environment);
        }

        Vector<ValueID, 32> reordered;
        for (size_t index = begin; index < allocationIndex; ++index) {
            if (!m_isHoisted.quickGet(values[index].index()))
                reordered.append(values[index]);
        }
        for (size_t index = begin; index < end; ++index) {
            if (m_isHoisted.quickGet(values[index].index()))
                reordered.append(values[index]);
        }
        reordered.appendVector(createdValues);
        reordered.append(object.allocation);
        for (size_t index = allocationIndex + 1; index < end; ++index) {
            if (!m_isHoisted.quickGet(values[index].index()) && !m_isRemoved.quickGet(values[index].index()))
                reordered.append(values[index]);
        }
        for (size_t index = begin; index < end; ++index) {
            m_isInWindow.quickClear(values[index].index());
            m_isHoisted.quickClear(values[index].index());
            m_isRemoved.quickClear(values[index].index());
        }
        values.removeAt(begin, end - begin);
        values.insertVector(begin, reordered);

        Value& allocation = m_function.edit(object.allocation);
        allocation.setFields(fields.span());
        allocation.setValuesOperands(listedValues.span());
    }

    // The store writes the word that says "not made yet". The closure has no code, so it stands right in front of the store, where nothing comes between the two.
    void deferClosureOf(const ClosureStore& store)
    {
        Value& closure = m_function.edit(store.closure);
        closure.setCapturesOperands({ });
        closure.setIsMadeOnDemand(true);
        ++m_counts.closures;

        Vector<ValueID, 2> moved;
        for (ValueID id = m_function.at(store.store).valueOperand();; id = m_function.at(id).operand(0)) {
            moved.append(id);
            if (id == store.closure)
                break;
        }
        moved.reverse();
        BlockValues& values = m_function.edit(store.block, BlockEdit::Values).values;
        if (!values.contains(store.closure))
            takeOutOfItsBlock(store.closure);
        values.removeAllMatching([&](ValueID id) {
            return moved.contains(id);
        });
        size_t position = values.find(store.store);
        RELEASE_ASSERT(position != notFound);
        values.insertVector(position, moved);
    }

    void removeStores(std::span<const ValueReference> stores)
    {
        if (stores.empty())
            return;

        BitVector isRemoved(m_function.values.size());
        for (const ValueReference& store : stores) {
            isRemoved.quickSet(store.value.index());
            ValueID stored = m_function.at(store.value).valueOperand();
            ValueID definition = definitionThroughUpcasts(m_function, stored);
            if (m_function.at(definition).opcode != Opcode::NewClosure)
                continue;
            for (ValueID id = stored;; id = m_function.at(id).operand(0)) {
                isRemoved.quickSet(id.index());
                if (id == definition)
                    break;
            }
        }
        m_function.edit(entryBlock, BlockEdit::Values).values.removeAllMatching([&](ValueID id) {
            return isRemoved.quickGet(id.index());
        });
        m_didDetachValues = true;
    }

    AnalysisContext m_context;
    const DeferralAnalysis& m_analysis;
    Function& m_function;
    TypeArena& m_types;
    IRBuilder m_builder;
    std::optional<FunctionAnalyses> m_analyses;
    BitVector m_isInWindow;
    BitVector m_isHoisted;
    BitVector m_isRemoved;
    EditCounts m_counts;
    bool m_didDetachValues { false };
    Vector<ValueID> m_narrowedValues;
};

} // anonymous namespace

static Vector<FunctionEdits> groupByFunction(const DeferralAnalysis& analysis)
{
    std::span<const ClosureFieldFreshObject> objects = analysis.objects();
    std::span<const ValueReference> reads = analysis.reads();
    std::span<const ValueReference> namedAccesses = analysis.namedAccesses();
    std::span<const ValueReference> keyOrderTests = analysis.keyOrderTests();
    std::span<const ValueReference> removedStores = analysis.removedStores();
    std::span<const ClosureStore> deferredStores = analysis.deferredStores();

    Vector<FunctionEdits> edits;
    while (true) {
        FunctionID function;
        auto considerFirst = [&](const auto& remaining) {
            if (!remaining.empty() && (!function || remaining.front().function < function))
                function = remaining.front().function;
        };
        considerFirst(objects);
        considerFirst(reads);
        considerFirst(namedAccesses);
        considerFirst(keyOrderTests);
        considerFirst(removedStores);
        considerFirst(deferredStores);
        if (!function)
            return edits;

        edits.append({
            function,
            takeEditsOf(function, objects),
            takeEditsOf(function, reads),
            takeEditsOf(function, namedAccesses),
            takeEditsOf(function, keyOrderTests),
            takeEditsOf(function, removedStores),
            takeEditsOf(function, deferredStores),
        });
    }
}

static void addEnvironmentFields(ProgramEditor& editor, const DeferralAnalysis& analysis)
{
    for (const ClosureEnvironmentField& entry : analysis.environmentFields()) {
        if (!entry.isNew)
            continue;

        Field field;
        field.declared = DeclaredType { .type = entry.type, .flags = { PositionFlags::IsFromCompiler } };
        field.flags = { FieldFlags::IsRequired, FieldFlags::IsHidden, FieldFlags::IsAddedLate };
        field.flow.location.initialization = InitializationState::SetAtCreation;
        field.flow.occupancy = Occupancy::AlwaysFilled;
        field.flow.stability = stabilityOfFilledSlot;

        Layout& layout = editor.atForHiddenFields(entry.layout);
        uint32_t index = layout.appendField(WTF::move(field));
        RELEASE_ASSERT(index == entry.field);
        layout.flow.closureEnvironmentField = index;
        for (FieldOrder& order : layout.flow.constructorOrders)
            order.fields.insert(0, index);
    }
}

static void recordDeferrals(ProgramEditor& editor, const DeferralAnalysis& analysis)
{
    const Program& program = editor.program();
    for (FieldReference candidate : analysis.candidates()) {
        const FieldState& state = analysis.stateOf(candidate.layout, candidate.field);
        if (!state.canDefer())
            continue;
        FieldFlow& flow = editor.at(candidate).flow;
        flow.location.deferral = deferralFor(program, state.function);
        flow.location.deferredFunction = state.function;
        if (!analysis.isFilledByEveryAllocation(candidate))
            continue;
        flow.location.initialization = InitializationState::SetAtCreation;
        if (flow.stability.containsAll(stabilityOfFilledSlot))
            flow.occupancy = Occupancy::AlwaysFilled;
    }
    addEnvironmentFields(editor, analysis);
}

static void applyDeferrals(ProgramEditor& editor, BuildContext& build, const DeferralAnalysis& analysis)
{
    const Program& program = editor.program();
    recordDeferrals(editor, analysis);

    Vector<FunctionEdits> edits = groupByFunction(analysis);
    Vector<FunctionID> functions = edits.map([](const FunctionEdits& edit) {
        return edit.function;
    });
    Vector<EditCounts> counts(edits.size());
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        counts[index] = ClosureFieldRewriter(AnalysisContext { build, program, nullptr, nullptr, id }, analysis, function, arenas.types).run(edits[index]);
    });

    EditCounts total;
    for (const EditCounts& count : counts) {
        total.closures += count.closures;
        total.stores += count.stores;
    }
    Statistics& statistics = build.statistics();
    statistics.count(StatisticID::FieldsDeferringClosures, analysis.numberOfDeferringFields());
    statistics.count(StatisticID::ClosuresDeferredInFields, total.closures);
    statistics.count(StatisticID::StoresListedByAllocations, total.stores);
}

// Every arm of a branch makes a closure of one function over the same values, and a parameter of the block behind the arms takes the one that was made.
class ClosuresOfArmsJoiner {
public:
    ClosuresOfArmsJoiner(const AnalysisContext& context, Function& function, TypeArena& types)
        : m_context(context)
        , m_function(function)
        , m_types(types)
    {
    }

    // Asked of every function of the program, so it looks at edges first, and at the one block behind an edge that carries a closure.
    static bool mayHaveJoin(const Function& function)
    {
        return std::ranges::any_of(function.blocks, [&](const Block& block) {
            return std::ranges::any_of(block.terminator.targets, [&](const Target& target) {
                const Block& behind = function.at(target.block);
                for (uint32_t index = 0; index < target.arguments.size() && index < behind.parameters.size(); ++index) {
                    if (function.at(target.arguments[index]).opcode == Opcode::NewClosure && givesFieldParameter(function, behind, behind.parameters[index]))
                        return true;
                }
                return false;
            });
        });
    }

    void run()
    {
        FunctionAnalyses analyses(m_context, m_function);
        Vector<Join, 8> joins;
        for (uint32_t index = 0; index < m_function.blocks.size(); ++index)
            findJoins(analyses, BlockID(index), joins);
        if (joins.isEmpty())
            return;

        IRBuilder builder(m_context, m_function, m_types, &analyses);
        for (const Join& join : joins) {
            builder.setOrigin(m_function.at(join.closures[0]).origin());
            ValueID closure = builder.insertBefore(ProgramPoint { join.block, 0 }, Value(m_function.at(join.closures[0])));
            m_function.edit(join.user, ValueEdit::Definition).setOperand(join.operandIndex, closure);
        }
        builder.applyInsertions();
        for (const Join& join : joins) {
            for (size_t index = 0; index < join.closures.size(); ++index) {
                bool wasListed = m_function.edit(join.blocksOfClosures[index], BlockEdit::Values).values.removeFirst(join.closures[index]);
                RELEASE_ASSERT(wasListed);
            }
        }
        for (size_t first = 0; first < joins.size();) {
            BitVector parameters;
            size_t end = first;
            for (; end < joins.size() && joins[end].block == joins[first].block; ++end)
                parameters.set(joins[end].parameterIndex);
            builder.removeParameters(joins[first].block, parameters);
            first = end;
        }
        compactFunction(m_function);
    }

private:
    static bool givesField(Opcode opcode) { return opcode == Opcode::NewObject || isFieldStore(opcode); }

    static bool givesFieldParameter(const Function& function, const Block& block, ValueID parameter)
    {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            const Value& value = function.at(id);
            if (!givesField(value.opcode))
                return false;
            bool usesParameter = false;
            value.forEachChild([&](ValueID operand) {
                usesParameter |= operand == parameter;
            });
            return usesParameter;
        });
    }

    struct Join {
        BlockID block;
        uint32_t parameterIndex { 0 };
        ValueID user;
        uint32_t operandIndex { 0 };
        Vector<ValueID, 2> closures;
        Vector<BlockID, 2> blocksOfClosures;
    };

    void findJoins(FunctionAnalyses& analyses, BlockID block, Vector<Join, 8>& joins)
    {
        const Block& contents = m_function.at(block);
        std::span<const Edge> edges = analyses.cfg().incomingEdges(block);
        if (contents.isHandlerBlock() || edges.size() < 2 || std::ranges::any_of(edges, &Edge::isHandlerEdge))
            return;
        const UseLists& uses = analyses.useLists();
        for (uint32_t parameterIndex = 0; parameterIndex < contents.parameters.size(); ++parameterIndex) {
            ValueID parameter = contents.parameters[parameterIndex];
            std::span<const Use> usesOfParameter = uses.usesOf(parameter);
            if (usesOfParameter.size() != 1 || usesOfParameter[0].kind != Use::Kind::Operand || usesOfParameter[0].block != block || !givesField(m_function.at(usesOfParameter[0].user).opcode))
                continue;
            Join join { .block = block, .parameterIndex = parameterIndex, .user = usesOfParameter[0].user, .operandIndex = usesOfParameter[0].operandIndex };
            bool everyArmMakesTheClosure = std::ranges::all_of(edges, [&](const Edge& edge) {
                const Arguments& arguments = targetOf(m_function, edge).arguments;
                if (parameterIndex >= arguments.size())
                    return false;
                ValueID id = arguments[parameterIndex];
                const Value& closure = m_function.at(id);
                ValueLocation location = uses.locationOf(id);
                if (closure.opcode != Opcode::NewClosure || closure.type != m_function.at(parameter).type || uses.useCount(id) != 1 || location.placement != ValuePlacement::InBlock)
                    return false;
                if (!join.closures.isEmpty()) {
                    const Value& first = m_function.at(join.closures[0]);
                    if (closure.function() != first.function() || closure.storage() != first.storage() || closure.isMadeOnDemand() != first.isMadeOnDemand() || !std::ranges::equal(closure.capturesOperands(), first.capturesOperands()))
                        return false;
                }
                join.closures.append(id);
                join.blocksOfClosures.append(location.block);
                return true;
            });
            if (everyArmMakesTheClosure)
                joins.append(WTF::move(join));
        }
    }

    const AnalysisContext& m_context;
    Function& m_function;
    TypeArena& m_types;
};

static void joinClosuresOfArms(ProgramEditor& editor, BuildContext& build)
{
    const Program& program = editor.program();
    Vector<uint8_t> mayHaveJoin;
    mayHaveJoin.fill(0, program.functions.size());
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        // An arm hands on a closure that the function makes.
        if (!mayHoldValueWith(program, FunctionID(index), { Opcode::NewClosure }))
            return;
        const Function& function = program.functions[index];
        mayHaveJoin[index] = function.hasBody() && ClosuresOfArmsJoiner::mayHaveJoin(function);
    });
    Vector<FunctionID> functions;
    for (size_t index = 0; index < mayHaveJoin.size(); ++index) {
        if (mayHaveJoin[index])
            functions.append(FunctionID(index));
    }
    if (functions.isEmpty())
        return;
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        AnalysisContext context { build, program, nullptr, nullptr, id };
        ClosuresOfArmsJoiner(context, function, arenas.types).run();
    });
}

void deferClosuresInFields(ProgramEditor& editor, BuildContext& build)
{
    if (build.recordedOptions().useWideReferences())
        return;

    joinClosuresOfArms(editor, build);
    const Program& program = editor.program();
    bool isDumped = build.options().logging.dumpedPass == PassID::DeferClosuresInFields;
    DeferralAnalysis analysis(program, build);
    analysis.run();
    if (isDumped) [[unlikely]]
        analysis.dump(WTF::dataFile());
    if (analysis.defersAnything())
        applyDeferrals(editor, build, analysis);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
