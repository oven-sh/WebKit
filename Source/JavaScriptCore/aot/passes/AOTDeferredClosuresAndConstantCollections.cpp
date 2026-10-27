/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeferClosuresInFields.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallGraph.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include "AOTIRBuilder.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include "ArrayConventions.h"
#include "PureNaN.h"
#include "Structure.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::GetSlot::object == OperandIndex::GetDeferredClosureSlot::object && OperandIndex::GetSlot::guard == OperandIndex::GetDeferredClosureSlot::guard);
static_assert(OperandIndex::GetSlotOrUndefined::object == OperandIndex::GetDeferredClosureSlot::object && OperandIndex::GetSlotOrUndefined::guard == OperandIndex::GetDeferredClosureSlot::guard);
static_assert(OperandIndex::GetSlotOrUndefined::object == OperandIndex::GetDeferredClosureSlotOrUndefined::object && OperandIndex::GetSlotOrUndefined::guard == OperandIndex::GetDeferredClosureSlotOrUndefined::guard);

AOT_THRESHOLD(size_t, maxWindowSize, 512);
AOT_THRESHOLD(unsigned, maxCallDepth, 4);
static constexpr OptionSet<FieldStability> stabilityOfFilledSlot { FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };
static constexpr OpcodeSet storesOfFields { Opcode::PutSlot, Opcode::PutSlotOrAdd };
static constexpr OpcodeSet operationsGivingFields = storesOfFields | OpcodeSet { Opcode::NewObject };

namespace {

struct FieldState {
    bool canDefer() const { return function && !isRejected; }

    FunctionID function;
    uint32_t numberOfInitializingAllocations { 0 };
    bool isRejected { false };
    // Some store that gives the closure only expects the layout of its object.
    bool isGivenWhereLayoutIsExpected { false };
};

struct ValueReference {
    explicit operator bool() const { return !!value; }
    friend auto operator<=>(const ValueReference&, const ValueReference&) = default;

    FunctionID function;
    ValueID value;
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
    KnowledgeLevel level { KnowledgeLevel::Proven };
};

struct FieldInitializer {
    uint32_t field { 0 };
    ValueReference store;
    ValueID value;
    std::optional<Value> calleeValue;
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
    uint32_t numberOfWindows { 0 };
};

struct ClosureFieldFreshObject {
    FunctionID function;
    BlockID block;
    ValueID allocation;
    LayoutID layout;
    Vector<FieldInitializer> initializers;
    Vector<EnteredCall> enteredCalls;
};

struct ScannedFunction {
    Vector<ClosureFieldFreshObject> objects;
    Vector<ClosureStore> closureStores;
};

// A store to a field of the receiver, or, without a layout, a call that hands the receiver on.
struct PrologueStep {
    ValueID operation;
    LayoutID layout;
    uint32_t field { 0 };
    ValueID closure;
};

struct CalleePrologue {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CalleePrologue);

    Vector<PrologueStep> steps;
    bool isWholeBody { false };
};

enum class Fold : uint8_t { None, Value, Placeholder };

struct ClosureEnvironmentField {
    LayoutID layout;
    uint32_t field { 0 };
    TypeID type;
    bool isNew { false };
};

struct CalleeState {
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

struct DeferrableLayouts {
    // A store may leave its closure unmade in an object of such a layout.
    BitVector all;
    // The operations of the program make every object of such a layout, and each of them makes that layout and no other.
    BitVector allocatedOnlyByOperations;
};

} // anonymous namespace

