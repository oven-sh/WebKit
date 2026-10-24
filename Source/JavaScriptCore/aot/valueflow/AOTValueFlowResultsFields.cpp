/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"

namespace JSC { namespace AOT { namespace Typed {

void ValueFlowUsesOfHeapDeleter::operator()(ValueFlowUsesOfHeap* usesOfHeap) const
{
    delete usesOfHeap;
}

std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter> computeUsesOfHeap(const ValueFlowResultInputs& inputs)
{
    return std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter>(new ValueFlowUsesOfHeap(inputs));
}

ValueFlowHeapView& heapViewOf(ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.heap;
}

ValueSetID objectsThatMayBeEnumerated(const ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.uses.objectsThatMayBeEnumerated();
}

namespace {

// What the steps below share.
struct FieldResultsWithoutStores {
    const ValueFlowResultInputs& inputs;
    FlowResultTables& tables;
    FlowApplication& application;
    const ValueFlowConstructions& constructions;

    const Program& program() const { return inputs.program; }
    bool hasObjects(LayoutID id) const { return hasObjectsThatCodeAlters(program().at(id).kind) && application.layouts[id.index()].flow.allocation != AllocationState::NeverAllocated; }
    OptionSet<ObjectMutation> possibleMutationsOf(LayoutID id) const
    {
        const LayoutFlow& flow = application.layouts[id.index()].flow;
        return flow.allocationClasses.isEmpty() ? allObjectMutations : allObjectMutations - flow.impossibleMutations;
    }
};

struct FieldResults : FieldResultsWithoutStores {
    ValueFlowHeapView& heap;
    const LayoutsOfObjects& layoutsOfObjects;
    StoresByLayout& stores;

    // The view remembers these chains and makes them with its own maker, which no thread of a loop uses for anything else. So the threads of a loop ask one at a time.
    mutable Lock lockOfChainsBuiltByProgram { };

    ValueSetID chainBuiltByProgram(ValueSetID objects) const
    {
        Locker locker { lockOfChainsBuiltByProgram };
        return heap.chainBuiltByProgram(objects);
    }
};

// A NewObject or a Data in a function that is reached or that is compiled without a solve.
struct AllocatingOperation {
    FunctionID function;
    ValueID value;
    // See findAllocationsThatGetMoreFields().
    bool getsMoreFields { false };
};

} // anonymous namespace

// The objects of a layout hold the fields of the layouts above it too.
static void forEachFieldOfObjectsWith(const Program& program, const FlowApplication& application, LayoutID id, NOESCAPE const Invocable<void(FieldReference declaration, const Field&)> auto& functor)
{
    Vector<LayoutID, 4> layoutsBelow;
    Vector<FieldReference, 16> visited;
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            bool isHidden = field.key && std::ranges::any_of(layoutsBelow, [&](LayoutID below) {
                return program.at(below).findField(field.key).has_value();
            });
            if (isHidden)
                continue;
            FieldReference declaration = declarationOf(program, { current, index });
            if (!layoutsBelow.isEmpty() && visited.contains(declaration))
                continue;
            visited.append(declaration);
            functor(declaration, field);
        }
        layoutsBelow.append(current);
        current = provenBaseOf(application, current);
    }
}

static bool declaresFieldOnChain(const Program& program, const FlowApplication& application, LayoutID id, PropertyKeyID key)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (program.at(current).findField(key))
            return true;
        current = provenBaseOf(application, current);
    }
    return false;
}

// Own keys other than array indexes are listed in creation order, so the members behind a name that is no field are defined after the allocation.
static Vector<uint32_t, 16> fieldsFilledBy(const Program& program, const Layout& declared, const DataNode::Object& object)
{
    Vector<uint32_t, 16> fields;
    for (const DataMember& member : object.members) {
        if (std::optional<uint32_t> field = declared.findField(member.key)) {
            fields.append(*field);
            continue;
        }
        if (program.nameClassOf(member.key) != NameClass::Index)
            break;
    }
    return fields;
}

// `{ wrap: "nowrap", ...style, overflow: .. }` allocates an object with `wrap` and defines `overflow` in it before anybody else sees it: no finished object has the keys of the allocation alone.
static BitVector findAllocationsThatGetMoreFields(const Program& program, const Function& function)
{
    BitVector allocations(function.values.size());
    for (const Value& value : function.values) {
        if (value.opcode != Opcode::PropDefine || !value.key())
            continue;
        ValueID object = value.operand(OperandIndex::PropDefine::object);
        const Value& allocation = function.at(object);
        if (allocation.opcode != Opcode::NewObject)
            continue;
        std::optional<uint32_t> field = program.at(declaredLayoutOf(program, allocation.layout())).findField(value.key());
        if (!field || !std::ranges::contains(allocation.fields(), *field))
            allocations.quickSet(object.index());
    }
    return allocations;
}

// In the order of the functions, and in a function in the order of its values. The steps that look at the allocations walk this list and not the program.
static Vector<AllocatingOperation> collectAllocatingOperations(const FieldResultsWithoutStores& results)
{
    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    return collectInParallel<AllocatingOperation>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<AllocatingOperation>& operations) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::Data })))
            return;
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, results.tables, FunctionID(index)))
            return;
        const Function& function = program.functions[index];
        size_t numberOfOtherOperations = operations.size();
        bool allocatesObject = false;
        bool definesProperty = false;
        for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            Opcode opcode = function.values[valueIndex].opcode;
            allocatesObject |= opcode == Opcode::NewObject;
            definesProperty |= opcode == Opcode::PropDefine;
            if (opcode == Opcode::NewObject || opcode == Opcode::Data)
                operations.append({ FunctionID(index), ValueID(valueIndex) });
        }
        if (!allocatesObject || !definesProperty)
            return;
        BitVector allocationsThatGetMoreFields = findAllocationsThatGetMoreFields(program, function);
        for (AllocatingOperation& operation : operations.mutableSpan().subspan(numberOfOtherOperations))
            operation.getsMoreFields = allocationsThatGetMoreFields.quickGet(operation.value.index());
    });
}

// The fields that an object may lack when somebody else than the code that makes it gets hold of it.
static void noteFieldsMissingAtCreation(const FieldResults& results, std::span<const AllocatingOperation> operations, Vector<BitVector>& isMissingAtCreation)
{
    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    const ParallelLoops& loops = inputs.loops();
    auto noteMissing = [&](const Vector<FieldReference>& declarations) {
        for (FieldReference declaration : declarations)
            isMissingAtCreation[declaration.layout.index()].quickSet(declaration.field);
    };

    // A constructor that the analysis has followed fills a field before the object leaves its hands, or it does not.
    noteMissing(collectInParallel<FieldReference>(loops, program.layouts.size(), [&](unsigned index, Vector<FieldReference>& missing) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (!results.hasObjects(id))
            return;
        if (results.constructions.isFollowed(id)) {
            forEachFieldOfObjectsWith(program, results.application, id, [&](FieldReference declaration, const Field& field) {
                if (!results.constructions.isStoredBeforeObjectLeaks(id, field.key, declaration))
                    missing.append(declaration);
            });
            return;
        }
        if (!layout.flags.containsAny({ LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction }))
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
                missing.append(declarationOf(program, { id, field }));
        }
        // The constructors above fill their fields by name, and an accessor of this class takes such a store.
        forEachFieldOfObjectsWith(program, results.application, id, [&](FieldReference declaration, const Field& field) {
            if (declaration.layout != id && results.constructions.mayBeKeptFromCreatingInheritedField(id, field.key))
                missing.append(declaration);
        });
    }));

    noteMissing(collectInParallel<FieldReference>(loops, operations.size(), [&](unsigned position, Vector<FieldReference>& missing) {
        const Value& value = program.at(operations[position].function).at(operations[position].value);
        // Constant data is whole when the operation hands it out. ExpandData makes an allocation of it and, for the members behind a name that is no field, definitions that find their slots empty.
        if (value.opcode == Opcode::Data) {
            forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
                const DataNode::Object* object = node.tryAs<DataNode::Object>();
                if (!object || !object->layout || results.constructions.isFollowed(object->layout))
                    return;
                const Layout& layout = program.at(object->layout);
                Vector<uint32_t, 16> listed = fieldsFilledBy(program, layout, *object);
                for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                    if (!listed.contains(field))
                        missing.append(declarationOf(program, { object->layout, field }));
                }
            });
            return;
        }
        if (program.at(value.layout()).isEnvironment() || results.constructions.isFollowed(value.layout()))
            return;
        std::span<const uint32_t> listed = value.fields();
        for (uint32_t field = 0; field < program.at(value.layout()).fields.size(); ++field) {
            if (!std::ranges::contains(listed, field))
                missing.append(declarationOf(program, { value.layout(), field }));
        }
    }));

    // An object that has its layout from something else than an allocation with that layout.
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<LayoutID> layoutsOfOtherObjects = collectInParallel<LayoutID>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<LayoutID>& layouts) {
        ValueFlowObjectID object(index);
        if (universe.kindOf(object) != ValueFlowObjectKind::Site || !universe.layoutOf(object))
            return;
        const ValueFlowSite& site = *universe.siteOf(object);
        if (results.constructions.isFollowed(site.layout) || isSiteOfTypedResultThatNothingAllocates(inputs, object) || !results.hasObjects(site.layout))
            return;
        if (site.origin == ValueFlowSiteOrigin::DataNode)
            return;
        if (site.origin != ValueFlowSiteOrigin::TypedResult && !inputs.isReached(universe.functionOf(object)) && !isUnsolvedCode(inputs, results.tables, universe.functionOf(object)))
            return;
        if (site.origin == ValueFlowSiteOrigin::Operation && !site.ordinal && program.at(universe.functionOf(object)).at(site.value).opcode == Opcode::NewObject)
            return;
        layouts.append(site.layout);
    });
    AOT::sort(layoutsOfOtherObjects);
    removeRepeatedElements(layoutsOfOtherObjects);
    for (LayoutID layout : layoutsOfOtherObjects) {
        forEachFieldOfObjectsWith(program, results.application, layout, [&](FieldReference declaration, const Field&) {
            isMissingAtCreation[declaration.layout.index()].quickSet(declaration.field);
        });
    }
}

// An object that nobody but followed code gets hold of.
static bool isSeenByFollowedCodeAlone(const FieldResults& results, LayoutID id)
{
    return !results.possibleMutationsOf(id).containsAny(mutationsThatReach) && !results.stores.hasObjectOfCodeWithoutSolve(id);
}

// All that lies in such an object was written there, or the table of built-ins lists it.
static bool holdsOnlyWhatIsWrittenUnder(const ValueFlowResultInputs& inputs, ValueFlowObjectID object, PropertyKeyID key)
{
    const ValueFlowUniverse& universe = inputs.universe();
    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
        return true;
    case ValueFlowObjectKind::Site:
    case ValueFlowObjectKind::Instance: {
        LayoutID layout = universe.layoutOf(object);
        return layout && hasObjectsThatCodeAlters(inputs.program.at(layout).kind) && !inputs.classOf(object);
    }
    case ValueFlowObjectKind::Prototype:
        return key != universe.builtinTables().heap().constructorKey();
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// What a thread of a loop asks the heap about the objects of one layout.
class ObjectsOfLayoutInHeap {
public:
    // The chains that the thread has walked, by the objects that they start at: the objects of most layouts have the same prototypes.
    using Chains = SetsByNumber;

    ObjectsOfLayoutInHeap(const FieldResults& results, ValueSetMaker& maker, Chains& chains, LayoutID id)
        : m_results(results)
        , m_maker(maker)
        , m_chains(chains)
        , m_id(id)
        , m_objects(results.layoutsOfObjects.objectsWith(id))
    {
    }

    ValueSetID objects() const { return m_objects; }
    // A value, an accessor or a member of a built-in object may lie under the key in an object that one of these inherits from.
    bool mayFindNameAbove(PropertyKeyID);
    // Nor does anything come there while the program runs.
    bool inheritsNothingUnder(PropertyKeyID);
    bool isObservedByStoresOfHeirs(PropertyKeyID);
    bool mayBeDeletedUnder(PropertyKeyID);
    bool mayBeRedefinedUnder(PropertyKeyID);

private:
    bool isWrittenUnder(ValueSetID objects, ValueFlowName name) { return !m_results.inputs.sets().isEmpty(std::as_const(m_results.heap).writtenUnder(m_maker, objects, name)); }
    ValueSetID objectsAbove();

    const FieldResults& m_results;
    ValueSetMaker& m_maker;
    Chains& m_chains;
    LayoutID m_id;
    ValueSetID m_objects;
    // None until the first field asks.
    ValueSetID m_objectsAbove;
};

} // anonymous namespace

