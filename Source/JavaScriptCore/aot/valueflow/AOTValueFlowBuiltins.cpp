/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBuiltins.h"

#if ENABLE(AOT)

#include "AOTTypeRelations.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueFlowRulesModules.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr ValueFlowName elements = ValueFlowName::of(ValueFlowNameKind::Elements);
constexpr ValueFlowName unknownKey = ValueFlowName::of(ValueFlowNameKind::UnknownKey);
constexpr ValueFlowName everyProperty = ValueFlowName::of(ValueFlowNameKind::EveryProperty);
constexpr ValueFlowName everyGetter = ValueFlowName::of(ValueFlowNameKind::EveryGetter);
constexpr ValueFlowName prototypeLink = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
constexpr ValueFlowName iteratedValues = ValueFlowName::of(ValueFlowNameKind::IteratedValues);
constexpr ValueFlowName awaitedValue = ValueFlowName::of(ValueFlowNameKind::AwaitedValue);

constexpr ValueFlowOperand operandHolding(ValueSetID constant)
{
    return { { }, constant, ValueSets::anything };
}

constexpr ValueFlowOperand operandAt(ValueFlowNodeID node, ValueSetID mask = ValueSets::anything)
{
    return { node, ValueSets::nothing, mask };
}

ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
}

ValueFlowName property(ValueFlowRuleContext& context, BuiltinName name)
{
    return ValueFlowName::property(context.program().keyOf(name));
}

ValueSetID everythingButUnknownObject(ValueFlowRuleContext& context)
{
    return ValueSets::complement(context.universe().unknownObjectAlone());
}

bool mayReturnUnknownObject(const BuiltinType& type)
{
    if (type.isAnything() || type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }))
        return true;
    return type.freshness == BuiltinFreshness::No && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind);
}

