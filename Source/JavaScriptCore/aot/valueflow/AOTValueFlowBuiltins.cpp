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
#include "AOTTypeRelations.h"
#include "AOTUseLists.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueFlowTypedResults.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <functional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr ValueFlowName elements = ValueFlowName::of(ValueFlowNameKind::Elements);
constexpr ValueFlowName unknownKey = ValueFlowName::of(ValueFlowNameKind::UnknownKey);
constexpr ValueFlowName everyProperty = ValueFlowName::of(ValueFlowNameKind::EveryProperty);
constexpr ValueFlowName everyGetter = ValueFlowName::of(ValueFlowNameKind::EveryGetter);
constexpr ValueFlowName everySetter = ValueFlowName::of(ValueFlowNameKind::EverySetter);
constexpr ValueFlowName copiedFrom = ValueFlowName::of(ValueFlowNameKind::CopiedFrom);
constexpr ValueFlowName prototypeLink = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
constexpr ValueFlowName allocatedAs = ValueFlowName::of(ValueFlowNameKind::AllocatedAs);
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

// A walk that goes on from what it finds comes from the global object to `process`. What it reads from the global object holds the unknown object, and beside that the normal form leaves out whatever has escaped.
// The table says which members of an object of the host are such objects themselves.
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

// The host defines these as not enumerable, so whoever goes over the enumerable properties of their holder passes them by: Object.keys, values, entries and assign, a spread, JSON.stringify.
// Each is made by code of the library when it is first read: by its name, under a key nobody knows on the holder itself, or for a descriptor.
// The host's formatter, behind console.log and Bun.inspect, shows what is not enumerable too, on the object and on what lies above it.
static constexpr BuiltinID membersThatWalksPassBy[] { Builtins::Console_stdout, Builtins::Console_stderr };

// What native code reads that goes over the own properties of objects of the table.
static void forEachNodeReadByWalkOver(ValueFlowRuleContext& context, ClimbingSet holders, Protocols protocols, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    if (protocols.contains(Protocol::PrototypeChain)) {
        if (context.meet(holders, ValueSets::anything))
            functor(context.lookup(holders, everyProperty));
        return;
    }
    Vector<ValueFlowObjectID, 2> holdersOfSuchMembers;
    for (BuiltinID member : membersThatWalksPassBy) {
        ValueFlowObjectID holder = universe.objectOf(builtins.at(member).holder);
        if (context.contains(holders, holder) && !holdersOfSuchMembers.contains(holder))
            holdersOfSuchMembers.append(holder);
    }
    ClimbingSet others = context.restrict(holders, ValueSets::complement(context.maker().of(holdersOfSuchMembers.span())));
    if (context.meet(others, ValueSets::anything))
        functor(context.lookup(others, everyProperty));
    for (ValueFlowObjectID holder : holdersOfSuchMembers) {
        // What the program has put there is enumerable for all that anybody knows.
        functor(context.read(only(context, holder), everyProperty));
        for (BuiltinID member : builtins.membersOf(universe.builtinOf(holder))) {
            if (builtins.at(member).name && !std::ranges::contains(membersThatWalksPassBy, builtins.canonicalID(member)))
                functor(context.lookup(only(context, holder), property(context, builtins.at(member).name)));
        }
    }
}

ValueSetID everythingButUnknownObject(ValueFlowRuleContext& context)
{
    return ValueSets::complement(context.universe().unknownObjectAlone());
}

ValueSetID unknownObject(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    return context.maker().intersect(unknownValue(context, source), context.universe().objects());
}

