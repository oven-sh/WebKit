/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTThreads.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRulesModules.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace JSC { namespace AOT { namespace Typed {

using ObjectAccess = ValueFlowAccessThroughObject;
using ObjectAccessKind = ValueFlowAccessThroughObjectKind;

static constexpr OptionSet<ObjectMutation> reachMutations { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript };
static constexpr ValueFlowName everyPropertyName = ValueFlowName::of(ValueFlowNameKind::EveryProperty);

static bool hasNamespaceOfProgram(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Somebody holds the object whom nobody follows.
static bool isInUnknownHands(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    return inputs.hasEscaped(object) || inputs.isLost(object) || inputs.isHeldByNativeCode(object) || markedMutationsOf(inputs, object).containsAny(reachMutations);
}

// The names under which a value lies, and under which a read looks for one.
static bool namesValue(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::EveryProperty:
        return true;
    default:
        return false;
    }
}

static bool namesAccessor(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        return true;
    default:
        return false;
    }
}

namespace {

struct VariableOnObject {
    ValueFlowObjectID object;
    VariableID variable;
    // None where no constant names the property.
    PropertyKeyID key;
    // A static field under a computed key, or under a name that a number converts to.
    bool mayLieUnderAnyName { false };
};

// The namespace objects and the class objects that the analysis follows, with the variables that are their properties.
class FollowedObjects {
public:
    explicit FollowedObjects(const ValueFlowResultInputs& inputs)
        : m_inputs(inputs)
    {
    }

    void addObject(ValueFlowObjectID object) { m_objects.append(object); }
    // One of the objects. The unknown object stands for it wherever code that nobody compiles may have put it.
    void addEscapedObject(ValueFlowObjectID object) { m_escapedObjects.append(object); }
    void addVariable(VariableOnObject&& variable) { m_variables.append(WTF::move(variable)); }
    void finish(ValueSetMaker&);

    bool isEmpty() const { return m_objects.isEmpty(); }
    ValueSetID objects() const { return m_setOfObjects; }
    bool isNamespace(ValueFlowObjectID object) const { return m_inputs.universe().kindOf(object) == ValueFlowObjectKind::Namespace; }
    ModuleID moduleOf(ValueFlowObjectID namespaceObject) const { return ModuleID(namespaceObject.index() - m_inputs.universe().namespaceOf(ModuleID(0u)).index()); }
    std::span<const VariableOnObject> variables() const LIFETIME_BOUND { return m_variables.span(); }
    std::span<const VariableOnObject> variablesOn(ValueFlowObjectID object) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(m_variables, object, { }, &VariableOnObject::object);
        return std::span(found);
    }

    // Every statement that has asked for a node of the set asks. Whichever thread asks first finds the answer, for all of them.
    bool hasObjectIn(ValueSetID set) const
    {
        uint32_t index = ValueSets::indexInTableBySet(set);
        if (index >= m_numberOfSetsRemembered)
            return m_inputs.sets().meet(set, m_setOfObjects);
        std::atomic<uint8_t>& entry = m_setsWithObject[index / setsPerEntry];
        unsigned shift = index % setsPerEntry * 2;
        uint8_t known = entry.load(std::memory_order_relaxed) >> shift & 3;
        if (!known) {
            known = m_inputs.sets().meet(set, m_setOfObjects) ? holdsObject : holdsNoObject;
            entry.fetch_or(known << shift, std::memory_order_relaxed);
        }
        return known == holdsObject;
    }

    void forEachObjectIn(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const
    {
        const ValueSets& sets = m_inputs.sets();
        if (!hasObjectIn(set))
            return;
        bool listsSet = !ValueSets::isComplement(set) && sets.size(set) <= m_objects.size();
        sets.forEachMember(listsSet ? set : m_setOfObjects, [&](ValueFlowObjectID object) {
            if (sets.contains(listsSet ? m_setOfObjects : set, object))
                functor(object);
        });
    }

    // With the objects that the unknown object in the set stands for.
    void forEachObjectStoredThrough(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const
    {
        if (!m_inputs.sets().contains(set, ValueFlowUniverse::unknownObject())) {
            forEachObjectIn(set, functor);
            return;
        }
        for (ValueFlowObjectID object : m_escapedObjects)
            functor(object);
        forEachObjectIn(set, [&](ValueFlowObjectID object) {
            if (!m_hasEscaped.quickGet(object.index()))
                functor(object);
        });
    }

private:
    static constexpr uint32_t setsPerEntry = 4;
    static constexpr uint8_t holdsNoObject = 1;
    static constexpr uint8_t holdsObject = 2;

    const ValueFlowResultInputs& m_inputs;
    Vector<ValueFlowObjectID> m_objects;
    Vector<ValueFlowObjectID> m_escapedObjects;
    // By object.
    BitVector m_hasEscaped;
    // By the number of the set, for the sets that there are when the objects are all listed: zero where nobody has asked.
    std::unique_ptr<std::atomic<uint8_t>[]> m_setsWithObject;
    uint32_t m_numberOfSetsRemembered { 0 };
    // Ascending by object.
    Vector<VariableOnObject> m_variables;
    ValueSetID m_setOfObjects { ValueSets::nothing };
};

void FollowedObjects::finish(ValueSetMaker& maker)
{
    m_setOfObjects = maker.of(m_objects.span());
    std::ranges::stable_sort(m_variables, { }, &VariableOnObject::object);
    m_hasEscaped.ensureSize(m_inputs.universe().numberOfObjects());
    for (ValueFlowObjectID object : m_escapedObjects)
        m_hasEscaped.quickSet(object.index());
    m_numberOfSetsRemembered = m_inputs.sets().endOfTableBySet();
    m_setsWithObject = std::make_unique<std::atomic<uint8_t>[]>(m_numberOfSetsRemembered / setsPerEntry + 1);
}

bool mayName(ValueFlowName name, const VariableOnObject& variable)
{
    switch (name.kind) {
    case ValueFlowNameKind::Property:
        return variable.mayLieUnderAnyName || (variable.key && variable.key.index() == name.payload);
    case ValueFlowNameKind::Elements:
        return variable.mayLieUnderAnyName;
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::EveryProperty:
        return true;
    default:
        return false;
    }
}

// Every read of a value goes through a node that is keyed by the objects it looks into: those that the lookup starts at and all that lies above them.
class AccessCollector {
public:
    AccessCollector(const ValueFlowResultInputs&, const FollowedObjects&, ValueFlowVariablesOnObjects&);

    void collectAccesses(ValueFlowHeapView&);
    void noteObjectsThatMayBeEnumerated(ValueFlowHeapView&, ValueSetID enumerated);
    bool isReadUnderUnknownName(ModuleID module) const { return m_isNamespaceReadUnderUnknownName[module.index()]; }
    // Something other than the definition of the class stores into the class object under a name that is not surely one of its static fields, or defines an accessor there.
    bool isStoredUnderOtherName(FunctionID constructor) const { return m_isClassObjectStoredUnderOtherName[constructor.index()]; }

private:
    struct Origin {
        FunctionID function;
        ValueID site;
    };

    const ValueFlowSolver& solver() const { return m_inputs.solver(); }
    uint32_t numberOfStatementsOfBodies() const { return solver().input().statements.size(); }
    bool isSharedLookup(ValueFlowStatementID) const;
    bool reachesFollowedObject(ValueFlowStatementID sharedLookup) const { return m_lookupReachesFollowedObject[sharedLookup.index() - numberOfStatementsOfBodies()]; }
    void findLookupsReachingFollowedObjects();
    ValueFlowName nameOf(PropertyKeyID key) const { return m_inputs.program.isIndexLike(key) ? ValueFlowName::of(ValueFlowNameKind::Elements) : ValueFlowName::property(key); }

    void noteLook(ValueSetID objects, ValueFlowName, Origin, ObjectAccessKind, Vector<ObjectAccess>&);
    void noteLooksOf(ValueFlowStatementID sharedLookup, Origin, ObjectAccessKind, Vector<ObjectAccess>&);
    void noteStore(const ValueFlowStatement*, ValueSetID objects, ValueFlowName, Origin, Vector<ObjectAccess>&);
    bool holdsFewMembers(const ValueFlowOperand&) const;
    bool storesValueInside(const ValueFlowStatement&, VariableID) const;
    void noteTestBy(const ValueFlowStatement&, ValueSetID chain, Origin, Vector<ObjectAccess>&);
    void collectAccessesOf(ValueFlowStatementID, Vector<ObjectAccess>&);
    void noteTestBy(const ValueFlowBuiltinApplication&, const BuiltinOverload&, const BuiltinPropertyAction&, ValueFlowHeapView&, SetsMeetingOneSet& setsWithFollowedObjects, Vector<ObjectAccess>&);
    void collectTestsOfBuiltins(ValueFlowHeapView&, Vector<ObjectAccess>&);
    void findValuesInsideTypes(ValueSetMaker&);

    const ValueFlowResultInputs& m_inputs;
    const FollowedObjects& m_followed;
    ValueFlowVariablesOnObjects& m_results;
    // By statement that a rule has made.
    Vector<uint8_t> m_lookupReachesFollowedObject;
    // The threads of a loop set these. By module, by variable, and by the constructor of the class.
    Vector<uint8_t> m_isNamespaceReadUnderUnknownName;
    Vector<uint8_t> m_isStoredOtherType;
    Vector<uint8_t> m_isStoredByBuiltin;
    Vector<uint8_t> m_isClassObjectStoredUnderOtherName;
    // By variable: what is sure to lie inside the type of a static field. Nothing for a variable whose type is only expected, which takes any value.
    Vector<ValueSetID> m_valuesInsideType;
};

} // anonymous namespace

AccessCollector::AccessCollector(const ValueFlowResultInputs& inputs, const FollowedObjects& followed, ValueFlowVariablesOnObjects& results)
    : m_inputs(inputs)
    , m_followed(followed)
    , m_results(results)
    , m_lookupReachesFollowedObject(FillWith { }, inputs.solver().numberOfStatements() - inputs.solver().input().statements.size(), static_cast<uint8_t>(0))
    , m_isNamespaceReadUnderUnknownName(FillWith { }, inputs.program.modules.size(), static_cast<uint8_t>(0))
    , m_isStoredOtherType(FillWith { }, inputs.program.variables.size(), static_cast<uint8_t>(0))
    , m_isStoredByBuiltin(FillWith { }, inputs.program.variables.size(), static_cast<uint8_t>(0))
    , m_isClassObjectStoredUnderOtherName(FillWith { }, inputs.program.functions.size(), static_cast<uint8_t>(0))
    , m_valuesInsideType(FillWith { }, inputs.program.variables.size(), ValueSets::nothing)
{
}

// The lookup of a value. What it finds it hands to whoever has asked for it and does nothing else with.
bool AccessCollector::isSharedLookup(ValueFlowStatementID id) const
{
    if (!solver().isMadeByRule(id) || solver().statementAt(id).kind != ValueFlowStatementKind::Lookup)
        return false;
    switch (solver().keyOf(id).name().kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::EveryProperty:
        return true;
    default:
        return false;
    }
}

// A lookup that starts at a placeholder and at other receivers hands on to a lookup for each, which goes no further.
void AccessCollector::findLookupsReachingFollowedObjects()
{
    uint32_t first = numberOfStatementsOfBodies();
    uint32_t count = m_lookupReachesFollowedObject.size();
    forEachIndexInParallel(m_inputs.loops(), count, [&](unsigned index) {
        ValueFlowStatementID id(first + index);
        if (!isSharedLookup(id) || solver().isAtRest(id))
            return;
        m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
            if (namesValue(key.name().kind) && m_followed.hasObjectIn(key.set))
                m_lookupReachesFollowedObject[index] = 1;
        });
    });
    Vector<uint32_t> handingOn = findIndicesInParallel(m_inputs.loops(), count, [&](unsigned index) {
        ValueFlowStatementID id(first + index);
        if (m_lookupReachesFollowedObject[index] || !isSharedLookup(id) || solver().isAtRest(id))
            return false;
        bool handsOn = false;
        m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey&, ValueFlowStatementID made) {
            handsOn |= made && made != id && isSharedLookup(made) && reachesFollowedObject(made);
        });
        return handsOn;
    });
    for (uint32_t index : handingOn)
        m_lookupReachesFollowedObject[index] = 1;
}

