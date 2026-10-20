/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowObjectState.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPrototypes.h"
#include "AOTGrouping.h"
#include <algorithm>
#include <utility>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<FlowLocationEvent> storeEvents { FlowLocationEvent::StoredByOperation, FlowLocationEvent::StoredByManyOperations };
static constexpr OptionSet<FlowObjectUse> unknownKeyReadUses = unnamedReadUses | FlowObjectUse::Serialized;

static constexpr OptionSet<FlowLocationEvent> unknownJavaScriptEvents {
    FlowLocationEvent::WrittenByUnknownCode,
    FlowLocationEvent::ReadByUnknownCode,
    FlowLocationEvent::Deleted,
    FlowLocationEvent::Redefined,
    FlowLocationEvent::StoredOutsideType,
    FlowLocationEvent::StoredByManyOperations,
    FlowLocationEvent::StoredOtherValue,
};

static LayoutID parentOf(const Program& program, std::span<const FlowBaseResult> bases, LayoutID layout)
{
    const FlowBaseResult& base = bases[layout.index()];
    switch (base.state) {
    case BaseState::Layout:
        return base.base;
    case BaseState::Unknown:
        return predictedHeritageOf(program.at(layout));
    case BaseState::None:
    case BaseState::BuiltinClass:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

class ParentChainWalker {
public:
    ParentChainWalker(const Program& program, std::span<const FlowBaseResult> bases)
        : m_program(program)
        , m_bases(bases)
        , m_isOnChain(bases.size())
    {
    }

    std::span<const LayoutID> chainOf(LayoutID layout) LIFETIME_BOUND
    {
        for (LayoutID previous : m_chain)
            m_isOnChain.clear(previous.index());
        m_chain.shrink(0);

        for (LayoutID current = layout; current && !m_isOnChain.set(current.index()); current = parentOf(m_program, m_bases, current))
            m_chain.append(current);
        return m_chain.span();
    }

private:
    const Program& m_program;
    std::span<const FlowBaseResult> m_bases;
    BitVector m_isOnChain;
    Vector<LayoutID, 16> m_chain;
};

} // anonymous namespace

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlowHierarchy);

FlowHierarchy::FlowHierarchy(const FlowSolution& solution, Vector<FlowBaseResult>&& basesByLayout)
    : m_solution(solution)
    , m_bases(WTF::move(basesByLayout))
{
    RELEASE_ASSERT(m_bases.size() == solution.program.layouts.size());
    computeDictionaryClasses();
    runTasksInParallel(solution.build.parallelLoops(), {
        [&] { computeChains(); },
        [&] { computeLayoutsBelow(); },
        [&] { computeObjects(); },
    });
}

void FlowHierarchy::computeChains()
{
    const Program& program = m_solution.program;
    unsigned count = m_bases.size();

    ParentChainWalker walker(program, m_bases.span());
    Vector<uint32_t> depths(count, [&](size_t index) {
        return static_cast<uint32_t>(walker.chainOf(LayoutID(index)).size() - 1);
    });
    Vector<uint32_t> firstLayoutOfDepth;
    groupByIndex(count, firstLayoutOfDepth, m_layoutsFromBasesDown, [&](const auto& visit) {
        for (unsigned index = 0; index < count; ++index)
            visit(depths[index], LayoutID(index));
    });

    m_hasProvenChain.ensureSize(count);
    for (LayoutID layout : m_layoutsFromBasesDown) {
        const FlowBaseResult& base = baseOf(layout);
        bool isProven = false;
        switch (base.state) {
        case BaseState::Layout:
            // A base comes earlier in this order unless the parents form a cycle, and then its bit is still clear.
            isProven = m_hasProvenChain.get(base.base.index());
            break;
        case BaseState::Unknown:
            isProven = !program.at(layout).has(LayoutFlags::HasHeritage);
            break;
        case BaseState::None:
        case BaseState::BuiltinClass:
            isProven = true;
            break;
        }
        m_hasProvenChain.quickSet(layout.index(), isProven);
    }
}

void FlowHierarchy::computeLayoutsBelow()
{
    unsigned count = m_bases.size();
    ParentChainWalker walker(m_solution.program, m_bases.span());
    groupByIndex(count, m_firstLayoutBelow, m_layoutsBelow, [&](const auto& visit) {
        for (unsigned index = 0; index < count; ++index) {
            LayoutID layout(index);
            for (LayoutID layoutOrAncestor : walker.chainOf(layout))
                visit(layoutOrAncestor.index(), layout);
        }
    });
}

// `{ type: "string", ...rest }` is allocated without fields, and its names are defined one after the other.
static bool isDefinitionOfNameOn(const Value& value, ValueID literal)
{
    switch (value.opcode) {
    case Opcode::PropDefine:
        return value.objectOperand() == literal;
    case Opcode::DefineGetter:
    case Opcode::DefineSetter:
        return value.key() && value.objectOperand() == literal;
    default:
        return false;
    }
}

