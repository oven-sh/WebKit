/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRulesModules.h"
#include <wtf/Atomics.h>
#include <wtf/EnumSet.h>

namespace JSC { namespace AOT { namespace Typed {

using ObjectAccess = ValueFlowAccessThroughObject;
using ObjectAccessKind = ValueFlowAccessThroughObjectKind;

static constexpr EnumSet<ValueFlowNameKind> namesOfValues { ValueFlowNameKind::Property, ValueFlowNameKind::Elements, ValueFlowNameKind::UnknownKey, ValueFlowNameKind::EveryProperty };
static constexpr EnumSet<ValueFlowNameKind> namesOfValuesAndAccessors = namesOfValues | EnumSet<ValueFlowNameKind> { ValueFlowNameKind::Getter, ValueFlowNameKind::Setter, ValueFlowNameKind::GetterUnderUnknownKey, ValueFlowNameKind::SetterUnderUnknownKey };

static constexpr EnumSet<ValueFlowNameKind> namesOfRemovalsAndRedefinitions { ValueFlowNameKind::DeletedMember, ValueFlowNameKind::DeletedMemberUnderUnknownKey, ValueFlowNameKind::RedefinedMember, ValueFlowNameKind::RedefinedMemberUnderUnknownKey };

static ValueFlowName propertyChangedUnder(ValueFlowName written)
{
    bool namesProperty = written.kind == ValueFlowNameKind::DeletedMember || written.kind == ValueFlowNameKind::RedefinedMember;
    return namesProperty ? ValueFlowName { ValueFlowNameKind::Property, written.payload } : everyPropertyName;
}

static bool testsProperty(const BuiltinPropertyAction& action)
{
    return action.kind == BuiltinPropertyActionKind::TestsByKey || action.kind == BuiltinPropertyActionKind::ReadsGetter || action.kind == BuiltinPropertyActionKind::ReadsSetter;
}

struct ValueFlowAccessesThroughObjects {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowAccessesThroughObjects);

    struct VariableOnObject {
        ValueFlowObjectID object;
        VariableID variable;
        PropertyKeyID key;
        bool mayLieUnderAnyName { false };

        bool mayBeNamedBy(ValueFlowName name) const
        {
            if (name.kind == ValueFlowNameKind::Property)
                return mayLieUnderAnyName || (key && key.index() == name.payload);
            return name.kind == ValueFlowNameKind::Elements ? mayLieUnderAnyName : name.kind == ValueFlowNameKind::UnknownKey || name.kind == ValueFlowNameKind::EveryProperty;
        }
    };

    explicit ValueFlowAccessesThroughObjects(const ValueFlowResultInputs& inputs)
        : inputs(inputs)
        , solver(inputs.solver())
        , sets(inputs.sets())
        , heap(inputs)
    {
    }

    static void setFlag(Vector<uint8_t>& flags, uint32_t index) { AtomicRef(flags[index]).store(1, std::memory_order_relaxed); }
    bool isNamespace(ValueFlowObjectID object) const { return inputs.universe().kindOf(object) == ValueFlowObjectKind::Namespace; }
    std::span<const VariableOnObject> variablesOn(ValueFlowObjectID object) const LIFETIME_BOUND { return std::span(std::ranges::equal_range(variables, object, { }, &VariableOnObject::object)); }
    ValueFlowName nameOf(PropertyKeyID key) const { return ValueFlowName::property(inputs.program, key); }
    bool reachesFollowedObject(ValueFlowStatementID sharedLookup) const { return lookupReachesFollowedObject[sharedLookup.index() - solver.input().statements.size()]; }

    bool hasObjectIn(ValueSetID set)
    {
        uint32_t index = ValueSets::indexInTableBySet(set);
        if (index / 4 >= setsWithObject.size())
            return sets.meet(set, setOfObjects);
        AtomicRef entry(setsWithObject[index / 4]);
        unsigned shift = index % 4 * 2;
        uint8_t known = entry.load(std::memory_order_relaxed) >> shift & 3;
        if (!known) {
            known = sets.meet(set, setOfObjects) ? 2 : 1;
            entry.fetch_or(known << shift, std::memory_order_relaxed);
        }
        return known == 2;
    }

    void forEachObjectIn(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor)
    {
        if (!hasObjectIn(set))
            return;
        bool listsSet = !ValueSets::isComplement(set) && sets.size(set) <= objects.size();
        sets.forEachMember(listsSet ? set : setOfObjects, [&](ValueFlowObjectID object) {
            if (sets.contains(listsSet ? setOfObjects : set, object))
                functor(object);
        });
    }