ValueSetID ObjectsOfLayoutInHeap::objectsAbove()
{
    if (!m_objectsAbove) {
        const ValueFlowHeapView& heap = m_results.heap;
        ValueSetID prototypes = heap.prototypesOf(m_maker, m_objects);
        m_objectsAbove = m_chains.ensure(prototypes.index(), [&] {
            return heap.chainOf(m_maker, prototypes);
        }).iterator->value;
    }
    return m_objectsAbove;
}

bool ObjectsOfLayoutInHeap::mayFindNameAbove(PropertyKeyID key)
{
    const ValueFlowResultInputs& inputs = m_results.inputs;
    const ValueSets& sets = inputs.sets();
    ValueSetID above = objectsAbove();
    if (ValueSets::isComplement(above) || sets.meet(above, inputs.universe().placeholdersAlone()))
        return true;
    const ValueFlowMembersUnderKey& members = inputs.universe().builtinTables().heap().under(key);
    if (sets.meet(above, members.holders) || sets.meet(above, members.holdersOfUndeclaredProperties))
        return true;

    ValueFlowName names[] {
        ValueFlowName::property(key),
        { ValueFlowNameKind::Getter, key.index() },
        { ValueFlowNameKind::Setter, key.index() },
        ValueFlowName::of(ValueFlowNameKind::UnknownKey),
        ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey),
        ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey),
        ValueFlowName::of(ValueFlowNameKind::CopiedFrom),
    };
    for (ValueFlowName name : names) {
        if (std::as_const(m_results.heap).writtenUnder(m_maker, above, name) != ValueSets::nothing)
            return true;
    }
    bool mayHoldMore = false;
    sets.forEachMember(above, [&](ValueFlowObjectID object) {
        mayHoldMore |= !holdsOnlyWhatIsWrittenUnder(inputs, object, key);
    });
    return mayHoldMore;
}

// The analysis leaves the objects of the realm out of a write through an object nobody knows, so the results about the built-in holders answer for them.
bool ObjectsOfLayoutInHeap::inheritsNothingUnder(PropertyKeyID key)
{
    const ValueFlowResultInputs& inputs = m_results.inputs;
    const Program& program = inputs.program;
    if (program.isIndexLike(key) || m_results.stores.hasObjectOfCodeWithoutSolve(m_id) || m_results.stores.getsCopiedProperties(m_id) || mayFindNameAbove(key))
        return false;
    RELEASE_ASSERT(m_results.tables.builtins.areFilled);
    bool mayGainName = false;
    inputs.sets().forEachMember(m_maker.intersect(objectsAbove(), inputs.universe().builtinObjects()), [&](ValueFlowObjectID object) {
        BuiltinID holder = program.builtins().canonicalID(inputs.universe().builtinOf(object));
        if (!listsEveryMemberUnder(program, holder, key))
            mayGainName = true;
        else if (!isClosedKey(program, holder, key))
            mayGainName |= !builtinHolderNeverGains(m_results.tables.builtins, holder, key);
    });
    return !mayGainName;
}

// A store through an object that inherits from this one ends its walk at the field.
bool ObjectsOfLayoutInHeap::isObservedByStoresOfHeirs(PropertyKeyID key)
{
    OptionSet<ObjectMutation> possible = m_results.possibleMutationsOf(m_id);
    if (!possible.contains(ObjectMutation::BecomePrototype))
        return false;
    return possible.containsAny({ ObjectMutation::ChangeIntegrity, ObjectMutation::RedefineProperty }) || mayFindNameAbove(key);
}

// HeapRule::noteDeletedMembers() has put down every deletion, under its key or as one under a key nobody knows.
bool ObjectsOfLayoutInHeap::mayBeDeletedUnder(PropertyKeyID key)
{
    return isWrittenUnder(m_objects, { ValueFlowNameKind::DeletedMember, key.index() }) || isWrittenUnder(m_objects, ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey));
}

// HeapRule::markRedefinition() has put down every redefinition but those through a proxy, under its key or as one under a key nobody knows.
bool ObjectsOfLayoutInHeap::mayBeRedefinedUnder(PropertyKeyID key)
{
    return isWrittenUnder(m_objects, { ValueFlowNameKind::RedefinedMember, key.index() }) || isWrittenUnder(m_objects, ValueFlowName::of(ValueFlowNameKind::RedefinedMemberUnderUnknownKey));
}

static void computeKeyEnumerations(const FieldResults& results, const UsesOfObjects& uses)
{
    const Program& program = results.program();
    Vector<uint32_t> layoutsNeverEnumerated = findIndicesInParallel(results.inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!results.hasObjects(id) || results.application.layouts[index].flow.allocationClasses.isEmpty() || !results.inputs.build.isEnabled(FlowResultKind::ObjectUses, index))
            return false;
        return isSeenByFollowedCodeAlone(results, id) && !results.possibleMutationsOf(id).contains(ObjectMutation::BecomePrototype) && !uses.mayBeEnumerated(results.layoutsOfObjects.objectsWith(id));
    });
    for (uint32_t index : layoutsNeverEnumerated) {
        LayoutFlow& flow = results.application.layouts[index].flow;
        flow.keys = KeyEnumeration::NeverEnumerated;
        for (AllocationClassID allocationClass : flow.allocationClasses)
            results.tables.sites.allocationClasses[allocationClass.index()].keys = KeyEnumeration::NeverEnumerated;
    }
}

// The objects of several layouts may hold one field. It is an accessor where they all say so, in one form.
static void uniteAccessors(FlowAccessorField& known, FlowAccessorField&& other, bool isFirst)
{
    if (isFirst) {
        known = WTF::move(other);
        return;
    }
    if (known.form != other.form) {
        known = { };
        return;
    }
    for (FunctionID setter : other.setters)
        insertInUniquedSortedVector(known.setters, setter);
    if (known.getters.isEmpty() || other.getters.isEmpty()) {
        known.getters.clear();
        return;
    }
    for (FunctionID getter : other.getters)
        insertInUniquedSortedVector(known.getters, getter);
}

static ValueFlowFieldContents computeDeclaredFields(const FieldResults& results, const UsesOfObjects& uses, std::span<const AllocatingOperation> allocatingOperations)
{
    // What the objects of one layout say about a field that they hold.
    struct HeldField {
        FieldReference declaration;
        OptionSet<ObjectMutation> possibleMutations;
        bool mayBeObserved { false };
        bool mayBeCalled { false };
        bool mayBeAccessed { false };
        bool isStoredAfterCreation { false };
        bool isAskedForAccessor { false };
        bool isAskedForStoredValues { false };
        bool mayInheritName { true };
        FlowAccessorField accessor;
        ValueSetID stored { ValueSets::nothing };
        ValueFlowNumbers storedNumbers;
    };

    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    const ParallelLoops& loops = inputs.loops();
    BuildContext& build = inputs.build;
    FlowApplication& application = results.application;
    const StoresByLayout& stores = results.stores;
    uint32_t numberOfLayouts = program.layouts.size();

    ValueFlowFieldContents contents;
    contents.stored = Vector<Vector<ValueSetID>>(numberOfLayouts);
    contents.storedNumbers = Vector<Vector<ValueFlowNumbers>>(numberOfLayouts);
    contents.storedInAppendedFields = Vector<Vector<ValueSetID>>(numberOfLayouts);
    Vector<Vector<OptionSet<ObjectMutation>>> possibleMutations(numberOfLayouts);
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        size_t numberOfFields = program.layouts[index].fields.size();
        contents.stored[index] = Vector<ValueSetID>(FillWith { }, numberOfFields, ValueSets::nothing);
        contents.storedNumbers[index] = Vector<ValueFlowNumbers>(numberOfFields);
        possibleMutations[index] = Vector<OptionSet<ObjectMutation>>(numberOfFields);
    });
    Vector<BitVector> hasObjects(numberOfLayouts, [&](size_t index) {
        return BitVector(program.layouts[index].fields.size());
    });
    Vector<BitVector> isMissingAtCreation = hasObjects;
    Vector<BitVector> mayBeObserved = hasObjects;
    Vector<BitVector> mayBeCalled = hasObjects;
    Vector<BitVector> mayBeAccessed = hasObjects;
    Vector<BitVector> isStoredAfterCreation = hasObjects;
    Vector<BitVector> isAskedForAccessor = hasObjects;
    Vector<BitVector> mayInheritName = hasObjects;
    noteFieldsMissingAtCreation(results, allocatingOperations, isMissingAtCreation);
    ValueSetMakers makers(inputs);

    // What happens to the objects of a layout happens to every field that they hold.
    Vector<Vector<HeldField>> fieldsHeldByObjectsWith(numberOfLayouts);
    Vector<ObjectsOfLayoutInHeap::Chains> chainsOfWorker(loops.width());
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
        LayoutID id(index);
        if (!results.hasObjects(id))
            return;
        ValueSetMaker& maker = makers.of(worker);
        ObjectsOfLayoutInHeap inHeap(results, maker, chainsOfWorker[worker], id);
        OptionSet<ObjectMutation> possible = stores.getsCopiedProperties(id) ? allObjectMutations : results.possibleMutationsOf(id);
        bool isSeenByOthers = !isSeenByFollowedCodeAlone(results, id);
        bool isDeletedByStatementsAlone = !isSeenByOthers || (results.heap.reaches(inputs.universe().unknownObjectAlone(), inHeap.objects()) && !stores.hasObjectOfCodeWithoutSolve(id) && !possible.contains(ObjectMutation::ReachUnknownJavaScript));
        ValueSetID objects = inHeap.objects();
        // Exact for a layout without a proven base.
        fieldsHeldByObjectsWith[index].reserveInitialCapacity(program.layouts[index].fields.size());
        forEachFieldOfObjectsWith(program, application, id, [&](FieldReference declaration, const Field& field) {
            HeldField held { .declaration = declaration };
            OptionSet<ObjectMutation> possibleForField = possible;
            // A deletion takes the field that it names and leaves the others in their slots. Which keys are named is known of the objects that no unknown JavaScript gets hold of.
            if (field.key && isDeletedByStatementsAlone && !stores.getsCopiedProperties(id) && !inHeap.mayBeDeletedUnder(field.key))
                possibleForField.remove(ObjectMutation::DeleteField);
            // So does a redefinition.
            if (field.key && !isSeenByOthers && !stores.getsCopiedProperties(id) && !inHeap.mayBeRedefinedUnder(field.key))
                possibleForField.remove(ObjectMutation::RedefineProperty);
            held.possibleMutations = possibleForField;
            if (!field.key) {
                held.possibleMutations = allObjectMutations;
                held.mayBeObserved = true;
                fieldsHeldByObjectsWith[index].append(WTF::move(held));
                return;
            }
            held.mayBeObserved = isSeenByOthers || uses.mayBeObservedUnder(objects, field.key) || inHeap.isObservedByStoresOfHeirs(field.key);
            held.mayBeCalled = uses.mayBeCalledUnder(objects, field.key);
            held.mayBeAccessed = uses.mayBeAccessedUnder(objects, field.key);
            held.isStoredAfterCreation = stores.isStoredAfterCreation(id, field.key);
            // No lookup goes on past a property that every object has from its creation on.
            bool mayBeMissing = isMissingAtCreation[declaration.layout.index()].quickGet(declaration.field) || possibleForField.containsAny({ ObjectMutation::DeleteField, ObjectMutation::ReachUnknownJavaScript });
            held.mayInheritName = !mayBeMissing || !build.isEnabled(FlowResultKind::OwnNames, declaration.layout.index()) || !inHeap.inheritsNothingUnder(field.key);
            if (!hasNodeOfItsOwn(field) && build.isEnabled(FlowResultKind::AppendedFields, declaration.layout.index())) {
                held.isAskedForAccessor = true;
                held.accessor = stores.accessorDefinedUnder(maker, id, field.key);
            }
            if (!hasNodeOfItsOwn(field)) {
                held.isAskedForStoredValues = true;
                held.stored = stores.storedUnder(maker, id, field.key);
                held.storedNumbers = stores.numbersStoredUnder(id, field.key);
            }
            fieldsHeldByObjectsWith[index].append(WTF::move(held));
        });
    });
    chainsOfWorker.clear();

    for (Vector<HeldField>& heldFields : fieldsHeldByObjectsWith) {
        for (HeldField& held : heldFields) {
            uint32_t layout = held.declaration.layout.index();
            uint32_t field = held.declaration.field;
            auto note = [&](bool holds, Vector<BitVector>& fields) {
                if (holds)
                    fields[layout].quickSet(field);
            };
            hasObjects[layout].quickSet(field);
            note(held.mayBeObserved, mayBeObserved);
            note(held.mayBeCalled, mayBeCalled);
            note(held.mayBeAccessed, mayBeAccessed);
            note(held.isStoredAfterCreation, isStoredAfterCreation);
            note(held.mayInheritName, mayInheritName);
            possibleMutations[layout][field].add(held.possibleMutations);
            if (held.isAskedForAccessor)
                uniteAccessors(application.layouts[layout].fields[field].accessor, WTF::move(held.accessor), !isAskedForAccessor[layout].quickSet(field));
            if (held.isAskedForStoredValues) {
                ValueSetID& stored = contents.stored[layout][field];
                stored = makers.of(0).unite(stored, held.stored);
                contents.storedNumbers[layout][field].unite(held.storedNumbers);
            }
        }
        heldFields.clear();
    }

    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!hasObjects[index].quickGet(field) || layout.fields[field].flags.contains(FieldFlags::IsInherited))
                continue;
            if (hasNodeOfItsOwn(layout.fields[field])) {
                contents.stored[index][field] = inputs.solver().valueOf(inputs.bodies().nodes().field(LayoutID(index), field));
                contents.storedNumbers[index][field] = ValueFlowNumbers::any();
            }

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
            if (!mayInheritName[index].quickGet(field))
                flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;
            // Which allocations and constructors fill a field is read from them.
            if (!isMissingAtCreation[index].quickGet(field) && flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::Initialization, index))
                flow.location.initialization = InitializationState::SetAtCreation;
            // Nothing puts a value there but the operation that allocates the object. A field that no allocation lists either is never stored, which the results about types say.
            bool isChangedAfterCreation = hasNodeOfItsOwn(layout.fields[field]) || isStoredAfterCreation[index].quickGet(field) || possible.containsAny({ ObjectMutation::DeleteField, ObjectMutation::RedefineProperty });
            if (!isChangedAfterCreation && flow.location.writers == WriterState::AllKnown) {
                flow.location.stores = StoreCount::AtMostOnce;
                flow.location.isUnchangedDuringActivation = true;
            }

            bool isNeverObserved = !hasNodeOfItsOwn(layout.fields[field]) && !mayBeObserved[index].quickGet(field);
            // A field that nobody reads or stores gets no slot. An access without a stamp names every layout that the type of its receiver admits, also those that never arrive there.
            if (isNeverObserved && flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::ObjectUses, index) && build.isEnabledEverywhere(FlowResultKind::ReceiverSets)) {
                bool isCalled = mayBeCalled[index].quickGet(field);
                bool isAccessed = mayBeAccessed[index].quickGet(field);
                flow.location.reads = isCalled && isAccessed ? ReadState::Unknown : isCalled ? ReadState::OnlyCalled : isAccessed ? ReadState::OnlyAccessed : ReadState::NeverRead;
            }
        }
    });
    return contents;
}