// Whoever reads what the objects hold without a lookup reads it from each object that the untracked object stands for.
ClimbingSet withWhatUntrackedObjectCovers(ValueFlowRuleContext& context, ClimbingSet objects)
{
    if (!context.contains(objects, ValueFlowUniverse::untrackedObject()))
        return objects;
    return context.unite(objects, context.valueWithin(ClimbingSet(context.universe().knownObjects()), ValueFlowProgramNodes::lost()));
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
    case ValueFlowCallKind::CallByHost:
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
    case ValueFlowCallKind::CallByHost:
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

// A call that native code makes on its own, of a member that it has found under a name.
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

    forEachPartOfCallees(context, context.restrict(call.callees, universe.callables()), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName);
        context.connect(call.thisValue, group.thisValue);
        context.add(group.otherArguments, call.arguments);
        // The call that has handed the object to native code throws the unknown value.
        context.connect(group.thrown, ValueFlowProgramNodes::escaped());
        if (call.result)
            context.connect(group.result, call.result);
    });
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
        m_context.use(m_builtin, useOf(m_call.kind));

        std::optional<BuiltinDispatchTarget> root = m_builtins.dispatchOf(m_builtin, modeOf(m_call.kind));
        if (!root) {
            throwValue(unknownObject(m_context, ValueFlowUnknownSource::ThrownByRunTime));
            // A function that no row lets be constructed refuses `new`. But a module that native code makes puts each of its functions as its own constructor (putNativeFn in Bun's _NativeModule.h):
            // `new (require("node:util/types").isDate)(value)` answers a boolean. A walk over the 1,560 functions that can be reached from the global object and the modules has found no other.
            if (isConstruction(m_call.kind) && isPutByNativeModule()) {
                actWithoutOverload();
                handToUndescribedNativeCode(BuiltinOperand::argumentsFrom(0));
            }
            return;
        }

        notePropertiesThatRowsAskFor();
        Vector<BuiltinOverloadID, 4> selected;
        if (BuiltinOverloadID whateverIsPassed = overloadForAnotherNewTarget(m_builtins, m_builtin, m_call.kind)) {
            // In a construction the receiver is new.target. A placeholder may stand for the constructor itself.
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
        for (BuiltinOverloadID overload : selected)
            apply(m_builtins.at(overload));
        if (mayGetThisThatDeclaredOverloadsLeaveOut())
            applyWhatIsNotDeclared(selected.span());
    }

    bool isPutByNativeModule() const
    {
        BuiltinID holder = m_builtins.at(m_builtins.canonicalID(m_builtin)).holder;
        return holder && m_context.sets().contains(m_tables.heap().objectsOfNativeModules(), m_universe.objectOf(m_builtins.canonicalID(holder)));
    }

    void applyHostUses(const ValueFlowOperand& value, const BuiltinHostUses& uses)
    {
        actWithoutOverload();
        useByHost(value, uses);
    }

    void callFromHost(const ValueFlowOperand& callees, const BuiltinHostCall& call)
    {
        actWithoutOverload();
        callByHost(callees, call);
    }

    // Native code finds the member on an operand and calls it on that operand. The protocol says what becomes of the answer.
    void callNamedMember(ClimbingSet members, const ValueFlowOperand& thisValue, std::span<const BuiltinPlace> arguments, ValueFlowNodeID result)
    {
        actWithoutOverload();
        forEachPartOfCallees(m_context, m_context.restrict(members, m_universe.callables()), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName);
            m_context.connect(thisValue, group.thisValue);
            passArguments(group, arguments);
            passNoFurtherArguments(m_context, group, arguments.size());
            m_context.connect(group.thrown, escaped());
            if (result)
                m_context.connect(group.result, result);
        });
    }

    void describeHostObject(const BuiltinHostObject& record, ValueFlowObjectID object)
    {
        actWithoutOverload();
        ClimbingSet self = only(m_context, object);
        if (record.prototype)
            m_context.add(m_context.written(self, prototypeLink), only(m_context, m_universe.objectOf(record.prototype)));
        for (const BuiltinHostObjectField& field : record.fields) {
            if (field.name) {
                connectFrom(field.value, m_context.written(self, property(m_context, field.name)));
                continue;
            }
            connectFrom(field.value, m_context.written(self, unknownKey));
            connectFrom(field.value, m_context.written(self, elements));
            m_context.mark(self, ObjectMutation::AddProperty);
        }
        // A field is named by its number, and one node holds what is stored under any number. Only what code of the library stores lies there: what native code puts into a field by itself, which no record lists, lies in a slot, where a read of the field finds it too.
        // An object of which no record lists an internal field has none.
        if (record.internalFields.empty())
            return;
        m_context.add(m_context.written(self, cellOfValueFromLibrary(m_builtins)), unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
        ValueFlowName internalFields = ValueFlowName::of(ValueFlowNameKind::InternalFields);
        for (const BuiltinHostObjectInternalField& field : record.internalFields)
            useByHost(operandAt(m_context.written(self, internalFields)), field.uses);
    }

private:
    static constexpr ValueFlowNodeID escaped() { return ValueFlowProgramNodes::escaped(); }
    static constexpr ValueFlowNodeID lost() { return ValueFlowProgramNodes::lost(); }

    void actWithoutOverload()
    {
        static const NeverDestroyed<BuiltinOverload> noOverload;
        m_overload = &noOverload.get();
        m_handsOnResult = false;
        m_resultObject = { };
        m_contentsObject = m_universe.resultOf(m_builtin, 1);
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
                used = operandAt(m_context.lookup(m_context.valueOf(used), m_program.isIndexLike(key) ? elements : ValueFlowName::property(key)));
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
        // The group of the host's calls is how the code behind a described entry is entered. A call that a record lists is a call like any other: the overloads say what it does with its operands.
        ClimbingSet callables = m_context.restrict(m_context.valueOf(callee), m_universe.callables());
        callByHost(m_context.restrict(callables, ValueSets::complement(m_tables.closuresThatEntriesDescribe())), ValueFlowCallKind::CallByHost, call);
        callByHost(m_context.restrict(callables, m_tables.closuresThatEntriesDescribe()), ValueFlowCallKind::CallByNativeCode, call);
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
            m_context.connect(group.thrown, escaped());
            useByHost(operandAt(group.result), call.result);
        });
    }

    // Native code hands the value to a built-in, as a call of the program would.
    void passToBuiltin(const ValueFlowOperand& value, BuiltinID builtin, unsigned argumentIndex)
    {
        ValueFlowCallGroup group = m_context.group(only(m_context, m_universe.objectOf(builtin)), ValueFlowCallKind::CallByHost);
        BuiltinID holder = m_builtins.at(builtin).holder;
        if (holder && isObjectKind(m_builtins.at(holder).kind))
            m_context.add(group.thisValue, only(m_context, m_universe.objectOf(holder)));
        m_context.connect(value, group.argumentAt(argumentIndex));
        m_context.connect(group.result, escaped());
        m_context.connect(group.thrown, escaped());
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

    // Native code does more with the result than answer it: a callback gets it as `this` or as an argument, or it is put somewhere.
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

    // No node stands for a result that the program drops: `setTimeout(callback, 0);`. Native code has the result all the same, and the timer is the `this` of the callback.
    // Where the overload hands the result on, one cell on the built-in's own object stands for the dropped results of all its calls.
    bool keepsDroppedResult() const { return !m_call.result && m_handsOnResult; }
    ValueFlowNodeID nodeTakingResult() { return keepsDroppedResult() ? m_context.written(only(m_context, m_universe.objectOf(m_builtin)), cellOfDroppedResults(m_builtins)) : m_call.result; }
    ValueFlowNodeID nodeHoldingResult() { return keepsDroppedResult() ? m_context.read(only(m_context, m_universe.objectOf(m_builtin)), cellOfDroppedResults(m_builtins)) : m_call.result; }

    // The declared overloads of such an entry say what it does with its holder as `this`. Array.from, Array.of and Promise.resolve construct a class of the program that they find there.
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
            if (overload.mode != modeOf(m_call.kind) || !overload.isUndeclared())
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
    ClimbingSet known(ClimbingSet objects) { return m_context.restrict(objects, m_universe.knownObjects()); }
    ClimbingSet unknownObjectAlone() const { return ClimbingSet(m_universe.unknownObjectAlone()); }
    ClimbingSet untrackedObjectAlone() const { return ClimbingSet(m_universe.untrackedObjectAlone()); }
    bool mayBeUnknownObject(ClimbingSet objects) const { return m_context.contains(objects, ValueFlowUniverse::unknownObject()); }
    bool mayBeUntrackedObject(ClimbingSet objects) const { return m_context.contains(objects, ValueFlowUniverse::untrackedObject()); }
    bool holdsAnything(ClimbingSet set) const { return m_context.meet(set, ValueSets::anything); }

    // What is stored through a placeholder lies in one node for each name. Only what is stored through the unknown object escapes.
    void forEachNodeWrittenThrough(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        functor(m_context.written(known(objects), name));
        if (mayBeUntrackedObject(objects))
            functor(m_context.written(untrackedObjectAlone(), name));
        if (!mayBeUnknownObject(objects))
            return;
        noteStoreThroughUnknownObject(m_context, name);
        functor(m_context.written(unknownObjectAlone(), name));
        functor(escaped());
    }

    // What a spread or Object.assign has copied onto an object is written nowhere on it: it lies on the objects that it was copied from, and on those that something was copied from onto them.
    // A lookup by name goes there as it goes to what the object inherits from. Whoever takes all that an object holds has to go there too.
    // `state = { ...state, a }` copies from what it has made itself: the walk ends where a step finds no object that it has not seen. The answer holds every source, so it grows as the sets grow.
    ClimbingSet withSourcesOfCopies(ClimbingSet objects)
    {
        ClimbingSet all = objects;
        ClimbingSet last = known(objects);
        for (;;) {
            last = known(m_context.valueOf(m_context.read(last, copiedFrom)));
            bool findsNewSource = false;
            m_context.forEachMember(last, [&](ValueFlowObjectID source) {
                findsNewSource |= !m_context.contains(all, source);
            });
            if (!findsNewSource)
                return all;
            all = m_context.unite(all, last);
        }
    }

    // An object that has escaped may hold what was stored through the unknown object, and one that flow has stopped following what was stored through the untracked object.
    // The untracked object itself may be any of the latter.
    void forEachNodeReadThrough(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        // A spread has values alone: the getters of a source have run when it was made. A definition by the descriptors of a source has its accessors too.
        if (name == everyProperty || name == everyGetter || name == everySetter)
            objects = withSourcesOfCopies(objects);
        bool answersByName = mayBeUntrackedObject(objects);
        functor(m_context.read(answersByName ? m_context.unite(known(objects), m_context.valueWithin(ClimbingSet(m_universe.knownObjects()), lost())) : known(objects), name));
        if (answersByName || m_context.holdsAnyOf(objects, escaped()))
            functor(m_context.read(unknownObjectAlone(), name));
        if (answersByName || m_context.holdsAnyOf(objects, lost()))
            functor(m_context.read(untrackedObjectAlone(), name));
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
            return admitsAbsence || mayHold(m_call.otherArguments) || (index >= m_call.arguments.size() && mayHold(m_call.laterArguments));
        }
        case BuiltinOperand::Kind::ArgumentsFrom:
            // The branch is taken when every argument that is passed lies in the tested type.
            return m_call.argumentsMayBeAbsent || std::ranges::all_of(m_call.arguments.subspan(std::min<size_t>(position.argumentIndex(), m_call.arguments.size())), mayHold);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The sets tell no integer from another. A literal that the call spells out says which one it is.
    bool isIntegerLiteralWithin(BuiltinOperand position, BuiltinIntegerRange range) const
    {
        if (!m_call.operation || position.kind() != BuiltinOperand::Kind::Argument || position.argumentIndex() >= m_call.arguments.size() || range.isEmpty())
            return false;
        const Function& function = m_program.at(m_call.operation->function);
        std::span<const ValueID> arguments = function.at(m_call.operation->site).variadicOperands();
        if (position.argumentIndex() >= arguments.size())
            return false;
        const Value& argument = function.at(definitionThroughUpcasts(function, arguments[position.argumentIndex()]));
        return argument.opcode == Opcode::Integer && argument.integer() >= range.minimum && argument.integer() <= range.maximum;
    }

    // Among what the operand holds: what built-ins have allocated as instances of these classes and of no other. With the prototype object of one of them above it, it also behaves as it was made.
    // The node that an allocation writes is asked itself: it is filled in the run that hands the object out, so before the object has come anywhere. What nothing is written about is sure of nothing.
    ValueSetID allocatedByBuiltinsAs(BuiltinOperand position, ValueSetID classes, bool hasPrototypeOfClass)
    {
        if (classes == ValueSets::nothing)
            return ValueSets::nothing;
        ValueSetID others = ValueSets::complement(classes);
        Vector<ValueFlowObjectID, 8> allocated;
        m_context.forEachMember(m_context.restrict(valueAt(position), m_context.maker().intersect(m_universe.objectsThatMayBeAnything(), m_universe.knownObjects())), [&](ValueFlowObjectID object) {
            ClimbingSet named = m_context.valueOf(m_context.written(only(m_context, object), allocatedAs));
            if (!holdsAnything(named) || m_context.meet(named, others))
                return;
            if (hasPrototypeOfClass && m_context.meet(m_context.valueOf(m_context.read(only(m_context, object), prototypeLink)), others))
                return;
            allocated.append(object);
        });
        return m_context.maker().of(allocated.span());
    }

    void noteAllocationAs(BuiltinClassID builtinClass, ValueFlowObjectID object)
    {
        ValueFlowObjectID standing = builtinClass ? m_tables.objectStandingFor(builtinClass) : ValueFlowObjectID();
        m_context.add(m_context.written(only(m_context, object), allocatedAs), only(m_context, standing ? standing : object));
    }

    // What every overload below the target refuses as the receiver. A built-in looks at its receiver before it does anything else, so for such a receiver it throws and that is all.
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
        // The mark of a location that nothing has been stored into is no value of the language. It lies in the set of a variable from the start, and no call passes it: a read that would find it throws.
        ValueSetID stillTested = ValueSets::complement(m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Absent)));
        bool absenceIsStillTested = true;
        for (const BuiltinDispatchBranch& branch : node.branches) {
            bool admitsAbsence = m_builtins.at(branch.tested).isAnything() || m_builtins.at(branch.tested).primitives.contains(BuiltinPrimitive::NotPassed);
            ValueSetID tested = m_context.maker().intersect(m_tables.maskOf(branch.tested), stillTested);
            if (node.operand == BuiltinOperand::receiver()) {
                ValueSetID refused = m_context.maker().intersect(tested, receiversRefusedBelow(branch.target));
                if (refused != ValueSets::nothing && mayLieIn(node.operand, refused, false))
                    throwValue(unknownObject(m_context, ValueFlowUnknownSource::ThrownByRunTime));
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

            // An instance passes a test for pristine instances until something takes the behavior away that it was made with.
            ValueSetID whilePristine = m_context.maker().unite(m_tables.membersOfWhilePristine(branch.tested), allocatedByBuiltinsAs(node.operand, m_tables.classesOfWhilePristine(branch.tested), true));
            if (whilePristine == ValueSets::nothing)
                continue;
            constexpr OptionSet<ObjectMutation> mutationsOfBuiltinBehavior { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
            ClimbingSet changed = m_context.markedAmong(m_context.restrict(valueAt(node.operand), whilePristine), mutationsOfBuiltinBehavior);
            if (!m_context.meet(changed, ValueSets::anything))
                stillTested = m_context.maker().intersect(stillTested, ValueSets::complement(whilePristine));
        }
    }

    // The getters of Temporal and of Intl. Each looks at its receiver first and throws for whatever is no instance of its class. Of an instance it answers what the engine keeps for it:
    // it stores nothing that a program sees, keeps nothing and calls nothing.
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

        m_overload = &overload;
        m_handsOnResult = false;
        m_resultObject = { };
        m_contentsObject = m_universe.resultOf(m_builtin, 1);
        bool callsValueNobodyKnows = overload.isUndeclared()
            || std::ranges::any_of(overload.flow.callbacks, [&](const BuiltinCallback& callback) { return mayBeUnknownObject(valueAt(callback.callee)); })
            || std::ranges::any_of(overload.flow.handlerCalls, [&](const BuiltinHandlerCall& handlerCall) { return mayBeUnknownObject(valueAt(handlerCall.callee)); });
        if (callsValueNobodyKnows)
            letWhateverHasEscapedBeCalled(m_context);
    }

    void apply(const BuiltinOverload& overload)
    {
        if (isGetterOfEngineWithoutRow(overload)) {
            answer(unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
            throwValue(unknownObject(m_context, ValueFlowUnknownSource::ThrownByRunTime));
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
                return;
            }
        }

        m_overload = &overload;
        m_handsOnResult = handsOnResult(overload);
        // A call without objects of its own shares those of the built-in with every other such call.
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
            // In a construction this is new.target. What the constructor makes for a class of the program has the prototype object of that class above it, and the unknown object stands only for what has escaped.
            handToUndescribedNativeCode(BuiltinOperand::receiver());
        }
        for (const BuiltinParameter& parameter : overload.parameters)
            applyParameter(parameter);
        if (!takesKeysAndValuesFromPairs()) {
            for (const BuiltinTransfer& transfer : overload.flow.transfers)
                connect(transfer.from, transfer.to);
        }
        for (const BuiltinCallback& callback : overload.flow.callbacks)
            applyCallback(callback);
        for (const BuiltinHandlerCall& handlerCall : overload.flow.handlerCalls)
            applyHandlerCall(handlerCall);
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
            applyPropertyAction(action);
        m_plainDataOfResult = { };
        if (ValueFlowNodeID result = nodeTakingResult())
            m_context.add(result, answersObjectsOfNativeCell(overload) ? m_tables.primitivesOf(overload.result.type) : valueOfType(overload.result.type, m_resultObject));
        if (m_call.operation)
            allocateTypedResult(overload, *m_call.operation, m_call, m_resultObject, m_plainDataOfResult, m_context);
        // A constructor whose answer nobody follows, as that of `Function`. What it makes for a class of the program has the prototype object of that class above it, and the unknown object stands only for what has escaped.
        if (const BuiltinType& answer = m_builtins.at(overload.result.type); isConstruction(m_call.kind) && (answer.isAnything() || answer.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction })))
            m_context.connect(m_context.lookup(m_context.restrict(m_context.valueOf(m_call.thisValue), ValueSets::complement(m_universe.builtinObjects())), property(m_context, BuiltinNames::prototype)), escaped());

        if (overload.effects.capability == BuiltinCapability::JS)
            throwValue(unknownValue(m_context, ValueFlowUnknownSource::ThrownByRunTime));
        else if (overload.throws())
            throwValue(unknownObject(m_context, ValueFlowUnknownSource::ThrownByRunTime));

        // The engine's own native code and the helpers of the compiler call no function of the host's library. What hangs on the promise that a call of the host may answer comes in with the built-in.
        const Builtin& entry = m_builtins.at(m_builtin);
        if (overload.isUndeclared() && entry.origin == BuiltinOrigin::Host && !entry.flags.contains(BuiltinFlag::IsCompilerHelper))
            letUndeclaredNativeCodeRun(m_context);
    }

    // $toClass(function, name, base) puts the function below the base, and gives it a new prototype object below that of the base. The model of the table would make one object for all calls:
    // every class of the library would have it, a store to `Readable.prototype` would be one to `EventEmitter.prototype`, and a name that two classes define would be known of neither.
    // The object that the function was born with stands for the new one. Only the library names $toClass, and there the call stands in front of every read of `prototype`.
    void makeClass()
    {
        std::span<const ValueFlowOperand> arguments = m_call.arguments;
        if (arguments.size() >= 3) {
            const ValueFlowOperand& function = arguments[0];
            const ValueFlowOperand& base = arguments[2];
            ValueFlowName prototype = property(m_context, BuiltinNames::prototype);
            BuiltinPropertyAction change { .kind = BuiltinPropertyActionKind::ChangesPrototype, .object = BuiltinOperand::argument(0) };
            Typed::applyPropertyAction(m_context, change, { function, { }, base, { }, m_call.thrown });
            Typed::applyPropertyAction(m_context, change, { operandAt(m_context.lookup(m_context.valueOf(function), prototype)), { }, operandAt(m_context.lookup(m_context.valueOf(base), prototype)), { }, m_call.thrown });
        }
        answer(m_universe.undefinedValue());
        throwValue(unknownObject(m_context, ValueFlowUnknownSource::ThrownByRunTime));
    }

    // Object.entries makes a new array of new pairs, at the call: the model of the table would make one array and one pair for all calls. The own values of the operand go into the pairs and nowhere else.
    // Whoever reads the own values runs the getters of this operand on this operand: in the hub of the behavior it would stand beside the operands of all other calls.
    void makeEntries()
    {
        ClimbingSet array = only(m_context, m_resultObject);
        ClimbingSet pair = only(m_context, m_contentsObject);
        for (ValueFlowObjectID object : { m_resultObject, m_contentsObject }) {
            noteAllocationAs(BuiltinClasses::Array, object);
            m_context.add(m_context.written(only(m_context, object), prototypeLink), only(m_context, m_universe.objectOf(Builtins::ArrayPrototype)));
            writeNamesDefinedOnNewInstance(BuiltinClasses::Array, object);
        }
        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::PairsOfEntries), pair);
        m_context.add(m_context.written(array, elements), pair);
        BuiltinPlace ownValues { .kind = BuiltinPlaceKind::OwnValues, .operand = BuiltinOperand::argument(0) };
        for (ValueFlowName name : { elements, keyOfEntriesPair })
            m_context.add(m_context.written(pair, name), m_universe.strings());
        for (ValueFlowName name : { elements, valueOfEntriesPair })
            connectFrom(ownValues, m_context.written(pair, name));
        connectOperands(BuiltinOperand::argument(0), m_context.written(pair, sourcesOfEntriesPair));
        answer(m_context.maker().of(m_resultObject));
        throwValue(unknownValue(m_context, ValueFlowUnknownSource::ThrownByRunTime));
    }

    // The constructor of a Map takes element 0 of each entry for a key and element 1 for its value, and the records let all that an entry holds be either. Where every entry is a pair that Object.entries has made
    // and that nobody has touched since, the two are what the pair was made with. Whatever arrives later, another entry, a store into a pair, a pair in unknown hands, brings the records back, which say more.
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

    // Making a proxy runs no code. The proxy is the unknown object, and whoever holds it reaches the target and the handler through it: both are in unknown hands like the proxy.
    // What is read, stored or called through the proxy is read, stored or called through the unknown object, by the statement that does it. Nobody holds a proxy that the program drops.
    void makeProxy()
    {
        throwValue(unknownObject(m_context, ValueFlowUnknownSource::ThrownByRunTime));
        if (!m_call.result)
            return;
        connectOperands(BuiltinOperand::argument(0), escaped());
        connectOperands(BuiltinOperand::argument(1), escaped());
        // A built-in object of the realm behind a proxy is held by the program as an object nobody knows: what compiled code stores through the proxy arrives there.
        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.restrict(objectsAt(BuiltinOperand::argument(0)), m_universe.objectsOfRealm()));
        // No statement spells out the call of a trap. The run time finds it on the handler under the name of a member of Reflect.
        for (BuiltinID trap : m_builtins.membersOf(Builtins::Reflect)) {
            m_context.forEachMember(m_context.restrict(escapedMembersUnder(m_context, m_program.keyOf(m_builtins.at(trap).name)), m_universe.builtinFunctions()), [&](ValueFlowObjectID function) {
                noteCallOfBuiltin(m_universe.builtinOf(function), ValueFlowCallKind::CallByNativeCode, m_context);
            });
        }
        // A store through the proxy and a change of its integrity reach the target as definitions of its properties.
        m_context.mark(objectsAt(BuiltinOperand::argument(0)), ObjectMutation::RedefineProperty);
        answer(unknownObject(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
    }

    // Where an instance of a class may exist, native code does with it what hangs on the constructor and on the prototype object. So the rows of both count from the first overload that is applied
    // and may answer such an instance. Nobody has named the two objects, so this is no use of them.
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

        // What the run time does with a promise takes no further call, so whoever may make one counts.
        bool isCall = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Construct;
        bool runsOtherCode = overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
        bool answersAnything = m_builtins.at(overload.result.type).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject });
        if (m_builtins.at(m_builtin).origin == BuiltinOrigin::Host && (isCall || overload.isUndeclared()) && runsOtherCode && answersAnything)
            appendHoldersOf(BuiltinClasses::Promise);

        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins), m_context.maker().of(holdersOfRows.span()));
        noteInstancesIn(overload.result.type);
        // Nothing says what native code without a row answers.
        if (overload.isUndeclared() && m_builtins.at(m_builtin).origin == BuiltinOrigin::Host)
            m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances), m_tables.heap().prototypeObjectsOfHost());
    }

    // An instance of each class that the type names, or that something in such an instance is said to hold, may exist from here on.
    void noteInstancesIn(BuiltinTypeID type)
    {
        Vector<ValueFlowObjectID, 16> prototypes;
        Vector<BuiltinTypeID, 8> types { type };
        for (size_t index = 0; index < types.size(); ++index) {
            if (!types[index])
                continue;
            for (const BuiltinTypePart& part : m_builtins.at(types[index]).parts) {
                for (BuiltinClassID current = part.builtinClass; current; current = m_builtins.at(current).parent) {
                    if (BuiltinID prototype = m_builtins.at(current).prototype)
                        prototypes.append(m_universe.objectOf(prototype));
                }
                for (const BuiltinSlotContents& contents : part.contents)
                    types.appendIfNotContains(contents.type);
            }
        }
        m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances), m_context.maker().of(prototypes.span()));
    }

    // Whether the objects, or what lies above them, may have a property under the name. An object nobody knows may have any.
    bool mayHaveProperty(ClimbingSet holders, BuiltinName name)
    {
        PropertyKeyID key = m_program.keyOf(name);
        ClimbingSet chain = m_context.valueOf(m_context.lookup(m_context.restrict(holders, m_universe.objects()), prototypeLink));
        if (mayBeUnknownObject(chain) || mayBeUntrackedObject(chain))
            return true;
        bool isWritten = false;
        for (ValueFlowName written : { ValueFlowName::property(key), ValueFlowName::getter(key), ValueFlowName::setter(key) }) {
            forEachNodeReadThrough(chain, written, [&](ValueFlowNodeID node) {
                isWritten |= holdsAnything(m_context.valueOf(node));
            });
        }
        // The lookup knows what the table and the layouts declare. It answers undefined for a name that is not there.
        return isWritten || m_context.meet(m_context.valueOf(m_context.lookup(holders, ValueFlowName::property(key))), ValueSets::complement(m_universe.undefinedValue()));
    }

    // The sets tell one string from another only where it names a property. A literal that the call spells out says which one it is, and any other operand may be any text.
    bool mayHaveText(const BuiltinCondition& condition) const
    {
        BuiltinOperand position = condition.operand;
        if (!m_call.operation || position.kind() != BuiltinOperand::Kind::Argument || position.argumentIndex() >= m_call.arguments.size())
            return true;
        const Function& function = m_program.at(m_call.operation->function);
        std::span<const ValueID> arguments = function.at(m_call.operation->site).variadicOperands();
        if (position.argumentIndex() >= arguments.size())
            return true;
        const Value& argument = function.at(definitionThroughUpcasts(function, arguments[position.argumentIndex()]));
        if (argument.opcode != Opcode::String)
            return true;
        std::span<const uint8_t> text = m_program.strings[argument.string()];
        auto isWholeText = [&](BuiltinName listed) {
            return std::ranges::equal(text, m_builtins.textOf(listed));
        };
        auto isStartOfText = [&](BuiltinName listed) {
            std::span<const uint8_t> start = m_builtins.textOf(listed);
            return text.size() >= start.size() && std::ranges::equal(text.first(start.size()), start);
        };
        return std::ranges::any_of(condition.isOneOf, isWholeText) || std::ranges::any_of(condition.startsWith, isStartOfText);
    }

    // The row is about one entry, so it takes a call whose receivers list that entry, however it came there. Beside the unknown object the normal form leaves out an object that has escaped,
    // so the node is asked for the object itself. The untracked object stands for what the analysis has stopped following.
    // That a method of some name is called on a receiver nobody knows says nothing about the entry: it is not taken for it. Should it be the entry at run time,
    // what hangs on the row was not compiled and throws the error that names it where native code comes to call it.
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

    // A row of the table may hold only for a call whose operand has a property, or is one of some texts. That some call passes such an operand is kept with the built-in, in a cell of native code
    // that has the name of the property, or the first of the texts: the rules of the host and the results ask for it there.
    void notePropertiesThatRowsAskFor()
    {
        auto noteIn = [&](BuiltinName cell) {
            m_context.add(m_context.written(only(m_context, m_universe.objectOf(m_builtin)), nameOfNativeCell(cell)), m_universe.undefinedValue());
        };
        auto note = [&](const BuiltinCondition* condition) {
            if (!condition)
                return;
            if (condition->hasProperty && mayHaveProperty(valueAt(condition->operand), condition->hasProperty))
                noteIn(condition->hasProperty);
            if (condition->firstText() && mayHaveText(*condition) && mayBeCalledOn(condition->calledOn))
                noteIn(condition->firstText());
        };
        BuiltinID builtin = m_builtins.canonicalID(m_builtin);
        for (const BuiltinModuleLoad& load : m_builtins.moduleLoadsOf(builtin))
            note(load.condition);
        for (const BuiltinCalledFunction& called : m_builtins.calledFunctionsOf(builtin))
            note(called.condition);
        for (const BuiltinFeatureUse& use : m_builtins.featureUsesOf(builtin))
            note(use.condition);
    }

    // Whoever calls an object that a built-in has made finds a bound function in it or nothing. Any other function that native code makes, and a proxy, is the unknown object.
    bool isCalledInWaysNobodyFollows(const BuiltinTypePart& part) const
    {
        if (part.kind != BuiltinTypePartKind::Instance || part.builtinClass == BuiltinClasses::BoundFunction)
            return false;
        return isFunctionClass(m_builtins, part.builtinClass);
    }

    // The objects that such a call answers are the ones in the cell, which the transfer brings. The type says which primitives there are beside them.
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

    // The records of a declared overload say what a call does with the operands of that call. The function of the library that runs is one for all calls: through it every array that is
    // mapped anywhere would meet every callback. Its body is followed only where the records say nothing about an operand or about the result.
    RuntimeFunctionID functionThatSaysWhatCallDoes(const BuiltinOverload& overload) const
    {
        // What a construction makes is made at the call, with the prototype that new.target says. A model makes one object for all calls.
        if (overload.code.model && !isConstruction(m_call.kind))
            return overload.code.model;
        return doRecordsDescribe(m_builtins, overload) ? RuntimeFunctionID() : overload.libraryFunction();
    }

    // The body is one for all calls: an operand of this call stands there beside those of every other call, and beside the unknown object it is no longer listed once it has escaped.
    // A read that the records declare holds for the operands of this very call, whenever the rule looks: `Reflect.get(process, "getBuiltinModule")`.
    void applyReadsIntoResult(const BuiltinOverload& overload)
    {
        if (overload.isUndeclared() || overload.code.model)
            return;
        m_overload = &overload;
        m_handsOnResult = false;
        m_resultObject = { };
        m_contentsObject = m_universe.resultOf(m_builtin, 1);
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            if (action.kind == BuiltinPropertyActionKind::ReadsByKey && action.value.kind == BuiltinPlaceKind::Result)
                applyPropertyAction(action);
        }
    }

    void enter(FunctionID function)
    {
        ValueFlowCallGroup group = m_context.group(only(m_context, m_universe.closureOf(function)), m_call.kind);
        m_context.connect(m_call.thisValue, isConstruction(m_call.kind) ? group.newTarget : group.thisValue);
        if (isConstruction(m_call.kind))
            m_context.connect(m_call.thisValue, m_context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands));
        for (unsigned index = 0; index < m_call.arguments.size(); ++index)
            m_context.connect(m_call.arguments[index], group.argumentAt(index));
        m_context.connect(m_call.laterArguments, group.laterArguments);
        m_context.connect(m_call.otherArguments, group.otherArguments);
        passNoFurtherArguments(m_context, group, m_call.argumentsMayBeAbsent || m_call.otherArguments.node ? 0 : m_call.arguments.size());
        if (m_call.result)
            m_context.connect(group.result, m_call.result);
        if (m_call.thrown)
            m_context.connect(group.thrown, m_call.thrown);

        // The body calls what it was handed. Where it also runs for calls that records describe, its call of a callee nobody knows says nothing: see callOperandOfDescribedCall().
        // No record speaks for this call, so it says here that code runs which nobody compiles.
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
        // Native code reaches the holder of its own method without being handed it. The function of a native binding hangs on no object of the realm.
        bool isCalledOnItsHolder = operand.isReceiver() && m_builtins.at(m_builtin).holder != Builtins::NativeFunctions;
        ValueSetID handed = isCalledOnItsHolder ? ValueSets::complement(m_universe.builtinObjects()) : ValueSets::anything;
        forEachOperandAt(operand, [&](const ValueFlowOperand& passed) {
            if (operand.isReceiver())
                m_context.add(escaped(), m_context.restrict(m_context.valueOf(passed), handed));
            else
                m_context.connect(passed, escaped());
        });
        if (m_context.meet(valueAt(operand), m_context.maker().intersect(m_universe.objects(), handed))) {
            letUnknownCodeWrite(m_context);
            letWhateverHasEscapedBeCalled(m_context);
        }
        answer(unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
        throwValue(unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
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
                // A list is read index by index: an object above may hold the element, or a getter that answers it.
                m_context.connect(m_context.lookup(m_context.restrict(known(objectsAt(parameter.operand)), ValueSets::complement(m_universe.builtinObjects())), elements), hub);
            } else if (m_context.runsCodeThatRecordsDescribe()) {
                // A hub does nothing to the unknown object but say that the host chooses what it calls on it. Here the object is an operand of a call whose records have said so, if it is so.
                forEachOperandAt(parameter.operand, [&](ValueFlowOperand operand) {
                    operand.mask = m_context.maker().intersect(operand.mask, everythingButUnknownObject(m_context));
                    m_context.connect(operand, hub);
                });
            } else
                connectOperands(parameter.operand, hub);
            // The hub does not list an object of the host that had escaped before it arrived beside the unknown object. What the operand names is read here, where nothing stands beside it.
            if (parameter.uses.contains(OperandUse::AppliesTransitively) && parameter.protocols.contains(Protocol::OwnValues)) {
                ClimbingSet hostObjects = withHostObjectsAmongMembers(m_context, m_context.restrict(valueAt(parameter.operand), m_tables.heap().holdersOfMembersThatHostMakes()));
                forEachNodeReadByWalkOver(m_context, hostObjects, parameter.protocols, [&](ValueFlowNodeID members) {
                    m_context.connect(members, hub);
                });
            }
        }
        if (parameter.protocols.contains(Protocol::Species))
            constructSpecies(parameter.operand);

        // The built-in may return what a member of the operand holds, and an unknown result stands only for what has escaped.
        if (parameter.protocols.containsAny({ Protocol::Named, Protocol::CallsNamed }) && mayReturnUnknownObject(m_builtins.at(m_overload->result.type))) {
            ClimbingSet holders = valueAt(parameter.operand);
            for (BuiltinName name : parameter.namedMembers)
                m_context.connect(m_context.lookup(holders, property(m_context, name)), escaped());
            // Or what a member that it calls has answered: `text.split(separator)` is whatever the method of the separator under @@split returns.
            for (BuiltinName name : parameter.calledMembers) {
                forEachPartOfCallees(m_context, m_context.restrict(m_context.valueOf(m_context.lookup(holders, property(m_context, name))), m_universe.callables()), [&](ClimbingSet callees) {
                    m_context.connect(m_context.group(callees, ValueFlowCallKind::CallByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName).result, escaped());
                });
            }
        }
    }

    // The getter of a built-in constructor under @@species answers its receiver, so a class of the program that extends one is its own species.
    void constructSpecies(BuiltinOperand operand)
    {
        ClimbingSet constructors = m_context.valueOf(m_context.lookup(valueAt(operand), property(m_context, BuiltinNames::constructor)));
        ClimbingSet species = m_context.unite(constructors, m_context.valueOf(m_context.lookup(constructors, property(m_context, BuiltinNames::speciesSymbol))));
        ClimbingSet answeredToClassesOfProgram = m_context.valueOf(m_context.lookup(m_context.restrict(constructors, ValueSets::complement(m_universe.builtinObjects())), property(m_context, BuiltinNames::speciesSymbol)));
        bool isReturned = m_builtins.at(m_overload->result.type).freshness == BuiltinFreshness::UnlessSpecies;

        // A built-in constructor as the species makes what the entry says.
        ValueSetID followed = m_context.maker().unite(m_universe.closures(), m_universe.otherCallables());
        followed = m_context.maker().unite(followed, m_universe.untrackedObjectAlone());
        ClimbingSet keptByNativeCode;
        forEachPartOfCallees(m_context, m_context.restrict(species, followed), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::ConstructByNativeCode);
            m_context.add(group.newTarget, callees);
            m_context.add(group.otherArguments, unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
            if (m_call.thrown)
                m_context.connect(group.thrown, m_call.thrown);
            if (isReturned) {
                if (ValueFlowNodeID result = nodeTakingResult())
                    m_context.connect(group.result, result);
                m_objectsMadeBySpecies = m_context.unite(m_objectsMadeBySpecies, m_context.restrict(m_context.valueOf(group.result), m_universe.objects()));
            } else {
                m_context.connect(group.result, escaped());
                keptByNativeCode = m_context.unite(keptByNativeCode, m_context.restrict(m_context.valueOf(group.result), m_universe.objects()));
            }
        });
        // What is not returned native code goes on to work with: RegExp.prototype[@@matchAll] and [@@split] store `lastIndex` into it. No record says what it stores, or where.
        // It defines no accessor there and leaves alone what lies above.
        if (ClimbingSet kept = known(keptByNativeCode); m_context.meet(kept, m_universe.objects())) {
            m_context.mark(kept, { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation, ObjectMutation::StoreOfOtherType });
            m_context.add(m_context.written(kept, unknownKey), unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        }
        // A built-in constructor other than that of the class which the entry makes: `array.constructor = Map`. Nobody follows what it makes here.
        // One that finds nothing under @@species is no species: above every array lies Object.prototype, whose `constructor` is Object, and Object[@@species] is undefined.
        // It is the species where a getter of the program answers it: `static get [Symbol.species]() { return Object; }`, and `new Object(0)` is a Number.
        auto findsSpeciesGetter = [&](BuiltinID constructor) {
            constexpr unsigned longestChain = 4;
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
            answer(unknownObject(m_context, ValueFlowUnknownSource::ResultOfUnknownCallee));
            m_objectsMadeBySpecies = m_context.unite(m_objectsMadeBySpecies, unknownObjectAlone());
        }
        m_context.mark(m_objectsMadeBySpecies, { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });

        // Native code fills what the species constructor has made: it defines the elements one by one and stores the length, which may run a setter of the program. It does nothing else to the object:
        // it deletes nothing, freezes nothing, and leaves alone the other names and what lies above. Where no record says with what it fills, anything may lie under an index.
        auto goesIntoResult = [](const BuiltinTransfer& transfer) {
            return transfer.to.kind == BuiltinPlaceKind::ResultSlot || transfer.to.kind == BuiltinPlaceKind::ResultField;
        };
        ClimbingSet made = known(m_objectsMadeBySpecies);
        if (std::ranges::none_of(m_overload->flow.transfers, goesIntoResult) && m_context.meet(made, m_universe.objects())) {
            m_context.add(escaped(), made);
            m_context.mark(made, ObjectMutation::StoreOfOtherType);
            m_context.add(m_context.written(made, elements), unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
            m_context.add(m_context.written(made, property(m_context, BuiltinNames::length)), m_universe.anyPrimitive());
            // A value that is defined where an accessor lies makes another kind of property of it.
            bool mayHaveAccessorUnderIndex = std::ranges::any_of(std::array { ValueFlowNameKind::GetterUnderUnknownKey, ValueFlowNameKind::SetterUnderUnknownKey }, [&](ValueFlowNameKind kind) {
                return holdsAnything(m_context.valueOf(m_context.read(made, ValueFlowName::of(kind))));
            });
            if (mayHaveAccessorUnderIndex)
                m_context.mark(made, ObjectMutation::RedefineProperty);
        }
    }

    // An element of an array is an indexed property, which the program also reads and writes by itself.
    ValueFlowName nameOf(BuiltinSlotID slot) const
    {
        AbstractHeap heap = m_builtins.at(slot).heap;
        if (heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements)
            return elements;
        if (m_builtins.at(slot).owner == BuiltinClasses::BoundFunction)
            return { ValueFlowNameKind::HiddenSlot, slot.index() };
        return { ValueFlowNameKind::Slot, slot.index() };
    }

    ValueFlowName nameOfNativeCell(BuiltinName cell) const { return { ValueFlowNameKind::Slot, m_builtins.slotCount() + cell.index() }; }

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

    OptionSet<Protocol> protocolsOf(BuiltinOperand operand) const
    {
        if (operand.isReceiver())
            return m_overload->receiver ? m_overload->receiver->protocols : OptionSet<Protocol>();
        OptionSet<Protocol> protocols;
        for (const BuiltinParameter& parameter : m_overload->parameters) {
            if (parameter.operand == operand || (!parameter.operand.isReceiver() && parameter.operand.coversArgument(operand.argumentIndex())))
                protocols.add(parameter.protocols);
        }
        return protocols;
    }

    void forEachNodeHoldingOwnValuesOf(ClimbingSet holders, ValueSetID& constant, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
    {
        // The table knows what a built-in object holds, and the lookup reads the table. It also looks at the objects above, which no own value lies on.
        forEachNodeReadByWalkOver(m_context, m_context.restrict(holders, m_universe.builtinObjects()), { }, functor);
        if (m_context.meet(holders, m_universe.strings()))
            constant = m_context.maker().unite(constant, m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::String)));

        ClimbingSet objects = m_context.restrict(holders, m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_universe.builtinObjects())));
        forEachNodeReadThrough(objects, everyProperty, functor);
        if (mayBeUnknownObject(objects))
            constant = m_context.maker().unite(constant, unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        // A member that has a setter and no getter reads as undefined.
        forEachNodeReadThrough(objects, everySetter, [&](ValueFlowNodeID node) {
            if (holdsAnything(m_context.valueOf(node)))
                constant = m_context.maker().unite(constant, m_universe.undefinedValue());
        });
        ClimbingSet getters;
        forEachNodeReadThrough(objects, everyGetter, [&](ValueFlowNodeID node) {
            getters = m_context.unite(getters, m_context.valueOf(node));
        });
        forEachPartOfCallees(m_context, m_context.restrict(getters, m_universe.callables()), [&](ClimbingSet callees) {
            ValueFlowCallGroup group = m_context.group(callees, ValueFlowCallKind::Get);
            m_context.add(group.thisValue, objects);
            connectThrown(group, BuiltinCallbackTimes::Once);
            functor(group.result);
        });
    }

    // The library's code behind a described built-in is entered with operands nobody knows, and hands them on to other built-ins. That a call goes over its operand the records say where that call is,
    // so a built-in that is applied in such code opens and steps no operand nobody knows: what it takes from there is a value nobody knows.
    ClimbingSet iteratedOperand(ClimbingSet iterated, ValueSetID& constant)
    {
        if (!mayBeUnknownObject(iterated) || !m_call.operation || !isCodeThatRecordsDescribe(m_universe, m_call.operation->function))
            return iterated;
        constant = m_context.maker().unite(constant, unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        return m_context.restrict(iterated, everythingButUnknownObject(m_context));
    }

    // The nodes that hold what lies at the place. What no node holds is added to the constant.
    void forEachNodeAt(const BuiltinPlace& place, ValueSetID& constant, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
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
            // No node holds the prototype that an object is created with. Of a placeholder the lookup answers the placeholder and all that is above what it covers.
            // Above a primitive lies the prototype object of its wrapper: `"xyz".__proto__` is String.prototype.
            ClimbingSet objects = objectsAt(*place.operand);
            ClimbingSet primitives = m_context.restrict(valueAt(*place.operand), ValueSets::complement(m_universe.objects()));
            ClimbingSet above = objectsRightAbove(m_context, m_context.unite(known(objects), primitives));
            functor(m_context.holding(above));
            // What lies above a generator is the prototype object of its function, for which the generators themselves stand. It is no generator.
            if (m_context.meet(above, m_universe.sureInstancesOf(BuiltinClasses::Generator)) || m_context.meet(above, m_universe.sureInstancesOf(BuiltinClasses::AsyncGenerator)))
                constant = m_context.maker().unite(constant, unknownObject(m_context, ValueFlowUnknownSource::ObjectWithoutLayout));
            if (m_context.meet(objects, m_universe.placeholdersAlone()))
                functor(m_context.lookup(m_context.restrict(objects, m_universe.placeholdersAlone()), prototypeLink));
            // The unknown object stands for an instance that has escaped, and what lies above an instance has not escaped with it.
            if (mayBeUnknownObject(objects))
                functor(m_context.nodes().ofHost(ValueFlowHostNode::ObjectsAboveEscapedInstances));
            // A chain ends: null lies above Object.prototype, and may lie above an object of which the table or the program says nothing.
            bool mayBeEndOfChain = m_context.meet(objects, m_universe.placeholdersAlone());
            bool mayBeNamespace = false;
            m_context.forEachMember(known(objects), [&](ValueFlowObjectID object) {
                ValueFlowObjectKind kind = m_universe.kindOf(object);
                if (kind == ValueFlowObjectKind::Builtin || kind == ValueFlowObjectKind::ModuleBuiltin)
                    mayBeEndOfChain |= m_builtins.at(m_universe.builtinOf(object)).prototypeLink != BuiltinPrototypeLink::Builtin;
                mayBeNamespace |= kind == ValueFlowObjectKind::Namespace;
            });
            // A lookup finds nothing above the namespace object of a module. Whoever asks what lies there gets an empty object of the run time's own.
            if (mayBeNamespace)
                constant = m_context.maker().unite(constant, unknownObject(m_context, ValueFlowUnknownSource::ObjectWithoutLayout));
            if (mayBeEndOfChain)
                constant = m_context.maker().unite(constant, m_context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
            return;
        }
        case BuiltinPlaceKind::IteratedValues:
            functor(m_context.lookup(iteratedOperand(valueAt(*place.operand), constant), iteratedValues));
            return;
        case BuiltinPlaceKind::OwnValues: {
            // Where what is done to the operand is done to what it holds as well, the own values of its own values lie at the place too, and so on down.
            bool goesOnDown = usesOf(*place.operand).contains(OperandUse::AppliesTransitively);
            // What an iteration hands out is among what the built-in takes from an operand that it iterates over. An iterator that the program has written makes it in `next`: it lies nowhere on the operand.
            bool isIteratedOver = protocolsOf(*place.operand).contains(Protocol::Iterate);
            ValueSetID objectsOfProgram = m_context.maker().intersect(m_context.maker().intersect(m_universe.objects(), everythingButUnknownObject(m_context)), ValueSets::complement(m_universe.builtinObjects()));
            // Beside the unknown object the normal form leaves out whatever has escaped: a host object that the operand lists is walked all the same.
            ClimbingSet holders = valueAt(*place.operand);
            if (mayBeUnknownObject(holders)) {
                forEachOperandAt(*place.operand, [&](const ValueFlowOperand& operand) {
                    if (operand.node)
                        holders = m_context.unite(holders, m_context.valueWithin(ClimbingSet(m_tables.heap().holdersOfMembersThatHostMakes()), operand.node));
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
                // Native code that reads a list index by index finds what an object above holds there. What lies under a name above, the methods of a class before all, is no member of the list.
                take(m_context.lookup(m_context.restrict(holders, objectsOfProgram), elements));
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
            // A name is looked up, so it is found above the holder too. A step without a name takes every own value: what lies above the holder is none of them.
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
                functor(m_context.read(only(m_context, m_resultObject), nameOf(place.slot)));
            return;
        case BuiltinPlaceKind::ResultField:
            if (m_resultObject)
                functor(m_context.read(only(m_context, m_resultObject), nameOfResultField(place.fieldIndex)));
            return;
        case BuiltinPlaceKind::NewValue:
            constant = m_context.maker().unite(constant, valueOfType(place.newValueType, m_contentsObject));
            noteInstancesIn(place.newValueType);
            forEachCallbackResultIn(place.newValueType, functor);
            return;
        case BuiltinPlaceKind::NativeMemory:
            constant = m_context.maker().unite(constant, unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The type of a new value may list what native code makes and what it hands on as it is: an operand, what an operand holds, what a callback answers.
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
                    functor(part.isSettled ? m_context.lookup(m_context.valueOf(result), awaitedValue) : result);
                });
            }
        }
    }

    // What lies at the place as it stands in this round.
    ClimbingSet valueAt(const BuiltinPlace& place)
    {
        ClimbingSet value = place.kind == BuiltinPlaceKind::Operand ? valueAt(*place.operand) : ClimbingSet();
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
        if (place.kind == BuiltinPlaceKind::NewValue) {
            forEachPlaceHandedOnIn(place.newValueType, [&](const BuiltinPlace& handedOn) {
                forEachOperandAt(handedOn, functor);
            });
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

    // A Map and a Set keep a number that fits an int32 as that int32, however it came: -0 lies there as the integer 0.
    void normalizeKeysStoredAt(BuiltinSlotID id, ValueFlowNodeID target)
    {
        const BuiltinSlot& slot = m_builtins.at(id);
        bool holdsKeys = (slot.owner == BuiltinClasses::Map && slot.contentsSlot == ContentsSlot::Key) || (slot.owner == BuiltinClasses::Set && slot.contentsSlot == ContentsSlot::Element);
        if (!holdsKeys || target == escaped())
            return;
        if (m_context.holdsAnyOf(only(m_context, ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber)), target))
            m_context.add(target, only(m_context, ValueFlowUniverse::primitive(PrimitiveSet::Integer)));
    }

    // The nodes that take what is put at the place: none where nobody reads it.
    void forEachTargetAt(const BuiltinPlace& place, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& takeTarget)
    {
        auto functor = [&](ValueFlowNodeID target) {
            takeTarget(target);
            if (place.kind == BuiltinPlaceKind::Slot || place.kind == BuiltinPlaceKind::ResultSlot)
                normalizeKeysStoredAt(place.slot, target);
        };
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
            forEachNodeWrittenThrough(objectsAt(*place.operand), unknownKey, functor);
            return;
        case BuiltinPlaceKind::Prototype:
            forEachNodeWrittenThrough(objectsAt(*place.operand), prototypeLink, functor);
            return;
        case BuiltinPlaceKind::NativeCell:
            functor(nodeWrittenToNativeCell(m_context, place.nativeCell));
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

    // A promise is resolved with what a record puts into its value: it adopts a promise or a thenable, the rejection too, and never holds one.
    static bool resolvesPromise(const BuiltinPlace& to)
    {
        return (to.kind == BuiltinPlaceKind::ResultSlot || to.kind == BuiltinPlaceKind::Slot) && to.slot == BuiltinSlots::PromiseValue;
    }

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
        forEachTargetAt(to, [&](ValueFlowNodeID target) {
            connectFrom(source, target);
        });
    }

    // What native code makes or has at hand of the type. `fresh` stands for an object that this call allocates. One object stands for everything that is made to be put below it.
    // None where the unknown object stands for what the call returns.
    // "A new array, or whatever object the species constructor makes": the instance is what the entry makes by itself, and constructSpecies() answers the other.
    static bool listsWhatIsMadeWithoutSpecies(const BuiltinType& type)
    {
        return type.freshness == BuiltinFreshness::UnlessSpecies && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind);
    }

    bool holdsModuleObjects(const BuiltinTypePart& part) const
    {
        return std::ranges::any_of(part.contents, [&](const BuiltinSlotContents& contents) {
            return std::ranges::any_of(m_builtins.at(contents.type).parts, [&](const BuiltinTypePart& held) {
                return held.kind == BuiltinTypePartKind::Builtin && m_builtins.canonicalID(held.builtin) == Builtins::ModuleObject;
            });
        });
    }

    ValueSetID valueOfType(BuiltinTypeID id, ValueFlowObjectID fresh)
    {
        if (!id)
            return unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult);
        const BuiltinType& type = m_builtins.at(id);
        if (type.isAnything())
            return unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult);

        ValueSetMaker& maker = m_context.maker();
        ValueSetID value = m_tables.primitivesOf(id);
        bool isPlainDataOfResult = type.flags.contains(BuiltinTypeFlag::HasPlainData) && !type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }) && fresh && fresh == m_resultObject && m_contentsObject;
        if (isPlainDataOfResult)
            value = maker.unite(value, plainDataHolding(primitivesInsidePlainData(value)));
        else if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }) && !listsWhatIsMadeWithoutSpecies(type))
            value = maker.unite(value, unknownObject(m_context, ValueFlowUnknownSource::UndescribedNativeResult));

        Vector<ValueFlowObjectID, 32> objects;
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Builtin:
                // Of a built-in of which each module has an object of its own, native code may hand out any. The one that createRequire() makes is no module's,
                // and the module objects hang together as the requests of the build say.
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
                if (type.freshness == BuiltinFreshness::No || !fresh || isCalledInWaysNobodyFollows(part)) {
                    value = maker.unite(value, unknownObject(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
                    // The unknown object stands only for what has escaped. So the module objects in the list that `module.children` answers are in unknown hands.
                    if (holdsModuleObjects(part)) {
                        objects.shrink(0);
                        appendModuleObjectsAnsweredBy(m_context, m_builtins.canonicalID(m_builtin), objects);
                        m_context.add(ValueFlowProgramNodes::escaped(), ClimbingSet(maker.of(objects.span())));
                    }
                    if (part.kind == BuiltinTypePartKind::Instance)
                        noteConstructorsAboveUnknownInstance(part);
                    break;
                }
                allocate(part, fresh, type.freshness);
                value = maker.unite(value, maker.of(fresh));
                break;
            // Native code makes none of these. A transfer or a callback brings them to a result, and whoever reads the place of a new value takes them from where they lie.
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

    // As the results say it of an object: markedMutationsOf().
    bool mayHaveReachedUnknownJavaScript(ClimbingSet objects)
    {
        if (m_context.meet(m_context.markedAmong(objects, ObjectMutation::ReachUnknownJavaScript), ValueSets::anything))
            return true;
        if (!m_context.holdsAnyOf(objects, ValueFlowProgramNodes::escaped()))
            return false;
        return m_context.contains(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::UnseenCodeRuns)), ValueFlowUniverse::unknownObject())
            || m_context.meet(m_context.markedAmong(ClimbingSet(m_universe.unknownObjectAlone()), ObjectMutation::ReachUnknownJavaScript), ValueSets::anything);
    }

    // Where the names in the text are the fields of a record that typed code takes, the parser makes an object with the layout of the record, so that the object passes the test at the entrance of that code.
    // Only of a record of which nothing is claimed: an object of it has been in the hands of JavaScript that nobody has seen, and may have come back with anything that its slots accept.
    // The run time makes the records that are listed here and no other: ObservedUse::ReachesTypedParse.
    ValueSetID recordsMadeByParsers()
    {
        Vector<ValueFlowObjectID, 8> made;
        for (LayoutID layout : m_universe.recordsThatTypedCodeTakes()) {
            if (mayHaveReachedUnknownJavaScript(ClimbingSet(m_universe.objectsWith(layout))))
                made.append(m_universe.instanceOf(layout));
        }
        m_context.forEachMember(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers)), [&](ValueFlowObjectID record) {
            made.append(record);
        });
        return m_context.maker().of(made.span());
    }

    // Beside the data a row lists the primitives that the call itself may answer. It speaks of what lies INSIDE the data only where it lists all that a text in JSON can hold: null, booleans, numbers, strings.
    // Any other plain data holds any primitive: `headers.toJSON()` answers an object and nothing else, and the object holds strings.
    ValueSetID primitivesInsidePlainData(ValueSetID listed) const
    {
        constexpr PrimitiveSet heldByJSON[] { PrimitiveSet::Null, PrimitiveSet::False, PrimitiveSet::True, PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber, PrimitiveSet::String };
        bool listsWhatJSONHolds = std::ranges::all_of(heldByJSON, [&](PrimitiveSet kind) {
            return m_context.sets().contains(listed, ValueFlowUniverse::primitive(kind));
        });
        return listsWhatJSONHolds ? listed : m_universe.anyPrimitive();
    }

    // What a parser makes of a text: ordinary objects and arrays that hold primitives and more such objects. They hold no function and nothing that was there before the call, and nobody else has them.
    // One object and one array of the call stand for all of them.
    ValueSetID plainDataHolding(ValueSetID primitives)
    {
        ValueSetMaker& maker = m_context.maker();
        ClimbingSet object = only(m_context, m_resultObject);
        ClimbingSet array = only(m_context, m_contentsObject);
        ValueFlowObjectID members[] { m_resultObject, m_contentsObject };
        ValueSetID records = recordsMadeByParsers();
        ValueSetID objects = maker.unite(maker.of(std::span<const ValueFlowObjectID>(members)), records);
        ValueSetID data = maker.unite(primitives, objects);
        m_plainDataOfResult = data;
        if (records != ValueSets::nothing) {
            m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers), records);
            m_context.add(m_context.written(ClimbingSet(records), prototypeLink), only(m_context, m_universe.objectOf(Builtins::ObjectPrototype)));
            m_context.add(m_context.written(ClimbingSet(records), unknownKey), data);
        }
        m_context.add(m_context.written(object, prototypeLink), only(m_context, m_universe.objectOf(Builtins::ObjectPrototype)));
        m_context.add(m_context.written(array, prototypeLink), only(m_context, m_universe.objectOf(Builtins::ArrayPrototype)));
        noteAllocationAs(BuiltinClasses::Object, m_resultObject);
        noteAllocationAs(BuiltinClasses::Array, m_contentsObject);
        m_context.add(m_context.written(object, unknownKey), data);
        m_context.add(m_context.written(array, elements), data);
        m_context.mark(object, ObjectMutation::AddProperty);
        return objects;
    }

    // As Object.create(prototype) does: the result never lies under the prototype object of its class unless the operand is that object.
    bool takesPrototypeOfResultFromOperand() const
    {
        return std::ranges::any_of(m_overload->flow.propertyActions, [](const BuiltinPropertyAction& action) {
            return action.kind == BuiltinPropertyActionKind::BecomesPrototype && action.value.kind == BuiltinPlaceKind::Result;
        });
    }

    // A row that names a class says "of that class or of one below it": what filter makes of bytes is a typed array, and Uint8Array.prototype lies above it, not the object that all typed arrays share.
    // The constructor of a class makes instances of that very class, and so does a row that says so.
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

    // The unknown object stands for an instance that native code hands out. Whoever reads `constructor` through it finds the class that the row names: `digest.constructor.from(..)`.
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
        ValueFlowNodeID prototype = m_context.written(object, prototypeLink);
        noteAllocationAs(part.kind == BuiltinTypePartKind::Instance ? part.builtinClass : BuiltinClassID(), fresh);
        switch (part.kind) {
        case BuiltinTypePartKind::Instance:
            // A class without a prototype object says nothing about what lies above its instances. A bound function has what lies above its target, which the overload says.
            if (BuiltinID prototypeObject = m_builtins.at(part.builtinClass).prototype) {
                if (fresh != m_resultObject || !takesPrototypeOfResultFromOperand()) {
                    m_context.add(prototype, only(m_context, m_universe.objectOf(prototypeObject)));
                    if (!part.isOfClassItself)
                        addPrototypesOfClassesBelow(part.builtinClass, prototype);
                }
            } else if (!m_overload->result.prototypeSource || fresh != m_resultObject)
                m_context.add(prototype, unknownObject(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
            // An unaliased result may have own properties that the table does not declare. A lookup that comes by the unknown object answers the unknown value for every name.
            // Whoever takes all that the object holds makes no lookup, so the same is said of the object itself.
            if (freshness == BuiltinFreshness::Unaliased && !listsOwnNamesOf(part.builtinClass, fresh)) {
                m_context.add(prototype, unknownObject(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
                m_context.add(m_context.written(object, unknownKey), unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
            }
            if (isConstruction(m_call.kind) && fresh == m_resultObject) {
                // A class of the program that extends the built-in class hands down its own prototype object.
                m_context.connect(m_context.lookup(m_context.valueOf(m_call.thisValue), property(m_context, BuiltinNames::prototype)), prototype);
                // A constructor that runs JavaScript may call what it finds on the new object, as that of Map calls `set` for each entry. No record says which members, so native code has all that such a class hands down, and the class.
                // A method that asks an instance for its constructor and that for its species says so itself.
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
                m_context.add(m_context.written(object, layout.isTuple ? elements : property(m_context, field.name)), valueOfType(field.type, m_contentsObject));
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
                m_context.add(held, valueOfType(contents.type, m_contentsObject));
            // A transfer brings what the call hands on to a slot of the result. No place names a slot of what lies below the result.
            if (fresh == m_resultObject)
                continue;
            forEachPlaceHandedOnIn(contents.type, [&](const BuiltinPlace& handedOn) {
                connectFrom(handedOn, held);
            });
        }
        if (part.kind == BuiltinTypePartKind::Instance) {
            fillSlotsThatRowLeavesOut(part, fresh);
            writeNamesDefinedOnNewInstance(part.builtinClass, fresh);
            // Whoever takes the own values of a String object takes its characters. A lookup finds them at the prototype object.
            if (m_builtins.definesCharactersOnNewInstances(part.builtinClass))
                m_context.add(m_context.written(object, elements), only(m_context, ValueFlowUniverse::primitive(PrimitiveSet::String)));
        }
        if (fresh != m_resultObject)
            return;
        for (const BuiltinResultProperty& own : m_overload->result.properties)
            m_context.add(m_context.written(object, property(m_context, own.name)), valueOfType(own.type, m_contentsObject));
    }

    // `Object.keys(o)` and its relatives, where every object that `o` may be is a literal of the program that nobody else holds: its names are the fields of its layout
    // and the names under which statements have added a property. So the list holds those constants, and a text nobody knows only where a name nobody knows was added. False for any other call.
    // A literal of constants is a literal like any other: it has the members that its record lists, which need not all be fields.
    // What the constructor of a base class allocates is no such literal: it has the layout, and so the fields, of whichever class is constructed.
    bool listOwnKeysIn(ValueFlowNodeID held)
    {
        BuiltinID builtin = m_builtins.canonicalID(m_builtin);
        bool listsSymbols = builtin == Builtins::ReflectOwnKeys;
        if (builtin != Builtins::ObjectKeys && builtin != Builtins::ObjectGetOwnPropertyNames && !listsSymbols)
            return false;
        // For undefined and null the call throws and lists nothing, and no call passes the mark of an empty variable.
        ValueFlowObjectID neverListed[] { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null), ValueFlowUniverse::primitive(PrimitiveSet::Absent) };
        ClimbingSet listed = m_context.restrict(valueAt(BuiltinOperand::argument(0)), ValueSets::complement(m_context.maker().of(neverListed)));
        // Until an object arrives there is nothing to list.
        if (!holdsAnything(listed))
            return true;
        // Any other primitive is converted to an object whose names the table knows.
        if (m_context.meet(listed, ValueSets::complement(m_universe.knownObjects())))
            return false;
        if (holdsAnything(m_context.markedAmong(listed, { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript })) || m_context.holdsAnyOf(listed, escaped()) || m_context.holdsAnyOf(listed, ValueFlowProgramNodes::lost()))
            return false;

        Vector<ValueFlowObjectID, 16> keys;
        bool areLiterals = true;
        auto list = [&](PropertyKeyID key) {
            PropertyKeyKind kind = m_program.at(key).kind;
            if (kind == PropertyKeyKind::Text || (listsSymbols && kind != PropertyKeyKind::PrivateName && kind != PropertyKeyKind::EnginePrivateName))
                keys.append(m_universe.keyConstant(key));
        };
        m_context.forEachMember(listed, [&](ValueFlowObjectID object) {
            const ValueFlowSite* site = m_universe.kindOf(object) == ValueFlowObjectKind::Site ? m_universe.siteOf(object) : nullptr;
            const Value* creation = site && site->origin != ValueFlowSiteOrigin::BuiltinResult && site->origin != ValueFlowSiteOrigin::TypedResult ? &m_program.at(m_universe.functionOf(object)).at(site->value) : nullptr;
            // An array of constants has its indexes for names, and an object of constants without a layout is written by the optimizer alone.
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
        m_context.connect(m_context.masked(m_context.read(listed, ValueFlowName::of(ValueFlowNameKind::AddedNames)), listsSymbols ? ValueSets::anything : m_universe.strings()), held);
        return true;
    }

    // The overload lists them for its result or takes them from a map of descriptors, or the class and the classes above list what is put on an instance.
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

    // What a row does not say is not known: a slot holds nothing only where the type of the new object says `never`.
    void fillSlotsThatRowLeavesOut(const BuiltinTypePart& part, ValueFlowObjectID fresh)
    {
        for (BuiltinClassID current = part.builtinClass; current; current = m_builtins.at(current).parent) {
            for (BuiltinSlotID slot : m_builtins.at(current).slots) {
                if (std::ranges::contains(part.contents, slot, &BuiltinSlotContents::slot) || (fresh == m_resultObject && isFilledByRecord(slot)))
                    continue;
                BuiltinTypeID holds = m_builtins.at(slot).holds;
                m_context.add(m_context.written(only(m_context, fresh), nameOf(slot)), holds ? valueOfType(holds, m_contentsObject) : unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult));
            }
        }
    }

    // What native code puts on a new instance under the names whose types the class and the classes above say. A name that the program does not spell is read by nobody.
    void writeNamesDefinedOnNewInstance(BuiltinClassID builtinClass, ValueFlowObjectID fresh)
    {
        ClimbingSet object = only(m_context, fresh);
        for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
            const BuiltinClass& entry = m_builtins.at(current);
            // The table does not say which attributes native code gives the properties: those of an error are not enumerable.
            if (!entry.namesDefinedOnNewInstances.empty())
                m_context.mark(object, { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty });
            for (const BuiltinResultProperty& own : entry.typesOfNamesDefinedOnNewInstances) {
                if (PropertyKeyID key = m_program.keyOf(own.name))
                    m_context.add(m_context.written(object, ValueFlowName::property(key)), m_tables.primitivesOf(own.type));
            }
            for (const BuiltinPropertyMaker& maker : entry.makersOfNamesDefinedOnNewInstances) {
                PropertyKeyID made = m_program.keyOf(maker.name);
                if (!mayHaveMaker(maker))
                    continue;
                // The function has escaped by the store that put it there, so it is entered with operands nobody knows, and the instance is one of them.
                m_context.add(escaped(), object);
                if (made)
                    m_context.add(m_context.written(object, ValueFlowName::property(made)), unknownValue(m_context, ValueFlowUnknownSource::ResultOfUnknownCallee));
            }
        }
    }

    // Only what the program has stored counts: what the table has there is native code's own. A name that the program does not spell lies only where something was stored under a key nobody knows.
    bool mayHaveMaker(const BuiltinPropertyMaker& maker)
    {
        ClimbingSet chain = m_context.valueOf(m_context.lookup(only(m_context, m_universe.objectOf(maker.holder)), prototypeLink));
        if (m_context.meet(chain, m_universe.placeholdersAlone()))
            return true;
        PropertyKeyID key = m_program.keyOf(maker.member);
        return m_context.meet(m_context.valueOf(m_context.read(m_context.restrict(chain, m_universe.knownObjects()), key ? ValueFlowName::property(key) : unknownKey)), m_universe.callables());
    }

    // A rest operand among the arguments is handed on in order: `f.call(thisArg, a, b)` passes a first and b second. What is spread behind them, and what the row lists behind the rest operand, may land anywhere.
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
            m_context.connect(group.result, escaped());
            return;
        case BuiltinCallbackResultUse::Returned:
            if (ValueFlowNodeID result = nodeTakingResult())
                m_context.connect(group.result, result);
            return;
        case BuiltinCallbackResultUse::Stored:
            if (resolvesPromise(resultTo))
                adoptRejection(resultTo, m_context.valueOf(group.result));
            forEachTargetAt(resultTo, [&](ValueFlowNodeID target) {
                m_context.connect(resolvesPromise(resultTo) ? m_context.lookup(m_context.valueOf(group.result), awaitedValue) : group.result, target);
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

    // The code behind an entry hands on the operands that it was entered with, and nobody knows those: see ValueFlowCalleeOrigin.
    ValueFlowCalleeOrigin originOfOperands() const
    {
        return m_call.operation && isCodeBehindDeclaredEntries(m_program, m_call.operation->function) ? ValueFlowCalleeOrigin::OperandOfAppliedEntry : ValueFlowCalleeOrigin::Value;
    }

    void applyCallback(const BuiltinCallback& callback)
    {
        forEachPartOfCallees(m_context, m_context.restrict(valueAt(callback.callee), m_universe.callables()), [&](ClimbingSet callees) {
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
            // A promise that is resolved with what the function answers is rejected with what the function throws.
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
                forEachOperandAt(*action.value.operand, [&](const ValueFlowOperand& source) {
                    assignFrom(action, source);
                });
            else
                forEachOperandAt(action.value, applyWithValue);
            return;
        case BuiltinPropertyActionKind::DefinesFrom:
            forEachOperandAt(*action.value.operand, applyWithValue);
            return;
        case BuiltinPropertyActionKind::StoresByKey:
        case BuiltinPropertyActionKind::DefinesName:
        case BuiltinPropertyActionKind::DefinesGetter:
        case BuiltinPropertyActionKind::DefinesSetter:
        case BuiltinPropertyActionKind::TakesDescriptor:
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

    // On a built-in object every name counts. A source that the call spells out as an object literal is assigned to one name by name.
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

    // The names of an object literal that the call spells out at the position and that goes nowhere else: it holds what its own definitions put there, under those names, and nothing more.
    // None where something else stands there, where a definition computes its name and the caller has no use for the other names alone, and where anything but the definitions and the call gets hold of the object.
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
        std::ranges::sort(names);
        names.shrink(names.size() - std::ranges::unique(names).size());
        return names;
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
    bool m_handsOnResult { false };
    // Of the overload that is applied: none where the unknown object stands for what it returns.
    ValueFlowObjectID m_resultObject;
    ValueFlowObjectID m_contentsObject;
    // What lies in the plain data that the overload answers, where the two objects of the call stand for that data. None where the unknown object does.
    ValueSetID m_plainDataOfResult;
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
    description.operation = &call;
    return description;
}

// The key that stands for the symbols that the call makes. The pass that names created symbols has interned one for a call that names Symbol itself, with no description or a constant one.
std::optional<PropertyKeyID> keyOfSymbolCreatedBy(const Program& program, const ValueFlowStatement& call)
{
    const Function& function = program.at(call.function);
    const Value& operation = function.at(call.site);
    if (operation.opcode != Opcode::Call || !function.module || !operation.hasPosition())
        return std::nullopt;

    StringID description;
    std::span<const ValueID> arguments = operation.variadicOperands();
    if (arguments.size() > 1)
        return std::nullopt;
    if (!arguments.empty()) {
        const Value& argument = function.at(definitionThroughUpcasts(function, arguments[0]));
        if (argument.opcode != Opcode::String)
            return std::nullopt;
        description = argument.string();
    }
    return program.types.find({ .kind = PropertyKeyKind::CreatedSymbol, .text = description, .module = function.module, .position = operation.position });
}

} // anonymous namespace

static ValueFlowName nameOfNativeCell(ValueFlowRuleContext& context, BuiltinName cell)
{
    return { ValueFlowNameKind::Slot, context.program().builtins().slotCount() + cell.index() };
}

ValueFlowNodeID nodeWrittenToNativeCell(ValueFlowRuleContext& context, BuiltinName cell)
{
    return context.written(only(context, context.universe().objectOf(Builtins::GlobalThis)), nameOfNativeCell(context, cell));
}

// An object that native code makes by itself lies in its cell for whoever looks there. A function among them is the object of its entry, whose overloads say what a call does.
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

// Native code has put there what code of the library made, so that is what the program has in its hands: a read answers it and a call enters it, with the operands of the call.
// The overloads of the entry sum up what the code of the library does, for a program that does not hold that code.
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
        // The first read runs the code that makes the value.
        if (call.thrown)
            context.add(call.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        // A store through the setter replaces the value, and the overload says what a read finds then.
        return !context.holdsAnyOf(only(context, entry), context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins));
    }

    context.use(builtin, useOf(call.kind));
    ClimbingSet callees = context.restrict(context.valueOf(value), universe.callables());
    if (!isConstruction(call.kind)) {
        ValueSetID describedByEntries = universe.builtinTables().closuresThatEntriesDescribe();
        context.forEachMember(context.restrict(callees, describedByEntries), [&](ValueFlowObjectID closure) {
            applyEntryDescribing(closure, call, context);
        });
        callees = context.restrict(callees, ValueSets::complement(describedByEntries));
    }
    // Whoever calls the entry may be seen code.
    if (call.kind != ValueFlowCallKind::CallByHost)
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), context.restrict(callees, universe.closures()));
    forEachPartOfCallees(context, callees, [&](ClimbingSet part) {
        ValueFlowCallGroup group = context.group(part, call.kind);
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
    });
    return true;
}

void applyHostUses(BuiltinID onBehalfOf, const ValueFlowOperand& value, const BuiltinHostUses& uses, ValueFlowRuleContext& context)
{
    ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByHost };
    BuiltinApplication(onBehalfOf, noCall, context).applyHostUses(value, uses);
}

void callFromHost(BuiltinID onBehalfOf, const ValueFlowOperand& callees, const BuiltinHostCall& call, ValueFlowRuleContext& context)
{
    ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByHost };
    BuiltinApplication(onBehalfOf, noCall, context).callFromHost(callees, call);
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
    // One key stands for every symbol that the call makes, however often it runs. Nothing is converted, so nothing of the program runs and nothing is thrown but what any call throws.
    if (kind == ValueFlowCallKind::Call && context.program().builtins().canonicalID(builtin) == Builtins::Symbol) {
        if (std::optional<PropertyKeyID> key = keyOfSymbolCreatedBy(context.program(), call)) {
            noteCallOfBuiltin(builtin, kind, context);
            if (call.result)
                context.add(call.result, only(context, context.universe().keyConstant(*key)));
            return;
        }
    }
    if (kind == ValueFlowCallKind::Call && context.program().builtins().canonicalID(builtin) == Builtins::SymbolFor) {
        const Program& program = context.program();
        const Function& function = program.at(call.function);
        std::optional<PropertyKey> named = keyOfSymbolRegisteredBy(program.builtins(), function, function.at(call.site));
        if (std::optional<PropertyKeyID> key = named ? program.types.find(*named) : std::nullopt) {
            noteCallOfBuiltin(builtin, kind, context);
            if (call.result)
                context.add(call.result, only(context, context.universe().keyConstant(*key)));
            return;
        }
    }
    ValueFlowBuiltinCall description = describe(kind, call, context);
    description.thisValue.mask = context.maker().intersect(description.thisValue.mask, receivers);
    applyBuiltin(builtin, description, context);
}

void applyBuiltinFoundThroughUnknownObject(BuiltinID builtin, const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    ValueFlowBuiltinCall description = describe(ValueFlowCallKind::Call, call, context);
    description.thisValue = { { }, context.universe().unknownObjectAlone(), ValueSets::anything };
    description.saysOnlyWhatIsCalled = true;
    BuiltinApplication(builtin, description, context).run();
}

bool raisesEventWhenCalled(const BuiltinTable& builtins, BuiltinID builtin)
{
    return eventOfCalling(builtins, builtins.canonicalID(builtin), false).has_value();
}

// `f.bind(thisValue)` binds no argument. What a call makes is an object of that call alone, so the operation says it: a call that passes one argument at the most and spreads no list.
bool bindsNoArgument(const Program& program, const ValueFlowUniverse& universe, ValueFlowObjectID boundFunction)
{
    const ValueFlowSite* site = universe.siteOf(boundFunction);
    if (!site || site->origin != ValueFlowSiteOrigin::BuiltinResult || site->ordinal)
        return false;
    const Value& operation = program.at(universe.functionOf(boundFunction)).at(site->value);
    return operation.opcode == Opcode::Call && operation.variadicOperands().size() <= 1;
}

// A set that holds every one of the callees for which bindsNoArgument() holds, and beside them only objects that are none of the callees' or for which it holds too.
// It follows from the object alone, and a group of the unknown object would ask it of all that has escaped in each of its runs. So each callee is asked once, by whoever meets it first.
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

// Each argument of the call keeps its position. A function of the table is applied with the operands of this very call, as if the program had called it on the bound `this`:
// `const define = Object.defineProperty.bind(Object); define(object, key, descriptor)` stores under that key and no other.
static void callTargetsWithoutBoundArguments(ClimbingSet callees, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    auto slot = [&](BuiltinSlotID id) {
        return context.read(callees, { ValueFlowNameKind::HiddenSlot, id.index() });
    };
    ClimbingSet targets = context.restrict(context.valueOf(slot(BuiltinSlots::BoundFunctionTarget)), universe.callables());
    ValueFlowBuiltinCall onBoundThis = description;
    // The bound function makes the call, not the operation: nothing in the image lists what it enters.
    onBoundThis.kind = ValueFlowCallKind::CallByNativeCode;
    onBoundThis.thisValue = { slot(BuiltinSlots::BoundFunctionBoundThis), ValueSets::nothing, ValueSets::anything };
    // A built-in that lets in code raises its event where it is applied, and the rule of a group does that.
    Vector<ValueFlowObjectID, 4> applied;
    context.forEachMember(context.restrict(targets, universe.builtinFunctions()), [&](ValueFlowObjectID target) {
        if (raisesEventWhenCalled(context.program().builtins(), universe.builtinOf(target)))
            return;
        applied.append(target);
        applyBuiltin(universe.builtinOf(target), onBoundThis, context);
    });
    forEachPartOfCallees(context, context.restrict(targets, ValueSets::complement(context.maker().of(applied.span()))), [&](ClimbingSet others) {
        ValueFlowCallGroup group = context.group(others, ValueFlowCallKind::CallByNativeCode);
        context.connect(onBoundThis.thisValue, group.thisValue);
        for (unsigned index = 0; index < description.arguments.size(); ++index)
            context.connect(description.arguments[index], group.argumentAt(index));
        context.connect(description.laterArguments, group.laterArguments);
        context.connect(description.otherArguments, group.otherArguments);
        passNoFurtherArguments(context, group, description.argumentsMayBeAbsent || description.otherArguments.node ? 0 : description.arguments.size());
        if (description.result)
            context.connect(group.result, description.result);
        if (description.thrown)
            context.connect(group.thrown, description.thrown);
    });
}

static void callOtherCallables(ClimbingSet callees, const ValueFlowBuiltinCall& description, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowCallKind kind = isConstruction(description.kind) ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode;
    ClimbingSet allCallees = callees;
    if (!isConstruction(kind)) {
        // The callees are other callables, all of them.
        ValueSetID bindingNoArgument = boundFunctionsBindingNoArgumentAmong(callees, context);
        if (context.meet(callees, bindingNoArgument)) {
            callTargetsWithoutBoundArguments(context.restrict(callees, bindingNoArgument), description, context);
            callees = context.restrict(callees, ValueSets::complement(bindingNoArgument));
        }
    }
    auto slot = [&](BuiltinSlotID id) {
        return context.read(callees, { ValueFlowNameKind::HiddenSlot, id.index() });
    };

    // The bound arguments come first, so no argument of the call keeps its position.
    forEachPartOfCallees(context, context.restrict(context.valueOf(slot(BuiltinSlots::BoundFunctionTarget)), universe.callables()), [&](ClimbingSet targets) {
        ValueFlowCallGroup group = context.group(targets, kind);
        if (isConstruction(kind)) {
            // The target takes the place of the bound function as new.target. Any other new.target goes on to the target: `class Derived extends Bound` constructs the target for Derived.
            context.add(group.newTarget, targets);
            ClimbingSet handedOn = context.restrict(context.valueOf(description.thisValue), ValueSets::complement(universe.otherCallables()));
            context.add(group.newTarget, handedOn);
            context.add(context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands), handedOn);
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
    });
    settlePromisesThrough(allCallees, description, context);
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
    applyEntryDescribing(closure, describe(kind, call, context), context);
}

void noteCallOfBuiltin(BuiltinID builtin, ValueFlowCallKind kind, ValueFlowRuleContext& context)
{
    context.use(builtin, useOf(kind));
    // Nobody has looked at the operands, so any overload may take the call.
    noteCallThatRunsOtherCode(builtin, context);
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
    description.laterArguments = operandAt(group.laterArguments);
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

// An arrow function, a method and an async function are born without the property: a definition puts an accessor there, or they inherit one.
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
    ClimbingSet found = context.valueOf(context.lookup(ClimbingSet(asked), ValueFlowName::getter(context.program().keyOf(BuiltinNames::prototype))));
    forEachPartOfCallees(context, context.restrict(found, universe.callables()), [&](ClimbingSet getters) {
        ValueFlowCallGroup group = context.group(getters, ValueFlowCallKind::Get);
        context.add(group.thisValue, asked);
        if (thrown)
            context.connect(group.thrown, thrown);
    });
}

// `tested instanceof constructor`: what the run time looks up on the constructor and calls. With the operation where one asks.
static void testAgainstConstructor(ValueFlowRuleContext& context, const ValueFlowOperand& tested, const ValueFlowOperand& constructor, ValueFlowNodeID thrown, const ValueFlowStatement* operation)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet methods = context.restrict(context.valueOf(context.lookup(context.valueOf(constructor), property(context, BuiltinNames::hasInstanceSymbol))), universe.callables());

    // The entry of a built-in is applied with the operands of this operation: in a group, what one operation tests would meet the constructors of all the others.
    ClimbingSet builtinFunctions = context.restrict(methods, universe.builtinFunctions());
    if (operation)
        applyEventsOfCall(*operation, context, builtinFunctions);
    // A closure and a function of the table are no bound functions. The method that every function inherits asks such a function for its prototype property and walks the chain of what is tested:
    // but for a getter under that name no code runs, and nobody keeps what is tested. No test for a class tells them from a bound function, so they are left out here.
    ClimbingSet otherConstructors = context.restrict(context.valueOf(constructor), ValueSets::complement(context.maker().unite(universe.builtinFunctions(), universe.closures())));
    ValueFlowBuiltinCall application;
    application.kind = ValueFlowCallKind::CallByNativeCode;
    application.arguments = std::span(&tested, 1);
    application.thrown = thrown;
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
        context.connect(constructor, group.thisValue);
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

// Whatever a behavior applies to anywhere in the program lies in one hub. Native code calls what it finds on a receiver with that receiver as `this`, and a function nobody knows keeps its `this`, as does a function of the table
// that nothing describes. That one may be found on one receiver says nothing about the receivers beside it, so each object is handed to the functor by itself, with an operand that holds it alone.
// What is looked up on it under a name is then the lookup of which there is one for each (object, name), whoever asks. Nothing tells apart what a primitive or a placeholder stands for: those come together.
// With more objects than the analysis tells apart in one value only those past the limit come together: tellObjectsApart().
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

// The objects among the receivers with those above them. What the table says a built-in object holds is no code of the program and leads to none.
static ClimbingSet chainOfProgramFrom(ValueFlowRuleContext& context, ClimbingSet objects)
{
    const ValueFlowUniverse& universe = context.universe();
    return context.restrict(context.valueOf(context.lookup(objects, prototypeLink)), context.maker().intersect(universe.objects(), ValueSets::complement(universe.builtinObjects())));
}

// The part of a behavior that reads and calls nothing by itself. A lookup tells its receivers apart where it finds an accessor nobody knows, so all receivers are asked together.
static void readMembersOfReceivers(ValueFlowRuleContext& context, ValueFlowNodeID hub, const ValueFlowBehavior& behavior, ClimbingSet receivers)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = withWhatUntrackedObjectCovers(context, context.restrict(receivers, universe.objects()));
    // What was copied onto a receiver lies on the objects that it was copied from: they are looked through like the receiver.
    if (behavior.isTransitive) {
        context.connect(context.read(objects, everyProperty), hub);
        context.connect(context.read(objects, copiedFrom), hub);
        // A list is read index by index: an object above may hold the element, or a getter that answers it.
        context.connect(context.lookup(context.restrict(objects, context.maker().intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects()))), elements), hub);
    }
    // The method that every function inherits asks the target of a bound function in its place, and the target of that one if it is bound too.
    if (behavior.isTransitive && behavior.protocols.contains(Protocol::HasInstance))
        context.connect(context.read(objects, { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() }), hub);
    // A walk that goes up the chain shows what a receiver inherits, as the formatter of console.log does: an object that lies on the objects above is looked through like one that lies on the receiver.
    // Of a function up there, a method or the class under `constructor`, it shows the name at most.
    if (behavior.isTransitive && behavior.protocols.containsAll({ Protocol::OwnValues, Protocol::PrototypeChain }))
        context.add(hub, context.restrict(context.valueOf(context.read(chainOfProgramFrom(context, objects), everyProperty)), ValueSets::complement(universe.callables())));
    // The table knows what a built-in object holds, and the lookup reads the table: whoever walks a receiver that the program names reads every member, and one that the host makes at its first read is made.
    // Beside the unknown object the normal form leaves out whatever has escaped, so the hub is asked for what it lists.
    ClimbingSet hostObjects = behavior.protocols.contains(Protocol::OwnValues) ? context.valueWithin(ClimbingSet(universe.builtinTables().heap().holdersOfMembersThatHostMakes()), hub) : ClimbingSet();
    if (behavior.isTransitive)
        hostObjects = withHostObjectsAmongMembers(context, hostObjects);
    forEachNodeReadByWalkOver(context, hostObjects, behavior.protocols, [&](ValueFlowNodeID members) {
        if (behavior.isTransitive)
            context.connect(members, hub);
    });
    if (!behavior.protocols.contains(Protocol::Named))
        return;

    // Without names: the built-in may read any member, an inherited one too.
    if (behavior.namedMembers.empty() && behavior.isTransitive)
        context.connect(context.read(chainOfProgramFrom(context, objects), everyProperty), hub);
    for (BuiltinName name : behavior.namedMembers) {
        ValueFlowNodeID member = context.lookup(receivers, property(context, name));
        if (behavior.isTransitive) {
            context.connect(member, hub);
            continue;
        }
        // The conversion goes with the members.
        if (behavior.protocols.contains(Protocol::ToPrimitive))
            context.connect(member, context.hub(universe.builtinTables().conversionToPrimitive()));
    }
}