    void forEachObjectStoredThrough(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor)
    {
        bool holdsUnknownObject = sets.contains(set, ValueFlowUniverse::unknownObject());
        if (holdsUnknownObject)
            sets.forEachMember(setOfEscapedObjects, functor);
        forEachObjectIn(set, [&](ValueFlowObjectID object) {
            if (!holdsUnknownObject || !sets.contains(setOfEscapedObjects, object))
                functor(object);
        });
    }

    bool isSharedLookup(ValueFlowStatementID id) const
    {
        if (!solver.isMadeByRule(id) || solver.statementAt(id).kind != ValueFlowStatementKind::Lookup)
            return false;
        ValueFlowNameKind kind = solver.keyOf(id).name().kind;
        return kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Elements || kind == ValueFlowNameKind::EveryProperty;
    }

    void findLookupsReachingFollowedObjects()
    {
        uint32_t first = solver.input().statements.size();
        uint32_t count = lookupReachesFollowedObject.size();
        forEachIndexInParallel(inputs.loops(), count, [&](unsigned index) {
            ValueFlowStatementID id(first + index);
            if (!isSharedLookup(id) || solver.isAtRest(id))
                return;
            inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
                if (namesOfValues.contains(key.name().kind) && hasObjectIn(key.set))
                    lookupReachesFollowedObject[index] = 1;
            });
        });
        Vector<uint32_t> handingOn = findIndicesInParallel(inputs.loops(), count, [&](unsigned index) {
            ValueFlowStatementID id(first + index);
            if (lookupReachesFollowedObject[index] || !isSharedLookup(id) || solver.isAtRest(id))
                return false;
            bool handsOn = false;
            inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey&, ValueFlowStatementID made) {
                handsOn |= made && made != id && isSharedLookup(made) && reachesFollowedObject(made);
            });
            return handsOn;
        });
        for (uint32_t index : handingOn)
            lookupReachesFollowedObject[index] = 1;
    }

    void noteLookInto(ValueFlowObjectID object, ValueFlowName name, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        if (isNamespace(object) && name.kind != ValueFlowNameKind::Property) {
            setFlag(isNamespaceReadUnderUnknownName, inputs.universe().moduleOf(object).index());
            return;
        }
        for (const VariableOnObject& variable : variablesOn(object)) {
            access.variable = variable.variable;
            if (variable.mayBeNamedBy(name))
                found.append(access);
        }
    }

    void noteLook(ValueSetID lookedInto, ValueFlowName name, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        forEachObjectIn(lookedInto, [&](ValueFlowObjectID object) {
            noteLookInto(object, name, access, found);
        });
    }

    // A namespace never changes. Whether it accepts a removal or a definition depends on the export of that name and on its value.
    void noteChangeAskedOfNamespaces(ValueSetID asked, ValueFlowName written, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        forEachObjectIn(asked, [&](ValueFlowObjectID object) {
            if (isNamespace(object))
                noteLookInto(object, propertyChangedUnder(written), access, found);
        });
    }

    void noteLooksOf(ValueFlowStatementID sharedLookup, ObjectAccess access, Vector<ObjectAccess>& found, bool followsLookupsHandedOnTo = true)
    {
        inputs.forEachMadeNodeAskedForBy(sharedLookup, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            if (key.kind == ValueFlowMadeNodeKind::Lookup) {
                if (followsLookupsHandedOnTo && made && made != sharedLookup && isSharedLookup(made) && reachesFollowedObject(made))
                    noteLooksOf(made, access, found, false);
            } else if (namesOfValues.contains(key.name().kind))
                noteLook(key.set, key.name(), access, found);
        });
    }

    bool holdsFewMembers(const ValueFlowOperand& operand) const
    {
        ValueSetID held = operand.node ? solver.possibleValuesOf(operand.node) : ValueSets::nothing;
        return !ValueSets::isComplement(held) && sets.size(held) <= inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
    }

    // The type of a value behind a check holds only where the check passes, and the generic version stores the value all the same.
    bool storesValueInside(const ValueFlowStatement& statement, VariableID variable)
    {
        const DeclaredType& declared = inputs.program.at(variable).declared;
        if (declared.isExpected() || inputs.program.at(declared.written()).isAnything())
            return true;
        std::optional<uint32_t> index = indexOfValueStoredIntoProperty(statement.opcode);
        if (!index || !holdsFewMembers(inputs.bodies().operandsOf(statement)[*index]))
            return false;
        const Function& function = inputs.program.at(statement.function);
        const Type& type = inputs.program.at(*declared.established());
        ValueID stored = function.at(statement.site).operand(*index);
        const BitVector* valuesBehindChecks = inputs.bodies().valuesBehindChecks(statement.function);
        bool isBehindCheck = !valuesBehindChecks || valuesBehindChecks->get(stored.index());
        bool acceptsIntegers = type.primitives & Primitive::Integer && (isBehindCheck || inputs.program.typeOf(function, stored).integers.isSubsetOf(type.integers));
        bool isInside = true;
        bool storesIntegers = false;
        ValueFlowMembers members = forEachMemberOf(inputs, inputs.bodies().operandsOf(statement)[*index], [&](ValueFlowObjectID member) {
            if (sets.contains(valuesInsideType[variable.index()], member))
                return;
            storesIntegers |= member == ValueFlowUniverse::primitive(PrimitiveSet::Integer);
            isInside &= acceptsIntegers && member == ValueFlowUniverse::primitive(PrimitiveSet::Integer);
        });
        if (!isInside || members != ValueFlowMembers::Listed)
            return false;
        if (storesIntegers && isBehindCheck)
            setFlag(isStoredIntegersBehindChecks, variable.index());
        return true;
    }

    void noteStore(const ValueFlowStatement* statement, ValueSetID storedThrough, ValueFlowName name, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        forEachObjectStoredThrough(storedThrough, [&](ValueFlowObjectID object) {
            if (isNamespace(object))
                return;
            bool namesStaticField = false;
            for (const VariableOnObject& variable : variablesOn(object)) {
                if (!variable.mayBeNamedBy(name))
                    continue;
                namesStaticField |= name.kind == ValueFlowNameKind::Property && !variable.mayLieUnderAnyName;
                access.variable = variable.variable;
                found.append(access);
                if (statement && !indexOfValueStoredIntoProperty(statement->opcode))
                    setFlag(isStoredByBuiltin, variable.variable.index());
                else if (!statement || !storesValueInside(*statement, variable.variable))
                    setFlag(isStoredOtherType, variable.variable.index());
            }
            if (!namesStaticField && (!statement || statement->opcode != Opcode::DefineMethod))
                setFlag(isClassObjectStoredUnderOtherName, inputs.universe().closureFunction(object).index());
        });
    }

    void noteTestBy(const ValueFlowStatement& statement, ValueSetID chain, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        if (statement.opcode == Opcode::PropHas || statement.opcode == Opcode::ScopeHasBinding)
            noteLook(chain, nameOf(inputs.program.at(statement.function).at(statement.site).key()), access, found);
        if (statement.opcode != Opcode::ElemHas)
            return;
        Vector<PropertyKeyID, 4> keys;
        const ValueFlowOperand& property = inputs.bodies().operandsOf(statement)[OperandIndex::ElemHas::property];
        bool areListed = holdsFewMembers(property) && forEachMemberOf(inputs, property, [&](ValueFlowObjectID member) {
            keys.append(inputs.universe().keyOf(member));
        }) == ValueFlowMembers::Listed;
        if (!areListed || keys.contains(PropertyKeyID())) {
            noteLook(chain, everyPropertyName, access, found);
            return;
        }
        for (PropertyKeyID key : keys)
            noteLook(chain, nameOf(key), access, found);
    }

    void collectAccessesOf(ValueFlowStatementID id, Vector<ObjectAccess>& found)
    {
        const ValueFlowStatement& statement = solver.statementAt(id);
        bool isMadeByRule = solver.isMadeByRule(id);
        bool hasRun = isMadeByRule ? !solver.isAtRest(id) : !statement.function || inputs.isReached(statement.function);
        if (!hasRun || isSharedLookup(id))
            return;

        bool isOperation = !isMadeByRule && statement.kind == ValueFlowStatementKind::Operation && statement.function && statement.site;
        if (isOperation && statement.opcode == Opcode::NewClass)
            return;
        auto accessOf = [&](ObjectAccessKind kind) {
            return isOperation ? ObjectAccess { { }, statement.function, statement.site, kind } : ObjectAccess { { }, { }, { }, kind };
        };
        bool answersWhatItReads = isOperation && (statement.opcode == Opcode::PropGet || statement.opcode == Opcode::ElemGet);

        inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Written, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            ValueFlowNameKind kind = key.name().kind;
            if (key.kind == ValueFlowMadeNodeKind::Read && namesOfValues.contains(kind))
                noteLook(key.set, key.name(), accessOf(ObjectAccessKind::Observation), found);
            if (key.kind == ValueFlowMadeNodeKind::Written && namesOfValuesAndAccessors.contains(kind))
                noteStore(isOperation ? &statement : nullptr, key.set, key.name(), accessOf(ObjectAccessKind::Store), found);
            if (key.kind == ValueFlowMadeNodeKind::Written && namesOfRemovalsAndRedefinitions.contains(kind))
                noteChangeAskedOfNamespaces(key.set, key.name(), accessOf(ObjectAccessKind::Observation), found);
            if (key.kind != ValueFlowMadeNodeKind::Lookup || !made || isLookupOfCalleeByCall(inputs, statement, key))
                return;
            if (kind == ValueFlowNameKind::PrototypeLink) {
                if (isOperation)
                    noteTestBy(statement, solver.possibleValuesOf(solver.statementAt(made).result), accessOf(ObjectAccessKind::Observation), found);
            } else if (isSharedLookup(made) && reachesFollowedObject(made))
                noteLooksOf(made, accessOf(answersWhatItReads ? ObjectAccessKind::Read : ObjectAccessKind::Observation), found);
        });
    }

    void noteTestBy(const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinPropertyAction& action, SetsMeetingOneSet& setsWithFollowedObjects, Vector<ObjectAccess>& found)
    {
        const Program& program = inputs.program;
        const ValueFlowUniverse& universe = inputs.universe();
        ObjectAccess access { { }, application.function, application.site, ObjectAccessKind::Observation };

        ValueSetID lookedInto = application.valueAt(action.object, heap.maker());
        const BuiltinParameter* parameter = program.builtins().parameterFor(overload, action.object);
        bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
        if (!looksAtOwnPropertyAlone)
            lookedInto = heap.chainOf(lookedInto);
        if (!setsWithFollowedObjects.contains(lookedInto))
            return;

        switch (action.key.kind) {
        case BuiltinKeyKind::None:
            return;
        case BuiltinKeyKind::Names:
            for (BuiltinName name : action.key.names)
                noteLook(lookedInto, nameOf(program.keyOf(name)), access, found);
            return;
        case BuiltinKeyKind::Operand: {
            ValueSetID keys = application.valueAt(*action.key.operand, heap.maker());
            if (sets.isEmpty(keys) || !sets.isSubset(keys, universe.keys()) || sets.meet(keys, universe.builtinTables().heap().keysNobodyKnows()) || sets.size(keys) > inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
                break;
            sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
                noteLook(lookedInto, nameOf(universe.keyOf(constant)), access, found);
            });
            return;
        }
        case BuiltinKeyKind::Any:
        case BuiltinKeyKind::Indexes:
        case BuiltinKeyKind::ElementsOf:
            break;
        }
        noteLook(lookedInto, everyPropertyName, access, found);
    }

    void collectTestsOfBuiltins(Vector<ObjectAccess>& found)
    {
        const BuiltinTable& builtins = inputs.program.builtins();
        BitVector builtinsThatTest = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.flow.propertyActions, testsProperty);
        });
        SetsMeetingOneSet setsWithFollowedObjects(sets, setOfObjects);
        forEachBuiltinApplication(inputs, heap, builtinsThatTest, [&](const ValueFlowBuiltinApplication& application) {
            for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
                if (overload.mode != application.mode())
                    continue;
                for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                    if (testsProperty(action))
                        noteTestBy(application, overload, action, setsWithFollowedObjects, found);
                }
            }
        });
    }

    void addNamespaceObjects()
    {
        const Program& program = inputs.program;
        bool modulesAreNamedAtRunTime = unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution);
        ValueSetID created = solver.possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::CreatedNamespaces));
        if (sets.meet(created, inputs.universe().placeholdersAlone()))
            return;
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            if (module.kind == ModuleKind::Runtime || module.kind == ModuleKind::External || !inputs.build.isEnabled(FlowResultKind::ObjectUses, index))
                continue;
            if (modulesAreNamedAtRunTime && mayAnswerToName(program, ModuleID(index)))
                continue;
            ValueFlowObjectID namespaceObject = inputs.universe().namespaceOf(ModuleID(index));
            if (!sets.contains(created, namespaceObject)) {
                results.namespaceUses[index] = NamespaceUse::NeverCreated;
                continue;
            }
            bool isInUnknownHands = inputs.hasEscaped(namespaceObject) || inputs.isLost(namespaceObject) || inputs.isHeldByNativeCode(namespaceObject)
                || markedMutationsOf(inputs, namespaceObject).containsAny({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript });
            // What is asked under an index names no export.
            bool hasExportUnderIndex = std::ranges::any_of(module.exports, [&](const ModuleExport& entry) {
                return mayBeElementKey(program.strings.nameClass(entry.name));
            });
            if (isInUnknownHands || hasExportUnderIndex || commonJsModuleCapturedBy(program, module))
                continue;
            objects.append(namespaceObject);
            for (uint32_t position = 0; position < module.exports.size(); ++position) {
                const ModuleExport& entry = module.exports[position];
                if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                    continue;
                PropertyKeyID key = module.namespaceLayout ? program.at(module.namespaceLayout).fields[position].key : program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }).value_or(PropertyKeyID());
                variables.append({ namespaceObject, entry.variable, key, false });
            }
        }
    }

    void addClassObjects(const ValueFlowStampedCalls& calls, Vector<ValueFlowObjectID>& escapedObjects)
    {
        for (const Layout& layout : inputs.program.layouts) {
            if (layout.staticFields.isEmpty())
                continue;
            ValueFlowObjectID classObject = layout.constructor ? inputs.universe().closureOf(layout.constructor) : ValueFlowObjectID();
            OptionSet<ObjectMutation> marked = classObject ? markedMutationsOf(inputs, classObject) : OptionSet<ObjectMutation> { };
            bool isFollowed = classObject && !inputs.isLost(classObject) && !inputs.isHeldByNativeCode(classObject) && !marked.contains(ObjectMutation::ReachUnknownJavaScript)
                && !calls.functionsNamedByUnsolvedCode.get(layout.constructor.index());
            bool hasEscaped = isFollowed && (inputs.hasEscaped(classObject) || marked.contains(ObjectMutation::ReachNativeCode));
            if (isFollowed)
                objects.append(classObject);
            if (hasEscaped)
                escapedObjects.append(classObject);
            for (const StaticField& field : layout.staticFields) {
                uint32_t variable = field.variable.index();
                if (!isFollowed) {
                    results.staticFieldsInUnknownHands.quickSet(variable);
                    continue;
                }
                variables.append({ classObject, field.variable, field.key, !field.key || inputs.program.isIndexLike(field.key) });
                if (hasEscaped)
                    results.staticFieldsOfEscapedClassObjects.quickSet(variable);
                if (marked.contains(ObjectMutation::DeleteField))
                    results.deletedStaticFields.quickSet(variable);
                if (marked.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity }))
                    results.redefinedStaticFields.quickSet(variable);
            }
        }
    }

    void collect(const ValueFlowStampedCalls& calls)
    {
        const Program& program = inputs.program;
        results.namespaceUses.fill(NamespaceUse::Unknown, program.modules.size());
        for (BitVector* ofVariables : { &results.staticFieldsInUnknownHands, &results.staticFieldsOfEscapedClassObjects, &results.deletedStaticFields, &results.redefinedStaticFields, &results.staticFieldsStoredOtherTypes, &results.staticFieldsStoredIntegersBehindChecks, &results.staticFieldsStoredByBuiltins })
            ofVariables->ensureSize(program.variables.size());
        results.classObjectsStoredUnderStaticFieldsAlone.ensureSize(program.layouts.size());

        Vector<ValueFlowObjectID> escapedObjects;
        addNamespaceObjects();
        addClassObjects(calls, escapedObjects);
        if (objects.isEmpty())
            return;

        setOfObjects = heap.maker().of(objects.span());
        setOfEscapedObjects = heap.maker().of(escapedObjects.span());
        AOT::stableSort(variables, { }, &VariableOnObject::object);
        setsWithObject.fill(0, sets.endOfTableBySet() / 4);
        lookupReachesFollowedObject.fill(0, solver.numberOfStatements() - solver.input().statements.size());
        isNamespaceReadUnderUnknownName.fill(0, program.modules.size());
        isStoredOtherType.fill(0, program.variables.size());
        isStoredIntegersBehindChecks.fill(0, program.variables.size());
        isStoredByBuiltin.fill(0, program.variables.size());
        isClassObjectStoredUnderOtherName.fill(0, program.functions.size());
        valuesInsideType.fill(ValueSets::nothing, program.variables.size());
        for (const VariableOnObject& variable : variables) {
            const DeclaredType& declared = program.at(variable.variable).declared;
            if (!isNamespace(variable.object) && !declared.isExpected())
                valuesInsideType[variable.variable.index()] = inputs.universe().membersSurelyInside(heap.maker(), program, *declared.established());
        }
        findLookupsReachingFollowedObjects();

        Vector<ObjectAccess> testsOfBuiltins;
        runTasksInParallel(inputs.loops(), {
            [&] {
                results.accesses = collectInParallel<ObjectAccess>(inputs.loops(), solver.numberOfStatements(), [&](unsigned index, Vector<ObjectAccess>& found) {
                    collectAccessesOf(ValueFlowStatementID(index), found);
                });
            },
            [&] {
                collectTestsOfBuiltins(testsOfBuiltins);
            },
        });
        results.accesses.appendVector(WTF::move(testsOfBuiltins));
    }

    const ValueFlowResultInputs& inputs;
    const ValueFlowSolver& solver;
    const ValueSets& sets;
    ValueFlowHeapView heap;
    ValueFlowVariablesOnObjects results;
    Vector<ValueFlowObjectID> objects;
    ValueSetID setOfObjects { ValueSets::nothing };
    ValueSetID setOfEscapedObjects { ValueSets::nothing };
    Vector<VariableOnObject> variables;
    Vector<uint8_t> setsWithObject;
    Vector<uint8_t> lookupReachesFollowedObject;
    Vector<uint8_t> isNamespaceReadUnderUnknownName;
    Vector<uint8_t> isStoredOtherType;
    Vector<uint8_t> isStoredIntegersBehindChecks;
    Vector<uint8_t> isStoredByBuiltin;
    Vector<uint8_t> isClassObjectStoredUnderOtherName;
    Vector<ValueSetID> valuesInsideType;
};