void AccessCollector::noteLook(ValueSetID objects, ValueFlowName name, Origin origin, ObjectAccessKind kind, Vector<ObjectAccess>& found)
{
    m_followed.forEachObjectIn(objects, [&](ValueFlowObjectID object) {
        // An export may have any name, a number among them.
        if (m_followed.isNamespace(object) && name.kind != ValueFlowNameKind::Property) {
            std::atomic_ref(m_isNamespaceReadUnderUnknownName[m_followed.moduleOf(object).index()]).store(1, std::memory_order_relaxed);
            return;
        }
        for (const VariableOnObject& variable : m_followed.variablesOn(object)) {
            if (mayName(name, variable))
                found.append({ variable.variable, origin.function, origin.site, kind });
        }
    });
}

void AccessCollector::noteLooksOf(ValueFlowStatementID sharedLookup, Origin origin, ObjectAccessKind kind, Vector<ObjectAccess>& found)
{
    m_inputs.forEachMadeNodeAskedForBy(sharedLookup, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
        if (key.kind == ValueFlowMadeNodeKind::Read && namesValue(key.name().kind)) {
            noteLook(key.set, key.name(), origin, kind, found);
            return;
        }
        if (key.kind != ValueFlowMadeNodeKind::Lookup || !made || made == sharedLookup || !isSharedLookup(made) || !reachesFollowedObject(made))
            return;
        m_inputs.forEachMadeNodeAskedForBy(made, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read }), false, [&](const ValueFlowNodeKey& keyOfPart, ValueFlowStatementID) {
            if (namesValue(keyOfPart.name().kind))
                noteLook(keyOfPart.set, keyOfPart.name(), origin, kind, found);
        });
    });
}

