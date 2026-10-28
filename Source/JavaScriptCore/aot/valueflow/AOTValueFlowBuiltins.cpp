/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBuiltins.h"

#if ENABLE(AOT)

#include "AOTFlowObjectOperations.h"
#include "AOTFlowSiteData.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeRelations.h"
#include "AOTUseLists.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueFlowTypedResults.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <functional>
#include <wtf/NeverDestroyed.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr ValueFlowName everySetter = ValueFlowName::of(ValueFlowNameKind::EverySetter);
constexpr ValueFlowCellName iteratedValues { ValueFlowNameKind::IteratedValues };

constexpr ValueFlowOperand operandHolding(ValueSetID constant)
{
    return { { }, constant, ValueSets::anything };
}

constexpr ValueFlowOperand operandOfBuiltinAt(ValueFlowNodeID node, ValueSetID mask = ValueSets::anything)
{
    return { node, ValueSets::nothing, mask };
}

ValueFlowName property(ValueFlowRuleContext& context, BuiltinName name)
{
    return ValueFlowName::property(context.program(), context.program().keyOf(name));
}

static ClimbingSet withHostObjectsAmongMembers(ValueFlowRuleContext& context, ClimbingSet hostObjects)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowHeapTables& heap = universe.builtinTables().heap();
    Vector<ValueFlowObjectID, 16> found;
    context.forEachMember(hostObjects, [&](ValueFlowObjectID object) {
        found.append(object);
    });
    for (size_t index = 0; index < found.size(); ++index) {
        BuiltinID holder = universe.builtinOf(found[index]);
        if (!holder || universe.moduleOf(found[index]))
            continue;
        context.forEachMember(ClimbingSet(context.maker().intersect(heap.membersOf(holder).values, heap.holdersOfMembersThatHostMakes())), [&](ValueFlowObjectID member) {
            if (!found.contains(member))
                found.append(member);
        });
    }
    return ClimbingSet(context.maker().of(found.span()));
}

static void forEachNodeReadByWalkOver(ValueFlowRuleContext& context, ClimbingSet holders, Protocols protocols, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    if (protocols.contains(Protocol::PrototypeChain)) {
        if (context.meet(holders, ValueSets::anything))
            functor(context.lookup(holders, everyPropertyName));
        return;
    }
    Vector<ValueFlowObjectID, 2> holdersOfSuchMembers;
    for (auto [mark, member] : builtins.marked(BuiltinMark::IsNotEnumerable)) {
        ValueFlowObjectID holder = universe.objectOf(builtins.at(member).holder);
        if (context.contains(holders, holder) && !holdersOfSuchMembers.contains(holder))
            holdersOfSuchMembers.append(holder);
    }
    ClimbingSet others = context.restrict(holders, ValueSets::complement(context.maker().of(holdersOfSuchMembers.span())));
    if (context.meet(others, ValueSets::anything))
        functor(context.lookup(others, everyOwnPropertyName));
    for (ValueFlowObjectID holder : holdersOfSuchMembers) {
        functor(context.readWhatWasWritten(only(context, holder), everyPropertyName));
        for (BuiltinID member : builtins.membersOf(universe.builtinOf(holder))) {
            if (builtins.at(member).name && !builtins.has(BuiltinMark::IsNotEnumerable, builtins.canonicalID(member)))
                functor(context.lookup(only(context, holder), property(context, builtins.at(member).name)));
        }
    }
}

ValueFlowCellName nameOfNativeCell(ValueFlowRuleContext& context, BuiltinName cell)
{
    return ValueFlowCellName { ValueFlowNameKind::Slot, context.program().builtins().slotCount() + cell.index() };
}

ValueFlowNodeID nodeWrittenToNativeCell(ValueFlowRuleContext& context, BuiltinName cell)
{
    return context.written(only(context, context.universe().objectOf(Builtins::GlobalThis)), nameOfNativeCell(context, cell));
}

ValueSetID everythingButUnknownObject(ValueFlowRuleContext& context)
{
    return ValueSets::complement(context.universe().unknownObjectAlone());
}

ValueSetID unknownObjectSet(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    return context.maker().intersect(unknownValue(context, source), context.universe().objects());
}

ClimbingSet withWhatUntrackedObjectCovers(ValueFlowRuleContext& context, ClimbingSet objects)
{
    if (!context.contains(objects, ValueFlowUniverse::untrackedObject()))
        return objects;
    return context.unite(objects, context.valueWithin(ClimbingSet(context.universe().knownObjects()), ValueFlowProgramNodes::lost()));
}

std::pair<BuiltinCallMode, BuiltinUse> modeAndUseOf(ValueFlowCallKind kind)
{
    switch (kind) {
    case ValueFlowCallKind::Call:
    case ValueFlowCallKind::CallByNativeCode:
    case ValueFlowCallKind::CallByHost:
        return { BuiltinCallMode::Call, BuiltinUse::Called };
    case ValueFlowCallKind::Construct:
    case ValueFlowCallKind::ConstructByNativeCode:
        return { BuiltinCallMode::Construct, BuiltinUse::Constructed };
    case ValueFlowCallKind::Get:
        return { BuiltinCallMode::Get, BuiltinUse::Read };
    case ValueFlowCallKind::Set:
        return { BuiltinCallMode::Set, BuiltinUse::Written };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What each of several receivers finds by itself. A callee gets the receivers that find it, whichever others were looked at beside them.
class ReceiversByWhatTheyFind {
public:
    ReceiversByWhatTheyFind(ValueFlowRuleContext& context, const ValueFlowOperand& receivers, NOESCAPE const Invocable<ClimbingSet(ClimbingSet)> auto& find)
        : m_context(context)
        , m_others(receivers)
    {
        const ValueFlowUniverse& universe = context.universe();
        context.forEachMember(context.restrict(context.valueOf(receivers), universe.knownObjects()), [&](ValueFlowObjectID receiver) {
            m_known.append({ receiver, find(only(context, receiver)) });
        });
        m_others.mask = context.maker().intersect(m_others.mask, ValueSets::complement(universe.knownObjects()));
    }

    // A set that holds a callee nobody follows lists none of the callees that one stands for. A receiver that finds one of those and no callee nobody follows names it.
    ClimbingSet withCalleesFoundAlone(ClimbingSet callees) const
    {
        if (!m_context.meet(callees, m_context.universe().placeholdersAlone()))
            return callees;
        for (const Known& known : m_known)
            callees = m_context.unite(callees, known.found);
        return callees;
    }

    void forEachOperandHoldingReceiversOf(ClimbingSet callees, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor) const
    {
        ValueSetID placeholders = m_context.universe().placeholdersAlone();
        ClimbingSet sought = m_context.meet(callees, placeholders) ? ClimbingSet(placeholders) : callees;
        Vector<ValueFlowObjectID, 8> finding;
        for (const Known& known : m_known) {
            if (m_context.meet(known.found, sought))
                finding.append(known.receiver);
        }
        if (!finding.isEmpty())
            functor(operandHolding(m_context.maker().of(finding.span())));
        if (m_others.node || m_context.meet(m_context.valueOf(m_others), ValueSets::anything))
            functor(m_others);
    }

    void passReceiversOf(ClimbingSet callees, ValueFlowNodeID thisValue) const
    {
        forEachOperandHoldingReceiversOf(callees, [&](const ValueFlowOperand& finding) {
            m_context.connect(finding, thisValue);
        });
    }

private:
    struct Known {
        ValueFlowObjectID receiver;
        ClimbingSet found;
    };

    ValueFlowRuleContext& m_context;
    Vector<Known, 8> m_known;
    ValueFlowOperand m_others;
};

static void callFromNativeCode(ValueFlowRuleContext& context, ClimbingSet members, const ValueFlowOperand& thisValue, ValueSetID arguments, ValueFlowNodeID result, NOESCAPE const Invocable<ClimbingSet(ClimbingSet)> auto& find)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!context.meet(members, universe.callables()))
        return;
    ReceiversByWhatTheyFind receivers(context, thisValue, find);
    members = receivers.withCalleesFoundAlone(members);

    // A built-in is applied to the operands of this call and answers to this call alone, whoever else calls it.
    ValueFlowBuiltinCall description { .kind = ValueFlowCallKind::CallByNativeCode, .argumentsMayBeAbsent = true, .otherArguments = operandHolding(arguments), .result = result, .thrown = ValueFlowProgramNodes::escaped(), .isFoundOnReceiver = true };
    Vector<ValueFlowObjectID, 4> applied;
    context.forEachMember(context.restrict(members, universe.builtinFunctions()), [&](ValueFlowObjectID callee) {
        BuiltinID builtin = universe.builtinOf(callee);
        if (raisesEventWhenCalled(context.program().builtins(), builtin))
            return;
        applied.append(callee);
        description.resultObject = universe.resultOf(builtin, 0);
        description.contentsObject = universe.resultOf(builtin, 1);
        receivers.forEachOperandHoldingReceiversOf(only(context, callee), [&](const ValueFlowOperand& finding) {
            description.thisValue = finding;
            applyBuiltin(builtin, description, context);
        });
    });

    forEachPartOfCallees(context, context.restrict(members, context.maker().intersect(universe.callables(), ValueSets::complement(context.maker().of(applied.span())))), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName);
        receivers.passReceiversOf(callees, group.thisValue);
        context.add(group.otherArguments, arguments);
        carryOut(context, group.thrown);
        if (result)
            context.connect(group.result, result);
    });
}

void passOperandsOf(const ValueFlowBuiltinCall& call, const ValueFlowCallGroup& group, ValueFlowRuleContext& context)
{
    context.connect(call.thisValue, isConstruction(call.kind) ? group.newTarget : group.thisValue);
    if (isConstruction(call.kind))
        context.connect(call.thisValue, context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands));
    for (unsigned index = 0; index < call.arguments.size(); ++index)
        context.connect(call.arguments[index], group.argumentAt(index));
    context.connect(call.laterArguments, group.laterArguments);
    context.connect(call.otherArguments, group.otherArguments);
    passNoFurtherArguments(context, group, call.argumentsMayBeAbsent || call.otherArguments.node ? 0 : call.arguments.size());
    if (call.result)
        context.connect(group.result, call.result);
    if (call.thrown)
        context.connect(group.thrown, call.thrown);
}

ValueSetID objectsAllocatedByBuiltinsAs(ValueFlowRuleContext& context, ClimbingSet values, ValueSetID classes, bool hasPrototypeOfClass)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueSetID others = ValueSets::complement(classes);
    Vector<ValueFlowObjectID, 8> allocated;
    context.forEachMember(context.restrict(values, context.maker().intersect(universe.objectsThatMayBeAnything(), universe.knownObjects())), [&](ValueFlowObjectID object) {
        ClimbingSet named = context.valueOf(context.written(only(context, object), allocatedAsName));
        if (!context.meet(named, ValueSets::anything) || context.meet(named, others))
            return;
        if (hasPrototypeOfClass && context.meet(context.valueOf(context.readWhatWasWritten(only(context, object), prototypeLinkName)), others))
            return;
        allocated.append(object);
    });
    return context.maker().of(allocated.span());
}

static void noteCallThatRunsOtherCode(BuiltinID builtin, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    context.add(context.written(only(context, universe.objectOf(builtin)), cellOfCallsThatRunOtherCode(context.program().builtins())), universe.undefinedValue());
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
        m_context.use(m_builtin, modeAndUseOf(m_call.kind).second);
        if (callsFunction() || m_context.isGuardedByRemovedFeature(m_builtin))
            throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));

        std::optional<BuiltinDispatchTarget> root = m_builtins.dispatchOf(m_builtin, modeAndUseOf(m_call.kind).first);
        if (!root) {
            throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));
            if (isConstruction(m_call.kind) && isPutByNativeModule()) {
                actWithoutOverload();
                handToUndescribedNativeCode(BuiltinOperand::argumentsFrom(0));
            }
            return;
        }

        notePropertiesThatRowsAskFor();
        Vector<BuiltinOverloadID, 4> selected;
        if (BuiltinOverloadID whateverIsPassed = overloadForAnotherNewTarget(m_builtins, m_builtin, m_call.kind)) {
            ValueSetMaker& maker = m_context.maker();
            ValueSetID itself = maker.of(m_universe.objectOf(m_builtins.canonicalID(m_builtin)));
            ClimbingSet newTargets = valueAt(BuiltinOperand::receiver());
            if (m_context.meet(newTargets, maker.unite(itself, m_universe.placeholdersAlone())))
                select(*root, selected);
            if (m_context.meet(newTargets, maker.intersect(ValueSets::complement(itself), ValueSets::complement(m_universe.anyPrimitive()))))
                selected.appendIfNotContains(whateverIsPassed);
        } else
            select(*root, selected);
        if (m_call.saysOnlyWhatIsCalled) {
            for (BuiltinOverloadID overload : selected)
                noteWhatIsCalledBy(m_builtins.at(overload));
            return;
        }
        if (m_call.readsOnlySpecies) {
            for (BuiltinOverloadID overload : selected)
                readSpeciesFor(m_builtins.at(overload));
            return;
        }
        for (BuiltinOverloadID overload : selected)
            apply(m_builtins.at(overload));
        if (mayGetThisThatDeclaredOverloadsLeaveOut())
            applyWhatIsNotDeclared(selected.span());
    }

    // The Throws flag of an overload covers only what follows the conversion of its operands. Any call can fail earlier: on a conversion, on the receiver check, on stack overflow.
    bool callsFunction() const
    {
        BuiltinCallMode mode = modeAndUseOf(m_call.kind).first;
        return mode == BuiltinCallMode::Call || mode == BuiltinCallMode::Construct || m_builtins.at(m_builtin).kind == BuiltinKind::Accessor;
    }

    bool runsOperandCode(const BuiltinOverload& overload) const
    {
        auto hasBehavior = [&](const BuiltinParameter& parameter) {
            return !!m_tables.behaviorOf(parameter);
        };
        return (overload.receiver && hasBehavior(*overload.receiver)) || std::ranges::any_of(overload.parameters, hasBehavior);
    }

    bool isPutByNativeModule() const
    {
        BuiltinID holder = m_builtins.at(m_builtins.canonicalID(m_builtin)).holder;
        return holder && m_context.sets().contains(m_tables.heap().objectsOfNativeModules(), m_universe.objectOf(m_builtins.canonicalID(holder)));
    }

    void callNamedMember(ClimbingSet members, const ValueFlowOperand& thisValue, std::span<const BuiltinPlace> arguments, ValueFlowNodeID result, NOESCAPE const Invocable<ClimbingSet(ClimbingSet)> auto& find)
    {
        actWithoutOverload();
        ReceiversByWhatTheyFind receivers(m_context, thisValue, find);
        forEachPartOfCallees(m_context, m_context.restrict(receivers.withCalleesFoundAlone(members), m_universe.callables()), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName);
            receivers.passReceiversOf(callees, group.thisValue);
            passArguments(group, arguments);
            passNoFurtherArguments(m_context, group, arguments.size());
            carryOut(m_context, group.thrown);
            if (result)
                m_context.connect(group.result, result);
        });
    }

    void describeHostObject(const BuiltinHostObject& record, ValueFlowObjectID object)
    {
        actWithoutOverload();
        ClimbingSet self = only(m_context, object);
        if (record.prototype)
            m_context.add(m_context.written(self, prototypeLinkName), only(m_context, m_universe.objectOf(record.prototype)));
        for (const BuiltinHostObjectField& field : record.fields) {
            if (field.name) {
                connectFrom(field.value, m_context.written(self, property(m_context, field.name)));
                continue;
            }
            connectFrom(field.value, m_context.written(self, unknownKeyName));
            connectFrom(field.value, m_context.written(self, elementsName));
            m_context.mark(self, ObjectMutation::AddProperty);
        }
        if (record.internalFields.empty())
            return;
        m_context.add(m_context.written(self, cellOfValueFromLibrary(m_builtins)), undescribedValue());
        ValueFlowName internalFields = ValueFlowName::of(ValueFlowNameKind::InternalFields);
        for (const BuiltinHostObjectInternalField& field : record.internalFields)
            useByHost(operandOfBuiltinAt(m_context.written(self, internalFields)), field.uses);
    }

    BuiltinApplication& actWithoutOverload()
    {
        static const NeverDestroyed<BuiltinOverload> noOverload;
        return actWithoutResultOf(noOverload.get());
    }

    void useByHost(const ValueFlowOperand& value, const BuiltinHostUses& uses)
    {
        if (!uses.areListed) {
            m_context.connect(value, escaped());
            return;
        }
        for (const BuiltinHostUse& use : uses.list) {
            ValueFlowOperand used = value;
            for (BuiltinName name : use.memberPath) {
                PropertyKeyID key = m_program.keyOf(name);
                used = operandOfBuiltinAt(m_context.lookup(m_context.valueOf(used), ValueFlowName::property(m_program, key)));
            }
            switch (use.use) {
            case BuiltinHostReadUse::Inspected:
                break;
            case BuiltinHostReadUse::Called:
                if (use.call)
                    callByHost(used, *use.call);
                else
                    m_context.connect(used, escaped());
                break;
            case BuiltinHostReadUse::PassedTo:
                passToBuiltin(used, m_builtins.canonicalID(use.builtin), use.argumentIndex);
                break;
            case BuiltinHostReadUse::Kept:
                m_context.connect(used, nodeWrittenToNativeCell(m_context, use.nativeCell));
                break;
            case BuiltinHostReadUse::IsValueOf:
                m_context.connect(used, m_context.written(only(m_context, m_universe.objectOf(use.builtin)), cellOfValueFromLibrary(m_builtins)));
                break;
            }
        }
    }

    void callByHost(const ValueFlowOperand& callee, const BuiltinHostCall& call)
    {
        ClimbingSet callables = m_context.restrict(m_context.valueOf(callee), m_universe.callables());
        callByHost(m_context.restrict(callables, ValueSets::complement(m_tables.closuresThatEntriesDescribe())), ValueFlowCallKind::CallByHost, call);
        callByHost(m_context.restrict(callables, m_tables.closuresThatEntriesDescribe()), ValueFlowCallKind::CallByNativeCode, call);
    }