static bool canAppendFieldsTo(const FieldResults& results, LayoutID id)
{
    switch (results.program().at(id).kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
        break;
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    // A derived layout has the fields of the layout it was derived from, and that one takes the names.
    if (results.program().at(id).isDerived() || !results.hasObjects(id) || results.application.layouts[id.index()].flow.allocationClasses.isEmpty())
        return false;
    return results.inputs.build.isEnabled(FlowResultKind::AppendedFields, id.index());
}

static bool canNameField(const Program& program, const BitVector& symbolsCreatedOnce, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        return !program.isIndexLike(key);
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::CreatedSymbol:
        // The key of a call that may run again stands for several symbols, and a field has one key.
        return symbolsCreatedOnce.get(key.index());
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// How many operations of reached functions name each key. Counted when the first layout has more names than it may keep.
class AccessCounts {
public:
    explicit AccessCounts(const ValueFlowResultInputs& inputs)
        : m_inputs(inputs)
    {
    }

    uint32_t of(PropertyKeyID key)
    {
        if (!m_isCounted)
            count();
        return m_counts[key.index()];
    }

private:
    void count()
    {
        m_isCounted = true;
        size_t numberOfKeys = m_inputs.program.types.numberOfPropertyKeys();
        Vector<Vector<uint32_t>> countsOfWorker(m_inputs.loops().width());
        forEachIndexInParallel(m_inputs.loops(), m_inputs.program.functions.size(), [&](unsigned index, unsigned worker) {
            if (!m_inputs.isReached(FunctionID(index)))
                return;
            Vector<uint32_t>& counts = countsOfWorker[worker];
            if (counts.isEmpty())
                counts.fill(0, numberOfKeys);
            for (const Value& value : m_inputs.program.functions[index].values) {
                if (Value::opcodesWithKey.contains(value.opcode) && value.key())
                    ++counts[value.key().index()];
            }
        });
        m_counts.fill(0, numberOfKeys);
        for (const Vector<uint32_t>& counts : countsOfWorker) {
            for (size_t key = 0; key < counts.size(); ++key)
                m_counts[key] += counts[key];
        }
    }

    const ValueFlowResultInputs& m_inputs;
    Vector<uint32_t> m_counts;
    bool m_isCounted { false };
};

// The layouts by the proven chains of bases.
struct ProvenHierarchy {
    ProvenHierarchy(const Program& program, const FlowApplication& application)
        : layoutsDirectlyBelow(program.layouts.size())
    {
        Vector<uint32_t> depth(FillWith { }, program.layouts.size(), 0u);
        uint32_t greatestDepth = 0;
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            if (LayoutID base = provenBaseOf(application, LayoutID(index)))
                layoutsDirectlyBelow[base.index()].append(LayoutID(index));
            LayoutID current = provenBaseOf(application, LayoutID(index));
            for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                ++depth[index];
                current = provenBaseOf(application, current);
            }
            greatestDepth = std::max(greatestDepth, depth[index]);
        }
        // By depth, and at one depth in the order of the layouts.
        Vector<uint32_t> nextAtDepth(FillWith { }, greatestDepth + 2, 0u);
        for (uint32_t depthOfLayout : depth)
            ++nextAtDepth[depthOfLayout + 1];
        for (uint32_t atDepth = 0; atDepth <= greatestDepth; ++atDepth)
            nextAtDepth[atDepth + 1] += nextAtDepth[atDepth];
        fromBasesDown.grow(program.layouts.size());
        for (uint32_t index = 0; index < program.layouts.size(); ++index)
            fromBasesDown[nextAtDepth[depth[index]]++] = LayoutID(index);
    }

    void forEachLayoutBelow(LayoutID layout, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        Vector<LayoutID, 8> worklist { layout };
        while (!worklist.isEmpty()) {
            LayoutID current = worklist.takeLast();
            functor(current);
            worklist.appendVector(layoutsDirectlyBelow[current.index()]);
        }
    }

    Vector<Vector<LayoutID>> layoutsDirectlyBelow;
    Vector<LayoutID> fromBasesDown;
};

} // anonymous namespace

// One object of the universe stands for the instances of a class and of the classes below it, so what a class below stores into its own field seems to be stored into the instances of this class too.
static Vector<Vector<PropertyKeyID>> collectKeysDeclaredBelow(const Program& program, const BitVector& isCandidate)
{
    Vector<Vector<PropertyKeyID>> keysDeclaredBelow(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Vector<PropertyKeyID, 8> ownKeys;
        for (const Field& field : program.layouts[index].fields) {
            if (field.key && !field.flags.contains(FieldFlags::IsInherited) && isCandidate.quickGet(field.key.index()))
                ownKeys.append(field.key);
        }
        if (ownKeys.isEmpty())
            continue;
        LayoutID above = layoutExpectedAbove(program, LayoutID(index));
        for (size_t steps = 0; above && steps < program.layouts.size(); ++steps) {
            keysDeclaredBelow[above.index()].appendVector(ownKeys);
            above = layoutExpectedAbove(program, above);
        }
    }
    for (Vector<PropertyKeyID>& keys : keysDeclaredBelow) {
        AOT::sort(keys);
        removeRepeatedElements(keys);
    }
    return keysDeclaredBelow;
}

using LayoutsByKey = HashMap<uint32_t, Vector<LayoutID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

// The layouts that have a field under each of the names, ascending. A family of a large program has thousands of members, and few names are asked about.
static LayoutsByKey collectLayoutsWithFields(const Program& program, const BitVector& isCandidate)
{
    LayoutsByKey layoutsWithField;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        for (const Field& field : program.layouts[index].fields) {
            if (!field.key || !isCandidate.quickGet(field.key.index()))
                continue;
            Vector<LayoutID>& layouts = layoutsWithField.add(field.key.index(), Vector<LayoutID>()).iterator->value;
            if (layouts.isEmpty() || layouts.last() != LayoutID(index))
                layouts.append(LayoutID(index));
        }
    }
    return layoutsWithField;
}

// A store that finds an accessor above the object runs the setter, or does nothing where there is none: it fills no slot.
static bool findsAccessorAbove(const FieldResults& results, ValueSetMaker& maker, LayoutID id, PropertyKeyID key)
{
    const ValueSets& sets = results.inputs.sets();
    // Few names have an accessor anywhere in the program. The others ask for no chain.
    Vector<ValueSetID, 4> receiversOfDefinitions;
    for (ValueFlowName name : { ValueFlowName::getter(key), ValueFlowName::setter(key) }) {
        results.heap.forEachNodeWrittenUnder(name, [&](ValueSetID writtenThrough, ValueSetID value) {
            if (!sets.isEmpty(value))
                receiversOfDefinitions.append(writtenThrough);
        });
    }
    if (receiversOfDefinitions.isEmpty())
        return false;

    ValueSetID objects = results.inputs.universe().objectsWith(id);
    ValueSetID above = maker.intersect(results.chainBuiltByProgram(objects), ValueSets::complement(objects));
    return std::ranges::any_of(receiversOfDefinitions, [&](ValueSetID writtenThrough) {
        return !sets.isEmpty(maker.intersect(writtenThrough, above));
    });
}

static void removeKeysWithoutRoom(const FieldResults& results, ValueSetMaker& maker, const BitVector& symbolsCreatedOnce, std::span<const PropertyKeyID> keysDeclaredBelow, const LayoutsByKey& layoutsWithField, std::span<const LayoutID> declaredViews, std::span<const PropertyKeyID> keysStoredByMembersAbove, LayoutID id, Vector<PropertyKeyID>& keys)
{
    const Program& program = results.program();
    // Where the program declares types for the objects, a name that none of them has gets no room: neither a layout of their families nor a view through which the program may see the objects.
    Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, id);
    keys.removeAllMatching([&](PropertyKeyID key) {
        auto declarers = layoutsWithField.find(key.index());
        // The type of `Symbol.for("name")` is `symbol`, so no type has a member under it.
        PropertyKeyKind kind = program.at(key).kind;
        bool canBeDeclared = kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol;
        bool isOutsideFamilies = canBeDeclared && !families.isEmpty() && (declarers == layoutsWithField.end() || (!haveCommonMember(declaredViews, declarers->value.span()) && std::ranges::none_of(families, [&](LayoutSetID family) {
            return haveCommonMember(program.at(family).members.span(), declarers->value.span());
        })));
        if (isOutsideFamilies || std::ranges::binary_search(keysDeclaredBelow, key))
            return true;
        if (!canNameField(program, symbolsCreatedOnce, key) || declaresFieldOnChain(program, results.application, id, key))
            return true;
        if (results.stores.accessorDefinedUnder(maker, id, key).isAccessor())
            return false;
        // Where only code that nobody has followed constructs with the function, its `this` is a placeholder, and what it stores there is listed under no object of the layout.
        bool isStored = results.stores.isStoredByConstructor(id, key) || std::ranges::binary_search(keysStoredByMembersAbove, key) || !results.inputs.sets().isEmpty(std::as_const(results.stores).storedUnder(maker, id, key));
        return results.stores.mayGetAccessorUnder(id, key) || !isStored || findsAccessorAbove(results, maker, id, key);
    });
}