// Nobody goes through a large set member by member: whoever asks takes such an operand for anything.
bool AccessCollector::holdsFewMembers(const ValueFlowOperand& operand) const
{
    ValueSetID held = operand.node ? solver().valueOf(operand.node) : ValueSets::nothing;
    return !ValueSets::isComplement(held) && m_inputs.sets().size(held) <= m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
}

bool AccessCollector::storesValueInside(const ValueFlowStatement& statement, VariableID variable) const
{
    const DeclaredType& declared = m_inputs.program.at(variable).declared;
    if (declared.flags.contains(PositionFlags::IsExpected) || m_inputs.program.at(declared.type).isAnything())
        return true;
    std::optional<uint32_t> index = indexOfValueStoredIntoProperty(statement.opcode);
    if (!index || !holdsFewMembers(m_inputs.bodies().operandsOf(statement)[*index]))
        return false;
    bool isInside = true;
    ValueFlowMembers members = forEachMemberOf(m_inputs, m_inputs.bodies().operandsOf(statement)[*index], [&](ValueFlowObjectID member) {
        isInside &= m_inputs.sets().contains(m_valuesInsideType[variable.index()], member);
    });
    return isInside && members == ValueFlowMembers::Listed;
}

// Nobody stores into a namespace object. The statement is none where no operation spells the store out.
void AccessCollector::noteStore(const ValueFlowStatement* statement, ValueSetID objects, ValueFlowName name, Origin origin, Vector<ObjectAccess>& found)
{
    // The members that a class defines are listed with the class.
    bool definesMemberOfClass = statement && statement->opcode == Opcode::DefineMethod;
    m_followed.forEachObjectStoredThrough(objects, [&](ValueFlowObjectID object) {
        if (m_followed.isNamespace(object))
            return;
        bool namesStaticField = false;
        for (const VariableOnObject& variable : m_followed.variablesOn(object)) {
            if (!mayName(name, variable))
                continue;
            namesStaticField |= name.kind == ValueFlowNameKind::Property && !variable.mayLieUnderAnyName;
            found.append({ variable.variable, origin.function, origin.site, ObjectAccessKind::Store });
            if (statement && !indexOfValueStoredIntoProperty(statement->opcode))
                std::atomic_ref(m_isStoredByBuiltin[variable.variable.index()]).store(1, std::memory_order_relaxed);
            else if (!statement || !storesValueInside(*statement, variable.variable))
                std::atomic_ref(m_isStoredOtherType[variable.variable.index()]).store(1, std::memory_order_relaxed);
        }
        if (!namesStaticField && !definesMemberOfClass)
            std::atomic_ref(m_isClassObjectStoredUnderOtherName[m_inputs.universe().closureFunction(object).index()]).store(1, std::memory_order_relaxed);
    });
}