BuiltinCallMode modeOf(ValueFlowCallKind kind)
{
    switch (kind) {
    case ValueFlowCallKind::Call:
    case ValueFlowCallKind::CallByNativeCode:
        return BuiltinCallMode::Call;
    case ValueFlowCallKind::Construct:
    case ValueFlowCallKind::ConstructByNativeCode:
        return BuiltinCallMode::Construct;
    case ValueFlowCallKind::Get:
        return BuiltinCallMode::Get;
    case ValueFlowCallKind::Set:
        return BuiltinCallMode::Set;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinUse useOf(ValueFlowCallKind kind)
{
    switch (kind) {
    case ValueFlowCallKind::Call:
    case ValueFlowCallKind::CallByNativeCode:
        return BuiltinUse::Called;
    case ValueFlowCallKind::Construct:
    case ValueFlowCallKind::ConstructByNativeCode:
        return BuiltinUse::Constructed;
    case ValueFlowCallKind::Get:
        return BuiltinUse::Read;
    case ValueFlowCallKind::Set:
        return BuiltinUse::Written;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A call that native code makes on its own.
struct CallByNativeCode {
    ClimbingSet callees;
    ValueFlowOperand thisValue;
    // Every argument holds this.
    ValueSetID arguments { ValueSets::nothing };
    // None where native code drops the result.
    ValueFlowNodeID result;
};

void callFromNativeCode(ValueFlowRuleContext& context, const CallByNativeCode& call)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!context.meet(call.callees, universe.callables()))
        return;

    ValueFlowCallGroup group = context.group(context.restrict(call.callees, universe.callables()), ValueFlowCallKind::CallByNativeCode);
    context.connect(call.thisValue, group.thisValue);
    context.add(group.otherArguments, call.arguments);
    // The call that has handed the object to native code throws the unknown value.
    context.connect(group.thrown, ValueFlowProgramNodes::escaped());
    if (call.result)
        context.connect(group.result, call.result);
}

class BuiltinApplication {
    WTF_MAKE_NONCOPYABLE(BuiltinApplication);
public:
    BuiltinApplication(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
        : m_builtin(builtin)
        , m_call(call)
        , m_context(context)
        , m_program(context.program())
        , m_builtins(m_program.builtins())
        , m_universe(context.universe())
        , m_tables(m_universe.builtinTables())
        , m_undefined(operandHolding(m_universe.undefinedValue()))
    {
    }

    void run()
    {
        m_context.use(m_builtin, useOf(m_call.kind));

        std::optional<BuiltinDispatchTarget> root = m_builtins.dispatchOf(m_builtin, modeOf(m_call.kind));
        if (!root) {
            throwValue(m_universe.unknownObjectAlone());
            return;
        }

        Vector<BuiltinOverloadID, 4> selected;
        select(*root, selected);
        for (BuiltinOverloadID overload : selected)
            apply(m_builtins.at(overload));
    }

private:
    static constexpr ValueFlowNodeID escaped() { return ValueFlowProgramNodes::escaped(); }

    void throwValue(ValueSetID value)
    {
        if (m_call.thrown)
            m_context.add(m_call.thrown, value);
    }

    void answer(ValueSetID value)
    {
        if (m_call.result)
            m_context.add(m_call.result, value);
    }

    static bool isPassed(const ValueFlowOperand& operand) { return operand.node || (operand.constant && operand.constant != ValueSets::nothing); }

    // An argument that is not passed reads as undefined.
    void forEachOperandAt(BuiltinOperand position, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor) const
    {
        auto hand = [&](const ValueFlowOperand& operand) {
            if (isPassed(operand))
                functor(operand);
        };

        switch (position.kind()) {
        case BuiltinOperand::Kind::Receiver:
            // A call without a receiver passes undefined as this.
            functor(isPassed(m_call.thisValue) ? m_call.thisValue : m_undefined);
            return;
        case BuiltinOperand::Kind::Argument:
            if (position.argumentIndex() < m_call.arguments.size()) {
                hand(m_call.arguments[position.argumentIndex()]);
                if (!m_call.argumentsMayBeAbsent)
                    return;
            }
            hand(m_call.otherArguments);
            functor(m_undefined);
            return;
        case BuiltinOperand::Kind::ArgumentsFrom:
            for (size_t index = position.argumentIndex(); index < m_call.arguments.size(); ++index)
                hand(m_call.arguments[index]);
            hand(m_call.otherArguments);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ClimbingSet valueAt(BuiltinOperand position)
    {
        ClimbingSet value;
        forEachOperandAt(position, [&](const ValueFlowOperand& operand) {
            value = m_context.unite(value, m_context.valueOf(operand));
        });
        return value;
    }

    ClimbingSet objectsAt(BuiltinOperand position) { return m_context.restrict(valueAt(position), m_universe.objects()); }
    ClimbingSet known(ClimbingSet objects) { return m_context.restrict(objects, m_universe.knownObjects()); }
    ClimbingSet unknownObjectAlone() const { return ClimbingSet(m_universe.unknownObjectAlone()); }
    bool mayBeUnknownObject(ClimbingSet objects) const { return m_context.contains(objects, ValueFlowUniverse::unknownObject()); }

    // What is stored through the unknown object lies in one node for each name, and it escapes.
    void forEachNodeWrittenThrough(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        functor(m_context.written(known(objects), name));
        if (!mayBeUnknownObject(objects))
            return;
        functor(m_context.written(unknownObjectAlone(), name));
        functor(escaped());
    }

    // An object that has escaped may hold what was stored through the unknown object.
    void forEachNodeReadThrough(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        functor(m_context.read(known(objects), name));
        if (m_context.meet(m_context.valueWithin(objects, escaped()), ValueSets::anything))
            functor(m_context.read(unknownObjectAlone(), name));
    }

    void connectOperands(BuiltinOperand position, ValueFlowNodeID target)
    {
        forEachOperandAt(position, [&](const ValueFlowOperand& operand) {
            m_context.connect(operand, target);
        });
    }

    // Whether the operand may hold a member of the mask. `admitsAbsence` says the same of an argument that is not passed.
    bool mayLieIn(BuiltinOperand position, ValueSetID mask, bool admitsAbsence)
    {
        auto mayHold = [&](const ValueFlowOperand& operand) {
            return m_context.meet(m_context.valueOf(operand), mask);
        };

        switch (position.kind()) {
        case BuiltinOperand::Kind::Receiver:
            // A call without a receiver passes undefined as this.
            return mayHold(isPassed(m_call.thisValue) ? m_call.thisValue : m_undefined);
        case BuiltinOperand::Kind::Argument: {
            unsigned index = position.argumentIndex();
            if (index < m_call.arguments.size() && (mayHold(m_call.arguments[index]) || !m_call.argumentsMayBeAbsent))
                return mayHold(m_call.arguments[index]);
            return admitsAbsence || mayHold(m_call.otherArguments);
        }
        case BuiltinOperand::Kind::ArgumentsFrom:
            // The branch is taken when every argument that is passed lies in the tested type.
            return m_call.argumentsMayBeAbsent || std::ranges::all_of(m_call.arguments.subspan(std::min<size_t>(position.argumentIndex(), m_call.arguments.size())), mayHold);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The tests of a node run one after the other. What is sure to pass one never comes to the next.
    // An operand that holds nothing yet lets no overload in. The statement runs again when the operand has moved.
    void select(BuiltinDispatchTarget target, Vector<BuiltinOverloadID, 4>& selected)
    {
        if (target.isOverload()) {
            selected.appendIfNotContains(target.overload);
            return;
        }

        const BuiltinDispatchNode& node = m_builtins.at(target.node);
        bool isRest = node.operand.kind() == BuiltinOperand::Kind::ArgumentsFrom;
        ValueSetID stillTested = ValueSets::anything;
        bool absenceIsStillTested = true;
        for (const BuiltinDispatchBranch& branch : node.branches) {
            bool admitsAbsence = m_builtins.at(branch.tested).isAnything() || m_builtins.at(branch.tested).primitives.contains(BuiltinPrimitive::NotPassed);
            if (mayLieIn(node.operand, m_context.maker().intersect(m_tables.maskOf(branch.tested), stillTested), admitsAbsence && absenceIsStillTested))
                select(branch.target, selected);
            if (isRest)
                continue;
            stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(m_tables.membersSurelyOf(branch.tested)));
            absenceIsStillTested &= !admitsAbsence;

            // An instance passes a test for pristine instances until something takes the behavior away that it was made with.
            ValueSetID whilePristine = m_tables.membersOfWhilePristine(branch.tested);
            if (whilePristine == ValueSets::nothing)
                continue;
            constexpr OptionSet<ObjectMutation> mutationsOfBuiltinBehavior { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
            ClimbingSet changed = m_context.markedAmong(m_context.restrict(valueAt(node.operand), whilePristine), mutationsOfBuiltinBehavior);
            if (!m_context.meet(changed, ValueSets::anything))
                stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(whilePristine));
        }
    }

    void apply(const BuiltinOverload& overload)
    {
        if (RuntimeFunctionID function = functionThatSaysWhatCallDoes(overload)) {
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_program, function)) {
                enter(entry->function);
                return;
            }
        }

        m_overload = &overload;
        m_resultObject = mayAnswerCallableObject(overload) ? ValueFlowObjectID() : m_call.resultObject;
        m_objectsMadeBySpecies = { };
        if (overload.receiver)
            applyParameter(*overload.receiver);
        else if (overload.isUndeclared() && !isConstruction(m_call.kind))
            handToUndescribedNativeCode(BuiltinOperand::receiver());
        for (const BuiltinParameter& parameter : overload.parameters)
            applyParameter(parameter);
        for (const BuiltinTransfer& transfer : overload.flow.transfers)
            connect(transfer.from, transfer.to);
        for (const BuiltinCallback& callback : overload.flow.callbacks)
            applyCallback(callback);
        for (const BuiltinHandlerCall& handlerCall : overload.flow.handlerCalls)
            applyHandlerCall(handlerCall);
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
            applyPropertyAction(action);
        if (m_call.result)
            m_context.add(m_call.result, valueOfType(overload.result.type, m_resultObject));

        if (overload.effects.capability == BuiltinCapability::JS)
            throwValue(unknownValue(m_context));
        else if (overload.throws())
            throwValue(m_universe.unknownObjectAlone());

        // The engine's own native code and the helpers of the compiler call no function of the host's library. What hangs on the promise that a call of the host may answer comes in with the built-in.
        const Builtin& entry = m_builtins.at(m_builtin);
        if (overload.isUndeclared() && entry.origin == BuiltinOrigin::Host && !entry.flags.contains(BuiltinFlag::IsCompilerHelper))
            letUndeclaredNativeCodeRun(m_context);
    }

    // Whoever calls an object that a built-in has made finds a bound function in it or nothing. Any other function that native code makes, and a proxy, is the unknown object.
    bool isCalledInWaysNobodyFollows(const BuiltinTypePart& part) const
    {
        if (part.kind != BuiltinTypePartKind::Instance || part.builtinClass == BuiltinClasses::BoundFunction)
            return false;
        return isFunctionClass(m_builtins, part.builtinClass);
    }

    bool mayAnswerCallableObject(const BuiltinOverload& overload) const
    {
        return std::ranges::any_of(m_builtins.at(overload.result.type).parts, [&](const BuiltinTypePart& part) {
            return isCalledInWaysNobodyFollows(part);
        });
    }

    // The records of a declared overload say what a call does with the operands of that call. The function of the library that runs is one for all calls: through it every array that is
    // mapped anywhere would meet every callback. Its body is followed only where the records say nothing about an operand or about the result.
    RuntimeFunctionID functionThatSaysWhatCallDoes(const BuiltinOverload& overload) const
    {
        // What a construction makes is made at the call, with the prototype that new.target says. A model makes one object for all calls.
        if (overload.code.model && !isConstruction(m_call.kind))
            return overload.code.model;
        auto claimsEveryUse = [](const BuiltinParameter& parameter) {
            return parameter.uses == OperandUses::all();
        };
        bool isDescribed = !overload.isUndeclared() && !m_builtins.at(overload.result.type).isAnything()
            && !(overload.receiver && claimsEveryUse(*overload.receiver)) && std::ranges::none_of(overload.parameters, claimsEveryUse);
        return isDescribed ? RuntimeFunctionID() : overload.libraryFunction();
    }

    void enter(FunctionID function)
    {
        ValueFlowCallGroup group = m_context.group(only(m_context, m_universe.closureOf(function)), m_call.kind);
        m_context.connect(m_call.thisValue, isConstruction(m_call.kind) ? group.newTarget : group.thisValue);
        for (unsigned index = 0; index < m_call.arguments.size(); ++index)
            m_context.connect(m_call.arguments[index], index < numberOfGroupedArguments ? group.argument(index) : group.otherArguments);
        m_context.connect(m_call.otherArguments, group.otherArguments);
        passNoFurtherArguments(m_context, group, m_call.argumentsMayBeAbsent || m_call.otherArguments.node ? 0 : m_call.arguments.size());
        if (m_call.result)
            m_context.connect(group.result, m_call.result);
        if (m_call.thrown)
            m_context.connect(group.thrown, m_call.thrown);
    }

    void handToUndescribedNativeCode(BuiltinOperand operand)
    {
        // Native code reaches the holder of its own method without being handed it.
        ValueSetID handed = operand.isReceiver() ? ValueSets::complement(m_universe.builtinObjects()) : ValueSets::anything;
        forEachOperandAt(operand, [&](ValueFlowOperand passed) {
            passed.mask = passed.mask ? m_context.maker().intersect(passed.mask, handed) : handed;
            m_context.connect(passed, escaped());
        });
        if (m_context.meet(valueAt(operand), m_context.maker().intersect(m_universe.objects(), handed)))
            openEscapedObjects(m_context);
        answer(unknownValue(m_context));
        throwValue(unknownValue(m_context));
    }

    void applyParameter(const BuiltinParameter& parameter)
    {
        if (parameter.uses == OperandUses::all()) {
            handToUndescribedNativeCode(parameter.operand);
            return;
        }

        if (parameter.uses.containsAny({ OperandUse::WritesContents, OperandUse::MovesElements }))
            m_context.mark(objectsAt(parameter.operand), ObjectMutation::StoreAfterCreation);

        if (ValueFlowBehaviorID behavior = m_tables.behaviorOf(parameter)) {
            ValueFlowNodeID hub = m_context.hub(behavior);
            // An object of the operand is among the receivers only where something that the operand holds leads back to it.
            if (parameter.uses.contains(OperandUse::AppliesToContents) && !parameter.uses.contains(OperandUse::AppliesTransitively)) {
                forEachNodeReadThrough(objectsAt(parameter.operand), everyProperty, [&](ValueFlowNodeID contents) {
                    m_context.connect(contents, hub);
                });
            } else
                connectOperands(parameter.operand, hub);
        }
        if (parameter.protocols.contains(Protocol::Species))
            constructSpecies(parameter.operand);

        // The built-in may return what a member of the operand holds, and an unknown result stands only for what has escaped.
        if (parameter.protocols.containsAny({ Protocol::Named, Protocol::CallsNamed }) && mayReturnUnknownObject(m_builtins.at(m_overload->result.type))) {
            ClimbingSet holders = valueAt(parameter.operand);
            for (BuiltinName name : parameter.namedMembers)
                m_context.connect(m_context.lookup(holders, property(m_context, name)), escaped());
        }
    }

    // The getter of a built-in constructor under @@species answers its receiver, so a class of the program that extends one is its own species.
    void constructSpecies(BuiltinOperand operand)
    {
        ClimbingSet constructors = m_context.valueOf(m_context.lookup(valueAt(operand), property(m_context, BuiltinNames::constructor)));
        ClimbingSet species = m_context.unite(constructors, m_context.valueOf(m_context.lookup(constructors, property(m_context, BuiltinNames::speciesSymbol))));
        bool isReturned = m_builtins.at(m_overload->result.type).freshness == BuiltinFreshness::UnlessSpecies;

        // A built-in constructor as the species makes what the entry says.
        ValueSetID followed = m_context.maker().unite(m_universe.closures(), m_universe.otherCallables());
        if (m_context.meet(species, followed)) {
            ClimbingSet callees = m_context.restrict(species, followed);
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::ConstructByNativeCode);
            m_context.add(group.newTarget, callees);
            m_context.add(group.otherArguments, unknownValue(m_context));
            if (m_call.thrown)
                m_context.connect(group.thrown, m_call.thrown);
            if (isReturned) {
                if (m_call.result)
                    m_context.connect(group.result, m_call.result);
                m_objectsMadeBySpecies = m_context.unite(m_objectsMadeBySpecies, m_context.restrict(m_context.valueOf(group.result), m_universe.objects()));
            } else
                m_context.connect(group.result, escaped());
        }
        if (isReturned && m_context.contains(species, ValueFlowUniverse::unknownObject())) {
            answer(m_universe.unknownObjectAlone());
            m_objectsMadeBySpecies = m_context.unite(m_objectsMadeBySpecies, unknownObjectAlone());
        }
        m_context.mark(m_objectsMadeBySpecies, { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });

        // Native code fills what the species constructor has made. Where no record says with what, anything may lie in it.
        auto goesIntoResult = [](const BuiltinTransfer& transfer) {
            return transfer.to.kind == BuiltinPlaceKind::ResultSlot || transfer.to.kind == BuiltinPlaceKind::ResultField;
        };
        ClimbingSet made = known(m_objectsMadeBySpecies);
        if (std::ranges::none_of(m_overload->flow.transfers, goesIntoResult) && m_context.meet(made, m_universe.objects())) {
            m_context.add(escaped(), made);
            letAnythingHappenTo(m_context, made);
        }
    }

    // An element of an array is an indexed property, which the program also reads and writes by itself.
    ValueFlowName nameOf(BuiltinSlotID slot) const
    {
        AbstractHeap heap = m_builtins.at(slot).heap;
        if (heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements)
            return elements;
        return { ValueFlowNameKind::Slot, slot.index() };
    }

    // Memory of native code that the program reaches only through the entries that name the cell. It has no holder, so the global object stands in.
    ValueFlowName nameOfNativeCell(BuiltinName cell) const { return { ValueFlowNameKind::Slot, m_builtins.slotCount() + cell.index() }; }
    ClimbingSet holderOfNativeCells() { return only(m_context, m_universe.objectOf(Builtins::GlobalThis)); }

    ValueFlowName nameOfResultField(unsigned fieldIndex)
    {
        for (const BuiltinTypePart& part : m_builtins.at(m_overload->result.type).parts) {
            if (part.kind != BuiltinTypePartKind::Layout)
                continue;
            const BuiltinLayout& layout = m_builtins.at(part.layout);
            if (layout.isTuple)
                return elements;
            if (fieldIndex < layout.fields.size())
                return property(m_context, layout.fields[fieldIndex].name);
        }
        return unknownKey;
    }

    OperandUses usesOf(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return m_overload->receiver ? m_overload->receiver->uses : OperandUses();
        OperandUses uses;
        for (const BuiltinParameter& parameter : m_overload->parameters) {
            if (parameter.operand == operand || (!parameter.operand.isReceiver() && parameter.operand.coversArgument(operand.argumentIndex())))
                uses.add(parameter.uses);
        }
        return uses;
    }

    void forEachNodeHoldingOwnValuesOf(ClimbingSet holders, ValueSetID& constant, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        // The table knows what a built-in object holds, and the lookup reads the table. It also looks at the objects above, which no own value lies on.
        if (m_context.meet(holders, m_universe.builtinObjects()))
            functor(m_context.lookup(m_context.restrict(holders, m_universe.builtinObjects()), everyProperty));
        if (m_context.meet(holders, m_universe.strings()))
            constant = m_context.maker().unite(constant, m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::String)));

        ClimbingSet objects = m_context.restrict(holders, m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_universe.builtinObjects())));
        forEachNodeReadThrough(objects, everyProperty, functor);
        if (mayBeUnknownObject(objects))
            constant = m_context.maker().unite(constant, unknownValue(m_context));
        ClimbingSet getters;
        forEachNodeReadThrough(objects, everyGetter, [&](ValueFlowNodeID node) {
            getters = m_context.unite(getters, m_context.valueOf(node));
        });
        if (!m_context.meet(getters, m_universe.callables()))
            return;
        ValueFlowCallGroup group = m_context.group(m_context.restrict(getters, m_universe.callables()), ValueFlowCallKind::Get);
        m_context.add(group.thisValue, objects);
        connectThrown(group, BuiltinCallbackTimes::Once);
        functor(group.result);
    }

    // The nodes that hold what lies at the place. What no node holds is added to the constant.
    void forEachNodeAt(const BuiltinPlace& place, ValueSetID& constant, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        auto readThrough = [&](ClimbingSet objects, ValueFlowName name) {
            forEachNodeReadThrough(objects, name, functor);
            if (mayBeUnknownObject(objects))
                constant = m_context.maker().unite(constant, unknownValue(m_context));
        };

        switch (place.kind) {
        case BuiltinPlaceKind::None:
        case BuiltinPlaceKind::Operand:
            return;
        case BuiltinPlaceKind::Slot:
            readThrough(objectsAt(*place.operand), nameOf(place.slot));
            return;
        case BuiltinPlaceKind::Prototype:
            // No node holds the prototype that an object is created with. The lookup answers the objects themselves and all that are above them.
            functor(m_context.lookup(objectsAt(*place.operand), prototypeLink));
            return;
        case BuiltinPlaceKind::IteratedValues:
            functor(m_context.lookup(valueAt(*place.operand), iteratedValues));
            return;
        case BuiltinPlaceKind::OwnValues: {
            // Where what is done to the operand is done to what it holds as well, the own values of its own values lie at the place too, and so on down.
            bool goesOnDown = usesOf(*place.operand).contains(OperandUse::AppliesTransitively);
            ValueSetID objectsOfProgram = m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
            ClimbingSet holders = valueAt(*place.operand);
            ClimbingSet visited = holders;
            for (bool findsMore = true; findsMore;) {
                ClimbingSet found;
                forEachNodeHoldingOwnValuesOf(holders, constant, [&](ValueFlowNodeID node) {
                    functor(node);
                    if (goesOnDown)
                        found = m_context.unite(found, m_context.restrict(m_context.valueOf(node), objectsOfProgram));
                });
                findsMore = false;
                m_context.forEachMember(found, [&](ValueFlowObjectID object) {
                    findsMore |= !m_context.contains(visited, object);
                });
                visited = m_context.unite(visited, found);
                holders = found;
            }
            return;
        }
        case BuiltinPlaceKind::Member: {
            ClimbingSet holders = valueAt(*place.operand);
            ValueFlowNodeID member;
            for (BuiltinName name : place.memberPath) {
                member = m_context.lookup(holders, name ? property(m_context, name) : everyProperty);
                holders = m_context.valueOf(member);
            }
            if (member)
                functor(member);
            return;
        }
        case BuiltinPlaceKind::NativeCell:
            functor(m_context.read(holderOfNativeCells(), nameOfNativeCell(place.nativeCell)));
            return;
        case BuiltinPlaceKind::Result:
            if (m_call.result)
                functor(m_call.result);
            return;
        case BuiltinPlaceKind::ResultSlot:
            if (m_resultObject)
                functor(m_context.read(only(m_context, m_resultObject), nameOf(place.slot)));
            return;
        case BuiltinPlaceKind::ResultField:
            if (m_resultObject)
                functor(m_context.read(only(m_context, m_resultObject), nameOfResultField(place.fieldIndex)));
            return;
        case BuiltinPlaceKind::NewValue:
            constant = m_context.maker().unite(constant, valueOfType(place.newValueType, m_call.contentsObject));
            return;
        case BuiltinPlaceKind::NativeMemory:
            constant = m_context.maker().unite(constant, unknownValue(m_context));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What lies at the place as it stands in this round.
    ClimbingSet valueAt(const BuiltinPlace& place)
    {
        ClimbingSet value = place.kind == BuiltinPlaceKind::Operand ? valueAt(*place.operand) : ClimbingSet();
        ValueSetID constant = ValueSets::nothing;
        forEachNodeAt(place, constant, [&](ValueFlowNodeID node) {
            value = m_context.unite(value, m_context.valueOf(node));
        });
        value = m_context.unite(value, ClimbingSet(constant));
        return place.isSettled ? m_context.valueOf(m_context.lookup(value, awaitedValue)) : value;
    }

    void forEachOperandAt(const BuiltinPlace& place, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor)
    {
        if (place.kind == BuiltinPlaceKind::None)
            return;
        if (place.isSettled) {
            BuiltinPlace unsettled = place;
            unsettled.isSettled = false;
            functor(operandAt(m_context.lookup(valueAt(unsettled), awaitedValue)));
            return;
        }
        if (place.kind == BuiltinPlaceKind::Operand) {
            forEachOperandAt(*place.operand, functor);
            return;
        }

        ValueSetID constant = ValueSets::nothing;
        forEachNodeAt(place, constant, [&](ValueFlowNodeID node) {
            functor(operandAt(node));
        });
        if (constant != ValueSets::nothing)
            functor(operandHolding(constant));
    }

    void connectFrom(const BuiltinPlace& place, ValueFlowNodeID target)
    {
        if (!target)
            return;
        forEachOperandAt(place, [&](const ValueFlowOperand& operand) {
            m_context.connect(operand, target);
        });
    }

    // The nodes that take what is put at the place: none where nobody reads it.
    void forEachTargetAt(const BuiltinPlace& place, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        // The program may have kept the object that its species constructor has made.
        auto writeToResult = [&](ValueFlowName name) {
            functor(m_resultObject ? m_context.written(only(m_context, m_resultObject), name) : escaped());
            if (m_context.meet(m_objectsMadeBySpecies, m_universe.objects()))
                forEachNodeWrittenThrough(m_objectsMadeBySpecies, name, functor);
        };

        switch (place.kind) {
        case BuiltinPlaceKind::None:
            return;
        case BuiltinPlaceKind::Result:
            if (m_call.result)
                functor(m_call.result);
            return;
        case BuiltinPlaceKind::ResultSlot:
            writeToResult(nameOf(place.slot));
            return;
        case BuiltinPlaceKind::ResultField:
            writeToResult(nameOfResultField(place.fieldIndex));
            return;
        case BuiltinPlaceKind::Slot:
            forEachNodeWrittenThrough(objectsAt(*place.operand), nameOf(place.slot), functor);
            return;
        case BuiltinPlaceKind::OwnValues:
            forEachNodeWrittenThrough(objectsAt(*place.operand), unknownKey, functor);
            return;
        case BuiltinPlaceKind::Prototype:
            forEachNodeWrittenThrough(objectsAt(*place.operand), prototypeLink, functor);
            return;
        case BuiltinPlaceKind::NativeCell:
            functor(m_context.written(holderOfNativeCells(), nameOfNativeCell(place.nativeCell)));
            return;
        case BuiltinPlaceKind::NativeMemory:
        case BuiltinPlaceKind::Operand:
        case BuiltinPlaceKind::IteratedValues:
        case BuiltinPlaceKind::NewValue:
        case BuiltinPlaceKind::Member:
            functor(escaped());
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void connect(const BuiltinPlace& from, const BuiltinPlace& to)
    {
        forEachTargetAt(to, [&](ValueFlowNodeID target) {
            connectFrom(from, target);
        });
    }

    // What native code makes or has at hand of the type. `fresh` stands for an object that this call allocates. One object stands for everything that is made to be put below it.
    ValueSetID valueOfType(BuiltinTypeID id, ValueFlowObjectID fresh)
    {
        if (!id)
            return unknownValue(m_context);
        const BuiltinType& type = m_builtins.at(id);
        if (type.isAnything())
            return unknownValue(m_context);

        ValueSetMaker& maker = m_context.maker();
        ValueSetID value = m_tables.primitivesOf(id);
        if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }))
            value = maker.unite(value, m_universe.unknownObjectAlone());

        Vector<ValueFlowObjectID, 32> objects;
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Builtin:
                // Of a built-in of which each module has an object of its own, native code may hand out any.
                m_context.use(part.builtin, BuiltinUse::Read);
                objects.shrink(0);
                m_universe.appendObjectsOf(part.builtin, objects);
                value = maker.unite(value, maker.of(objects.span()));
                break;
            case BuiltinTypePartKind::Instance:
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            case BuiltinTypePartKind::Layout:
                if (type.freshness == BuiltinFreshness::No || !fresh || isCalledInWaysNobodyFollows(part)) {
                    value = maker.unite(value, m_universe.unknownObjectAlone());
                    break;
                }
                allocate(part, fresh, type.freshness);
                value = maker.unite(value, maker.of(fresh));
                break;
            // A transfer or a callback says where these come from.
            case BuiltinTypePartKind::Operand:
            case BuiltinTypePartKind::Slot:
            case BuiltinTypePartKind::IteratedValues:
            case BuiltinTypePartKind::CallbackResult:
            case BuiltinTypePartKind::Prototype:
                break;
            }
        }
        return value;
    }

    void allocate(const BuiltinTypePart& part, ValueFlowObjectID fresh, BuiltinFreshness freshness)
    {
        ClimbingSet object = only(m_context, fresh);
        ValueFlowNodeID prototype = m_context.written(object, prototypeLink);
        switch (part.kind) {
        case BuiltinTypePartKind::Instance:
            if (BuiltinID prototypeObject = m_builtins.at(part.builtinClass).prototype)
                m_context.add(prototype, only(m_context, m_universe.objectOf(prototypeObject)));
            else
                m_context.add(prototype, m_universe.unknownObjectAlone());
            // An unaliased result may have own properties that the table does not declare. A lookup that comes by the unknown object answers the unknown value for every name.
            if (freshness == BuiltinFreshness::Unaliased && m_builtins.mayDefineOnNewInstances(part.builtinClass, { }))
                m_context.add(prototype, m_universe.unknownObjectAlone());
            // A class of the program that extends the built-in class hands down its own prototype object.
            if (isConstruction(m_call.kind) && fresh == m_resultObject)
                m_context.connect(m_context.lookup(m_context.valueOf(m_call.thisValue), property(m_context, BuiltinNames::prototype)), prototype);
            break;
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            connectFrom({ .kind = BuiltinPlaceKind::Prototype, .operand = BuiltinOperand::receiver() }, prototype);
            break;
        case BuiltinTypePartKind::Layout: {
            const BuiltinLayout& layout = m_builtins.at(part.layout);
            m_context.add(prototype, only(m_context, m_universe.objectOf(layout.isTuple ? Builtins::ArrayPrototype : Builtins::ObjectPrototype)));
            for (const BuiltinLayoutField& field : layout.fields)
                m_context.add(m_context.written(object, layout.isTuple ? elements : property(m_context, field.name)), valueOfType(field.type, m_call.contentsObject));
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (std::optional<BuiltinOperand> source = m_overload->result.prototypeSource; source && fresh == m_resultObject)
            connectFrom({ .kind = BuiltinPlaceKind::Prototype, .operand = source }, prototype);
        for (const BuiltinSlotContents& contents : part.contents)
            m_context.add(m_context.written(object, nameOf(contents.slot)), valueOfType(contents.type, m_call.contentsObject));
    }

    void passArguments(const ValueFlowCallGroup& group, std::span<const BuiltinPlace> arguments)
    {
        for (unsigned index = 0; index < arguments.size(); ++index)
            connectFrom(arguments[index], index < numberOfGroupedArguments ? group.argument(index) : group.otherArguments);
    }

    void useResult(const ValueFlowCallGroup& group, BuiltinCallbackResultUse use, const BuiltinPlace& resultTo)
    {
        switch (use) {
        case BuiltinCallbackResultUse::Ignored:
        case BuiltinCallbackResultUse::Tested:
            return;
        case BuiltinCallbackResultUse::Escapes:
            m_context.connect(group.result, escaped());
            return;
        case BuiltinCallbackResultUse::Returned:
            if (m_call.result)
                m_context.connect(group.result, m_call.result);
            return;
        case BuiltinCallbackResultUse::Stored:
            forEachTargetAt(resultTo, [&](ValueFlowNodeID target) {
                m_context.connect(group.result, target);
            });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What a function throws that is called after the built-in has returned goes to native code.
    void connectThrown(const ValueFlowCallGroup& group, BuiltinCallbackTimes times)
    {
        if (isCalledAfterReturn(times) || !m_call.thrown)
            m_context.connect(group.thrown, escaped());
        else
            m_context.connect(group.thrown, m_call.thrown);
    }

    void applyCallback(const BuiltinCallback& callback)
    {
        ClimbingSet callees = m_context.restrict(valueAt(callback.callee), m_universe.callables());
        if (!m_context.meet(callees, m_universe.callables()))
            return;
        ValueFlowCallGroup group = m_context.group(callees, callback.constructs ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode);
        if (callback.constructs) {
            if (callback.newTarget.kind == BuiltinPlaceKind::None)
                m_context.add(group.newTarget, callees);
            else
                connectFrom(callback.newTarget, group.newTarget);
        } else if (callback.thisValue.kind == BuiltinPlaceKind::None)
            m_context.add(group.thisValue, m_universe.undefinedValue());
        else
            connectFrom(callback.thisValue, group.thisValue);
        passArguments(group, callback.arguments);
        connectFrom(callback.spreadArguments, group.otherArguments);
        useResult(group, callback.resultUse, callback.resultTo);
        connectThrown(group, callback.times);
    }

    void applyHandlerCall(const BuiltinHandlerCall& handlerCall)
    {
        ClimbingSet callees = m_context.restrict(valueAt(handlerCall.callee), m_universe.callables());
        if (!m_context.meet(callees, m_universe.callables()))
            return;
        ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::CallByNativeCode);
        if (handlerCall.thisValue.kind == BuiltinPlaceKind::None)
            m_context.add(group.thisValue, m_universe.undefinedValue());
        else
            connectFrom(handlerCall.thisValue, group.thisValue);
        passArguments(group, handlerCall.arguments);
        useResult(group, handlerCall.resultUse, { });
        connectThrown(group, handlerCall.times);
    }

    void forEachKeyOperand(const BuiltinKey& key, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor) const
    {
        if (key.kind == BuiltinKeyKind::Operand || key.kind == BuiltinKeyKind::ElementsOf)
            forEachOperandAt(*key.operand, functor);
        else
            functor({ });
    }

    // The heap rules do to the properties what the action says. Every action only adds, so a position that covers several operands is one application for each.
    void applyPropertyAction(const BuiltinPropertyAction& action)
    {
        auto applyWith = [&](const ValueFlowOperand& value, ValueFlowNodeID target) {
            forEachOperandAt(action.object, [&](const ValueFlowOperand& object) {
                forEachKeyOperand(action.key, [&](const ValueFlowOperand& key) {
                    Typed::applyPropertyAction(m_context, action, { object, key, value, target, m_call.thrown });
                });
            });
        };
        auto applyWithValue = [&](const ValueFlowOperand& value) {
            applyWith(value, { });
        };

        switch (action.kind) {
        case BuiltinPropertyActionKind::ReadsByKey:
        case BuiltinPropertyActionKind::ReadsGetter:
        case BuiltinPropertyActionKind::ReadsSetter: {
            // A read that nobody takes still runs a getter.
            bool isTaken = false;
            forEachTargetAt(action.value, [&](ValueFlowNodeID target) {
                isTaken = true;
                applyWith({ }, target);
            });
            if (!isTaken)
                applyWith({ }, { });
            return;
        }
        case BuiltinPropertyActionKind::DescribesOwnProperties:
            applyWith({ }, m_call.result);
            return;
        case BuiltinPropertyActionKind::DeletesByKey:
        case BuiltinPropertyActionKind::ChangesIntegrity:
        case BuiltinPropertyActionKind::TestsByKey:
            applyWith({ }, { });
            return;
        case BuiltinPropertyActionKind::BecomesPrototype:
            if (action.value.kind == BuiltinPlaceKind::Result)
                applyWith(m_resultObject ? operandHolding(m_context.maker().of(m_resultObject)) : ValueFlowOperand(), { });
            else
                forEachOperandAt(action.value, applyWithValue);
            return;
        case BuiltinPropertyActionKind::AssignsFrom:
            // The place names what the sources hold, the heap rules take the sources.
            if (action.value.kind == BuiltinPlaceKind::OwnValues)
                forEachOperandAt(*action.value.operand, applyWithValue);
            else
                forEachOperandAt(action.value, applyWithValue);
            return;
        case BuiltinPropertyActionKind::StoresByKey:
        case BuiltinPropertyActionKind::DefinesName:
        case BuiltinPropertyActionKind::DefinesGetter:
        case BuiltinPropertyActionKind::DefinesSetter:
        case BuiltinPropertyActionKind::TakesDescriptor:
        case BuiltinPropertyActionKind::TakesDescriptorMap:
        case BuiltinPropertyActionKind::ChangesPrototype:
            forEachOperandAt(action.value, applyWithValue);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    BuiltinID m_builtin;
    const ValueFlowBuiltinCall& m_call;
    ValueFlowRuleContext& m_context;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    const ValueFlowUniverse& m_universe;
    const ValueFlowBuiltinTables& m_tables;
    ValueFlowOperand m_undefined;
    const BuiltinOverload* m_overload { nullptr };
    // Of the overload that is applied: none where the unknown object stands for what it returns.
    ValueFlowObjectID m_resultObject;
    ClimbingSet m_objectsMadeBySpecies;
};

bool passesList(Opcode opcode)
{
    return opcode == Opcode::CallList || opcode == Opcode::ConstructList || opcode == Opcode::SuperConstructList;
}

// The operands of all six operations: the callee, then this or new.target, then the arguments or the list of them.
ValueFlowBuiltinCall describe(ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(call);

    ValueFlowBuiltinCall description;
    description.kind = kind;
    // Without the operand the callee is new.target.
    bool hasSecondOperand = !isConstruction(kind) || !!context.program().at(call.function).at(call.site).operand(1);
    description.thisValue = operands[hasSecondOperand ? 1 : 0];
    if (passesList(call.opcode))
        description.otherArguments = operandAt(context.lookup(context.valueOf(operands[2]), elements));
    else
        description.arguments = operands.subspan(2);
    description.result = call.result;
    description.thrown = call.thrown;
    description.resultObject = universe.siteObject(call, 0);
    description.contentsObject = universe.siteObject(call, 1);
    return description;
}

} // anonymous namespace

void applyBuiltin(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    if (applyStepOfBuiltinIterator(builtin, call, context) || applyConstructionOfPromise(builtin, call, context))
        return;
    BuiltinApplication(builtin, call, context).run();
    holdContentsIteratedByResult(builtin, call, context);
}

void applyBuiltin(BuiltinID builtin, ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    applyBuiltin(builtin, describe(kind, call, context), context);
}

static void callOtherCallables(ClimbingSet callees, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowCallKind kind = isConstruction(description.kind) ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode;
    auto slot = [&](BuiltinSlotID id) {
        return context.read(callees, { ValueFlowNameKind::Slot, id.index() });
    };

    // The bound arguments come first, so no argument of the call keeps its position.
    ClimbingSet targets = context.restrict(context.valueOf(slot(BuiltinSlots::BoundFunctionTarget)), universe.callables());
    ValueFlowCallGroup group = context.group(targets, kind);
    if (isConstruction(kind))
        context.add(group.newTarget, targets);
    else
        context.connect(slot(BuiltinSlots::BoundFunctionBoundThis), group.thisValue);
    context.connect(slot(BuiltinSlots::BoundFunctionBoundArguments), group.otherArguments);
    for (const ValueFlowOperand& argument : description.arguments)
        context.connect(argument, group.otherArguments);
    context.connect(description.otherArguments, group.otherArguments);
    if (description.result)
        context.connect(group.result, description.result);
    if (description.thrown)
        context.connect(group.thrown, description.thrown);
    settlePromisesThrough(callees, description, context);
}

void noteCallOfBuiltin(BuiltinID builtin, ValueFlowCallKind kind, ValueFlowRuleContext& context)
{
    context.use(builtin, useOf(kind));
}

void callOtherCallables(ClimbingSet callees, ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    callOtherCallables(callees, describe(kind, call, context), context);
}

static void withDescriptionOf(ValueFlowCallKind kind, const ValueFlowCallGroup& group, NOESCAPE const Invocable<void(ValueFlowBuiltinCall&)> auto& functor)
{
    std::array<ValueFlowOperand, numberOfGroupedArguments> arguments;
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        arguments[index] = operandAt(group.argument(index));

    ValueFlowBuiltinCall description;
    description.kind = kind;
    description.thisValue = operandAt(isConstruction(kind) ? group.newTarget : group.thisValue);
    description.arguments = arguments;
    description.argumentsMayBeAbsent = true;
    description.otherArguments = operandAt(group.otherArguments);
    description.result = group.result;
    description.thrown = group.thrown;
    functor(description);
}

void applyBuiltin(BuiltinID builtin, ValueFlowCallKind kind, const ValueFlowCallGroup& group, ValueFlowRuleContext& context)
{
    withDescriptionOf(kind, group, [&](ValueFlowBuiltinCall& description) {
        description.resultObject = context.universe().resultOf(builtin, 0);
        description.contentsObject = context.universe().resultOf(builtin, 1);
        applyBuiltin(builtin, description, context);
    });
}

void callOtherCallables(ClimbingSet callees, ValueFlowCallKind kind, const ValueFlowCallGroup& group, ValueFlowRuleContext& context)
{
    withDescriptionOf(kind, group, [&](ValueFlowBuiltinCall& description) {
        callOtherCallables(callees, description, context);
    });
}

void runInstanceOf(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const ValueFlowOperand& tested = operands[0];
    const ValueFlowOperand& constructor = operands[1];

    if (statement.result)
        context.add(statement.result, context.maker().of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::False), ValueFlowUniverse::primitive(PrimitiveSet::True) }));
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context));

    ClimbingSet methods = context.restrict(context.valueOf(context.lookup(context.valueOf(constructor), property(context, BuiltinNames::hasInstanceSymbol))), universe.callables());
    if (!context.meet(methods, universe.callables()))
        return;
    ValueFlowCallGroup group = context.group(methods, ValueFlowCallKind::CallByNativeCode);
    context.connect(constructor, group.thisValue);
    context.connect(tested, group.argument(0));
    if (statement.thrown)
        context.connect(group.thrown, statement.thrown);
}