// What native code finds under the name on the receivers and calls. The library's code behind a described built-in is entered with operands nobody knows, and an object that it makes holds them:
// under a key nobody knows such an operand may lie under this name too. What a call of the built-in does with its operands the records say where the call is, so it is no callee here.
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
        return context.valueOf(context.lookup(receivers, property(context, name)));
    ValueSetID madeInDescribedCode = context.maker().of(made.span());
    ClimbingSet found = context.restrict(context.valueOf(context.lookup(ClimbingSet(madeInDescribedCode), property(context, name))), ValueSets::complement(universe.unknownObjectAlone()));
    return context.unite(found, context.valueOf(context.lookup(context.restrict(receivers, ValueSets::complement(madeInDescribedCode)), property(context, name))));
}

// The universe is made before anybody knows the callee of a call, so what a built-in allocates may be anything there, a function too. Each allocation leaves what stands for its class on the object:
// what was allocated as an ordinary object or an array and as nothing else, as the data that JSON.parse makes, is called by nobody. See allocatedByBuiltinsAs().
static ClimbingSet withoutWhatBuiltinsMadeAsNoFunction(ValueFlowRuleContext& context, ClimbingSet values)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowBuiltinTables& tables = universe.builtinTables();
    ValueFlowObjectID classesOfData[] { tables.objectStandingFor(BuiltinClasses::Object), tables.objectStandingFor(BuiltinClasses::Array) };
    ValueSetID otherClasses = ValueSets::complement(context.maker().of(std::span<const ValueFlowObjectID>(classesOfData)));
    Vector<ValueFlowObjectID, 8> made;
    context.forEachMember(context.restrict(values, context.maker().intersect(universe.objectsThatMayBeAnything(), universe.knownObjects())), [&](ValueFlowObjectID object) {
        ClimbingSet named = context.valueOf(context.written(only(context, object), allocatedAs));
        if (context.meet(named, ValueSets::anything) && !context.meet(named, otherClasses))
            made.append(object);
    });
    return made.isEmpty() ? values : context.restrict(values, ValueSets::complement(context.maker().of(made.span())));
}

