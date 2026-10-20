/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowRecords.h"
#include "AOTLayout.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static bool hasNodeOfItsOwn(const Field& field)
{
    return field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden });
}

namespace {

using SetsByNumber = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

ValueSetID setAt(const SetsByNumber& sets, uint32_t number)
{
    auto found = sets.find(number);
    return found == sets.end() ? ValueSets::nothing : found->value;
}

// A property is written through a set of objects under a name. This sorts what is written by the layouts of the objects.
class StoresByLayout {
public:
    explicit StoresByLayout(const ValueFlowResultInputs&);

    void collect();
    // All that a store may put under the key into an object of the layout.
    ValueSetID storedUnder(LayoutID, PropertyKeyID);
    // The properties of other objects are copied onto an object of the layout, and nobody has followed which.
    bool getsCopiedProperties(LayoutID layout) const { return m_getsCopiedProperties.quickGet(layout.index()) || (m_unknownObjectGetsCopiedProperties && m_hasEscapedObject.quickGet(layout.index())); }

private:
    struct ObjectsOfSet {
        Vector<LayoutID, 4> layouts;
        bool holdsUnknownObject { false };
    };

    const ObjectsOfSet& objectsOf(ValueSetID);
    void unite(ValueSetID& into, ValueSetID value) { into = m_maker.unite(into, value); }
    void note(const ValueFlowNodeKey&, ValueSetID value);

    const ValueFlowResultInputs& m_inputs;
    ValueSetMaker m_maker;
    HashMap<uint32_t, ObjectsOfSet, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_objectsOfSet;
    Vector<SetsByNumber> m_storedUnderKey;
    Vector<ValueSetID> m_storedUnderUnknownKey;
    BitVector m_getsCopiedProperties;
    BitVector m_hasEscapedObject;
    // What is stored through the unknown object is stored into every object that has escaped.
    SetsByNumber m_storedThroughUnknownObject;
    ValueSetID m_storedThroughUnknownObjectUnderUnknownKey { ValueSets::nothing };
    bool m_unknownObjectGetsCopiedProperties { false };
};

} // anonymous namespace

StoresByLayout::StoresByLayout(const ValueFlowResultInputs& inputs)
    : m_inputs(inputs)
    , m_maker(inputs.bodies().sets())
    , m_storedUnderKey(inputs.program.layouts.size())
    , m_storedUnderUnknownKey(inputs.program.layouts.size(), [](size_t) { return ValueSets::nothing; })
    , m_getsCopiedProperties(inputs.program.layouts.size())
    , m_hasEscapedObject(inputs.program.layouts.size())
{
}

static LayoutID layoutOfObject(const ValueFlowUniverse& universe, ValueFlowObjectID object)
{
    ValueFlowObjectKind kind = universe.kindOf(object);
    return kind == ValueFlowObjectKind::Site || kind == ValueFlowObjectKind::Instance ? universe.layoutOf(object) : LayoutID();
}

const StoresByLayout::ObjectsOfSet& StoresByLayout::objectsOf(ValueSetID set)
{
    return m_objectsOfSet.ensure(set.index(), [&] {
        ObjectsOfSet objects;
        m_inputs.sets().forEachMember(set, [&](ValueFlowObjectID member) {
            if (member == ValueFlowUniverse::unknownObject())
                objects.holdsUnknownObject = true;
            else if (LayoutID layout = layoutOfObject(m_inputs.universe(), member))
                objects.layouts.append(layout);
        });
        std::ranges::sort(objects.layouts);
        removeRepeatedElements(objects.layouts);
        return objects;
    }).iterator->value;
}