void runBehavior(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowBuiltinTables& tables = universe.builtinTables();
    ValueFlowNodeID hub = statement.result;
    const ValueFlowBehavior& behavior = tables.at(context.nodes().behaviorOf(hub));

    // Native code does all of this to an escaped object anyway.
    ValueSetID mask = everythingButUnknownObject(context);
    ClimbingSet receivers = context.restrict(context.valueOf(hub), mask);
    ValueFlowOperand thisValue = operandAt(hub, mask);
    // What a protocol reads from a receiver, or is handed by its code, is a receiver too.
    ValueFlowNodeID handedOut = behavior.isTransitive ? hub : ValueFlowNodeID();
    auto call = [&](BuiltinName name, ValueSetID arguments, ValueFlowNodeID result) {
        callFromNativeCode(context, { context.valueOf(context.lookup(receivers, property(context, name))), thisValue, arguments, result });
    };
    auto take = [&](ValueFlowNodeID node) {
        if (handedOut)
            context.connect(node, handedOut);
    };

    // What the table says a built-in object holds is no code of the program and leads to none.
    ValueSetID objectsOfProgram = context.maker().intersect(universe.objects(), ValueSets::complement(universe.builtinObjects()));
    ClimbingSet objects = context.restrict(receivers, universe.objects());
    auto runGettersOf = [&](ClimbingSet holders) {
        ClimbingSet getters = context.restrict(context.valueOf(context.read(holders, everyGetter)), universe.callables());
        if (!context.meet(getters, universe.callables()))
            return;
        ValueFlowCallGroup group = context.group(getters, ValueFlowCallKind::Get);
        context.connect(thisValue, group.thisValue);
        context.connect(group.thrown, ValueFlowProgramNodes::escaped());
        take(group.result);
    };
    // The receivers with the objects above them.
    auto chain = [&] {
        return context.restrict(context.valueOf(context.lookup(objects, prototypeLink)), objectsOfProgram);
    };

    if (behavior.isTransitive)
        context.connect(context.read(objects, everyProperty), hub);
    if (behavior.protocols.contains(Protocol::OwnValues))
        runGettersOf(objects);
    if (behavior.protocols.contains(Protocol::ToPrimitive)) {
        for (BuiltinName name : { BuiltinNames::toPrimitiveSymbol, BuiltinNames::valueOf, BuiltinNames::toString })
            call(name, universe.anyPrimitive(), { });
    }
    if (behavior.protocols.contains(Protocol::ToJSON))
        call(BuiltinNames::toJSON, universe.anyPrimitive(), handedOut ? handedOut : ValueFlowProgramNodes::escaped());
    if (behavior.protocols.contains(Protocol::HasInstance))
        call(BuiltinNames::hasInstanceSymbol, unknownValue(context), { });
    if (behavior.protocols.contains(Protocol::Thenable))
        take(context.lookup(receivers, awaitedValue));
    if (behavior.protocols.contains(Protocol::Iterate))
        take(context.lookup(receivers, iteratedValues));
    if (behavior.protocols.contains(Protocol::Named)) {
        // Without names: the built-in may read any member, an inherited one too.
        if (behavior.namedMembers.empty()) {
            ClimbingSet holders = chain();
            take(context.read(holders, everyProperty));
            runGettersOf(holders);
        }
        for (BuiltinName name : behavior.namedMembers) {
            ValueFlowNodeID member = context.lookup(receivers, property(context, name));
            take(member);
            // The conversion goes with the members.
            if (!handedOut && behavior.protocols.contains(Protocol::ToPrimitive))
                context.connect(member, context.hub(tables.conversionToPrimitive()));
        }
    }
    if (behavior.protocols.contains(Protocol::CallsNamed)) {
        // Whatever is called here gets operands that the host chooses. An object nobody knows may have such a member too.
        bool callsSomething = context.contains(context.valueOf(hub), ValueFlowUniverse::unknownObject());
        // Without names: the built-in may call any method that it finds.
        if (behavior.calledMembers.empty()) {
            ClimbingSet methods = context.valueOf(context.read(chain(), everyProperty));
            callFromNativeCode(context, { methods, thisValue, unknownValue(context), handedOut ? handedOut : ValueFlowProgramNodes::escaped() });
            callsSomething |= context.meet(methods, universe.callables());
        }
        for (BuiltinName name : behavior.calledMembers) {
            call(name, unknownValue(context), handedOut ? handedOut : ValueFlowProgramNodes::escaped());
            callsSomething |= context.meet(context.valueOf(context.lookup(receivers, property(context, name))), universe.callables());
        }
        if (callsSomething)
            letHostChooseOperands(context);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