private:
    static constexpr ValueFlowNodeID escaped() { return ValueFlowProgramNodes::escaped(); }
    ValueFlowNodeID valuesCarriedOut() const { return m_context.nodes().ofHost(ValueFlowHostNode::ValuesCarriedOut); }
    static constexpr ValueFlowNodeID lost() { return ValueFlowProgramNodes::lost(); }

    BuiltinApplication& actWithoutResultOf(const BuiltinOverload& overload)
    {
        m_overload = &overload;
        m_handsOnResult = false;
        m_resultObject = { };
        m_contentsObject = m_universe.resultOf(m_builtin, 1);
        return *this;
    }

    void callByHost(ClimbingSet callables, ValueFlowCallKind kind, const BuiltinHostCall& call)
    {
        forEachPartOfCallees(m_context, callables, [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, kind);
            if (call.thisValue.kind == BuiltinPlaceKind::None)
                m_context.add(group.thisValue, m_universe.undefinedValue());
            else
                connectFrom(call.thisValue, group.thisValue);
            if (call.listsArguments) {
                passArguments(group, call.arguments);
                connectFrom(call.otherArguments, group.otherArguments);
                passNoFurtherArguments(m_context, group, call.arguments.size());
            } else
                passWhatAnnotationsSay(m_context, callees);
            carryOut(m_context, group.thrown);
            useByHost(operandOfBuiltinAt(group.result), call.result);
        });
    }

    void passToBuiltin(const ValueFlowOperand& value, BuiltinID builtin, unsigned argumentIndex)
    {
        ValueFlowCallGroup group = m_context.group(only(m_context, m_universe.objectOf(builtin)), ValueFlowCallKind::CallByHost);
        BuiltinID holder = m_builtins.at(builtin).holder;
        if (holder && isObjectKind(m_builtins.at(holder).kind))
            m_context.add(group.thisValue, only(m_context, m_universe.objectOf(holder)));
        m_context.connect(value, group.argumentAt(argumentIndex));
        carryOut(m_context, group.result);
        carryOut(m_context, group.thrown);
    }

    void throwValue(ValueSetID value)
    {
        if (m_call.thrown)
            m_context.add(m_call.thrown, value);
    }

    void answer(ValueSetID value)
    {
        if (ValueFlowNodeID result = nodeTakingResult())
            m_context.add(result, value);
    }

    static bool handsOnResult(const BuiltinOverload& overload)
    {
        auto isResult = [](const BuiltinPlace& place) {
            return place.kind == BuiltinPlaceKind::Result;
        };
        return std::ranges::any_of(overload.flow.transfers, isResult, &BuiltinTransfer::from)
            || std::ranges::any_of(overload.flow.propertyActions, isResult, &BuiltinPropertyAction::value)
            || std::ranges::any_of(overload.flow.callbacks, [&](const BuiltinCallback& callback) {
                return isResult(callback.newTarget) || isResult(callback.thisValue) || isResult(callback.spreadArguments) || std::ranges::any_of(callback.arguments, isResult);
            })
            || std::ranges::any_of(overload.flow.handlerCalls, [&](const BuiltinHandlerCall& handlerCall) {
                return isResult(handlerCall.callee) || isResult(handlerCall.thisValue) || std::ranges::any_of(handlerCall.arguments, isResult);
            });
    }

    bool keepsDroppedResult() const { return !m_call.result && m_handsOnResult; }
    ValueFlowNodeID nodeTakingResult() { return keepsDroppedResult() ? m_context.written(only(m_context, m_universe.objectOf(m_builtin)), cellOfDroppedResults(m_builtins)) : m_call.result; }
    ValueFlowNodeID nodeHoldingResult() { return keepsDroppedResult() ? m_context.read(only(m_context, m_universe.objectOf(m_builtin)), cellOfDroppedResults(m_builtins)) : m_call.result; }

    bool mayGetThisThatDeclaredOverloadsLeaveOut()
    {
        const Builtin& entry = m_builtins.at(m_builtin);
        if (!entry.flags.contains(BuiltinFlag::RequiresHolderAsThis) || isConstruction(m_call.kind))
            return false;
        ValueSetID described = m_universe.undefinedValue();
        if (entry.holder) {
            Vector<ValueFlowObjectID, 32> holders;
            m_universe.appendObjectsOf(entry.holder, holders);
            described = m_context.maker().of(holders.span());
        }
        return m_context.meet(valueAt(BuiltinOperand::receiver()), ValueSets::complement(described));
    }

    void applyWhatIsNotDeclared(std::span<const BuiltinOverloadID> applied)
    {
        bool hasUndeclaredOverload = false;
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtin)) {
            if (overload.mode != modeAndUseOf(m_call.kind).first || !overload.isUndeclared())
                continue;
            hasUndeclaredOverload = true;
            bool isApplied = std::ranges::any_of(applied, [&](BuiltinOverloadID id) {
                return &m_builtins.at(id) == &overload;
            });
            if (!isApplied)
                apply(overload);
        }
        if (hasUndeclaredOverload)
            return;
        noteCallThatRunsOtherCode(m_builtin, m_context);
        handToUndescribedNativeCode(BuiltinOperand::receiver());
        handToUndescribedNativeCode(BuiltinOperand::argumentsFrom(0));
    }

    static bool isPassed(const ValueFlowOperand& operand) { return operand.node || (operand.constant && operand.constant != ValueSets::nothing); }

    void forEachOperandAt(BuiltinOperand position, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor) const
    {
        auto hand = [&](const ValueFlowOperand& operand) {
            if (isPassed(operand))
                functor(operand);
        };

        switch (position.kind()) {
        case BuiltinOperand::Kind::Receiver: {
            if (!isPassed(m_call.thisValue))
                return functor(m_undefined);
            ValueFlowOperand receiver = m_call.thisValue;
            if (m_receiversAccepted != ValueSets::anything)
                receiver.mask = m_context.maker().intersect(receiver.mask, m_receiversAccepted);
            functor(receiver);
            return;
        }
        case BuiltinOperand::Kind::Argument:
            if (position.argumentIndex() < m_call.arguments.size()) {
                hand(m_call.arguments[position.argumentIndex()]);
                if (!m_call.argumentsMayBeAbsent)
                    return;
            } else
                hand(m_call.laterArguments);
            hand(m_call.otherArguments);
            functor(m_undefined);
            return;
        case BuiltinOperand::Kind::ArgumentsFrom:
            for (size_t index = position.argumentIndex(); index < m_call.arguments.size(); ++index)
                hand(m_call.arguments[index]);
            hand(m_call.laterArguments);
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
    ClimbingSet known(ClimbingSet objects) { return knownObjectsThatAreFollowed(m_context, objects); }
    ClimbingSet unknownObjectAlone() const { return ClimbingSet(m_universe.unknownObjectAlone()); }
    ClimbingSet untrackedObjectAlone() const { return ClimbingSet(m_universe.untrackedObjectAlone()); }
    bool mayBeUnknownObject(ClimbingSet objects) const { return m_context.contains(objects, ValueFlowUniverse::unknownObject()); }
    bool mayBeUntrackedObject(ClimbingSet objects) const { return m_context.contains(objects, ValueFlowUniverse::untrackedObject()); }
    bool holdsAnything(ClimbingSet set) const { return m_context.meet(set, ValueSets::anything); }

    void forEachNodeWrittenThrough(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        functor(m_context.written(known(objects), name));
        if (mayBeUntrackedObject(objects))
            functor(m_context.written(untrackedObjectAlone(), name));
        if (!mayBeUnknownObject(objects))
            return;
        functor(m_context.written(unknownObjectAlone(), name));
        functor(valuesCarriedOut());
    }

    ClimbingSet withSourcesOfCopies(ClimbingSet objects)
    {
        ClimbingSet all = objects;
        ClimbingSet last = known(objects);
        for (;;) {
            last = known(m_context.valueOf(m_context.readWhatWasWritten(last, copiedFromName)));
            bool findsNewSource = false;
            m_context.forEachMember(last, [&](ValueFlowObjectID source) {
                findsNewSource |= !m_context.contains(all, source);
            });
            if (!findsNewSource)
                return all;
            all = m_context.unite(all, last);
        }
    }

    void forEachNodeReadThrough(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        if (name == everyPropertyName || name == everyGetterName || name == everySetter)
            objects = withSourcesOfCopies(objects);
        bool answersByName = mayBeUntrackedObject(objects);
        functor(m_context.readWhatWasWritten(answersByName ? m_context.unite(known(objects), m_context.valueWithin(ClimbingSet(m_universe.knownObjects()), lost())) : known(objects), name));
        if (answersByName || m_context.holdsAnyOf(objects, escaped()))
            functor(m_context.readWhatWasWritten(unknownObjectAlone(), name));
        if (answersByName || m_context.holdsAnyOf(objects, lost()))
            functor(m_context.readWhatWasWritten(untrackedObjectAlone(), name));
    }

    void connectOperands(BuiltinOperand position, ValueFlowNodeID target)
    {
        forEachOperandAt(position, [&](const ValueFlowOperand& operand) {
            m_context.connect(operand, target);
        });
    }

    bool mayLieIn(BuiltinOperand position, ValueSetID mask, bool admitsAbsence)
    {
        auto mayHold = [&](const ValueFlowOperand& operand) {
            return m_context.meet(m_context.valueOf(operand), mask);
        };

        switch (position.kind()) {
        case BuiltinOperand::Kind::Receiver:
            return mayHold(isPassed(m_call.thisValue) ? m_call.thisValue : m_undefined);
        case BuiltinOperand::Kind::Argument: {
            unsigned index = position.argumentIndex();
            if (index < m_call.arguments.size() && (mayHold(m_call.arguments[index]) || !m_call.argumentsMayBeAbsent))
                return mayHold(m_call.arguments[index]);
            return admitsAbsence || mayHold(m_call.otherArguments) || (index >= m_call.arguments.size() && mayHold(m_call.laterArguments));
        }
        case BuiltinOperand::Kind::ArgumentsFrom:
            return m_call.argumentsMayBeAbsent || std::ranges::all_of(m_call.arguments.subspan(std::min<size_t>(position.argumentIndex(), m_call.arguments.size())), mayHold);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const Value* argumentSpelledOutAt(BuiltinOperand position) const
    {
        if (!m_call.operation || position.kind() != BuiltinOperand::Kind::Argument || position.argumentIndex() >= m_call.arguments.size())
            return nullptr;
        const Function& function = m_program.at(m_call.operation->function);
        std::span<const ValueID> arguments = function.at(m_call.operation->site).variadicOperands();
        if (position.argumentIndex() >= arguments.size())
            return nullptr;
        return &function.at(definitionThroughUpcasts(function, arguments[position.argumentIndex()]));
    }

    bool isIntegerLiteralWithin(BuiltinOperand position, BuiltinIntegerRange range) const
    {
        const Value* argument = range.isEmpty() ? nullptr : argumentSpelledOutAt(position);
        return argument && argument->opcode == Opcode::Integer && argument->integer() >= range.minimum && argument->integer() <= range.maximum;
    }

    ValueSetID allocatedByBuiltinsAs(BuiltinOperand position, ValueSetID classes, bool hasPrototypeOfClass) { return classes == ValueSets::nothing ? ValueSets::nothing : objectsAllocatedByBuiltinsAs(m_context, valueAt(position), classes, hasPrototypeOfClass); }

    void noteAllocationAs(BuiltinClassID builtinClass, ValueFlowObjectID object)
    {
        ValueFlowObjectID standing = builtinClass ? m_tables.objectStandingFor(builtinClass) : ValueFlowObjectID();
        m_context.add(m_context.written(only(m_context, object), allocatedAsName), only(m_context, standing ? standing : object));
    }

    ValueSetID receiversRefusedBelow(BuiltinDispatchTarget target)
    {
        if (target.isOverload()) {
            const BuiltinParameter* receiver = m_builtins.at(target.overload).receiver;
            return receiver && receiver->refused ? m_tables.membersSurelyOf(receiver->refused) : ValueSets::nothing;
        }
        ValueSetID refused = ValueSets::anything;
        for (const BuiltinDispatchBranch& branch : m_builtins.at(target.node).branches)
            refused = m_context.maker().intersect(refused, receiversRefusedBelow(branch.target));
        return refused;
    }

    void select(BuiltinDispatchTarget target, Vector<BuiltinOverloadID, 4>& selected)
    {
        if (target.isOverload()) {
            selected.appendIfNotContains(target.overload);
            return;
        }

        const BuiltinDispatchNode& node = m_builtins.at(target.node);
        bool isRest = node.operand.kind() == BuiltinOperand::Kind::ArgumentsFrom;
        ValueSetID stillTested = ValueSets::complement(m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Absent)));
        bool absenceIsStillTested = true;
        for (const BuiltinDispatchBranch& branch : node.branches) {
            bool admitsAbsence = m_builtins.at(branch.tested).isAnything() || m_builtins.at(branch.tested).primitives.contains(BuiltinPrimitive::NotPassed);
            ValueSetID tested = m_context.maker().intersect(m_tables.maskOf(branch.tested), stillTested);
            if (node.operand == BuiltinOperand::receiver()) {
                ValueSetID refused = m_context.maker().intersect(tested, receiversRefusedBelow(branch.target));
                if (refused != ValueSets::nothing && mayLieIn(node.operand, refused, false))
                    throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));
                tested = m_context.maker().intersect(tested, ValueSets::complement(refused));
            }
            if (mayLieIn(node.operand, tested, admitsAbsence && absenceIsStillTested))
                select(branch.target, selected);
            if (isRest)
                continue;
            stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(m_tables.membersSurelyOf(branch.tested)));
            stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(allocatedByBuiltinsAs(node.operand, m_tables.classesSurelyOf(branch.tested), false)));
            if (isIntegerLiteralWithin(node.operand, m_builtins.at(branch.tested).integers))
                stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Integer))));
            absenceIsStillTested &= !admitsAbsence;

            ValueSetID whilePristine = m_context.maker().unite(m_tables.membersOfWhilePristine(branch.tested), allocatedByBuiltinsAs(node.operand, m_tables.classesOfWhilePristine(branch.tested), true));
            if (whilePristine == ValueSets::nothing)
                continue;
            OptionSet<ObjectMutation> mutations { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
            bool testsNames = std::ranges::any_of(m_builtins.at(branch.tested).parts, [](const BuiltinTypePart& part) {
                return part.pristine.containsAny({ PristineAspects::Names, PristineAspects::AsCreated });
            });
            if (testsNames)
                mutations.add(ObjectMutation::AddProperty);
            ClimbingSet changed = m_context.markedAmong(m_context.restrict(valueAt(node.operand), whilePristine), mutations);
            if (!m_context.meet(changed, ValueSets::anything))
                stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(whilePristine));
        }
    }

    bool isGetterOfEngineWithoutRow(const BuiltinOverload& overload) const
    {
        const Builtin& entry = m_builtins.at(m_builtin);
        return overload.isUndeclared() && overload.mode == BuiltinCallMode::Get && entry.kind == BuiltinKind::Accessor && entry.origin == BuiltinOrigin::Engine;
    }

    void noteWhatIsCalledBy(const BuiltinOverload& overload)
    {
        if (isGetterOfEngineWithoutRow(overload))
            return;
        if (overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
            noteCallThatRunsOtherCode(m_builtin, m_context);
        if (RuntimeFunctionID function = functionThatSaysWhatCallDoes(overload)) {
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_program, function)) {
                enter(entry->function);
                return;
            }
        }
        if (originOfOperands() != ValueFlowCalleeOrigin::Value)
            return;

        actWithoutResultOf(overload);
        bool callsValueNobodyKnows = overload.isUndeclared()
            || std::ranges::any_of(overload.flow.callbacks, [&](const BuiltinCallback& callback) { return mayBeUnknownObject(valueAt(callback.callee)); })
            || std::ranges::any_of(overload.flow.handlerCalls, [&](const BuiltinHandlerCall& handlerCall) { return mayBeUnknownObject(valueAt(handlerCall.callee)); });
        if (callsValueNobodyKnows)
            letWhateverHasEscapedBeCalled(m_context);
    }

    void readSpeciesFor(const BuiltinOverload& overload)
    {
        if (overload.code.model || !doRecordsDescribe(m_builtins, overload))
            return;
        actWithoutResultOf(overload);
        m_objectsMadeBySpecies = { };
        auto read = [&](const BuiltinParameter& parameter) {
            if (parameter.uses != OperandUses::all() && parameter.protocols.contains(Protocol::Species))
                constructSpecies(parameter.operand);
        };
        if (overload.receiver)
            read(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            read(parameter);
    }

    // A receiver that the overload does not accept makes the call throw. All that the call does with it is to describe it in the error. A data property asks nothing of the receiver that finds it.
    ValueSetID receiversAcceptedBy(const BuiltinOverload& overload) const
    {
        if (isConstruction(m_call.kind) || overload.isUndeclared() || m_builtins.at(m_builtin).kind == BuiltinKind::DataProperty)
            return ValueSets::anything;
        return m_tables.acceptedValues(overload.receiver);
    }

    // Some native functions convert their arguments before they check the receiver.
    void convertArguments(const BuiltinOverload& overload)
    {
        for (const BuiltinParameter& parameter : overload.parameters) {
            ValueFlowBehaviorID behavior = m_tables.behaviorOf(parameter);
            if (!behavior || parameter.conversion == BuiltinConversion::None)
                continue;
            connectOperands(parameter.operand, m_context.hub(behavior));
            throwValue(unknownValue(m_context, ValueFlowUnknownSource::ThrownByRunTime));
        }
    }

    void apply(const BuiltinOverload& overload)
    {
        SetForScope receiversAccepted(m_receiversAccepted, receiversAcceptedBy(overload));
        if (m_receiversAccepted != ValueSets::anything && !m_context.meet(valueAt(BuiltinOperand::receiver()), ValueSets::anything)) {
            if (overload.receiver->roles.contains(BuiltinOperandRole::DescribedWhenRefused))
                describeRefusedOperands(*overload.receiver);
            throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));
            convertArguments(overload);
            return;
        }
        if (isGetterOfEngineWithoutRow(overload)) {
            answer(undescribedValue());
            throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));
            return;
        }
        noteClassesOfResult(overload);
        if (overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
            noteCallThatRunsOtherCode(m_builtin, m_context);
        bool makesPairs = m_builtins.canonicalID(m_builtin) == Builtins::ObjectEntries && !overload.isUndeclared();
        if (m_builtins.canonicalID(m_builtin) == Builtins::PrivateGlobalsToClass && !overload.isUndeclared()) {
            makeClass();
            return;
        }
        if (RuntimeFunctionID function = makesPairs ? RuntimeFunctionID() : functionThatSaysWhatCallDoes(overload)) {
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_program, function)) {
                enter(entry->function);
                applyReadsIntoResult(overload);
                if (m_builtins.canonicalID(m_builtin) == Builtins::ObjectFromEntries)
                    listNamedBunOnResult(BuiltinOperand::argument(0));
                for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                    if (action.kind == BuiltinPropertyActionKind::DescribesOwnProperties)
                        listNamedBunOnResult(action.object);
                }
                return;
            }
        }

        m_overload = &overload;
        m_handsOnResult = handsOnResult(overload);
        m_resultObject = m_call.resultObject ? m_call.resultObject : m_universe.resultOf(m_builtin, 0);
        if (BuiltinID canonical = m_builtins.canonicalID(m_builtin); (canonical == Builtins::Proxy && isConstruction(m_call.kind)) || canonical == Builtins::ProxyRevocable) {
            makeProxy();
            return;
        }
        if (mayAnswerCallableObject(overload))
            m_resultObject = { };
        m_contentsObject = m_call.contentsObject ? m_call.contentsObject : m_universe.resultOf(m_builtin, 1);
        m_objectsMadeBySpecies = { };
        if (makesPairs) {
            makeEntries();
            return;
        }
        if (overload.receiver)
            applyParameter(*overload.receiver);
        else if (overload.isUndeclared()) {
            handToUndescribedNativeCode(BuiltinOperand::receiver());
        }
        for (const BuiltinParameter& parameter : overload.parameters)
            applyParameter(parameter);
        if (!takesKeysAndValuesFromPairs()) {
            for (const BuiltinTransfer& transfer : overload.flow.transfers)
                connect(transfer.from, transfer.to);
        }
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            applyCallback(callback);
            if (callback.afterwards == BuiltinCallbackAftermath::WritesIntoWhatItFinds)
                letUnknownCodeWrite(m_context);
        }
        for (const BuiltinHandlerCall& handlerCall : overload.flow.handlerCalls)
            applyHandlerCall(handlerCall);
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
            applyPropertyAction(action);
        if (!overload.isUndeclared()) {
            listNamedBunAmongSettledValues();
            readMemberUnderKeyOfMap();
        }
        m_plainDataOfResult = { };
        if (ValueFlowNodeID result = nodeTakingResult())
            m_context.add(result, answersObjectsOfNativeCell(overload) ? m_tables.primitivesOf(overload.result.type) : valueOfType(overload.result.type, m_resultObject));
        if (m_call.operation)
            allocateTypedResult(overload, *m_call.operation, m_call, m_resultObject, m_plainDataOfResult, m_context);
        if (const BuiltinType& answer = m_builtins.at(overload.result.type); isConstruction(m_call.kind) && (answer.isAnything() || answer.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction })))
            m_context.connect(m_context.lookup(m_context.restrict(m_context.valueOf(m_call.thisValue), ValueSets::complement(m_universe.builtinObjects())), property(m_context, BuiltinNames::prototype)), escaped());

        if (overload.effects.capability == BuiltinCapability::JS || runsOperandCode(overload))
            throwValue(unknownValue(m_context, ValueFlowUnknownSource::ThrownByRunTime));
        else if (overload.throws())
            throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));

        const Builtin& entry = m_builtins.at(m_builtin);
        if (overload.isUndeclared() && entry.origin == BuiltinOrigin::Host && !entry.flags.contains(BuiltinFlag::IsCompilerHelper))
            letUndeclaredNativeCodeRun(m_context);
    }

    void makeClass()
    {
        std::span<const ValueFlowOperand> arguments = m_call.arguments;
        if (arguments.size() >= 3) {
            const ValueFlowOperand& function = arguments[0];
            const ValueFlowOperand& base = arguments[2];
            ValueFlowName prototype = property(m_context, BuiltinNames::prototype);
            BuiltinPropertyAction change { .kind = BuiltinPropertyActionKind::ChangesPrototype, .object = BuiltinOperand::argument(0) };
            Typed::applyPropertyAction(m_context, change, { function, { }, base, { }, m_call.thrown });
            Typed::applyPropertyAction(m_context, change, { operandOfBuiltinAt(m_context.lookup(m_context.valueOf(function), prototype)), { }, operandOfBuiltinAt(m_context.lookup(m_context.valueOf(base), prototype)), { }, m_call.thrown });
        }
        answer(m_universe.undefinedValue());
        throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));
    }

    void makeEntries()
    {
        ClimbingSet array = only(m_context, m_resultObject);
        ClimbingSet pair = only(m_context, m_contentsObject);
        for (ValueFlowObjectID object : { m_resultObject, m_contentsObject }) {
            noteAllocationAs(BuiltinClasses::Array, object);
            m_context.add(m_context.written(only(m_context, object), prototypeLinkName), only(m_context, m_universe.objectOf(Builtins::ArrayPrototype)));
            writeNamesDefinedOnNewInstance(BuiltinClasses::Array, object);
        }
        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::PairsOfEntries), pair);
        m_context.add(m_context.written(array, elementsName), pair);
        BuiltinPlace ownValues { .kind = BuiltinPlaceKind::OwnValues, .operand = BuiltinOperand::argument(0) };
        for (ValueFlowName name : std::initializer_list<ValueFlowName> { elementsName, keyOfEntriesPair })
            m_context.add(m_context.written(pair, name), m_universe.strings());
        for (ValueFlowName name : std::initializer_list<ValueFlowName> { elementsName, valueOfEntriesPair })
            connectFrom(ownValues, m_context.written(pair, name));
        connectOperands(BuiltinOperand::argument(0), m_context.written(pair, sourcesOfEntriesPair));
        answer(m_context.maker().of(m_resultObject));
        throwValue(unknownValue(m_context, ValueFlowUnknownSource::ThrownByRunTime));
    }

    bool takesKeysAndValuesFromPairs()
    {
        if (!isConstruction(m_call.kind) || m_builtins.canonicalID(m_builtin) != Builtins::Map || !m_resultObject)
            return false;
        ClimbingSet entries = m_context.restrict(valueAt(BuiltinPlace { .kind = BuiltinPlaceKind::IteratedValues, .operand = BuiltinOperand::argument(0) }), m_universe.objects());
        constexpr OptionSet<ObjectMutation> mutationsOfContents { ObjectMutation::StoreAfterCreation, ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::DeleteField, ObjectMutation::ChangePrototype };
        bool someEntryIsOtherwise = holdsAnything(m_context.membersMissingFrom(m_context.nodes().ofHost(ValueFlowHostNode::PairsOfEntries), entries))
            || m_context.holdsAnyOf(entries, escaped()) || m_context.holdsAnyOf(entries, ValueFlowProgramNodes::lost()) || holdsAnything(m_context.markedAmong(entries, mutationsOfContents));
        if (someEntryIsOtherwise) {
            m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::MapsFilledAsRecordsSay), only(m_context, m_resultObject));
            return false;
        }
        m_context.add(m_context.written(only(m_context, m_resultObject), entriesPairsOfMap), entries);
        for (const BuiltinTransfer& transfer : m_overload->flow.transfers) {
            RELEASE_ASSERT(transfer.to.kind == BuiltinPlaceKind::ResultSlot);
            forEachTargetAt(transfer.to, [&](ValueFlowNodeID target) {
                m_context.connect(m_context.read(entries, transfer.to.slot == BuiltinSlots::MapKey ? keyOfEntriesPair : valueOfEntriesPair), target);
            });
        }
        return true;
    }

    void makeProxy()
    {
        throwValue(unknownObjectSet(m_context, ValueFlowUnknownSource::ThrownByRunTime));
        if (!m_call.result)
            return;
        connectOperands(BuiltinOperand::argument(1), escaped());
        // Proxy.revocable answers an object that holds the proxy.
        bool answersProxy = m_builtins.canonicalID(m_builtin) == Builtins::Proxy;
        ValueFlowNodeID proxy = answersProxy ? m_call.result : m_context.written(only(m_context, m_resultObject), unknownKeyName);
        forEachOperandAt(BuiltinOperand::argument(0), [&](const ValueFlowOperand& target) {
            carryOut(m_context, target);
            if (ValueFlowNodeID exports = exportsOfHoldersIn(m_context, target))
                m_context.connect(exports, escaped());
            // A name read on the proxy is read on its target.
            if (ValueFlowNodeID listedBun = findNodeOfListedBun(m_context, target))
                m_context.connect(listedBun, proxy);
        });
        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.restrict(objectsAt(BuiltinOperand::argument(0)), m_universe.objectsOfRealm()));
        for (BuiltinID trap : m_builtins.membersOf(Builtins::Reflect)) {
            m_context.forEachMember(m_context.restrict(escapedMembersUnder(m_context, m_program.keyOf(m_builtins.at(trap).name)), m_universe.builtinFunctions()), [&](ValueFlowObjectID function) {
                noteCallOfBuiltin(m_universe.builtinOf(function), ValueFlowCallKind::CallByNativeCode, m_context);
            });
        }
        m_context.mark(objectsAt(BuiltinOperand::argument(0)), ObjectMutation::RedefineProperty);
        m_context.add(proxy, undescribedObject());
        // The object of the call goes beside it, for whoever predicts where a proxy of the program arrives. It has no layout and no class, so every rule takes it for an object of which nothing is known.
        ValueFlowObjectID made = answersProxy ? m_resultObject : m_call.contentsObject ? m_call.contentsObject : m_universe.resultOf(m_builtin, 1);
        noteAllocationAs({ }, made);
        m_context.add(proxy, only(m_context, made));
        if (!answersProxy) {
            ClimbingSet pair = only(m_context, m_resultObject);
            m_context.add(m_context.written(pair, prototypeLinkName), only(m_context, m_universe.objectOf(Builtins::ObjectPrototype)));
            noteAllocationAs(BuiltinClasses::Object, m_resultObject);
            m_context.add(m_call.result, pair);
        }
        markWhereNameReadOnProxyMayAnswerMemberLeftOut(proxy);
        m_context.connect(m_context.masked(escaped(), m_context.maker().of(ValueFlowUniverse::markOfLoadedBun())), proxy);
        m_context.add(proxy, m_context.restrict(objectsAt(BuiltinOperand::argument(0)), m_context.maker().unite(m_tables.heap().objectsThatLetInUnseenCode(), m_context.maker().of(m_universe.objectOf(Builtins::Bun)))));
    }

    // A name read on the proxy is read on its target, or it is what the get trap of the handler answers. The handler has escaped, and so has what its traps answer.
    void markWhereNameReadOnProxyMayAnswerMemberLeftOut(ValueFlowNodeID proxy)
    {
        ValueSetID mark = m_context.maker().of(ValueFlowUniverse::markOfMemberLeftOut());
        ValueSetID modules = m_context.maker().intersect(m_tables.heap().objectsOfPublicModules(), ValueSets::complement(m_context.maker().of(m_universe.objectOf(Builtins::Bun))));
        m_context.forEachMember(m_context.restrict(objectsAt(BuiltinOperand::argument(0)), modules), [&](ValueFlowObjectID module) {
            ValueSetID leftOut = m_tables.heap().membersOf(m_universe.builtinOf(module)).leftOut;
            if (leftOut == ValueSets::nothing)
                return;
            m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), leftOut);
            m_context.add(proxy, mark);
        });
        m_context.connect(m_context.masked(escaped(), mark), proxy);
    }

    void noteClassesOfResult(const BuiltinOverload& overload)
    {
        Vector<ValueFlowObjectID, 16> holdersOfRows;
        auto appendHoldersOf = [&](BuiltinClassID builtinClass) {
            for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
                for (BuiltinID object : { m_builtins.at(current).constructor, m_builtins.at(current).prototype }) {
                    if (object)
                        holdersOfRows.append(m_universe.objectOf(object));
                }
            }
        };

        Vector<BuiltinTypeID, 8> types { overload.result.type };
        for (size_t index = 0; index < types.size(); ++index) {
            for (const BuiltinTypePart& part : m_builtins.at(types[index]).parts) {
                appendHoldersOf(part.builtinClass);
                for (const BuiltinSlotContents& contents : part.contents)
                    types.appendIfNotContains(contents.type);
            }
        }

        bool isCall = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Construct;
        bool runsOtherCode = overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
        bool answersAnything = m_builtins.at(overload.result.type).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject });
        if (m_builtins.at(m_builtin).origin == BuiltinOrigin::Host && (isCall || overload.isUndeclared()) && runsOtherCode && answersAnything)
            appendHoldersOf(BuiltinClasses::Promise);

        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins), m_context.maker().of(holdersOfRows.span()));
        noteInstancesIn(m_context, overload.result.type);
        if (overload.isUndeclared() && m_builtins.at(m_builtin).origin == BuiltinOrigin::Host)
            m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances), m_tables.heap().prototypeObjectsOfHost());
    }

    bool mayHaveProperty(ClimbingSet holders, BuiltinName name)
    {
        PropertyKeyID key = m_program.keyOf(name);
        ClimbingSet chain = m_context.valueOf(m_context.lookup(m_context.restrict(holders, m_universe.objects()), prototypeLinkName));
        if (mayBeUnknownObject(chain) || mayBeUntrackedObject(chain))
            return true;
        bool isWritten = false;
        for (ValueFlowName written : { ValueFlowName::property(m_program, key), ValueFlowName::getter(m_program, key), ValueFlowName::setter(m_program, key) }) {
            forEachNodeReadThrough(chain, written, [&](ValueFlowNodeID node) {
                isWritten |= holdsAnything(m_context.valueOf(node));
            });
        }
        return isWritten || m_context.meet(m_context.valueOf(m_context.lookup(holders, ValueFlowName::property(m_program, key))), ValueSets::complement(m_universe.undefinedValue()));
    }

    bool hasFieldUnderName(ClimbingSet holders, BuiltinName name)
    {
        PropertyKeyID key = m_program.keyOf(name);
        bool hasField = false;
        m_context.forEachMember(Typed::withSourcesOfCopies(m_context, known(holders)), [&](ValueFlowObjectID object) {
            LayoutID layout = m_universe.layoutOf(object);
            hasField |= layout && std::ranges::contains(m_program.at(layout).fields, key, &Field::key);
        });
        return hasField;
    }

    bool mayHaveText(const BuiltinCondition& condition) const
    {
        const Value* argument = argumentSpelledOutAt(condition.operand);
        if (!argument || argument->opcode != Opcode::String)
            return true;
        std::span<const uint8_t> text = m_program.strings[argument->string()];
        auto isWholeText = [&](BuiltinName listed) {
            return std::ranges::equal(text, m_builtins.textOf(listed));
        };
        auto isStartOfText = [&](BuiltinName listed) {
            std::span<const uint8_t> start = m_builtins.textOf(listed);
            return text.size() >= start.size() && std::ranges::equal(text.first(start.size()), start);
        };
        return std::ranges::any_of(condition.isOneOf, isWholeText) || std::ranges::any_of(condition.startsWith, isStartOfText);
    }

    bool mayBeCalledOn(BuiltinID entry)
    {
        if (!entry)
            return true;
        Vector<ValueFlowObjectID, 32> objectsOfEntry;
        m_universe.appendObjectsOf(entry, objectsOfEntry);
        ClimbingSet objects(m_context.maker().of(objectsOfEntry.span()));
        bool listsEntry = false;
        forEachOperandAt(BuiltinOperand::receiver(), [&](const ValueFlowOperand& receiver) {
            listsEntry |= (receiver.constant && m_context.meet(objects, receiver.constant)) || (receiver.node && m_context.holdsAnyOf(objects, receiver.node));
        });
        if (listsEntry)
            return true;
        return mayBeUntrackedObject(valueAt(BuiltinOperand::receiver())) && m_context.holdsAnyOf(objects, ValueFlowProgramNodes::lost());
    }

    void notePropertiesThatRowsAskFor()
    {
        auto noteIn = [&](BuiltinName cell) {
            m_context.add(m_context.written(only(m_context, m_universe.objectOf(m_builtin)), nameOfNativeCell(m_context, cell)), m_universe.undefinedValue());
        };
        BuiltinID builtin = m_builtins.canonicalID(m_builtin);
        const BuiltinUnseenCode* row = m_builtins.unseenCodeOf(builtin);
        BuiltinName propertyThatRunsUnseenCode = row && row->condition ? row->condition->hasProperty : BuiltinName();
        auto mayHave = [&](const BuiltinCondition& condition) {
            ClimbingSet holders = valueAt(condition.operand);
            return condition.hasProperty == propertyThatRunsUnseenCode ? hasFieldUnderName(holders, condition.hasProperty) : mayHaveProperty(holders, condition.hasProperty);
        };
        auto note = [&](const BuiltinCondition* condition) {
            if (!condition)
                return;
            if (condition->hasProperty && mayHave(*condition))
                noteIn(condition->hasProperty);
            if (condition->firstText() && mayHaveText(*condition) && mayBeCalledOn(condition->calledOn))
                noteIn(condition->firstText());
        };
        if (row)
            note(row->condition);
        for (const BuiltinModuleLoad& load : m_builtins.moduleLoadsOf(builtin))
            note(load.condition);
        for (const BuiltinCalledFunction& called : m_builtins.calledFunctionsOf(builtin))
            note(called.condition);
        for (const BuiltinFeatureUse& use : m_builtins.featureUsesOf(builtin))
            note(use.condition);
    }

    bool isCalledInWaysNobodyFollows(const BuiltinTypePart& part) const { return part.kind == BuiltinTypePartKind::Instance && part.builtinClass != BuiltinClasses::BoundFunction && isFunctionClass(m_builtins, part.builtinClass); }

    static bool answersObjectsOfNativeCell(const BuiltinOverload& overload)
    {
        return overload.result.type && std::ranges::any_of(overload.flow.transfers, [](const BuiltinTransfer& transfer) {
            return transfer.from.kind == BuiltinPlaceKind::NativeCell && transfer.to.kind == BuiltinPlaceKind::Result;
        });
    }

    bool mayAnswerCallableObject(const BuiltinOverload& overload) const
    {
        return std::ranges::any_of(m_builtins.at(overload.result.type).parts, [&](const BuiltinTypePart& part) {
            return isCalledInWaysNobodyFollows(part);
        });
    }

    RuntimeFunctionID functionThatSaysWhatCallDoes(const BuiltinOverload& overload) const
    {
        if (overload.code.model && !isConstruction(m_call.kind))
            return overload.code.model;
        return doRecordsDescribe(m_builtins, overload) ? RuntimeFunctionID() : overload.libraryFunction();
    }

    void applyReadsIntoResult(const BuiltinOverload& overload)
    {
        if (overload.isUndeclared() || overload.code.model)
            return;
        actWithoutResultOf(overload);
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            if (action.kind == BuiltinPropertyActionKind::ReadsByKey && action.value.kind == BuiltinPlaceKind::Result)
                applyPropertyAction(action);
        }
    }

    void enter(FunctionID function)
    {
        passOperandsOf(m_call, m_context.group(only(m_context, m_universe.closureOf(function)), m_call.kind), m_context);
        if (handsOverUnknownObject() && isCodeThatRecordsDescribe(m_universe, function))
            letUnknownCodeWrite(m_context);
    }

    bool handsOverUnknownObject()
    {
        auto mayBeUnknown = [&](const ValueFlowOperand& operand) {
            return mayBeUnknownObject(m_context.valueOf(operand));
        };
        return mayBeUnknown(m_call.thisValue) || std::ranges::any_of(m_call.arguments, mayBeUnknown) || mayBeUnknown(m_call.laterArguments) || mayBeUnknown(m_call.otherArguments);
    }

    void handToUndescribedNativeCode(BuiltinOperand operand)
    {
        bool isCalledOnItsHolder = operand.isReceiver() && m_builtins.at(m_builtin).holder != Builtins::NativeFunctions;
        ValueSetID handed = isCalledOnItsHolder ? ValueSets::complement(m_universe.builtinObjects()) : ValueSets::anything;
        forEachOperandAt(operand, [&](const ValueFlowOperand& passed) {
            if (operand.isReceiver())
                m_context.add(escaped(), m_context.restrict(m_context.valueOf(passed), handed));
            else
                carryOut(m_context, passed);
        });
        if (m_context.meet(valueAt(operand), m_context.maker().intersect(m_universe.objects(), handed))) {
            letUnknownCodeWrite(m_context);
            letWhateverHasEscapedBeCalled(m_context);
        }
        answer(undescribedValue());
        throwValue(undescribedValue());
    }

    // A method of a host class that was found on its receiver refuses an object that stands below the holder of the method or has the method stored on its chain: no other object finds it.
    // What stands below an object nobody knows may find it there, and is in the hands of native code.
    void describeReceiversThatAnswer(ValueSetID refused, ValueFlowNodeID description)
    {
        ValueSetMaker& maker = m_context.maker();
        auto objectsOf = [&](BuiltinID builtin) {
            Vector<ValueFlowObjectID, 32> objects;
            if (builtin)
                m_universe.appendObjectsOf(builtin, objects);
            return maker.of(objects.span());
        };
        BuiltinID method = m_builtins.canonicalID(m_builtin);
        ValueSetID holders = objectsOf(m_builtins.at(method).holder);
        ValueSetID methods = objectsOf(method);
        ValueSetID objectsOfProgram = maker.intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
        ValueSetID objectsNobodyKnows = ValueSets::complement(m_universe.knownObjects());
        auto answers = [&](ClimbingSet chain) {
            return m_context.meet(chain, holders) || m_context.meet(m_context.valueOf(m_context.readWhatWasWritten(m_context.restrict(chain, objectsOfProgram), everyPropertyName)), methods);
        };

        ClimbingSet receivers = m_context.restrict(valueAt(BuiltinOperand::receiver()), maker.intersect(refused, m_universe.knownObjects()));
        ClimbingSet chainOfAll = m_context.valueOf(m_context.lookup(receivers, prototypeLinkName));
        if (!answers(chainOfAll) && !m_context.meet(chainOfAll, objectsNobodyKnows))
            return;
        ValueFlowObjectsToldApart apart = tellObjectsApart(m_context, receivers);
        if (apart.rest != ValueSets::nothing)
            apart.groups.append(apart.rest);
        for (ValueSetID group : apart.groups) {
            ClimbingSet chain = m_context.valueOf(m_context.lookup(ClimbingSet(group), prototypeLinkName));
            if (answers(chain))
                m_context.add(description, group);
            if (m_context.meet(chain, objectsNobodyKnows))
                m_context.add(escaped(), group);
        }
    }

    // A function is described by its name, and an object of a class that the parameter takes is not refused. Of any other object that it takes, native code may refuse what it reads out of it.
    void describeRefusedOperands(const BuiltinParameter& parameter)
    {
        SetForScope everyReceiver(m_receiversAccepted, ValueSets::anything);
        ValueFlowNodeID description = m_context.hub(m_tables.descriptionOfRefusedOperands());
        bool takesAnyObject = !parameter.accepted || m_builtins.at(parameter.accepted).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData });
        if (!takesAnyObject) {
            ValueSetID refused = ValueSets::complement(m_context.maker().unite(m_universe.callables(), m_tables.membersSurelyOf(parameter.accepted)));
            if (parameter.operand.isReceiver() && m_call.isFoundOnReceiver)
                return describeReceiversThatAnswer(refused, description);
            forEachOperandAt(parameter.operand, [&](ValueFlowOperand operand) {
                operand.mask = m_context.maker().intersect(operand.mask, refused);
                m_context.connect(operand, description);
            });
            return;
        }

        ValueSetID allButFunctions = ValueSets::complement(m_universe.callables());
        ValueFlowBehaviorID behavior = m_tables.behaviorOf(parameter);
        if (behavior && m_tables.at(behavior).isTransitive)
            m_context.connect(m_context.masked(m_context.hub(behavior), allButFunctions), description);
        else {
            for (BuiltinName name : parameter.namedMembers)
                m_context.connect(m_context.masked(m_context.lookup(valueAt(parameter.operand), property(m_context, name)), allButFunctions), description);
        }
        if (parameter.uses.contains(OperandUse::ReadsContents)) {
            forEachNodeReadThrough(objectsAt(parameter.operand), everyPropertyName, [&](ValueFlowNodeID contents) {
                m_context.connect(m_context.masked(contents, allButFunctions), description);
            });
            m_context.connect(m_context.masked(m_context.lookup(m_context.restrict(known(objectsAt(parameter.operand)), ValueSets::complement(m_universe.builtinObjects())), elementsName), allButFunctions), description);
        }
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
            if (parameter.uses.contains(OperandUse::AppliesToContents) && !parameter.uses.contains(OperandUse::AppliesTransitively)) {
                forEachNodeReadThrough(objectsAt(parameter.operand), everyPropertyName, [&](ValueFlowNodeID contents) {
                    m_context.connect(contents, hub);
                });
                m_context.connect(m_context.lookup(m_context.restrict(known(objectsAt(parameter.operand)), ValueSets::complement(m_universe.builtinObjects())), elementsName), hub);
            } else if (m_context.runsCodeThatRecordsDescribe()) {
                forEachOperandAt(parameter.operand, [&](ValueFlowOperand operand) {
                    operand.mask = m_context.maker().intersect(operand.mask, everythingButUnknownObject(m_context));
                    m_context.connect(operand, hub);
                });
            } else
                connectOperands(parameter.operand, hub);
            if (parameter.uses.contains(OperandUse::AppliesTransitively) && parameter.protocols.contains(Protocol::OwnValues)) {
                ClimbingSet hostObjects = withHostObjectsAmongMembers(m_context, m_context.restrict(valueAt(parameter.operand), m_tables.heap().holdersOfMembersThatHostMakes()));
                forEachNodeReadByWalkOver(m_context, hostObjects, parameter.protocols, [&](ValueFlowNodeID members) {
                    m_context.connect(members, hub);
                });
            }
        }
        // A retired node takes no prototype object of a library class. The node of the operand hands on those that come to it, whether before it is retired or after.
        if (m_overload->code.implementation.inlineBuiltin == InlineBuiltin::OwnPropertyKeys && parameter.protocols.contains(Protocol::OwnKeys))
            connectOperands(parameter.operand, m_context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfLibraryClassesWhoseOwnKeysAreListed));
        if (parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused))
            describeRefusedOperands(parameter);
        if (parameter.protocols.contains(Protocol::Species))
            constructSpecies(parameter.operand);

        if (parameter.protocols.containsAny({ Protocol::Named, Protocol::CallsNamed }) && mayReturnUnknownObject(m_builtins.at(m_overload->result.type))) {
            ClimbingSet holders = valueAt(parameter.operand);
            for (BuiltinName name : parameter.namedMembers)
                m_context.connect(m_context.lookup(holders, property(m_context, name)), escaped());
            for (BuiltinName name : parameter.calledMembers) {
                forEachPartOfCallees(m_context, m_context.restrict(m_context.valueOf(m_context.lookup(holders, property(m_context, name))), m_universe.callables()), [&](ClimbingSet callees) {
                    m_context.connect(m_context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName).result, escaped());
                });
            }
        }
    }

    bool isCalledByProgramCode() const
    {
        FunctionID function = m_call.operation && m_call.operation->function ? m_call.operation->function : m_context.functionOfStatement();
        if (!function)
            return false;
        FunctionRole role = m_program.at(function).role;
        return role != FunctionRole::Library && role != FunctionRole::Model;
    }

    void constructSpecies(BuiltinOperand operand)
    {
        ValueFlowNodeID found = m_context.lookup(valueAt(operand), property(m_context, BuiltinNames::constructor));
        ClimbingSet constructors = m_context.valueOf(found);
        ClimbingSet species = m_context.unite(constructors, m_context.valueOf(m_context.lookup(constructors, property(m_context, BuiltinNames::speciesSymbol))));
        ClimbingSet answeredToClassesOfProgram = m_context.valueOf(m_context.lookup(m_context.restrict(constructors, ValueSets::complement(m_universe.builtinObjects())), property(m_context, BuiltinNames::speciesSymbol)));
        bool isReturned = m_builtins.at(m_overload->result.type).freshness == BuiltinFreshness::UnlessSpecies;

        ValueSetID followed = m_context.maker().unite(m_universe.closures(), m_universe.otherCallables());
        followed = m_context.maker().unite(followed, m_universe.untrackedObjectAlone());
        // An object of the program that has escaped stands in the node beside the object nobody knows, and so does what it holds.
        ClimbingSet answeredBuiltinFunctions = m_context.restrict(answeredToClassesOfProgram, m_universe.builtinFunctions());
        ValueSetID objectsOfProgram = m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
        if (std::optional<ClimbingSet> holders = m_context.holdersUnder(property(m_context, BuiltinNames::speciesSymbol), ClimbingSet(objectsOfProgram))) {
            ValueFlowNodeID held = m_context.readWhatWasWritten(m_context.valueWithin(*holders, found), property(m_context, BuiltinNames::speciesSymbol));
            answeredBuiltinFunctions = m_context.unite(answeredBuiltinFunctions, m_context.valueWithin(ClimbingSet(m_universe.builtinFunctions()), held));
        }
        Vector<ValueFlowObjectID, 4> raisingEvents;
        m_context.forEachMember(answeredBuiltinFunctions, [&](ValueFlowObjectID constructor) {
            if (raisesEventWhenCalled(m_builtins, m_universe.builtinOf(constructor)))
                raisingEvents.append(constructor);
        });
        ClimbingSet keptByNativeCode;
        forEachPartOfCallees(m_context, m_context.unite(m_context.restrict(species, followed), ClimbingSet(m_context.maker().of(raisingEvents.span()))), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::ConstructByNativeCode);
            m_context.add(group.newTarget, callees);
            m_context.add(group.otherArguments, undescribedValue());
            if (m_call.thrown)
                m_context.connect(group.thrown, m_call.thrown);
            if (isReturned) {
                if (ValueFlowNodeID result = nodeTakingResult())
                    m_context.connect(group.result, result);
                m_objectsMadeBySpecies = m_context.unite(m_objectsMadeBySpecies, m_context.restrict(m_context.valueOf(group.result), m_universe.objects()));
            } else {
                carryOut(m_context, group.result);
                keptByNativeCode = m_context.unite(keptByNativeCode, m_context.restrict(m_context.valueOf(group.result), m_universe.objects()));
            }
        });
        if (ClimbingSet kept = known(keptByNativeCode); m_context.meet(kept, m_universe.objects())) {
            m_context.mark(kept, { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation, ObjectMutation::StoreOfOtherType });
            m_context.add(m_context.written(kept, unknownKeyName), unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        }
        auto findsSpeciesGetter = [&](BuiltinID constructor) {
            AOT_THRESHOLD(unsigned, longestChain, 4);
            for (unsigned step = 0; constructor && step < longestChain; ++step) {
                if (m_builtins.findOwnMember(constructor, BuiltinNames::speciesSymbol))
                    return true;
                const Builtin& entry = m_builtins.at(constructor);
                if (entry.prototypeLink != BuiltinPrototypeLink::Builtin)
                    return entry.prototypeLink == BuiltinPrototypeLink::Unknown;
                constructor = m_builtins.canonicalID(entry.inheritsFrom);
            }
            return !!constructor;
        };
        bool mayBeAnotherBuiltinConstructor = false;
        bool answersAnyObject = m_builtins.at(m_overload->result.type).flags.contains(BuiltinTypeFlag::HasAnyObject);
        m_context.forEachMember(m_context.restrict(species, answersAnyObject ? m_universe.builtinFunctions() : ValueSets::nothing), [&](ValueFlowObjectID constructor) {
            BuiltinID canonical = m_builtins.canonicalID(m_universe.builtinOf(constructor));
            mayBeAnotherBuiltinConstructor |= (m_context.contains(answeredToClassesOfProgram, constructor) || findsSpeciesGetter(canonical)) && std::ranges::none_of(m_builtins.at(m_overload->result.type).parts, [&](const BuiltinTypePart& part) {
                return part.kind == BuiltinTypePartKind::Instance && m_builtins.canonicalID(m_builtins.at(part.builtinClass).constructor) == canonical;
            });
        });
        if (isReturned && (mayBeAnotherBuiltinConstructor || m_context.contains(species, ValueFlowUniverse::unknownObject()))) {
            answer(unknownObjectSet(m_context, ValueFlowUnknownSource::ResultOfUnknownCallee));
            m_objectsMadeBySpecies = m_context.unite(m_objectsMadeBySpecies, unknownObjectAlone());
        }
        m_context.mark(m_objectsMadeBySpecies, { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });

        ClimbingSet made = known(m_objectsMadeBySpecies);
        if (m_context.meet(made, m_universe.objects())) {
            m_context.add(escaped(), made);
            m_context.mark(made, ObjectMutation::StoreOfOtherType);
            m_context.add(m_context.written(made, elementsName), unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
            m_context.add(m_context.written(made, property(m_context, BuiltinNames::length)), m_universe.anyPrimitive());
            bool mayHaveAccessorUnderIndex = std::ranges::any_of(std::array { ValueFlowNameKind::GetterUnderUnknownKey, ValueFlowNameKind::SetterUnderUnknownKey }, [&](ValueFlowNameKind kind) {
                return holdsAnything(m_context.valueOf(m_context.readWhatWasWritten(made, ValueFlowName::of(kind))));
            });
            if (mayHaveAccessorUnderIndex)
                m_context.mark(made, ObjectMutation::RedefineProperty);
        }
    }

    ValueFlowName nameOf(BuiltinSlotID slot) const
    {
        AbstractHeap heap = m_builtins.at(slot).heap;
        if (heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements)
            return elementsName;
        if (m_builtins.at(slot).owner == BuiltinClasses::BoundFunction)
            return { ValueFlowNameKind::HiddenSlot, slot.index() };
        return { ValueFlowNameKind::Slot, slot.index() };
    }

    ValueFlowName nameOfResultField(unsigned fieldIndex)
    {
        for (const BuiltinTypePart& part : m_builtins.at(m_overload->result.type).parts) {
            if (part.kind != BuiltinTypePartKind::Layout)
                continue;
            const BuiltinLayout& layout = m_builtins.at(part.layout);
            if (layout.isTuple)
                return elementsName;
            if (fieldIndex < layout.fields.size())
                return property(m_context, layout.fields[fieldIndex].name);
        }
        return unknownKeyName;
    }

    template<typename Set>
    Set saidOf(BuiltinOperand operand, Set BuiltinParameter::* member) const
    {
        if (operand.isReceiver())
            return m_overload->receiver ? m_overload->receiver->*member : Set();
        Set said;
        for (const BuiltinParameter& parameter : m_overload->parameters) {
            if (parameter.operand == operand || (!parameter.operand.isReceiver() && parameter.operand.coversArgument(operand.argumentIndex())))
                said.add(parameter.*member);
        }
        return said;
    }

    bool isConvertedToObject(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return m_overload->receiver && m_overload->receiver->conversion == BuiltinConversion::ToObject;
        for (const BuiltinParameter& parameter : m_overload->parameters) {
            if (parameter.operand == operand)
                return parameter.conversion == BuiltinConversion::ToObject;
        }
        return false;
    }

    void forEachOperandHandedOnAt(BuiltinOperand position, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor)
    {
        if (!isConvertedToObject(position)) {
            forEachOperandAt(position, functor);
            return;
        }
        ValueSetID wrapped = m_context.maker().intersect(m_universe.primitives(), ValueSets::complement(m_universe.undefinedValue()));
        bool makesWrapper = false;
        forEachOperandAt(position, [&](const ValueFlowOperand& operand) {
            makesWrapper |= m_context.meet(m_context.valueOf(operand), wrapped);
            ValueFlowOperand object = operand;
            object.constant = operand.constant ? m_context.maker().intersect(operand.constant, m_universe.objects()) : operand.constant;
            object.mask = m_context.maker().intersect(operand.mask, m_universe.objects());
            if (isPassed(object))
                functor(object);
        });
        if (makesWrapper)
            functor(operandHolding(unknownObjectSet(m_context, ValueFlowUnknownSource::ObjectWithoutLayout)));
    }

    void forEachNodeHoldingOwnValuesOf(ClimbingSet holders, ValueSetID& constant, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        forEachNodeReadByWalkOver(m_context, m_context.restrict(holders, m_universe.builtinObjects()), { }, functor);
        // A definition makes enumerable what a built-in object holds since it was made.
        if (ClimbingSet redefined = m_context.markedAmong(m_context.restrict(holders, m_universe.builtinObjects()), ObjectMutation::RedefineProperty); holdsAnything(redefined))
            functor(m_context.lookup(redefined, everyOwnPropertyHeldName));
        if (m_context.meet(holders, m_universe.strings()))
            constant = m_context.maker().unite(constant, m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::String)));

        ClimbingSet objects = m_context.restrict(holders, m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_universe.builtinObjects())));
        forEachNodeReadThrough(objects, everyPropertyName, functor);
        functor(enumerablePropertiesNobodyWrote(m_context, withSourcesOfCopies(objects), MetBy::Program));
        if (mayBeUnknownObject(objects))
            constant = m_context.maker().unite(constant, unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        forEachNodeReadThrough(objects, everySetter, [&](ValueFlowNodeID node) {
            if (holdsAnything(m_context.valueOf(node)))
                constant = m_context.maker().unite(constant, m_universe.undefinedValue());
        });
        ClimbingSet getters;
        forEachNodeReadThrough(objects, everyGetterName, [&](ValueFlowNodeID node) {
            getters = m_context.unite(getters, m_context.valueOf(node));
        });
        forEachPartOfCallees(m_context, m_context.restrict(getters, m_universe.callables()), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::Get);
            m_context.add(group.thisValue, objects);
            connectThrown(group, BuiltinCallbackTimes::Once);
            functor(group.result);
        });
    }

    ClimbingSet iteratedOperand(ClimbingSet iterated, ValueSetID& constant)
    {
        if (!mayBeUnknownObject(iterated) || !m_call.operation || !isCodeThatRecordsDescribe(m_universe, m_call.operation->function))
            return iterated;
        constant = m_context.maker().unite(constant, unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        return m_context.restrict(iterated, everythingButUnknownObject(m_context));
    }

    NEVER_INLINE void forEachNodeAt(const BuiltinPlace& place, ValueSetID& constant, const ScopedLambda<void(ValueFlowNodeID)>& functor)
    {
        auto readThrough = [&](ClimbingSet objects, ValueFlowName name) {
            forEachNodeReadThrough(objects, name, functor);
            if (mayBeUnknownObject(objects))
                constant = m_context.maker().unite(constant, unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        };

        switch (place.kind) {
        case BuiltinPlaceKind::None:
        case BuiltinPlaceKind::Operand:
            return;
        case BuiltinPlaceKind::Slot:
            readThrough(objectsAt(*place.operand), nameOf(place.slot));
            return;
        case BuiltinPlaceKind::Prototype: {
            ClimbingSet objects = objectsAt(*place.operand);
            ClimbingSet primitives = m_context.restrict(valueAt(*place.operand), ValueSets::complement(m_universe.objects()));
            ClimbingSet above = objectsRightAbove(m_context, m_context.unite(known(objects), primitives));
            functor(m_context.holding(above));
            if (m_context.meet(above, m_universe.sureInstancesOf(BuiltinClasses::Generator)) || m_context.meet(above, m_universe.sureInstancesOf(BuiltinClasses::AsyncGenerator)))
                constant = m_context.maker().unite(constant, unknownObjectSet(m_context, ValueFlowUnknownSource::ObjectWithoutLayout));
            if (m_context.meet(objects, m_universe.placeholdersAlone()))
                functor(m_context.lookup(m_context.restrict(objects, m_universe.placeholdersAlone()), prototypeLinkName));
            if (mayBeUnknownObject(objects))
                functor(m_context.nodes().ofHost(ValueFlowHostNode::ObjectsAboveEscapedInstances));
            // The prototype objects of library classes above escaped instances are held apart. The modules of the run time list the keys of none.
            if (mayBeUnknownObject(objects) && isCalledByProgramCode())
                functor(heldPrototypeObjectsOfLibraryClasses(m_context));
            bool mayBeEndOfChain = m_context.meet(objects, m_universe.placeholdersAlone());
            bool mayBeNamespace = false;
            m_context.forEachMember(known(objects), [&](ValueFlowObjectID object) {
                ValueFlowObjectKind kind = m_universe.kindOf(object);
                if (kind == ValueFlowObjectKind::Builtin || kind == ValueFlowObjectKind::ModuleBuiltin)
                    mayBeEndOfChain |= m_builtins.at(m_universe.builtinOf(object)).prototypeLink != BuiltinPrototypeLink::Builtin;
                mayBeNamespace |= kind == ValueFlowObjectKind::Namespace;
            });
            if (mayBeNamespace)
                constant = m_context.maker().unite(constant, unknownObjectSet(m_context, ValueFlowUnknownSource::ObjectWithoutLayout));
            if (mayBeEndOfChain)
                constant = m_context.maker().unite(constant, m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
            return;
        }
        case BuiltinPlaceKind::IteratedValues:
            functor(m_context.lookup(iteratedOperand(valueAt(*place.operand), constant), iteratedValues));
            return;
        case BuiltinPlaceKind::OwnValues: {
            bool goesOnDown = saidOf(*place.operand, &BuiltinParameter::uses).contains(OperandUse::AppliesTransitively);
            bool isIteratedOver = saidOf(*place.operand, &BuiltinParameter::protocols).contains(Protocol::Iterate);
            ValueSetID objectsOfProgram = m_context.maker().intersect(m_context.maker().intersect(m_universe.objects(), everythingButUnknownObject(m_context)), ValueSets::complement(m_universe.builtinObjects()));
            ClimbingSet holders = valueAt(*place.operand);
            forEachOperandAt(*place.operand, [&](const ValueFlowOperand& operand) {
                if (holdsNamedOrLoadedBun(operand))
                    constant = m_context.maker().unite(constant, m_context.maker().of(ValueFlowUniverse::markOfLoadedBun()));
            });
            if (mayBeUnknownObject(holders)) {
                forEachOperandAt(*place.operand, [&](const ValueFlowOperand& operand) {
                    if (operand.node)
                        holders = m_context.unite(holders, m_context.valueWithin(ClimbingSet(m_tables.heap().holdersOfMembersThatHostMakes()), operand.node));
                    if (ValueFlowNodeID exports = exportsOfHoldersListedBesideUnknownObject(m_context, operand))
                        functor(exports);
                });
            }
            ClimbingSet visited = holders;
            for (bool findsMore = true; findsMore;) {
                ClimbingSet found;
                auto take = [&](ValueFlowNodeID node) {
                    functor(node);
                    if (goesOnDown)
                        found = m_context.unite(found, m_context.restrict(m_context.valueOf(node), objectsOfProgram));
                };
                forEachNodeHoldingOwnValuesOf(holders, constant, take);
                take(m_context.lookup(m_context.restrict(holders, objectsOfProgram), elementsName));
                if (isIteratedOver)
                    take(m_context.lookup(iteratedOperand(holders, constant), iteratedValues));
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
            for (size_t step = 0; step < place.memberPath.size(); ++step) {
                bool isLastStep = step + 1 == place.memberPath.size();
                ValueSetID constantOfStep = ValueSets::nothing;
                ClimbingSet found;
                auto take = [&](ValueFlowNodeID node) {
                    if (isLastStep)
                        functor(node);
                    else
                        found = m_context.unite(found, m_context.valueOf(node));
                };
                if (BuiltinName name = place.memberPath[step])
                    take(m_context.lookup(holders, property(m_context, name)));
                else
                    forEachNodeHoldingOwnValuesOf(holders, constantOfStep, take);
                if (isLastStep)
                    constant = m_context.maker().unite(constant, constantOfStep);
                else
                    holders = m_context.unite(found, ClimbingSet(constantOfStep));
            }
            return;
        }
        case BuiltinPlaceKind::NativeCell:
            functor(nodeReadFromNativeCell(m_context, place.nativeCell));
            return;
        case BuiltinPlaceKind::Result:
            if (ValueFlowNodeID result = nodeHoldingResult())
                functor(result);
            return;
        case BuiltinPlaceKind::ResultSlot:
            if (m_resultObject)
                functor(m_context.readWhatWasWritten(only(m_context, m_resultObject), nameOf(place.slot)));
            return;
        case BuiltinPlaceKind::ResultField:
            if (m_resultObject)
                functor(m_context.readWhatWasWritten(only(m_context, m_resultObject), nameOfResultField(place.fieldIndex)));
            return;
        case BuiltinPlaceKind::NewValue:
            constant = m_context.maker().unite(constant, valueOfType(place.newValueType, m_contentsObject));
            noteInstancesIn(m_context, place.newValueType);
            forEachCallbackResultIn(place.newValueType, functor);
            return;
        case BuiltinPlaceKind::NativeMemory:
            constant = m_context.maker().unite(constant, undescribedValue());
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void forEachPlaceHandedOnIn(BuiltinTypeID type, NOESCAPE const Invocable<void(const BuiltinPlace&)> auto& functor)
    {
        if (!type)
            return;
        for (const BuiltinTypePart& part : m_builtins.at(type).parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Operand:
                functor({ .kind = BuiltinPlaceKind::Operand, .operand = part.operand });
                break;
            case BuiltinTypePartKind::Slot:
                functor({ .kind = BuiltinPlaceKind::Slot, .operand = part.operand, .slot = part.slot });
                break;
            case BuiltinTypePartKind::IteratedValues:
                functor({ .kind = BuiltinPlaceKind::IteratedValues, .operand = part.operand, .isSettled = part.isSettled });
                break;
            case BuiltinTypePartKind::Prototype:
                functor({ .kind = BuiltinPlaceKind::Prototype, .operand = part.operand });
                break;
            case BuiltinTypePartKind::Instance:
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            case BuiltinTypePartKind::Layout:
            case BuiltinTypePartKind::Builtin:
            case BuiltinTypePartKind::CallbackResult:
                break;
            }
        }
    }

    void forEachCallbackResultIn(BuiltinTypeID type, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        if (!type)
            return;
        for (const BuiltinTypePart& part : m_builtins.at(type).parts) {
            if (part.kind != BuiltinTypePartKind::CallbackResult)
                continue;
            for (const BuiltinCallback& callback : m_overload->flow.callbacks) {
                if (part.operand != callback.callee)
                    continue;
                forEachPartOfCallees(m_context, m_context.restrict(valueAt(callback.callee), m_universe.callables()), [&](ClimbingSet callees) {
                    ValueFlowNodeID result = m_context.group(callees, callback.constructs ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode, originOfOperands()).result;
                    functor(part.isSettled ? m_context.lookup(m_context.valueOf(result), awaitedValueName) : result);
                    if (ValueFlowNodeID listedBun = part.isSettled ? findNodeOfListedBun(m_context, operandOfBuiltinAt(result)) : ValueFlowNodeID())
                        functor(listedBun);
                });
            }
        }
    }

    ClimbingSet valueAt(const BuiltinPlace& place)
    {
        ClimbingSet value;
        if (place.kind == BuiltinPlaceKind::Operand) {
            forEachOperandHandedOnAt(*place.operand, [&](const ValueFlowOperand& operand) {
                value = m_context.unite(value, m_context.valueOf(operand));
            });
        }
        if (place.kind == BuiltinPlaceKind::NewValue) {
            forEachPlaceHandedOnIn(place.newValueType, [&](const BuiltinPlace& handedOn) {
                value = m_context.unite(value, valueAt(handedOn));
            });
        }
        ValueSetID constant = ValueSets::nothing;
        forEachNodeAt(place, constant, [&](ValueFlowNodeID node) {
            value = m_context.unite(value, m_context.valueOf(node));
        });
        value = m_context.unite(value, ClimbingSet(constant));
        return place.isSettled ? m_context.valueOf(m_context.lookup(value, awaitedValueName)) : value;
    }

    ValueFlowNodeID findNodeOfBunListedAt(const BuiltinPlace& place)
    {
        ValueFlowNodeID listedBun;
        forEachOperandAt(place, [&](const ValueFlowOperand& operand) {
            if (!listedBun)
                listedBun = findNodeOfListedBun(m_context, operand);
        });
        return listedBun;
    }

    bool holdsNamedBun(const ValueFlowOperand& operand)
    {
        return operand.node && m_context.holdsAnyOf(only(m_context, ValueFlowUniverse::markOfNamedBun()), operand.node) && m_context.holdsAnyOf(only(m_context, m_universe.objectOf(Builtins::Bun)), operand.node);
    }

    bool holdsNamedOrLoadedBun(const ValueFlowOperand& operand)
    {
        return holdsNamedBun(operand) || (operand.node && m_context.holdsAnyOf(only(m_context, ValueFlowUniverse::markOfLoadedBun()), operand.node) && m_context.holdsAnyOf(only(m_context, m_universe.objectOf(Builtins::Bun)), operand.node));
    }

    // A name read on the result is a name of the object at the operand.
    void listNamedBunOnResult(BuiltinOperand position)
    {
        if (!m_call.result)
            return;
        forEachOperandAt(position, [&](const ValueFlowOperand& operand) {
            if (holdsNamedBun(operand))
                m_context.connect(findNodeOfListedBun(m_context, operand), m_call.result);
        });
    }

    // Where the result may also be the unknown object, its node may stop listing the new object. The unknown object stands for it then, and holds what it holds.
    void storeNamedBunInUnknownObject(const BuiltinPlace& source, ValueFlowName name)
    {
        if (!m_call.result || !m_context.holdsAnyOf(unknownObjectAlone(), m_call.result))
            return;
        forEachOperandAt(source, [&](const ValueFlowOperand& operand) {
            if (holdsNamedBun(operand))
                m_context.add(m_context.written(unknownObjectAlone(), name), only(m_context, m_universe.objectOf(Builtins::Bun)));
        });
    }

    // The rows of these two say that what they hand out holds anything.
    void listNamedBunAmongSettledValues()
    {
        BuiltinID canonical = m_builtins.canonicalID(m_builtin);
        BuiltinPlace source;
        ValueFlowName name;
        if (canonical == Builtins::PromiseAllSettled) {
            source = { .kind = BuiltinPlaceKind::IteratedValues, .operand = BuiltinOperand::argument(0), .isSettled = true };
            name = property(m_context, BuiltinNames::value);
        } else if (canonical == Builtins::PromiseWithResolvers) {
            // Nobody follows the function that resolves the promise, so whatever has escaped may have been handed to it.
            source = { .kind = BuiltinPlaceKind::NativeMemory };
            name = nameOf(BuiltinSlots::PromiseValue);
        } else
            return;
        auto store = [&](const ValueFlowOperand& operand) {
            if (holdsNamedBun(operand) && m_contentsObject)
                m_context.add(m_context.written(only(m_context, m_contentsObject), name), only(m_context, m_universe.objectOf(Builtins::Bun)));
        };
        if (source.kind == BuiltinPlaceKind::NativeMemory) {
            store(operandOfBuiltinAt(escaped()));
            if (holdsNamedBun(operandOfBuiltinAt(escaped())) && m_call.result && m_context.holdsAnyOf(unknownObjectAlone(), m_call.result))
                m_context.add(m_context.written(unknownObjectAlone(), name), only(m_context, m_universe.objectOf(Builtins::Bun)));
            return;
        }
        forEachOperandAt(source, store);
        storeNamedBunInUnknownObject(source, name);
    }

    // A map that was filled with what came out of a built-in module under keys nobody knows answers the member that the key names.
    void readMemberUnderKeyOfMap()
    {
        if (m_builtins.canonicalID(m_builtin) != Builtins::MapPrototypeGet || !m_call.result)
            return;
        bool mayAnswerMemberOfBun = m_context.holdsAnyOf(only(m_context, ValueFlowUniverse::markOfLoadedBun()), m_call.result);
        if (!mayAnswerMemberOfBun && !m_context.holdsAnyOf(only(m_context, ValueFlowUniverse::markOfMemberLeftOut()), m_call.result))
            return;
        BuiltinPropertyAction read { .kind = BuiltinPropertyActionKind::ReadsByKey, .object = BuiltinOperand::receiver(), .key = { .kind = BuiltinKeyKind::Operand, .operand = BuiltinOperand::argument(0) } };
        ValueFlowOperand module = operandHolding(mayAnswerMemberOfBun ? m_context.maker().of(m_universe.objectOf(Builtins::Bun)) : ValueSets::nothing);
        forEachKeyOperand(read.key, [&](const ValueFlowOperand& key) {
            Typed::applyPropertyAction(m_context, read, { module, key, { }, m_call.result, m_call.thrown });
        });
    }

    NEVER_INLINE void forEachOperandAt(const BuiltinPlace& place, const ScopedLambda<void(const ValueFlowOperand&)>& functor)
    {
        if (place.kind == BuiltinPlaceKind::None)
            return;
        if (place.isSettled) {
            BuiltinPlace unsettled = place;
            unsettled.isSettled = false;
            functor(operandOfBuiltinAt(m_context.lookup(valueAt(unsettled), awaitedValueName)));
            if (ValueFlowNodeID listedBun = findNodeOfBunListedAt(unsettled))
                functor(operandOfBuiltinAt(listedBun));
            return;
        }
        if (place.kind == BuiltinPlaceKind::Operand) {
            forEachOperandHandedOnAt(*place.operand, functor);
            return;
        }
        if (place.kind == BuiltinPlaceKind::NewValue) {
            forEachPlaceHandedOnIn(place.newValueType, [&](const BuiltinPlace& handedOn) {
                forEachOperandAt(handedOn, functor);
            });
        }

        ValueSetID constant = ValueSets::nothing;
        forEachNodeAt(place, constant, [&](ValueFlowNodeID node) {
            functor(operandOfBuiltinAt(node));
        });
        if (constant != ValueSets::nothing)
            functor(operandHolding(constant));
        if (place.kind == BuiltinPlaceKind::IteratedValues) {
            // Whatever goes over an array may hand out its elements.
            if (ValueFlowNodeID listedBun = findNodeOfListedBun(m_context, operandOfBuiltinAt(m_context.readWhatWasWritten(known(objectsAt(*place.operand)), elementsName))))
                functor(operandOfBuiltinAt(listedBun));
        }
    }

    void connectFrom(const BuiltinPlace& place, ValueFlowNodeID target)
    {
        if (!target)
            return;
        forEachOperandAt(place, [&](const ValueFlowOperand& operand) {
            if (target == valuesCarriedOut())
                carryOut(m_context, operand);
            else
                m_context.connect(operand, target);
        });
    }

    void normalizeKeysStoredAt(BuiltinSlotID id, ValueFlowNodeID target)
    {
        const BuiltinSlot& slot = m_builtins.at(id);
        bool holdsKeys = (slot.owner == BuiltinClasses::Map && slot.contentsSlot == ContentsSlot::Key) || (slot.owner == BuiltinClasses::Set && slot.contentsSlot == ContentsSlot::Element);
        if (!holdsKeys || target == valuesCarriedOut())
            return;
        if (m_context.holdsAnyOf(only(m_context, ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber)), target))
            m_context.add(target, only(m_context, ValueFlowUniverse::primitive(PrimitiveSet::Integer)));
    }

    void forEachTargetAt(const BuiltinPlace& place, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& takeTarget)
    {
        auto functor = [&](ValueFlowNodeID target) {
            takeTarget(target);
            if (place.kind == BuiltinPlaceKind::Slot || place.kind == BuiltinPlaceKind::ResultSlot)
                normalizeKeysStoredAt(place.slot, target);
        };
        auto writeToResult = [&](ValueFlowName name) {
            functor(m_resultObject ? m_context.written(only(m_context, m_resultObject), name) : valuesCarriedOut());
            if (m_context.meet(m_objectsMadeBySpecies, m_universe.objects()))
                forEachNodeWrittenThrough(m_objectsMadeBySpecies, name, functor);
        };

        switch (place.kind) {
        case BuiltinPlaceKind::None:
            return;
        case BuiltinPlaceKind::Result:
            if (ValueFlowNodeID result = nodeTakingResult())
                functor(result);
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
            forEachNodeWrittenThrough(objectsAt(*place.operand), unknownKeyName, functor);
            return;
        case BuiltinPlaceKind::Prototype:
            forEachNodeWrittenThrough(objectsAt(*place.operand), prototypeLinkName, functor);
            return;
        case BuiltinPlaceKind::NativeCell:
            functor(nodeWrittenToNativeCell(m_context, place.nativeCell));
            return;
        case BuiltinPlaceKind::Operand:
        case BuiltinPlaceKind::IteratedValues:
        case BuiltinPlaceKind::NewValue:
        case BuiltinPlaceKind::NativeMemory:
        case BuiltinPlaceKind::Member:
            functor(valuesCarriedOut());
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static bool resolvesPromise(const BuiltinPlace& to) { return (to.kind == BuiltinPlaceKind::ResultSlot || to.kind == BuiltinPlaceKind::Slot) && to.slot == BuiltinSlots::PromiseValue; }

    void adoptRejection(const BuiltinPlace& resolved, ClimbingSet values)
    {
        BuiltinPlace reason = resolved;
        reason.slot = BuiltinSlots::PromiseReason;
        forEachTargetAt(reason, [&](ValueFlowNodeID held) {
            adoptRejectionOf(m_context, values, held);
        });
    }

    void connect(const BuiltinPlace& from, const BuiltinPlace& to)
    {
        BuiltinPlace source = from;
        if (resolvesPromise(to) && !from.isSettled) {
            adoptRejection(to, valueAt(from));
            source.isSettled = true;
        }
        bool isReceiver = from.kind == BuiltinPlaceKind::Operand && from.operand->isReceiver();
        forEachTargetAt(to, [&](ValueFlowNodeID target) {
            connectFrom(source, isReceiver && target == valuesCarriedOut() ? escaped() : target);
        });
        if (to.kind == BuiltinPlaceKind::ResultSlot)
            storeNamedBunInUnknownObject(source, nameOf(to.slot));
        else if (to.kind == BuiltinPlaceKind::ResultField)
            storeNamedBunInUnknownObject(source, nameOfResultField(to.fieldIndex));
    }

    static bool listsWhatIsMadeWithoutSpecies(const BuiltinType& type) { return type.freshness == BuiltinFreshness::UnlessSpecies && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind); }

    bool holdsModuleObjects(const BuiltinTypePart& part) const
    {
        return std::ranges::any_of(part.contents, [&](const BuiltinSlotContents& contents) {
            return std::ranges::any_of(m_builtins.at(contents.type).parts, [&](const BuiltinTypePart& held) {
                return held.kind == BuiltinTypePartKind::Builtin && m_builtins.canonicalID(held.builtin) == Builtins::ModuleObject;
            });
        });
    }

    ValueSetID undescribedValue() { return m_builtins.at(m_builtin).origin == BuiltinOrigin::Host ? unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult) : undescribedValueThatIsNoBuffer(m_context); }
    ValueSetID undescribedObject() { return m_context.maker().intersect(undescribedValue(), m_universe.objects()); }

    ValueSetID valueInSlot(BuiltinSlotID slot, BuiltinTypeID type)
    {
        if (slot == BuiltinSlots::PromiseReason && (!type || m_builtins.at(type).isAnything()))
            return unknownValue(m_context, ValueFlowUnknownSource::ThrownByRunTime);
        return valueOfType(type, m_contentsObject);
    }

    ValueSetID valueOfType(BuiltinTypeID id, ValueFlowObjectID fresh)
    {
        if (!id)
            return undescribedValue();
        const BuiltinType& type = m_builtins.at(id);
        if (type.isAnything())
            return undescribedValue();

        ValueSetMaker& maker = m_context.maker();
        ValueSetID value = m_tables.primitivesOf(id);
        bool isPlainDataOfResult = type.flags.contains(BuiltinTypeFlag::HasPlainData) && !type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }) && fresh && fresh == m_resultObject && m_contentsObject;
        if (isPlainDataOfResult)
            value = maker.unite(value, plainDataHolding(primitivesInsidePlainData(value), m_resultObject, m_contentsObject, m_plainDataOfResult));
        else if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }) && !listsWhatIsMadeWithoutSpecies(type))
            value = maker.unite(value, undescribedObject());
        else if (type.flags.contains(BuiltinTypeFlag::HasPlainData) && !listsWhatIsMadeWithoutSpecies(type)) {
            // One object and one array stand for all the plain data that lies inside what built-ins make.
            ValueSetID data;
            value = maker.unite(value, plainDataHolding(primitivesInsidePlainData(value), m_universe.resultOf(Builtins::JSONParse, 0), m_universe.resultOf(Builtins::JSONParse, 1), data));
        }

        Vector<ValueFlowObjectID, 32> objects;
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Builtin:
                m_context.use(part.builtin, BuiltinUse::Read);
                objects.shrink(0);
                if (m_builtins.canonicalID(part.builtin) == Builtins::ModuleObject)
                    appendModuleObjectsAnsweredBy(m_context, m_builtins.canonicalID(m_builtin), objects);
                else if (m_builtin == Builtins::NodeModuleCreateRequire)
                    objects.append(m_universe.objectOf(part.builtin));
                else
                    m_universe.appendObjectsOf(part.builtin, objects);
                value = maker.unite(value, maker.of(objects.span()));
                break;
            case BuiltinTypePartKind::Instance:
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            case BuiltinTypePartKind::Layout:
                if (holdsModuleObjects(part)) {
                    ValueFlowObjectID list = m_universe.resultOf(m_builtin, 0);
                    allocate(part, list, type.freshness);
                    value = maker.unite(value, maker.of(list));
                    for (const BuiltinSlotContents& contents : part.contents)
                        handChildrenToWhatReplacesTheirList(m_context, valueInSlot(contents.slot, contents.type));
                    break;
                }
                if (type.freshness == BuiltinFreshness::No || !fresh || isCalledInWaysNobodyFollows(part)) {
                    bool mayBeBuffer = part.kind == BuiltinTypePartKind::Instance
                        && (!m_builtins.at(part.builtinClass).prototype || std::ranges::contains(m_tables.classesAtOrBelow(part.builtinClass), BuiltinClassID(BuiltinClasses::Buffer)));
                    value = maker.unite(value, mayBeBuffer ? undescribedObject() : maker.intersect(undescribedValueThatIsNoBuffer(m_context), m_universe.objects()));
                    if (part.kind == BuiltinTypePartKind::Instance)
                        noteConstructorsAboveUnknownInstance(part);
                    break;
                }
                allocate(part, fresh, type.freshness);
                value = maker.unite(value, maker.of(fresh));
                break;
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

    bool mayHaveReachedUnknownJavaScript(ClimbingSet objects)
    {
        if (m_context.meet(m_context.markedAmong(objects, ObjectMutation::ReachUnknownJavaScript), ValueSets::anything))
            return true;
        if (!m_context.holdsAnyOf(objects, ValueFlowProgramNodes::escaped()))
            return false;
        return m_context.contains(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::UnseenCodeRuns)), ValueFlowUniverse::unknownObject())
            || m_context.meet(m_context.markedAmong(ClimbingSet(m_universe.unknownObjectAlone()), ObjectMutation::ReachUnknownJavaScript), ValueSets::anything);
    }

    ValueSetID recordsMadeByParsers()
    {
        Vector<ValueFlowObjectID, 8> made;
        for (LayoutID layout : m_universe.recordsThatTypedCodeTakes()) {
            if (mayHaveReachedUnknownJavaScript(ClimbingSet(m_universe.objectsWith(layout))))
                made.append(m_universe.instanceOf(layout));
        }
        return m_context.withMembersOf(m_context.maker().of(made.span()), m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers)));
    }

    ValueSetID primitivesInsidePlainData(ValueSetID listed) const
    {
        constexpr PrimitiveSet heldByJSON[] { PrimitiveSet::Null, PrimitiveSet::False, PrimitiveSet::True, PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber, PrimitiveSet::String };
        bool listsWhatJSONHolds = std::ranges::all_of(heldByJSON, [&](PrimitiveSet kind) {
            return m_context.sets().contains(listed, ValueFlowUniverse::primitive(kind));
        });
        return listsWhatJSONHolds ? listed : m_universe.anyPrimitive();
    }

    ValueSetID plainDataHolding(ValueSetID primitives, ValueFlowObjectID plainObject, ValueFlowObjectID plainArray, ValueSetID& data)
    {
        ValueSetMaker& maker = m_context.maker();
        ClimbingSet object = only(m_context, plainObject);
        ClimbingSet array = only(m_context, plainArray);
        ValueFlowObjectID members[] { plainObject, plainArray };
        ValueSetID records = recordsMadeByParsers();
        ValueSetID objects = maker.unite(maker.of(std::span<const ValueFlowObjectID>(members)), records);
        data = maker.unite(primitives, objects);
        if (records != ValueSets::nothing) {
            m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers), records);
            m_context.add(m_context.written(ClimbingSet(records), prototypeLinkName), only(m_context, m_universe.objectOf(Builtins::ObjectPrototype)));
            m_context.add(m_context.written(ClimbingSet(records), unknownKeyName), data);
        }
        m_context.add(m_context.written(object, prototypeLinkName), only(m_context, m_universe.objectOf(Builtins::ObjectPrototype)));
        m_context.add(m_context.written(array, prototypeLinkName), only(m_context, m_universe.objectOf(Builtins::ArrayPrototype)));
        noteAllocationAs(BuiltinClasses::Object, plainObject);
        noteAllocationAs(BuiltinClasses::Array, plainArray);
        m_context.add(m_context.written(object, unknownKeyName), data);
        m_context.add(m_context.written(array, elementsName), data);
        m_context.mark(object, ObjectMutation::AddProperty);
        return objects;
    }

    bool takesPrototypeOfResultFromOperand() const
    {
        return std::ranges::any_of(m_overload->flow.propertyActions, [](const BuiltinPropertyAction& action) {
            return action.kind == BuiltinPropertyActionKind::BecomesPrototype && action.value.kind == BuiltinPlaceKind::Result;
        });
    }

    void addPrototypesOfClassesBelow(BuiltinClassID named, ValueFlowNodeID prototype)
    {
        if (BuiltinID constructor = m_builtins.at(named).constructor; constructor && m_builtins.canonicalID(constructor) == m_builtins.canonicalID(m_builtin))
            return;
        for (BuiltinClassID below : m_tables.classesAtOrBelow(named)) {
            BuiltinID prototypeObject = m_builtins.at(below).prototype;
            if (below != named && prototypeObject)
                m_context.add(prototype, only(m_context, m_universe.objectOf(prototypeObject)));
        }
    }

    void noteConstructorsAboveUnknownInstance(const BuiltinTypePart& part)
    {
        ValueFlowNodeID constructors = m_context.nodes().ofHost(ValueFlowHostNode::ConstructorsAboveEscapedInstances);
        for (BuiltinClassID builtinClass : m_tables.classesAtOrBelow(part.builtinClass)) {
            if (builtinClass != part.builtinClass && part.isOfClassItself)
                continue;
            if (BuiltinID constructor = m_builtins.at(builtinClass).constructor) {
                m_context.use(constructor, BuiltinUse::Read);
                m_context.add(constructors, only(m_context, m_universe.objectOf(m_builtins.canonicalID(constructor))));
            }
        }
    }

    void allocate(const BuiltinTypePart& part, ValueFlowObjectID fresh, BuiltinFreshness freshness)
    {
        ClimbingSet object = only(m_context, fresh);
        ValueFlowNodeID prototype = m_context.written(object, prototypeLinkName);
        noteAllocationAs(part.kind == BuiltinTypePartKind::Instance ? part.builtinClass : BuiltinClassID(), fresh);
        switch (part.kind) {
        case BuiltinTypePartKind::Instance:
            if (BuiltinID prototypeObject = m_builtins.at(part.builtinClass).prototype) {
                if (fresh != m_resultObject || !takesPrototypeOfResultFromOperand()) {
                    m_context.add(prototype, only(m_context, m_universe.objectOf(prototypeObject)));
                    if (!part.isOfClassItself)
                        addPrototypesOfClassesBelow(part.builtinClass, prototype);
                }
            } else if (!m_overload->result.prototypeSource || fresh != m_resultObject)
                m_context.add(prototype, undescribedObject());
            if (freshness == BuiltinFreshness::Unaliased && !listsOwnNamesOf(part.builtinClass, fresh)) {
                m_context.add(prototype, undescribedObject());
                m_context.add(m_context.written(object, unknownKeyName), undescribedValue());
            }
            if (isConstruction(m_call.kind) && fresh == m_resultObject) {
                m_context.connect(m_context.lookup(m_context.valueOf(m_call.thisValue), property(m_context, BuiltinNames::prototype)), prototype);
                ClimbingSet classesOfProgram = m_context.restrict(m_context.valueOf(m_call.thisValue), ValueSets::complement(m_universe.builtinObjects()));
                bool mayCallMembersOfNewObject = m_overload->effects.capability == BuiltinCapability::JS && !m_builtins.at(part.builtinClass).constructorOnlyReadsPrototypeOfNewTarget;
                if (holdsAnything(classesOfProgram) && mayCallMembersOfNewObject) {
                    m_context.add(escaped(), object);
                    m_context.add(escaped(), classesOfProgram);
                }
            }
            break;
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            connectFrom({ .kind = BuiltinPlaceKind::Prototype, .operand = BuiltinOperand::receiver() }, prototype);
            break;
        case BuiltinTypePartKind::Layout: {
            const BuiltinLayout& layout = m_builtins.at(part.layout);
            m_context.add(prototype, only(m_context, m_universe.objectOf(layout.isTuple ? Builtins::ArrayPrototype : Builtins::ObjectPrototype)));
            for (unsigned index = 0; index < layout.fields.size(); ++index) {
                const BuiltinLayoutField& field = layout.fields[index];
                if (fresh == m_resultObject && holdsOnlyWhatIsStored(index))
                    continue;
                m_context.add(m_context.written(object, layout.isTuple ? elementsName : property(m_context, field.name)), valueOfType(field.type, m_contentsObject));
            }
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (std::optional<BuiltinOperand> source = m_overload->result.prototypeSource; source && fresh == m_resultObject)
            connectFrom({ .kind = BuiltinPlaceKind::Prototype, .operand = source }, prototype);
        for (const BuiltinSlotContents& contents : part.contents) {
            ValueFlowNodeID held = m_context.written(object, nameOf(contents.slot));
            if (fresh != m_resultObject || !listOwnKeysIn(held))
                m_context.add(held, valueInSlot(contents.slot, contents.type));
            if (fresh == m_resultObject)
                continue;
            forEachPlaceHandedOnIn(contents.type, [&](const BuiltinPlace& handedOn) {
                connectFrom(handedOn, held);
                storeNamedBunInUnknownObject(handedOn, nameOf(contents.slot));
            });
        }
        if (part.kind == BuiltinTypePartKind::Instance) {
            fillSlotsThatRowLeavesOut(part, fresh);
            writeNamesDefinedOnNewInstance(part.builtinClass, fresh);
            if (m_builtins.definesCharactersOnNewInstances(part.builtinClass))
                m_context.add(m_context.written(object, elementsName), only(m_context, ValueFlowUniverse::primitive(PrimitiveSet::String)));
        }
        if (fresh != m_resultObject)
            return;
        for (const BuiltinResultProperty& own : m_overload->result.properties)
            m_context.add(m_context.written(object, property(m_context, own.name)), valueOfType(own.type, m_contentsObject));
    }

    bool listOwnKeysIn(ValueFlowNodeID held)
    {
        BuiltinID builtin = m_builtins.canonicalID(m_builtin);
        bool listsSymbols = builtin == Builtins::ReflectOwnKeys;
        if (builtin != Builtins::ObjectKeys && builtin != Builtins::ObjectGetOwnPropertyNames && !listsSymbols)
            return false;
        ValueFlowObjectID neverListed[] { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null), ValueFlowUniverse::primitive(PrimitiveSet::Absent) };
        ClimbingSet listed = m_context.restrict(valueAt(BuiltinOperand::argument(0)), ValueSets::complement(m_context.maker().of(neverListed)));
        if (!holdsAnything(listed))
            return true;
        if (m_context.meet(listed, ValueSets::complement(m_universe.knownObjects())))
            return false;
        if (holdsAnything(m_context.markedAmong(listed, { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript })) || m_context.holdsAnyOf(listed, escaped()) || m_context.holdsAnyOf(listed, ValueFlowProgramNodes::lost()))
            return false;

        Vector<ValueFlowObjectID, 16> keys;
        bool areLiterals = true;
        auto list = [&](PropertyKeyID key) {
            PropertyKeyKind kind = m_program.at(key).kind;
            if (kind == PropertyKeyKind::Text || (listsSymbols && kind != PropertyKeyKind::PrivateName && kind != PropertyKeyKind::EnginePrivateName && kind != PropertyKeyKind::ModulePrivateName))
                keys.append(m_universe.keyConstant(key));
        };
        m_context.forEachMember(listed, [&](ValueFlowObjectID object) {
            const ValueFlowSite* site = m_universe.kindOf(object) == ValueFlowObjectKind::Site ? m_universe.siteOf(object) : nullptr;
            const Value* creation = site && site->origin != ValueFlowSiteOrigin::BuiltinResult && site->origin != ValueFlowSiteOrigin::TypedResult ? &m_program.at(m_universe.functionOf(object)).at(site->value) : nullptr;
            bool isLiteralOfConstants = creation && site->origin == ValueFlowSiteOrigin::DataNode && site->layout;
            bool isLiteral = isLiteralOfConstants || (creation && site->origin == ValueFlowSiteOrigin::Operation && (creation->opcode == Opcode::NewObject || creation->opcode == Opcode::NewDictionary));
            if (!isLiteral || m_context.sets().contains(m_universe.objectsThatPickTheirLayout(), object)) {
                areLiterals = false;
                return;
            }
            if (isLiteralOfConstants) {
                forEachDataNodeInPreorder(m_program.at(creation->data()).root, [&](uint32_t ordinal, const DataNode& node) {
                    if (ordinal != site->ordinal)
                        return;
                    for (const DataMember& member : node.as<DataNode::Object>().members)
                        list(member.key);
                });
            }
            if (!site->layout)
                return;
            for (const Field& field : m_program.at(site->layout).fields)
                list(field.key);
        });
        if (!areLiterals)
            return false;
        m_context.add(held, ClimbingSet(m_context.maker().of(keys.span())));
        m_context.connect(m_context.masked(m_context.read(listed, ValueFlowCellName { ValueFlowNameKind::AddedNames }), listsSymbols ? ValueSets::anything : m_universe.strings()), held);
        return true;
    }

    bool listsOwnNamesOf(BuiltinClassID builtinClass, ValueFlowObjectID fresh) const
    {
        if (fresh == m_resultObject && !m_overload->result.properties.empty())
            return true;
        if (fresh == m_resultObject && std::ranges::contains(m_overload->flow.propertyActions, BuiltinPropertyActionKind::DescribesResult, &BuiltinPropertyAction::kind))
            return true;
        return !m_builtins.listsOwnPropertiesOfNewInstances(builtinClass) && !m_builtins.mayDefineOnNewInstances(builtinClass, { });
    }

    bool holdsOnlyWhatIsStored(unsigned fieldOfResult) const
    {
        auto saysSo = [&](const BuiltinPlace& place) {
            return place.kind == BuiltinPlaceKind::ResultField && place.fieldIndex == fieldOfResult && place.holdsOnlyWhatIsStored;
        };
        const auto& flow = m_overload->flow;
        return std::ranges::any_of(flow.transfers, saysSo, &BuiltinTransfer::to)
            || std::ranges::any_of(flow.callbacks, saysSo, &BuiltinCallback::resultTo)
            || std::ranges::any_of(flow.propertyActions, saysSo, &BuiltinPropertyAction::value);
    }

    bool isFilledByRecord(BuiltinSlotID slot) const
    {
        auto namesSlot = [&](const BuiltinPlace& place) {
            return place.kind == BuiltinPlaceKind::ResultSlot && place.slot == slot;
        };
        const auto& flow = m_overload->flow;
        return std::ranges::any_of(flow.transfers, namesSlot, &BuiltinTransfer::to)
            || std::ranges::any_of(flow.callbacks, namesSlot, &BuiltinCallback::resultTo)
            || std::ranges::any_of(flow.propertyActions, namesSlot, &BuiltinPropertyAction::value);
    }

    void fillSlotsThatRowLeavesOut(const BuiltinTypePart& part, ValueFlowObjectID fresh)
    {
        for (BuiltinClassID current = part.builtinClass; current; current = m_builtins.at(current).parent) {
            for (BuiltinSlotID slot : m_builtins.at(current).slots) {
                if (std::ranges::contains(part.contents, slot, &BuiltinSlotContents::slot) || (fresh == m_resultObject && isFilledByRecord(slot)))
                    continue;
                BuiltinTypeID holds = m_builtins.at(slot).holds;
                m_context.add(m_context.written(only(m_context, fresh), nameOf(slot)), valueInSlot(slot, holds));
            }
        }
    }

    void writeNamesDefinedOnNewInstance(BuiltinClassID builtinClass, ValueFlowObjectID fresh)
    {
        ClimbingSet object = only(m_context, fresh);
        for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
            const BuiltinClass& entry = m_builtins.at(current);
            if (!entry.namesDefinedOnNewInstances.empty())
                m_context.mark(object, { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty });
            for (const BuiltinResultProperty& own : entry.typesOfNamesDefinedOnNewInstances) {
                if (PropertyKeyID key = m_program.keyOf(own.name))
                    m_context.add(m_context.written(object, ValueFlowName::property(m_program, key)), m_tables.primitivesOf(own.type));
            }
            for (const BuiltinPropertyMaker& maker : entry.makersOfNamesDefinedOnNewInstances) {
                PropertyKeyID made = m_program.keyOf(maker.name);
                if (!mayHaveMaker(maker))
                    continue;
                m_context.add(escaped(), object);
                if (made)
                    m_context.add(m_context.written(object, ValueFlowName::property(m_program, made)), unknownValue(m_context, ValueFlowUnknownSource::ResultOfUnknownCallee));
            }
        }
    }

    bool mayHaveMaker(const BuiltinPropertyMaker& maker)
    {
        ClimbingSet chain = m_context.valueOf(m_context.lookup(only(m_context, m_universe.objectOf(maker.holder)), prototypeLinkName));
        if (m_context.meet(chain, m_universe.placeholdersAlone()))
            return true;
        PropertyKeyID key = m_program.keyOf(maker.member);
        return m_context.meet(m_context.valueOf(m_context.readWhatWasWritten(m_context.restrict(chain, m_universe.knownObjects()), key ? ValueFlowName::property(m_program, key) : unknownKeyName)), m_universe.callables());
    }

    void passArguments(const ValueFlowCallGroup& group, std::span<const BuiltinPlace> arguments)
    {
        unsigned next = 0;
        bool positionIsKnown = true;
        auto takeTarget = [&] {
            unsigned index = next++;
            return positionIsKnown ? group.argumentAt(index) : group.otherArguments;
        };
        for (const BuiltinPlace& argument : arguments) {
            bool isRest = argument.kind == BuiltinPlaceKind::Operand && !argument.isSettled && argument.operand->kind() == BuiltinOperand::Kind::ArgumentsFrom;
            if (!isRest) {
                connectFrom(argument, takeTarget());
                continue;
            }
            for (size_t index = argument.operand->argumentIndex(); index < m_call.arguments.size(); ++index) {
                ValueFlowNodeID target = takeTarget();
                if (isPassed(m_call.arguments[index]))
                    m_context.connect(m_call.arguments[index], target);
            }
            for (const ValueFlowOperand* operand : { &m_call.laterArguments, &m_call.otherArguments }) {
                if (isPassed(*operand))
                    m_context.connect(*operand, group.otherArguments);
            }
            positionIsKnown = false;
        }
    }

    void useResult(const ValueFlowCallGroup& group, BuiltinCallbackResultUse use, const BuiltinPlace& resultTo)
    {
        switch (use) {
        case BuiltinCallbackResultUse::Ignored:
        case BuiltinCallbackResultUse::Tested:
            return;
        case BuiltinCallbackResultUse::Escapes:
            carryOut(m_context, group.result);
            return;
        case BuiltinCallbackResultUse::Returned:
            if (ValueFlowNodeID result = nodeTakingResult())
                m_context.connect(group.result, result);
            return;
        case BuiltinCallbackResultUse::Stored:
            if (resolvesPromise(resultTo))
                adoptRejection(resultTo, m_context.valueOf(group.result));
            forEachTargetAt(resultTo, [&](ValueFlowNodeID target) {
                if (resolvesPromise(resultTo))
                    connectAwaitedValue(m_context, operandOfBuiltinAt(group.result), target);
                else
                    m_context.connect(group.result, target);
            });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void connectThrown(const ValueFlowCallGroup& group, BuiltinCallbackTimes times) { m_context.connect(group.thrown, isCalledAfterReturn(times) || !m_call.thrown ? valuesCarriedOut() : m_call.thrown); }

    ValueFlowCalleeOrigin originOfOperands() const { return m_call.operation && isCodeBehindDeclaredEntries(m_program, m_call.operation->function) ? ValueFlowCalleeOrigin::OperandOfAppliedEntry : ValueFlowCalleeOrigin::Value; }

    bool listsObjectThatLetsInUnseenCode(const BuiltinPlace& place)
    {
        bool lists = false;
        forEachOperandAt(place, [&](const ValueFlowOperand& operand) {
            lists = lists || Typed::listsObjectThatLetsInUnseenCode(m_context, operand);
        });
        return lists;
    }

    ClimbingSet calleesOf(const BuiltinCallback& callback)
    {
        ClimbingSet callees = m_context.restrict(valueAt(callback.callee), m_universe.callables());
        if (!mayBeUnknownObject(callees))
            return callees;
        if (!listsObjectThatLetsInUnseenCode(callback.thisValue) && !listsObjectThatLetsInUnseenCode(callback.spreadArguments) && !std::ranges::any_of(callback.arguments, [&](const BuiltinPlace& argument) { return listsObjectThatLetsInUnseenCode(argument); }))
            return callees;
        forEachOperandAt(callback.callee, [&](const ValueFlowOperand& operand) {
            callees = m_context.unite(callees, builtinFunctionsListedBesideUnknownObject(m_context, operand.node));
        });
        return callees;
    }

    void applyCallback(const BuiltinCallback& callback)
    {
        forEachPartOfCallees(m_context, calleesOf(callback), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, callback.constructs ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode, originOfOperands());
            if (callback.constructs) {
                if (callback.newTarget.kind == BuiltinPlaceKind::None)
                    m_context.add(group.newTarget, callees);
                else {
                    connectFrom(callback.newTarget, group.newTarget);
                    connectFrom(callback.newTarget, m_context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands));
                }
            } else if (callback.thisValue.kind == BuiltinPlaceKind::None)
                m_context.add(group.thisValue, m_universe.undefinedValue());
            else
                connectFrom(callback.thisValue, group.thisValue);
            passArguments(group, callback.arguments);
            connectFrom(callback.spreadArguments, group.otherArguments);
            useResult(group, callback.resultUse, callback.resultTo);
            connectThrown(group, callback.times);
            if (callback.resultUse == BuiltinCallbackResultUse::Stored && resolvesPromise(callback.resultTo)) {
                BuiltinPlace reason = callback.resultTo;
                reason.slot = BuiltinSlots::PromiseReason;
                forEachTargetAt(reason, [&](ValueFlowNodeID held) {
                    m_context.connect(group.thrown, held);
                });
            }
        });
    }

    void applyHandlerCall(const BuiltinHandlerCall& handlerCall)
    {
        forEachPartOfCallees(m_context, m_context.restrict(valueAt(handlerCall.callee), m_universe.callables()), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::CallByNativeCode, originOfOperands());
            if (handlerCall.thisValue.kind == BuiltinPlaceKind::None)
                m_context.add(group.thisValue, m_universe.undefinedValue());
            else
                connectFrom(handlerCall.thisValue, group.thisValue);
            passArguments(group, handlerCall.arguments);
            useResult(group, handlerCall.resultUse, { });
            connectThrown(group, handlerCall.times);
        });
    }

    void forEachKeyOperand(const BuiltinKey& key, NOESCAPE const Invocable<void(const ValueFlowOperand&)> auto& functor) const
    {
        if (key.kind == BuiltinKeyKind::Operand || key.kind == BuiltinKeyKind::ElementsOf)
            forEachOperandAt(*key.operand, functor);
        else
            functor({ });
    }

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
            bool isTaken = false;
            forEachTargetAt(action.value, [&](ValueFlowNodeID target) {
                isTaken = true;
                applyWith({ }, target);
                // A name under which the descriptor is found later is the name of what it describes.
                if (action.value.kind == BuiltinPlaceKind::ResultField && m_call.result)
                    m_context.connect(m_context.masked(target, m_context.maker().unite(m_context.maker().of(ValueFlowUniverse::markOfLoadedBun()), m_context.maker().of(ValueFlowUniverse::markOfMemberLeftOut()))), m_call.result);
            });
            if (!isTaken)
                applyWith({ }, { });
            if (action.value.kind == BuiltinPlaceKind::ResultField && m_resultObject) {
                forEachOperandAt(action.object, [&](const ValueFlowOperand& described) {
                    m_context.connect(described, m_context.written(ClimbingSet(m_context.maker().of(m_resultObject)), describedObjectName));
                });
            }
            return;
        }
        case BuiltinPropertyActionKind::DescribesOwnProperties:
            applyWith({ }, m_call.result);
            listNamedBunOnResult(action.object);
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
            if (action.value.kind == BuiltinPlaceKind::OwnValues)
                forEachOperandAt(*action.value.operand, [&](const ValueFlowOperand& source) {
                    assignFrom(action, source);
                });
            else
                forEachOperandAt(action.value, applyWithValue);
            return;
        case BuiltinPropertyActionKind::DefinesFrom:
            forEachOperandAt(*action.value.operand, applyWithValue);
            return;
        case BuiltinPropertyActionKind::TakesDescriptor:
            if (std::optional<DescriptorReadUnderSameKey> read = findDescriptorReadUnderSameKey(action)) {
                if (read->descriptor)
                    copyDescribedProperties(action, read->descriptor);
                if (!read->isTouched)
                    return;
            }
            forEachOperandAt(action.value, applyWithValue);
            return;
        case BuiltinPropertyActionKind::StoresByKey:
        case BuiltinPropertyActionKind::DefinesName:
        case BuiltinPropertyActionKind::DefinesGetter:
        case BuiltinPropertyActionKind::DefinesSetter:
        case BuiltinPropertyActionKind::ChangesPrototype:
            forEachOperandAt(action.value, applyWithValue);
            return;
        case BuiltinPropertyActionKind::TakesDescriptorMap: {
            bool standsAtOperand = action.value.kind == BuiltinPlaceKind::Operand && !action.value.isSettled;
            bool hasComputedNames = false;
            std::optional<Vector<PropertyKeyID, 16>> names = standsAtOperand ? namesOfLiteralAt(*action.value.operand, &hasComputedNames) : std::nullopt;
            forEachOperandAt(action.value, [&](const ValueFlowOperand& map) {
                forEachOperandAt(action.object, [&](const ValueFlowOperand& object) {
                    defineAsMapDescribes(action, object, map, names, hasComputedNames);
                });
            });
            return;
        }
        case BuiltinPropertyActionKind::DescribesResult: {
            std::optional<Vector<PropertyKeyID, 16>> names = namesOfLiteralAt(action.object);
            ValueFlowOperand result = operandHolding(m_resultObject ? m_context.maker().of(m_resultObject) : m_universe.unknownObjectAlone());
            forEachOperandAt(action.object, [&](const ValueFlowOperand& map) {
                defineAsMapDescribes(action, result, map, names);
            });
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Matches defineProperty(target, key, getOwnPropertyDescriptor(source, key)) with one key value and an untouched descriptor.
    struct DescriptorReadUnderSameKey {
        ValueFlowObjectID descriptor;
        bool isTouched { false };
    };
    std::optional<DescriptorReadUnderSameKey> findDescriptorReadUnderSameKey(const BuiltinPropertyAction& action)
    {
        constexpr uint32_t numberOfOperandsBeforeArguments = 2;
        constexpr OptionSet<ObjectMutation> mutationsOfDescriptor { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty,
            ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation, ObjectMutation::StoreOfOtherType };
        if (!m_call.operation || action.value.kind != BuiltinPlaceKind::Operand || action.value.operand->kind() != BuiltinOperand::Kind::Argument
            || action.key.kind != BuiltinKeyKind::Operand || action.key.operand->kind() != BuiltinOperand::Kind::Argument)
            return std::nullopt;
        const Function& function = m_program.at(m_call.operation->function);
        const Value& definition = function.at(m_call.operation->site);
        uint32_t descriptorIndex = numberOfOperandsBeforeArguments + action.value.operand->argumentIndex();
        uint32_t keyIndex = numberOfOperandsBeforeArguments + action.key.operand->argumentIndex();
        if (definition.opcode != Opcode::Call || descriptorIndex >= definition.numberOfOperands() || keyIndex >= definition.numberOfOperands())
            return std::nullopt;

        ValueID read = definitionThroughTests(function, definition.operand(descriptorIndex));
        if (function.at(read).opcode != Opcode::Call || function.at(read).numberOfOperands() != numberOfOperandsBeforeArguments + 2)
            return std::nullopt;
        const Value& callee = function.at(sameObjectThroughCasts(function, function.at(read).operand(0)));
        if (callee.opcode != Opcode::Builtin)
            return std::nullopt;
        BuiltinID reader = m_builtins.canonicalID(callee.builtin());
        if (reader != Builtins::ObjectGetOwnPropertyDescriptor && reader != Builtins::ReflectGetOwnPropertyDescriptor)
            return std::nullopt;
        if (definitionThroughTests(function, function.at(read).operand(numberOfOperandsBeforeArguments + 1)) != definitionThroughTests(function, definition.operand(keyIndex)))
            return std::nullopt;

        ValueFlowObjectID descriptor;
        bool holdsOtherObject = false;
        forEachOperandAt(action.value, [&](const ValueFlowOperand& value) {
            m_context.forEachMember(m_context.restrict(m_context.valueOf(value), m_universe.objects()), [&](ValueFlowObjectID object) {
                const ValueFlowSite* site = m_universe.siteOf(object);
                if (site && site->origin == ValueFlowSiteOrigin::BuiltinResult && site->value == read && m_universe.functionOf(object) == m_call.operation->function && (!descriptor || descriptor == object))
                    descriptor = object;
                else
                    holdsOtherObject = true;
            });
        });
        if (!descriptor)
            return DescriptorReadUnderSameKey { { }, holdsOtherObject };
        ClimbingSet alone(m_context.maker().of(descriptor));
        bool isTouched = holdsOtherObject || m_context.meet(m_context.markedAmong(alone, mutationsOfDescriptor), ValueSets::anything) || m_context.holdsAnyOf(alone, escaped()) || m_context.holdsAnyOf(alone, ValueFlowProgramNodes::lost());
        return DescriptorReadUnderSameKey { descriptor, isTouched };
    }

    void copyDescribedProperties(const BuiltinPropertyAction& action, ValueFlowObjectID descriptor)
    {
        BuiltinPropertyAction copy = action;
        copy.kind = BuiltinPropertyActionKind::DefinesFrom;
        ValueFlowOperand sources = operandOfBuiltinAt(m_context.read(ClimbingSet(m_context.maker().of(descriptor)), describedObjectName));
        forEachOperandAt(action.object, [&](const ValueFlowOperand& object) {
            Typed::applyPropertyAction(m_context, copy, { object, { }, sources, { }, m_call.thrown });
        });
    }

    void assignFrom(const BuiltinPropertyAction& action, const ValueFlowOperand& source)
    {
        std::span<const ValueFlowOperand> arguments = m_call.arguments;
        bool isArgument = !arguments.empty() && std::greater_equal { }(&source, arguments.data()) && std::less_equal { }(&source, &arguments.back());
        ValueSetID builtinObjects = m_context.maker().intersect(m_universe.builtinObjects(), ValueSets::complement(m_universe.placeholdersAlone()));
        forEachOperandAt(action.object, [&](const ValueFlowOperand& object) {
            ClimbingSet targets = m_context.valueOf(object);
            bool areBuiltinObjects = m_context.meet(targets, ValueSets::anything) && !m_context.meet(targets, ValueSets::complement(builtinObjects));
            defineAsMapDescribes(action, object, source, isArgument && areBuiltinObjects ? namesOfLiteralAt(BuiltinOperand::argument(&source - arguments.data())) : std::nullopt);
        });
    }

    void defineAsMapDescribes(const BuiltinPropertyAction& action, const ValueFlowOperand& object, const ValueFlowOperand& map, const std::optional<Vector<PropertyKeyID, 16>>& namesInMap, bool mapHasComputedNames = false)
    {
        ValueFlowPropertyActionOperands operands { object, { }, map, { }, m_call.thrown };
        if (namesInMap)
            operands.namesInMap = namesInMap->span();
        operands.mapHasComputedNames = mapHasComputedNames;
        Typed::applyPropertyAction(m_context, action, operands);
    }

    std::optional<Vector<PropertyKeyID, 16>> namesOfLiteralAt(BuiltinOperand position, bool* hasComputedNames = nullptr)
    {
        if (!m_call.operation || position.kind() != BuiltinOperand::Kind::Argument)
            return std::nullopt;
        const Function& function = m_program.at(m_call.operation->function);
        ValueID call = m_call.operation->site;
        if (function.at(call).opcode != Opcode::Call && function.at(call).opcode != Opcode::Construct)
            return std::nullopt;
        constexpr uint32_t numberOfOperandsBeforeArguments = 2;
        uint32_t operandIndex = numberOfOperandsBeforeArguments + position.argumentIndex();
        if (operandIndex >= function.at(call).numberOfOperands())
            return std::nullopt;

        auto handsOnItsOperand = [](const Value& value) {
            return isCast(value.opcode) || value.opcode == Opcode::FinishObject;
        };
        ValueID literal = function.at(call).operand(operandIndex);
        while (literal && handsOnItsOperand(function.at(literal)))
            literal = function.at(literal).operand(0);
        if (!literal)
            return std::nullopt;

        Vector<PropertyKeyID, 16> names;
        const Value& creation = function.at(literal);
        switch (creation.opcode) {
        case Opcode::NewDictionary:
            break;
        case Opcode::NewObject:
            for (uint32_t field : creation.fields())
                names.append(m_program.at(creation.layout()).fields[field].key);
            break;
        case Opcode::Data: {
            const auto* object = m_program.at(creation.data()).root.tryAs<DataNode::Object>();
            if (!object)
                return std::nullopt;
            for (const DataMember& member : object->members)
                names.append(member.key);
            break;
        }
        default:
            return std::nullopt;
        }

        CFG cfg(function);
        UseLists uses(function, cfg);
        Vector<ValueID, 8> worklist { literal };
        while (!worklist.isEmpty()) {
            for (const Use& use : uses.usesOf(worklist.takeLast())) {
                if (use.kind != Use::Kind::Operand)
                    return std::nullopt;
                const Value& user = function.at(use.user);
                if (use.user == call && use.operandIndex == operandIndex)
                    continue;
                if (handsOnItsOperand(user) && !use.operandIndex)
                    worklist.append(use.user);
                else if (user.opcode == Opcode::PropDefine && use.operandIndex == OperandIndex::PropDefine::object && user.key())
                    names.append(user.key());
                else if (hasComputedNames && user.opcode == Opcode::ElemDefine && use.operandIndex == OperandIndex::ElemDefine::object)
                    *hasComputedNames = true;
                else
                    return std::nullopt;
            }
        }
        AOT::sort(names);
        names.shrink(names.size() - std::ranges::unique(names).size());
        return names;
    }

    BuiltinID m_builtin;
    const ValueFlowBuiltinCall& m_call;
    ValueSetID m_receiversAccepted { ValueSets::anything };
    ValueFlowRuleContext& m_context;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    const ValueFlowUniverse& m_universe;
    const ValueFlowBuiltinTables& m_tables;
    ValueFlowOperand m_undefined;
    const BuiltinOverload* m_overload { nullptr };
    bool m_handsOnResult { false };
    ValueFlowObjectID m_resultObject;
    ValueFlowObjectID m_contentsObject;
    ValueSetID m_plainDataOfResult;
    ClimbingSet m_objectsMadeBySpecies;
};

