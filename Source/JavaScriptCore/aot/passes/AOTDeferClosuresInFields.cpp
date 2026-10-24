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
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

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
class WindowScanner {
public:
    static constexpr OpcodeSet scannedOpcodes = operationsGivingFields | OpcodeSet { Opcode::CloneAs };

    WindowScanner(const Program& program, BuildContext& build, const DeferrableLayouts& layouts, FunctionID id)
        : m_program(program)
        , m_build(build)
        , m_layouts(layouts)
        , m_id(id)
        , m_function(program.at(id))
    {
    }

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
            if (mayAllocateOtherLayout(m_program, value) || !isEnabled || value.opcode != Opcode::NewObject)
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

private:
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
        const LayoutSet& receivers = m_program.at(store.receivers());
        return receivers.isComplete && receivers.members.size() == 1 ? receivers.members[0] : LayoutID();
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
        // Code that only expects the layout of its objects reads their members by name as well, and a read by name makes the closure in the run time, which costs as much as forty closures made beside the allocation.
        if (!isClosure(store.valueOperand()) || store.level() != KnowledgeLevel::Proven)
            return;
        const LayoutSet& receivers = m_program.at(store.receivers());
        if (!receivers.isComplete || receivers.members.isEmpty())
            return;
        std::optional<FieldReference> declaration;
        for (LayoutID member : receivers.members) {
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
            found.append({ m_id, block, id, closure, declaration->layout, declaration->field, m_function.at(closure).function() });
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
            return WindowScanner(m_program, m_build, m_layouts, callee).findPrologue();
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
    const Function& m_function;
    std::optional<FunctionAnalyses> m_analyses;
    HashMap<FunctionID, std::unique_ptr<CalleePrologue>> m_prologues;
    Vector<ValueID, 32> m_window;
    Vector<OptionSet<Trait>> m_traits;
    bool m_hasSkippedOperation { false };
};

struct DeferralAnalysis {
    enum AccessKind : uint8_t { SlotRead, SlotStore, PresenceTest, NamedAccess, KeyOrderTest, FunctionEntry, Allocation };

    DeferralAnalysis(const Program& program, BuildContext& build)
        : program(program)
        , build(build)
        , functionsBySize(largeFunctions(program, build.parallelLoops()))
    {
        firstRows.reserveInitialCapacity(program.layouts.size() + 1);
        firstRows.append(0);
        for (const Layout& layout : program.layouts)
            firstRows.append(firstRows.last() + layout.fields.size());
    }

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

    // An access to a slot names the layouts of its objects. For an access by name the type of the object says them.
    void forEachCandidateOf(ValueReference reference, const Invocable<void(FieldReference, uint32_t row)> auto& functor) const
    {
        const Function& function = program.at(reference.function);
        const Value& access = function.at(reference.value);
        std::span<const LayoutID> members;
        if (Value::opcodesWithFieldAccess.contains(access.opcode) || access.opcode == Opcode::IsSlotFilled)
            members = program.at(access.receivers()).members.span();
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
        bool result = false;
        forEachCandidateOf(reference, [&](FieldReference, uint32_t row) {
            result |= !onlyIfItDefers || states[row].canDefer();
        });
        return result;
    }