// `this._body = value` in a member of the object that Object.create is handed: whoever calls the member calls it on what is made below that object.
static Vector<PropertyKeyID> keysStoredIntoThisByMembersAbove(const FieldResults& results, const BitVector& layoutsHandedToObjectCreate, LayoutID created)
{
    const Program& program = results.program();
    Vector<PropertyKeyID> keys;
    if (!program.at(created).has(LayoutFlags::IsOfCreatedObject) || layoutsHandedToObjectCreate.quickGet(created.index()) || !results.possibleMutationsOf(created).contains(ObjectMutation::ReachUnknownJavaScript))
        return keys;
    LayoutID current = created;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const ClassFlow& classFlow = results.application.layouts[current.index()].classFlow;
        for (const ClassMember& member : classFlow.members) {
            const Function& function = program.at(member.function);
            std::optional<unsigned> thisParameter = function.hasBody() ? function.thisParameterIndex() : std::nullopt;
            if (!thisParameter)
                continue;
            ValueID thisValue = function.at(entryBlock).parameters[*thisParameter];
            for (const Value& value : function.values) {
                if (value.opcode == Opcode::PropSet && value.key() && definitionThroughSameValues(function, value.objectOperand()) == thisValue)
                    keys.append(value.key());
            }
        }
        current = classFlow.prototypeBase;
    }
    AOT::sort(keys);
    removeRepeatedElements(keys);
    keys.removeAllMatching([&](PropertyKeyID key) {
        bool isNameOfAccessor = false;
        for (ValueFlowName name : { ValueFlowName::getter(key), ValueFlowName::setter(key) }) {
            results.heap.forEachNodeWrittenUnder(name, [&](ValueSetID, ValueSetID value) {
                isNameOfAccessor |= !results.inputs.sets().isEmpty(value);
            });
        }
        return isNameOfAccessor;
    });
    return keys;
}

// The names that the program mentions least go to the extra properties.
static void keepMostAccessedKeys(const FieldResults& results, AccessCounts& counts, LimitID limit, LayoutID layout, uint64_t numberOfOtherFields, Vector<PropertyKeyID>& keys)
{
    BuildContext& build = results.inputs.build;
    if (keys.isEmpty() || !build.exceeds(limit, numberOfOtherFields + keys.size(), Subject { .layout = layout }))
        return;
    uint64_t allowed = build.limit(limit);
    size_t numberOfKeysKept = allowed > numberOfOtherFields ? allowed - numberOfOtherFields : 0;
    auto rankOf = [&](PropertyKeyID key) {
        return std::pair { results.stores.isStoredByConstructor(layout, key), counts.of(key) };
    };
    std::ranges::nth_element(keys, keys.begin() + numberOfKeysKept, [&](PropertyKeyID a, PropertyKeyID b) {
        return std::pair { rankOf(b), a } < std::pair { rankOf(a), b };
    });
    keys.shrink(numberOfKeysKept);
    AOT::sort(keys);
}

// The names of the accessors that the objects of the layout get.
static Vector<PropertyKeyID> keysOfAccessorsIn(const FieldResults& results, ValueSetMaker& maker, LayoutID id)
{
    Vector<PropertyKeyID> keys = results.stores.keysOfAccessorsDefinedIn(maker, id);
    if (!results.stores.getsAccessorsUnderKeysNobodyKnows(id))
        return keys;
    // A name that the program puts above the objects is more likely read from there.
    const Program& program = results.program();
    const ValueFlowUniverse& universe = results.inputs.universe();
    const ValueSets& sets = results.inputs.sets();
    const ValueFlowHeapView& heap = results.heap;
    ValueSetID objects = results.layoutsOfObjects.objectsWith(id);
    ValueSetID above = maker.intersect(results.chainBuiltByProgram(objects), ValueSets::complement(objects));
    auto liesAbove = [&](PropertyKeyID key) {
        if (!sets.isEmpty(heap.writtenUnder(maker, above, ValueFlowName::property(key))) || !sets.isEmpty(heap.writtenUnder(maker, above, ValueFlowName::getter(key))))
            return true;
        BuiltinName name = program.builtinNameOf(key);
        if (!name)
            return false;
        bool isMemberOfBuiltin = false;
        sets.forEachMember(above, [&](ValueFlowObjectID holder) {
            if (BuiltinID builtin = universe.builtinOf(holder))
                isMemberOfBuiltin |= !!program.builtins().findOwnMember(builtin, name);
        });
        return isMemberOfBuiltin;
    };
    Vector<PropertyKeyID> read(results.stores.keysReadFrom(id));
    read.removeAllMatching([&](PropertyKeyID key) {
        return !results.stores.accessorDefinedUnder(maker, id, key).isAccessor() || liesAbove(key);
    });
    uniteSortedKeys(keys, read);
    return keys;
}

static Vector<Vector<PropertyKeyID>> chooseAppendedKeys(const FieldResults& results, const ProvenHierarchy& hierarchy)
{
    const Program& program = results.program();
    uint32_t numberOfLayouts = program.layouts.size();
    BitVector noSymbols;
    const BitVector& symbolsCreatedOnce = results.inputs.symbolsCreatedOnce ? *results.inputs.symbolsCreatedOnce : noSymbols;

    const ParallelLoops& loops = results.inputs.loops();
    ValueSetMakers makers(results.inputs);
    Vector<Vector<PropertyKeyID>> keysByLayout(numberOfLayouts);
    Vector<Vector<PropertyKeyID>> keysStoredByMembersAbove(numberOfLayouts);
    Vector<uint64_t> fieldCountsAsAnalyzed(FillWith { }, numberOfLayouts, static_cast<uint64_t>(0));
    BitVector layoutsHandedToObjectCreate(numberOfLayouts);
    for (uint32_t index = program.numberOfLayoutsAsRead(); index < numberOfLayouts; ++index) {
        if (LayoutID base = program.layouts[index].has(LayoutFlags::IsOfCreatedObject) ? results.application.layouts[index].classFlow.prototypeBase : LayoutID())
            layoutsHandedToObjectCreate.quickSet(base.index());
    }
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
        LayoutID current(index);
        for (size_t steps = 0; current && steps < numberOfLayouts; ++steps) {
            fieldCountsAsAnalyzed[index] += program.at(current).fields.size();
            current = provenBaseOf(results.application, current);
        }
        if (!canAppendFieldsTo(results, LayoutID(index)))
            return;
        // What only a store through a placeholder, the code of a built-in or a store under a key that may be one of several puts there goes to the extra properties.
        Vector<PropertyKeyID> accessors = keysOfAccessorsIn(results, makers.of(worker), LayoutID(index));
        // What Object.create makes below an object nobody knows stays an ordinary object unless it is made to hold accessors.
        if (accessors.isEmpty() && program.layouts[index].has(LayoutFlags::IsOfCreatedObject) && results.application.layouts[index].classFlow.evaluations != ExecutionCount::AtMostOnce)
            return;
        keysByLayout[index] = results.stores.keysNamedByStoresIntoKnownObjects(LayoutID(index));
        uniteSortedKeys(keysByLayout[index], accessors);
        keysStoredByMembersAbove[index] = keysStoredIntoThisByMembersAbove(results, layoutsHandedToObjectCreate, LayoutID(index));
        uniteSortedKeys(keysByLayout[index], keysStoredByMembersAbove[index]);
    });
    BitVector isCandidate(program.types.numberOfPropertyKeys());
    for (const Vector<PropertyKeyID>& keys : keysByLayout) {
        for (PropertyKeyID key : keys)
            isCandidate.quickSet(key.index());
    }
    Vector<Vector<PropertyKeyID>> keysDeclaredBelow;
    LayoutsByKey layoutsWithField;
    Vector<Vector<LayoutID>> declaredViews;
    runTasksInParallel(loops, {
        [&] {
            keysDeclaredBelow = collectKeysDeclaredBelow(program, isCandidate);
        },
        [&] {
            layoutsWithField = collectLayoutsWithFields(program, isCandidate);
        },
        [&] {
            BitVector declaredLayoutsWithKeys(numberOfLayouts);
            for (uint32_t index = 0; index < numberOfLayouts; ++index) {
                if (!keysByLayout[index].isEmpty())
                    declaredLayoutsWithKeys.quickSet(declaredLayoutOf(program, LayoutID(index)).index());
            }
            declaredViews = declaredViewsOf(program, declaredLayoutsWithKeys);
        },
    });
    Vector<uint32_t> layoutsWithKeys;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!keysByLayout[index].isEmpty())
            layoutsWithKeys.append(index);
    }
    AOT::stableSort(layoutsWithKeys, std::greater { }, [&](uint32_t index) {
        return keysByLayout[index].size();
    });
    forEachIndexInParallel(loops, layoutsWithKeys.size(), [&](unsigned position, unsigned worker) {
        uint32_t index = layoutsWithKeys[position];
        removeKeysWithoutRoom(results, makers.of(worker), symbolsCreatedOnce, keysDeclaredBelow[index].span(), layoutsWithField, declaredViews[declaredLayoutOf(program, LayoutID(index)).index()].span(), keysStoredByMembersAbove[index].span(), LayoutID(index), keysByLayout[index]);
    });

    AccessCounts counts(results.inputs);
    for (LayoutID layout : hierarchy.fromBasesDown) {
        Vector<PropertyKeyID>& keys = keysByLayout[layout.index()];
        if (keys.isEmpty())
            continue;
        uint64_t numberOfInheritedKeys = 0;
        LayoutID ancestor = provenBaseOf(results.application, layout);
        for (size_t steps = 0; ancestor && steps < numberOfLayouts; ++steps) {
            const Vector<PropertyKeyID>& inheritedKeys = keysByLayout[ancestor.index()];
            numberOfInheritedKeys += inheritedKeys.size();
            keys.removeAllMatching([&](PropertyKeyID key) {
                return std::ranges::binary_search(inheritedKeys, key);
            });
            ancestor = provenBaseOf(results.application, ancestor);
        }
        // A layout below inherits the appended fields on top of the fields it has.
        uint64_t largestFieldCountBelow = 0;
        hierarchy.forEachLayoutBelow(layout, [&](LayoutID below) {
            largestFieldCountBelow = std::max(largestFieldCountBelow, fieldCountsAsAnalyzed[below.index()]);
        });
        // The slot of an accessor is a narrow reference, and the objects that hold many of them are few.
        Vector<PropertyKeyID> accessors;
        keys.removeAllMatching([&](PropertyKeyID key) {
            bool isAccessor = results.stores.accessorDefinedUnder(layout, key).isAccessor();
            if (isAccessor)
                accessors.append(key);
            return isAccessor;
        });
        keepMostAccessedKeys(results, counts, LimitID::MaxAppendedFieldsPerLayout, layout, 0, keys);
        uniteSortedKeys(keys, accessors);
        keepMostAccessedKeys(results, counts, LimitID::FieldsInKeyOrder, layout, largestFieldCountBelow + numberOfInheritedKeys, keys);
    }
    return keysByLayout;
}

