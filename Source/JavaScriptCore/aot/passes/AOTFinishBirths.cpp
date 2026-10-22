/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTIRBuilder.h"
#include "AOTLayout.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet storesBySlot { Opcode::PutSlot, Opcode::PutSlotOrAdd };
static constexpr OpcodeSet storesByName { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet finishingOperations { Opcode::FinishBirth, Opcode::TryFinishBirth };

static constexpr OpcodeSet operationsThatKeepObjectPrivate {
    Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::IsSlotFilled, Opcode::ShapeTest, Opcode::HasNoExtraProperties, Opcode::HasNoFieldOutsideSlot,
    Opcode::HasBirthFieldsFilled, Opcode::IsNotFrozen, Opcode::HasLayoutPrototype, Opcode::BelongsToInstance, Opcode::HasKeyOrder, Opcode::AllGuards,
};

static_assert(OperandIndex::PutSlot::object == OperandIndex::PutSlotOrAdd::object && OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);

namespace {

// An object is born with the mark that its fields are not all stored. Behind the store by slot that fills the last of the fields that every constructor fills, the mark is taken off
// if no other code has run since somebody else could reach the object. Otherwise the run time looks at the slots: there, before other code may see the object, and where the object leaves.
class BirthFinisher {
public:
    explicit BirthFinisher(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_cfg(context.analyses.cfg())
    {
    }

    PassOutcome run()
    {
        collectObjects();
        if (m_objects.isEmpty())
            return PassOutcome::Unchanged;

        Vector<std::optional<States>> atEnd(m_function.blocks.size());
        for (BlockID block : m_cfg.reversePostOrder()) {
            finishOnWaysThatLagBehind(block, atEnd);
            States states = statesAtHeadOf(block, atEnd);
            if (block == entryBlock && m_allocatedThis)
                states[*objectDefinedBy(m_allocatedThis)] = State { BitVector(m_keys.size()), BitVector(m_keys.size()), true, false, m_thisMayHaveBeenSeen, m_thisMayHaveBeenSeen };
            walk(block, states);
            atEnd[block.index()] = WTF::move(states);
        }
        for (ValueID test : m_testsOfUnfinishedObjects)
            m_function.edit(test, ValueEdit::Definition).setMayMeetUnfinishedObject(true);
        if (m_insertions.isEmpty()) {
            if (m_testsOfUnfinishedObjects.isEmpty())
                return PassOutcome::Unchanged;
            // A flag of a test is all that has changed.
            m_context.keptAnalyses = analysesThatReadNoTypes;
            return PassOutcome::Changed;
        }

        IRBuilder builder(m_context.analyses.context(), m_function, m_context.types, &m_context.analyses);
        for (const Insertion& insertion : m_insertions) {
            builder.setOrigin(m_function.at(insertion.object).origin());
            builder.insertBefore(insertion.point, IRBuilder::make(insertion.operation, { insertion.object }));
        }
        builder.applyInsertions();
        size_t finished = std::ranges::count(m_insertions, Opcode::FinishBirth, &Insertion::operation);
        m_context.build.statistics().count(StatisticID::BirthsFinished, finished);
        m_context.build.statistics().count(StatisticID::BirthsLeftToRunTime, m_insertions.size() - finished);
        return PassOutcome::Changed;
    }

private:
    using Layouts = Vector<LayoutID, 1>;

    struct Insertion {
        Opcode operation;
        ValueID object;
        ProgramPoint point;
    };

    struct State {
        BitVector stored;
        // Of those, what has been stored since other code could last change the object.
        BitVector storedSinceOtherCodeRan;
        bool isAlive { false };
        bool isFinished { false };
        bool isReachableByOthers { true };
        bool wasSeenByOthers { true };

        void meet(const State& other)
        {
            stored.filter(other.stored);
            storedSinceOtherCodeRan.filter(other.storedSinceOtherCodeRan);
            isAlive &= other.isAlive;
            isFinished &= other.isFinished;
            isReachableByOthers |= other.isReachableByOthers;
            wasSeenByOthers |= other.wasSeenByOthers;
        }

        void forgetWhatLoopMayUndo()
        {
            storedSinceOtherCodeRan.clearAll();
            isFinished = false;
            isReachableByOthers = true;
            wasSeenByOthers = true;
        }
    };
    using States = Vector<State, 4>;

    std::optional<uint32_t> numberOf(PropertyKeyID key) const
    {
        size_t number = key ? m_keys.find(key) : notFound;
        if (number == notFound)
            return std::nullopt;
        return static_cast<uint32_t>(number);
    }

    bool isFilled(LayoutID layout, const BitVector& stored) const
    {
        const Layout& fields = m_program.at(layout);
        for (uint32_t field = 0; field < fields.fields.size(); ++field) {
            if (!isFilledAtBirth(m_program, { layout, field }))
                continue;
            std::optional<uint32_t> number = numberOf(fields.fields[field].key);
            if (!number || !stored.quickGet(*number))
                return false;
        }
        return true;
    }

    bool isFilled(const Layouts& layouts, const BitVector& stored) const
    {
        return std::ranges::all_of(layouts, [&](LayoutID layout) {
            return isFilled(layout, stored);
        });
    }

    bool isFilled(const Value& storeBySlot, const BitVector& stored) const
    {
        return std::ranges::all_of(m_program.at(storeBySlot.receivers()).members, [&](LayoutID layout) {
            return isFilled(layout, stored);
        });
    }

    bool mayBeMarker(ValueID stored) const
    {
        return m_program.typeOf(m_function, stored).mayBeUndefinedOrNull();
    }

    State stateAtBirth(const Value& allocation) const
    {
        State state { BitVector(m_keys.size()), BitVector(m_keys.size()), true, false, false, false };
        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> values = allocation.variadicOperands();
        for (size_t index = 0; index < fields.size(); ++index) {
            if (holdsRawValueFromBirth(m_program, { allocation.layout(), fields[index] }) && (index >= values.size() || mayBeMarker(values[index])))
                continue;
            if (std::optional<uint32_t> number = numberOf(m_program.at(allocation.layout()).fields[fields[index]].key))
                state.stored.quickSet(*number);
        }
        state.storedSinceOtherCodeRan = state.stored;
        return state;
    }

    bool mayStoreMarkerInPlaceOfRawValue(const Value& storeBySlot) const
    {
        if (!mayBeMarker(storeBySlot.operand(OperandIndex::PutSlot::value)))
            return false;
        return std::ranges::any_of(m_program.at(storeBySlot.receivers()).members, [&](LayoutID layout) {
            std::optional<FieldReference> field = findField(m_program, layout, storeBySlot.key(), BaseChain::Proven);
            return field && holdsRawValueFromBirth(m_program, *field);
        });
    }

    State nothingKnown() const
    {
        return { BitVector(m_keys.size()), BitVector(m_keys.size()), false, false, true, true };
    }

    std::optional<uint32_t> objectDefinedBy(ValueID value) const
    {
        if (!m_isObject.quickGet(value.index()))
            return std::nullopt;
        return static_cast<uint32_t>(m_objects.find(value));
    }

    std::optional<uint32_t> objectNamedBy(ValueID value) const
    {
        if (!value)
            return std::nullopt;
        if (std::optional<uint32_t> object = objectDefinedBy(value))
            return object;
        return objectDefinedBy(m_context.analyses.identityOf(value));
    }

    void addObject(ValueID value, Layouts&& layouts)
    {
        m_isObject.quickSet(value.index());
        m_objects.append(value);
        m_layouts.append(WTF::move(layouts));
    }

    void numberKeysOf(LayoutID layout)
    {
        const Layout& fields = m_program.at(layout);
        for (uint32_t field = 0; field < fields.fields.size(); ++field) {
            PropertyKeyID key = fields.fields[field].key;
            if (key && isFilledAtBirth(m_program, { layout, field }) && !m_keys.contains(key))
                m_keys.append(key);
        }
    }

    // `new` of a callee that nobody knows makes the object of a constructor function and hands it over as `this`, unfinished. Whoever calls the function hands over any value.
    bool mayBeHandedObjectByNew() const
    {
        return m_function.kind == FunctionKind::Plain && m_function.role != FunctionRole::AllocatedThisVersion && !m_function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
    }

    Layouts layoutsStoredIntoThis(ValueID thisValue) const
    {
        Layouts result;
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (!storesBySlot.contains(value.opcode) || m_context.analyses.identityOf(value.operand(OperandIndex::PutSlot::object)) != thisValue)
                    continue;
                for (LayoutID layout : m_program.at(value.receivers()).members) {
                    if (hasFieldsFilledAtBirth(m_program, layout) && !result.contains(layout))
                        result.append(layout);
                }
            }
        }
        return result;
    }

    // The caller has allocated `this` and has not finished it: the version of a constructor that is handed its object, and the function that stores the initial values of a class's fields.
    Layouts layoutsOfAllocatedThis() const
    {
        bool isInitializer = m_function.kind == FunctionKind::Initializer;
        std::optional<unsigned> thisParameter = m_function.thisParameterIndex();
        if (!thisParameter)
            return { };
        if (mayBeHandedObjectByNew())
            return layoutsStoredIntoThis(m_function.at(entryBlock).parameters[*thisParameter]);
        if (m_function.role != FunctionRole::AllocatedThisVersion && !isInitializer)
            return { };
        LayoutSetID layouts = m_program.typeOf(m_function, m_function.at(entryBlock).parameters[*thisParameter]).layouts;
        // Where the check of `this` has become a test at its uses.
        if (!layouts && isInitializer && m_function.thisType.type)
            layouts = m_context.types.at(m_function.thisType.type).layouts;
        if (!layouts)
            return { };
        Layouts result;
        for (LayoutID layout : m_program.at(layouts).members) {
            if (hasFieldsFilledAtBirth(m_program, layout))
                result.append(layout);
        }
        return result;
    }

    // The constructor of the parent class has run in front of the initializer of a derived class. Every layout of `this` is the class of the initializer or lies below it,
    // so one without a parent is that class.
    bool initializerMayMeetObjectSeenByOthers() const
    {
        LayoutSetID layouts = m_function.thisType.type ? m_context.types.at(m_function.thisType.type).layouts : LayoutSetID();
        return !layouts || std::ranges::none_of(m_program.at(layouts).members, [&](LayoutID layout) {
            return !m_program.at(declaredLayoutOf(m_program, layout)).has(LayoutFlags::HasHeritage);
        });
    }

    void collectObjects()
    {
        m_isObject = BitVector(m_function.values.size());
        Layouts layoutsOfThis = layoutsOfAllocatedThis();
        bool hasSuchAllocation = !layoutsOfThis.isEmpty();
        for (LayoutID layout : layoutsOfThis)
            numberKeysOf(layout);
        forEachAllocationWithSuchFields([&](ValueID, const Value& allocation) {
            numberKeysOf(allocation.layout());
            hasSuchAllocation = true;
        });
        if (!hasSuchAllocation)
            return;
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (!storesBySlot.contains(m_function.at(id).opcode))
                    continue;
                for (LayoutID layout : m_program.at(m_function.at(id).receivers()).members)
                    numberKeysOf(layout);
            }
        }
        if (!layoutsOfThis.isEmpty()) {
            m_allocatedThis = m_function.at(entryBlock).parameters[*m_function.thisParameterIndex()];
            m_thisMayHaveBeenSeen = mayBeHandedObjectByNew() || (m_function.kind == FunctionKind::Initializer && initializerMayMeetObjectSeenByOthers());
            addObject(m_allocatedThis, WTF::move(layoutsOfThis));
        }
        forEachAllocationWithSuchFields([&](ValueID id, const Value& allocation) {
            if (!isFilled(allocation.layout(), stateAtBirth(allocation).stored))
                addObject(id, { allocation.layout() });
        });
        if (m_objects.isEmpty())
            return;
        bool hasFoundParameter = true;
        while (hasFoundParameter) {
            hasFoundParameter = false;
            for (BlockID block : m_cfg.reversePostOrder()) {
                const Block& target = m_function.at(block);
                for (uint32_t index = 0; index < target.parameters.size(); ++index) {
                    if (objectNamedBy(target.parameters[index]))
                        continue;
                    Layouts layouts = layoutsArrivingAt(block, index);
                    if (!layouts.isEmpty()) {
                        addObject(target.parameters[index], WTF::move(layouts));
                        hasFoundParameter = true;
                    }
                }
            }
        }
    }

    void forEachAllocationWithSuchFields(NOESCAPE const Invocable<void(ValueID, const Value&)> auto& functor) const
    {
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (value.opcode == Opcode::NewObject && hasFieldsFilledAtBirth(m_program, value.layout()))
                    functor(id, value);
            }
        }
    }

    Layouts layoutsArrivingAt(BlockID block, uint32_t parameterIndex) const
    {
        Layouts layouts;
        for (BlockID predecessor : m_cfg.predecessors(block)) {
            const Block& from = m_function.at(predecessor);
            if (from.handler && from.handler->block == block)
                return { };
            for (const Target& target : from.terminator.targets) {
                if (target.block != block)
                    continue;
                std::optional<uint32_t> object = parameterIndex < target.arguments.size() ? objectNamedBy(target.arguments[parameterIndex]) : std::nullopt;
                if (!object)
                    return { };
                for (LayoutID layout : m_layouts[*object]) {
                    if (!layouts.contains(layout))
                        layouts.append(layout);
                }
            }
        }
        return layouts;
    }

    void forEachArrival(BlockID block, NOESCAPE const Invocable<void(BlockID predecessor, uint32_t objectInBlock, uint32_t objectInPredecessor)> auto& functor) const
    {
        const Block& target = m_function.at(block);
        for (BlockID predecessor : m_cfg.predecessors(block)) {
            for (uint32_t object = 0; object < m_objects.size(); ++object) {
                if (!target.parameters.contains(m_objects[object]))
                    functor(predecessor, object, object);
            }
            for (const Target& edge : m_function.at(predecessor).terminator.targets) {
                if (edge.block != block)
                    continue;
                for (uint32_t index = 0; index < target.parameters.size(); ++index) {
                    if (std::optional<uint32_t> parameter = objectDefinedBy(target.parameters[index]))
                        functor(predecessor, *parameter, *objectNamedBy(edge.arguments[index]));
                }
            }
        }
    }

    void finishOnWaysThatLagBehind(BlockID block, Vector<std::optional<States>>& atEnd)
    {
        BitVector isFinishedOnSomeWay(m_objects.size());
        forEachArrival(block, [&](BlockID predecessor, uint32_t object, uint32_t arriving) {
            if (atEnd[predecessor.index()] && (*atEnd[predecessor.index()])[arriving].isFinished)
                isFinishedOnSomeWay.quickSet(object);
        });
        forEachArrival(block, [&](BlockID predecessor, uint32_t object, uint32_t arriving) {
            if (!isFinishedOnSomeWay.quickGet(object) || !atEnd[predecessor.index()] || m_function.at(predecessor).terminator.targets.size() != 1)
                return;
            State& state = (*atEnd[predecessor.index()])[arriving];
            if (state.isFinished || !state.isAlive || !isKnownToBeObject(m_objects[arriving]))
                return;
            m_insertions.append({ Opcode::TryFinishBirth, m_objects[arriving], pointBeforeTerminatorOf(m_function, predecessor) });
            state.isFinished = true;
        });
    }

    bool isDefinedAtHeadOf(BlockID block, uint32_t object) const
    {
        return m_context.analyses.dominates(m_objects[object], ProgramPoint::atHeadOf(block));
    }

    States statesAtHeadOf(BlockID block, const Vector<std::optional<States>>& atEnd) const
    {
        States states(m_objects.size(), [&](size_t) {
            return nothingKnown();
        });
        BitVector hasState(m_objects.size());
        auto meet = [&](uint32_t object, const State& arriving) {
            if (hasState.quickSet(object))
                states[object].meet(arriving);
            else
                states[object] = arriving;
        };
        BitVector comesAroundLoop(m_objects.size());
        forEachArrival(block, [&](BlockID predecessor, uint32_t object, uint32_t arriving) {
            if (const std::optional<States>& before = atEnd[predecessor.index()])
                meet(object, (*before)[arriving]);
            else
                comesAroundLoop.quickSet(object);
        });
        for (uint32_t object = 0; object < m_objects.size(); ++object) {
            if (!isDefinedAtHeadOf(block, object) || (comesAroundLoop.quickGet(object) && m_function.at(block).parameters.contains(m_objects[object])))
                states[object] = nothingKnown();
            else if (comesAroundLoop.quickGet(object))
                states[object].forgetWhatLoopMayUndo();
        }
        return states;
    }

    // `this` of an initializer whose check has become a test at its uses is any value where the test has failed.
    bool isKnownToBeObject(ValueID value) const
    {
        return m_program.typeOf(m_function, value).isSubsetOf(m_context.types.at(TypeTable::object), m_program);
    }

    bool keepsObjectPrivate(const Value& value, const Effects& effects) const
    {
        if (operationsThatKeepObjectPrivate.contains(value.opcode) || isCastWithoutTest(value.opcode))
            return true;
        return effects.writes.containsOnly(bookkeepingHeaps) && !effects.mayRunOtherCode() && !m_program.at(value.type).hasObjects();
    }

    // The mark stands for a field outside its slot too, and for the fields that the constructors of the parents fill.
    bool provesNoFieldOutsideSlot(const Value& storeBySlot) const
    {
        ValueID guard = storeBySlot.guardOperand();
        bool hasParent = std::ranges::any_of(m_program.at(storeBySlot.receivers()).members, [&](LayoutID layout) {
            return m_program.at(declaredLayoutOf(m_program, layout)).has(LayoutFlags::HasHeritage);
        });
        if (!guard || hasParent)
            return false;
        ValueID object = m_context.analyses.identityOf(storeBySlot.operand(OperandIndex::PutSlot::object));
        const GuardFacts& facts = m_context.analyses.guardFacts(guard);
        return facts.find(Proof::NoFieldOutsideSlot, object) || facts.find(Proof::BirthFieldsFilled, object);
    }

    void walk(BlockID id, States& states)
    {
        const Block& block = m_function.at(id);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            const Value& value = m_function.at(block.values[index]);
            if (std::optional<uint32_t> born = objectDefinedBy(block.values[index]))
                states[*born] = stateAtBirth(value);
            Effects effects = m_context.analyses.effectsOf(block.values[index]).onAnyEdge();
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                std::optional<uint32_t> object = objectNamedBy(operand);
                if (!object)
                    return;
                State& state = states[*object];
                if (finishingOperations.contains(value.opcode)) {
                    state.isFinished = true;
                    return;
                }
                if (value.opcode == Opcode::HasNoFieldOutsideSlot && !state.isFinished)
                    m_testsOfUnfinishedObjects.append(block.values[index]);
                bool isStoredSomewhere = storesBySlot.contains(value.opcode) && operandIndex == OperandIndex::PutSlot::value;
                if (isStoredSomewhere || !keepsObjectPrivate(value, effects))
                    state.isReachableByOthers = true;
            });
            if (value.opcode == Opcode::CallDirect && value.isConstruction()) {
                value.forEachOperand([&](ValueID operand, uint32_t) {
                    std::optional<uint32_t> object = objectNamedBy(operand);
                    if (!object || !states[*object].isAlive || states[*object].isFinished || !isKnownToBeObject(operand))
                        return;
                    m_insertions.append({ Opcode::TryFinishBirth, operand, { id, index + 1 } });
                    states[*object].isFinished = true;
                });
            }
            if (effects.mayRunOtherCode() || effects.writes.contains(AbstractHeap::NamedProperties)) {
                // The initializer stores the first fields, and finishes the object itself where it stores the last.
                bool callsInitializer = value.opcode == Opcode::CallDirect && m_program.at(value.function()).kind == FunctionKind::Initializer;
                for (uint32_t object = 0; object < states.size(); ++object) {
                    State& state = states[object];
                    if (state.isReachableByOthers)
                        state.storedSinceOtherCodeRan.clearAll();
                    if (!state.isReachableByOthers || state.wasSeenByOthers)
                        continue;
                    state.wasSeenByOthers = true;
                    if (state.isAlive && !state.isFinished && !callsInitializer && isKnownToBeObject(m_objects[object]))
                        m_insertions.append({ Opcode::TryFinishBirth, m_objects[object], { id, index } });
                }
            }
            // Code that runs seldom stores by name. Whether that has filled the slot the run time sees, behind the store that may have filled the last one.
            if (storesByName.contains(value.opcode)) {
                std::optional<uint32_t> object = objectNamedBy(value.objectOperand());
                std::optional<uint32_t> number = numberOf(value.key());
                if (!object || !number)
                    continue;
                State& state = states[*object];
                state.stored.quickSet(*number);
                state.isReachableByOthers = true;
                state.wasSeenByOthers = true;
                if (state.isFinished || !state.isAlive || !isFilled(m_layouts[*object], state.stored) || !isKnownToBeObject(value.objectOperand()))
                    continue;
                m_insertions.append({ Opcode::TryFinishBirth, value.objectOperand(), { id, index + 1 } });
                state.isFinished = true;
                continue;
            }
            if (!storesBySlot.contains(value.opcode))
                continue;
            ValueID receiver = value.operand(OperandIndex::PutSlot::object);
            std::optional<uint32_t> object = objectNamedBy(receiver);
            std::optional<uint32_t> number = numberOf(value.key());
            if (!object || !number)
                continue;
            State& state = states[*object];
            if (mayStoreMarkerInPlaceOfRawValue(value)) {
                state.stored.quickClear(*number);
                state.storedSinceOtherCodeRan.quickClear(*number);
                state.isFinished = false;
                continue;
            }
            state.stored.quickSet(*number);
            state.storedSinceOtherCodeRan.quickSet(*number);
            if (state.isFinished || !state.isAlive || !isFilled(value, state.stored))
                continue;
            // Whatever others have done to the object before: every field of birth has been stored since, and none lies outside its slot.
            bool needsNoLook = !state.wasSeenByOthers || (isFilled(value, state.storedSinceOtherCodeRan) && provesNoFieldOutsideSlot(value));
            m_insertions.append({ needsNoLook ? Opcode::FinishBirth : Opcode::TryFinishBirth, receiver, { id, index + 1 } });
            state.isFinished = true;
        }

        auto letRunTimeLook = [&](ValueID leaving) {
            std::optional<uint32_t> object = objectNamedBy(leaving);
            if (!object || states[*object].isFinished || !isKnownToBeObject(leaving))
                return;
            m_insertions.append({ Opcode::TryFinishBirth, leaving, pointBeforeTerminatorOf(m_function, id) });
            states[*object].isFinished = true;
        };
        block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            letRunTimeLook(operand);
        });
        if (block.terminator.targets.size() != 1)
            return;
        const Target& next = block.terminator.targets[0];
        for (uint32_t index = 0; index < next.arguments.size(); ++index) {
            if (!objectNamedBy(m_function.at(next.block).parameters[index]))
                letRunTimeLook(next.arguments[index]);
        }
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    const CFG& m_cfg;
    BitVector m_isObject;
    Vector<ValueID, 4> m_objects;
    Vector<Layouts, 4> m_layouts;
    Vector<PropertyKeyID, 16> m_keys;
    Vector<Insertion, 4> m_insertions;
    Vector<ValueID, 4> m_testsOfUnfinishedObjects;
    ValueID m_allocatedThis;
    bool m_thisMayHaveBeenSeen { false };
};

} // anonymous namespace

PassOutcome finishBirths(FunctionPassContext& context)
{
    return BirthFinisher(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