// The chain is all that the rule of the test has looked through.
void AccessCollector::noteTestBy(const ValueFlowStatement& statement, ValueSetID chain, Origin origin, Vector<ObjectAccess>& found)
{
    switch (statement.opcode) {
    case Opcode::PropHas:
    case Opcode::ScopeHasBinding:
        noteLook(chain, nameOf(m_inputs.program.at(statement.function).at(statement.site).key()), origin, ObjectAccessKind::Observation, found);
        return;
    case Opcode::ElemHas: {
        Vector<PropertyKeyID, 4> keys;
        const ValueFlowOperand& property = m_inputs.bodies().operandsOf(statement)[OperandIndex::ElemHas::property];
        if (!holdsFewMembers(property)) {
            noteLook(chain, everyPropertyName, origin, ObjectAccessKind::Observation, found);
            return;
        }
        bool isEveryKeyKnown = true;
        ValueFlowMembers members = forEachMemberOf(m_inputs, property, [&](ValueFlowObjectID member) {
            if (PropertyKeyID key = m_inputs.universe().keyOf(member))
                keys.append(key);
            else
                isEveryKeyKnown = false;
        });
        if (!isEveryKeyKnown || members != ValueFlowMembers::Listed) {
            noteLook(chain, everyPropertyName, origin, ObjectAccessKind::Observation, found);
            return;
        }
        for (PropertyKeyID key : keys)
            noteLook(chain, nameOf(key), origin, ObjectAccessKind::Observation, found);
        return;
    }
    default:
        return;
    }
}