// The host has nothing to choose where the record lists no operand, or primitives that native code makes.
static bool passesOnlyPrimitives(const BuiltinTable& builtins, const BuiltinNamedMemberCall& record)
{
    return std::ranges::all_of(record.arguments, [&](const BuiltinPlace& argument) {
        if (argument.kind != BuiltinPlaceKind::NewValue || !argument.newValueType)
            return false;
        const BuiltinType& type = builtins.at(argument.newValueType);
        return !type.isAnything() && type.parts.empty() && !type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData });
    });
}

// The host loads a start-up module for a method that it finds under one of these names and calls, and for no other call: the table lists the names, and the records what is passed.
// `next` on an iterator nobody knows gets nothing out of such a module.
static bool isCalledWithFunctionsOfStartupModules(ValueFlowRuleContext& context, BuiltinName name)
{
    return std::ranges::contains(context.program().builtins().namesOfHostMethodsCallingStartupLoads(), name);
}

// The host finds the method under its name. Of the names under which it passes a function of a start-up module the stores are followed: on an object nobody knows lies what some statement has stored
// under the name, on whatever object, or the method of a class of the table of which an instance may exist. A class that the table does not have comes out of native code that nothing describes,
// which says so by itself: see letUndeclaredNativeCodeRun().
static bool mayBeFoundOnObjectNobodyKnows(ValueFlowRuleContext& context, BuiltinName name)
{
    const ValueFlowUniverse& universe = context.universe();
    if (context.meet(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::StoredUnderNamesOfMethodsCallingStartupLoads)), universe.callables()))
        return true;
    ClimbingSet prototypes(universe.builtinTables().heap().under(context.program().keyOf(name)).prototypeObjects);
    return context.holdsAnyOf(prototypes, context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins))
        || context.holdsAnyOf(prototypes, ValueFlowProgramNodes::escaped());
}