static DeferrableLayouts findDeferrableLayouts(const Program& program)
{
    size_t size = program.layouts.size();
    BitVector isBase(size);
    BitVector isFilledByOthers(size);
    for (size_t index = 0; index < size; ++index) {
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

    DeferrableLayouts result { BitVector(size), BitVector(size) };
    for (size_t index = 0; index < size; ++index) {
        const Layout& layout = program.layouts[index];
        if (isFilledByOthers.quickGet(index) || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        result.all.quickSet(index);
        bool hasOtherConstructor = layout.constructor && program.at(layout.constructor).kind != FunctionKind::BaseConstructor;
        if (!isBase.quickGet(index) && layout.flow.baseState != BaseState::Layout && !hasOtherConstructor)
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
    // A call of a field whose closure is not made yet enters the function through a closure in the frame of the stub, and the list of recorded callees, which the collector visits, holds the closure that the function is entered through.
    if (callee.has(FunctionCapabilities::RecordsCallee) && !callee.has(FunctionCapabilities::RecordsCalleeOnceRenamed))
        return false;
    if (!callee.captures.isEmpty()) {
        const Type& captured = program.at(callee.captures[0].written());
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
    return program.at(callee.captures[0].written()).environment ? ClosureDeferral::CapturesEnvironment : ClosureDeferral::CapturesObject;
}

static bool createsValueWithoutRunningCode(const Value& value)
{
    if (isRematerializable(value) || isCastWithoutTest(value.opcode) || value.opcode == Opcode::AllGuards || makesFact(value.opcode))
        return true;
    return interpretedOf(value.opcode) == Interpretation::Allocation && declaredBoundOf(value.opcode).isUnobservable();
}

static void leaveUnmade(Value& closure)
{
    closure.setCapturesOperands({ });
    closure.setIsMadeOnDemand(true);
}

// The slot has a word for a closure that is not made, one for an absent field, and what a narrow reference holds: undefined, null and objects.
static bool mayBeOtherThanObjectOrNullish(const Type& type)
{
    return type.primitives & ~Primitive::UndefinedOrNull;
}

static std::optional<Value> tryCreatePlaceholder(const Program& program, FieldReference reference)
{
    const Field& field = fieldAt(program, declarationOf(program, reference));
    if (!canBeStoredBehindAllocation(field))
        return std::nullopt;
    const Type& type = program.at(slotTypeOf(program, field));
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

// One closure for one object.
static ValueID findClosureUsedOnce(FunctionAnalyses& analyses, BlockID block, ValueID stored)
{
    const Function& function = analyses.function();
    const UseLists& uses = analyses.useLists();
    ValueID id = definitionThroughUpcasts(function, stored);
    const Value& closure = function.at(id);
    if (closure.opcode != Opcode::NewClosure || !canBeCreatedOnDemand(analyses.program(), closure))
        return { };

    for (ValueID cast = stored; cast != id; cast = function.at(cast).operand(0)) {
        if (uses.useCount(cast) != 1)
            return { };
    }
    ValueLocation location = uses.locationOf(id);
    if (uses.useCount(id) != 1 || location.placement != ValuePlacement::InBlock)
        return { };
    if (location.block == block)
        return id;
    bool userRunsOnceForEachClosure = stored == id && !analyses.isInCycle(block) && analyses.dominators().dominates(location.block, block);
    return userRunsOnceForEachClosure ? id : ValueID();
}

bool canLeaveClosureUnmade(FunctionAnalyses& analyses, BlockID block, ValueID object, ValueID given)
{
    const Function& function = analyses.function();
    ValueID id = findClosureUsedOnce(analyses, block, given);
    if (!id)
        return false;
    std::span<const ValueID> captures = function.at(id).capturesOperands();
    return captures.empty() || sameObjectThroughCasts(function, captures[0]) == object || analyses.program().typeOf(function, captures[0]).environment;
}

static void forEachFinalInitializer(const ClosureFieldFreshObject& object, const Invocable<void(size_t, const FieldInitializer&)> auto& functor, const Vector<Fold, 16>* foldsOfCounted = nullptr)
{
    Vector<uint32_t, 16> visitedFields;
    for (size_t index = object.initializers.size(); index--;) {
        const FieldInitializer& initializer = object.initializers[index];
        if ((foldsOfCounted && foldsOfCounted->at(index) == Fold::None) || visitedFields.contains(initializer.field))
            continue;
        visitedFields.append(initializer.field);
        functor(index, initializer);
    }
}

static auto& findCalleeStore(auto& calleeStores, ValueReference store)
{
    auto found = std::ranges::lower_bound(calleeStores, store, { }, &CalleeStoreState::store);
    RELEASE_ASSERT(found != calleeStores.end() && found->store == store);
    return *found;
}

template<typename Edit>
static std::span<const Edit> editsOf(FunctionID function, const Vector<Edit>& editsByFunction)
{
    auto edits = std::ranges::equal_range(editsByFunction, function, { }, &Edit::function);
    return { edits.begin(), edits.end() };
}

namespace {

// Reads one function. A window is the run of operations behind the point where an object comes into being for the function, which is an allocation or, in a function that gets the object as its receiver, the start, up to the first operation that may show the object to other code.
struct WindowScanner {
    static constexpr OpcodeSet scannedOpcodes = operationsGivingFields;

    void scan(Vector<uint32_t>& allocationsOfLayouts, ScannedFunction& found)
    {
        bool isEnabled = m_build.isEnabled(PassID::DeferClosuresInFields, m_id);
        forEachListedValue(m_program, m_id, scannedOpcodes, [&](BlockID block, uint32_t index, ValueID id, const Value& value) {
            if (storesOfFields.contains(value.opcode)) {
                if (isEnabled)
                    describeStore(block, id, value, found.closureStores);
                return;
            }
            if (!m_layouts.allocatedOnlyByOperations.quickGet(value.layout().index()))
                return;
            WTF::atomicExchangeAdd(&allocationsOfLayouts[value.layout().index()], 1u, std::memory_order_relaxed);
            if (mayAllocateOtherLayout(m_program, value) || !isEnabled)
                return;
            ClosureFieldFreshObject object = describeObject(block, index);
            bool isInitializedWithClosure = object.initializers.containsIf([](const FieldInitializer& initializer) {
                return !!initializer.closureFunction;
            });
            if (isInitializedWithClosure)
                found.objects.append(WTF::move(object));
        });
    }

    // Null where something other than a direct call may enter the function.
    std::unique_ptr<CalleePrologue> findPrologue()
    {
        if (!m_function.hasBody() || !m_function.thisParameterIndex() || !m_build.isEnabled(PassID::DeferClosuresInFields, m_id))
            return nullptr;
        const Block& entry = m_function.at(entryBlock);
        if (entry.parameters.size() != m_function.numberOfEntryParameters() || (!m_function.hasOnlyDirectCallers() && m_function.kind != FunctionKind::Initializer))
            return nullptr;

        auto prologue = makeUnique<CalleePrologue>();
        beginWindow(entry.parameters[*m_function.thisParameterIndex()]);
        bool didReachEnd = walkWindow(entry.values.span(), [&](ValueID id, const Value& store) {
            LayoutID layout = layoutInitializedBy(store);
            if (!layout || !m_layouts.allocatedOnlyByOperations.quickGet(layout.index()))
                return false;
            std::optional<uint32_t> field = m_program.at(layout).findField(store.key());
            if (field)
                prologue->steps.append({ id, layout, *field, isClosure(store.valueOperand()) ? findDeferrableClosure(entryBlock, store.valueOperand()) : ValueID() });
            return !!field;
        }, [&](ValueID id, const Value&) {
            prologue->steps.append({ .operation = id });
            return true;
        });
        prologue->isWholeBody = didReachEnd && entry.values.size() <= maxWindowSize && entry.terminator.kind == TerminatorKind::Return && !mayHoldObject(entry.terminator.valueOperand());
        return prologue;
    }

    enum class Trait : uint8_t { IsInWindow = 1, IsObject = 2, MayHoldObject = 4, CanBeHoisted = 8 };

    bool has(ValueID id, Trait trait) const { return m_traits[id.index()].contains(trait); }
    bool isObject(ValueID id) const { return id && has(id, Trait::IsObject); }
    bool mayHoldObject(ValueID id) const { return id && has(id, Trait::MayHoldObject); }
    bool isAvailableAtAllocation(ValueID id) const { return !has(id, Trait::IsInWindow) || has(id, Trait::CanBeHoisted); }
    bool isClosure(ValueID id) const { return m_function.at(definitionThroughUpcasts(m_function, id)).opcode == Opcode::NewClosure; }

    bool usesObject(const Value& value) const
    {
        return std::ranges::any_of(value.operands(), [&](ValueID operand) {
            return mayHoldObject(operand);
        });
    }

    void addToWindow(ValueID id, OptionSet<Trait> traits)
    {
        m_window.append(id);
        m_traits[id.index()] = traits | Trait::IsInWindow;
    }

    void beginWindow(ValueID object)
    {
        if (m_traits.isEmpty())
            m_traits.fill({ }, m_function.values.size());
        for (ValueID id : m_window)
            m_traits[id.index()] = { };
        m_window.shrink(0);
        m_hasSkippedOperation = false;
        addToWindow(object, { Trait::IsObject, Trait::MayHoldObject });
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
        OptionSet<Trait> traits;
        traits.set(Trait::IsObject, usesOnlyObject && (isCastWithoutTest(value.opcode) || value.opcode == Opcode::AllGuards));
        traits.set(Trait::MayHoldObject, holdsObject);
        traits.set(Trait::CanBeHoisted, canBeHoisted);
        addToWindow(id, traits);
    }

    // Says whether the window goes on behind the last of the values. The functors say whether it goes on behind a store to the object and behind a call that gets the object as its receiver and nowhere else.
    bool walkWindow(std::span<const ValueID> values, const Invocable<bool(ValueID, const Value&)> auto& addStore, const Invocable<bool(ValueID, const Value&)> auto& enterCall)
    {
        for (ValueID id : values.first(std::min(values.size(), maxWindowSize))) {
            const Value& value = m_function.at(id);
            if (storesOfFields.contains(value.opcode) && isObject(value.objectOperand())) {
                if (!addStore(id, value))
                    return false;
                continue;
            }
            bool isCallWithObject = value.opcode == Opcode::CallDirect && usesObject(value);
            if (!isCallWithObject && createsValueWithoutRunningCode(value)) {
                addCreatedValue(id, value);
                continue;
            }
            if (isCallWithObject ? !passesObjectOnlyAsReceiver(value) || !enterCall(id, value) : usesObject(value))
                return false;
            addToWindow(id, { });
            m_hasSkippedOperation = true;
        }
        return true;
    }

    LayoutID layoutInitializedBy(const Value& store) const
    {
        if (store.level() != KnowledgeLevel::Proven || (store.guardOperand() && !isObject(store.guardOperand())))
            return { };
        auto receivers = m_program.at(store.receivers()).all();
        return receivers && receivers->value().size() == 1 ? receivers->value().front() : LayoutID();
    }

    bool passesObjectOnlyAsReceiver(const Value& call) const
    {
        const Function& callee = m_program.at(call.function());
        std::optional<unsigned> receiverIndex = callee.thisParameterIndex();
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (!receiverIndex || arguments.size() != callee.numberOfEntryParameters() || !isObject(arguments[*receiverIndex]))
            return false;
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (index != *receiverIndex && mayHoldObject(arguments[index]))
                return false;
        }
        return true;
    }

    // The closure that a field of the object of the window, or of the receiver of a store outside of any window, is given, where it can be left unmade.
    // Only an allocation, which asks with a place for the environment, can fill the hidden field that keeps the environment, and only its list holds a closure that is unmade already.
    ValueID findDeferrableClosure(BlockID block, ValueID given, ValueID receiverOutsideWindow = { }, ValueID listedEnvironment = { }, ValueID* environment = nullptr)
    {
        if (!m_analyses)
            m_analyses.emplace(AnalysisContext { m_build, m_program, nullptr, nullptr, m_id }, m_function);
        ValueID id = findClosureUsedOnce(*m_analyses, block, given);
        if (!id)
            return { };
        const Value& closure = m_function.at(id);
        std::span<const ValueID> captures = closure.capturesOperands();
        bool isUnmade = closure.isMadeOnDemand();
        if (isUnmade ? !environment : captures.size() != m_program.at(closure.function()).captures.size())
            return { };

        switch (deferralFor(m_program, closure.function())) {
        case ClosureDeferral::Unknown:
            break;
        case ClosureDeferral::CapturesNothing:
            return id;
        case ClosureDeferral::CapturesObject: {
            bool capturesReceiver = receiverOutsideWindow ? sameObjectThroughCasts(m_function, captures[0]) == sameObjectThroughCasts(m_function, receiverOutsideWindow) : isUnmade || isObject(captures[0]);
            return capturesReceiver ? id : ValueID();
        }
        case ClosureDeferral::CapturesEnvironment: {
            ValueID captured = isUnmade ? listedEnvironment : captures[0];
            if (!environment || !captured || has(captured, Trait::IsInWindow))
                return { };
            *environment = captured;
            return id;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void describeStore(BlockID block, ValueID id, const Value& store, Vector<ClosureStore>& found)
    {
        if (!isClosure(store.valueOperand()))
            return;
        auto receivers = m_program.at(store.receivers()).all();
        if (!receivers || receivers->value().empty())
            return;
        std::optional<FieldReference> declaration;
        for (LayoutID member : *receivers) {
            std::optional<uint32_t> field = m_program.at(member).findField(store.key());
            if (!field || !m_layouts.all.quickGet(member.index()))
                return;
            FieldReference declarationOfMember = declarationOf(m_program, { member, *field });
            if (declaration && *declaration != declarationOfMember)
                return;
            declaration = declarationOfMember;
        }
        if (!m_layouts.all.quickGet(declaration->layout.index()) || !canDeferClosure(m_program, *declaration))
            return;
        if (ValueID closure = findDeferrableClosure(block, store.valueOperand(), store.objectOperand()))
            found.append({ m_id, block, id, closure, declaration->layout, declaration->field, m_function.at(closure).function(), store.level() });
    }

    void addInitializer(ClosureFieldFreshObject& object, ValueID listedEnvironment, uint32_t field, ValueID store, ValueID value)
    {
        FieldInitializer initializer { .field = field, .value = value, .isAvailableAtAllocation = !store || isAvailableAtAllocation(value) };
        if (store)
            initializer.store = { m_id, store };
        if (isClosure(value) && canDeferClosure(m_program, { object.layout, field }))
            initializer.closure = findDeferrableClosure(object.block, value, { }, listedEnvironment, &initializer.environment);
        if (initializer.closure)
            initializer.closureFunction = m_function.at(initializer.closure).function();
        object.initializers.append(initializer);
    }

    const CalleePrologue* prologueOf(FunctionID callee)
    {
        return m_prologues.ensure(callee, [&] {
            return WindowScanner { m_program, m_build, m_layouts, callee }.findPrologue();
        }).iterator->value.get();
    }

    // An argument without a function is a value of this function, one with a function a constant of that function, and a null one is unknown.
    bool walkPrologue(ClosureFieldFreshObject& object, const EnteredCall& call, std::span<const ValueReference> arguments, unsigned depth)
    {
        const CalleePrologue* prologue = depth <= maxCallDepth ? prologueOf(call.callee) : nullptr;
        if (!prologue)
            return false;
        object.enteredCalls.append(call);
        const Function& callee = m_program.at(call.callee);
        auto resolve = [&](ValueID id) -> ValueReference {
            if (!id)
                return { };
            ValueID definition = definitionThroughUpcasts(callee, id);
            if (isRematerializable(callee.at(definition)))
                return { call.callee, definition };
            size_t index = callee.at(definition).opcode == Opcode::Parameter ? callee.at(entryBlock).parameters.find(definition) : notFound;
            return index == notFound ? ValueReference() : arguments[index];
        };

        for (const PrologueStep& step : prologue->steps) {
            const Value& operation = callee.at(step.operation);
            if (!step.layout) {
                std::span<const ValueID> operands = operation.argumentsOperands();
                Vector<ValueReference, 8> nestedArguments(operands.size(), [&](size_t index) {
                    return resolve(operands[index]);
                });
                if (!walkPrologue(object, { { call.callee, step.operation }, operation.function(), true }, nestedArguments.span(), depth + 1))
                    return false;
                continue;
            }
            if (step.layout != object.layout)
                return false;
            FieldInitializer initializer { .field = step.field, .store = { call.callee, step.operation }, .isInCallee = true };
            ValueReference stored = step.closure ? ValueReference { call.callee, step.closure } : resolve(operation.valueOperand());
            if (stored.function && (!step.closure || canDeferClosure(m_program, { object.layout, step.field }))) {
                initializer.calleeValue = m_program.at(stored.function).at(stored.value);
                if (step.closure)
                    initializer.closureFunction = initializer.calleeValue->function();
                else
                    initializer.isAvailableAtAllocation = true;
            } else if (stored && !step.closure) {
                initializer.value = stored.value;
                initializer.isAvailableAtAllocation = isAvailableAtAllocation(stored.value);
            }
            object.initializers.append(WTF::move(initializer));
        }
        return prologue->isWholeBody;
    }

    ClosureFieldFreshObject describeObject(BlockID block, size_t allocationIndex)
    {
        std::span<const ValueID> values = m_function.at(block).values.span();
        const Value& allocation = m_function.at(values[allocationIndex]);
        const Layout& layout = m_program.at(allocation.layout());
        ClosureFieldFreshObject object { m_id, block, values[allocationIndex], allocation.layout(), { }, { } };
        beginWindow(object.allocation);

        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> listedValues = allocation.valuesOperands();
        ValueID listedEnvironment;
        if (layout.flow.closureEnvironmentField) {
            size_t index = std::ranges::find(fields, *layout.flow.closureEnvironmentField) - fields.begin();
            if (index < fields.size())
                listedEnvironment = listedValues[index];
        }
        for (size_t index = 0; index < fields.size(); ++index)
            addInitializer(object, listedEnvironment, fields[index], { }, listedValues[index]);

        walkWindow(values.subspan(allocationIndex + 1), [&](ValueID id, const Value& store) {
            std::optional<uint32_t> field;
            if (layoutInitializedBy(store) == object.layout)
                field = layout.findField(store.key());
            if (field)
                addInitializer(object, listedEnvironment, *field, id, store.valueOperand());
            return !!field;
        }, [&](ValueID id, const Value& call) {
            std::span<const ValueID> operands = call.argumentsOperands();
            Vector<ValueReference, 8> arguments(operands.size(), [&](size_t index) {
                return ValueReference { { }, operands[index] };
            });
            return walkPrologue(object, { { m_id, id }, call.function(), false }, arguments.span(), 0);
        });
        return object;
    }

    const Program& m_program;
    BuildContext& m_build;
    const DeferrableLayouts& m_layouts;
    FunctionID m_id;
    const Function& m_function { m_program.at(m_id) };
    std::optional<FunctionAnalyses> m_analyses { };
    HashMap<FunctionID, std::unique_ptr<CalleePrologue>> m_prologues { };
    Vector<ValueID, 32> m_window { };
    Vector<OptionSet<Trait>> m_traits { };
    bool m_hasSkippedOperation { false };
};

struct DeferralAnalysis {
    enum AccessKind : uint8_t { SlotRead, SlotStore, PresenceTest, NamedAccess, KeyOrderTest, FunctionEntry, Allocation };

    struct CandidateWithKey {
        auto operator<=>(const CandidateWithKey&) const = default;

        PropertyKeyID key;
        FieldReference field;
    };

    const Value& valueAt(ValueReference reference) const { return program.at(reference.function).at(reference.value); }
    uint32_t rowOf(FieldReference reference) const
    {
        FieldReference declaration = declarationOf(program, reference);
        return firstRows[declaration.layout.index()] + declaration.field;
    }
    const FieldState& stateOf(FieldReference reference) const { return states[rowOf(reference)]; }
    void reject(uint32_t row) { numberOfChanges += !std::exchange(states[row].isRejected, true); }

    // The hidden field that keeps the environment is filled by allocations alone, so every allocation has to list the closure and nothing else may come into the field.
    bool keepsEnvironment(const FieldState& state) const { return deferralFor(program, state.function) == ClosureDeferral::CapturesEnvironment; }
    bool defersOverEnvironment(const FieldState& state) const { return state.canDefer() && keepsEnvironment(state); }

    bool defers(LayoutID layout, uint32_t field, FunctionID closureFunction) const
    {
        const FieldState& state = stateOf({ layout, field });
        return state.canDefer() && state.function == closureFunction;
    }
    bool defers(const ClosureFieldFreshObject& object, const FieldInitializer& initializer) const { return defers(object.layout, initializer.field, initializer.closureFunction); }

    bool defersAnyClosure(const ClosureFieldFreshObject& object) const
    {
        return std::ranges::any_of(object.initializers, [&](const FieldInitializer& initializer) {
            return defers(object, initializer);
        });
    }

    auto findCandidatesWithKey(PropertyKeyID key) const { return std::ranges::equal_range(candidatesByKey, key, { }, &CandidateWithKey::key); }

    // Nothing says which objects arrive at a store by name into any object, so it reaches every candidate under its key.
    bool storesIntoAnyObject(ValueReference reference) const
    {
        const Function& function = program.at(reference.function);
        const Value& access = function.at(reference.value);
        return !Value::opcodesWithFieldAccess.contains(access.opcode)
            && access.opcode != Opcode::IsSlotFilled
            && storedOf(access.opcode)
            && access.operand(0)
            && program.typeOf(function, access.operand(0)).anyObject;
    }

    // An access to a slot names the layouts of its objects. For an access by name the type of the object says them.
    void forEachCandidateOf(ValueReference reference, const Invocable<void(FieldReference, uint32_t row)> auto& functor) const
    {
        const Function& function = program.at(reference.function);
        const Value& access = function.at(reference.value);
        if (storesIntoAnyObject(reference)) {
            for (const CandidateWithKey& candidate : findCandidatesWithKey(access.key()))
                functor(candidate.field, rowOf(candidate.field));
            return;
        }
        std::span<const LayoutID> members;
        if (Value::opcodesWithFieldAccess.contains(access.opcode) || access.opcode == Opcode::IsSlotFilled)
            members = program.at(access.receivers()).completeMembers().value();
        else if (access.numberOfOperands() && access.operand(0))
            members = membersOf(program, program.typeOf(function, access.operand(0)));
        for (LayoutID member : members) {
            std::optional<uint32_t> field = program.at(member).findField(access.key());
            if (!field)
                continue;
            uint32_t row = rowOf({ member, *field });
            if (states[row].function)
                functor(FieldReference { member, *field }, row);
        }
    }

    bool reachesCandidate(ValueReference reference, bool onlyIfItDefers) const
    {
        if (!onlyIfItDefers && storesIntoAnyObject(reference))
            return !findCandidatesWithKey(valueAt(reference).key()).empty();
        bool result = false;
        forEachCandidateOf(reference, [&](FieldReference, uint32_t row) {
            result |= !onlyIfItDefers || states[row].canDefer();
        });
        return result;
    }

    // `ifAbsent` is the answer for a receiver without the field.
    bool holdsForFieldInEveryReceiver(const Value& access, bool ifAbsent, const Invocable<bool(FieldReference)> auto& predicate) const
    {
        return std::ranges::all_of(program.at(access.receivers()).completeMembers(), [&](LayoutID member) {
            std::optional<uint32_t> field = program.at(member).findField(access.key());
            return field ? predicate(FieldReference { member, *field }) : ifAbsent;
        });
    }

    void findObjects()
    {
        DeferrableLayouts layouts = findDeferrableLayouts(program);
        allocationsOfLayouts.fill(0, program.layouts.size());
        dataMembersOfFields.fill(0, firstRows.last());
        const ParallelLoops& loops = build.parallelLoops();
        forEachIndexInParallel(loops, program.data.size(), [&](unsigned index) {
            forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t, const DataNode& node) {
                const DataNode::Object* object = node.tryAs<DataNode::Object>();
                if (!object || !object->layout)
                    return;
                WTF::atomicExchangeAdd(&allocationsOfLayouts[object->layout.index()], 1u, std::memory_order_relaxed);
                for (const DataMember& member : object->members) {
                    if (member.value.is<DataNode::Undefined>() || member.value.is<DataNode::Null>())
                        continue;
                    if (std::optional<uint32_t> field = program.at(object->layout).findField(member.key))
                        WTF::atomicExchangeAdd(&dataMembersOfFields[rowOf({ object->layout, *field })], 1u, std::memory_order_relaxed);
                }
            });
        });
        Vector<ScannedFunction> scanned = collectInParallel<ScannedFunction>(loops, program.functions.size(), functionsBySize.span(), [&](unsigned index, Vector<ScannedFunction>& found) {
            if (!mayListValueWith(program, FunctionID(index), WindowScanner::scannedOpcodes) || !program.functions[index].hasBody())
                return;
            ScannedFunction function;
            WindowScanner { program, build, layouts, FunctionID(index) }.scan(allocationsOfLayouts, function);
            if (!function.objects.isEmpty() || !function.closureStores.isEmpty())
                found.append(WTF::move(function));
        });
        for (ScannedFunction& function : scanned) {
            objects.appendVector(WTF::move(function.objects));
            closureStores.appendVector(function.closureStores);
        }
        layoutsAllocatedOnlyByOperations = WTF::move(layouts.allocatedOnlyByOperations);
    }

    // A slot leaves the closures of one function unmade.
    void findCandidates()
    {
        struct Place {
            auto operator<=>(const Place&) const = default;

            FieldReference field;
            FunctionID function;
            ValueReference operation;
        };
        Vector<Place> places;
        for (const ClosureFieldFreshObject& object : objects) {
            forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
                if (initializer.closureFunction)
                    places.append({ { object.layout, initializer.field }, initializer.closureFunction, initializer.store ? initializer.store : ValueReference { object.function, object.allocation } });
            });
        }
        for (const ClosureStore& store : closureStores)
            places.append({ { store.layout, store.field }, store.closureFunction, { store.function, store.store } });
        AOT::sort(places);
        removeRepeatedElements(places);

        using Maker = std::pair<LayoutID, FunctionID>;
        Vector<Maker> makers = places.map([](const Place& place) {
            return Maker { place.field.layout, place.operation.function };
        });
        AOT::sort(makers);

        states.fill({ }, firstRows.last());
        keysOfCandidates.ensureSize(program.types.numberOfPropertyKeys());
        layoutsWithCandidates.ensureSize(program.layouts.size());
        layoutsWithEnvironmentCandidates.ensureSize(program.layouts.size());
        for (size_t next = 0; next < places.size();) {
            using Rank = std::tuple<bool, size_t, size_t>;
            FieldReference field = places[next].field;
            FunctionID chosen;
            Rank rankOfChosen;
            size_t numberOfFunctions = 0;
            for (; next < places.size() && places[next].field == field; ++numberOfFunctions) {
                FunctionID function = places[next].function;
                Rank rank { deferralFor(program, function) != ClosureDeferral::CapturesEnvironment, 0, 0 };
                for (; next < places.size() && places[next].field == field && places[next].function == function; ++next) {
                    ++std::get<1>(rank);
                    std::get<2>(rank) = std::max(std::get<2>(rank), std::ranges::equal_range(makers, Maker { field.layout, places[next].operation.function }).size());
                }
                if (chosen && rank <= rankOfChosen)
                    continue;
                chosen = function;
                rankOfChosen = rank;
            }

            FieldState& state = states[rowOf(field)];
            state.function = chosen;
            candidates.append(field);
            candidatesByKey.append({ fieldAt(program, field).key, field });
            keysOfCandidates.quickSet(fieldAt(program, field).key.index());
            if (std::get<0>(rankOfChosen))
                continue;
            layoutsWithEnvironmentCandidates.quickSet(field.layout.index());
            state.isRejected |= numberOfFunctions > 1;
        }
        AOT::sort(candidatesByKey);
        for (const ClosureStore& store : closureStores)
            states[rowOf({ store.layout, store.field })].isGivenWhereLayoutIsExpected |= store.level != KnowledgeLevel::Proven;

        for (size_t index = 0; index < program.layouts.size(); ++index) {
            for (uint32_t field = 0; field < program.layouts[index].fields.size(); ++field) {
                if (stateOf({ LayoutID(index), field }).function)
                    layoutsWithCandidates.quickSet(index);
            }
        }
    }

    void findEnteredCalls()
    {
        Vector<EnteredCall> calls;
        for (const ClosureFieldFreshObject& object : objects)
            calls.appendVector(object.enteredCalls);
        if (calls.isEmpty())
            return;
        AOT::sort(calls, { }, &EnteredCall::call);
        calleesByFunction.fill({ }, program.functions.size());
        for (const EnteredCall& call : calls) {
            CalleeState& callee = calleesByFunction[call.callee.index()];
            if (callSites.isEmpty() || callSites.last().call != call.call) {
                callSites.append(call);
                ++callee.numberOfEnteredSites;
            }
            ++callSites.last().numberOfWindows;
            ++callee.numberOfWindows;
        }
    }

    bool isCalleeOfWindow(uint32_t function) const { return !calleesByFunction.isEmpty() && calleesByFunction[function].numberOfEnteredSites; }

    CalleeState& calleeOfWindow(FunctionID function)
    {
        CalleeState& callee = calleesByFunction[function.index()];
        RELEASE_ASSERT(callee.numberOfEnteredSites);
        return callee;
    }

    std::optional<AccessKind> classifyAccess(ValueReference reference) const
    {
        const Value& value = valueAt(reference);
        auto kindIf = [](bool condition, AccessKind kind) {
            return condition ? std::optional(kind) : std::nullopt;
        };
        if (Value::opcodesWithFunction.contains(value.opcode)) {
            bool entersFunction = false;
            if (value.function() && isCalleeOfWindow(value.function().index())) {
                forEachFunctionReference(value, [&](FunctionID, FunctionReferenceKind kind) {
                    entersFunction |= keepsFunctionAlive(kind);
                });
            }
            return kindIf(entersFunction, FunctionEntry);
        }
        if (value.opcode == Opcode::HasKeyOrder)
            return kindIf(layoutsWithEnvironmentCandidates.quickGet(value.layout().index()), KeyOrderTest);
        if (value.opcode == Opcode::NewObject)
            return kindIf(layoutsWithCandidates.quickGet(value.layout().index()), Allocation);
        bool isTestInFrontOfReads = value.opcode == Opcode::IsSlotFilled && !value.accessSite();
        if (!Value::opcodesWithKey.contains(value.opcode) || isTestInFrontOfReads || value.opcode == Opcode::GetDeferredClosureSlot || value.opcode == Opcode::GetDeferredClosureSlotOrUndefined)
            return std::nullopt;

        PropertyKeyID key = value.key();
        if (!key || TypeTable::isProvisional(key) || !keysOfCandidates.quickGet(key.index()) || !reachesCandidate(reference, false))
            return std::nullopt;
        if (value.opcode == Opcode::IsSlotFilled)
            return PresenceTest;
        if (!Value::opcodesWithFieldAccess.contains(value.opcode))
            return NamedAccess;
        return storedOf(value.opcode) ? SlotStore : SlotRead;
    }

    void findAccesses()
    {
        using FoundAccess = std::pair<ValueReference, AccessKind>;
        Vector<FoundAccess> foundAccesses = collectInParallel<FoundAccess>(build.parallelLoops(), program.functions.size(), functionsBySize.span(), [&](unsigned index, Vector<FoundAccess>& found) {
            // classifyAccess() answers for no other operation.
            constexpr OpcodeSet classifiedOpcodes = Value::opcodesWithFunction | Value::opcodesWithKey | OpcodeSet { Opcode::HasKeyOrder, Opcode::NewObject };
            // It answers for no operation all of whose names are refused here. Whoever adds a case to it that goes by something else widens this.
            auto mayBeClassified = [&](NameKind kind, uint32_t name) {
                switch (kind) {
                case NameKind::Key:
                    return keysOfCandidates.get(name);
                case NameKind::Function:
                    return isCalleeOfWindow(name);
                case NameKind::Layout:
                    return layoutsWithCandidates.get(name) || layoutsWithEnvironmentCandidates.get(name);
                case NameKind::Variable:
                    return false;
                }
                RELEASE_ASSERT_NOT_REACHED();
            };
            forEachListedValueNaming(program, FunctionID(index), classifiedOpcodes, mayBeClassified, [&](BlockID, ValueID id, const Value&) {
                if (std::optional<AccessKind> kind = classifyAccess({ FunctionID(index), id }))
                    found.append({ { FunctionID(index), id }, *kind });
            });
        });
        for (const FoundAccess& access : foundAccesses)
            accesses[access.second].append(access.first);
    }

    // A store can leave its function for the allocations where every run of the function is part of a window that comes to the store.
    void findCalleeStores()
    {
        for (ValueReference entry : accesses[FunctionEntry])
            ++calleeOfWindow(valueAt(entry).function()).numberOfEntrySites;
        for (const EnteredCall& site : callSites) {
            CalleeState& callee = calleeOfWindow(site.callee);
            callee.isEnteredOnlyFromWindows = callee.numberOfEntrySites == callee.numberOfEnteredSites;
        }
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (const EnteredCall& site : callSites) {
                if (!site.passesOwnReceiver)
                    continue;
                CalleeState& callee = calleeOfWindow(site.callee);
                const CalleeState& caller = calleeOfWindow(site.call.function);
                if (!callee.isEnteredOnlyFromWindows || (caller.isEnteredOnlyFromWindows && site.numberOfWindows == caller.numberOfWindows))
                    continue;
                callee.isEnteredOnlyFromWindows = false;
                didChange = true;
            }
        }

        Vector<ValueReference> stores;
        for (const ClosureFieldFreshObject& object : objects) {
            for (const FieldInitializer& initializer : object.initializers) {
                if (initializer.isInCallee)
                    stores.append(initializer.store);
            }
        }
        AOT::sort(stores);
        for (ValueReference store : stores) {
            if (calleeStores.isEmpty() || calleeStores.last().store != store)
                calleeStores.append({ .store = store });
            ++calleeStores.last().numberOfWindows;
        }
        for (CalleeStoreState& store : calleeStores) {
            const CalleeState& callee = calleeOfWindow(store.store.function);
            store.canLeaveCallee = callee.isEnteredOnlyFromWindows && store.numberOfWindows == callee.numberOfWindows;
        }
    }

    bool mayBeOtherThanObject(FunctionID function, ValueID value) const
    {
        return !value || mayBeOtherThanObjectOrNullish(program.typeOf(program.at(function), value));
    }

    // What is undefined or null is no closure over anything.
    bool storesUndefinedOrNull(ValueReference reference) const
    {
        const Function& function = program.at(reference.function);
        const Value& store = function.at(reference.value);
        ValueID stored = store.operand(*storedOf(store.opcode));
        return stored && program.typeOf(function, stored).isUndefinedOrNull();
    }

    // The function of the closure that the store brings, where that closure is made over the object stored into.
    FunctionID findStoredClosureOverReceiver(ValueReference reference) const
    {
        const Function& function = program.at(reference.function);
        const Value& store = function.at(reference.value);
        const Value& closure = function.at(definitionThroughUpcasts(function, store.operand(*storedOf(store.opcode))));
        if (closure.opcode != Opcode::NewClosure || closure.isMadeOnDemand())
            return { };
        std::span<const ValueID> captures = closure.capturesOperands();
        bool isOverReceiver = captures.size() == 1 && sameObjectThroughCasts(function, captures[0]) == sameObjectThroughCasts(function, store.operand(0));
        return isOverReceiver ? closure.function() : FunctionID();
    }

    // What the slot cannot hold takes the property out of its slot.
    // Where every closure that comes into the field is made over the object that holds the field, whoever wants what the closure captures has it at hand: the object.
    void rejectFieldsByWhatComesIntoThem()
    {
        for (FieldReference candidate : candidates) {
            const LocationFlow& location = flowOf(program, candidate).location;
            bool everyWriterIsFound = location.writers == WriterState::AllKnown && location.storeNaming == StoreNaming::EveryStoreNamesLocation;
            bool mayGetOtherThanObject = isNeverMovedOutByStore(program, candidate) && !everyWriterIsFound && mayBeOtherThanObjectOrNullish(program.at(slotTypeOf(program, fieldAt(program, candidate))));
            if (mayGetOtherThanObject || dataMembersOfFields[rowOf(candidate)])
                reject(rowOf(candidate));
        }
        for (size_t index : layoutsWithEnvironmentCandidates) {
            const Layout& layout = program.layouts[index];
            if (!layout.flow.closureEnvironmentField && build.exceeds(LimitID::FieldsInKeyOrder, layout.fields.size() + 1, Subject { .layout = LayoutID(index) }))
                rejectFieldsOverEnvironments(LayoutID(index));
        }

        Vector<ValueReference> initializingStores;
        Vector<ValueReference> describedAllocations;
        for (const ClosureFieldFreshObject& object : objects) {
            describedAllocations.append({ object.function, object.allocation });
            for (const FieldInitializer& initializer : object.initializers) {
                if (initializer.store)
                    initializingStores.append(initializer.store);
            }
        }
        AOT::sort(initializingStores);
        AOT::sort(describedAllocations);

        // One summary per key, kept at the index of the first candidate with that key.
        struct AnyObjectStoreSummary {
            bool storesNonObject { false };
            bool storesOtherClosure { false };
            FunctionID closureOverReceiver;
        };
        Vector<AnyObjectStoreSummary> summaries;
        summaries.fill({ }, candidatesByKey.size());

        mayHoldOtherClosure.ensureSize(firstRows.last());
        for (AccessKind kind : { SlotStore, NamedAccess }) {
            for (ValueReference reference : accesses[kind]) {
                const Value& store = valueAt(reference);
                std::optional<uint8_t> stored = storedOf(store.opcode);
                if (!stored)
                    continue;
                bool isOtherThanObject = mayBeOtherThanObject(reference.function, store.operand(*stored));
                bool storesSomething = !storesUndefinedOrNull(reference);
                FunctionID closureOverReceiver = findStoredClosureOverReceiver(reference);
                if (storesIntoAnyObject(reference)) {
                    AnyObjectStoreSummary& summary = summaries[findCandidatesWithKey(store.key()).begin() - candidatesByKey.begin()];
                    summary.storesNonObject |= isOtherThanObject;
                    if (storesSomething) {
                        summary.storesOtherClosure |= !closureOverReceiver || (summary.closureOverReceiver && summary.closureOverReceiver != closureOverReceiver);
                        summary.closureOverReceiver = closureOverReceiver;
                    }
                    continue;
                }
                bool isBehindInitialization = kind == SlotStore && !std::ranges::binary_search(initializingStores, reference);
                forEachCandidateOf(reference, [&](FieldReference field, uint32_t row) {
                    if (isOtherThanObject && (kind == SlotStore || isNeverMovedOutByStore(program, field)))
                        reject(row);
                    if (isBehindInitialization && keepsEnvironment(states[row]))
                        reject(row);
                    if (storesSomething && states[row].function != closureOverReceiver)
                        mayHoldOtherClosure.quickSet(row);
                });
            }
        }
        for (size_t first = 0, index = 0; index < candidatesByKey.size(); ++index) {
            if (candidatesByKey[index].key != candidatesByKey[first].key)
                first = index;
            const AnyObjectStoreSummary& summary = summaries[first];
            FieldReference field = candidatesByKey[index].field;
            uint32_t row = rowOf(field);
            // Of what a store into any object brings into one field, only the type of the slot speaks.
            if (summary.storesNonObject && isNeverMovedOutByStore(program, field) && mayBeOtherThanObjectOrNullish(program.at(slotTypeOf(program, fieldAt(program, field)))))
                reject(row);
            if (summary.storesOtherClosure || (summary.closureOverReceiver && summary.closureOverReceiver != states[row].function))
                mayHoldOtherClosure.quickSet(row);
        }
        for (ValueReference reference : accesses[Allocation]) {
            const Value& allocation = valueAt(reference);
            if (allocation.opcode != Opcode::NewObject) {
                for (uint32_t field = 0; field < program.at(allocation.layout()).fields.size(); ++field)
                    mayHoldOtherClosure.quickSet(rowOf({ allocation.layout(), field }));
            }
            if (std::ranges::binary_search(describedAllocations, reference))
                continue;
            std::span<const uint32_t> fields = allocation.fields();
            for (size_t index = 0; index < fields.size(); ++index) {
                uint32_t row = rowOf({ allocation.layout(), fields[index] });
                if (!states[row].function)
                    continue;
                mayHoldOtherClosure.quickSet(row);
                if (mayBeOtherThanObject(reference.function, allocation.variadicOperands()[index]))
                    reject(row);
            }
        }
    }

    bool findsClosureOverReceiver(const Value& read) const
    {
        return !program.at(read.receivers()).completeMembers().value().empty() && holdsForFieldInEveryReceiver(read, false, [&](FieldReference field) {
            const FieldState& state = stateOf(field);
            const LocationFlow& location = flowOf(program, field).location;
            return state.canDefer()
                && deferralFor(program, state.function) == ClosureDeferral::CapturesObject
                && !mayHoldOtherClosure.quickGet(rowOf(field))
                && location.writers == WriterState::AllKnown
                && location.storeNaming == StoreNaming::EveryStoreNamesLocation;
        });
    }

    bool isFilledByEveryAllocation(FieldReference reference) const
    {
        uint32_t numberOfAllocations = allocationsOfLayouts[reference.layout.index()];
        return numberOfAllocations && layoutsAllocatedOnlyByOperations.quickGet(reference.layout.index()) && stateOf(reference).numberOfInitializingAllocations == numberOfAllocations;
    }

    // A read that gives undefined for an empty slot has no guard that says the slot is filled.
    bool mayFindSlotEmpty(const Value& read) const
    {
        return read.opcode == Opcode::GetSlotOrUndefined && !holdsForFieldInEveryReceiver(read, false, [&](FieldReference field) {
            const FieldFlow& flow = flowOf(program, field);
            bool isFilledWithClosure = stateOf(field).canDefer() && isFilledByEveryAllocation(field);
            return flow.stability.containsAll(stabilityOfFilledSlot) && (isFilledWithClosure || flow.occupancy == Occupancy::AlwaysFilled);
        });
    }

    Vector<Fold, 16> selectFolds(const ClosureFieldFreshObject& object) const
    {
        Vector<Fold, 16> folds(object.initializers.size(), [&](size_t index) {
            return object.initializers[index].store ? Fold::None : Fold::Value;
        });
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
                    && (!initializer.isInCallee || findCalleeStore(calleeStores, initializer.store).canLeaveCallee));
            if (canFold) {
                folds[index] = Fold::Value;
                if (!isListed)
                    listedFields.append(initializer.field);
                continue;
            }

            fieldsWithRemainingStores.append(initializer.field);
            if (isListed)
                continue;
            if (canListMoreFields && !stateOf({ object.layout, initializer.field }).canDefer() && tryCreatePlaceholder(program, { object.layout, initializer.field })) {
                folds[index] = Fold::Placeholder;
                listedFields.append(initializer.field);
            } else
                canListMoreFields = false;
        }

        for (size_t index = folds.size(); index-- && folds[index] != Fold::Value;)
            folds[index] = Fold::None;
        return folds;
    }

    void countFolds()
    {
        for (FieldReference candidate : candidates)
            states[rowOf(candidate)].numberOfInitializingAllocations = 0;
        for (CalleeStoreState& store : calleeStores)
            store.numberOfFolds = 0;
        for (const ClosureFieldFreshObject& object : objects) {
            Vector<Fold, 16> folds = selectFolds(object);
            forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
                uint32_t row = rowOf({ object.layout, initializer.field });
                if (!states[row].canDefer() || defers(object, initializer))
                    return;
                mayHoldOtherClosure.quickSet(row);
                if (initializer.calleeValue ? mayBeOtherThanObjectOrNullish(program.at(initializer.calleeValue->type)) : mayBeOtherThanObject(object.function, initializer.value))
                    reject(row);
            }, &folds);
            for (size_t index = 0; index < folds.size(); ++index) {
                if (folds[index] == Fold::Value && object.initializers[index].isInCallee)
                    ++findCalleeStore(calleeStores, object.initializers[index].store).numberOfFolds;
            }
            forEachFinalInitializer(object, [&](size_t index, const FieldInitializer& initializer) {
                if (folds[index] == Fold::Value && defers(object, initializer))
                    ++states[rowOf({ object.layout, initializer.field })].numberOfInitializingAllocations;
            });
        }

        for (CalleeStoreState& store : calleeStores) {
            if (store.numberOfFolds != store.numberOfWindows)
                numberOfChanges += std::exchange(store.canLeaveCallee, false);
        }
        for (FieldReference candidate : candidates) {
            const FieldState& state = stateOf(candidate);
            if (defersOverEnvironment(state) && state.numberOfInitializingAllocations != allocationsOfLayouts[candidate.layout.index()])
                reject(rowOf(candidate));
        }
    }

    // A read takes an empty slot for an absent property behind a test that no field of the object lies outside its slot, or without one where the property stays in its slot.
    // The first read makes the closure in the run time, which costs as much as forty closures made beside the allocation. Where the layout is proven the reads are few. Code that only expects it makes an object for whoever comes, so the closure waits only where no code that is expected to run often asks for it.
    void rejectFieldsOfExpectedLayoutsReadByWarmCode()
    {
        for (AccessKind kind : { SlotRead, NamedAccess }) {
            for (ValueReference reference : accesses[kind]) {
                if (storedOf(valueAt(reference).opcode) || isCold(heat.of(program, reference.function)).predicted())
                    continue;
                forEachCandidateOf(reference, [&](FieldReference, uint32_t row) {
                    if (states[row].isGivenWhereLayoutIsExpected)
                        reject(row);
                });
            }
        }
    }

    void rejectFieldsReadWhereTheyMayHaveLeftTheSlot()
    {
        for (AccessKind kind : { SlotRead, PresenceTest }) {
            for (ValueReference reference : accesses[kind]) {
                const Value& access = valueAt(reference);
                // A read that gives any value looks among the extra properties where it finds the slot empty and the object says that a field lies outside its slot.
                bool looksOutsideSlot = kind == SlotRead && (access.opcode != Opcode::GetSlotOrUndefined || access.type == TypeTable::anything);
                if (looksOutsideSlot || !reachesCandidate(reference, true))
                    continue;
                bool staysInSlot = holdsForFieldInEveryReceiver(access, true, [&](FieldReference field) {
                    return isNeverMovedOutByStore(program, field) || !slotTakesEveryValue(program, fieldAt(program, declarationOf(program, field)));
                });
                if (staysInSlot)
                    continue;
                forEachCandidateOf(reference, [&](FieldReference, uint32_t row) {
                    reject(row);
                });
            }
        }
    }

    void rejectFieldsOverEnvironments(LayoutID layout)
    {
        for (uint32_t field = 0; field < program.at(layout).fields.size(); ++field) {
            if (defersOverEnvironment(stateOf({ layout, field })))
                reject(rowOf({ layout, field }));
        }
    }

    ValueID environmentOf(const ClosureFieldFreshObject& object) const
    {
        ValueID environment;
        forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
            if (initializer.environment && defers(object, initializer))
                environment = initializer.environment;
        });
        return environment;
    }

    // An object has one hidden field, of one type, for the environment of its closures.
    void rejectFieldsOverSeveralEnvironments()
    {
        for (const ClosureFieldFreshObject& object : objects) {
            if (!layoutsWithEnvironmentCandidates.quickGet(object.layout.index()))
                continue;
            ValueID environment = environmentOf(object);
            bool hasSeveralEnvironments = false;
            forEachFinalInitializer(object, [&](size_t, const FieldInitializer& initializer) {
                hasSeveralEnvironments |= initializer.environment && initializer.environment != environment && defers(object, initializer);
            });
            if (hasSeveralEnvironments)
                rejectFieldsOverEnvironments(object.layout);
        }

        for (size_t index : layoutsWithEnvironmentCandidates) {
            const Layout& layout = program.layouts[index];
            TypeID type;
            if (layout.flow.closureEnvironmentField)
                type = layout.fields[*layout.flow.closureEnvironmentField].declared.written();
            bool hasSeveralTypes = false;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                const FieldState& state = stateOf({ LayoutID(index), field });
                if (!defersOverEnvironment(state))
                    continue;
                TypeID captured = program.at(state.function).captures[0].written();
                hasSeveralTypes |= type && type != captured;
                type = captured;
            }
            if (hasSeveralTypes)
                rejectFieldsOverEnvironments(LayoutID(index));
        }
    }

    const ClosureEnvironmentField* findEnvironmentField(LayoutID layout) const
    {
        auto found = std::ranges::lower_bound(environmentFields, layout, { }, &ClosureEnvironmentField::layout);
        return found != environmentFields.end() && found->layout == layout ? &*found : nullptr;
    }

    void finish()
    {
        for (FieldReference candidate : candidates) {
            const FieldState& state = stateOf(candidate);
            numberOfDeferringFields += state.canDefer();
            if (!defersOverEnvironment(state))
                continue;
            const Layout& layout = program.at(candidate.layout);
            std::optional<uint32_t> existingField = layout.flow.closureEnvironmentField;
            environmentFields.append({ candidate.layout, existingField.value_or(static_cast<uint32_t>(layout.fields.size())), program.at(state.function).captures[0].written(), !existingField });
        }
        AOT::sort(environmentFields, { }, &ClosureEnvironmentField::layout);
        removeRepeatedElements(environmentFields, [](const ClosureEnvironmentField& a, const ClosureEnvironmentField& b) {
            return a.layout == b.layout;
        });

        for (const CalleeStoreState& store : calleeStores) {
            if (store.canLeaveCallee)
                removedStores.append(store.store);
        }
        objects.removeAllMatching([&](const ClosureFieldFreshObject& object) {
            return !defersAnyClosure(object);
        });

        Vector<ValueReference> storesListedByAllocations = removedStores;
        for (const ClosureFieldFreshObject& object : objects) {
            Vector<Fold, 16> folds = selectFolds(object);
            for (size_t index = 0; index < folds.size(); ++index) {
                if (folds[index] == Fold::Value && object.initializers[index].store)
                    storesListedByAllocations.append(object.initializers[index].store);
            }
        }
        AOT::sort(storesListedByAllocations);
        closureStores.removeAllMatching([&](const ClosureStore& store) {
            return !defers(store.layout, store.field, store.closureFunction) || std::ranges::binary_search(storesListedByAllocations, ValueReference { store.function, store.store });
        });
        accesses[SlotRead].removeAllMatching([&](ValueReference reference) {
            return !reachesCandidate(reference, true);
        });
        auto isNarrowed = [&](FieldReference field, uint32_t row) {
            const FieldFlow& flow = flowOf(program, field);
            return states[row].canDefer() && isFilledByEveryAllocation(field) && flow.occupancy != Occupancy::AlwaysFilled && flow.stability.containsAll(stabilityOfFilledSlot);
        };
        BitVector narrowedKeys;
        narrowedKeys.ensureSize(program.types.numberOfPropertyKeys());
        for (const CandidateWithKey& candidate : candidatesByKey) {
            if (isNarrowed(candidate.field, rowOf(candidate.field)))
                narrowedKeys.quickSet(candidate.key.index());
        }
        accesses[NamedAccess].removeAllMatching([&](ValueReference reference) {
            if (storesIntoAnyObject(reference))
                return !narrowedKeys.quickGet(valueAt(reference).key().index());
            bool reachesNarrowedCandidate = false;
            forEachCandidateOf(reference, [&](FieldReference field, uint32_t row) {
                reachesNarrowedCandidate |= isNarrowed(field, row);
            });
            return !reachesNarrowedCandidate;
        });
        accesses[KeyOrderTest].removeAllMatching([&](ValueReference reference) {
            const ClosureEnvironmentField* field = findEnvironmentField(valueAt(reference).layout());
            return !field || !field->isNew;
        });
    }

    void run()
    {
        firstRows.reserveInitialCapacity(program.layouts.size() + 1);
        firstRows.append(0);
        for (const Layout& layout : program.layouts)
            firstRows.append(firstRows.last() + layout.fields.size());
        findObjects();
        if (objects.isEmpty() && closureStores.isEmpty())
            return;
        findCandidates();
        findEnteredCalls();
        findAccesses();
        findCalleeStores();
        rejectFieldsByWhatComesIntoThem();
        rejectFieldsOfExpectedLayoutsReadByWarmCode();
        for (uint64_t numberOfChangesBefore = numberOfChanges + 1; numberOfChangesBefore != numberOfChanges;) {
            numberOfChangesBefore = numberOfChanges;
            countFolds();
            rejectFieldsReadWhereTheyMayHaveLeftTheSlot();
            rejectFieldsOverSeveralEnvironments();
        }
        finish();
    }

    const Program& program;
    const Heat& heat;
    BuildContext& build;
    Vector<uint32_t> functionsBySize = largeFunctions(program, build.parallelLoops());
    // A class and the classes below it have one row for a field: that of the class that declares it.
    Vector<uint32_t> firstRows { };
    Vector<FieldState> states { };
    Vector<uint32_t> allocationsOfLayouts { };
    Vector<uint32_t> dataMembersOfFields { };
    Vector<ClosureFieldFreshObject> objects { };
    Vector<ClosureStore> closureStores { };
    Vector<FieldReference> candidates { };
    Vector<CandidateWithKey> candidatesByKey { };
    BitVector keysOfCandidates { };
    BitVector layoutsWithCandidates { };
    BitVector layoutsAllocatedOnlyByOperations { };
    BitVector layoutsWithEnvironmentCandidates { };
    BitVector mayHoldOtherClosure { };
    Vector<EnteredCall> callSites { };
    Vector<CalleeState> calleesByFunction { };
    Vector<CalleeStoreState> calleeStores { };
    std::array<Vector<ValueReference>, 7> accesses { };
    Vector<ValueReference> removedStores { };
    Vector<ClosureEnvironmentField> environmentFields { };
    uint64_t numberOfChanges { 0 };
    uint64_t numberOfDeferringFields { 0 };
};