ValueFlowBuiltinCall describeBuiltinUse(ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(call);
    bool hasSecondOperand = !isConstruction(kind) || !!context.program().at(call.function).at(call.site).operand(1);
    ValueFlowBuiltinCall description { .kind = kind, .thisValue = operands[hasSecondOperand ? 1 : 0], .result = call.result, .thrown = call.thrown, .resultObject = universe.siteObject(call, 0), .contentsObject = universe.siteObject(call, 1), .operation = &call };
    if (passesList(call.opcode))
        description.otherArguments = operandOfBuiltinAt(context.lookup(context.valueOf(operands[2]), elementsName));
    else
        description.arguments = operands.subspan(2);
    return description;
}

} // anonymous namespace

ValueFlowNodeID nodeReadFromNativeCell(ValueFlowRuleContext& context, BuiltinName cell)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const BuiltinHostObject> records = context.program().builtins().hostObjects();
    for (uint32_t index = 0; index < records.size(); ++index) {
        const BuiltinHostObject& record = records[index];
        if (record.nativeCell != cell)
            continue;
        ValueFlowObjectID object = record.function ? universe.objectOf(record.function) : universe.hostObject(index);
        context.add(nodeWrittenToNativeCell(context, cell), only(context, object));
        if (record.function)
            continue;
        ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByHost };
        BuiltinApplication(Builtins::UndeclaredNativeCode, noCall, context).describeHostObject(record, object);
    }
    return context.read(only(context, universe.objectOf(Builtins::GlobalThis)), nameOfNativeCell(context, cell));
}