    // `ifAbsent` is the answer for a receiver without the field.
    bool holdsForFieldInEveryReceiver(const Value& access, bool ifAbsent, const Invocable<bool(FieldReference)> auto& predicate) const
    {
        return std::ranges::all_of(program.at(access.receivers()).members, [&](LayoutID member) {
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
            WindowScanner(program, build, layouts, FunctionID(index)).scan(allocationsOfLayouts, function);
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
            keysOfCandidates.quickSet(fieldAt(program, field).key.index());
            if (std::get<0>(rankOfChosen))
                continue;
            layoutsWithEnvironmentCandidates.quickSet(field.layout.index());
            state.isRejected |= numberOfFunctions > 1;
        }

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
        if (value.opcode == Opcode::NewObject || value.opcode == Opcode::CloneAs)
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
            constexpr OpcodeSet classifiedOpcodes = Value::opcodesWithFunction | Value::opcodesWithKey | OpcodeSet { Opcode::HasKeyOrder, Opcode::NewObject, Opcode::CloneAs };
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
            ++calleesByFunction[valueAt(entry).function().index()].numberOfEntrySites;
        for (const EnteredCall& site : callSites) {
            CalleeState& callee = calleesByFunction[site.callee.index()];
            callee.isEnteredOnlyFromWindows = callee.numberOfEntrySites == callee.numberOfEnteredSites;
        }
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (const EnteredCall& site : callSites) {
                CalleeState& callee = calleesByFunction[site.callee.index()];
                const CalleeState& caller = calleesByFunction[site.call.function.index()];
                if (!site.passesOwnReceiver || !callee.isEnteredOnlyFromWindows || (caller.isEnteredOnlyFromWindows && site.numberOfWindows == caller.numberOfWindows))
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
            const CalleeState& callee = calleesByFunction[store.store.function.index()];
            store.canLeaveCallee = callee.isEnteredOnlyFromWindows && store.numberOfWindows == callee.numberOfWindows;
        }
    }

    bool mayBeOtherThanObject(FunctionID function, ValueID value) const
    {
        return !value || mayBeOtherThanObjectOrNullish(program.typeOf(program.at(function), value));
    }

    // What is undefined or null is no closure over anything.
    bool storesClosureOverReceiver(ValueReference reference, FunctionID closureFunction) const
    {
        const Function& function = program.at(reference.function);
        const Value& store = function.at(reference.value);
        ValueID stored = store.operand(*storedOf(store.opcode));
        if (stored && program.typeOf(function, stored).isUndefinedOrNull())
            return true;
        const Value& closure = function.at(definitionThroughUpcasts(function, stored));
        if (closure.opcode != Opcode::NewClosure || closure.function() != closureFunction || closure.isMadeOnDemand())
            return false;
        std::span<const ValueID> captures = closure.capturesOperands();
        return captures.size() == 1 && sameObjectThroughCasts(function, captures[0]) == sameObjectThroughCasts(function, store.operand(0));
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

        mayHoldOtherClosure.ensureSize(firstRows.last());
        for (AccessKind kind : { SlotStore, NamedAccess }) {
            for (ValueReference reference : accesses[kind]) {
                const Value& store = valueAt(reference);
                std::optional<uint8_t> stored = storedOf(store.opcode);
                if (!stored)
                    continue;
                bool isOtherThanObject = mayBeOtherThanObject(reference.function, store.operand(*stored));
                bool isBehindInitialization = kind == SlotStore && !std::ranges::binary_search(initializingStores, reference);
                forEachCandidateOf(reference, [&](FieldReference field, uint32_t row) {
                    if (isOtherThanObject && (kind == SlotStore || isNeverMovedOutByStore(program, field)))
                        reject(row);
                    if (isBehindInitialization && keepsEnvironment(states[row]))
                        reject(row);
                    if (!storesClosureOverReceiver(reference, states[row].function))
                        mayHoldOtherClosure.quickSet(row);
                });
            }
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
        return !program.at(read.receivers()).members.isEmpty() && holdsForFieldInEveryReceiver(read, false, [&](FieldReference field) {
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
                type = layout.fields[*layout.flow.closureEnvironmentField].declared.type;
            bool hasSeveralTypes = false;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                const FieldState& state = stateOf({ LayoutID(index), field });
                if (!defersOverEnvironment(state))
                    continue;
                TypeID captured = program.at(state.function).captures[0].type;
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
            environmentFields.append({ candidate.layout, existingField.value_or(static_cast<uint32_t>(layout.fields.size())), program.at(state.function).captures[0].type, !existingField });
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
        accesses[NamedAccess].removeAllMatching([&](ValueReference reference) {
            bool isNarrowed = false;
            forEachCandidateOf(reference, [&](FieldReference field, uint32_t row) {
                const FieldFlow& flow = flowOf(program, field);
                isNarrowed |= states[row].canDefer() && isFilledByEveryAllocation(field) && flow.occupancy != Occupancy::AlwaysFilled && flow.stability.containsAll(stabilityOfFilledSlot);
            });
            return !isNarrowed;
        });
        accesses[KeyOrderTest].removeAllMatching([&](ValueReference reference) {
            const ClosureEnvironmentField* field = findEnvironmentField(valueAt(reference).layout());
            return !field || !field->isNew;
        });
    }

    void run()
    {
        findObjects();
        if (objects.isEmpty() && closureStores.isEmpty())
            return;
        findCandidates();
        findEnteredCalls();
        findAccesses();
        findCalleeStores();
        rejectFieldsByWhatComesIntoThem();
        for (uint64_t numberOfChangesBefore = numberOfChanges + 1; numberOfChangesBefore != numberOfChanges;) {
            numberOfChangesBefore = numberOfChanges;
            countFolds();
            rejectFieldsReadWhereTheyMayHaveLeftTheSlot();
            rejectFieldsOverSeveralEnvironments();
        }
        finish();
    }

    const Program& program;
    BuildContext& build;
    Vector<uint32_t> functionsBySize;
    // A class and the classes below it have one row for a field: that of the class that declares it.
    Vector<uint32_t> firstRows;
    Vector<FieldState> states;
    Vector<uint32_t> allocationsOfLayouts;
    Vector<uint32_t> dataMembersOfFields;
    Vector<ClosureFieldFreshObject> objects;
    Vector<ClosureStore> closureStores;
    Vector<FieldReference> candidates;
    BitVector keysOfCandidates;
    BitVector layoutsWithCandidates;
    BitVector layoutsAllocatedOnlyByOperations;
    BitVector layoutsWithEnvironmentCandidates;
    BitVector mayHoldOtherClosure;
    Vector<EnteredCall> callSites;
    Vector<CalleeState> calleesByFunction;
    Vector<CalleeStoreState> calleeStores;
    std::array<Vector<ValueReference>, 7> accesses;
    Vector<ValueReference> removedStores;
    Vector<ClosureEnvironmentField> environmentFields;
    uint64_t numberOfChanges { 0 };
    uint64_t numberOfDeferringFields { 0 };
};

class ClosureFieldRewriter {
public:
    ClosureFieldRewriter(const AnalysisContext& context, const DeferralAnalysis& analysis, Function& function, TypeArena& types)
        : m_context(context)
        , m_program(context.program)
        , m_analysis(analysis)
        , m_function(function)
        , m_types(types)
        , m_builder(context, function, types)
    {
    }

    void run()
    {
        FunctionID id = m_context.id;
        for (ValueReference reference : editsOf(id, m_analysis.accesses[DeferralAnalysis::SlotRead])) {
            if (!tryTakeCaptureFromReceiver(reference.value))
                becomeReadOfDeferredClosure(reference.value);
        }
        for (ValueReference reference : editsOf(id, m_analysis.accesses[DeferralAnalysis::NamedAccess]))
            m_narrowedValues.append(reference.value);
        for (ValueReference reference : editsOf(id, m_analysis.accesses[DeferralAnalysis::KeyOrderTest])) {
            Value& test = m_function.edit(reference.value);
            Vector<uint32_t, 16> fields { m_analysis.findEnvironmentField(test.layout())->field };
            fields.append(test.fields());
            test.setFields(fields.span());
        }
        for (const ClosureFieldFreshObject& object : editsOf(id, m_analysis.objects))
            listInitializers(object);
        removeStores(editsOf(id, m_analysis.removedStores));
        for (const ClosureStore& store : editsOf(id, m_analysis.closureStores))
            deferClosureOf(store);

        if (!m_narrowedValues.isEmpty()) {
            FunctionAnalyses analyses(m_context, m_function);
            narrowValueTypes(TypePropagationInputs { m_program, id, m_function, analyses, m_types, m_context.build }, m_narrowedValues.span());
        }
        if (m_didDetachValues)
            compactFunction(m_function);
    }

private:
    const UseLists& useLists()
    {
        if (!m_analyses)
            m_analyses.emplace(m_context, m_function);
        return m_analyses->useLists();
    }

    void count(StatisticID statistic) { m_context.build.statistics().count(statistic); }

    void setResultType(ValueID id, Value& value, TypeID previousType)
    {
        value.type = resultTypeOf(m_program, m_function, value, m_types);
        if (value.type != previousType)
            m_narrowedValues.append(id);
    }

    // A call that is resolved reads the closure only for what it captures, which is the object at hand. A read that nothing else uses goes, and with it the closure that it would make.
    bool tryTakeCaptureFromReceiver(ValueID id)
    {
        const Value& read = m_function.at(id);
        if (!m_analysis.findsClosureOverReceiver(read))
            return false;
        const UseLists& uses = useLists();
        ValueID receiver = read.objectOperand();
        bool hasOtherUses = false;
        for (const Use& use : uses.usesOf(id)) {
            bool isTaken = use.kind == Use::Kind::Operand
                && m_function.at(use.user).opcode == Opcode::GetCapture
                && m_program.typeOf(m_function, receiver).isSubsetOf(m_program.typeOf(m_function, use.user), m_program);
            if (!isTaken) {
                hasOtherUses = true;
                continue;
            }
            // The object at hand may be known better than what the function says it captures.
            becomeUpcast(m_function, use.user, receiver);
            Value& capture = m_function.edit(use.user);
            setResultType(use.user, capture, capture.type);
        }
        if (hasOtherUses)
            return false;
        m_function.edit(uses.blockOf(id), BlockEdit::Values).values.removeFirst(id);
        m_didDetachValues = true;
        return true;
    }

    void becomeReadOfDeferredClosure(ValueID id)
    {
        Value& read = m_function.edit(id);
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
        setResultType(id, read, previous.type);
        read.setExpected(expectedTypeOf(m_program, m_function, read, m_types));
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
        bool wasListed = m_function.edit(useLists().blockOf(id), BlockEdit::Values).values.removeFirst(id);
        RELEASE_ASSERT(wasListed);
    }

    size_t deferClosure(const BlockValues& values, const FieldInitializer& initializer, Vector<ValueID, 8>& createdValues)
    {
        leaveUnmade(m_function.edit(initializer.closure));
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
                return firstIndex;
        }
    }

    void listInitializers(const ClosureFieldFreshObject& object)
    {
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
            count(StatisticID::StoresListedByAllocations);
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
        auto listCreatedValue = [&](Value&& value) {
            createdValues.append(m_builder.addDetached(WTF::move(value)));
            listedValues.append(createdValues.last());
        };
        for (size_t index : sources) {
            const FieldInitializer& initializer = object.initializers[index];
            if (folds[index] == Fold::Placeholder) {
                listCreatedValue(*tryCreatePlaceholder(m_program, { object.layout, initializer.field }));
                continue;
            }

            bool isDeferred = m_analysis.defers(object, initializer);
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
        auto appendValues = [&](size_t from, size_t to, bool isHoisted) {
            for (ValueID id : values.subspan(from, to - from)) {
                if (m_isHoisted.quickGet(id.index()) == isHoisted && !m_isRemoved.quickGet(id.index()))
                    reordered.append(id);
            }
        };
        appendValues(begin, allocationIndex, false);
        appendValues(begin, end, true);
        reordered.appendVector(createdValues);
        reordered.append(object.allocation);
        appendValues(allocationIndex + 1, end, false);
        for (ValueID id : values.subspan(begin, end - begin)) {
            m_isInWindow.quickClear(id.index());
            m_isHoisted.quickClear(id.index());
            m_isRemoved.quickClear(id.index());
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
        leaveUnmade(m_function.edit(store.closure));
        count(StatisticID::ClosuresDeferredInFields);

        Vector<ValueID, 2> moved;
        for (ValueID id = m_function.at(store.store).valueOperand(); id != store.closure; id = m_function.at(id).operand(0))
            moved.append(id);
        moved.append(store.closure);
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
            for (ValueID id = stored; id != definition; id = m_function.at(id).operand(0))
                isRemoved.quickSet(id.index());
            isRemoved.quickSet(definition.index());
        }
        m_function.edit(entryBlock, BlockEdit::Values).values.removeAllMatching([&](ValueID id) {
            return isRemoved.quickGet(id.index());
        });
        m_didDetachValues = true;
    }

    AnalysisContext m_context;
    const Program& m_program;
    const DeferralAnalysis& m_analysis;
    Function& m_function;
    TypeArena& m_types;
    IRBuilder m_builder;
    std::optional<FunctionAnalyses> m_analyses;
    BitVector m_isInWindow;
    BitVector m_isHoisted;
    BitVector m_isRemoved;
    bool m_didDetachValues { false };
    Vector<ValueID> m_narrowedValues;
};

// Every arm of a branch makes a closure of one function over the same values, and a parameter of the block behind the arms takes the one that was made.
struct ClosureJoin {
    BlockID block;
    uint32_t parameterIndex { 0 };
    ValueID user;
    uint32_t operandIndex { 0 };
    Vector<ValueID, 2> closures;
    Vector<BlockID, 2> blocksOfClosures;
};

} // anonymous namespace

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
        ClosureFieldRewriter(AnalysisContext { build, analysis.program, nullptr, nullptr, id }, analysis, function, arenas.types).run();
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

static void findClosureJoins(const Function& function, FunctionAnalyses& analyses, BlockID block, Vector<ClosureJoin, 8>& joins)
{
    const Block& contents = function.at(block);
    std::span<const Edge> edges = analyses.cfg().incomingEdges(block);
    if (contents.isHandlerBlock() || edges.size() < 2 || std::ranges::any_of(edges, &Edge::isHandlerEdge))
        return;
    const UseLists& uses = analyses.useLists();
    for (uint32_t parameterIndex = 0; parameterIndex < contents.parameters.size(); ++parameterIndex) {
        ValueID parameter = contents.parameters[parameterIndex];
        std::span<const Use> usesOfParameter = uses.usesOf(parameter);
        if (usesOfParameter.size() != 1 || usesOfParameter[0].kind != Use::Kind::Operand || usesOfParameter[0].block != block || !operationsGivingFields.contains(function.at(usesOfParameter[0].user).opcode))
            continue;
        ClosureJoin join { .block = block, .parameterIndex = parameterIndex, .user = usesOfParameter[0].user, .operandIndex = usesOfParameter[0].operandIndex };
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

static void joinClosuresOfArms(const AnalysisContext& context, Function& function, TypeArena& types)
{
    FunctionAnalyses analyses(context, function);
    Vector<ClosureJoin, 8> joins;
    for (uint32_t index = 0; index < function.blocks.size(); ++index)
        findClosureJoins(function, analyses, BlockID(index), joins);
    if (joins.isEmpty())
        return;

    IRBuilder builder(context, function, types, &analyses);
    for (const ClosureJoin& join : joins) {
        builder.setOrigin(function.at(join.closures[0]).origin());
        ValueID closure = builder.insertBefore(ProgramPoint { join.block, 0 }, Value(function.at(join.closures[0])));
        function.edit(join.user, ValueEdit::Definition).setOperand(join.operandIndex, closure);
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

void deferClosuresInFields(ProgramEditor& editor, BuildContext& build)
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

    DeferralAnalysis analysis(program, build);
    analysis.run();
    if (analysis.numberOfDeferringFields)
        applyDeferrals(editor, build, analysis);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