// Every arm of a branch makes a closure of one function over the same values, and a parameter of the block behind the arms takes the one that was made.
struct ClosureJoin {
    BlockID block;
    uint32_t parameterIndex { 0 };
    Use use;
    Vector<ValueID, 2> closures;
    Vector<BlockID, 2> blocksOfClosures;
};

} // anonymous namespace

static void rewriteClosureFields(const AnalysisContext& context, const DeferralAnalysis& analysis, Function& function, TypeArena& types)
{
    const Program& program = context.program;
    IRBuilder builder(context, function, types);
    std::optional<FunctionAnalyses> analysesForUseLists;
    BitVector isInWindow;
    BitVector isHoisted;
    BitVector isRemoved;
    bool didDetachValues = false;
    Vector<ValueID> narrowedValues;

    auto useLists = [&]() -> const UseLists& {
        if (!analysesForUseLists)
            analysesForUseLists.emplace(context, function);
        return analysesForUseLists->useLists();
    };

    auto count = [&](StatisticID statistic) { context.build.statistics().count(statistic); };

    auto setResultType = [&](ValueID id, Value& value, TypeID previousType) {
        value.type = resultTypeOf(program, function, value, types);
        if (value.type != previousType)
            narrowedValues.append(id);
    };

    // A call that is resolved reads the closure only for what it captures, which is the object at hand. A read that nothing else uses goes, and with it the closure that it would make.
    auto tryTakeCaptureFromReceiver = [&](ValueID id) {
        const Value& read = function.at(id);
        if (!analysis.findsClosureOverReceiver(read))
            return false;
        const UseLists& uses = useLists();
        ValueID receiver = read.objectOperand();
        bool hasOtherUses = false;
        for (const Use& use : uses.usesOf(id)) {
            bool isTaken = use.kind == Use::Kind::Operand
                && function.at(use.user).opcode == Opcode::GetCapture
                && program.typeOf(function, receiver).isSubsetOf(program.typeOf(function, use.user), program);
            if (!isTaken) {
                hasOtherUses = true;
                continue;
            }
            // The object at hand may be known better than what the function says it captures.
            becomeUpcast(function, use.user, receiver);
            Value& capture = function.edit(use.user);
            setResultType(use.user, capture, capture.type);
        }
        if (hasOtherUses)
            return false;
        function.edit(uses.blockOf(id), BlockEdit::Values).values.removeFirst(id);
        didDetachValues = true;
        return true;
    };

    auto becomeReadOfDeferredClosure = [&](ValueID id) {
        Value& read = function.edit(id);
        Value previous = read;
        // The read that takes a filled slot has a guard that says so, or every receiver keeps the slot filled.
        bool mayFindSlotEmpty = analysis.mayFindSlotEmpty(previous);
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
        setResultType(id, read, previous.type);
        read.setExpected(expectedTypeOf(program, function, read, types));
    };

    auto hoist = [&](auto& self, ValueID id) -> void {
        if (!isInWindow.quickGet(id.index()) || isHoisted.quickSet(id.index()))
            return;
        RELEASE_ASSERT(createsValueWithoutRunningCode(function.at(id)));
        function.at(id).forEachChild([&](ValueID operand) {
            self(self, operand);
        });
    };

    auto takeOutOfItsBlock = [&](ValueID id) {
        bool wasListed = function.edit(useLists().blockOf(id), BlockEdit::Values).values.removeFirst(id);
        RELEASE_ASSERT(wasListed);
    };

    auto deferClosure = [&](const BlockValues& values, const FieldInitializer& initializer, Vector<ValueID, 8>& createdValues) {
        leaveUnmade(function.edit(initializer.closure));
        size_t firstIndex = values.size();
        if (!values.contains(initializer.closure)) {
            RELEASE_ASSERT(initializer.value == initializer.closure);
            takeOutOfItsBlock(initializer.closure);
            createdValues.append(initializer.closure);
            return firstIndex;
        }
        for (ValueID id = initializer.value;; id = function.at(id).operand(0)) {
            isHoisted.quickSet(id.index());
            firstIndex = std::min(firstIndex, values.find(id));
            if (id == initializer.closure)
                return firstIndex;
        }
    };

    auto listInitializers = [&](const ClosureFieldFreshObject& object) {
        Vector<Fold, 16> folds = analysis.selectFolds(object);
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

        BlockValues& values = function.edit(object.block, BlockEdit::Values).values;
        size_t allocationIndex = values.find(object.allocation);
        RELEASE_ASSERT(allocationIndex != notFound);
        size_t begin = allocationIndex;
        size_t end = allocationIndex + 1;

        isInWindow.ensureSize(function.values.size());
        isHoisted.ensureSize(function.values.size());
        isRemoved.ensureSize(function.values.size());
        for (size_t index = 0; index < folds.size(); ++index) {
            const FieldInitializer& initializer = object.initializers[index];
            if (folds[index] != Fold::Value || !initializer.store)
                continue;
            count(StatisticID::StoresListedByAllocations);
            ValueID lastValueInWindow = initializer.isInCallee ? initializer.value : initializer.store.value;
            size_t position = lastValueInWindow ? values.find(lastValueInWindow) : notFound;
            if (position != notFound)
                end = std::max(end, position + 1);
            if (initializer.isInCallee)
                continue;
            isRemoved.quickSet(initializer.store.value.index());
            didDetachValues = true;
        }
        for (size_t index = allocationIndex + 1; index < end; ++index)
            isInWindow.quickSet(values[index].index());

        builder.setOrigin(function.at(object.allocation).origin());
        Vector<ValueID, 16> listedValues;
        Vector<ValueID, 8> createdValues;
        auto listCreatedValue = [&](Value&& value) {
            createdValues.append(builder.addDetached(WTF::move(value)));
            listedValues.append(createdValues.last());
        };
        for (size_t index : sources) {
            const FieldInitializer& initializer = object.initializers[index];
            if (folds[index] == Fold::Placeholder) {
                listCreatedValue(*tryCreatePlaceholder(program, { object.layout, initializer.field }));
                continue;
            }

            bool isDeferred = analysis.defers(object, initializer);
            if (isDeferred)
                count(StatisticID::ClosuresDeferredInFields);
            if (initializer.calleeValue) {
                Value copy = *initializer.calleeValue;
                RELEASE_ASSERT(isDeferred == (copy.opcode == Opcode::NewClosure));
                if (isDeferred)
                    leaveUnmade(copy);
                listCreatedValue(WTF::move(copy));
                continue;
            }
            if (isDeferred)
                begin = std::min(begin, deferClosure(values, initializer, createdValues));
            else
                hoist(hoist, initializer.value);
            listedValues.append(initializer.value);
        }

        const ClosureEnvironmentField* environmentField = analysis.findEnvironmentField(object.layout);
        if (environmentField && !fields.contains(environmentField->field)) {
            ValueID environment = analysis.environmentOf(object);
            RELEASE_ASSERT(environment);
            fields.insert(0, environmentField->field);
            listedValues.insert(0, environment);
        }

        Vector<ValueID, 32> reordered;
        auto appendValues = [&](size_t from, size_t to, bool takesHoisted) {
            for (ValueID id : values.subspan(from, to - from)) {
                if (isHoisted.quickGet(id.index()) == takesHoisted && !isRemoved.quickGet(id.index()))
                    reordered.append(id);
            }
        };
        appendValues(begin, allocationIndex, false);
        appendValues(begin, end, true);
        reordered.appendVector(createdValues);
        reordered.append(object.allocation);
        appendValues(allocationIndex + 1, end, false);
        for (ValueID id : values.subspan(begin, end - begin)) {
            isInWindow.quickClear(id.index());
            isHoisted.quickClear(id.index());
            isRemoved.quickClear(id.index());
        }
        values.removeAt(begin, end - begin);
        values.insertVector(begin, reordered);

        Value& allocation = function.edit(object.allocation);
        allocation.setFields(fields.span());
        allocation.setValuesOperands(listedValues.span());
        allocation.setListsOnlyStoredValues(false);
    };

    // The store writes the word that says "not made yet". The closure has no code, so it stands right in front of the store, where nothing comes between the two.
    auto deferClosureOf = [&](const ClosureStore& store) {
        leaveUnmade(function.edit(store.closure));
        count(StatisticID::ClosuresDeferredInFields);

        Vector<ValueID, 2> moved;
        for (ValueID id = function.at(store.store).valueOperand(); id != store.closure; id = function.at(id).operand(0))
            moved.append(id);
        moved.append(store.closure);
        moved.reverse();
        BlockValues& values = function.edit(store.block, BlockEdit::Values).values;
        if (!values.contains(store.closure))
            takeOutOfItsBlock(store.closure);
        values.removeAllMatching([&](ValueID id) {
            return moved.contains(id);
        });
        size_t position = values.find(store.store);
        RELEASE_ASSERT(position != notFound);
        values.insertVector(position, moved);
    };

    auto removeStores = [&](std::span<const ValueReference> stores) {
        if (stores.empty())
            return;

        BitVector leavesEntryBlock(function.values.size());
        for (const ValueReference& store : stores) {
            leavesEntryBlock.quickSet(store.value.index());
            ValueID stored = function.at(store.value).valueOperand();
            ValueID definition = definitionThroughUpcasts(function, stored);
            if (function.at(definition).opcode != Opcode::NewClosure)
                continue;
            for (ValueID id = stored; id != definition; id = function.at(id).operand(0))
                leavesEntryBlock.quickSet(id.index());
            leavesEntryBlock.quickSet(definition.index());
        }
        function.edit(entryBlock, BlockEdit::Values).values.removeAllMatching([&](ValueID id) {
            return leavesEntryBlock.quickGet(id.index());
        });
        didDetachValues = true;
    };

    FunctionID id = context.id;
    for (ValueReference reference : editsOf(id, analysis.accesses[DeferralAnalysis::SlotRead])) {
        if (!tryTakeCaptureFromReceiver(reference.value))
            becomeReadOfDeferredClosure(reference.value);
    }
    for (ValueReference reference : editsOf(id, analysis.accesses[DeferralAnalysis::NamedAccess]))
        narrowedValues.append(reference.value);
    for (ValueReference reference : editsOf(id, analysis.accesses[DeferralAnalysis::KeyOrderTest])) {
        Value& test = function.edit(reference.value);
        Vector<uint32_t, 16> fields { analysis.findEnvironmentField(test.layout())->field };
        fields.append(test.fields());
        test.setFields(fields.span());
    }
    for (const ClosureFieldFreshObject& object : editsOf(id, analysis.objects))
        listInitializers(object);
    removeStores(editsOf(id, analysis.removedStores));
    for (const ClosureStore& store : editsOf(id, analysis.closureStores))
        deferClosureOf(store);

    if (!narrowedValues.isEmpty()) {
        FunctionAnalyses analyses(context, function);
        narrowValueTypes(TypePropagationInputs { program, id, function, analyses, types, context.build }, narrowedValues.span());
    }
    if (didDetachValues)
        compactFunction(function);
}