void StoresByLayout::note(const ValueFlowNodeKey& key, ValueSetID value)
{
    ValueFlowName name = key.name();
    const ObjectsOfSet& objects = objectsOf(key.set);
    switch (name.kind) {
    case ValueFlowNameKind::Property:
        for (LayoutID layout : objects.layouts)
            unite(m_storedUnderKey[layout.index()].add(name.payload, ValueSets::nothing).iterator->value, value);
        if (objects.holdsUnknownObject)
            unite(m_storedThroughUnknownObject.add(name.payload, ValueSets::nothing).iterator->value, value);
        return;
    case ValueFlowNameKind::UnknownKey:
        for (LayoutID layout : objects.layouts)
            unite(m_storedUnderUnknownKey[layout.index()], value);
        if (objects.holdsUnknownObject)
            unite(m_storedThroughUnknownObjectUnderUnknownKey, value);
        return;
    case ValueFlowNameKind::CopiedFrom:
        for (LayoutID layout : objects.layouts)
            m_getsCopiedProperties.quickSet(layout.index());
        m_unknownObjectGetsCopiedProperties |= objects.holdsUnknownObject;
        return;
    default:
        return;
    }
}

void StoresByLayout::collect()
{
    const ValueFlowSolver& solver = m_inputs.solver();
    for (uint32_t index = 0; index < solver.graph().numberOfNodes(); ++index) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(index));
        if (!key || key->kind != ValueFlowMadeNodeKind::Written)
            continue;
        ValueSetID value = solver.valueOf(ValueFlowNodeID(index));
        if (value != ValueSets::nothing)
            note(*key, value);
    }
    m_inputs.sets().forEachMember(solver.valueOf(ValueFlowProgramNodes::escaped()), [&](ValueFlowObjectID object) {
        if (LayoutID layout = layoutOfObject(m_inputs.universe(), object))
            m_hasEscapedObject.quickSet(layout.index());
    });
}

ValueSetID StoresByLayout::storedUnder(LayoutID layout, PropertyKeyID key)
{
    ValueSetID stored = m_maker.unite(setAt(m_storedUnderKey[layout.index()], key.index()), m_storedUnderUnknownKey[layout.index()]);
    if (!m_hasEscapedObject.quickGet(layout.index()))
        return stored;
    return m_maker.unite(stored, m_maker.unite(setAt(m_storedThroughUnknownObject, key.index()), m_storedThroughUnknownObjectUnderUnknownKey));
}