// A name that followed code stores into the objects of a layout and that the layout does not declare becomes a field that no allocation fills.
static void appendFields(const FieldResults& results, const ProvenHierarchy& hierarchy, std::span<const Vector<PropertyKeyID>> keysByLayout, ValueFlowFieldContents& contents)
{
    BuildContext& build = results.inputs.build;
    const StoresByLayout& stores = results.stores;
    ValueSetMakers makers(results.inputs);
    Vector<ObjectsOfLayoutInHeap::Chains> chainsOfWorker(results.inputs.loops().width());
    forEachIndexInParallel(results.inputs.loops(), keysByLayout.size(), [&](unsigned index, unsigned worker) {
        if (keysByLayout[index].isEmpty())
            return;
        ValueSetMaker& maker = makers.of(worker);
        FlowLayoutUpdate& update = results.application.layouts[index];
        for (PropertyKeyID key : keysByLayout[index]) {
            OptionSet<ObjectMutation> possible;
            ValueSetID stored = ValueSets::nothing;
            bool isStoredBeforeObjectsLeak = true;
            bool inheritsNothing = build.isEnabled(FlowResultKind::OwnNames, index);
            hierarchy.forEachLayoutBelow(LayoutID(index), [&](LayoutID below) {
                if (!results.hasObjects(below))
                    return;
                inheritsNothing = inheritsNothing && ObjectsOfLayoutInHeap(results, maker, chainsOfWorker[worker], below).inheritsNothingUnder(key);
                possible.add(stores.getsCopiedProperties(below) ? allObjectMutations : results.possibleMutationsOf(below));
                stored = maker.unite(stored, stores.storedUnder(maker, below, key));
                isStoredBeforeObjectsLeak &= results.constructions.isFollowed(below) && results.constructions.isStoredBeforeObjectLeaks(below, key, { });
            });

            FlowAppendedField field;
            field.key = key;
            bool isFirst = true;
            hierarchy.forEachLayoutBelow(LayoutID(index), [&](LayoutID below) {
                if (results.hasObjects(below))
                    uniteAccessors(field.accessor, stores.accessorDefinedUnder(maker, below, key), std::exchange(isFirst, false));
            });
            if (!possible.contains(ObjectMutation::ReachUnknownJavaScript) && !results.inputs.sets().isEmpty(stored) && build.isEnabled(FlowResultKind::StoreCounts, index))
                field.flow.location.writers = WriterState::AllKnown;
            if (isStoredBeforeObjectsLeak && field.flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::Initialization, index))
                field.flow.location.initialization = InitializationState::SetAtCreation;
            if (build.isEnabled(FlowResultKind::HolderMutations, index)) {
                if (!possible.contains(ObjectMutation::DeleteField))
                    field.flow.stability.add(FieldStability::IsNeverDeleted);
                if (!possible.contains(ObjectMutation::RedefineProperty))
                    field.flow.stability.add(FieldStability::IsNeverRedefined);
            }
            if (inheritsNothing)
                field.flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;
            update.appendedFields.append(WTF::move(field));
            contents.storedInAppendedFields[index].append(stored);
        }
    });
    uint64_t numberOfAppendedFields = 0;
    for (const Vector<PropertyKeyID>& keys : keysByLayout)
        numberOfAppendedFields += keys.size();
    build.statistics().count(StatisticID::FlowAppendedFields, numberOfAppendedFields);
}

static bool isFieldOrAppendedField(const FieldResults& results, std::span<const Vector<PropertyKeyID>> appendedKeys, LayoutID id, PropertyKeyID key)
{
    const Program& program = results.program();
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (program.at(current).findField(key) || std::ranges::binary_search(appendedKeys[current.index()], key))
            return true;
        current = provenBaseOf(results.application, current);
    }
    return false;
}

// What native code constructs it may fill with properties that no node takes: the species of an array gets a length.
static BitVector findLayoutsConstructedByNativeCode(const FieldResults& results)
{
    const ValueFlowSolver& solver = results.inputs.solver();
    BitVector layouts(results.program().layouts.size());
    Vector<ValueSetID> resultsOfConstructions = collectFromCallGroups<ValueSetID>(results.inputs, results.heap, [&](const ValueFlowNodeKey& key, ValueFlowNodeID first, Vector<ValueSetID>& sets) {
        if (key.callKind() == ValueFlowCallKind::ConstructByNativeCode)
            sets.append(solver.valueOf(callGroupAt(first).result));
    });
    if (resultsOfConstructions.isEmpty())
        return layouts;

    AOT::sort(resultsOfConstructions);
    removeRepeatedElements(resultsOfConstructions);
    ValueSetMaker& maker = results.heap.maker();
    ValueSetID made = maker.intersect(results.heap.expand(maker.uniteAll(resultsOfConstructions.span())), results.inputs.universe().knownObjects());
    if (ValueSets::isComplement(made)) {
        for (size_t layout = 0; layout < results.program().layouts.size(); ++layout)
            layouts.quickSet(layout);
        return layouts;
    }
    results.layoutsOfObjects.forEachLayoutOfMembers(made, [&](LayoutID layout) {
        layouts.quickSet(layout.index());
    });
    return layouts;
}

// The rules mark an object as soon as something is stored into it under a name that the layout of its site does not declare.
static void computeAddedKeys(const FieldResults& results, std::span<const Vector<PropertyKeyID>> appendedKeys)
{
    const Program& program = results.program();
    BuildContext& build = results.inputs.build;
    BitVector isConstructedByNativeCode = findLayoutsConstructedByNativeCode(results);
    const Vector<PropertyKeyID>& sharedKeys = results.stores.keysStoredThroughPlaceholders();
    results.tables.wholeProgram.keysStoredThroughUnknownReceivers = sharedKeys;
    results.tables.wholeProgram.keysDefinedOnObjectsWithoutKnownLayout = results.stores.keysDefinedOnObjectsWithoutKnownLayout();
    const Vector<PropertyKeyID>& keysOfSeveralSymbols = results.tables.wholeProgram.keysOfSeveralSymbols;
    // A layout has its allocation classes to itself.
    forEachIndexInParallel(results.inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        LayoutFlow& flow = results.application.layouts[index].flow;
        LayoutKind kind = program.layouts[index].kind;
        if (!results.hasObjects(id) || flow.allocationClasses.isEmpty() || (kind != LayoutKind::Record && kind != LayoutKind::Instance))
            return;

        bool listsEveryName = !results.stores.getsNameNobodyLists(id) && !isConstructedByNativeCode.quickGet(index)
            && !results.possibleMutationsOf(id).contains(ObjectMutation::ReachUnknownJavaScript);
        Vector<PropertyKeyID> addedKeys;
        bool mayGainSharedKey = false;
        if (listsEveryName) {
            auto isField = [&](PropertyKeyID key) {
                return !std::ranges::binary_search(keysOfSeveralSymbols, key) && isFieldOrAppendedField(results, appendedKeys, id, key);
            };
            addedKeys = results.stores.keysStoredIntoKnownObjects(id);
            addedKeys.removeAllMatching(isField);
            mayGainSharedKey = results.stores.isReachedThroughPlaceholders(id) && !std::ranges::all_of(sharedKeys, isField);
        }
        bool mayAddProperty = !listsEveryName || !addedKeys.isEmpty() || mayGainSharedKey;

        auto apply = [&](OptionSet<ObjectMutation>& impossibleMutations, Vector<PropertyKeyID>& keys, bool& keysAreComplete, bool& isReachedThroughUnknownReceivers) {
            impossibleMutations.set(ObjectMutation::AddProperty, !mayAddProperty && build.isEnabled(FlowResultKind::HolderMutations, index));
            if (!build.isEnabled(FlowResultKind::OwnNames, index))
                return;
            keys = addedKeys;
            keysAreComplete = listsEveryName;
            isReachedThroughUnknownReceivers = mayGainSharedKey;
        };
        apply(flow.impossibleMutations, flow.addedKeys, flow.addedKeysAreComplete, flow.isReachedThroughUnknownReceivers);
        for (AllocationClassID allocationClass : flow.allocationClasses) {
            AllocationClass& record = results.tables.sites.allocationClasses[allocationClass.index()];
            apply(record.impossibleMutations, record.addedKeys, record.addedKeysAreComplete, record.isReachedThroughUnknownReceivers);
        }
    });
}

// The layouts with an object that neither an allocation with the layout nor constant data makes. No order lists the keys of such an object.
static BitVector findLayoutsOfObjectsWithoutListedOrder(const FieldResults& results)
{
    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<LayoutID> layoutsOfSites = collectInParallel<LayoutID>(inputs.loops(), universe.numberOfObjects(), [&](unsigned index, Vector<LayoutID>& found) {
        ValueFlowObjectID object(index);
        if (universe.kindOf(object) != ValueFlowObjectKind::Site || !universe.layoutOf(object) || isSiteOfTypedResultThatNothingAllocates(inputs, object))
            return;
        const ValueFlowSite& site = *universe.siteOf(object);
        bool isAllocation = site.origin == ValueFlowSiteOrigin::Operation && !site.ordinal && program.at(universe.functionOf(object)).at(site.value).opcode == Opcode::NewObject;
        if (!isAllocation && site.origin != ValueFlowSiteOrigin::DataNode)
            found.append(site.layout);
    });
    BitVector layouts(program.layouts.size());
    for (LayoutID layout : layoutsOfSites) {
        layouts.quickSet(layout.index());
        layouts.quickSet(declaredLayoutOf(program, layout).index());
    }
    return layouts;
}

// Nothing creates a field behind the construction of an object: no name is added, and no field that the construction may leave out is stored later.
static FieldCreation ownFieldCreationOf(const FieldResults& results, const BitVector& layoutsOfObjectsWithoutListedOrder, LayoutID id)
{
    const Program& program = results.program();
    const Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
        if (layoutsOfObjectsWithoutListedOrder.quickGet(id.index()))
            return FieldCreation::Unknown;
        break;
    case LayoutKind::Instance:
        // The construction speaks for the constructors and their helpers alone, and a method may create a field that they left out.
        if (!results.constructions.createsFieldsOnlyInListedOrders(id))
            return FieldCreation::Unknown;
        break;
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return FieldCreation::Unknown;
    }
    if (results.possibleMutationsOf(id).contains(ObjectMutation::AddProperty) || !results.application.layouts[id.index()].appendedFields.isEmpty())
        return FieldCreation::Unknown;

    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        const Field& field = layout.fields[index];
        if (field.flags.contains(FieldFlags::IsInherited))
            continue;
        bool isFilledByConstruction = results.application.layouts[id.index()].fields[index].flow.location.initialization == InitializationState::SetAtCreation;
        if (!isFilledByConstruction && (!field.key || hasNodeOfItsOwn(field) || results.stores.isStoredAfterCreation(id, field.key)))
            return FieldCreation::Unknown;
    }
    return FieldCreation::OnlyAtConstruction;
}

static void computeFieldCreations(const FieldResults& results)
{
    const Program& program = results.program();
    const ParallelLoops& loops = results.inputs.loops();
    BitVector layoutsOfObjectsWithoutListedOrder = findLayoutsOfObjectsWithoutListedOrder(results);
    Vector<FieldCreation> ownFieldCreation(FillWith { }, program.layouts.size(), FieldCreation::Unknown);
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (results.hasObjects(id) && results.inputs.build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
            ownFieldCreation[index] = ownFieldCreationOf(results, layoutsOfObjectsWithoutListedOrder, id);
    });
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        bool isCreatedAtConstruction = true;
        LayoutID current(index);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            const LayoutFlow& flow = results.application.layouts[current.index()].flow;
            bool hasProvenLink = flow.baseState == BaseState::Layout || flow.baseState == BaseState::None;
            // A base that only the classes below it construct creates its fields in their objects.
            bool createsOwnFieldsAtConstruction = ownFieldCreation[current.index()] == FieldCreation::OnlyAtConstruction
                || (current != LayoutID(index) && !results.hasObjects(current) && results.constructions.createsFieldsOnlyInListedOrders(current));
            isCreatedAtConstruction &= hasProvenLink && createsOwnFieldsAtConstruction;
            current = provenBaseOf(results.application, current);
        }
        if (isCreatedAtConstruction)
            results.application.layouts[index].flow.fieldCreation = FieldCreation::OnlyAtConstruction;
    });
}