static bool namesNoProperty(const Function& function, ValueID literal)
{
    return !function.values.containsIf([&](const Value& value) {
        return isDefinitionOfNameOn(value, literal);
    });
}

static bool isEmptyObjectLiteral(const FlowSolution& solution, const FlowSite& site)
{
    const FlowSiteDescription& description = site.description;
    if (description.kind != FlowSiteKind::Layout || !description.value)
        return false;
    if (!site.function || solution.graph.isHost(site.function))
        return false;
    const Function& function = solution.program.at(site.function);
    const Value& allocation = function.at(description.value);
    if (description.origin == FlowSiteOrigin::Operation)
        return allocation.opcode == Opcode::NewObject && allocation.fields().empty() && !allocation.prototypeOperand() && namesNoProperty(function, description.value);
    if (description.origin != FlowSiteOrigin::DataNode || description.ordinal)
        return false;
    const auto* root = solution.program.at(allocation.data()).root.tryAs<DataNode::Object>();
    return root && root->members.isEmpty();
}

static bool isAllocatedAndHasEvent(const FlowSolution& solution, FlowObjectID object, FlowObjectEvent event)
{
    return solution.records.eventsOf(object).containsAll({ FlowObjectEvent::Allocated, event });
}

ValueID findCopyFillingLiteral(const Function& function, ValueID literal)
{
    const Value& allocation = function.at(literal);
    if (allocation.opcode != Opcode::NewObject || !allocation.fields().empty() || allocation.prototypeOperand())
        return { };
    for (const Block& block : function.blocks) {
        size_t index = block.values.find(literal);
        if (index == notFound)
            continue;
        if (index + 1 == block.values.size())
            return { };
        ValueID next = block.values[index + 1];
        const Value& copy = function.at(next);
        bool fillsLiteral = copy.opcode == Opcode::CopyDataProperties && copy.targetOperand() == literal && copy.sourceOperand() != literal;
        return fillsLiteral ? next : ValueID();
    }
    return { };
}

static bool isLiteralFilledByCopy(const FlowSolution& solution, const FlowSite& site)
{
    const FlowSiteDescription& description = site.description;
    if (description.kind != FlowSiteKind::Layout || description.origin != FlowSiteOrigin::Operation || !description.value)
        return false;
    if (!site.function || solution.graph.isHost(site.function))
        return false;
    return !!findCopyFillingLiteral(solution.program.at(site.function), description.value);
}

static bool hasWrittenType(const Field& field)
{
    return field.declared.written || field.declared.type != TypeTable::anything;
}

void FlowHierarchy::computeDictionaryClasses()
{
    const FlowObjectTable& objects = m_solution.objects;
    unsigned count = objects.numberOfAllocationClasses();
    m_dictionaryClasses.ensureSize(count);
    for (unsigned index = 0; index < count; ++index) {
        AllocationClassID allocationClass(index);
        const FlowAllocationClassMembers& members = objects.at(allocationClass);
        if (!members.layout || members.sites.isEmpty() || !m_solution.build.isEnabled(FlowResultKind::DictionaryObjects, index))
            continue;
        const Layout& layout = m_solution.program.at(declaredLayoutOf(m_solution.program, members.layout));
        if (layout.kind != LayoutKind::Record)
            continue;
        auto someObjectHasEvent = [&](FlowObjectEvent event) {
            bool hasEvent = false;
            forEachObjectOfClass(objects, allocationClass, [&](FlowObjectID object) {
                hasEvent |= isAllocatedAndHasEvent(m_solution, object, event);
            });
            return hasEvent;
        };
        auto everySiteIs = [&](bool (*isSuchSite)(const FlowSolution&, const FlowSite&)) {
            return std::ranges::all_of(members.sites, [&](AllocationSiteID site) {
                return isSuchSite(m_solution, objects.at(site));
            });
        };
        bool isStoredLikeDictionary = layout.fields.isEmpty() && someObjectHasEvent(FlowObjectEvent::StoredByComputedKey) && everySiteIs(isEmptyObjectLiteral);
        bool holdsWhatItCopies = std::ranges::none_of(layout.fields, hasWrittenType) && someObjectHasEvent(FlowObjectEvent::CopiedUnderUnknownKeys) && everySiteIs(isLiteralFilledByCopy);
        if (isStoredLikeDictionary || holdsWhatItCopies)
            m_dictionaryClasses.quickSet(index);
    }
}