void ValueFlowAccessesThroughObjectsDeleter::operator()(ValueFlowAccessesThroughObjects* accesses) const
{
    delete accesses;
}

std::unique_ptr<ValueFlowAccessesThroughObjects, ValueFlowAccessesThroughObjectsDeleter> collectAccessesThroughObjects(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls)
{
    std::unique_ptr<ValueFlowAccessesThroughObjects, ValueFlowAccessesThroughObjectsDeleter> collected(new ValueFlowAccessesThroughObjects(inputs));
    collected->collect(calls);
    return collected;
}

ValueFlowVariablesOnObjects computeVariablesOnObjects(const ValueFlowResultInputs& inputs, ValueFlowAccessesThroughObjects& collected, const ValueFlowUsesOfHeap& usesOfHeap)
{
    ValueFlowVariablesOnObjects& results = collected.results;
    if (collected.objects.isEmpty())
        return WTF::move(results);
    collected.forEachObjectIn(objectsThatMayBeEnumerated(usesOfHeap), [&](ValueFlowObjectID object) {
        collected.noteLook(collected.heap.maker().of(object), everyPropertyName, { }, results.accesses);
    });
    for (uint32_t index = 0; index < inputs.program.variables.size(); ++index) {
        results.staticFieldsStoredOtherTypes.quickSet(index, collected.isStoredOtherType[index]);
        results.staticFieldsStoredIntegersBehindChecks.quickSet(index, collected.isStoredIntegersBehindChecks[index]);
        results.staticFieldsStoredByBuiltins.quickSet(index, collected.isStoredByBuiltin[index]);
    }
    for (ValueFlowObjectID object : collected.objects) {
        if (collected.isNamespace(object) && !collected.isNamespaceReadUnderUnknownName[inputs.universe().moduleOf(object).index()])
            results.namespaceUses[inputs.universe().moduleOf(object).index()] = NamespaceUse::NamedReadsOnly;
    }
    for (uint32_t index = 0; index < inputs.program.layouts.size(); ++index) {
        const Layout& layout = inputs.program.layouts[index];
        if (!layout.staticFields.isEmpty() && layout.constructor && inputs.sets().contains(collected.setOfObjects, inputs.universe().closureOf(layout.constructor)) && !collected.isClassObjectStoredUnderOtherName[layout.constructor.index()])
            results.classObjectsStoredUnderStaticFieldsAlone.quickSet(index);
    }
    sortAndRemoveDuplicates(inputs.loops(), results.accesses);
    return WTF::move(results);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