// The orders in which the allocations and the constant data of a layout without a constructor list its fields: those that a finished object may have come first, and of them the most frequent.
static void computeInitialFieldOrders(const FieldResultsWithoutStores& results, std::span<const AllocatingOperation> operations)
{
    struct CountedOrder {
        Vector<uint32_t> fields;
        uint32_t numberOfSites { 0 };
        bool isOrderOfFinishedObjects { false };
    };

    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    Vector<Vector<CountedOrder>> ordersOfLayout(program.layouts.size());
    auto listsOrdersOf = [&](LayoutID declared) {
        return hasObjectsThatCodeAlters(program.at(declared).kind) && !program.at(declared).constructor;
    };
    auto count = [&](LayoutID declared, std::span<const uint32_t> fields, bool getsMoreFields) {
        Vector<CountedOrder>& orders = ordersOfLayout[declared.index()];
        auto found = std::ranges::find_if(orders, [&](const CountedOrder& order) {
            return std::ranges::equal(order.fields, fields);
        });
        if (found == orders.end()) {
            orders.append({ Vector<uint32_t>(fields), 1, !getsMoreFields });
            return;
        }
        ++found->numberOfSites;
        found->isOrderOfFinishedObjects |= !getsMoreFields;
    };
    for (const AllocatingOperation& operation : operations) {
        const Value& value = program.at(operation.function).at(operation.value);
        // An allocation lists fields of the declared layout, whichever layout of the family it names.
        if (value.opcode == Opcode::NewObject) {
            LayoutID declared = declaredLayoutOf(program, value.layout());
            if (listsOrdersOf(declared))
                count(declared, value.fields(), operation.getsMoreFields);
            continue;
        }
        forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            LayoutID declared = declaredLayoutOf(program, object->layout);
            if (listsOrdersOf(declared))
                count(declared, fieldsFilledBy(program, program.at(declared), *object).span(), false);
        });
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Vector<CountedOrder>& orders = ordersOfLayout[index];
        if (orders.isEmpty() || !inputs.build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
            continue;
        AOT::sort(orders, [](const CountedOrder& a, const CountedOrder& b) {
            if (a.isOrderOfFinishedObjects != b.isOrderOfFinishedObjects)
                return a.isOrderOfFinishedObjects;
            if (a.numberOfSites != b.numberOfSites)
                return a.numberOfSites > b.numberOfSites;
            return std::ranges::lexicographical_compare(a.fields, b.fields);
        });
        for (CountedOrder& order : orders)
            results.application.layouts[index].flow.constructorOrders.append(FieldOrder { WTF::move(order.fields) });
    }
}

static void computeFieldsStoredOnceByConstructors(const FieldResults& results)
{
    // The one operation that stores by name, as far as somebody has looked.
    struct StoreSeen {
        FunctionID function;
        ValueID store;
        bool isStoredOtherwise { false };

        void note(FunctionID otherFunction, ValueID otherStore)
        {
            isStoredOtherwise |= store && (function != otherFunction || store != otherStore);
            function = otherFunction;
            store = otherStore;
        }
    };

    struct Candidate : StoreSeen {
        LayoutID layout;
        uint32_t field { 0 };
    };

    const Program& program = results.program();
    const ParallelLoops& loops = results.inputs.loops();
    FlowApplication& application = results.application;
    const StoresByLayout& stores = results.stores;

    // The objects of a layout below hold the field too, and another constructor runs for them.
    BitVector hasObjectsBelow(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!results.hasObjects(LayoutID(index)))
            continue;
        for (bool isProven : { true, false }) {
            auto above = [&](LayoutID layout) {
                return isProven ? provenBaseOf(application, layout) : layoutExpectedAbove(program, layout);
            };
            LayoutID current = above(LayoutID(index));
            for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                hasObjectsBelow.quickSet(current.index());
                current = above(current);
            }
        }
    }

    Vector<Candidate> candidates;
    HashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> candidatesUnderKey;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor || !results.hasObjects(id) || hasObjectsBelow.quickGet(index) || !results.constructions.isFollowed(id) || !results.inputs.build.isEnabled(FlowResultKind::StoreCounts, index))
            continue;
        // Code without a solve stores through the unknown object, and no statement stands for its stores.
        if (stores.hasEscapedObject(id) || stores.hasLostObject(id))
            continue;
        if (stores.getsNameNobodyLists(id) || results.heap.writtenUnder(results.layoutsOfObjects.objectsWith(id), ValueFlowName::of(ValueFlowNameKind::CopiedFrom)) != ValueSets::nothing)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            const Field& declaration = layout.fields[field];
            const FieldFlow& flow = application.layouts[index].fields[field].flow;
            if (!declaration.key || declaration.flags.contains(FieldFlags::IsInherited) || hasNodeOfItsOwn(declaration) || stores.mayGetAccessorUnder(id, declaration.key))
                continue;
            bool staysProperty = flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined });
            if (flow.location.writers != WriterState::AllKnown || flow.location.stores != StoreCount::Unknown || flow.location.initialization != InitializationState::SetAtCreation || !staysProperty)
                continue;
            candidatesUnderKey.add(declaration.key.index(), Vector<uint32_t, 2>()).iterator->value.append(candidates.size());
            Candidate candidate;
            candidate.layout = id;
            candidate.field = field;
            candidates.append(candidate);
        }
    }
    if (candidates.isEmpty())
        return;
    // A candidate stays one where every write is a store by name of one operation. Each thread looks at the writes that it takes, and whether all of them have seen the same operation shows in the end.
    Vector<Vector<StoreSeen>> storesSeenBy(loops.width());
    stores.forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID) {
        if (key.name().kind != ValueFlowNameKind::Property)
            return;
        auto found = candidatesUnderKey.find(key.payload);
        if (found == candidatesUnderKey.end())
            return;
        Vector<StoreSeen>& storesSeen = storesSeenBy[worker];
        if (storesSeen.isEmpty())
            storesSeen.grow(candidates.size());
        const ValueFlowStatement& statement = writer.statement;
        bool storesByName = statement.kind == ValueFlowStatementKind::Operation && (statement.opcode == Opcode::PropSet || statement.opcode == Opcode::PropDefine);
        // No candidate has an object that has escaped or is lost, and the candidates under a key stand in the order of their layouts.
        if (stores.placeholdersInSetOfWrite(key.set).almostEverything) {
            for (uint32_t index : found->value)
                storesSeen[index].isStoredOtherwise = true;
        }
        stores.forEachLayoutListedInSetOfWrite(key.set, writer, [&](LayoutID layout) {
            auto candidate = std::ranges::lower_bound(found->value, layout, { }, [&](uint32_t index) {
                return candidates[index].layout;
            });
            if (candidate == found->value.end() || candidates[*candidate].layout != layout)
                return;
            storesSeen[*candidate].isStoredOtherwise |= !storesByName;
            storesSeen[*candidate].note(statement.function, statement.site);
        });
    });
    for (const Vector<StoreSeen>& storesSeen : storesSeenBy) {
        for (uint32_t index = 0; index < storesSeen.size(); ++index) {
            candidates[index].isStoredOtherwise |= storesSeen[index].isStoredOtherwise;
            if (storesSeen[index].store)
                candidates[index].note(storesSeen[index].function, storesSeen[index].store);
        }
    }

    // A constructor function that somebody calls without new can run again on an object that exists already.
    std::optional<ValueSetID> calleesOfCalls;
    Vector<uint8_t> isCalledDirectly;
    auto isOnlyConstructed = [&](FunctionID function) {
        const ValueFlowResultInputs& inputs = results.inputs;
        if (!calleesOfCalls) {
            Vector<ValueSetID> calleesOfGroups = collectFromCallGroups<ValueSetID>(inputs, results.heap, [&](const ValueFlowNodeKey& key, ValueFlowNodeID, Vector<ValueSetID>& sets) {
                if (!isConstruction(key.callKind()))
                    sets.append(key.set);
            });
            UnionOfSets callees;
            for (ValueSetID set : calleesOfGroups)
                callees.add(set);
            calleesOfCalls = callees.take(results.heap.maker());
            isCalledDirectly.fill(0, program.functions.size());
            forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
                if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::DirectCall })))
                    return;
                if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, results.tables, FunctionID(index)))
                    return;
                for (const Value& value : program.functions[index].values) {
                    if (accessClassOf(value.opcode) == AccessClass::DirectCall && value.function())
                        AtomicRef(isCalledDirectly[value.function().index()]).store(1, std::memory_order_relaxed);
                }
            });
        }
        ValueFlowObjectID closure = inputs.universe().closureOf(function);
        return !inputs.hasEscaped(closure) && !inputs.isLost(closure) && !inputs.sets().contains(*calleesOfCalls, closure) && !isCalledDirectly[function.index()];
    };
    candidates.removeAllMatching([&](const Candidate& candidate) {
        if (candidate.isStoredOtherwise || !candidate.store)
            return true;
        switch (program.at(candidate.function).kind) {
        case FunctionKind::BaseConstructor:
        case FunctionKind::DerivedConstructor:
            return program.at(candidate.layout).constructor != candidate.function;
        case FunctionKind::Initializer:
            return false;
        case FunctionKind::Plain:
            return program.at(candidate.layout).constructor != candidate.function || !isOnlyConstructed(candidate.function);
        case FunctionKind::Arrow:
        case FunctionKind::Method:
        case FunctionKind::Getter:
        case FunctionKind::Setter:
        case FunctionKind::Module:
        case FunctionKind::CommonJsModule:
            return true;
        }
    });
    AOT::sort(candidates, { }, [](const Candidate& candidate) {
        return std::tuple { candidate.function, candidate.layout, candidate.field };
    });
    for (size_t first = 0; first < candidates.size();) {
        const Function& function = program.at(candidates[first].function);
        CFG cfg(function);
        BlockCycles cycles(cfg);
        BitVector isInCycle(function.values.size());
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            if (!cycles.isInCycle(BlockID(block)))
                continue;
            for (ValueID value : function.blocks[block].values)
                isInCycle.quickSet(value.index());
        }
        size_t end = first;
        for (; end < candidates.size() && candidates[end].function == candidates[first].function; ++end) {
            if (!isInCycle.quickGet(candidates[end].store.index()))
                application.layouts[candidates[end].layout.index()].fields[candidates[end].field].flow.location.stores = StoreCount::AtMostOnce;
        }
        first = end;
    }
}

static void publishConstructions(const FieldResultsWithoutStores& results)
{
    struct FieldAtBirth {
        FieldReference declaration;
        bool isStored;
        bool isStoredWithValue;
    };

    const Program& program = results.program();
    BuildContext& build = results.inputs.build;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
            results.application.layouts[index].flow.constructorOrders = results.constructions.ordersOf(LayoutID(index));
    }
    Vector<FieldAtBirth> fieldsAtBirth = collectInParallel<FieldAtBirth>(results.inputs.loops(), program.layouts.size(), [&](unsigned index, Vector<FieldAtBirth>& fields) {
        LayoutID id(index);
        if (!results.hasObjects(id) || !build.isEnabled(FlowResultKind::Initialization, index))
            return;
        forEachFieldOfObjectsWith(program, results.application, id, [&](FieldReference declaration, const Field& field) {
            fields.append({ declaration, results.constructions.isStoredAtBirth(id, field.key, declaration), results.constructions.isStoredWithValueAtBirth(id, field.key) });
        });
    });
    Vector<BitVector> isStoredAtBirth(program.layouts.size());
    Vector<BitVector> isLeftOutAtBirth(program.layouts.size());
    Vector<BitVector> isLeftWithoutValueAtBirth(program.layouts.size());
    for (const FieldAtBirth& field : fieldsAtBirth) {
        (field.isStored ? isStoredAtBirth : isLeftOutAtBirth)[field.declaration.layout.index()].set(field.declaration.field);
        if (!field.isStoredWithValue)
            isLeftWithoutValueAtBirth[field.declaration.layout.index()].set(field.declaration.field);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        for (size_t field : isStoredAtBirth[index]) {
            FieldFlow& flow = results.application.layouts[index].fields[field].flow;
            if (!isLeftOutAtBirth[index].get(field) && flow.occupancy == Occupancy::Unknown)
                flow.occupancy = isLeftWithoutValueAtBirth[index].get(field) ? Occupancy::FilledAtBirth : Occupancy::FilledWithValueAtBirth;
        }
    }

    BitVector& functions = results.tables.functions.functionsSeeingOnlyFinishedObjects;
    functions = BitVector(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (build.isEnabled(FlowResultKind::Initialization, index) && results.constructions.seesOnlyFinishedObjects(FunctionID(index)))
            functions.quickSet(index);
    }
}