static void applyEntryDescribing(ValueFlowObjectID closure, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);

static bool applyValueFromLibrary(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    builtin = builtins.canonicalID(builtin);
    if (!universe.builtinTables().holdsValueFromLibrary(builtin) || call.kind == ValueFlowCallKind::Set)
        return false;

    ValueFlowObjectID entry = universe.objectOf(builtin);
    ValueFlowNodeID value = context.read(only(context, entry), cellOfValueFromLibrary(builtins));
    if (call.kind == ValueFlowCallKind::Get) {
        context.use(builtin, BuiltinUse::Read);
        if (call.result)
            context.connect(value, call.result);
        if (call.thrown)
            context.add(call.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        return !context.holdsAnyOf(only(context, entry), context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins));
    }

    context.use(builtin, modeAndUseOf(call.kind).second);
    ClimbingSet callees = context.restrict(context.valueOf(value), universe.callables());
    if (!isConstruction(call.kind)) {
        ValueSetID describedByEntries = universe.builtinTables().closuresThatEntriesDescribe();
        context.forEachMember(context.restrict(callees, describedByEntries), [&](ValueFlowObjectID closure) {
            applyEntryDescribing(closure, call, context);
        });
        callees = context.restrict(callees, ValueSets::complement(describedByEntries));
    }
    if (call.kind != ValueFlowCallKind::CallByHost)
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), context.restrict(callees, universe.closures()));
    forEachPartOfCallees(context, callees, [&](ClimbingSet part) {
        passOperandsOf(call, context.group(part, call.kind), context);
    });
    return true;
}