void FlowHierarchy::computeObjects()
{
    const FlowObjectTable& objects = m_solution.objects;
    const FlowRecords& records = m_solution.records;
    unsigned count = m_bases.size();

    m_firstObject.reserveInitialCapacity(count + 1);
    for (unsigned index = 0; index < count; ++index) {
        size_t first = m_objects.size();
        m_firstObject.append(static_cast<uint32_t>(first));
        for (AllocationClassID allocationClass : objects.allocationClassesOf(LayoutID(index))) {
            if (hasOnlyOrdinaryObjects(allocationClass))
                continue;
            forEachObjectOfClass(objects, allocationClass, [&](FlowObjectID object) {
                if (records.eventsOf(object).contains(FlowObjectEvent::Allocated))
                    m_objects.append(object);
            });
        }
        std::ranges::sort(m_objects.mutableSubspan(first));
    }
    m_firstObject.append(static_cast<uint32_t>(m_objects.size()));
}

const FlowBaseResult* FlowHierarchy::findBuiltinBase(LayoutID layout) const
{
    LayoutID current = layout;
    for (size_t steps = 0; steps < m_bases.size(); ++steps) {
        const FlowBaseResult& base = baseOf(current);
        if (base.state != BaseState::Layout)
            return base.state == BaseState::BuiltinClass ? &base : nullptr;
        current = base.base;
    }
    return nullptr;
}

static bool isMadeByCalledConstructor(const FlowSolution& solution, const FlowSite& site)
{
    const FlowSiteDescription& description = site.description;
    if (description.kind != FlowSiteKind::Layout || description.origin != FlowSiteOrigin::Operation || !description.value)
        return false;
    if (!site.function || solution.graph.isHost(site.function))
        return false;
    // The allocation of a base constructor is the other end of such a call: it names a layout with a heritage only for the prototype object of another new.target.
    Opcode opcode = solution.program.at(site.function).at(description.value).opcode;
    return OperandPosition::newTarget(opcode) || opcode == Opcode::NewObject;
}

// A compiled constructor allocates the layout of new.target only for a class below its own, and a class whose chain ends in a built-in class is below none.
// So whatever constructor a call reaches, one nobody sees included, makes no compiled object of such a layout: after Object.setPrototypeOf(Sub, Plain) the super call of
// `class Sub extends Map` gets an ordinary object, and what has the type of the layout is a cell of the built-in class.
bool FlowHierarchy::hasOnlyOrdinaryObjects(AllocationClassID allocationClass) const
{
    const FlowObjectTable& objects = m_solution.objects;
    const FlowAllocationClassMembers& members = objects.at(allocationClass);
    if (!members.layout || members.sites.isEmpty())
        return false;
    if (createsDictionaries(allocationClass))
        return true;

    bool hasBuiltinBase = findBuiltinBase(members.layout);
    return std::ranges::all_of(members.sites, [&](AllocationSiteID id) {
        const FlowSite& site = objects.at(id);
        return allocatesOrdinaryObject(site.description) || (hasBuiltinBase && isMadeByCalledConstructor(m_solution, site));
    });
}

std::span<const LayoutID> FlowHierarchy::layoutsBelow(LayoutID layout) const
{
    return entriesOfGroup(m_firstLayoutBelow, m_layoutsBelow, layout.index());
}

std::span<const FlowObjectID> FlowHierarchy::objectsOf(LayoutID layout) const
{
    return entriesOfGroup(m_firstObject, m_objects, layout.index());
}

std::optional<FieldReference> FlowHierarchy::findDeclaredField(LayoutID layout, PropertyKeyID key) const
{
    if (!key)
        return std::nullopt;

    const Program& program = m_solution.program;
    LayoutID current = layout;
    while (current) {
        if (std::optional<uint32_t> field = program.at(current).findField(key))
            return FieldReference { current, *field };
        const FlowBaseResult& base = baseOf(current);
        current = base.state == BaseState::Layout ? base.base : LayoutID();
    }
    return std::nullopt;
}

static void groupLinksByCaller(const FlowSolution& solution, Vector<uint32_t>& firstLink, Vector<FlowCallLinkID>& linksByCaller)
{
    const FlowRecords& records = solution.records;
    std::span<const FlowCallLink> links = records.callLinks();
    unsigned numberOfInstances = solution.graph.numberOfInstances();

    groupByIndex(numberOfInstances, firstLink, linksByCaller, [&](const auto& visit) {
        for (size_t index = 0; index < links.size(); ++index)
            visit(links[index].callerInstance.index(), FlowCallLinkID(index));
    });
    forEachIndexInParallel(solution.build.parallelLoops(), numberOfInstances, [&](unsigned instance) {
        uint32_t first = firstLink[instance];
        std::ranges::sort(linksByCaller.mutableSubspan(first, firstLink[instance + 1] - first), { }, [&](FlowCallLinkID link) {
            return std::pair { records.at(link).site, link };
        });
    });
}

static void groupLinksByCallee(const FlowSolution& solution, Vector<uint32_t>& firstLink, Vector<FlowCallLinkID>& linksByCallee)
{
    std::span<const FlowCallLink> links = solution.records.callLinks();
    unsigned numberOfFunctions = solution.graph.hostFunction().index() + 1;

    groupByIndex(numberOfFunctions, firstLink, linksByCallee, [&](const auto& visit) {
        for (size_t index = 0; index < links.size(); ++index)
            visit(links[index].callee.index(), FlowCallLinkID(index));
    });
}