void AccessCollector::collectAccessesOf(ValueFlowStatementID id, Vector<ObjectAccess>& found)
{
    const ValueFlowStatement& statement = solver().statementAt(id);
    bool isMadeByRule = solver().isMadeByRule(id);
    bool hasRun = isMadeByRule ? !solver().isAtRest(id) : !statement.function || m_inputs.isReached(statement.function);
    if (!hasRun || isSharedLookup(id))
        return;

    bool isOperation = !isMadeByRule && statement.kind == ValueFlowStatementKind::Operation && statement.function && statement.site;
    // The creation of a class ties the variables of its static fields to the properties of the class object. It looks at neither.
    if (isOperation && statement.opcode == Opcode::NewClass)
        return;
    Origin origin = isOperation ? Origin { statement.function, statement.site } : Origin { };
    bool answersWhatItReads = isOperation && (statement.opcode == Opcode::PropGet || statement.opcode == Opcode::ElemGet);
    ObjectAccessKind kindOfLook = answersWhatItReads ? ObjectAccessKind::Read : ObjectAccessKind::Observation;

    m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Written, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
        switch (key.kind) {
        case ValueFlowMadeNodeKind::Read:
            if (namesValue(key.name().kind))
                noteLook(key.set, key.name(), origin, ObjectAccessKind::Observation, found);
            return;
        case ValueFlowMadeNodeKind::Written:
            if (namesValue(key.name().kind) || namesAccessor(key.name().kind))
                noteStore(isOperation ? &statement : nullptr, key.set, key.name(), origin, found);
            return;
        case ValueFlowMadeNodeKind::Lookup:
            if (!made || isLookupOfCalleeByCall(m_inputs, statement, key))
                return;
            if (key.name().kind == ValueFlowNameKind::PrototypeLink) {
                if (isOperation)
                    noteTestBy(statement, solver().valueOf(solver().statementAt(made).result), origin, found);
                return;
            }
            if (isSharedLookup(made) && reachesFollowedObject(made))
                noteLooksOf(made, origin, kindOfLook, found);
            return;
        default:
            return;
        }
    });
}