void applyHostUses(BuiltinID onBehalfOf, const ValueFlowOperand& value, const BuiltinHostUses& uses, ValueFlowRuleContext& context)
{
    ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByHost };
    BuiltinApplication(onBehalfOf, noCall, context).actWithoutOverload().useByHost(value, uses);
}

void callFromHost(BuiltinID onBehalfOf, const ValueFlowOperand& callees, const BuiltinHostCall& call, ValueFlowRuleContext& context)
{
    ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByHost };
    BuiltinApplication(onBehalfOf, noCall, context).actWithoutOverload().callByHost(callees, call);
}

bool doRecordsDescribe(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    auto claimsEveryUse = [](const BuiltinParameter& parameter) {
        return parameter.uses == OperandUses::all();
    };
    return !overload.isUndeclared() && !builtins.at(overload.result.type).isAnything()
        && !(overload.receiver && claimsEveryUse(*overload.receiver)) && std::ranges::none_of(overload.parameters, claimsEveryUse);
}

ValueFlowObjectID calleeOfDescribedCall(const ValueFlowUniverse& universe)
{
    return universe.resultOf(Builtins::CompilerHelpersTraverseArray, 1);
}

ValueSetID unknownCalleeOfStatement(ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    FunctionID function = context.functionOfStatement();
    if (!function || !isCodeThatRecordsDescribe(universe, function))
        return universe.unknownObjectAlone();
    return context.maker().of(calleeOfDescribedCall(universe));
}