FlowCallIndex::FlowCallIndex(const FlowSolution& solution)
    : m_graph(solution.graph)
    , m_records(solution.records)
{
    runTasksInParallel(solution.build.parallelLoops(), {
        [&] { groupLinksByCaller(solution, m_firstLinkOfInstance, m_linksByCaller); },
        [&] { groupLinksByCallee(solution, m_firstLinkOfCallee, m_linksByCallee); },
    });
}

std::span<const FlowCallLinkID> FlowCallIndex::linksFrom(FlowInstanceID instance) const
{
    return entriesOfGroup(m_firstLinkOfInstance, m_linksByCaller, instance.index());
}

std::span<const FlowCallLinkID> FlowCallIndex::linksAt(FlowInstanceID instance, ValueID site) const
{
    return recordsWith(linksFrom(instance), site, [&](FlowCallLinkID link) {
        return m_records.at(link).site;
    });
}

std::span<const FlowCallLinkID> FlowCallIndex::linksTo(FunctionID callee) const
{
    return entriesOfGroup(m_firstLinkOfCallee, m_linksByCallee, callee.index());
}

std::span<const FlowBuiltinCallLink> FlowCallIndex::builtinLinksAt(FlowInstanceID instance, ValueID site) const
{
    return recordsWith(m_records.builtinCallLinks(), std::pair { instance, site }, [](const FlowBuiltinCallLink& link) {
        return std::pair { link.callerInstance, link.site };
    });
}

bool FlowCallIndex::hasUnknownCalleeAt(FlowInstanceID instance, ValueID site) const
{
    // A callee that arrives after the instance gave up is linked to no site of it.
    if (m_graph.at(instance).state == FlowInstanceState::PastWorkLimit)
        return true;
    // An accessor, a protocol or a callback that the operation runs has callees of its own.
    return std::ranges::binary_search(m_records.unknownCallees(), FlowUnknownCallee { instance, site, FlowCallLinkKind::Call })
        || std::ranges::binary_search(m_records.unknownCallees(), FlowUnknownCallee { instance, site, FlowCallLinkKind::Construct });
}

FlowNodeID fieldNodeOn(const FlowGraph& graph, FlowObjectID object, const FlowFieldSubject& subject)
{
    if (subject.declaredIndex) {
        FlowFieldLocation location = findFieldLocation(graph, object, subject.layout, *subject.declaredIndex);
        return location.lookup == FlowFieldLookup::Found ? location.node : FlowNodeID();
    }
    const FlowProperty* property = graph.findProperty(object, subject.key);
    return property ? property->value : FlowNodeID();
}

void FlowLocationFacts::add(const FlowLocationRecord& record)
{
    hasNode = true;
    events.add(record.events);
    keepLowerWitness(firstStore, record.firstStore);
    keepLowerWitness(firstLoss, record.firstLoss);

    if (!record.deferrableFunction)
        return;
    if (!deferrableFunction) {
        deferrableFunction = record.deferrableFunction;
        deferrableCapture = record.deferrableCapture;
        return;
    }
    if (deferrableFunction != record.deferrableFunction) {
        hasOneDeferrableFunction = false;
        return;
    }
    if (deferrableCapture != record.deferrableCapture)
        deferrableCapture = ClosureDeferral::Unknown;
}

static bool hasAccessor(const FlowProperty* property)
{
    return property && (property->getter || property->setter);
}

static FlowLocationRecord aliasedRecordOf(const FlowSolution& solution, FlowObjectID object, PropertyKeyID key)
{
    FlowLocationRecord aliased;
    if (!key || !sharesLocationsWithUnknownObject(FlowWalkInputs::of(solution), object))
        return aliased;

    // A definition from a descriptor under a key that nobody can name is recorded on the part that stands for such keys.
    if (std::optional<FlowOpenPart> part = unnamedKeyPartOf(classOf(solution.program, key))) {
        FlowNodeID unnamedKeys = solution.graph.find(unknownFlowObject, *part);
        if (unnamedKeys && solution.records.recordOf(unnamedKeys).events.contains(FlowLocationEvent::Redefined))
            aliased.events.add(FlowLocationEvent::Redefined);
    }

    const FlowProperty* property = solution.graph.findProperty(unknownFlowObject, key);
    if (!property)
        return aliased;

    if (property->value) {
        const FlowLocationRecord& record = solution.records.recordOf(property->value);
        aliased.events.add(record.events);
        aliased.firstStore = record.firstStore;
        aliased.firstLoss = record.firstLoss;
    }
    if (hasAccessor(property))
        aliased.events.add(FlowLocationEvent::Redefined);
    if (aliased.events.containsAny(storeEvents))
        aliased.events.add(overwrittenLocationEvents);
    return aliased;
}