// The built-in finds out whether something lies under a key, or what kind of property does.
void AccessCollector::noteTestBy(const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinPropertyAction& action, ValueFlowHeapView& heap, SetsMeetingOneSet& setsWithFollowedObjects, Vector<ObjectAccess>& found)
{
    const Program& program = m_inputs.program;
    const ValueSets& sets = m_inputs.sets();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    Origin origin { application.function, application.site };

    ValueSetID objects = application.valueAt(action.object, heap.maker());
    const BuiltinParameter* parameter = program.builtins().parameterFor(overload, action.object);
    bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
    if (!looksAtOwnPropertyAlone)
        objects = heap.chainOf(objects);
    if (!setsWithFollowedObjects.contains(objects))
        return;

    switch (action.key.kind) {
    case BuiltinKeyKind::None:
        return;
    case BuiltinKeyKind::Names:
        for (BuiltinName name : action.key.names)
            noteLook(objects, nameOf(program.keyOf(name)), origin, ObjectAccessKind::Observation, found);
        return;
    case BuiltinKeyKind::Operand: {
        ValueSetID keys = application.valueAt(*action.key.operand, heap.maker());
        // An operand that is left out is undefined, which names a property like any other value.
        if (sets.isEmpty(keys) || !sets.isSubset(keys, universe.keys()) || sets.meet(keys, tables.keysNobodyKnows()) || sets.size(keys) > m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
            break;
        sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
            noteLook(objects, nameOf(universe.keyOf(constant)), origin, ObjectAccessKind::Observation, found);
        });
        return;
    }
    case BuiltinKeyKind::Any:
    case BuiltinKeyKind::Indexes:
    case BuiltinKeyKind::ElementsOf:
        break;
    }
    noteLook(objects, everyPropertyName, origin, ObjectAccessKind::Observation, found);
}

// Which overload a call selects depends on its operands. Every overload of the mode counts.
void AccessCollector::collectTestsOfBuiltins(ValueFlowHeapView& heap, Vector<ObjectAccess>& found)
{
    BitVector builtinsThatTest = builtinsWithOverload(m_inputs.program.builtins(), [](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
            return action.kind == BuiltinPropertyActionKind::TestsByKey || action.kind == BuiltinPropertyActionKind::ReadsGetter || action.kind == BuiltinPropertyActionKind::ReadsSetter;
        });
    });
    SetsMeetingOneSet setsWithFollowedObjects(m_inputs.sets(), m_followed.objects());
    forEachBuiltinApplication(m_inputs, heap, builtinsThatTest, [&](const ValueFlowBuiltinApplication& application) {
        for (const BuiltinOverload& overload : m_inputs.program.builtins().overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                switch (action.kind) {
                case BuiltinPropertyActionKind::TestsByKey:
                case BuiltinPropertyActionKind::ReadsGetter:
                case BuiltinPropertyActionKind::ReadsSetter:
                    noteTestBy(application, overload, action, heap, setsWithFollowedObjects, found);
                    break;
                default:
                    break;
                }
            }
        }
    });
}

void AccessCollector::findValuesInsideTypes(ValueSetMaker& maker)
{
    const Program& program = m_inputs.program;
    for (const VariableOnObject& variable : m_followed.variables()) {
        const DeclaredType& declared = program.at(variable.variable).declared;
        if (!m_followed.isNamespace(variable.object) && !declared.flags.contains(PositionFlags::IsExpected))
            m_valuesInsideType[variable.variable.index()] = m_inputs.universe().membersSurelyInside(maker, program, declared.type);
    }
}

void AccessCollector::collectAccesses(ValueFlowHeapView& heap)
{
    findValuesInsideTypes(heap.maker());
    findLookupsReachingFollowedObjects();
    Vector<ObjectAccess>& accesses = m_results.accesses;
    Vector<ObjectAccess> testsOfBuiltins;
    // Only the second task asks the view of the heap and makes sets with its maker.
    runTasksInParallel(m_inputs.loops(), {
        [&] {
            accesses = collectInParallel<ObjectAccess>(m_inputs.loops(), solver().numberOfStatements(), [&](unsigned index, Vector<ObjectAccess>& found) {
                collectAccessesOf(ValueFlowStatementID(index), found);
            });
        },
        [&] {
            collectTestsOfBuiltins(heap, testsOfBuiltins);
        },
    });
    accesses.appendVector(WTF::move(testsOfBuiltins));
}