// The part of a behavior that calls what it finds on the receivers. `thisValue` holds them.
static void callMembersOfReceivers(ValueFlowRuleContext& context, ValueFlowNodeID hub, const ValueFlowBehavior& behavior, ClimbingSet receivers, const ValueFlowOperand& thisValue)
{
    const ValueFlowUniverse& universe = context.universe();
    // What a protocol is handed by the code of a receiver is a receiver too.
    ValueFlowNodeID handedOut = behavior.isTransitive ? hub : ValueFlowNodeID();
    // Where nothing is called native code passes nothing: a value that nobody describes comes into being with the call.
    enum class Passes : bool { Primitives, ValuesNobodyDescribes };
    auto call = [&](BuiltinName name, Passes passes, ValueFlowNodeID result) {
        ClimbingSet members = membersToCall(context, receivers, name);
        if (!context.meet(members, universe.callables()))
            return;
        callFromNativeCode(context, { members, thisValue, passes == Passes::Primitives ? universe.anyPrimitive() : unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult), result });
    };
    auto take = [&](ValueFlowNodeID node) {
        if (handedOut)
            context.connect(node, handedOut);
    };

    ClimbingSet objects = withWhatUntrackedObjectCovers(context, context.restrict(receivers, universe.objects()));
    // A getter nobody knows keeps its `this`. Past the limit of what is told apart receivers come together, and that it lies on one of them says nothing about the others: each is asked by itself.
    auto runGettersOn = [&](NOESCAPE const Invocable<ClimbingSet(ClimbingSet)> auto& holdersOf) {
        forEachPartOfCallees(context, context.restrict(context.valueOf(context.read(holdersOf(objects), everyGetter)), universe.callables()), [&](ClimbingSet getters) {
            ValueFlowCallGroup group = context.group(getters, ValueFlowCallKind::Get);
            if (context.contains(getters, ValueFlowUniverse::unknownObject())) {
                context.forEachMember(context.restrict(objects, universe.knownObjects()), [&](ValueFlowObjectID receiver) {
                    if (context.contains(context.valueOf(context.read(holdersOf(only(context, receiver)), everyGetter)), ValueFlowUniverse::unknownObject()))
                        context.add(group.thisValue, only(context, receiver));
                });
                ValueFlowOperand holdingOthers = thisValue;
                holdingOthers.mask = context.maker().intersect(holdingOthers.mask, ValueSets::complement(universe.knownObjects()));
                context.connect(holdingOthers, group.thisValue);
            } else
                context.connect(thisValue, group.thisValue);
            context.connect(group.thrown, ValueFlowProgramNodes::escaped());
            take(group.result);
        });
    };

    if (behavior.protocols.contains(Protocol::OwnValues)) {
        runGettersOn([&](ClimbingSet holders) {
            return context.unite(holders, withSourcesOfCopies(context, context.restrict(holders, universe.knownObjects())));
        });
    }
    if (behavior.protocols.contains(Protocol::ToPrimitive)) {
        // A primitive is what it is converted to: nothing is looked up in it.
        ValueFlowOperand holdingConverted = thisValue;
        holdingConverted.mask = context.maker().intersect(holdingConverted.mask, universe.objects());
        ClimbingSet converted = context.restrict(receivers, universe.objects());
        for (BuiltinName name : { BuiltinNames::toPrimitiveSymbol, BuiltinNames::valueOf, BuiltinNames::toString })
            callFromNativeCode(context, { membersToCall(context, converted, name), holdingConverted, universe.anyPrimitive(), { } });
    }
    if (behavior.protocols.contains(Protocol::ToJSON))
        call(BuiltinNames::toJSON, Passes::Primitives, handedOut ? handedOut : ValueFlowProgramNodes::escaped());
    if (behavior.protocols.contains(Protocol::HasInstance))
        call(BuiltinNames::hasInstanceSymbol, Passes::ValuesNobodyDescribes, { });
    if (behavior.protocols.contains(Protocol::Thenable))
        take(context.lookup(receivers, awaitedValue));
    if (behavior.protocols.contains(Protocol::Iterate))
        take(context.lookup(receivers, iteratedValues));
    // Without names: the built-in may read any member, an inherited one too.
    if (behavior.protocols.contains(Protocol::Named) && behavior.namedMembers.empty())
        runGettersOn([&](ClimbingSet holders) {
            return chainOfProgramFrom(context, holders);
        });
    if (behavior.protocols.contains(Protocol::CallsNamed)) {
        // Whatever is called here gets operands that the host chooses.
        bool callsSomething = false;
        // Without names: the built-in may call any method that it finds.
        if (behavior.calledMembers.empty()) {
            ClimbingSet methods = context.valueOf(context.read(chainOfProgramFrom(context, objects), everyProperty));
            if (context.meet(methods, universe.callables())) {
                callFromNativeCode(context, { methods, thisValue, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult), handedOut ? handedOut : ValueFlowProgramNodes::escaped() });
                callsSomething = true;
            }
        }
        for (BuiltinName name : behavior.calledMembers) {
            ValueFlowNodeID result = handedOut ? handedOut : ValueFlowProgramNodes::escaped();
            ClimbingSet members = withoutWhatBuiltinsMadeAsNoFunction(context, context.valueOf(context.lookup(receivers, property(context, name))));
            const BuiltinNamedMemberCall* record = context.program().builtins().callOfMemberNamed(name);
            if (record) {
                ValueFlowBuiltinCall noCall { .kind = ValueFlowCallKind::CallByNativeCode };
                BuiltinApplication(Builtins::UndeclaredNativeCode, noCall, context).callNamedMember(members, thisValue, record->arguments, result);
            } else
                call(name, Passes::ValuesNobodyDescribes, result);
            if (!isCalledWithFunctionsOfStartupModules(context, name) || (record && passesOnlyPrimitives(context.program().builtins(), *record)))
                continue;
            // A value nobody knows among what the lookup answers was stored under a key nobody knows, or lies on an object nobody knows above the receiver.
            callsSomething |= context.meet(members, context.maker().intersect(universe.callables(), ValueSets::complement(universe.placeholdersAlone())))
                || (context.meet(members, universe.placeholdersAlone()) && mayBeFoundOnObjectNobodyKnows(context, name));
        }
        if (callsSomething)
            letHostChooseOperands(context);
    }
}