OptionSet<FlowLocationEvent> aliasedEventsOf(const FlowSolution& solution, FlowObjectID object, PropertyKeyID key)
{
    return aliasedRecordOf(solution, object, key).events;
}

template<typename Flag>
static Witness findWitness(const FlowSolution& solution, FlowObjectID object, Flag flag)
{
    Witness witness = solution.records.witnessOf(object, flag);
    if (!hasFunction(witness) && sharesLocationsWithUnknownObject(FlowWalkInputs::of(solution), object))
        return solution.records.witnessOf(unknownFlowObject, flag);
    return witness;
}

bool isTypedLocation(const FlowGraph& graph, FlowNodeID location)
{
    TypeID type = enforcedTypeOf(graph, location);
    return type && !graph.program().at(type).isAnything();
}

// The solver keeps what the type of a field refuses out of the locations that it knows for fields, and it looks for an inherited field along the predicted bases.
// The results follow the bases that flow has proven: what an object inherits from a base that nobody predicted, as the class of a mixin does, was an added property without a type during the solve.
bool holdsValueOutsideFieldType(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject, FlowNodeID location)
{
    if (!subject.declaredIndex)
        return false;

    const Program& program = inputs.program();
    TypeID declared = program.at(subject.layout).fields[*subject.declaredIndex].declared.type;
    if (!declared || program.at(declared).isAnything() || enforcedTypeOf(inputs.graph(), location) == declared)
        return false;
    return !inputs.solution.values().isInside(inputs.graph().valueOf(location), program.at(declared));
}

// The node of the unknown object under a name holds what the stores through unknown receivers put under that name, and nothing else.
static bool mayStoreOutsideTypeThroughUnknownObject(const FlowSolution& solution, PropertyKeyID key, FlowNodeID typedLocation)
{
    const FlowGraph& graph = solution.graph;
    const FlowProperty* alias = graph.findProperty(unknownFlowObject, key);
    if (!alias || !alias->value)
        return false;
    return !solution.values().isInside(graph.valueOf(alias->value), graph.program().at(enforcedTypeOf(graph, typedLocation)));
}

static void addUntrackedAccesses(const FlowFamilyInputs& inputs, FlowObjectID object, FlowLocationFacts& facts)
{
    const FlowObjectFacts& objectFacts = inputs.factsOf(object);
    if (objectFacts.events.contains(FlowObjectEvent::DeletedByUnknownKey)) {
        facts.events.add(FlowLocationEvent::Deleted);
        keepLowerWitness(facts.firstLoss, findWitness(inputs.solution, object, FlowObjectEvent::DeletedByUnknownKey));
    }
    if (objectFacts.events.contains(FlowObjectEvent::StoredByUnknownKey))
        facts.events.add(overwrittenLocationEvents);
    if (objectFacts.uses.containsAny(unknownKeyReadUses))
        facts.events.add(FlowLocationEvent::ReadAsValue);
    if (objectFacts.mutations.contains(ObjectMutation::ReachNativeCode))
        facts.events.add(FlowLocationEvent::ReadByUnknownCode);
    if (objectFacts.mutations.contains(ObjectMutation::ReachUnknownJavaScript)) {
        facts.events.add(unknownJavaScriptEvents);
        keepLowerWitness(facts.firstLoss, findWitness(inputs.solution, object, ObjectMutation::ReachUnknownJavaScript));
    }
}

static bool mayGetKeyUnseen(const FlowFamilyInputs& inputs, FlowObjectID object, PropertyKeyID key)
{
    FlowKeyClass keyClass = classOf(inputs.program(), key);
    if (!unnamedKeyPartOf(keyClass))
        return false;

    const FlowObjectFacts& facts = inputs.factsOf(object);
    bool mayBeStoredUnderKey = keyClass == FlowKeyClass::Index ? facts.mayGetIndexKey : facts.mayGetUnlistedKey;
    return mayBeStoredUnderKey || facts.mutations.contains(ObjectMutation::ReachUnknownJavaScript);
}

// What such a store brings is in no location that the type of a field is made from.
bool mayBeStoredWithoutLocation(const FlowFamilyInputs& inputs, FlowObjectID object, PropertyKeyID key)
{
    return aliasedEventsOf(inputs.solution, object, key).containsAny(storeEvents) || mayGetKeyUnseen(inputs, object, key);
}