void AccessCollector::noteObjectsThatMayBeEnumerated(ValueFlowHeapView& heap, ValueSetID enumerated)
{
    Vector<ObjectAccess>& accesses = m_results.accesses;
    // Whoever lists the keys of an object sees which properties it has, and may go on to read each.
    m_followed.forEachObjectIn(enumerated, [&](ValueFlowObjectID object) {
        noteLook(heap.maker().of(object), everyPropertyName, { }, ObjectAccessKind::Observation, accesses);
    });

    for (uint32_t index = 0; index < m_isStoredOtherType.size(); ++index) {
        if (m_isStoredOtherType[index])
            m_results.staticFieldsStoredOtherTypes.quickSet(index);
        if (m_isStoredByBuiltin[index])
            m_results.staticFieldsStoredByBuiltins.quickSet(index);
    }
}

static void addExportsOf(const Program& program, ModuleID id, ValueFlowObjectID namespaceObject, FollowedObjects& followed)
{
    const Module& module = program.at(id);
    for (uint32_t index = 0; index < module.exports.size(); ++index) {
        const ModuleExport& entry = module.exports[index];
        if (entry.kind != ModuleExportKind::Variable || !entry.variable)
            continue;
        PropertyKeyID key = module.namespaceLayout ? program.at(module.namespaceLayout).fields[index].key : program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }).value_or(PropertyKeyID());
        followed.addVariable({ namespaceObject, entry.variable, key, false });
    }
}

// Code that names a module at run time gets the namespace object of any module that answers to a name.
static void addNamespaceObjects(const ValueFlowResultInputs& inputs, ValueFlowVariablesOnObjects& results, FollowedObjects& followed)
{
    const Program& program = inputs.program;
    bool modulesAreNamedAtRunTime = unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution);
    ValueSetID created = inputs.solver().valueOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::CreatedNamespaces));
    // A placeholder may be any of them.
    if (inputs.sets().meet(created, inputs.universe().placeholdersAlone()))
        return;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (!hasNamespaceOfProgram(program.modules[index].kind) || !inputs.build.isEnabled(FlowResultKind::ObjectUses, index))
            continue;
        if (modulesAreNamedAtRunTime && mayAnswerToName(program, ModuleID(index)))
            continue;
        ValueFlowObjectID namespaceObject = inputs.universe().namespaceOf(ModuleID(index));
        if (!inputs.sets().contains(created, namespaceObject)) {
            results.namespaceUses[index] = NamespaceUse::NeverCreated;
            continue;
        }
        // The loader gives the object of such a module the names that it finds on what a CommonJS file exports, so a read by name need not be a read of a variable.
        if (isInUnknownHands(inputs, namespaceObject) || commonJsModuleCapturedBy(program, program.modules[index]))
            continue;
        followed.addObject(namespaceObject);
        addExportsOf(program, ModuleID(index), namespaceObject, followed);
    }
}

// One object stands for the class objects of all evaluations of a class.
static void addClassObjects(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, ValueFlowVariablesOnObjects& results, FollowedObjects& followed)
{
    const Program& program = inputs.program;
    for (const Layout& layout : program.layouts) {
        if (layout.staticFields.isEmpty())
            continue;
        ValueFlowObjectID classObject = layout.constructor ? inputs.universe().closureOf(layout.constructor) : ValueFlowObjectID();
        OptionSet<ObjectMutation> marked = classObject ? markedMutationsOf(inputs, classObject) : OptionSet<ObjectMutation> { };
        // Unsolved code does what it likes to whatever lies where it reads. Code that nobody compiles only reads what it was handed, until something says that it stores: then the object has every mark.
        bool isFollowed = classObject && !inputs.isLost(classObject) && !inputs.isHeldByNativeCode(classObject) && !marked.contains(ObjectMutation::ReachUnknownJavaScript)
            && !calls.functionsNamedByUnsolvedCode.get(layout.constructor.index());
        if (!isFollowed) {
            for (const StaticField& field : layout.staticFields)
                results.staticFieldsInUnknownHands.quickSet(field.variable.index());
            continue;
        }

        followed.addObject(classObject);
        bool hasEscaped = inputs.hasEscaped(classObject) || marked.contains(ObjectMutation::ReachNativeCode);
        if (hasEscaped)
            followed.addEscapedObject(classObject);
        for (const StaticField& field : layout.staticFields) {
            followed.addVariable({ classObject, field.variable, field.key, !field.key || program.isIndexLike(field.key) });
            if (hasEscaped)
                results.staticFieldsOfEscapedClassObjects.quickSet(field.variable.index());
            // No mark says under which key.
            if (marked.contains(ObjectMutation::DeleteField))
                results.deletedStaticFields.quickSet(field.variable.index());
            // Sealing and freezing change the attributes of every property of the class object.
            if (marked.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity }))
                results.redefinedStaticFields.quickSet(field.variable.index());
        }
    }
}