// An object nobody knows may be an instance of a class of the table that no statement has made: a Buffer out of evaluated text, or out of a native function that answers anything.
// What native code does to the object needs no rule. But the method that it calls there under the name of a protocol may be written in JavaScript, as Buffer.prototype.toJSON is,
// and the image holds the code behind a member only where something uses it. A method in native code is there anyway.
// Only where an instance of the class may exist: the prototype object counts as used from the first overload that may answer one (noteClassesOfResult) and from the first native result
// that nobody describes (unknownValue), and unseen code has it in its hands.
// The library's code behind a described built-in is entered with operands nobody knows, so every program has a hub that holds the unknown object.
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
    if (behavior.protocols.contains(Protocol::ToPrimitive)) {
        for (BuiltinName name : { BuiltinNames::toPrimitiveSymbol, BuiltinNames::valueOf, BuiltinNames::toString })
            find(name);
    }
    if (behavior.protocols.contains(Protocol::ToJSON))
        find(BuiltinNames::toJSON);
    if (behavior.protocols.contains(Protocol::HasInstance))
        find(BuiltinNames::hasInstanceSymbol);
    if (behavior.protocols.contains(Protocol::CallsNamed)) {
        for (BuiltinName name : behavior.calledMembers)
            find(name);
    }
    if (methods.isEmpty())
        return;
    callFromNativeCode(context, { ClimbingSet(context.maker().of(methods.span())), operandHolding(universe.unknownObjectAlone()), universe.anyPrimitive(), behavior.isTransitive ? hub : ValueFlowProgramNodes::escaped() });
}

void runBehavior(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ValueFlowNodeID hub = statement.result;
    const ValueFlowBehavior& behavior = context.universe().builtinTables().at(context.nodes().behaviorOf(hub));

    // Native code does all of this to an escaped object anyway.
    ValueSetID mask = everythingButUnknownObject(context);
    ClimbingSet receivers = context.restrict(context.valueOf(hub), mask);
    readMembersOfReceivers(context, hub, behavior, receivers);
    forEachReceiverToldApart(context, receivers, operandAt(hub, mask), [&](ClimbingSet toldApart, const ValueFlowOperand& thisValue) {
        callMembersOfReceivers(context, hub, behavior, toldApart, thisValue);
    });
    if (!context.contains(context.valueOf(hub), ValueFlowUniverse::unknownObject()))
        return;
    callLibraryMethodsOfUnknownReceiver(context, hub, behavior);
    // An object nobody knows may have a member that is called too.
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