// After the application the object has a slot for an appended or inherited field, though no operation that flow follows stores under the key.
// The slot stays empty: the store of a base constructor that runs a setter of the object's own class fills nothing, and every lookup passes the slot.
static void addFactsOfObjectWithoutNode(const FlowFamilyInputs& inputs, PropertyKeyID key, FlowObjectID object, FlowLocationFacts& facts)
{
    facts.events.add(FlowLocationEvent::MissingAtAllocation);
    if (hasAccessor(inputs.graph().findProperty(object, key))) {
        facts.events.add(FlowLocationEvent::Redefined);
        keepLowerWitness(facts.firstLoss, findWitness(inputs.solution, object, ObjectMutation::RedefineProperty));
    }

    FlowLocationRecord aliased = aliasedRecordOf(inputs.solution, object, key);
    facts.events.add(aliased.events);
    keepLowerWitness(facts.firstLoss, aliased.firstLoss);
    if (aliased.events.containsAny(storeEvents))
        keepLowerWitness(facts.firstLoss, aliased.firstStore);

    if (!mayBeStoredWithoutLocation(inputs, object, key))
        return;
    addUntrackedAccesses(inputs, object, facts);
    facts.events.add(FlowLocationEvent::StoredOutsideType);
}

static void addFactsOfObjectsWithoutNode(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject, FlowLocationFacts& facts)
{
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout)) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout)) {
            if (!fieldNodeOn(inputs.graph(), object, subject))
                addFactsOfObjectWithoutNode(inputs, subject.key, object, facts);
        }
    }
}

FlowLocationFacts collectLocationFacts(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject)
{
    const Program& program = inputs.program();
    const FlowGraph& graph = inputs.graph();
    bool isNamedProperty = subject.key && !program.at(subject.layout).isEnvironment();
    bool mayBeNamedByUnknownKey = isNamedProperty && unnamedKeyPartOf(classOf(program, subject.key)).has_value();

    FlowLocationFacts facts;
    if (isNamedProperty)
        addFactsOfObjectsWithoutNode(inputs, subject, facts);
    forEachFieldNode(inputs, subject, [&](FlowObjectID object, FlowNodeID node) {
        facts.add(inputs.records().recordOf(node));
        if (!isNamedProperty)
            return;

        if (holdsValueOutsideFieldType(inputs, subject, node)) {
            facts.events.add(FlowLocationEvent::StoredOutsideType);
            keepLowerWitness(facts.firstLoss, inputs.records().recordOf(node).firstStore);
        }

        FlowLocationRecord aliased = aliasedRecordOf(inputs.solution, object, subject.key);
        facts.add(aliased);
        OptionSet<FlowObjectEvent> unknownKeyStores;
        if (mayBeNamedByUnknownKey) {
            addUntrackedAccesses(inputs, object, facts);
            unknownKeyStores = inputs.factsOf(object).events & FlowObjectEvent::StoredByUnknownKey;
        }
        if (isTypedLocation(graph, node)) {
            if (aliased.events.containsAny(storeEvents) && mayStoreOutsideTypeThroughUnknownObject(inputs.solution, subject.key, node)) {
                facts.events.add(FlowLocationEvent::StoredOutsideType);
                keepLowerWitness(facts.firstLoss, aliased.firstStore);
            }
            for (FlowObjectEvent store : unknownKeyStores) {
                facts.events.add(FlowLocationEvent::StoredOutsideType);
                keepLowerWitness(facts.firstLoss, findWitness(inputs.solution, object, store));
            }
        }
        if (hasAccessor(graph.findProperty(object, subject.key))) {
            facts.events.add(FlowLocationEvent::Redefined);
            keepLowerWitness(facts.firstLoss, findWitness(inputs.solution, object, ObjectMutation::RedefineProperty));
        }
    });
    return facts;
}