struct ValueFlowAccessesThroughObjects {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowAccessesThroughObjects);

    explicit ValueFlowAccessesThroughObjects(const ValueFlowResultInputs& inputs)
        : heap(inputs)
        , followed(inputs)
    {
    }

    ValueFlowHeapView heap;
    ValueFlowVariablesOnObjects results;
    FollowedObjects followed;
    std::optional<AccessCollector> collector;
};

void ValueFlowAccessesThroughObjectsDeleter::operator()(ValueFlowAccessesThroughObjects* accesses) const
{
    delete accesses;
}

std::unique_ptr<ValueFlowAccessesThroughObjects, ValueFlowAccessesThroughObjectsDeleter> collectAccessesThroughObjects(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls)
{
    const Program& program = inputs.program;
    std::unique_ptr<ValueFlowAccessesThroughObjects, ValueFlowAccessesThroughObjectsDeleter> collected(new ValueFlowAccessesThroughObjects(inputs));
    ValueFlowVariablesOnObjects& results = collected->results;
    results.namespaceUses.fill(NamespaceUse::Unknown, program.modules.size());
    for (BitVector* variables : { &results.staticFieldsInUnknownHands, &results.staticFieldsOfEscapedClassObjects, &results.deletedStaticFields, &results.redefinedStaticFields, &results.staticFieldsStoredOtherTypes, &results.staticFieldsStoredByBuiltins })
        variables->ensureSize(program.variables.size());
    results.classObjectsStoredUnderStaticFieldsAlone.ensureSize(program.layouts.size());

    FollowedObjects& followed = collected->followed;
    addNamespaceObjects(inputs, results, followed);
    addClassObjects(inputs, calls, results, followed);
    if (followed.isEmpty())
        return collected;

    followed.finish(collected->heap.maker());
    collected->collector.emplace(inputs, followed, results);
    collected->collector->collectAccesses(collected->heap);
    return collected;
}

ValueFlowVariablesOnObjects computeVariablesOnObjects(const ValueFlowResultInputs& inputs, ValueFlowAccessesThroughObjects& collected, const ValueFlowUsesOfHeap& usesOfHeap)
{
    const Program& program = inputs.program;
    ValueFlowVariablesOnObjects& results = collected.results;
    if (!collected.collector)
        return WTF::move(results);
    const FollowedObjects& followed = collected.followed;
    AccessCollector& collector = *collected.collector;
    collector.noteObjectsThatMayBeEnumerated(collected.heap, objectsThatMayBeEnumerated(usesOfHeap));

    inputs.sets().forEachMember(followed.objects(), [&](ValueFlowObjectID object) {
        if (followed.isNamespace(object) && !collector.isReadUnderUnknownName(followed.moduleOf(object)))
            results.namespaceUses[followed.moduleOf(object).index()] = NamespaceUse::NamedReadsOnly;
    });
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.staticFields.isEmpty() || !layout.constructor || !inputs.sets().contains(followed.objects(), inputs.universe().closureOf(layout.constructor)))
            continue;
        if (!collector.isStoredUnderOtherName(layout.constructor))
            results.classObjectsStoredUnderStaticFieldsAlone.quickSet(index);
    }
    sortAndRemoveDuplicates(inputs.loops(), results.accesses, std::less<ObjectAccess>(), std::equal_to<ObjectAccess>());
    return WTF::move(results);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