bool isCodeThatRecordsDescribe(const ValueFlowUniverse& universe, FunctionID function)
{
    return universe.builtinTables().isCodeThatRecordsDescribe(function);
}

bool standsBehindDescribedBuiltins(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    const BuiltinTable& builtins = program.builtins();
    RuntimeFunctionID code = function.enclosingRuntimeFunction;
    if (function.role != FunctionRole::Library || !code || !builtins.contains(code) || builtins.at(code).implements.empty())
        return false;
    return std::ranges::all_of(builtins.at(code).implements, [&](BuiltinID builtin) {
        if (builtins.canonicalID(builtin) == Builtins::CompilerHelpersTraverseArray)
            return true;
        return std::ranges::all_of(builtins.overloadsOf(builtins.canonicalID(builtin)), [&](const BuiltinOverload& overload) {
            return overload.code.implementation.function != code || overload.code.model || overload.isUndeclared() || doRecordsDescribe(builtins, overload);
        });
    });
}

bool isCodeBehindDeclaredEntries(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    const BuiltinTable& builtins = program.builtins();
    if (function.role != FunctionRole::Library || !function.module || !program.at(function.module).flags.contains(ModuleFlags::IsLibrary))
        return false;
    RuntimeFunctionID code = function.enclosingRuntimeFunction;
    if (!code)
        return true;
    if (!builtins.contains(code) || builtins.at(code).implements.empty())
        return false;
    return std::ranges::all_of(builtins.at(code).implements, [&](BuiltinID builtin) {
        return std::ranges::none_of(builtins.overloadsOf(builtins.canonicalID(builtin)), [&](const BuiltinOverload& overload) {
            return overload.code.implementation.function == code && overload.isUndeclared();
        });
    });
}