// The fields that some allocation leaves out. A class below another gets its own fields after the constructor of the base has made the object,
// and the instance of a constructor function is made without any.
static void noteFieldsMissingAtCreation(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, Vector<BitVector>& isMissingAtCreation)
{
    const Program& program = inputs.program;
    auto noteMissing = [&](LayoutID layout, uint32_t field) {
        FieldReference declaration = declarationOf(program, { layout, field });
        isMissingAtCreation[declaration.layout.index()].quickSet(declaration.field);
    };
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!layout.flags.containsAny({ LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction }))
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
                noteMissing(LayoutID(index), field);
        }
    }
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index)))
            continue;
        for (const Value& value : program.functions[index].values) {
            if (value.opcode != Opcode::NewObject)
                continue;
            std::span<const uint32_t> listed = value.fields();
            for (uint32_t field = 0; field < program.at(value.layout()).fields.size(); ++field) {
                if (!std::ranges::contains(listed, field))
                    noteMissing(value.layout(), field);
            }
        }
    }
    // An object that has its layout from something else than an allocation with that layout.
    const ValueFlowUniverse& universe = inputs.universe();
    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        ValueFlowObjectID object(index);
        if (universe.kindOf(object) != ValueFlowObjectKind::Site || !universe.layoutOf(object))
            continue;
        const ValueFlowSite& site = *universe.siteOf(object);
        if (site.origin == ValueFlowSiteOrigin::Operation && !site.ordinal && program.at(universe.functionOf(object)).at(site.value).opcode == Opcode::NewObject)
            continue;
        for (uint32_t field = 0; field < program.at(site.layout).fields.size(); ++field)
            noteMissing(site.layout, field);
    }
}

ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    BuildContext& build = inputs.build;
    FlowApplication& application = *tables.application;
    uint32_t numberOfLayouts = program.layouts.size();

    ValueFlowFieldContents contents;
    contents.stored = Vector<Vector<ValueSetID>>(numberOfLayouts, [&](size_t index) {
        return Vector<ValueSetID>(program.layouts[index].fields.size(), [](size_t) { return ValueSets::nothing; });
    });
    Vector<Vector<OptionSet<ObjectMutation>>> possibleMutations(numberOfLayouts, [&](size_t index) {
        return Vector<OptionSet<ObjectMutation>>(program.layouts[index].fields.size());
    });
    Vector<BitVector> hasObjects(numberOfLayouts, [&](size_t index) {
        return BitVector(program.layouts[index].fields.size());
    });
    Vector<BitVector> isMissingAtCreation = hasObjects;
    noteFieldsMissingAtCreation(inputs, tables, isMissingAtCreation);

    StoresByLayout stores(inputs);
    stores.collect();
    ValueSetMaker maker(inputs.bodies().sets());

    // The objects of a layout hold the fields of the layouts above it too: what happens to them happens to those fields.
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = program.layouts[index];
        const LayoutFlow& flow = application.layouts[index].flow;
        if (!hasObjectsThatCodeAlters(layout.kind) || flow.allocation == AllocationState::NeverAllocated)
            continue;
        OptionSet<ObjectMutation> possible = flow.allocationClasses.isEmpty() || stores.getsCopiedProperties(LayoutID(index)) ? allObjectMutations : allObjectMutations - flow.impossibleMutations;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            FieldReference declaration = declarationOf(program, { LayoutID(index), field });
            hasObjects[declaration.layout.index()].quickSet(declaration.field);
            possibleMutations[declaration.layout.index()][declaration.field].add(possible);
            PropertyKeyID key = layout.fields[field].key;
            if (!key)
                possibleMutations[declaration.layout.index()][declaration.field] = allObjectMutations;
            else if (!hasNodeOfItsOwn(layout.fields[field])) {
                ValueSetID& stored = contents.stored[declaration.layout.index()][declaration.field];
                stored = maker.unite(stored, stores.storedUnder(LayoutID(index), key));
            }
        }
    }

    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!hasObjects[index].quickGet(field) || layout.fields[field].flags.contains(FieldFlags::IsInherited))
                continue;
            if (hasNodeOfItsOwn(layout.fields[field]))
                contents.stored[index][field] = inputs.solver().valueOf(inputs.bodies().nodes().field(LayoutID(index), field));

            OptionSet<ObjectMutation> possible = possibleMutations[index][field];
            FieldFlow& flow = application.layouts[index].fields[field].flow;
            if (!possible.contains(ObjectMutation::ReachUnknownJavaScript) && build.isEnabled(FlowResultKind::StoreCounts, index))
                flow.location.writers = WriterState::AllKnown;
            if (build.isEnabled(FlowResultKind::HolderMutations, index)) {
                if (!possible.contains(ObjectMutation::DeleteField))
                    flow.stability.add(FieldStability::IsNeverDeleted);
                if (!possible.contains(ObjectMutation::RedefineProperty))
                    flow.stability.add(FieldStability::IsNeverRedefined);
            }
            if (!isMissingAtCreation[index].quickGet(field) && flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::Initialization, index))
                flow.location.initialization = InitializationState::SetAtCreation;
        }
    }
    return contents;
}

void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutFlow& flow = application.layouts[index].flow;
        if (flow.allocationClasses.isEmpty())
            continue;
        bool isNeverStoredOtherTypes = true;
        for (uint32_t field = 0; field < program.layouts[index].fields.size(); ++field) {
            FieldReference declaration = declarationOf(program, { LayoutID(index), field });
            isNeverStoredOtherTypes &= application.layouts[declaration.layout.index()].fields[declaration.field].flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
        }
        if (!isNeverStoredOtherTypes)
            continue;
        flow.impossibleMutations.add(ObjectMutation::StoreOfOtherType);
        for (AllocationClassID allocationClass : flow.allocationClasses)
            tables.sites.allocationClasses[allocationClass.index()].impossibleMutations.add(ObjectMutation::StoreOfOtherType);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