void listStoresByLayout(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, const FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    usesOfHeap.layoutsOfObjects.emplace(inputs, usesOfHeap.heap);
    times.didFinish("fields: layouts of objects"_s);
    usesOfHeap.stores.emplace(inputs, tables, usesOfHeap.heap, *usesOfHeap.layoutsOfObjects);
}

ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, const ValueFlowConstructions& constructions, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueFlowHeapView& heap = usesOfHeap.heap;
    const UsesOfObjects& uses = usesOfHeap.uses;
    FieldResultsWithoutStores resultsWithoutStores { inputs, tables, *tables.application, constructions };
    Vector<AllocatingOperation> allocatingOperations;
    runTasksInParallel(inputs.loops(), {
        [&] {
            usesOfHeap.stores->noteListedWrites();
            usesOfHeap.stores->setReceiversOfReads(usesOfHeap.uses.takeReceiversOfReads());
            times.didFinish("fields: stores by layout"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            publishConstructions(resultsWithoutStores);
            allocatingOperations = collectAllocatingOperations(resultsWithoutStores);
            computeInitialFieldOrders(resultsWithoutStores, allocatingOperations.span());
            timesBeside.didFinish("fields: initial orders, beside stores by layout"_s);
        },
    });
    const LayoutsOfObjects& layoutsOfObjects = *usesOfHeap.layoutsOfObjects;
    StoresByLayout& stores = *usesOfHeap.stores;
    FieldResults results { resultsWithoutStores, heap, layoutsOfObjects, stores };
    times.didFinish("fields: the rest of the initial orders"_s);
    ValueFlowFieldContents contents = computeDeclaredFields(results, uses, allocatingOperations.span());
    allocatingOperations.clear();
    times.didFinish("fields: declared fields"_s);
    computeFieldsStoredOnceByConstructors(results);
    computeKeyEnumerations(results, uses);
    times.didFinish("fields: stored once, enumerations"_s);

    ProvenHierarchy hierarchy(inputs.program, *tables.application);
    Vector<Vector<PropertyKeyID>> appendedKeys = chooseAppendedKeys(results, hierarchy);
    times.didFinish("fields: appended keys chosen"_s);
    appendFields(results, hierarchy, appendedKeys.span(), contents);
    times.didFinish("fields: fields appended"_s);
    computeAddedKeys(results, appendedKeys.span());
    times.didFinish("fields: added keys"_s);
    computeFieldCreations(results);
    times.didFinish("fields: creations"_s);
    return contents;
}

// The type of an appended field is made of all that is stored under its name, so no store brings a value outside it.
static void typeAppendedFields(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& contents, TypeTable& types, FlowApplication& application)
{
    bool hasAppendedFields = std::ranges::any_of(application.layouts, [](const FlowLayoutUpdate& update) {
        return !update.appendedFields.isEmpty();
    });
    if (!hasAppendedFields)
        return;

    // By layout: the appended fields for whose key the families of the layout declare a type.
    Vector<BitVector> isDeclaredInFamilies = findAppendedFieldsDeclaredInFamilies(inputs.program, application.layouts, inputs.loops());

    ValueFlowPositionCounts counts(nullptr);
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    Type asRead = Type::anything();
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        for (uint32_t index = 0; index < application.layouts.size(); ++index) {
            Vector<FlowAppendedField>& fields = application.layouts[index].appendedFields;
            if (fields.isEmpty())
                continue;
            ValueFlowPositions positions(inputs, arena, counts, index, &maker);
            for (uint32_t field = 0; field < fields.size(); ++field) {
                FieldFlow& flow = fields[field].flow;
                if (flow.location.writers != WriterState::AllKnown)
                    continue;
                ValueSetID stored = maker.unite(contents.storedInAppendedFields[index][field], maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent)));
                ValueFlowPositionResult result = positions.resultAt(asRead, stored);
                if (result.proof == ValueFlowProof::Proven && result.narrowed) {
                    fields[field].position.type = arena.intern(*result.narrowed);
                    fields[field].position.isProven = true;
                }
                // Nothing is ever stored that lies outside `anything`.
                bool takesDeclaredType = !fields[field].position.isProven && isDeclaredInFamilies[index].get(field);
                // Which getters the slot of an accessor takes is a guess: a pair with another getter, or with a setter alone, is turned away and lies beside the slots.
                if (inputs.build.isEnabled(FlowResultKind::HolderMutations, index) && !fields[field].accessor.isAccessor() && !takesDeclaredType)
                    flow.stability.add(FieldStability::IsNeverStoredWithOtherType);
                if (flow.location.initialization == InitializationState::SetAtCreation && flow.stability == everyFlagOf<FieldStability>() && !inputs.sets().isEmpty(contents.storedInAppendedFields[index][field])) {
                    flow.occupancy = Occupancy::AlwaysFilled;
                    flow.prototypeName = PrototypeNameState::Unknown;
                }
            }
        }
    }
    TypeRemap remap = phase.finish(StringRemap { }, &inputs.loops());
    for (FlowLayoutUpdate& update : application.layouts) {
        for (FlowAppendedField& field : update.appendedFields)
            remapTypes(field.position, remap);
    }
}

void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& contents, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    typeAppendedFields(inputs, contents, types, application);

    // Whether it holds of a layout is read off the fields, which nothing in here writes to.
    for (uint32_t index : findIndicesInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        const LayoutFlow& flow = application.layouts[index].flow;
        if (flow.allocationClasses.isEmpty())
            return false;
        bool isNeverStoredOtherTypes = true;
        forEachFieldOfObjectsWith(program, application, LayoutID(index), [&](FieldReference declaration, const Field&) {
            isNeverStoredOtherTypes &= application.layouts[declaration.layout.index()].fields[declaration.field].flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
        });
        LayoutID current(index);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            for (const FlowAppendedField& field : application.layouts[current.index()].appendedFields)
                isNeverStoredOtherTypes &= field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
            current = provenBaseOf(application, current);
        }
        return isNeverStoredOtherTypes;
    })) {
        LayoutFlow& flow = application.layouts[index].flow;
        flow.impossibleMutations.add(ObjectMutation::StoreOfOtherType);
        for (AllocationClassID allocationClass : flow.allocationClasses)
            tables.sites.allocationClasses[allocationClass.index()].impossibleMutations.add(ObjectMutation::StoreOfOtherType);
    }
}

static bool leavesReadsToBeFound(ReadState reads)
{
    return reads == ReadState::OnlyCalled || reads == ReadState::OnlyAccessed;
}

// A field that is only called goes away once the reads that are left in the program do not reach it.
static void keepFieldsOnlyCalledWhoseReadsHaveStamps(const ValueFlowResultInputs& inputs, FlowApplication& application, ValueFlowHeapView* heap, StoresByLayout* stores)
{
    const Program& program = inputs.program;
    BitVector namesFieldOnlyCalled(program.types.numberOfPropertyKeys());
    bool hasFieldOnlyCalled = false;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            LocationFlow& location = application.layouts[index].fields[field].flow.location;
            if (!leavesReadsToBeFound(location.reads))
                continue;
            if (!stores) {
                location.reads = ReadState::Unknown;
                continue;
            }
            namesFieldOnlyCalled.quickSet(layout.fields[field].key.index());
            hasFieldOnlyCalled = true;
        }
    }
    if (!hasFieldOnlyCalled)
        return;

    // What is read through a placeholder is collected first, and each layout is looked at once.
    BitVector readThroughUnknownObject;
    BitVector readThroughUntrackedObject;
    BitVector readFromEveryObject;
    struct Read {
        uint32_t key;
        uint32_t receivers;
        uint32_t statement;
    };
    const ParallelLoops& loops = inputs.loops();
    const ValueFlowHeapView& heapForAllThreads = *heap;
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    // The reads that no pass will find by a stamp are listed on all threads.
    Vector<Read> readsWithoutStamp;
    {
        ValueSetMakers makers(inputs);
        readsWithoutStamp = collectWithMakers<Read>(inputs, makers, program.functions.size(), [&](unsigned body, ValueSetMaker& maker, Vector<Read>& found) {
            if (!inputs.isReached(FunctionID(body)))
                return;
            std::span<const FlowAccessStamp> stamps = application.bodies[body].accesses.span();
            // The generic version of the function repeats a read behind a check by name, and that read has no stamp: see GenericVersionBuilder::eraseProofsInCopy().
            std::optional<BitVector> valuesBehindChecks;
            auto isRepeatedWithoutStamp = [&](ValueID read) {
                if (!valuesBehindChecks) {
                    const Function& function = program.functions[body];
                    BitVector checksThatTestNothing(function.values.size());
                    for (const FlowClaimUpdate& claim : application.bodies[body].claims) {
                        if (claim.isRedundant || claim.arriving)
                            checksThatTestNothing.quickSet(claim.claim.index());
                    }
                    bool hasCheckThatStays = false;
                    for (uint32_t index = 0; index < function.values.size() && !hasCheckThatStays; ++index)
                        hasCheckThatStays = function.values[index].opcode == Opcode::Check && !checksThatTestNothing.quickGet(index);
                    valuesBehindChecks = hasCheckThatStays ? findValuesBehindChecks(program, function, CFG(function), &checksThatTestNothing) : BitVector();
                }
                return valuesBehindChecks->get(read.index());
            };
            for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index) {
                const ValueFlowStatement& statement = statements[index];
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::PropGet)
                    continue;
                PropertyKeyID key = program.at(statement.function).at(statement.site).key();
                if (!namesFieldOnlyCalled.quickGet(key.index()))
                    continue;
                if (findByValue(stamps, statement.site, &FlowAccessStamp::value) && !isRepeatedWithoutStamp(statement.site))
                    continue;
                found.append(Read { key.index(), heapForAllThreads.valueOf(maker, inputs.bodies().operandsOf(statement)[OperandIndex::PropGet::object]).index(), index });
            }
        });
    }
    // One set of receivers is looked at once for a name, on behalf of the first read that has it.
    parallelSort(loops, readsWithoutStamp.mutableSpan(), [](const Read& first, const Read& second) {
        return std::tuple { first.key, first.receivers, first.statement } < std::tuple { second.key, second.receivers, second.statement };
    });
    readsWithoutStamp.shrink(std::ranges::unique(readsWithoutStamp, [](const Read& first, const Read& second) {
        return first.key == second.key && first.receivers == second.receivers;
    }).begin() - readsWithoutStamp.begin());

    Vector<ValueSetID> setsOfReceivers(readsWithoutStamp.size(), [&](size_t position) {
        return ValueSetID(readsWithoutStamp[position].receivers);
    });
    AOT::sort(setsOfReceivers, { }, [](ValueSetID set) { return set.index(); });
    removeRepeatedElements(setsOfReceivers);
    stores->lookAt(setsOfReceivers.span());

    // A field has one name, so no two threads write one field.
    Vector<uint32_t> firstReadOfName;
    for (uint32_t position = 0; position < readsWithoutStamp.size(); ++position) {
        if (!position || readsWithoutStamp[position].key != readsWithoutStamp[position - 1].key)
            firstReadOfName.append(position);
    }
    uint32_t numberOfNames = firstReadOfName.size();
    firstReadOfName.append(readsWithoutStamp.size());
    Vector<StoresByLayout::PlaceholdersInSet> placeholdersReadThrough(numberOfNames);
    const StoresByLayout& storesForAllThreads = *stores;
    forEachIndexInParallel(loops, numberOfNames, [&](unsigned name) {
        PropertyKeyID key(readsWithoutStamp[firstReadOfName[name]].key);
        StoresByLayout::PlaceholdersInSet& all = placeholdersReadThrough[name];
        BitVector layoutsSearched(program.layouts.size());
        for (const Read& read : readsWithoutStamp.span().subspan(firstReadOfName[name], firstReadOfName[name + 1] - firstReadOfName[name])) {
            Writer reader { statements[read.statement] };
            StoresByLayout::PlaceholdersInSet placeholders = storesForAllThreads.placeholdersInSetOfWrite(ValueSetID(read.receivers));
            all.unknownObject |= placeholders.unknownObject;
            all.untrackedObject |= placeholders.untrackedObject;
            all.almostEverything |= placeholders.almostEverything;
            storesForAllThreads.forEachLayoutListedInSetOfWrite(ValueSetID(read.receivers), reader, [&](LayoutID layout) {
                if (layoutsSearched.quickSet(layout.index()))
                    return;
                // The lowest layout on the chain that has the name hides the ones above it.
                LayoutID current = layout;
                for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                    if (std::optional<uint32_t> found = program.at(current).findField(key)) {
                        FieldReference declaration = declarationOf(program, { current, *found });
                        LocationFlow& location = application.layouts[declaration.layout.index()].fields[declaration.field].flow.location;
                        if (leavesReadsToBeFound(location.reads))
                            location.reads = ReadState::Unknown;
                        break;
                    }
                    current = provenBaseOf(application, current);
                }
            });
        }
    });
    for (uint32_t name = 0; name < numberOfNames; ++name) {
        size_t key = readsWithoutStamp[firstReadOfName[name]].key;
        if (placeholdersReadThrough[name].unknownObject)
            readThroughUnknownObject.set(key);
        if (placeholdersReadThrough[name].untrackedObject)
            readThroughUntrackedObject.set(key);
        if (placeholdersReadThrough[name].almostEverything)
            readFromEveryObject.set(key);
    }
    if (readThroughUnknownObject.isEmpty() && readThroughUntrackedObject.isEmpty() && readFromEveryObject.isEmpty())
        return;
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        LayoutID layout(index);
        bool hasEscapedObject = storesForAllThreads.hasEscapedObject(layout);
        bool hasLostObject = storesForAllThreads.hasLostObject(layout);
        forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference declaration, const Field& field) {
            AtomicRef reads(application.layouts[declaration.layout.index()].fields[declaration.field].flow.location.reads);
            if (!field.key || !leavesReadsToBeFound(reads.load(std::memory_order_relaxed)))
                return;
            size_t key = field.key.index();
            if (readFromEveryObject.get(key) || (hasEscapedObject && readThroughUnknownObject.get(key)) || (hasLostObject && readThroughUntrackedObject.get(key)))
                reads.store(ReadState::Unknown, std::memory_order_relaxed);
        });
    });
}