void applyBuiltin(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    if (applyStepOfBuiltinIterator(builtin, call, context) || applyConstructionOfPromise(builtin, call, context) || applyValueFromLibrary(builtin, call, context))
        return;
    BuiltinApplication(builtin, call, context).run();
    holdContentsIteratedByResult(builtin, call, context);
}

void applyBuiltin(BuiltinID builtin, ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context, ValueSetID receivers)
{
    const Program& program = context.program();
    BuiltinID canonical = program.builtins().canonicalID(builtin);
    if (kind == ValueFlowCallKind::Call && (canonical == Builtins::Symbol || canonical == Builtins::SymbolFor)) {
        const Function& function = program.at(call.function);
        const Value& operation = function.at(call.site);
        std::optional<PropertyKey> named = canonical == Builtins::Symbol ? keyOfSymbolsCreatedBy(function, operation) : keyOfSymbolRegisteredBy(program.builtins(), function, operation);
        if (std::optional<PropertyKeyID> key = named ? program.types.find(*named) : std::nullopt) {
            noteCallOfBuiltin(builtin, kind, context);
            if (call.result)
                context.add(call.result, only(context, context.universe().keyConstant(*key)));
            return;
        }
    }
    ValueFlowBuiltinCall description = describeBuiltinUse(kind, call, context);
    description.thisValue.mask = context.maker().intersect(description.thisValue.mask, receivers);
    description.isFoundOnReceiver = receivers != ValueSets::anything;
    applyBuiltin(builtin, description, context);
}

bool listsObjectThatLetsInUnseenCode(ValueFlowRuleContext& context, const ValueFlowOperand& operand)
{
    ValueSetID lettingIn = context.universe().builtinTables().heap().objectsThatLetInUnseenCode();
    return context.meet(ClimbingSet(operand.constant), lettingIn) || (operand.node && context.holdsAnyOf(ClimbingSet(lettingIn), operand.node));
}

// Beside the unknown object the value of a callee leaves out the built-in functions that have escaped. The node lists them. loader.bind(..), loader.call(..): what a function that calls or binds what it is given does with a loader is followed whatever else the callee may be.
ClimbingSet builtinFunctionsListedBesideUnknownObject(ValueFlowRuleContext& context, ValueFlowNodeID callee)
{
    return callee ? context.valueWithin(ClimbingSet(context.universe().builtinTables().heap().functionsThatCallOrBindOperand()), callee) : ClimbingSet();
}

void applyBuiltinFoundThroughUnknownObject(BuiltinID builtin, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    ValueFlowBuiltinCall description = describeBuiltinUse(ValueFlowCallKind::Call, call, context);
    description.thisValue = { { }, context.universe().unknownObjectAlone(), ValueSets::anything };
    description.saysOnlyWhatIsCalled = true;
    BuiltinApplication(builtin, description, context).run();
}

void readSpeciesOfUnknownObject(BuiltinID builtin, ValueFlowRuleContext& context)
{
    ValueFlowOperand unknown { { }, unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction), ValueSets::anything };
    ValueFlowBuiltinCall description { .kind = ValueFlowCallKind::CallByNativeCode, .thisValue = unknown, .otherArguments = unknown, .result = ValueFlowProgramNodes::escaped(), .thrown = ValueFlowProgramNodes::escaped() };
    description.readsOnlySpecies = true;
    BuiltinApplication(builtin, description, context).run();
}

bool raisesEventWhenCalled(const BuiltinTable& builtins, BuiltinID builtin)
{
    builtin = builtins.canonicalID(builtin);
    return eventOfCalling(builtins, builtin, { }).has_value() || std::ranges::contains(builtins.loadersOfMachineCode(), builtin, &BuiltinLoaderOfMachineCode::callee);
}

bool bindsNoArgument(const Program& program, const ValueFlowUniverse& universe, ValueFlowObjectID boundFunction)
{
    const ValueFlowSite* site = universe.siteOf(boundFunction);
    if (!site || site->origin != ValueFlowSiteOrigin::BuiltinResult || site->ordinal)
        return false;
    const Value& operation = program.at(universe.functionOf(boundFunction)).at(site->value);
    return operation.opcode == Opcode::Call && operation.variadicOperands().size() <= 1;
}

static ValueSetID boundFunctionsBindingNoArgumentAmong(ClimbingSet callees, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::atomic<uint64_t>& remembered = universe.rememberedBoundFunctionsBindingNoArgument();
    uint64_t before = remembered.load(std::memory_order_acquire);
    ClimbingSet known = context.restrict(callees, universe.knownObjects());
    ClimbingSet notAskedYet = context.restrict(known, ValueSets::complement(ValueSetID(static_cast<uint32_t>(before >> 32))));
    if (!context.meet(notAskedYet, ValueSets::anything))
        return ValueSetID(static_cast<uint32_t>(before));

    Vector<ValueFlowObjectID, 64> asked;
    Vector<ValueFlowObjectID, 64> found;
    context.forEachMember(notAskedYet, [&](ValueFlowObjectID callee) {
        asked.append(callee);
        if (bindsNoArgument(context.program(), universe, callee))
            found.append(callee);
    });
    ValueSetID askedNow = context.maker().of(asked.span());
    ValueSetID foundNow = context.maker().of(found.span());
    while (true) {
        ValueSetID bindingNone = context.maker().unite(ValueSetID(static_cast<uint32_t>(before)), foundNow);
        uint64_t after = static_cast<uint64_t>(context.maker().unite(ValueSetID(static_cast<uint32_t>(before >> 32)), askedNow).index()) << 32 | bindingNone.index();
        if (remembered.compare_exchange_weak(before, after, std::memory_order_acq_rel, std::memory_order_acquire))
            return bindingNone;
    }
}

BuiltinOverloadID overloadForAnotherNewTarget(const BuiltinTable& builtins, BuiltinID builtin, ValueFlowCallKind kind)
{
    if (!isConstruction(kind) || builtins.canonicalID(builtin) != Builtins::Object)
        return { };
    for (BuiltinOverloadID id : builtins.overloadIDsOf(builtin)) {
        const BuiltinOverload& overload = builtins.at(id);
        if (overload.mode != BuiltinCallMode::Construct || !overload.result.type)
            continue;
        const BuiltinType& type = builtins.at(overload.result.type);
        if (type.freshness == BuiltinFreshness::Yes && type.primitives.isEmpty() && type.parts.size() == 1 && type.parts[0].kind == BuiltinTypePartKind::Instance && type.parts[0].builtinClass == BuiltinClasses::Object)
            return id;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ClimbingSet targetsOf(ClimbingSet boundFunctions, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    auto slot = [&](BuiltinSlotID id) {
        return context.read(boundFunctions, ValueFlowCellName { ValueFlowNameKind::HiddenSlot, id.index() });
    };
    auto lists = [&](const ValueFlowOperand& operand) {
        return listsObjectThatLetsInUnseenCode(context, operand);
    };
    auto holds = [&](BuiltinSlotID id) {
        return lists({ slot(id), ValueSets::nothing, ValueSets::anything });
    };
    ClimbingSet targets = context.restrict(context.valueOf(slot(BuiltinSlots::BoundFunctionTarget)), context.universe().callables());
    if (!context.contains(targets, ValueFlowUniverse::unknownObject()))
        return targets;
    if (!holds(BuiltinSlots::BoundFunctionBoundThis) && !holds(BuiltinSlots::BoundFunctionBoundArguments) && !std::ranges::any_of(description.arguments, lists) && !lists(description.laterArguments) && !lists(description.otherArguments))
        return targets;
    return context.unite(targets, builtinFunctionsListedBesideUnknownObject(context, slot(BuiltinSlots::BoundFunctionTarget)));
}

static void callTargetsWithoutBoundArguments(ClimbingSet callees, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    auto slot = [&](BuiltinSlotID id) {
        return context.read(callees, ValueFlowCellName { ValueFlowNameKind::HiddenSlot, id.index() });
    };
    ClimbingSet targets = targetsOf(callees, description, context);
    ValueFlowBuiltinCall onBoundThis = description;
    onBoundThis.kind = ValueFlowCallKind::CallByNativeCode;
    onBoundThis.thisValue = { slot(BuiltinSlots::BoundFunctionBoundThis), ValueSets::nothing, ValueSets::anything };
    Vector<ValueFlowObjectID, 4> applied;
    context.forEachMember(context.restrict(targets, universe.builtinFunctions()), [&](ValueFlowObjectID target) {
        if (raisesEventWhenCalled(context.program().builtins(), universe.builtinOf(target)))
            return;
        applied.append(target);
        applyBuiltin(universe.builtinOf(target), onBoundThis, context);
    });
    forEachPartOfCallees(context, context.restrict(targets, ValueSets::complement(context.maker().of(applied.span()))), [&](ClimbingSet others) {
        passOperandsOf(onBoundThis, context.group(others, onBoundThis.kind), context);
    });
}

static void callTargetsWithBoundArguments(ClimbingSet callees, ValueFlowCallKind kind, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    auto slot = [&](BuiltinSlotID id) {
        return context.read(callees, ValueFlowCellName { ValueFlowNameKind::HiddenSlot, id.index() });
    };

    auto callTargets = [&](ClimbingSet targets) {
        ValueFlowCallGroup group = context.group(targets, kind);
        if (isConstruction(kind)) {
            context.add(group.newTarget, targets);
            ValueFlowNodeID fromOperands = context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands);
            ClimbingSet newTargets = context.valueOf(description.thisValue);
            ClimbingSet handedOn = context.unite(context.restrict(newTargets, ValueSets::complement(universe.otherCallables())), context.valueWithin(context.restrict(newTargets, universe.otherCallables()), fromOperands));
            context.add(group.newTarget, handedOn);
            context.add(fromOperands, handedOn);
        } else
            context.connect(slot(BuiltinSlots::BoundFunctionBoundThis), group.thisValue);
        context.connect(slot(BuiltinSlots::BoundFunctionBoundArguments), group.otherArguments);
        for (const ValueFlowOperand& argument : description.arguments)
            context.connect(argument, group.otherArguments);
        context.connect(description.laterArguments, group.otherArguments);
        context.connect(description.otherArguments, group.otherArguments);
        if (description.result)
            context.connect(group.result, description.result);
        if (description.thrown)
            context.connect(group.thrown, description.thrown);
    };
    forEachPartOfCallees(context, targetsOf(callees, description, context), [&](ClimbingSet targets) {
        if (!isConstruction(kind))
            return callTargets(targets);
        // `new` on a bound function enters its target with that target as new.target, and no other.
        context.forEachMember(targets, [&](ValueFlowObjectID target) {
            callTargets(only(context, target));
        });
    });
}

// Each bound function enters its own target, with its own receiver and its own arguments, whichever others are called at the same place.
static void callOtherCallables(ClimbingSet callees, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    ValueFlowCallKind kind = isConstruction(description.kind) ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode;
    ValueSetID bindingNoArgument = isConstruction(kind) ? ValueSets::nothing : boundFunctionsBindingNoArgumentAmong(callees, context);
    auto callTargetsOf = [&](ClimbingSet boundFunctions) {
        if (ClimbingSet bare = context.restrict(boundFunctions, bindingNoArgument); context.meet(bare, ValueSets::anything))
            callTargetsWithoutBoundArguments(bare, description, context);
        if (ClimbingSet others = context.restrict(boundFunctions, ValueSets::complement(bindingNoArgument)); context.meet(others, ValueSets::anything))
            callTargetsWithBoundArguments(others, kind, description, context);
    };
    if (std::optional<ClimbingSet> boundFunctions = context.holdersUnder({ ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() }, callees)) {
        context.forEachMember(*boundFunctions, [&](ValueFlowObjectID boundFunction) {
            callTargetsOf(only(context, boundFunction));
        });
    }
    settlePromisesThrough(callees, description, context);
}

void enterCodeThatEntryDescribes(ValueFlowObjectID closure, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    BuiltinID entry = universe.builtinTables().entryDescribing(closure);
    ValueSetID passed = context.maker().unite(universe.anyPrimitive(), context.maker().of(universe.resultOf(entry, 0)));
    ValueFlowCallGroup group = context.group(only(context, closure), ValueFlowCallKind::CallByHost);
    context.add(group.thisValue, passed);
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        context.add(group.argument(index), passed);
    context.add(group.laterArguments, passed);
    context.add(group.otherArguments, passed);
}

static void applyEntryDescribing(ValueFlowObjectID closure, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    BuiltinID entry = context.universe().builtinTables().entryDescribing(closure);
    ValueFlowBuiltinCall description = call;
    description.resultObject = { };
    if (!description.contentsObject)
        description.contentsObject = context.universe().resultOf(entry, 1);
    BuiltinApplication(entry, description, context).run();
    if (call.kind != ValueFlowCallKind::CallByHost)
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(context, closure));
    enterCodeThatEntryDescribes(closure, context);
}

void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    applyEntryDescribing(closure, describeBuiltinUse(kind, call, context), context);
}

void connectEnumerableMembersOf(ClimbingSet hostObjects, ValueFlowNodeID to, ValueFlowRuleContext& context)
{
    forEachNodeReadByWalkOver(context, hostObjects, { }, [&](ValueFlowNodeID members) {
        context.connect(members, to);
    });
}

// An argument, a result or the contents of an object put a value into hands nobody sees. A receiver is only what a member was found on.
void carryOut(ValueFlowRuleContext& context, ValueFlowNodeID values)
{
    context.connect(values, context.nodes().ofHost(ValueFlowHostNode::ValuesCarriedOut));
}

void carryOut(ValueFlowRuleContext& context, const ValueFlowOperand& values)
{
    context.connect(values, values.maskKind == ValueFlowMaskKind::Plain ? context.nodes().ofHost(ValueFlowHostNode::ValuesCarriedOut) : ValueFlowProgramNodes::escaped());
}

void noteCallOfBuiltin(BuiltinID builtin, ValueFlowCallKind kind, ValueFlowRuleContext& context)
{
    context.use(builtin, modeAndUseOf(kind).second);
    noteCallThatRunsOtherCode(builtin, context);
}

void noteInstancesIn(ValueFlowRuleContext& context, BuiltinTypeID type)
{
    const BuiltinTable& builtins = context.program().builtins();
    Vector<ValueFlowObjectID, 16> prototypes;
    Vector<BuiltinTypeID, 8> types { type };
    for (size_t index = 0; index < types.size(); ++index) {
        if (!types[index])
            continue;
        for (const BuiltinTypePart& part : builtins.at(types[index]).parts) {
            for (BuiltinClassID current = part.builtinClass; current; current = builtins.at(current).parent) {
                if (BuiltinID prototype = builtins.at(current).prototype)
                    prototypes.append(context.universe().objectOf(prototype));
            }
            for (const BuiltinSlotContents& contents : part.contents)
                types.appendIfNotContains(contents.type);
        }
    }
    context.add(context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances), context.maker().of(prototypes.span()));
}

void callOtherCallables(ClimbingSet callees, ValueFlowCallKind kind, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    if (isConstruction(kind) && context.program().at(call.function).at(call.site).operand(1))
        context.connect(context.operandsOf(call)[1], context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands));
    callOtherCallables(callees, describeBuiltinUse(kind, call, context), context);
}

static void withDescriptionOf(ValueFlowCallKind kind, const ValueFlowCallGroup& group, NOESCAPE const Invocable<void(ValueFlowBuiltinCall&)> auto& functor)
{
    std::array<ValueFlowOperand, numberOfGroupedArguments> arguments;
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        arguments[index] = operandOfBuiltinAt(group.argument(index));
    ValueFlowBuiltinCall description { .kind = kind, .thisValue = operandOfBuiltinAt(isConstruction(kind) ? group.newTarget : group.thisValue), .arguments = arguments, .argumentsMayBeAbsent = true,
        .otherArguments = operandOfBuiltinAt(group.otherArguments), .laterArguments = operandOfBuiltinAt(group.laterArguments), .result = group.result, .thrown = group.thrown };
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

void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind kind, const ValueFlowCallGroup& group, ValueFlowRuleContext& context)
{
    withDescriptionOf(kind, group, [&](ValueFlowBuiltinCall& description) {
        applyEntryDescribing(closure, description, context);
    });
}

void callOtherCallables(ClimbingSet callees, ValueFlowCallKind kind, const ValueFlowCallGroup& group, ValueFlowRuleContext& context)
{
    withDescriptionOf(kind, group, [&](ValueFlowBuiltinCall& description) {
        callOtherCallables(callees, description, context);
    });
}

static void runGetterOfPrototypeProperty(ValueFlowRuleContext& context, const ValueFlowOperand& constructor, ValueFlowNodeID thrown)
{
    const ValueFlowUniverse& universe = context.universe();
    Vector<ValueFlowObjectID, 8> bornWithoutProperty;
    context.forEachMember(context.restrict(context.valueOf(constructor), universe.closures()), [&](ValueFlowObjectID closure) {
        if (!hasPrototypeProperty(context.program(), context.program().at(universe.closureFunction(closure))))
            bornWithoutProperty.append(closure);
    });
    if (bornWithoutProperty.isEmpty())
        return;
    ValueSetID asked = context.maker().of(bornWithoutProperty.span());
    auto find = [&](ClimbingSet constructors) {
        return context.valueOf(context.lookup(constructors, ValueFlowName::getter(context.program().keyOf(BuiltinNames::prototype))));
    };
    ReceiversByWhatTheyFind receivers(context, operandHolding(asked), find);
    forEachPartOfCallees(context, context.restrict(receivers.withCalleesFoundAlone(find(ClimbingSet(asked))), universe.callables()), [&](ClimbingSet getters) {
        ValueFlowCallGroup group = context.group(getters, ValueFlowCallKind::Get);
        receivers.passReceiversOf(getters, group.thisValue);
        if (thrown)
            context.connect(group.thrown, thrown);
    });
}