static void applyDeferrals(ProgramEditor& editor, BuildContext& build, const DeferralAnalysis& analysis)
{
    for (FieldReference candidate : analysis.candidates) {
        const FieldState& state = analysis.stateOf(candidate);
        if (!state.canDefer())
            continue;
        FieldFlow& flow = editor.at(candidate).flow;
        flow.location.deferral = deferralFor(editor.program(), state.function);
        flow.location.deferredFunction = state.function;
        if (!analysis.isFilledByEveryAllocation(candidate))
            continue;
        flow.location.initialization = InitializationState::SetAtCreation;
        if (flow.stability.containsAll(stabilityOfFilledSlot))
            flow.occupancy = Occupancy::AlwaysFilled;
    }

    for (const ClosureEnvironmentField& entry : analysis.environmentFields) {
        if (!entry.isNew)
            continue;
        Field field;
        field.declared = DeclaredType(Proven::byPosition(entry.type)).with(PositionFlags::IsFromCompiler);
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

    Vector<FunctionID> functions;
    auto addFunctionsOf = [&](const auto& edits) {
        for (const auto& edit : edits) {
            if (functions.isEmpty() || functions.last() != edit.function)
                functions.append(edit.function);
        }
    };
    addFunctionsOf(analysis.objects);
    for (auto kind : { DeferralAnalysis::SlotRead, DeferralAnalysis::NamedAccess, DeferralAnalysis::KeyOrderTest })
        addFunctionsOf(analysis.accesses[kind]);
    addFunctionsOf(analysis.removedStores);
    addFunctionsOf(analysis.closureStores);
    AOT::sort(functions);
    removeRepeatedElements(functions);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteClosureFields(AnalysisContext { build, analysis.program, nullptr, nullptr, id }, analysis, function, arenas.types);
    });
    build.statistics().count(StatisticID::FieldsDeferringClosures, analysis.numberOfDeferringFields);
}