// The realm keeps its built-in objects alive, and the registry of modules its namespace objects.
static bool livesAsLongAsRealm(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return true;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The nodes of an instance of one of these classes name everything that it refers to: its properties and the slots that the table declares. An instance of any other built-in class
// may hold more: the frame of a generator, the reactions of a promise, the buffer of a typed array, the cause of an error, a resource or a finalizer of the host.
// A registry is not among them for another reason: once it is collected its callback is not called any more.
static constexpr BuiltinClassID classesReferringThroughNodesOnly[] {
    BuiltinClasses::Array,
    BuiltinClasses::ArrayBuffer,
    BuiltinClasses::ArrayIterator,
    BuiltinClasses::BigInt,
    BuiltinClasses::Boolean,
    BuiltinClasses::BoundFunction,
    BuiltinClasses::DataView,
    BuiltinClasses::Date,
    BuiltinClasses::Map,
    BuiltinClasses::MapIterator,
    BuiltinClasses::Number,
    BuiltinClasses::Object,
    BuiltinClasses::RegExp,
    BuiltinClasses::Set,
    BuiltinClasses::SetIterator,
    BuiltinClasses::SharedArrayBuffer,
    BuiltinClasses::String,
    BuiltinClasses::StringIterator,
    BuiltinClasses::Symbol,
    BuiltinClasses::WeakMap,
    BuiltinClasses::WeakRef,
    BuiltinClasses::WeakSet,
};

static bool mayReferToMoreThanItsNodes(const FlowSolution& solution, FlowObjectID object)
{
    const FlowObjectTable& objects = solution.objects;
    const FlowObject& record = objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Opaque:
        return true;
    case FlowObjectKind::Instance:
        return !std::ranges::contains(classesReferringThroughNodesOnly, record.builtinClass);
    case FlowObjectKind::Allocation:
        // An instance of a class of the program that extends a built-in class has the fields of its layout for nodes and none of the slots.
        return objects.isMadeByBuiltinConstructor(object);
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool refersToObjectIn(const FlowSolution& solution, FlowObjectID object, const FlowObjectSet& objects)
{
    const FlowGraph& graph = solution.graph;
    bool refers = false;
    graph.forEachNodeOf(object, [&](FlowNodeID node) {
        refers = refers || graph.valueOf(node).objects.overlaps(objects);
    });
    if (refers)
        return true;

    FlowPrototypeSet prototypes = prototypesOf(FlowWalkInputs::of(solution), holderOf(solution.objects, object), { });
    if (!prototypes.isNeverUnknown && objects.containsUnknownObject())
        return true;
    return std::ranges::any_of(prototypes.holders, [&](const FlowHolder& holder) {
        return holder.object && objects.contains(holder.object);
    });
}

void forEachObjectReferredTo(const FlowSolution& solution, FlowObjectID object, NOESCAPE const ScopedLambda<void(FlowObjectID)>& functor)
{
    const FlowGraph& graph = solution.graph;
    auto visit = [&](FlowObjectID referent) {
        if (!livesAsLongAsRealm(solution.objects.kindOf(referent)))
            functor(referent);
    };
    graph.forEachNodeOf(object, [&](FlowNodeID node) {
        graph.valueOf(node).objects.forEach(visit);
    });
    FlowPrototypeSet prototypes = prototypesOf(FlowWalkInputs::of(solution), holderOf(solution.objects, object), { });
    if (!prototypes.isNeverUnknown)
        functor(unknownFlowObject);
    for (const FlowHolder& holder : prototypes.holders) {
        if (holder.object)
            visit(holder.object);
    }
}

// Whether a weak reference may be made of whatever has reached native code: by code that the build has not seen and that reaches all of it, or by a call that flow has not seen of one of the two built-ins that take a target.
static bool mayMakeWeakReferencesUnseen(const FlowSolution& solution)
{
    const BuiltinTable& builtins = solution.builtins();
    const FlowRecords& records = solution.records;
    // What a function does that the solver has given up on, nobody has seen.
    if (records.mayRunUnknownJavaScript() || !records.functionsPastWorkLimit().isEmpty())
        return true;

    BuiltinID takingTargets[] { builtins.canonicalID(Builtins::WeakRef), builtins.canonicalID(Builtins::FinalizationRegistryPrototypeRegister) };
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        if (records.builtinUse(builtin).uses.contains(BuiltinUse::Escaped) && std::ranges::contains(takingTargets, builtins.canonicalID(builtin)))
            return true;
    }
    return false;
}

FlowObservedLifetimes computeObservedLifetimes(const FlowSolution& solution)
{
    const FlowGraph& graph = solution.graph;
    const FlowObjectTable& objects = solution.objects;
    const ParallelLoops& loops = solution.build.parallelLoops();
    unsigned numberOfObjects = objects.size();
    bool targetsIncludeUnknownObject = false;

    FlowObservedLifetimes lifetimes;
    FlowObjectSet& observed = lifetimes.observed;
    for (unsigned index = 0; index < numberOfObjects; ++index) {
        FlowObjectID object(index);
        if (mayReferToMoreThanItsNodes(solution, object))
            observed.add(object);
        for (BuiltinSlotID slot : { BuiltinSlots::WeakRefTarget, BuiltinSlots::FinalizationRegistryTarget }) {
            FlowNodeID targets = graph.slot(object, slot);
            if (!targets)
                continue;
            const FlowValue& value = graph.valueOf(targets);
            observed.unite(value.objects);
            targetsIncludeUnknownObject |= value.hasUnknownObject();
            lifetimes.includesSymbols |= !!(value.primitives & Primitive::AnySymbol);
        }
    }
    // The unknown object among the targets stands for whatever has reached native code.
    if (mayMakeWeakReferencesUnseen(solution) || targetsIncludeUnknownObject)
        observed.unite(solution.objectsInNativeCode());
    // Code that the build has not seen makes weak references of what it is handed.
    observed.unite(solution.objectsInUnknownJavaScript());
    for (unsigned index = 0; index < numberOfObjects; ++index) {
        if (livesAsLongAsRealm(objects.kindOf(FlowObjectID(index))))
            observed.remove(FlowObjectID(index));
    }

    // Each round adds the objects that refer to one that the round before has added.
    FlowObjectSet& keepingAlive = lifetimes.keepingObservedAlive;
    keepingAlive = observed;
    FlowObjectSet lastAdded = observed;
    while (!lastAdded.isEmpty()) {
        Vector<uint32_t> added = findIndicesInParallel(loops, numberOfObjects, [&](unsigned index) {
            FlowObjectID object(index);
            return !keepingAlive.contains(object) && !livesAsLongAsRealm(objects.kindOf(object)) && refersToObjectIn(solution, object, lastAdded);
        });
        lastAdded.clear();
        for (uint32_t index : added)
            lastAdded.add(FlowObjectID(index));
        keepingAlive.unite(lastAdded);
    }
    return lifetimes;
}

bool mayKeepObservedObjectAlive(const FlowFamilyInputs& inputs, FlowNodeID location)
{
    const FlowValue& value = inputs.graph().valueOf(location);
    if (inputs.observedLifetimes.includesSymbols && (value.primitives & Primitive::AnySymbol))
        return true;
    return value.objects.overlaps(inputs.observedLifetimes.keepingObservedAlive);
}

ReadState readStateOf(OptionSet<FlowLocationEvent> events)
{
    constexpr OptionSet<FlowLocationEvent> otherReads { FlowLocationEvent::ReadAsValue, FlowLocationEvent::ReadByUnknownCode, FlowLocationEvent::PresenceObserved };
    if (events.containsAny(otherReads))
        return ReadState::Unknown;
    return events.contains(FlowLocationEvent::ReadAsCallee) ? ReadState::OnlyCalled : ReadState::NeverRead;
}

bool isStoredByOperation(OptionSet<FlowLocationEvent> events)
{
    return events.containsAny({ FlowLocationEvent::StoredByOperation, FlowLocationEvent::StoredByManyOperations });
}

UnprovenCause causeOf(ObjectMutation mutation)
{
    switch (mutation) {
    case ObjectMutation::ReachNativeCode:
        return UnprovenCause::ReachesNativeCode;
    case ObjectMutation::ReachUnknownJavaScript:
        return UnprovenCause::ReachesUnknownJavaScript;
    case ObjectMutation::DeleteField:
        return UnprovenCause::FieldDeleted;
    case ObjectMutation::RedefineProperty:
        return UnprovenCause::PropertyRedefined;
    case ObjectMutation::AddProperty:
        return UnprovenCause::PropertyAdded;
    case ObjectMutation::ChangeIntegrity:
        return UnprovenCause::IntegrityChanged;
    case ObjectMutation::ChangePrototype:
    case ObjectMutation::BecomePrototype:
        return UnprovenCause::PrototypeChanged;
    case ObjectMutation::StoreAfterCreation:
        return UnprovenCause::UnknownWriter;
    case ObjectMutation::StoreOfOtherType:
        return UnprovenCause::StoreOfOtherType;
    case ObjectMutation::LoseShape:
        return UnprovenCause::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

UnprovenCause causeOf(FlowObjectEvent event)
{
    switch (event) {
    case FlowObjectEvent::StoredByUnknownKey:
    case FlowObjectEvent::StoredByComputedKey:
    case FlowObjectEvent::CopiedUnderUnknownKeys:
        return UnprovenCause::UnknownKeyStore;
    case FlowObjectEvent::DeletedByUnknownKey:
        return UnprovenCause::UnknownKeyDelete;
    case FlowObjectEvent::OtherNewTarget:
        return UnprovenCause::OtherNewTarget;
    case FlowObjectEvent::Allocated:
    case FlowObjectEvent::AllocatedByConstructor:
    case FlowObjectEvent::AllocatedRepeatedly:
    case FlowObjectEvent::ElementsMoved:
    case FlowObjectEvent::ContentsStored:
    case FlowObjectEvent::StoredUnderNumericName:
    case FlowObjectEvent::HoleMade:
    case FlowObjectEvent::StoredWithinLength:
        return UnprovenCause::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void keepFirstLoss(FlowSiteLoss& current, const FlowSiteLoss& candidate)
{
    if (candidate.cause == UnprovenCause::Unknown)
        return;
    if (current.cause == UnprovenCause::Unknown || candidate.cause < current.cause) {
        current = candidate;
        return;
    }
    if (candidate.cause == current.cause && isLowerWitness(candidate.witness, current.witness))
        current = candidate;
}

Vector<FlowNodeID, 4> nodesOf(const FlowGraph& graph, const FlowFunctionVersion& version, FlowLocalID local)
{
    Vector<FlowNodeID, 4> nodes;
    if (!local)
        return nodes;

    nodes.reserveInitialCapacity(version.instances.size());
    for (FlowInstanceID instance : version.instances)
        nodes.append(graph.nodeOf(instance, local));
    return nodes;
}

Vector<FlowNodeID, 4> nodesOf(const FlowGraph& graph, const FlowFunctionVersion& version, ValueID value)
{
    const Vector<FlowLocalID>& localOfValue = graph.summaryOf(version.body).localOfValue;
    if (value.index() >= localOfValue.size())
        return { };
    return nodesOf(graph, version, localOfValue[value.index()]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