static void testAgainstConstructor(ValueFlowRuleContext& context, const ValueFlowOperand& tested, const ValueFlowOperand& constructor, ValueFlowNodeID thrown, const ValueFlowStatement* operation)
{
    const ValueFlowUniverse& universe = context.universe();
    auto find = [&](ClimbingSet constructors) {
        return context.valueOf(context.lookup(constructors, property(context, BuiltinNames::hasInstanceSymbol)));
    };
    ReceiversByWhatTheyFind receivers(context, constructor, find);
    ClimbingSet methods = context.restrict(receivers.withCalleesFoundAlone(find(context.valueOf(constructor))), universe.callables());

    ClimbingSet builtinFunctions = context.restrict(methods, universe.builtinFunctions());
    if (operation)
        applyEventsOfCall(*operation, context, builtinFunctions);
    ClimbingSet otherConstructors = context.restrict(context.valueOf(constructor), ValueSets::complement(context.maker().unite(universe.builtinFunctions(), universe.closures())));
    ValueFlowBuiltinCall application { .kind = ValueFlowCallKind::CallByNativeCode, .arguments = std::span(&tested, 1), .thrown = thrown };
    context.forEachMember(builtinFunctions, [&](ValueFlowObjectID method) {
        BuiltinID builtin = universe.builtinOf(method);
        bool isInheritedByEveryFunction = context.program().builtins().canonicalID(builtin) == Builtins::FunctionPrototypeSymbolHasInstance;
        if (isInheritedByEveryFunction && !context.meet(otherConstructors, ValueSets::anything))
            return;
        application.thisValue = isInheritedByEveryFunction ? context.operandHolding(otherConstructors) : constructor;
        application.resultObject = universe.resultOf(builtin, 0);
        application.contentsObject = universe.resultOf(builtin, 1);
        applyBuiltin(builtin, application, context);
    });
    if (context.meet(builtinFunctions, ValueSets::anything))
        runGetterOfPrototypeProperty(context, constructor, thrown);

    forEachPartOfCallees(context, context.restrict(methods, ValueSets::complement(universe.builtinFunctions())), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName);
        receivers.passReceiversOf(callees, group.thisValue);
        context.connect(tested, group.argument(0));
        if (thrown)
            context.connect(group.thrown, thrown);
    });
}

void runInstanceOf(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    if (statement.result)
        context.add(statement.result, context.maker().of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::False), ValueFlowUniverse::primitive(PrimitiveSet::True) }));
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
    testAgainstConstructor(context, operands[0], operands[1], statement.thrown, &statement);
}

static void forEachReceiverToldApart(ValueFlowRuleContext& context, ClimbingSet receivers, const ValueFlowOperand& holdingReceivers, NOESCAPE const Invocable<void(ClimbingSet, const ValueFlowOperand&)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectsToldApart objects = tellObjectsApart(context, context.restrict(receivers, universe.knownObjects()));
    for (ValueSetID group : objects.groups)
        functor(ClimbingSet(group), operandHolding(group));
    if (objects.rest != ValueSets::nothing)
        functor(ClimbingSet(objects.rest), operandHolding(objects.rest));
    ValueSetID others = ValueSets::complement(universe.knownObjects());
    if (!context.meet(receivers, others))
        return;
    ValueFlowOperand holdingOthers = holdingReceivers;
    holdingOthers.mask = context.maker().intersect(holdingOthers.mask, others);
    functor(context.restrict(receivers, others), holdingOthers);
}

void letNativeCodeTestAgainst(ValueFlowRuleContext& context, const ValueFlowOperand& constructors)
{
    ValueFlowOperand tested = context.operandHolding(ClimbingSet(unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction)));
    forEachReceiverToldApart(context, context.valueOf(constructors), constructors, [&](ClimbingSet, const ValueFlowOperand& constructor) {
        testAgainstConstructor(context, tested, constructor, ValueFlowProgramNodes::escaped(), nullptr);
    });
}

static ClimbingSet chainOfProgramFrom(ValueFlowRuleContext& context, ClimbingSet objects)
{
    const ValueFlowUniverse& universe = context.universe();
    return context.restrict(context.valueOf(context.lookup(objects, prototypeLinkName)), context.maker().intersect(universe.objects(), ValueSets::complement(universe.builtinObjects())));
}

static void readMembersOfReceivers(ValueFlowRuleContext& context, ValueFlowNodeID hub, const ValueFlowBehavior& behavior, ClimbingSet receivers)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = withWhatUntrackedObjectCovers(context, context.restrict(receivers, universe.objects()));
    if (behavior.isTransitive) {
        context.connect(context.readWhatWasWritten(objects, everyPropertyName), hub);
        context.connect(enumerablePropertiesNobodyWrote(context, objects, MetBy::NativeCode), hub);
        context.connect(context.readWhatWasWritten(objects, copiedFromName), hub);
        context.connect(context.lookup(context.restrict(objects, context.maker().intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects()))), elementsName), hub);
    }
    if (behavior.isTransitive && behavior.protocols.contains(Protocol::HasInstance))
        context.connect(context.read(objects, ValueFlowCellName { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() }), hub);
    if (behavior.isTransitive && behavior.protocols.containsAll({ Protocol::OwnValues, Protocol::PrototypeChain }))
        context.add(hub, context.restrict(context.valueOf(context.readWhatWasWritten(chainOfProgramFrom(context, objects), everyPropertyName)), ValueSets::complement(universe.callables())));
    ClimbingSet hostObjects = behavior.protocols.contains(Protocol::OwnValues) ? context.valueWithin(ClimbingSet(universe.builtinTables().heap().holdersOfMembersThatHostMakes()), hub) : ClimbingSet();
    if (behavior.isTransitive)
        hostObjects = withHostObjectsAmongMembers(context, hostObjects);
    forEachNodeReadByWalkOver(context, hostObjects, behavior.protocols, [&](ValueFlowNodeID members) {
        if (behavior.isTransitive)
            context.connect(members, hub);
    });
    if (!behavior.protocols.contains(Protocol::Named))
        return;

    if (behavior.namedMembers.empty() && behavior.isTransitive)
        context.connect(context.readWhatWasWritten(chainOfProgramFrom(context, objects), everyPropertyName), hub);
    for (BuiltinName name : behavior.namedMembers) {
        ValueFlowNodeID member = context.lookup(receivers, property(context, name));
        if (behavior.isTransitive) {
            context.connect(member, hub);
            continue;
        }
        if (behavior.protocols.contains(Protocol::ToPrimitive))
            context.connect(member, context.hub(universe.builtinTables().conversionToPrimitive()));
    }
}

// Beside the unknown object the members that have escaped are not listed.
static ClimbingSet withEscapedBuiltinsUnder(ValueFlowRuleContext& context, ClimbingSet found, BuiltinName name)
{
    if (!context.contains(found, ValueFlowUniverse::unknownObject()))
        return found;
    return context.unite(found, context.restrict(escapedMembersUnder(context, context.program().keyOf(name)), context.universe().builtinFunctions()));
}

static ClimbingSet membersToCall(ValueFlowRuleContext& context, ClimbingSet receivers, BuiltinName name)
{
    const ValueFlowUniverse& universe = context.universe();
    Vector<ValueFlowObjectID, 4> made;
    context.forEachMember(context.restrict(receivers, universe.knownObjects()), [&](ValueFlowObjectID receiver) {
        const ValueFlowSite* site = universe.siteOf(receiver);
        if (site && site->origin == ValueFlowSiteOrigin::Operation && isCodeThatRecordsDescribe(universe, universe.functionOf(receiver)))
            made.append(receiver);
    });
    if (made.isEmpty())
        return withEscapedBuiltinsUnder(context, context.valueOf(context.lookup(receivers, property(context, name))), name);
    ValueSetID madeInDescribedCode = context.maker().of(made.span());
    ClimbingSet found = context.restrict(context.valueOf(context.lookup(ClimbingSet(madeInDescribedCode), property(context, name))), ValueSets::complement(universe.unknownObjectAlone()));
    return context.unite(found, withEscapedBuiltinsUnder(context, context.valueOf(context.lookup(context.restrict(receivers, ValueSets::complement(madeInDescribedCode)), property(context, name))), name));
}

static ClimbingSet withoutWhatBuiltinsMadeAsNoFunction(ValueFlowRuleContext& context, ClimbingSet values)
{
    const ValueFlowBuiltinTables& tables = context.universe().builtinTables();
    ValueFlowObjectID classesOfData[] { tables.objectStandingFor(BuiltinClasses::Object), tables.objectStandingFor(BuiltinClasses::Array) };
    ValueSetID made = objectsAllocatedByBuiltinsAs(context, values, context.maker().of(std::span<const ValueFlowObjectID>(classesOfData)), false);
    return made == ValueSets::nothing ? values : context.restrict(values, ValueSets::complement(made));
}

static bool passesOnlyPrimitives(const BuiltinTable& builtins, const BuiltinNamedMemberCall& record)
{
    return std::ranges::all_of(record.arguments, [&](const BuiltinPlace& argument) {
        if (argument.kind != BuiltinPlaceKind::NewValue || !argument.newValueType)
            return false;
        const BuiltinType& type = builtins.at(argument.newValueType);
        return !type.isAnything() && type.parts.empty() && !type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData });
    });
}

static bool isCalledWithFunctionsOfStartupModules(ValueFlowRuleContext& context, BuiltinName name)
{
    return std::ranges::contains(context.program().builtins().namesOfHostMethodsCallingStartupLoads(), name);
}

bool mayBeFoundOnObjectNobodyKnows(ValueFlowRuleContext& context, BuiltinName name)
{
    const ValueFlowUniverse& universe = context.universe();
    if (context.meet(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::StoredUnderNamesOfMethodsCallingStartupLoads)), universe.callables()))
        return true;
    const ValueFlowHeapTables& heap = universe.builtinTables().heap();
    const ValueFlowMembersUnderKey& members = heap.under(context.program().keyOf(name));
    ClimbingSet prototypes(context.maker().unite(members.prototypeObjects, context.maker().intersect(members.holdersOfUndeclaredProperties, heap.prototypeObjectsOfHost())));
    return context.holdsAnyOf(prototypes, context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins))
        || context.holdsAnyOf(prototypes, ValueFlowProgramNodes::escaped());
}

static void callMembersOfReceivers(ValueFlowRuleContext& context, ValueFlowNodeID hub, const ValueFlowBehavior& behavior, ClimbingSet receivers, const ValueFlowOperand& thisValue)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID handedOut = behavior.isTransitive ? hub : ValueFlowNodeID();
    enum class Passes : bool { Primitives, ValuesNobodyDescribes };
    auto call = [&](BuiltinName name, Passes passes, ValueFlowNodeID result) {
        ClimbingSet members = membersToCall(context, receivers, name);
        if (!context.meet(members, universe.callables()))
            return;
        callFromNativeCode(context, members, thisValue, passes == Passes::Primitives ? universe.anyPrimitive() : unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult), result, [&](ClimbingSet receiver) {
            return membersToCall(context, receiver, name);
        });
    };
    auto take = [&](ValueFlowNodeID node) {
        if (handedOut)
            context.connect(node, handedOut);
    };

    ClimbingSet objects = withWhatUntrackedObjectCovers(context, context.restrict(receivers, universe.objects()));
    auto runGettersOn = [&](NOESCAPE const Invocable<ClimbingSet(ClimbingSet)> auto& holdersOf) {
        forEachPartOfCallees(context, context.restrict(context.valueOf(context.readWhatWasWritten(holdersOf(objects), everyGetterName)), universe.callables()), [&](ClimbingSet getters) {
            ValueFlowCallGroup group = context.group(getters, ValueFlowCallKind::Get);
            if (context.contains(getters, ValueFlowUniverse::unknownObject())) {
                context.forEachMember(context.restrict(objects, universe.knownObjects()), [&](ValueFlowObjectID receiver) {
                    if (context.contains(context.valueOf(context.readWhatWasWritten(holdersOf(only(context, receiver)), everyGetterName)), ValueFlowUniverse::unknownObject()))
                        context.add(group.thisValue, only(context, receiver));
                });
                ValueFlowOperand holdingOthers = thisValue;
                holdingOthers.mask = context.maker().intersect(holdingOthers.mask, ValueSets::complement(universe.knownObjects()));
                context.connect(holdingOthers, group.thisValue);
            } else
                context.connect(thisValue, group.thisValue);
            carryOut(context, group.thrown);
            take(group.result);
        });
    };

    if (behavior.protocols.contains(Protocol::OwnValues)) {
        runGettersOn([&](ClimbingSet holders) {
            return context.unite(holders, withSourcesOfCopies(context, context.restrict(holders, universe.knownObjects())));
        });
    }
    if (behavior.protocols.contains(Protocol::ToPrimitive)) {
        ValueFlowOperand holdingConverted = thisValue;
        holdingConverted.mask = context.maker().intersect(holdingConverted.mask, universe.objects());
        ClimbingSet converted = context.restrict(receivers, universe.objects());
        for (BuiltinName name : { BuiltinNames::toPrimitiveSymbol, BuiltinNames::valueOf, BuiltinNames::toString }) {
            callFromNativeCode(context, membersToCall(context, converted, name), holdingConverted, universe.anyPrimitive(), { }, [&](ClimbingSet receiver) {
                return membersToCall(context, receiver, name);
            });
        }
    }
    if (behavior.protocols.contains(Protocol::ToJSON))
        call(BuiltinNames::toJSON, Passes::Primitives, handedOut ? handedOut : ValueFlowProgramNodes::escaped());
    if (behavior.protocols.contains(Protocol::HasInstance))
        call(BuiltinNames::hasInstanceSymbol, Passes::ValuesNobodyDescribes, { });
    if (behavior.protocols.contains(Protocol::Thenable))
        take(context.lookup(receivers, awaitedValueName));
    if (behavior.protocols.contains(Protocol::Iterate))
        take(context.lookup(receivers, iteratedValues));
    if (behavior.protocols.contains(Protocol::Named) && behavior.namedMembers.empty())
        runGettersOn([&](ClimbingSet holders) {
            return chainOfProgramFrom(context, holders);
        });
    if (behavior.protocols.contains(Protocol::CallsNamed)) {
        bool callsSomething = false;
        if (behavior.calledMembers.empty()) {
            ClimbingSet methods = context.valueOf(context.readWhatWasWritten(chainOfProgramFrom(context, objects), everyPropertyName));
            if (context.meet(methods, universe.callables())) {
                callFromNativeCode(context, methods, thisValue, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult), handedOut ? handedOut : ValueFlowProgramNodes::escaped(), [&](ClimbingSet receiver) {
                    return context.valueOf(context.readWhatWasWritten(chainOfProgramFrom(context, withWhatUntrackedObjectCovers(context, receiver)), everyPropertyName));
                });
                callsSomething = true;
            }
        }
        for (BuiltinName name : behavior.calledMembers) {
            ValueFlowNodeID result = handedOut ? handedOut : ValueFlowProgramNodes::escaped();
            ClimbingSet members = withoutWhatBuiltinsMadeAsNoFunction(context, context.valueOf(context.lookup(receivers, property(context, name))));
            const BuiltinNamedMemberCall* record = context.program().builtins().callOfMemberNamed(name);
            if (record) {
                ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByNativeCode };
                BuiltinApplication(Builtins::UndeclaredNativeCode, noCall, context).callNamedMember(members, thisValue, record->arguments, result, [&](ClimbingSet receiver) {
                    return withoutWhatBuiltinsMadeAsNoFunction(context, context.valueOf(context.lookup(receiver, property(context, name))));
                });
            } else
                call(name, Passes::ValuesNobodyDescribes, result);
            if (!isCalledWithFunctionsOfStartupModules(context, name) || (record && passesOnlyPrimitives(context.program().builtins(), *record)))
                continue;
            callsSomething |= context.meet(members, context.maker().intersect(universe.callables(), ValueSets::complement(universe.placeholdersAlone())))
                || (context.meet(members, universe.placeholdersAlone()) && mayBeFoundOnObjectNobodyKnows(context, name));
        }
        if (callsSomething)
            letHostChooseOperands(context);
    }
}

static void forEachNameCalledIn(const ValueFlowBehavior& behavior, NOESCAPE const Invocable<void(BuiltinName)> auto& functor)
{
    if (behavior.protocols.contains(Protocol::ToPrimitive)) {
        for (BuiltinName name : { BuiltinNames::toPrimitiveSymbol, BuiltinNames::valueOf, BuiltinNames::toString })
            functor(name);
    }
    if (behavior.protocols.contains(Protocol::ToJSON))
        functor(BuiltinNames::toJSON);
    if (behavior.protocols.contains(Protocol::HasInstance))
        functor(BuiltinNames::hasInstanceSymbol);
    if (behavior.protocols.contains(Protocol::CallsNamed)) {
        for (BuiltinName name : behavior.calledMembers)
            functor(name);
    }
}

static void callLibraryMethodsOfUnknownReceiver(ValueFlowRuleContext& context, ValueFlowNodeID hub, const ValueFlowBehavior& behavior)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    Vector<ValueFlowObjectID, 4> methods;
    auto find = [&](BuiltinName name) {
        ClimbingSet prototypes(universe.builtinTables().heap().under(context.program().keyOf(name)).prototypeObjects);
        ClimbingSet ofClassesWithInstances = context.unite(context.valueWithin(prototypes, context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), context.valueWithin(prototypes, ValueFlowProgramNodes::escaped()));
        context.forEachMember(ofClassesWithInstances, [&](ValueFlowObjectID prototype) {
            BuiltinID method = builtins.findOwnMember(builtins.canonicalID(universe.builtinOf(prototype)), name);
            if (!method)
                return;
            bool isWrittenInJavaScript = std::ranges::any_of(builtins.overloadsOf(builtins.canonicalID(method)), [](const BuiltinOverload& overload) {
                return overload.mode == BuiltinCallMode::Call && overload.code.implementation.function;
            });
            if (isWrittenInJavaScript)
                methods.append(universe.objectOf(method));
        });
    };
    forEachNameCalledIn(behavior, find);
    if (methods.isEmpty())
        return;
    callFromNativeCode(context, ClimbingSet(context.maker().of(methods.span())), operandHolding(universe.unknownObjectAlone()), universe.anyPrimitive(), behavior.isTransitive ? hub : ValueFlowProgramNodes::escaped(), [](ClimbingSet) {
        return ClimbingSet();
    });
}

// The object nobody knows stands for the receivers that have escaped, and a set that holds it lists none of them. Their functions of the program run as whatever has escaped runs.
// A function of the table that one of them finds goes on doing to it what its row says.
static void applyBuiltinsThatActOnEscapedReceivers(ValueFlowRuleContext& context, ValueFlowNodeID hub, const ValueFlowBehavior& behavior)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ValueSetID acting = universe.builtinFunctionsThatActOnReceiver();
    forEachNameCalledIn(behavior, [&](BuiltinName name) {
        bool isNameOfOne = false;
        context.forEachMember(ClimbingSet(universe.builtinTables().heap().under(context.program().keyOf(name)).prototypeObjects), [&](ValueFlowObjectID prototype) {
            BuiltinID method = builtins.findOwnMember(builtins.canonicalID(universe.builtinOf(prototype)), name);
            isNameOfOne |= method && context.contains(ClimbingSet(acting), universe.objectOf(method));
        });
        if (!isNameOfOne)
            return;
        auto find = [&](ClimbingSet receivers) {
            return context.restrict(membersToCall(context, receivers, name), acting);
        };
        forEachReceiverToldApart(context, context.valueWithin(ClimbingSet(universe.knownObjects()), hub), { }, [&](ClimbingSet receivers, const ValueFlowOperand& thisValue) {
            callFromNativeCode(context, find(receivers), thisValue, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult), behavior.isTransitive ? hub : ValueFlowProgramNodes::escaped(), find);
        });
    });
}

// Every chain that ends at Object.prototype finds a constructor there at the latest.
static bool mayLackConstructor(ValueFlowRuleContext& context, ClimbingSet objects)
{
    const ValueFlowUniverse& universe = context.universe();
    if (context.meet(context.valueOf(context.lookup(objects, property(context, BuiltinNames::constructor))), universe.primitives()))
        return true;
    ClimbingSet chain = context.valueOf(context.lookup(objects, prototypeLinkName));
    if (context.meet(chain, context.maker().unite(ValueSets::complement(universe.knownObjects()), universe.objectsBornWithoutPrototype())))
        return true;
    ClimbingSet below = context.restrict(chain, ValueSets::complement(context.maker().of(universe.objectOf(Builtins::ObjectPrototype))));
    return context.meet(context.valueOf(context.readWhatWasWritten(below, prototypeLinkName)), universe.primitives());
}

// The message names the constructor of the value. A value without one is printed by the formatter.
static void describeRefusedOperands(ValueFlowRuleContext& context, ValueFlowNodeID hub)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowBuiltinTables& tables = universe.builtinTables();
    ClimbingSet described = context.restrict(context.valueOf(hub), universe.objects());
    ValueFlowNodeID constructors = context.lookup(described, property(context, BuiltinNames::constructor));
    context.connect(context.lookup(context.valueOf(constructors), property(context, BuiltinNames::name)), context.hub(tables.conversionToPrimitive()));

    ValueFlowNodeID formatter = context.hub(tables.formatterOfMessages());
    forEachReceiverToldApart(context, described, operandOfBuiltinAt(hub, universe.objects()), [&](ClimbingSet toldApart, const ValueFlowOperand& holding) {
        if (mayLackConstructor(context, toldApart))
            context.connect(holding, formatter);
    });
}

void runBehavior(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ValueFlowNodeID hub = statement.result;
    const ValueFlowBehavior& behavior = context.universe().builtinTables().at(context.nodes().behaviorOf(hub));
    if (behavior.describesRefusedOperands)
        return describeRefusedOperands(context, hub);

    ValueSetID mask = everythingButUnknownObject(context);
    ClimbingSet receivers = context.restrict(context.valueOf(hub), mask);
    readMembersOfReceivers(context, hub, behavior, receivers);
    forEachReceiverToldApart(context, receivers, operandOfBuiltinAt(hub, mask), [&](ClimbingSet toldApart, const ValueFlowOperand& thisValue) {
        callMembersOfReceivers(context, hub, behavior, toldApart, thisValue);
    });
    if (!context.contains(context.valueOf(hub), ValueFlowUniverse::unknownObject()))
        return;
    callLibraryMethodsOfUnknownReceiver(context, hub, behavior);
    applyBuiltinsThatActOnEscapedReceivers(context, hub, behavior);
    if (!behavior.protocols.contains(Protocol::CallsNamed))
        return;
    const BuiltinTable& builtins = context.program().builtins();
    bool noMemberGetsMoreThanPrimitives = !behavior.calledMembers.empty() && std::ranges::all_of(behavior.calledMembers, [&](BuiltinName name) {
        const BuiltinNamedMemberCall* record = builtins.callOfMemberNamed(name);
        return !isCalledWithFunctionsOfStartupModules(context, name) || (record && passesOnlyPrimitives(builtins, *record)) || !mayBeFoundOnObjectNobodyKnows(context, name);
    });
    if (!noMemberGetsMoreThanPrimitives)
        letHostChooseOperands(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