// A pass that takes a field away has to find every store to it. It finds an allocation that lists the field, and a store or a definition by name whose stamp lists the layouts of all its receivers.
void computeStoreNamingResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    if (!inputs.build.isEnabledEverywhere(FlowResultKind::ReceiverSets)) {
        keepFieldsOnlyCalledWhoseReadsHaveStamps(inputs, application, nullptr, nullptr);
        return;
    }

    ValueFlowHeapView& heap = usesOfHeap.heap;
    StoresByLayout& stores = *usesOfHeap.stores;
    keepFieldsOnlyCalledWhoseReadsHaveStamps(inputs, application, &heap, &stores);

    // A byte a field: whoever finds such a store sets it, on any thread.
    Vector<uint32_t> firstFieldOfLayout(FillWith { }, program.layouts.size() + 1, 0u);
    for (uint32_t index = 0; index < program.layouts.size(); ++index)
        firstFieldOfLayout[index + 1] = firstFieldOfLayout[index] + program.layouts[index].fields.size();
    Vector<uint8_t> isStoredWithoutName(FillWith { }, firstFieldOfLayout.last(), static_cast<uint8_t>(0));
    auto noteField = [&](FieldReference declaration) {
        AtomicRef(isStoredWithoutName[firstFieldOfLayout[declaration.layout.index()] + declaration.field]).store(1, std::memory_order_relaxed);
    };
    auto namesLocation = [&](const Writer& writer, ValueFlowName name) {
        const ValueFlowStatement& statement = writer.statement;
        if (statement.kind != ValueFlowStatementKind::Operation || name.kind != ValueFlowNameKind::Property)
            return false;
        if (statement.opcode == Opcode::NewObject)
            return true;
        if (statement.opcode != Opcode::PropSet && statement.opcode != Opcode::PropDefine)
            return false;
        if (program.at(statement.function).at(statement.site).key().index() != name.payload)
            return false;
        return !!findByValue(application.bodies[statement.function.index()].accesses.span(), statement.site, &FlowAccessStamp::value);
    };
    struct FieldUnderKey {
        uint32_t key;
        FieldReference declaration;
    };
    // Listed by the first thread that asks.
    struct FieldsOfLayout {
        Lock lock;
        std::atomic<bool> isListed { false };
        Vector<FieldUnderKey> fields;
    };
    UniqueArray<FieldsOfLayout> fieldsByLayout = makeUniqueArray<FieldsOfLayout>(program.layouts.size());
    auto fieldsOfObjectsWith = [&](LayoutID layout) -> const Vector<FieldUnderKey>& {
        FieldsOfLayout& entry = fieldsByLayout[layout.index()];
        if (entry.isListed.load(std::memory_order_acquire))
            return entry.fields;
        Locker locker { entry.lock };
        if (!entry.isListed.load(std::memory_order_relaxed)) {
            forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference declaration, const Field& field) {
                entry.fields.append({ field.key ? field.key.index() : UINT32_MAX, declaration });
            });
            AOT::stableSort(entry.fields, { }, &FieldUnderKey::key);
            entry.isListed.store(true, std::memory_order_release);
        }
        return entry.fields;
    };
    // What goes through a placeholder is collected first, and each layout is looked at once.
    struct NamesStored {
        BitVector names;
        bool mayBeAnyName { false };
        bool isEmpty { true };
    };
    struct SeenByWorker {
        NamesStored throughUnknownObject;
        NamesStored throughUntrackedObject;
        NamesStored intoEveryObject;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> nodesAndTypesOfReceiversSeen;
    };
    Vector<SeenByWorker> seenBy(inputs.loops().width());
    // Many statements write to one node. Which fields a write that names no location reaches hangs on the node and on the type of the receiver alone.
    const ValueFlowSolver& solver = inputs.solver();
    Vector<uint8_t> nodesSeenWithoutTypeOfReceiver(FillWith { }, solver.graph().numberOfNodes() - solver.input().numberOfNodes, static_cast<uint8_t>(0));
    stores.forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID) {
        SeenByWorker& seen = seenBy[worker];
        ValueFlowName name = key.name();
        bool mayBeAnyName = false;
        switch (name.kind) {
        case ValueFlowNameKind::Property:
        case ValueFlowNameKind::Getter:
        case ValueFlowNameKind::Setter:
            break;
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::GetterUnderUnknownKey:
        case ValueFlowNameKind::SetterUnderUnknownKey:
        case ValueFlowNameKind::CopiedFrom:
            mayBeAnyName = true;
            break;
        case ValueFlowNameKind::Elements:
        case ValueFlowNameKind::EveryProperty:
        case ValueFlowNameKind::EveryGetter:
        case ValueFlowNameKind::EverySetter:
        case ValueFlowNameKind::PrototypeLink:
        case ValueFlowNameKind::DeletedMember:
        case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
        case ValueFlowNameKind::RedefinedMember:
        case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
        case ValueFlowNameKind::PrivateField:
        case ValueFlowNameKind::Slot:
        case ValueFlowNameKind::HiddenSlot:
        case ValueFlowNameKind::InternalFields:
        case ValueFlowNameKind::SteppedValues:
        case ValueFlowNameKind::ReturnedValue:
        case ValueFlowNameKind::SentValues:
        case ValueFlowNameKind::OpenedBy:
        case ValueFlowNameKind::ResolvedPromises:
        case ValueFlowNameKind::RejectedPromises:
        case ValueFlowNameKind::AllocatedAs:
        case ValueFlowNameKind::OpenedIterators:
        case ValueFlowNameKind::IteratedValues:
        case ValueFlowNameKind::IteratorResults:
        case ValueFlowNameKind::AwaitedValue:
        case ValueFlowNameKind::MemberWritten:
        case ValueFlowNameKind::EveryMemberWritten:
        case ValueFlowNameKind::EveryMemberRead:
        case ValueFlowNameKind::RestArgument:
        case ValueFlowNameKind::AddedNames:
        case ValueFlowNameKind::FoundThroughEscapedInstances:
            return;
        }
        if (namesLocation(writer, name))
            return;
        uint32_t node = solver.indexOfMadeNodes(key);
        bool isSeen = writer.layoutsOfReceiver.empty() ? AtomicRef(nodesSeenWithoutTypeOfReceiver[node]).exchange(1, std::memory_order_relaxed) : !seen.nodesAndTypesOfReceiversSeen.add(static_cast<uint64_t>(node) << 32 | writer.familyOfReceiver.index()).isNewEntry;
        if (isSeen)
            return;
        auto noteFieldsOf = [&](LayoutID layout) {
            const Vector<FieldUnderKey>& fields = fieldsOfObjectsWith(layout);
            if (mayBeAnyName) {
                for (const FieldUnderKey& field : fields)
                    noteField(field.declaration);
                return;
            }
            auto first = std::ranges::lower_bound(fields, name.payload, { }, &FieldUnderKey::key);
            for (; first != fields.end() && first->key == name.payload; ++first)
                noteField(first->declaration);
            for (auto withoutKey = fields.end(); withoutKey != fields.begin() && (withoutKey - 1)->key == UINT32_MAX; --withoutKey)
                noteField((withoutKey - 1)->declaration);
        };
        // The type of the receiver picks among the layouts.
        if (!writer.layoutsOfReceiver.empty()) {
            stores.forEachLayoutWrittenThroughSetOfWrite(key.set, writer, noteFieldsOf);
            return;
        }
        auto noteName = [&](NamesStored& stored) {
            stored.isEmpty = false;
            if (mayBeAnyName)
                stored.mayBeAnyName = true;
            else
                stored.names.set(name.payload);
        };
        StoresByLayout::PlaceholdersInSet placeholders = stores.placeholdersInSetOfWrite(key.set);
        if (placeholders.unknownObject)
            noteName(seen.throughUnknownObject);
        if (placeholders.untrackedObject)
            noteName(seen.throughUntrackedObject);
        if (placeholders.almostEverything)
            noteName(seen.intoEveryObject);
        stores.forEachLayoutListedInSetOfWrite(key.set, writer, noteFieldsOf);
    });
    NamesStored throughUnknownObject;
    NamesStored throughUntrackedObject;
    NamesStored intoEveryObject;
    for (const SeenByWorker& seen : seenBy) {
        auto take = [](NamesStored& all, const NamesStored& some) {
            all.names.merge(some.names);
            all.mayBeAnyName |= some.mayBeAnyName;
            all.isEmpty &= some.isEmpty;
        };
        take(throughUnknownObject, seen.throughUnknownObject);
        take(throughUntrackedObject, seen.throughUntrackedObject);
        take(intoEveryObject, seen.intoEveryObject);
    }
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID layout(index);
        const NamesStored* reaching[] {
            &intoEveryObject,
            stores.hasEscapedObject(layout) ? &throughUnknownObject : nullptr,
            stores.hasLostObject(layout) ? &throughUntrackedObject : nullptr,
        };
        auto reaches = [](const NamesStored* stored) {
            return stored && !stored->isEmpty;
        };
        if (std::ranges::none_of(reaching, reaches))
            return;
        forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference declaration, const Field& field) {
            bool isStored = std::ranges::any_of(reaching, [&](const NamesStored* stored) {
                return reaches(stored) && (stored->mayBeAnyName || !field.key || stored->names.get(field.key.index()));
            });
            if (isStored)
                noteField(declaration);
        });
    });

    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            LocationFlow& location = application.layouts[index].fields[field].flow.location;
            bool isRedefined = !application.layouts[index].fields[field].flow.stability.contains(FieldStability::IsNeverRedefined);
            if (location.writers == WriterState::AllKnown && !isRedefined && !isStoredWithoutName[firstFieldOfLayout[index] + field] && !layout.fields[field].flags.contains(FieldFlags::IsInherited))
                location.storeNaming = StoreNaming::EveryStoreNamesLocation;
        }
    });
    // What code without a solve stores under a name it stores by an operation with that name, whose receiver has no stamp and a type that lists nothing.
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        if (!stores.hasEscapedObject(LayoutID(index)))
            return;
        forEachFieldOfObjectsWith(program, application, LayoutID(index), [&](FieldReference declaration, const Field&) {
            AtomicRef(application.layouts[declaration.layout.index()].fields[declaration.field].flow.location.storeNaming).store(StoreNaming::Unknown, std::memory_order_relaxed);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