// Asked of every function of the program, so it looks at edges first, and at the one block behind an edge that carries a closure.
static bool mayJoinClosuresOfArms(const Function& function)
{
    auto givesFieldParameter = [&](const Block& block, ValueID parameter) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return operationsGivingFields.contains(function.at(id).opcode) && std::ranges::contains(function.at(id).operands(), parameter);
        });
    };
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.terminator.targets, [&](const Target& target) {
            const Block& behind = function.at(target.block);
            for (uint32_t index = 0; index < target.arguments.size() && index < behind.parameters.size(); ++index) {
                if (function.at(target.arguments[index]).opcode == Opcode::NewClosure && givesFieldParameter(behind, behind.parameters[index]))
                    return true;
            }
            return false;
        });
    });
}

static void joinClosuresOfArms(const AnalysisContext& context, Function& function, TypeArena& types)
{
    FunctionAnalyses analyses(context, function);
    Vector<ClosureJoin, 8> joins;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        const Block& contents = function.at(block);
        std::span<const Edge> edges = analyses.cfg().incomingEdges(block);
        if (contents.isHandlerBlock() || edges.size() < 2 || std::ranges::any_of(edges, &Edge::isHandlerEdge))
            continue;
        const UseLists& uses = analyses.useLists();
        for (uint32_t parameterIndex = 0; parameterIndex < contents.parameters.size(); ++parameterIndex) {
            ValueID parameter = contents.parameters[parameterIndex];
            std::span<const Use> usesOfParameter = uses.usesOf(parameter);
            if (usesOfParameter.size() != 1 || usesOfParameter[0].kind != Use::Kind::Operand || usesOfParameter[0].block != block || !operationsGivingFields.contains(function.at(usesOfParameter[0].user).opcode))
                continue;
            ClosureJoin join { .block = block, .parameterIndex = parameterIndex, .use = usesOfParameter[0] };
            bool everyArmMakesTheClosure = std::ranges::all_of(edges, [&](const Edge& edge) {
                const Arguments& arguments = targetOf(function, edge).arguments;
                if (parameterIndex >= arguments.size())
                    return false;
                ValueID id = arguments[parameterIndex];
                const Value& closure = function.at(id);
                ValueLocation location = uses.locationOf(id);
                if (closure.opcode != Opcode::NewClosure || closure.type != function.at(parameter).type || uses.useCount(id) != 1 || location.placement != ValuePlacement::InBlock)
                    return false;
                if (!join.closures.isEmpty()) {
                    const Value& first = function.at(join.closures[0]);
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
    if (joins.isEmpty())
        return;

    IRBuilder builder(context, function, types, &analyses);
    for (const ClosureJoin& join : joins) {
        builder.setOrigin(function.at(join.closures[0]).origin());
        ValueID closure = builder.insertBefore(ProgramPoint { join.block, 0 }, Value(function.at(join.closures[0])));
        function.edit(join.use.user, ValueEdit::Definition).setOperand(join.use.operandIndex, closure);
    }
    builder.applyInsertions();
    for (const ClosureJoin& join : joins) {
        for (size_t index = 0; index < join.closures.size(); ++index) {
            bool wasListed = function.edit(join.blocksOfClosures[index], BlockEdit::Values).values.removeFirst(join.closures[index]);
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
    compactFunction(function);
}

void deferClosuresInFields(ProgramEditor& editor, const Heat& heat, BuildContext& build)
{
    if (build.recordedOptions().useWideReferences())
        return;

    const Program& program = editor.program();
    Vector<FunctionID> functionsWithJoins = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        // An arm hands on a closure that the function makes.
        return mayHoldValueWith(program, FunctionID(index), { Opcode::NewClosure }) && program.functions[index].hasBody() && mayJoinClosuresOfArms(program.functions[index]);
    }).map([](uint32_t index) {
        return FunctionID(index);
    });
    if (!functionsWithJoins.isEmpty()) {
        editFunctionsInParallel(editor, build, functionsWithJoins.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            joinClosuresOfArms(AnalysisContext { build, program, nullptr, nullptr, id }, function, arenas.types);
        });
    }

    DeferralAnalysis analysis { program, heat, build };
    analysis.run();
    if (analysis.numberOfDeferringFields)
        applyDeferrals(editor, build, analysis);
}

static constexpr OpcodeSet constantNodeOpcodes { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt };

namespace {

struct ConstructionSite {
    ValueID construction;
    ConstantCollectionKind kind;
    // The contents, or the record of the program that holds them already.
    DataNode contents;
    DataID data;
    Vector<ValueID> deadValues;
    // A collection of constants that nobody reads is made for nothing: no code runs, and only the lack of memory keeps it from being made.
    bool isUnused { false };
};

struct UnwrittenLiteral {
    ValueID literal;
    DataNode contents;
    DataID data;
};

struct DictionaryLiteral {
    ValueID dictionary;
    DataNode::Object contents;
    Vector<ValueID> deadValues;
    // Every name that the literal defines, in the order of the definitions: those of the contents, then those that are defined with something else.
    Vector<PropertyKeyID> names;
    bool runsAtMostOnce { false };
    bool isEmbedded { false };
    DataID data;
};

struct ConstantCollectionSites {
    bool isEmpty() const { return sites.isEmpty() && unwrittenLiterals.isEmpty() && dictionaries.isEmpty(); }

    FunctionID function;
    Vector<ConstructionSite> sites;
    Vector<UnwrittenLiteral> unwrittenLiterals;
    Vector<DictionaryLiteral> dictionaries;
};

using KeyIdentity = std::pair<uint8_t, uint64_t>;
using KeyIdentityTraits = WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint8_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
template<typename T> using ListsByHash = UncheckedKeyHashMap<uint64_t, Vector<T, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

} // anonymous namespace

// The key, and behind it the value if there is one.
template<typename Node>
static std::span<Node> partsOf(Node& entry, ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return { &entry, 1 };
    case ConstantCollectionKind::Map: {
        auto& pair = entry.template as<DataNode::Array>().elements;
        return { pair.begin(), pair.size() };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<KeyIdentity> identityOf(const DataNode& key)
{
    auto identity = [&](uint64_t payload) {
        return KeyIdentity { static_cast<uint8_t>(key.kind()), payload };
    };

    switch (key.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
        return identity(0);
    case DataNode::Kind::Boolean:
        return identity(key.as<DataNode::Boolean>().value);
    case DataNode::Kind::Number: {
        double number = key.as<DataNode::Number>().value;
        return identity(std::bit_cast<uint64_t>(number ? purifyNaN(number) : 0.0));
    }
    case DataNode::Kind::String:
        return identity(key.as<DataNode::String>().value.index());
    case DataNode::Kind::BigInt:
        return identity(key.as<DataNode::BigInt>().digits.index());
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool entriesArePairs(ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return false;
    case ConstantCollectionKind::Map:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Opcode operationFor(ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return Opcode::NewSetFromData;
    case ConstantCollectionKind::Map:
        return Opcode::NewMapFromData;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isUsableAs(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    return !contents.elements.isEmpty() && std::ranges::all_of(contents.elements, [&](const DataNode& entry) {
        const auto* pair = entry.tryAs<DataNode::Array>();
        if (entriesArePairs(kind) && (!pair || pair->elements.size() != 2))
            return false;
        return std::ranges::none_of(partsOf(entry, kind), [](const DataNode& part) {
            return part.is<DataNode::Hole>();
        });
    });
}

static bool createsObjects(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    return std::ranges::any_of(contents.elements, [&](const DataNode& entry) {
        return std::ranges::any_of(partsOf(entry, kind), [](const DataNode& part) {
            return !identityOf(part);
        });
    });
}

static bool isNegativeZero(const DataNode& node)
{
    const auto* number = node.tryAs<DataNode::Number>();
    return number && !number->value && std::signbit(number->value);
}

static std::optional<Vector<DataNode>> removeRepeatedKeys(std::span<const DataNode> entries, ConstantCollectionKind kind)
{
    UncheckedKeyHashMap<KeyIdentity, uint32_t, DefaultHash<KeyIdentity>, KeyIdentityTraits> positions;
    Vector<uint32_t> sources;
    sources.reserveInitialCapacity(entries.size());
    bool changesEntries = false;
    for (uint32_t index = 0; index < entries.size(); ++index) {
        const DataNode& key = partsOf(entries[index], kind)[0];
        changesEntries |= isNegativeZero(key);
        if (std::optional<KeyIdentity> identity = identityOf(key)) {
            auto position = positions.add(*identity, static_cast<uint32_t>(sources.size()));
            if (!position.isNewEntry) {
                // A repeated key keeps its first position and takes the last value. Equal keys are interchangeable.
                sources[position.iterator->value] = index;
                changesEntries = true;
                continue;
            }
        }
        sources.append(index);
    }
    if (!changesEntries)
        return std::nullopt;

    return sources.map([&](uint32_t source) {
        DataNode entry = entries[source];
        if (DataNode& key = partsOf(entry, kind)[0]; isNegativeZero(key))
            key = DataNode(DataNode::Number { 0 });
        return entry;
    });
}

static std::optional<DataNode> constantNodeOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constantID = constantThroughCasts(program, function, id);
    if (!constantID)
        return std::nullopt;
    const Value& constant = function.at(*constantID);
    switch (constant.opcode) {
    case Opcode::Undefined:
        return DataNode(DataNode::Undefined { });
    case Opcode::Null:
        return DataNode(DataNode::Null { });
    case Opcode::Boolean:
        return DataNode(DataNode::Boolean { constant.boolean() });
    case Opcode::Integer:
        return DataNode(DataNode::Number { static_cast<double>(constant.integer()) });
    case Opcode::Double:
        return DataNode(DataNode::Number { constant.number() });
    case Opcode::String:
        return DataNode(DataNode::String { constant.string() });
    case Opcode::BigInt:
        return DataNode(DataNode::BigInt { constant.digits() });
    default:
        RELEASE_ASSERT(!constantNodeOpcodes.contains(constant.opcode));
        return std::nullopt;
    }
}

// The arrays of a literal of constants that the optimizer has left in the heap, and that no store of the program reaches, are cells around one storage: it lies in the image unless it holds strings.
static std::optional<DataNode> tryReadUnwrittenLiteral(const AnalysisContext& context, const Function& function, const Value& literal)
{
    if (literal.storage() != AllocationStorage::Heap || literal.capacityOperand() || !literal.holes().empty() || !context.flow)
        return std::nullopt;
    AllocationSiteID site = literal.allocationSite();
    AllocationClassID allocationClass = site ? context.flow->at(site).allocationClass : AllocationClassID();
    if (!allocationClass || !context.flow->at(allocationClass).impossibleMutations.contains(ObjectMutation::StoreAfterCreation))
        return std::nullopt;

    DataNode::Array array { literal.contents(), { } };
    array.elements.reserveInitialCapacity(literal.elementsOperands().size());
    for (ValueID element : literal.elementsOperands()) {
        std::optional<DataNode> constant = constantNodeOf(context.program, function, element);
        if (!constant || constant->is<DataNode::BigInt>())
            return std::nullopt;
        array.elements.append(WTF::move(*constant));
    }
    if (array.elements.isEmpty() || arrayStorageOfData(context.program, array) != arrayStorageAtAllocation(context.program, context.program.at(literal.type)))
        return std::nullopt;
    return DataNode(WTF::move(array));
}

static bool namesGuard(const auto& user)
{
    bool result = false;
    user.forEachOperand([&](ValueID, uint32_t operandIndex) {
        result |= user.operandKind(operandIndex) == OperandKind::Guard;
    });
    return result;
}

// What completes a literal, the elements behind a spread for one, is no store for flow.
static bool hasToStayLiteral(FunctionAnalyses& analyses, ValueID literal)
{
    const Function& function = analyses.function();
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> array = escape.singleObject(literal);
    if (!array)
        return true;

    auto isUnderGuard = [&](const Use& use) {
        switch (use.kind) {
        case Use::Kind::Operand: {
            // A store of the array into another object may stand under a guard on that object.
            const Value& user = function.at(use.user);
            return user.operandKind(use.operandIndex) == OperandKind::Guard || (namesGuard(user) && guardedOf(user.opcode) == use.operandIndex);
        }
        case Use::Kind::TerminatorOperand:
            return namesGuard(function.at(use.block).terminator);
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (std::ranges::any_of(escape.usesOf(*array), [&](const ObjectUse& use) { return use.uses.containsAny(operandWritingUses) || isUnderGuard(use.use); }))
        return true;
    return std::ranges::any_of(escape.aliasesOf(*array), [&](ValueID alias) {
        return std::ranges::any_of(analyses.useLists().usesOf(alias), isUnderGuard);
    });
}

// An array literal without holes that nothing but the user sees.
static std::optional<ObjectID> findPrivateLiteral(FunctionAnalyses& analyses, ValueID value, ValueID user)
{
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> object = escape.singleObject(value);
    if (!object)
        return std::nullopt;
    const Value& literal = analyses.function().at(escape.at(*object).definition);
    if (literal.opcode != Opcode::NewArray || !literal.holes().empty() || literal.capacityOperand())
        return std::nullopt;
    std::span<const ObjectUse> uses = escape.usesOf(*object);
    if (uses.size() != 1 || uses.front().use.kind != Use::Kind::Operand || uses.front().use.user != user)
        return std::nullopt;
    bool canEraseWhenUnused = std::ranges::all_of(escape.aliasesOf(*object), [&](ValueID alias) {
        return analyses.useLists().locationOf(alias).placement == ValuePlacement::InBlock && analyses.isRemovableWhenUnused(alias);
    });
    return canEraseWhenUnused ? object : std::nullopt;
}

// The entries of a set are constants, those of a map are private literals themselves: pairs of constants.
static std::optional<DataNode> tryReadLiteral(FunctionAnalyses& analyses, ValueID value, ValueID user, bool holdsPairs, bool isPair, Vector<ValueID>& deadValues)
{
    std::optional<ObjectID> object = findPrivateLiteral(analyses, value, user);
    if (!object)
        return std::nullopt;
    ValueID id = analyses.escape().at(*object).definition;
    const Value& literal = analyses.function().at(id);
    if (isPair ? literal.elementsOperands().size() != 2 : literal.elementsOperands().empty())
        return std::nullopt;

    deadValues.append(analyses.escape().aliasesOf(*object));
    DataNode::Array array { literal.contents(), { } };
    array.elements.reserveInitialCapacity(literal.elementsOperands().size());
    for (ValueID element : literal.elementsOperands()) {
        std::optional<DataNode> node = holdsPairs ? tryReadLiteral(analyses, element, id, false, true, deadValues) : constantNodeOf(analyses.program(), analyses.function(), element);
        if (!node)
            return std::nullopt;
        array.elements.append(WTF::move(*node));
    }
    return DataNode(WTF::move(array));
}

static bool tryReadData(FunctionAnalyses& analyses, ValueID argument, ValueID definition, ConstructionSite& site)
{
    const Function& function = analyses.function();
    DataID data = function.at(definition).data();
    const auto* contents = analyses.program().at(data).root.tryAs<DataNode::Array>();
    if (!contents || !isUsableAs(*contents, site.kind))
        return false;

    const UseLists& useLists = analyses.useLists();
    ValueID user = site.construction;
    for (ValueID id = argument;; id = function.at(id).valueOperand()) {
        std::optional<Use> use = useLists.singleUse(id);
        if (!use || use->kind != Use::Kind::Operand || use->user != user || useLists.locationOf(id).placement != ValuePlacement::InBlock)
            return false;
        site.deadValues.append(id);
        if (id == definition)
            break;
        user = id;
    }

    // Every collection built from one array shares its objects, so the array has to be created exactly once per collection.
    if (createsObjects(*contents, site.kind) && useLists.blockOf(definition) != useLists.blockOf(site.construction))
        return false;

    site.data = data;
    return true;
}

static std::optional<ConstructionSite> tryReadConstruction(FunctionAnalyses& analyses, ValueID construction, ConstantCollectionKind kind)
{
    const Function& function = analyses.function();
    ConstructionSite site { .construction = construction, .kind = kind };
    ValueID argument = function.at(construction).argumentsOperands()[0];
    // What flow proved about the array stands between it and the constructor as a cast.
    ValueID definition = argument;
    while (isCastWithoutTest(function.at(definition).opcode))
        definition = function.at(definition).operand(0);
    if (function.at(definition).opcode != Opcode::Data) {
        std::optional<DataNode> contents = tryReadLiteral(analyses, argument, construction, entriesArePairs(kind), false, site.deadValues);
        if (!contents)
            return std::nullopt;
        site.contents = WTF::move(*contents);
    } else if (!tryReadData(analyses, argument, definition, site))
        return std::nullopt;

    const auto& contents = (site.data ? analyses.program().at(site.data).root : site.contents).as<DataNode::Array>();
    if (std::optional<Vector<DataNode>> entries = removeRepeatedKeys(contents.elements.span(), kind)) {
        site.contents = DataNode(DataNode::Array { contents.contents, WTF::move(*entries) });
        site.data = { };
    }
    site.isUnused = !analyses.useLists().useCount(construction);
    return site;
}

// `{ "+1": "a", smile: "b", .. }` where the type names no keys: an ordinary object that is filled name by name behind its allocation.
static std::optional<DictionaryLiteral> tryReadDictionary(FunctionAnalyses& analyses, ValueID dictionary)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    ValueLocation location = analyses.useLists().locationOf(dictionary);
    if (location.placement != ValuePlacement::InBlock)
        return std::nullopt;
    auto isConstantIndex = [&](ValueID property) {
        std::optional<ValueID> constant = constantThroughCasts(program, function, property);
        return constant && function.at(*constant).opcode == Opcode::Integer && function.at(*constant).integer() >= 0 && function.at(*constant).integer() <= MAX_ARRAY_INDEX;
    };

    DictionaryLiteral literal { .dictionary = dictionary, .runsAtMostOnce = analyses.executionCountOf(location.block) == ExecutionCount::AtMostOnce };
    bool takesConstants = true;
    Vector<ValueID, 2> aliases { dictionary };
    HashMap<PropertyKeyID, uint32_t> positions;
    for (ValueID id : function.at(location.block).values.subspan(location.index + 1)) {
        const Value& value = function.at(id);
        bool takesDictionary = false;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            takesDictionary |= aliases.contains(operand);
        });
        if (!takesDictionary)
            continue;
        if (isCastWithoutTest(value.opcode)) {
            aliases.append(id);
            continue;
        }
        if (value.opcode == Opcode::ElemDefine && !aliases.contains(value.valueOperand()) && isConstantIndex(value.propertyOperand()))
            continue;
        if (value.opcode != Opcode::PropDefine || aliases.contains(value.valueOperand()) || program.at(value.key()).kind != PropertyKeyKind::Text || program.isIndexLike(value.key()))
            break;
        auto position = positions.add(value.key(), literal.names.size());
        if (position.isNewEntry)
            literal.names.append(value.key());
        std::optional<DataNode> member = takesConstants ? constantNodeOf(analyses.program(), analyses.function(), value.valueOperand()) : std::nullopt;
        takesConstants = !!member;
        if (!member)
            continue;
        if (position.isNewEntry)
            literal.contents.members.append(DataMember { value.key(), WTF::move(*member) });
        else
            literal.contents.members[position.iterator->value].value = WTF::move(*member);
        literal.deadValues.append(id);
    }
    if (literal.names.isEmpty())
        return std::nullopt;
    return literal;
}

static std::optional<ConstantCollectionKind> collectionKindOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || call.receiverOperand() || call.argumentsOperands().size() != 1)
        return std::nullopt;

    const BuiltinTable& builtins = program.builtins();
    const BuiltinOverload& overload = builtins.at(call.overload());
    if (overload.mode != BuiltinCallMode::Construct)
        return std::nullopt;

    // A private literal of constants is iterated, and its entries are added, through locked prototypes.
    BuiltinID constructor = builtins.canonicalID(overload.builtin);
    if (constructor == builtins.at(BuiltinClasses::Set).constructor)
        return ConstantCollectionKind::Set;
    if (constructor == builtins.at(BuiltinClasses::Map).constructor)
        return ConstantCollectionKind::Map;
    return std::nullopt;
}

static void collectSites(const AnalysisContext& context, Vector<ConstantCollectionSites>& found)
{
    constexpr OpcodeSet opcodesOfSites { Opcode::NewDictionary, Opcode::NewArray, Opcode::CallBuiltin };
    if (!mayListValueWith(context.program, context.id, opcodesOfSites))
        return;
    const Function& function = context.program.at(context.id);
    if (!function.hasBody() || !context.build.isEnabled(PassID::EmbedConstantCollections, context.id))
        return;

    std::optional<FunctionAnalyses> analysesIfAsked;
    auto analyses = [&]() -> FunctionAnalyses& {
        return analysesIfAsked ? *analysesIfAsked : analysesIfAsked.emplace(context, function);
    };
    ConstantCollectionSites inFunction { .function = context.id };
    forEachListedValue(context.program, context.id, opcodesOfSites, [&](BlockID, uint32_t, ValueID id, const Value& value) {
        if (value.opcode == Opcode::NewDictionary) {
            if (std::optional<DictionaryLiteral> literal = tryReadDictionary(analyses(), id))
                inFunction.dictionaries.append(WTF::move(*literal));
        } else if (value.opcode == Opcode::NewArray) {
            std::optional<DataNode> contents = tryReadUnwrittenLiteral(context, function, value);
            if (!contents)
                return;
            // Storage with cells is made by the first array, for those that follow.
            if (!isStorageOfArray(*contents->tryAs<DataNode::Array>()) && analyses().executionCountOf(analyses().useLists().blockOf(id)) == ExecutionCount::AtMostOnce)
                return;
            if (!hasToStayLiteral(analyses(), id))
                inFunction.unwrittenLiterals.append({ id, WTF::move(*contents), { } });
        } else if (std::optional<ConstantCollectionKind> kind = collectionKindOf(context.program, value)) {
            if (std::optional<ConstructionSite> site = tryReadConstruction(analyses(), id, *kind))
                inFunction.sites.append(WTF::move(*site));
        }
    });
    // The literal that a collection is made of goes away with the construction.
    inFunction.unwrittenLiterals.removeAllMatching([&](const UnwrittenLiteral& unwritten) {
        return std::ranges::any_of(inFunction.sites, [&](const ConstructionSite& site) {
            return site.deadValues.contains(unwritten.literal);
        });
    });
    if (!inFunction.isEmpty())
        found.append(WTF::move(inFunction));
}

static void add(Hasher& hasher, const DataNode& node)
{
    add(hasher, node.kind());
    if (std::optional<KeyIdentity> identity = identityOf(node))
        add(hasher, identity->second);
    else if (const auto* array = node.tryAs<DataNode::Array>()) {
        add(hasher, array->contents);
        for (const DataNode& element : array->elements)
            add(hasher, element);
    } else if (const auto* object = node.tryAs<DataNode::Object>()) {
        add(hasher, object->layout);
        for (const DataMember& member : object->members) {
            add(hasher, member.key);
            add(hasher, member.value);
        }
    }
}

static DataID findOrAddData(ProgramEditor& editor, ListsByHash<DataID>& recordsByHash, DataNode&& root, ModuleID module)
{
    Vector<DataID, 1>& records = recordsByHash.add(computeHash(root), Vector<DataID, 1>()).iterator->value;
    for (DataID record : records) {
        if (editor.program().at(record).root == root)
            return record;
    }
    records.append(editor.addData(Data { WTF::move(root), module }));
    return records.last();
}

static void rewriteFunction(const AnalysisContext& context, Function& function, TypeArena& types, const ConstantCollectionSites& found)
{
    BitVector deadValues(function.values.size());
    auto addDeadValues = [&](const Vector<ValueID>& values) {
        for (ValueID dead : values)
            deadValues.quickSet(dead.index());
    };
    auto becomeReadOfData = [&](ValueID id, Opcode operation, DataID data) -> Value& {
        Value& value = function.edit(id);
        AllocationSiteID allocationSite = value.allocationSite();
        ArrayStorage arrayStorage = operation == Opcode::Data ? value.arrayStorage() : ArrayStorage::Other;
        value.become(operation);
        value.setData(data);
        value.setAllocationSite(allocationSite);
        if (operation == Opcode::Data)
            value.setArrayStorage(arrayStorage);
        value.type = resultTypeOf(context.program, function, value, types);
        return value;
    };

    if (!found.unwrittenLiterals.isEmpty() || !found.dictionaries.isEmpty()) {
        // The constants that only the literals have used.
        FunctionAnalyses analyses(context, function);
        const UseLists& useLists = analyses.useLists();
        HashMap<ValueID, uint32_t> usesByLiterals;
        for (const UnwrittenLiteral& unwritten : found.unwrittenLiterals) {
            for (ValueID element : function.at(unwritten.literal).elementsOperands())
                ++usesByLiterals.add(element, 0).iterator->value;
        }
        for (const DictionaryLiteral& dictionary : found.dictionaries) {
            for (ValueID definition : dictionary.deadValues)
                ++usesByLiterals.add(function.at(definition).valueOperand(), 0).iterator->value;
        }
        for (auto [id, uses] : usesByLiterals) {
            if (constantNodeOpcodes.contains(function.at(id).opcode) && useLists.useCount(id) == uses && useLists.locationOf(id).placement == ValuePlacement::InBlock)
                deadValues.quickSet(id.index());
        }
    }
    for (const DictionaryLiteral& literal : found.dictionaries) {
        addDeadValues(literal.deadValues);
        Value& dictionary = function.edit(literal.dictionary);
        dictionary.become(Opcode::Data);
        dictionary.setData(literal.data);
    }
    for (const UnwrittenLiteral& unwritten : found.unwrittenLiterals)
        becomeReadOfData(unwritten.literal, Opcode::Data, unwritten.data);
    Vector<ValueID> constructions;
    constructions.reserveInitialCapacity(found.sites.size());
    for (const ConstructionSite& site : found.sites) {
        addDeadValues(site.deadValues);
        if (site.isUnused) {
            deadValues.quickSet(site.construction.index());
            continue;
        }
        becomeReadOfData(site.construction, operationFor(site.kind), site.data).setExpected({ });
        constructions.append(site.construction);
    }

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        BitVector deadIndexes;
        for (size_t index = 0; index < values.size(); ++index) {
            if (deadValues.quickGet(values[index].index()))
                deadIndexes.set(index);
        }
        if (!deadIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), deadIndexes);
    }
    narrowValueTypes(TypePropagationInputs { context.program, context.id, function, analyses, types, context.build }, constructions.span());
    compactFunction(function);
}

// Objects with the same names in the same order share their Structures: the first pays for them, and a place that reads a name from all of them finds one Structure there.
static void keepDictionariesThatShareNothing(Vector<ConstantCollectionSites>& found)
{
    ListsByHash<DictionaryLiteral*> literalsByNames;
    for (ConstantCollectionSites& entry : found) {
        for (DictionaryLiteral& literal : entry.dictionaries) {
            Hasher hasher;
            for (PropertyKeyID name : literal.names)
                add(hasher, name);
            literalsByNames.add(hasher.hash(), Vector<DictionaryLiteral*, 1>()).iterator->value.append(&literal);
        }
    }
    for (auto& literals : literalsByNames.values()) {
        for (DictionaryLiteral* literal : literals) {
            bool isOnlyOneWithItsNames = literal->runsAtMostOnce && std::ranges::none_of(literals, [&](const DictionaryLiteral* other) {
                return other != literal && other->names == literal->names;
            });
            literal->isEmbedded = !literal->contents.members.isEmpty() && (isOnlyOneWithItsNames || literal->contents.members.size() > static_cast<size_t>(Structure::s_maxTransitionLength));
        }
    }
    for (ConstantCollectionSites& entry : found) {
        entry.dictionaries.removeAllMatching([](const DictionaryLiteral& literal) {
            return !literal.isEmbedded;
        });
    }
    found.removeAllMatching([](const ConstantCollectionSites& entry) {
        return entry.isEmpty();
    });
}

ConstantCollections embedConstantCollections(ProgramEditor& editor, const FlowResults& flow, const EffectSummaries* summariesFromFlow, BuildContext& build)
{
    const Program& program = editor.program();
    std::optional<EffectSummaries> ownSummaries;
    if (!summariesFromFlow)
        summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlow(program, flow, build));
    Vector<ConstantCollectionSites> found = collectInParallel<ConstantCollectionSites>(build.parallelLoops(), program.functions.size(), largeFunctions(program, build.parallelLoops()).span(), [&](unsigned index, Vector<ConstantCollectionSites>& foundInChunk) {
        collectSites({ build, program, &flow, summariesFromFlow, FunctionID(index) }, foundInChunk);
    });
    keepDictionariesThatShareNothing(found);

    ConstantCollections collections;
    ListsByHash<DataID> recordsByHash;
    for (ConstantCollectionSites& entry : found) {
        ModuleID module = program.at(entry.function).module;
        for (ConstructionSite& site : entry.sites) {
            if (site.isUnused)
                continue;
            if (!site.data)
                site.data = findOrAddData(editor, recordsByHash, WTF::move(site.contents), module);
            collections.m_collections.append(ConstantCollection { site.data, site.kind });
        }
        for (UnwrittenLiteral& unwritten : entry.unwrittenLiterals)
            unwritten.data = findOrAddData(editor, recordsByHash, WTF::move(unwritten.contents), module);
        for (DictionaryLiteral& dictionary : entry.dictionaries)
            dictionary.data = findOrAddData(editor, recordsByHash, DataNode(WTF::move(dictionary.contents)), module);
    }
    build.statistics().count(StatisticID::ConstantCollectionsEmbedded, collections.m_collections.size());

    Vector<FunctionID> functions = found.map([](const ConstantCollectionSites& entry) {
        return entry.function;
    });
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction({ build, program, &flow, summariesFromFlow, id }, function, arenas.types, found[index]);
    });

    sortAndRemoveDuplicates(build.parallelLoops(), collections.m_collections);
    if (ownSummaries)
        build.destroyLater(WTF::move(*ownSummaries));
    return collections;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
