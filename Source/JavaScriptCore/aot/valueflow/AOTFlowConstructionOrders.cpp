/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperands.h"
#include "AOTForwardDataflow.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLayout.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// The object that a constructor makes is followed from its allocation through the constructor and through every function that takes it, up to the first place where somebody else may get hold of it.

namespace {

enum class ConstructionRootKind : uint8_t {
    EntryParameter,
    Allocation,
    SuperConstruct,
};

struct ConstructionRoot {
    ConstructionRootKind kind { ConstructionRootKind::EntryParameter };
    ValueID definition;

    friend bool operator==(const ConstructionRoot&, const ConstructionRoot&) = default;
};

struct ConstructionName {
    PropertyKeyID key;
    FieldReference field;
    // Beside the name itself: stored with something that may be another value than undefined and null.
    bool standsForValue { false };

    friend bool operator==(const ConstructionName&, const ConstructionName&) = default;
    friend auto operator<=>(const ConstructionName&, const ConstructionName&) = default;
};

using ConstructionNames = SortedSet<ConstructionName>;

enum class ConstructionEventKind : uint8_t {
    Leak,
    Store,
    Read,
    Call,
    Exit,
};

struct ConstructionEvent {
    ConstructionEventKind kind { ConstructionEventKind::Leak };
    bool isExact { false };
    bool storesSelfReference { false };
    ProgramPoint point;
    ValueID origin;
    ValueID operand;
    uint32_t operandIndex { 0 };
    std::optional<uint32_t> name;
    std::optional<uint32_t> nameOfValue;
    // The event is about the result of this call, which took the object. Whether the result holds the object is decided from the callees of the call.
    ValueID holder;

    friend bool operator==(const ConstructionEvent&, const ConstructionEvent&) = default;
};

struct ConstructionSummary {
    ConstructionRoot root;
    bool isTracked { false };
    Vector<ConstructionName> names;
    Vector<ConstructionEvent> events;

    std::span<const ConstructionEvent> eventsAt(ProgramPoint) const LIFETIME_BOUND;
    uint32_t indexOfFirstEventAt(ProgramPoint) const;
    std::optional<uint32_t> indexOf(const ConstructionName&) const;
};

struct FunctionConstruction {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionConstruction);
    WTF_MAKE_NONCOPYABLE(FunctionConstruction);

    FunctionConstruction(const AnalysisContext& context, FunctionID id, const Function& body)
        : function(id)
        , analyses(std::in_place, context, body)
    {
    }

    const ConstructionSummary* find(ValueID rootDefinition) const LIFETIME_BOUND;
    const ValueFlowStatement* findStatement(ValueID site) const { return find(statements, site); }

    FunctionID function;
    std::optional<FunctionAnalyses> analyses;
    Vector<ConstructionSummary> summaries;
    // The statements of the operations, by the operation.
    Vector<const ValueFlowStatement*> statements;
    bool returnsOnlyConstructedObject { false };

private:
    static const ValueFlowStatement* find(const Vector<const ValueFlowStatement*>& ascendingStatements, ValueID site)
    {
        auto found = std::ranges::lower_bound(ascendingStatements, site, { }, &ValueFlowStatement::site);
        return found != ascendingStatements.end() && (*found)->site == site ? *found : nullptr;
    }
};

struct SolvedConstruction {
    bool mayLeak { true };
    // Whatever creates a property of the object creates it in the initial order.
    bool storesOnlyInInitialOrder { false };
    ConstructionNames storedBeforeLeak;
    ConstructionNames storedAtExit;
    ConstructionNames readBeforeStore;
    ConstructionNames readNames;
    // Under these the object may hold itself, or a closure or an environment that holds it.
    ConstructionNames selfReferenceNames;
    Vector<ConstructionName> initialOrder;
};

struct ResolvedConstructionEvent {
    bool doesNotLeak { false };
    // No function that does not take the object, and no built-in, may run in place of the callees or of the store, and nothing that the object inherits stops the store.
    bool runsOneCallee { false };
    bool resultHoldsObject { false };
    bool holderIsEmpty { false };
    Vector<uint32_t, 1> callees;
};

struct ConstructionNode {
    FunctionID function;
    ConstructionRoot root;
    // The function is followed as the constructor function that `new` has entered with an instance of its own layout, and not as a function that somebody hands an object to as `this`.
    bool isEnteredByNew { false };
    FunctionConstruction* construction { nullptr };
    // None where nothing is known about the object in the function.
    const ConstructionSummary* summary { nullptr };
    Vector<ResolvedConstructionEvent> resolvedEvents;
    Vector<uint32_t> callees;
    SolvedConstruction solved;
};

struct ConstructionGraph {
    using NodeIndexes = HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    static uint64_t keyOf(FunctionID function, ValueID root, bool isEnteredByNew) { return static_cast<uint64_t>(isEnteredByNew) << 63 | static_cast<uint64_t>(function.index()) << 32 | root.index(); }
    std::optional<uint32_t> find(FunctionID function, ValueID root, bool isEnteredByNew) const
    {
        auto found = nodeIndexes.find(keyOf(function, root, isEnteredByNew));
        return found == nodeIndexes.end() ? std::nullopt : std::optional(found->value);
    }

    Vector<ConstructionNode> nodes;
    NodeIndexes nodeIndexes;
    Vector<uint32_t> constructorNodes;
};

constexpr OperandUses confinedRetentionUses { OperandUse::Inspects, OperandUse::Retains };
// Whether an accessor takes the stored value is decided from the accessors that the store finds.
constexpr OperandUses namedStoreUses = confinedRetentionUses | OperandUses { OperandUse::PassesToUserCode };
// A store by name is an event of its own. Any other write may create a property under any name.
constexpr OperandUses unnamedWritingUses = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };
constexpr OperandUses contentsLeakingUses = callingUses | OperandUses { OperandUse::RetainsContents, OperandUse::AppliesTransitively, OperandUse::AppliesToContents };
constexpr Protocols enumeratingProtocols { Protocol::OwnKeys, Protocol::OwnValues };

} // anonymous namespace

static std::pair<BlockID, uint32_t> orderOfPoint(ProgramPoint point)
{
    return { point.block, point.index };
}

// What leaks through another operand of a call leaks when the call is entered, before anything the callee stores.
static unsigned orderWithinPoint(ConstructionEventKind kind)
{
    switch (kind) {
    case ConstructionEventKind::Store:
        return 0;
    case ConstructionEventKind::Read:
        return 1;
    case ConstructionEventKind::Leak:
        return 2;
    case ConstructionEventKind::Call:
        return 3;
    case ConstructionEventKind::Exit:
        return 4;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static auto orderOfEvent(const ConstructionEvent& event)
{
    return std::tuple { event.point.block, event.point.index, orderWithinPoint(event.kind), event.operandIndex, event.name, event.operand };
}

static bool isCall(Opcode opcode)
{
    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return true;
    case AccessClass::None:
    case AccessClass::Slot:
    case AccessClass::ByName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isExit(TerminatorKind kind)
{
    return kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall;
}

static bool isPrivateName(const Program& program, PropertyKeyID key)
{
    PropertyKeyKind kind = program.at(key).kind;
    return kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::EnginePrivateName;
}

static std::optional<ConstructionEventKind> accessEventOf(Opcode opcode, uint32_t operandIndex)
{
    if (receiverOf(opcode) != operandIndex)
        return std::nullopt;
    switch (accessOf(opcode)) {
    case Access::Store:
    case Access::Define:
        return ConstructionEventKind::Store;
    case Access::Read:
    case Access::Has:
        return ConstructionEventKind::Read;
    case Access::None:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ConstructionName fieldName(const Program& program, FieldReference reference)
{
    const Field& field = fieldAt(program, reference);
    if (field.key)
        return { field.key, { } };
    return { { }, declarationOf(program, reference) };
}

std::span<const ConstructionEvent> ConstructionSummary::eventsAt(ProgramPoint point) const
{
    auto found = std::ranges::equal_range(events, orderOfPoint(point), { }, [](const ConstructionEvent& event) {
        return orderOfPoint(event.point);
    });
    return events.subspan(found.begin() - events.begin(), found.size());
}

uint32_t ConstructionSummary::indexOfFirstEventAt(ProgramPoint point) const
{
    return static_cast<uint32_t>(eventsAt(point).data() - events.span().data());
}

std::optional<uint32_t> ConstructionSummary::indexOf(const ConstructionName& name) const
{
    auto found = std::ranges::lower_bound(names, name);
    if (found == names.end() || *found != name)
        return std::nullopt;
    return static_cast<uint32_t>(found - names.begin());
}

const ConstructionSummary* FunctionConstruction::find(ValueID rootDefinition) const
{
    auto found = std::ranges::lower_bound(summaries, rootDefinition, { }, [](const ConstructionSummary& summary) {
        return summary.root.definition;
    });
    if (found == summaries.end() || found->root.definition != rootDefinition)
        return nullptr;
    return &*found;
}

static bool allocatesConstructedObject(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::NewObject && function.kind == FunctionKind::BaseConstructor && program.at(value.layout()).kind == LayoutKind::Instance;
}

// AllocateTaggedLayouts puts one allocation for each tag where the one allocation of a constructor stood: in the constructor of a class, and in the copy of a constructor function that allocates `this` itself.
static bool allocatesObjectOfOwnClass(const Program& program, const Function& function, const Value& value)
{
    if (!allocatesConstructedObject(program, function, value) && !(value.opcode == Opcode::NewObject && function.has(FunctionFlags::AllocatesOwnThis)))
        return false;
    return function.constructedClass && declaredLayoutOf(program, value.layout()) == function.constructedClass;
}

static bool isConstructorFunctionOfOwnLayout(const Program& program, FunctionID id)
{
    LayoutID constructed = program.at(id).constructedClass;
    return constructed && program.at(constructed).constructor == id && program.at(constructed).flags.contains(LayoutFlags::IsConstructorFunction) && program.at(id).thisParameterIndex();
}

static Vector<ConstructionRoot, 2> constructorRootsOf(const Program& program, const Function& function)
{
    Vector<ConstructionRoot, 2> roots;
    if (!function.hasBody() || function.isUnsplitCoroutine() || function.constructorForm() == ConstructorForm::NotConstructor)
        return roots;
    if (std::optional<unsigned> index = function.thisParameterIndex()) {
        roots.append({ ConstructionRootKind::EntryParameter, function.at(entryBlock).parameters[*index] });
        return roots;
    }
    if (function.kind == FunctionKind::Plain)
        return roots;

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (allocatesConstructedObject(program, function, value))
                roots.append({ ConstructionRootKind::Allocation, id });
            else if (OperandPosition::parent(value.opcode))
                roots.append({ ConstructionRootKind::SuperConstruct, id });
        }
    }
    return roots;
}

// A constructor returns its completion value if that is an object, and else the constructed object: a merge point of the two.
static bool isConstructedObject(const Program& program, FunctionAnalyses& analyses, std::span<const ConstructionRoot> constructorRoots, ValueID result)
{
    const Function& constructor = analyses.function();
    BitVector isVisited(constructor.values.size());
    Vector<ValueID, 4> valuesToVisit { result };
    while (!valuesToVisit.isEmpty()) {
        ValueID value = valuesToVisit.takeLast();
        if (isVisited.set(value.index()) || !program.typeOf(constructor, value).hasObjects())
            continue;

        ValueID object = sameObjectThroughCasts(constructor, value);
        bool isRoot = std::ranges::any_of(constructorRoots, [&](const ConstructionRoot& root) {
            return root.definition == object;
        });
        if (isRoot)
            continue;

        ValueLocation location = analyses.useLists().locationOf(object);
        if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
            return false;
        for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(constructor, edge, location.index);
            if (!passed)
                return false;
            valuesToVisit.append(passed);
        }
    }
    return true;
}

static bool returnsOnlyConstructedObject(const Program& program, FunctionAnalyses& analyses, std::span<const ConstructionRoot> constructorRoots)
{
    return std::ranges::all_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
        const Terminator& terminator = analyses.function().at(block).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return false;
        if (terminator.kind != TerminatorKind::Return)
            return true;

        ValueID result = terminator.valueOperand();
        return !result || isConstructedObject(program, analyses, constructorRoots, result);
    });
}

static ConstructionName constructionNameOf(const Program& program, const Value& access)
{
    RELEASE_ASSERT(accessOf(access.opcode) != Access::None);
    if (Value::opcodesWithField.contains(access.opcode))
        return fieldName(program, { access.layout(), access.field() });
    RELEASE_ASSERT(Value::opcodesWithKey.contains(access.opcode));
    return { access.key(), { } };
}

namespace {

class ConstructionEventCollector {
public:
    ConstructionEventCollector(const Program& program, FunctionAnalyses& analyses, ConstructionRoot root)
        : m_program(program)
        , m_analyses(analyses)
        , m_function(analyses.function())
        , m_escape(analyses.escape())
        , m_useLists(analyses.useLists())
        , m_root(root)
    {
    }

    ConstructionSummary collect()
    {
        ConstructionSummary summary { m_root, false, { }, { } };
        if (m_escape.gaveUp())
            return summary;
        std::optional<ObjectID> object = m_escape.objectDefinedBy(m_root.definition);
        if (!object)
            return summary;

        findContainers(*object);
        findCallsTaking(*object);
        addDefinitionEvents();
        for (const ObjectUse& objectUse : m_escape.usesOf(*object)) {
            if (!isReadUnderNameOf(*object, usedValueOf(m_function, objectUse.use)))
                addUse(*object, objectUse);
        }
        for (ObjectID container : m_containers) {
            for (const ObjectUse& objectUse : m_escape.usesOf(container)) {
                if (!isReadUnderNameOf(*object, usedValueOf(m_function, objectUse.use)))
                    addContainerUse(*object, objectUse);
            }
        }
        for (ValueID call : m_callsTakingObject)
            addUsesOfResult(*object, call);
        addExits();
        finish(summary);
        return summary;
    }

private:
    void addEvent(const ConstructionEvent& event, std::optional<ConstructionName> name = std::nullopt, bool storesValue = false)
    {
        m_events.append(event);
        m_nameOfEvent.append(name);
        m_storesValue.append(storesValue);
    }

    bool mayBeValue(ValueID stored) const
    {
        return !m_program.typeOf(m_function, stored).isUndefinedOrNull();
    }

    ConstructionEvent eventAt(ConstructionEventKind kind, const Use& use) const
    {
        return { .kind = kind, .point = m_useLists.pointOf(use), .origin = use.user, .operand = usedValueOf(m_function, use), .operandIndex = use.operandIndex };
    }

    ConstructionEvent eventAtDefinition(ConstructionEventKind kind) const
    {
        ValueLocation location = m_useLists.locationOf(m_root.definition);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        return { .kind = kind, .isExact = true, .point = { location.block, location.index }, .origin = m_root.definition, .operand = m_root.definition };
    }

    void addLeak(const Use& use) { addEvent(eventAt(ConstructionEventKind::Leak, use)); }

    void addContainersOf(ObjectID content, ObjectID object)
    {
        for (ObjectID container : m_escape.containersOf(content)) {
            if (container == object || m_escape.at(container).kind == ObjectKind::EntryParameter)
                continue;
            if (!m_isContainer.set(container.index()))
                m_containers.append(container);
        }
    }

    void findContainers(ObjectID object)
    {
        addContainersOf(object, object);
        for (size_t next = 0; next < m_containers.size(); ++next)
            addContainersOf(m_containers[next], object);
    }

    void findCallsTaking(ObjectID object)
    {
        Vector<ValueID> calls;
        for (const ObjectUse& objectUse : m_escape.usesOf(object)) {
            const Use& use = objectUse.use;
            if (use.kind == Use::Kind::Operand && isCall(m_function.at(use.user).opcode))
                calls.append(use.user);
        }
        m_callsTakingObject = SortedSet<ValueID>(calls.span());
    }

    ValueID holderOf(ValueID value) const
    {
        ValueID definition = sameObjectThroughCasts(m_function, value);
        return m_callsTakingObject.contains(definition) ? definition : ValueID();
    }

    void addDefinitionEvents()
    {
        switch (m_root.kind) {
        case ConstructionRootKind::EntryParameter:
            return;
        case ConstructionRootKind::Allocation: {
            const Value& allocation = m_function.at(m_root.definition);
            std::span<const uint32_t> fields = allocation.fields();
            std::span<const ValueID> values = allocation.variadicOperands();
            for (size_t index = 0; index < fields.size(); ++index)
                addEvent(eventAtDefinition(ConstructionEventKind::Store), fieldName(m_program, { allocation.layout(), fields[index] }), index < values.size() && mayBeValue(values[index]));
            return;
        }
        case ConstructionRootKind::SuperConstruct:
            addEvent(eventAtDefinition(ConstructionEventKind::Call));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool mayRunOtherCode(ValueID operation)
    {
        const EffectsOnEdges& effects = m_analyses.effectsOf(operation);
        return effects.normal.mayRunOtherCode() || effects.handler.mayRunOtherCode();
    }

    bool isRetainedByContainers(const ObjectUse& objectUse) const
    {
        if (!objectUse.retainedBy || !objectUse.uses.containsOnly(confinedRetentionUses))
            return false;
        PointsToSet retainers = m_escape.pointsTo(objectUse.retainedBy);
        bool isConfined = !retainers.mayBeUntracked && !retainers.objects.isEmpty();
        for (ObjectID object : retainers.objects)
            isConfined &= m_isContainer.get(object.index());
        return isConfined;
    }

    // The object stays reachable through itself alone, until somebody reads the name.
    bool isStoredUnderNameOf(ObjectID object, const ObjectUse& objectUse) const
    {
        const Use& use = objectUse.use;
        if (use.kind != Use::Kind::Operand || storedOf(m_function.at(use.user).opcode) != use.operandIndex)
            return false;
        return objectUse.retainedBy && objectUse.uses.containsOnly(namedStoreUses) && m_escape.singleObject(objectUse.retainedBy) == object;
    }

    // Such a value is the object or a container only where the name holds a self reference, and there the read is the leak.
    bool isReadUnderNameOf(ObjectID object, ValueID value) const
    {
        const Value& definition = m_function.at(sameObjectThroughCasts(m_function, value));
        if (accessOf(definition.opcode) != Access::Read)
            return false;
        return m_escape.singleObject(definition.operand(*receiverOf(definition.opcode))) == object;
    }

    bool mayHoldObject(ObjectID object, ValueID value) const
    {
        if (isReadUnderNameOf(object, value))
            return false;
        for (ObjectID member : m_escape.pointsTo(value).objects) {
            if (member == object || m_isContainer.get(member.index()))
                return true;
        }
        return false;
    }

    bool leaksThroughTerminator(TerminatorKind kind) const
    {
        switch (kind) {
        case TerminatorKind::Return:
            return m_root.kind == ConstructionRootKind::EntryParameter;
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
            return false;
        default:
            return true;
        }
    }

    void addAccess(ConstructionEventKind kind, ObjectID object, const Use& use)
    {
        const Value& access = m_function.at(use.user);
        ConstructionEvent event = eventAt(kind, use);
        event.isExact = m_escape.singleObject(usedValueOf(m_function, use)) == object || isObjectWhereverItIsAllocated(object, usedValueOf(m_function, use));
        bool storesValue = false;
        if (kind == ConstructionEventKind::Store) {
            ValueID stored = access.operand(*storedOf(access.opcode));
            storesValue = mayBeValue(stored);
            event.storesSelfReference = mayHoldObject(object, stored);
            if (!event.storesSelfReference) {
                event.holder = holderOf(stored);
                event.storesSelfReference = !!event.holder;
            }
        }
        addEvent(event, constructionNameOf(m_program, access), storesValue);
    }

    // The allocations by tag meet in one value.
    bool isObjectWhereverItIsAllocated(ObjectID object, ValueID value)
    {
        ValueLocation location = m_useLists.locationOf(sameObjectThroughCasts(m_function, value));
        if (location.placement != ValuePlacement::Parameter || location.block == entryBlock || m_analyses.isInCycle(location.block))
            return false;
        bool isOneOfThem = false;
        for (Edge edge : m_analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(m_function, edge, location.index);
            if (!passed || m_analyses.isInCycle(edge.from))
                return false;
            ValueID allocation = sameObjectThroughCasts(m_function, passed);
            std::optional<ObjectID> allocated = m_escape.objectDefinedBy(allocation);
            if (!allocated || m_function.at(allocation).opcode != Opcode::NewObject || m_useLists.locationOf(allocation).block != edge.from)
                return false;
            isOneOfThem |= *allocated == object;
        }
        return isOneOfThem;
    }

    void addCall(ObjectID object, const Use& use)
    {
        ConstructionEvent event = eventAt(ConstructionEventKind::Call, use);
        event.isExact = m_escape.isExactUse(object, use);
        addEvent(event);
    }

    void addOperandUse(ObjectID object, const ObjectUse& objectUse)
    {
        const Use& use = objectUse.use;
        Opcode opcode = m_function.at(use.user).opcode;
        if (isCast(opcode))
            return;
        if (std::optional<ConstructionEventKind> access = accessEventOf(opcode, use.operandIndex)) {
            // A store to a private name creates nothing and throws where the field is missing, so it observes the field as a read does.
            if (opcode == Opcode::PrivateSet)
                addAccess(ConstructionEventKind::Read, object, use);
            addAccess(*access, object, use);
            return;
        }
        // An enumeration sees which fields exist so far.
        if (protocolsOf(opcode, use.operandIndex).containsAny(enumeratingProtocols)) {
            addLeak(use);
            return;
        }
        if (isRetainedByContainers(objectUse))
            return;
        if (storedOf(opcode) == use.operandIndex) {
            if (objectUse.isEscape && !isStoredUnderNameOf(object, objectUse))
                addLeak(use);
            return;
        }
        if (isCall(opcode)) {
            addCall(object, use);
            return;
        }
        if (objectUse.isEscape || mayRunOtherCode(use.user) || objectUse.uses.containsAny(unnamedWritingUses))
            addLeak(use);
    }

    void addUse(ObjectID object, const ObjectUse& objectUse)
    {
        const Use& use = objectUse.use;
        switch (use.kind) {
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            if (objectUse.isEscape)
                addLeak(use);
            return;
        case Use::Kind::TerminatorOperand:
            if (leaksThroughTerminator(m_function.at(use.block).terminator.kind))
                addLeak(use);
            return;
        case Use::Kind::Operand:
            addOperandUse(object, objectUse);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool mayLeakContents(const ObjectUse& objectUse)
    {
        if (objectUse.uses.containsAny(contentsLeakingUses))
            return true;
        if (objectUse.use.kind != Use::Kind::Operand)
            return false;
        return isCall(m_function.at(objectUse.use.user).opcode) || mayRunOtherCode(objectUse.use.user);
    }

    void addContainerUse(ObjectID object, const ObjectUse& objectUse)
    {
        if (isRetainedByContainers(objectUse) || isStoredUnderNameOf(object, objectUse))
            return;
        if (objectUse.isEscape || mayLeakContents(objectUse))
            addLeak(objectUse.use);
    }

    bool isStoredUnderNameOf(ObjectID object, const Use& use) const
    {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = m_function.at(use.user);
        std::optional<uint32_t> receiver = receiverOf(user.opcode);
        if (storedOf(user.opcode) != use.operandIndex || !receiver || accessEventOf(user.opcode, *receiver) != ConstructionEventKind::Store)
            return false;
        return m_escape.singleObject(user.operand(*receiver)) == object;
    }

    void addUsesOfResult(ObjectID object, ValueID call)
    {
        Vector<ValueID, 2> values { call };
        while (!values.isEmpty()) {
            for (const Use& use : m_useLists.usesOf(values.takeLast())) {
                if (use.kind == Use::Kind::Operand && isCastOrConversion(m_function.at(use.user).opcode)) {
                    values.append(use.user);
                    continue;
                }
                if (isStoredUnderNameOf(object, use))
                    continue;
                ConstructionEvent event = eventAt(ConstructionEventKind::Leak, use);
                event.holder = call;
                addEvent(event);
            }
        }
    }

    bool handsConstructionToAllocatingCopy(BlockID block) const
    {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            return false;
        const Value& call = m_function.at(terminator.callOperand());
        if (call.opcode != Opcode::CallDirect || !call.isConstruction())
            return false;
        const Function& callee = m_program.at(call.function());
        return callee.has(FunctionFlags::AllocatesOwnThis) && callee.derivation.source == m_analyses.context().id;
    }

    void addExits()
    {
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (isExit(m_function.at(block).terminator.kind) && !handsConstructionToAllocatingCopy(block))
                addEvent({ .kind = ConstructionEventKind::Exit, .point = pointBeforeTerminatorOf(m_function, block) });
        }
    }

    void finish(ConstructionSummary& summary)
    {
        Vector<ConstructionName> usedNames;
        auto nameOfValue = [](ConstructionName name) {
            name.standsForValue = true;
            return name;
        };
        for (size_t index = 0; index < m_events.size(); ++index) {
            if (!m_nameOfEvent[index])
                continue;
            usedNames.append(*m_nameOfEvent[index]);
            if (m_storesValue[index])
                usedNames.append(nameOfValue(*m_nameOfEvent[index]));
        }
        summary.names = Vector<ConstructionName>(ConstructionNames(usedNames.span()).span());
        for (size_t index = 0; index < m_events.size(); ++index) {
            if (!m_nameOfEvent[index])
                continue;
            m_events[index].name = summary.indexOf(*m_nameOfEvent[index]);
            if (m_storesValue[index])
                m_events[index].nameOfValue = summary.indexOf(nameOfValue(*m_nameOfEvent[index]));
        }
        AOT::sort(m_events, { }, orderOfEvent);
        removeRepeatedElements(m_events);
        summary.events = WTF::move(m_events);
        summary.isTracked = true;
    }

    const Program& m_program;
    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const EscapeAnalysis& m_escape;
    const UseLists& m_useLists;
    ConstructionRoot m_root;
    Vector<ObjectID, 4> m_containers;
    BitVector m_isContainer;
    SortedSet<ValueID> m_callsTakingObject;
    Vector<ConstructionEvent> m_events;
    Vector<std::optional<ConstructionName>> m_nameOfEvent;
    Vector<bool> m_storesValue;
};

} // anonymous namespace

static std::unique_ptr<FunctionConstruction> buildFunctionConstruction(const ValueFlowResultInputs& inputs, FunctionID id)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    AnalysisContext context { inputs.build, program, nullptr, nullptr, id, CallResults::NotFollowed };
    auto result = makeUnique<FunctionConstruction>(context, id, function);
    if (!function.hasBody())
        return result;

    Vector<ConstructionRoot, 2> roots = constructorRootsOf(program, function);
    result->returnsOnlyConstructedObject = returnsOnlyConstructedObject(program, *result->analyses, roots.span());
    for (ValueID parameter : function.at(entryBlock).parameters) {
        if (!mayBeReference(program.typeOf(function, parameter)))
            continue;
        bool isRoot = roots.containsIf([&](const ConstructionRoot& root) {
            return root.definition == parameter;
        });
        if (!isRoot)
            roots.append({ ConstructionRootKind::EntryParameter, parameter });
    }

    result->summaries.reserveInitialCapacity(roots.size());
    for (const ConstructionRoot& root : roots)
        result->summaries.append(ConstructionEventCollector(program, *result->analyses, root).collect());
    AOT::sort(result->summaries, { }, [](const ConstructionSummary& summary) {
        return summary.root.definition;
    });
    // Whoever solves a node asks for the control flow alone. The effects, the escape analysis and the use lists of every function that takes an object under construction are gigabytes in a large program.
    result->analyses.emplace(context, function);

    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind == ValueFlowStatementKind::Operation)
            result->statements.append(&statement);
    }
    AOT::sort(result->statements, { }, &ValueFlowStatement::site);
    return result;
}

// The solve has followed the values through the function.
static bool isFollowedCode(const ValueFlowResultInputs& inputs, FunctionID function)
{
    return inputs.isReached(function) && inputs.program.at(function).hasBody();
}

// False where the construction may run code that nobody follows.
static bool appendConstructionBodies(const ValueFlowResultInputs& inputs, FunctionID constructor, Vector<FunctionID, 2>& bodies)
{
    if (!isFollowedCode(inputs, constructor))
        return false;
    bodies.append(constructor);
    return true;
}

static std::optional<BuiltinOperand> builtinOperandOf(const Value& operation, const ConstructionEvent& event)
{
    if (std::optional<uint32_t> argument = operation.variadicOperandIndex(event.operandIndex)) {
        if (!VariadicOperandPosition::arguments(operation.opcode) || *argument > BuiltinOperand::maxArgumentIndex)
            return std::nullopt;
        return BuiltinOperand::argument(*argument);
    }
    if (OperandPosition::thisValue(operation.opcode) == event.operandIndex)
        return BuiltinOperand::receiver();
    return std::nullopt;
}

namespace {

// A callback that takes the operand is a callee of the event. The event leaks where the records name none.
constexpr OperandUses confinedUses {
    OperandUse::Inspects,
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::PassesToUserCode,
};

// After one of these a holder may keep a store through an object below from creating a property.
constexpr OptionSet<ObjectMutation> mutationsStoppingStores { ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity, ObjectMutation::ReachUnknownJavaScript };

enum class BuiltinEffectOnObject : uint8_t {
    MayLeak,
    HeldByResult,
    Confined,
    NoBuiltin,
};

// The functions that an event enters.
struct EnteredFunctions {
    bool leaks { false };
    bool hasFunctionWithoutObject { false };
    bool hasDirectCallee { false };
};

// What lies above the receivers of an access.
struct ChainOfReceivers {
    ValueSetID chain;
    ValueSetID above;
    bool isKnown { false };
};

enum class UnknownChains : uint8_t {
    MayTakeObject,
    HoldNothingUnderName,
};

class ConstructionGraphBuilder {
public:
    ConstructionGraphBuilder(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, Vector<std::unique_ptr<FunctionConstruction>>& functions, UnknownChains unknownChains = UnknownChains::MayTakeObject)
        : m_inputs(inputs)
        , m_heap(heap)
        , m_program(inputs.program)
        , m_universe(inputs.universe())
        , m_sets(inputs.sets())
        , m_functions(functions)
        , m_unknownChains(unknownChains)
    {
    }

    ConstructionGraph build()
    {
        addConstructorNodes();
        uint32_t next = 0;
        while (next < m_graph.nodes.size()) {
            buildMissingFunctions(next);
            for (uint32_t end = m_graph.nodes.size(); next < end; ++next)
                resolveNode(next);
        }
        for (ConstructionNode& node : m_graph.nodes)
            collectCallees(node);
        return WTF::move(m_graph);
    }

    // For what an object of the layout inherits from when it is made.
    bool mayBeStoppedAboveObjectsOf(LayoutID layout, PropertyKeyID key)
    {
        const std::optional<ChainOfReceivers>& chain = chainOfObjectsBeforeTheyLeak(m_heap.maker().of(m_universe.instanceOf(layout)));
        return !chain || !chain->isKnown || mayBeStoppedAbove(*chain, key);
    }

private:
    bool isFollowed(FunctionID function) const { return isFollowedCode(m_inputs, function); }

    uint32_t addNode(FunctionID function, ConstructionRoot root, bool isEnteredByNew = false)
    {
        return m_graph.nodeIndexes.ensure(ConstructionGraph::keyOf(function, root.definition, isEnteredByNew), [&] {
            uint32_t index = m_graph.nodes.size();
            m_graph.nodes.append(ConstructionNode { function, root, isEnteredByNew });
            return index;
        }).iterator->value;
    }

    void addConstructorNodes()
    {
        Vector<uint32_t> constructors = findIndicesInParallel(m_inputs.loops(), m_program.functions.size(), [&](unsigned index) {
            return m_program.functions[index].constructedClass && isFollowed(FunctionID(index));
        });
        for (uint32_t index : constructors) {
            FunctionID function(index);
            for (const ConstructionRoot& root : constructorRootsOf(m_program, m_program.at(function)))
                m_graph.constructorNodes.append(addNode(function, root, isConstructorFunctionOfOwnLayout(m_program, function)));
        }
    }

    void buildMissingFunctions(uint32_t firstNode)
    {
        Vector<FunctionID> functions;
        for (uint32_t index = firstNode; index < m_graph.nodes.size(); ++index) {
            FunctionID function = m_graph.nodes[index].function;
            if (!m_functions[function.index()])
                functions.append(function);
        }
        SortedSet<FunctionID> missing(functions.span());
        forEachIndexInParallel(m_inputs.loops(), static_cast<unsigned>(missing.size()), [&](unsigned index) {
            m_functions[missing[index].index()] = buildFunctionConstruction(m_inputs, missing[index]);
        });
    }

    void resolveNode(uint32_t index)
    {
        FunctionID function = m_graph.nodes[index].function;
        ConstructionRoot root = m_graph.nodes[index].root;
        FunctionConstruction* construction = m_functions[function.index()].get();
        const ConstructionSummary* summary = construction->find(root.definition);
        m_graph.nodes[index].construction = construction;
        if (!summary || !summary->isTracked || !isFollowed(function))
            return;

        Vector<ResolvedConstructionEvent> resolvedEvents;
        resolvedEvents.reserveInitialCapacity(summary->events.size());
        bool isEnteredByNew = m_graph.nodes[index].isEnteredByNew;
        for (const ConstructionEvent& event : summary->events)
            resolvedEvents.append(resolveEvent(*construction, root, isEnteredByNew, event));
        resolveHolders(summary->events, resolvedEvents);

        ConstructionNode& node = m_graph.nodes[index];
        node.summary = summary;
        node.resolvedEvents = WTF::move(resolvedEvents);
    }

    static void resolveHolders(std::span<const ConstructionEvent> events, Vector<ResolvedConstructionEvent>& resolvedEvents)
    {
        Vector<ValueID, 2> holders;
        for (size_t index = 0; index < events.size(); ++index) {
            if (resolvedEvents[index].resultHoldsObject)
                holders.append(events[index].origin);
        }
        for (size_t index = 0; index < events.size(); ++index) {
            if (!events[index].holder || holders.contains(events[index].holder))
                continue;
            resolvedEvents[index].holderIsEmpty = true;
            if (events[index].kind == ConstructionEventKind::Leak)
                resolvedEvents[index].doesNotLeak = true;
        }
    }

    ResolvedConstructionEvent resolveEvent(const FunctionConstruction& construction, const ConstructionRoot& root, bool isEnteredByNew, const ConstructionEvent& event)
    {
        ResolvedConstructionEvent resolved;
        switch (event.kind) {
        case ConstructionEventKind::Leak:
            return resolved;
        case ConstructionEventKind::Exit:
            resolved.doesNotLeak = true;
            return resolved;
        case ConstructionEventKind::Store:
        case ConstructionEventKind::Read:
            resolveAccess(construction, isEnteredByNew, event, resolved);
            break;
        case ConstructionEventKind::Call:
            if (root.kind == ConstructionRootKind::SuperConstruct && event.origin == root.definition && event.operand == root.definition)
                resolveParentConstruction(construction, event, resolved);
            else
                resolveCall(construction, event, resolved);
            break;
        }
        AOT::sort(resolved.callees);
        removeRepeatedElements(resolved.callees);
        return resolved;
    }

    // What the operands of the operation hold. Nothing where no rule has run for the operation.
    Vector<ValueSetID, 4> operandsOf(const FunctionConstruction& construction, ValueID site)
    {
        Vector<ValueSetID, 4> values;
        const ValueFlowStatement* statement = construction.findStatement(site);
        if (!statement)
            return values;
        for (const ValueFlowOperand& operand : m_inputs.bodies().operandsOf(*statement))
            values.append(m_heap.valueOf(operand));
        return values;
    }

    bool holdsPlaceholder(ValueSetID set) const
    {
        return ValueSets::isComplement(set) || m_sets.contains(set, ValueFlowUniverse::unknownObject()) || m_sets.contains(set, ValueFlowUniverse::untrackedObject());
    }

    void resolveParentConstruction(const FunctionConstruction& construction, const ConstructionEvent& event, ResolvedConstructionEvent& resolved)
    {
        resolved.runsOneCallee = true;
        Vector<ValueSetID, 4> operands = operandsOf(construction, event.origin);
        if (operands.isEmpty())
            return;
        ValueSetID parents = operands[0];
        if (holdsPlaceholder(parents))
            return;

        bool leaks = false;
        bool hasBuiltinParent = false;
        m_sets.forEachMember(parents, [&](ValueFlowObjectID parent) {
            if (m_sets.contains(m_universe.builtinFunctions(), parent)) {
                hasBuiltinParent = true;
                return;
            }
            FunctionID function = m_universe.closureFunction(parent);
            if (!function) {
                // Whatever cannot be called makes the construction throw.
                leaks |= m_sets.contains(m_universe.otherCallables(), parent);
                return;
            }
            Vector<FunctionID, 2> bodies;
            if (!appendConstructionBodies(m_inputs, function, bodies))
                leaks = true;
            for (FunctionID body : bodies) {
                Vector<ConstructionRoot, 2> roots = constructorRootsOf(m_program, m_program.at(body));
                // super() binds whatever object the parent returns. One that existed before has leaked, and has its fields already.
                const FunctionConstruction* parentConstruction = m_functions[body.index()].get();
                if (roots.isEmpty() || !parentConstruction || !parentConstruction->returnsOnlyConstructedObject)
                    leaks = true;
                for (const ConstructionRoot& root : roots)
                    resolved.callees.append(addNode(body, root));
            }
        });
        if (hasBuiltinParent && !resolved.callees.isEmpty())
            leaks = true;
        resolved.doesNotLeak = !leaks;
    }

    const ChainOfReceivers& chainOf(ValueSetID receivers)
    {
        return m_chains.ensure(receivers.index(), [&] {
            ChainOfReceivers result;
            result.above = m_heap.chainOf(m_heap.prototypesOf(receivers));
            result.chain = m_heap.maker().unite(m_heap.maker().intersect(receivers, m_universe.objects()), result.above);
            result.isKnown = !holdsPlaceholder(result.chain);
            return result;
        }).iterator->value;
    }

    // A locked location holds what the table says, as in the lookup of the rules (HeapRule::chainFrom).
    ValueSetID accessorsUnder(ValueSetID holders, ValueFlowName underKey, ValueFlowNameKind underUnknownKey)
    {
        holders = m_heap.maker().intersect(holders, ValueSets::complement(m_universe.builtinTables().heap().under(PropertyKeyID(underKey.payload)).holdersWithLockedLocation));
        return m_heap.maker().unite(m_heap.writtenUnder(holders, underKey), m_heap.writtenUnder(holders, ValueFlowName::of(underUnknownKey)));
    }

    // What the table has under the key on one of the holders is a function, an object or a constant that a read just finds.
    bool builtinMembersArePlainValues(ValueSetID holders, PropertyKeyID key) const
    {
        const BuiltinTable& builtins = m_program.builtins();
        const ValueFlowMembersUnderKey& members = m_universe.builtinTables().heap().under(key);
        if (m_sets.meet(holders, members.holdersOfUndeclaredProperties))
            return false;
        bool arePlain = true;
        m_sets.forEachMember(m_heap.maker().intersect(holders, members.holders), [&](ValueFlowObjectID holder) {
            for (BuiltinID member : builtins.membersOf(m_universe.builtinOf(holder))) {
                if (m_program.keyOf(builtins.at(member).name) != key)
                    continue;
                BuiltinKind kind = builtins.at(member).kind;
                arePlain &= isObjectKind(kind) || kind == BuiltinKind::Constant;
            }
        });
        return arePlain;
    }

    // An accessor or a read-only property that the object inherits keeps a store from creating a property of the object: the store runs the setter, throws, or does nothing.
    bool mayBeStoppedAbove(const ChainOfReceivers& chain, PropertyKeyID key)
    {
        ValueSetID above = chain.above;
        if (accessorsUnder(above, ValueFlowName::getter(key), ValueFlowNameKind::GetterUnderUnknownKey) != ValueSets::nothing)
            return true;
        if (accessorsUnder(above, ValueFlowName::setter(key), ValueFlowNameKind::SetterUnderUnknownKey) != ValueSets::nothing)
            return true;

        const BuiltinTable& builtins = m_program.builtins();
        const ValueFlowMembersUnderKey& members = m_universe.builtinTables().heap().under(key);
        if (m_sets.meet(above, members.holdersOfUndeclaredProperties))
            return true;
        bool mayBeStopped = false;
        m_sets.forEachMember(above, [&](ValueFlowObjectID holder) {
            if (BuiltinID builtin = m_universe.builtinOf(holder)) {
                if (!m_sets.contains(members.holders, holder))
                    return;
                // The functions and the objects of the table are created writable, and stay so unless the run time locks the place.
                mayBeStopped |= m_sets.contains(members.holdersWithLockedLocation, holder);
                for (BuiltinID member : builtins.membersOf(builtin))
                    mayBeStopped |= m_program.keyOf(builtins.at(member).name) == key && !isObjectKind(builtins.at(member).kind);
                return;
            }
            mayBeStopped |= markedMutationsOf(m_inputs, holder).containsAny(mutationsStoppingStores);
        });
        return mayBeStopped;
    }

    void enterWithObjectAsThis(ValueSetID functions, ResolvedConstructionEvent& resolved, EnteredFunctions& entered)
    {
        if (holdsPlaceholder(functions)) {
            entered.leaks = true;
            return;
        }
        m_sets.forEachMember(functions, [&](ValueFlowObjectID callee) {
            FunctionID function = m_universe.closureFunction(callee);
            if (!function) {
                entered.leaks |= m_universe.isObject(callee);
                return;
            }
            enter(function, m_program.at(function).thisParameterIndex(), resolved, entered);
        });
    }

    // Without a parameter the function runs and does not get the object.
    void enter(FunctionID function, std::optional<uint32_t> entryParameter, ResolvedConstructionEvent& resolved, EnteredFunctions& entered)
    {
        const Function& callee = m_program.at(function);
        if (!entryParameter) {
            entered.hasFunctionWithoutObject = true;
            return;
        }
        if (!isFollowed(function) || callee.isUnsplitCoroutine() || *entryParameter >= callee.at(entryBlock).parameters.size()) {
            entered.leaks = true;
            return;
        }
        resolved.callees.append(addNode(function, { ConstructionRootKind::EntryParameter, callee.at(entryBlock).parameters[*entryParameter] }));
    }

    // What the object inherits from when it is made: what its allocation is given, and for the instance of a constructor function what the function has as its prototype property.
    std::optional<ValueSetID> prototypesGivenAtCreation(ValueFlowObjectID object)
    {
        switch (m_universe.kindOf(object)) {
        case ValueFlowObjectKind::Instance: {
            LayoutID layout = m_universe.layoutOf(object);
            FunctionID constructor = m_program.at(layout).constructor;
            if (!constructor)
                return std::nullopt;
            ValueFlowObjectID atCreation[] { m_universe.prototypeOf(layout), m_universe.objectOf(Builtins::ObjectPrototype) };
            ValueSetID closure = m_heap.maker().of(m_universe.closureOf(constructor));
            if (m_heap.writtenUnder(closure, ValueFlowName::of(ValueFlowNameKind::CopiedFrom)) != ValueSets::nothing)
                return std::nullopt;
            ValueSetID assigned = m_heap.maker().unite(m_heap.writtenUnder(closure, ValueFlowName::property(m_universe.builtinTables().heap().prototypeKey())), m_heap.writtenUnder(closure, ValueFlowName::of(ValueFlowNameKind::UnknownKey)));
            return m_heap.maker().unite(m_heap.maker().of(std::span<const ValueFlowObjectID>(atCreation)), assigned);
        }
        case ValueFlowObjectKind::Site: {
            const ValueFlowSite& site = *m_universe.siteOf(object);
            if (site.origin != ValueFlowSiteOrigin::Operation || site.ordinal)
                return std::nullopt;
            FunctionID function = m_universe.functionOf(object);
            const Value& allocation = m_program.at(function).at(site.value);
            if (allocation.opcode != Opcode::NewObject || !allocation.prototypeOperand())
                return std::nullopt;
            for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(function.index())) {
                if (statement.kind == ValueFlowStatementKind::Operation && statement.site == site.value)
                    return m_heap.valueOf(m_inputs.bodies().operandsOf(statement)[OperandIndex::NewObject::prototype]);
            }
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }

    // Until an object leaks, only the code that is followed here holds it: it inherits from what it was given when it was made, and no accessor lies in it but what that code has defined.
    const std::optional<ChainOfReceivers>& chainOfObjectsBeforeTheyLeak(ValueSetID receivers)
    {
        return m_chainsBeforeLeak.ensure(receivers.index(), [&]() -> std::optional<ChainOfReceivers> {
            ValueSetID objects = m_heap.maker().intersect(receivers, m_universe.objects());
            if (holdsPlaceholder(objects) || m_sets.isEmpty(objects) || m_sets.size(objects) > m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
                return std::nullopt;
            ValueSetID prototypes = ValueSets::nothing;
            bool areGiven = true;
            m_sets.forEachMember(objects, [&](ValueFlowObjectID object) {
                std::optional<ValueSetID> given = areGiven ? prototypesGivenAtCreation(object) : std::nullopt;
                areGiven = given.has_value();
                if (given)
                    prototypes = m_heap.maker().unite(prototypes, *given);
            });
            if (!areGiven)
                return std::nullopt;
            ChainOfReceivers result;
            result.above = m_heap.chainOf(prototypes);
            result.chain = m_heap.maker().unite(objects, result.above);
            result.isKnown = !holdsPlaceholder(result.above) && !m_sets.meet(result.above, objects);
            return result;
        }).iterator->value;
    }

    void resolveAccess(const FunctionConstruction& construction, bool isEnteredByNew, const ConstructionEvent& event, ResolvedConstructionEvent& resolved)
    {
        const Value& access = m_program.at(construction.function).at(event.origin);
        // A field that is named by its layout is found without a lookup, and an allocation fills the fields that it lists.
        if (access.opcode == Opcode::NewObject || Value::opcodesWithField.contains(access.opcode)) {
            resolved.doesNotLeak = true;
            resolved.runsOneCallee = true;
            return;
        }
        // A definition creates a property of the object itself and looks at nothing that the object inherits.
        if (accessOf(access.opcode) == Access::Define) {
            resolved.doesNotLeak = true;
            resolved.runsOneCallee = true;
            return;
        }

        Vector<ValueSetID, 4> operands = operandsOf(construction, event.origin);
        if (operands.isEmpty())
            return;
        PropertyKeyID key = access.key();
        std::optional<ChainOfReceivers> chainOfRoot;
        // What `new` hands a constructor function is an instance of the function's own layout, whoever has written the `new`. Whatever else the parameter holds came by a call, and is no object that is being made.
        if (event.isExact)
            chainOfRoot = chainOfObjectsBeforeTheyLeak(isEnteredByNew ? m_universe.objectsWith(m_program.at(construction.function).constructedClass) : operands[*receiverOf(access.opcode)]);
        const ChainOfReceivers& chain = chainOfRoot ? *chainOfRoot : chainOf(operands[*receiverOf(access.opcode)]);
        if (!chain.isKnown) {
            if (m_unknownChains == UnknownChains::HoldNothingUnderName && event.kind == ConstructionEventKind::Store && event.isExact && access.opcode == Opcode::PropSet) {
                resolved.doesNotLeak = true;
                resolved.runsOneCallee = true;
            }
            return;
        }
        auto accessorsOnChain = [&](ValueFlowName underKey, ValueFlowNameKind underUnknownKey) {
            if (!chainOfRoot)
                return accessorsUnder(chain.chain, underKey, underUnknownKey);
            ValueSetID own = accessorsUnder(m_heap.maker().intersect(chain.chain, ValueSets::complement(chain.above)), underKey, underUnknownKey);
            return m_heap.maker().unite(m_heap.maker().intersect(own, ValueSets::complement(m_universe.placeholdersAlone())), accessorsUnder(chain.above, underKey, underUnknownKey));
        };

        EnteredFunctions entered;
        switch (accessOf(access.opcode)) {
        case Access::Read:
            entered.leaks = !builtinMembersArePlainValues(chain.chain, key);
            enterWithObjectAsThis(accessorsOnChain(ValueFlowName::getter(key), ValueFlowNameKind::GetterUnderUnknownKey), resolved, entered);
            break;
        case Access::Store:
            entered.leaks = !builtinMembersArePlainValues(chain.chain, key);
            enterWithObjectAsThis(accessorsOnChain(ValueFlowName::setter(key), ValueFlowNameKind::SetterUnderUnknownKey), resolved, entered);
            break;
        case Access::Define:
        case Access::Has:
            break;
        case Access::None:
            RELEASE_ASSERT_NOT_REACHED();
        }

        bool mayBeStopped = event.kind == ConstructionEventKind::Store && event.isExact && access.opcode == Opcode::PropSet && !isPrivateName(m_program, key) && mayBeStoppedAbove(chain, key);
        resolved.doesNotLeak = !entered.leaks;
        resolved.runsOneCallee = !entered.hasFunctionWithoutObject && !mayBeStopped;
    }

    // The function finds the argument in `arguments` or in the array of its rest parameter.
    static bool collectsArgument(const Function& callee, uint32_t argument)
    {
        return callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }) || (callee.has(FunctionFlags::HasRestParameter) && argument >= callee.numberOfPositionalParameters());
    }

    // Where the function gets what a call by value passes at the operand. Nothing where it does not get it, and a leak where it gets it in a way that no parameter shows.
    void enterByValue(FunctionID function, const Value& call, uint32_t operandIndex, ResolvedConstructionEvent& resolved, EnteredFunctions& entered)
    {
        const Function& callee = m_program.at(function);
        std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
        if (argument && collectsArgument(callee, *argument)) {
            entered.leaks = true;
            return;
        }
        enter(function, entryParameterOf(callee, call, operandIndex), resolved, entered);
    }

    void resolveCall(const FunctionConstruction& construction, const ConstructionEvent& event, ResolvedConstructionEvent& resolved)
    {
        const Value& call = m_program.at(construction.function).at(event.origin);
        EnteredFunctions entered;
        if (accessClassOf(call.opcode) == AccessClass::DirectCall) {
            // A callee that the solve has not reached stands behind a test that never comes out that way.
            if (m_inputs.isReached(call.function())) {
                std::optional<uint32_t> entryParameter = entryParameterOf(m_program.at(call.function()), call, event.operandIndex);
                entered.leaks = !entryParameter;
                enter(call.function(), entryParameter, resolved, entered);
            }
            resolved.doesNotLeak = !entered.leaks;
            resolved.runsOneCallee = !entered.hasFunctionWithoutObject;
            return;
        }

        Vector<ValueSetID, 4> operands = operandsOf(construction, event.origin);
        if (operands.isEmpty())
            return;
        ValueSetID callees = operands[0];
        if (holdsPlaceholder(callees))
            return;

        Vector<BuiltinID, 1> builtinCallees;
        m_sets.forEachMember(callees, [&](ValueFlowObjectID callee) {
            if (FunctionID function = m_universe.closureFunction(callee)) {
                entered.hasDirectCallee = true;
                enterByValue(function, call, event.operandIndex, resolved, entered);
            } else if (m_sets.contains(m_universe.builtinFunctions(), callee))
                builtinCallees.append(m_universe.builtinOf(callee));
            else
                entered.leaks |= m_sets.contains(m_universe.otherCallables(), callee);
        });

        BuiltinEffectOnObject builtinEffect = resolveBuiltinCallees(call, event, operands.span(), builtinCallees.span(), resolved, entered);
        if (builtinEffect == BuiltinEffectOnObject::MayLeak)
            entered.leaks = true;
        resolved.resultHoldsObject = builtinEffect == BuiltinEffectOnObject::HeldByResult;

        bool builtinRunsCallees = !resolved.callees.isEmpty() && !entered.hasDirectCallee;
        bool otherCodeMayRun = entered.hasFunctionWithoutObject || (builtinEffect != BuiltinEffectOnObject::NoBuiltin && !builtinRunsCallees);
        resolved.runsOneCallee = !otherCodeMayRun;
        resolved.doesNotLeak = !entered.leaks;
    }

    static bool covers(const BuiltinPlace& place, BuiltinOperand position)
    {
        if (place.kind != BuiltinPlaceKind::Operand || !place.operand)
            return false;
        if (*place.operand == position)
            return true;
        return position.kind() == BuiltinOperand::Kind::Argument && place.operand->coversArgument(position.argumentIndex());
    }

    // None where the call spreads a list, whose elements no operand shows.
    std::optional<ValueSetID> valueAt(const Value& call, std::span<const ValueSetID> operands, BuiltinOperand position)
    {
        if (position.isReceiver())
            return operands[1];
        if (!VariadicOperandPosition::arguments(call.opcode))
            return std::nullopt;
        std::span<const ValueSetID> arguments = operands.subspan(2);
        ValueSetID value = ValueSets::nothing;
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (position.coversArgument(index))
                value = m_heap.maker().unite(value, arguments[index]);
        }
        return value;
    }

    // Function.prototype.call hands the object on as this, and a built-in that goes over a list hands it on as an argument.
    void enterCallbacksTaking(const BuiltinOverload& overload, BuiltinOperand position, const Value& call, std::span<const ValueSetID> operands, ResolvedConstructionEvent& resolved, EnteredFunctions& entered)
    {
        bool isHandedOn = false;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            bool isThis = covers(callback.thisValue, position);
            std::optional<uint32_t> argument;
            for (uint32_t index = 0; index < callback.arguments.size(); ++index) {
                if (!covers(callback.arguments[index], position))
                    continue;
                // A place that stands for all arguments from one on hands them on in their order.
                BuiltinOperand handedOn = *callback.arguments[index].operand;
                argument = index + (handedOn.kind() == BuiltinOperand::Kind::ArgumentsFrom ? position.argumentIndex() - handedOn.argumentIndex() : 0);
            }
            bool isTakenInOtherWays = covers(callback.spreadArguments, position) || covers(callback.newTarget, position) || callback.callee == position;
            if (!isThis && !argument && !isTakenInOtherWays)
                continue;

            isHandedOn = true;
            std::optional<ValueSetID> callees = valueAt(call, operands, callback.callee);
            if (isTakenInOtherWays || (isThis && argument) || callback.times != BuiltinCallbackTimes::Once || callback.constructs || !callees || holdsPlaceholder(*callees)) {
                entered.leaks = true;
                continue;
            }
            m_sets.forEachMember(*callees, [&](ValueFlowObjectID callee) {
                FunctionID function = m_universe.closureFunction(callee);
                if (!function) {
                    entered.leaks |= m_sets.contains(m_universe.callables(), callee);
                    return;
                }
                const Function& entry = m_program.at(function);
                if (isThis) {
                    enter(function, entry.thisParameterIndex(), resolved, entered);
                    return;
                }
                if (collectsArgument(entry, *argument)) {
                    entered.leaks = true;
                    return;
                }
                bool hasParameter = *argument < entry.numberOfPositionalParameters();
                enter(function, hasParameter ? std::optional<uint32_t>(entry.firstParameterIndex() + *argument) : std::nullopt, resolved, entered);
            });
        }
        if (!isHandedOn || !overload.flow.handlerCalls.empty())
            entered.leaks = true;
    }

    bool retainsOnlyInNewResult(const BuiltinOverload& overload, const OperandBehavior& behavior) const
    {
        if (behavior.retainedIn != RetainedIn::Result)
            return false;
        switch (m_program.builtins().at(overload.result.type).freshness) {
        case BuiltinFreshness::Unaliased:
        case BuiltinFreshness::Yes:
            return true;
        case BuiltinFreshness::No:
        case BuiltinFreshness::UnlessSpecies:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Which overload a call selects depends on its operands. Every overload of the mode counts.
    BuiltinEffectOnObject resolveBuiltinCallees(const Value& call, const ConstructionEvent& event, std::span<const ValueSetID> operands, std::span<const BuiltinID> builtinCallees, ResolvedConstructionEvent& resolved, EnteredFunctions& entered)
    {
        if (builtinCallees.empty())
            return BuiltinEffectOnObject::NoBuiltin;
        std::optional<BuiltinOperand> position = builtinOperandOf(call, event);
        if (builtinCallees.size() > 1 || !position)
            return BuiltinEffectOnObject::MayLeak;

        BuiltinCallMode mode = call.opcode == Opcode::Call || call.opcode == Opcode::CallList ? BuiltinCallMode::Call : BuiltinCallMode::Construct;
        bool isHeldByResult = false;
        OperandUses uses;
        for (const BuiltinOverload& overload : m_program.builtins().overloadsOf(builtinCallees[0])) {
            if (overload.mode != mode)
                continue;
            OperandBehavior behavior = declaredBehaviorOf(overload, *position);
            OperandUses declaredUses = behavior.protocols.isEmpty() ? behavior.uses : OperandUses::all();
            if (declaredUses.contains(OperandUse::Retains) && retainsOnlyInNewResult(overload, behavior)) {
                declaredUses.remove(OperandUse::Retains);
                isHeldByResult = true;
            }
            if (declaredUses.containsOnly(confinedUses) && declaredUses.contains(OperandUse::PassesToUserCode))
                enterCallbacksTaking(overload, *position, call, operands, resolved, entered);
            uses.add(declaredUses);
        }
        if (!uses.containsOnly(confinedUses))
            return BuiltinEffectOnObject::MayLeak;
        if (uses.contains(OperandUse::PassesToUserCode) && entered.hasFunctionWithoutObject)
            return BuiltinEffectOnObject::MayLeak;
        return isHeldByResult ? BuiltinEffectOnObject::HeldByResult : BuiltinEffectOnObject::Confined;
    }

    static void collectCallees(ConstructionNode& node)
    {
        for (const ResolvedConstructionEvent& event : node.resolvedEvents)
            node.callees.appendVector(event.callees);
        AOT::sort(node.callees);
        removeRepeatedElements(node.callees);
    }

    const ValueFlowResultInputs& m_inputs;
    ValueFlowHeapView& m_heap;
    const Program& m_program;
    const ValueFlowUniverse& m_universe;
    const ValueSets& m_sets;
    Vector<std::unique_ptr<FunctionConstruction>>& m_functions;
    UnknownChains m_unknownChains;
    HashMap<uint32_t, ChainOfReceivers, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_chains;
    HashMap<uint32_t, std::optional<ChainOfReceivers>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_chainsBeforeLeak;
    ConstructionGraph m_graph;
};

struct ComposedCall {
    bool mayLeak { true };
    BitVector storedBeforeLeak;
    BitVector storedAtExit;
    BitVector readBeforeStore;
    BitVector readNames;
    BitVector selfReferenceNames;
};

struct StoredSets {
    bool mayLeak { false };
    bool hasExit { false };
    BitVector beforeLeak;
    BitVector atExit;
    BitVector readEarly;
    BitVector read;
    BitVector selfReferences;
    BitVector eventsReachingSelfReference;
};

// A name is stored on every path to the point. It holds a self reference on some path to the point.
struct NamesAtPoint {
    BitVector stored;
    BitVector selfReferences;
};

bool overlaps(const BitVector& a, const BitVector& b)
{
    BitVector common = a;
    common.filter(b);
    return !common.isEmpty();
}

bool storesSelfReference(const ConstructionEvent& event, const ResolvedConstructionEvent& resolved)
{
    return event.storesSelfReference && !resolved.holderIsEmpty;
}

bool storesOnlyData(const ResolvedConstructionEvent& resolved)
{
    return resolved.doesNotLeak && resolved.runsOneCallee && resolved.callees.isEmpty();
}

class StoredNamesProblem {
    WTF_MAKE_NONCOPYABLE(StoredNamesProblem);
public:
    using State = NamesAtPoint;

    StoredNamesProblem(const ConstructionGraph& graph, const ConstructionNode& node, const ConstructionNames& universe)
        : m_node(node)
        , m_summary(*node.summary)
        , m_function(node.construction->analyses->function())
        , m_universe(universe)
    {
        RELEASE_ASSERT(node.resolvedEvents.size() == m_summary.events.size());
        m_bitOfName = m_summary.names.map([&](const ConstructionName& name) {
            return bitOf(name);
        });
        m_composedCalls = m_node.callees.map([&](uint32_t callee) {
            const SolvedConstruction& solved = graph.nodes[callee].solved;
            return ComposedCall { solved.mayLeak, bitsOf(solved.storedBeforeLeak), bitsOf(solved.storedAtExit), bitsOf(solved.readBeforeStore), bitsOf(solved.readNames), bitsOf(solved.selfReferenceNames) };
        });

        size_t numberOfBits = m_universe.size();
        BitVector allBits(numberOfBits);
        for (size_t bit = 0; bit < numberOfBits; ++bit)
            allBits.set(bit);

        m_sets.beforeLeak = allBits;
        m_sets.atExit = WTF::move(allBits);
        m_sets.readEarly = BitVector(numberOfBits);
        m_sets.read = BitVector(numberOfBits);
        m_sets.selfReferences = BitVector(numberOfBits);
    }

    State stateAtEntry() const
    {
        size_t numberOfBits = m_universe.size();
        return { BitVector(numberOfBits), BitVector(numberOfBits) };
    }

    bool merge(State& into, const State& from) const
    {
        size_t storedCount = into.stored.bitCount();
        size_t selfReferenceCount = into.selfReferences.bitCount();
        into.stored.filter(from.stored);
        into.selfReferences.merge(from.selfReferences);
        return into.stored.bitCount() != storedCount || into.selfReferences.bitCount() != selfReferenceCount;
    }

    void enterEdge(State&, Edge) const { }

    void execute(State& state, const DataflowStep& step) { applyEventsAt(state, { step.block, step.index }); }

    void executeTerminator(State& state, const DataflowTerminatorStep& step) { applyEventsAt(state, pointBeforeTerminatorOf(m_function, step.block)); }

    const StoredSets& sets() const LIFETIME_BOUND { return m_sets; }

private:
    uint32_t bitOf(ConstructionName name) const
    {
        auto found = std::ranges::lower_bound(m_universe, name);
        RELEASE_ASSERT(found != m_universe.end() && *found == name);
        return static_cast<uint32_t>(found - m_universe.begin());
    }

    uint32_t bitOf(const ConstructionEvent& event) const { return m_bitOfName[*event.name]; }

    BitVector bitsOf(const ConstructionNames& names) const
    {
        BitVector bits(m_universe.size());
        for (ConstructionName name : names)
            bits.set(bitOf(name));
        return bits;
    }

    const ComposedCall& composedCallOf(uint32_t callee) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(m_node.callees, callee);
        RELEASE_ASSERT(found != m_node.callees.end() && *found == callee);
        return m_composedCalls[found - m_node.callees.begin()];
    }

    void applyLeak(const BitVector& storedAtLeak)
    {
        m_sets.beforeLeak.filter(storedAtLeak);
        m_sets.mayLeak = true;
    }

    void applyCompose(const ComposedCall& call, State& state)
    {
        BitVector readEarly = call.readBeforeStore;
        readEarly.exclude(state.stored);
        m_sets.readEarly.merge(readEarly);
        m_sets.read.merge(call.readNames);
        m_sets.selfReferences.merge(call.selfReferenceNames);
        if (call.mayLeak) {
            BitVector storedAtLeak = state.stored;
            storedAtLeak.merge(call.storedBeforeLeak);
            applyLeak(storedAtLeak);
        }
        state.stored.merge(call.storedAtExit);
        state.selfReferences.merge(call.selfReferenceNames);
    }

    void applyCallees(State& state, const State& stateBeforeOperation, const ResolvedConstructionEvent& resolved)
    {
        if (!resolved.doesNotLeak)
            applyLeak(stateBeforeOperation.stored);
        if (resolved.callees.isEmpty())
            return;

        State stateAfterCall = stateBeforeOperation;
        applyCompose(composedCallOf(resolved.callees[0]), stateAfterCall);
        for (uint32_t callee : resolved.callees.span().subspan(1)) {
            State stateAfterCallee = stateBeforeOperation;
            applyCompose(composedCallOf(callee), stateAfterCallee);
            merge(stateAfterCall, stateAfterCallee);
        }
        state.stored.merge(stateAfterCall.stored);
        state.selfReferences.merge(stateAfterCall.selfReferences);
    }

    // An access finds the accessors of every receiver, an operand that is not exact may be another object, and other code may run in place of a callee: such a callee's stores prove nothing.
    void applyPossibleCallees(State& state, const State& stateBeforeOperation, const ResolvedConstructionEvent& resolved)
    {
        if (!resolved.doesNotLeak)
            applyLeak(stateBeforeOperation.stored);
        for (uint32_t callee : resolved.callees) {
            State stateAfterCallee = stateBeforeOperation;
            applyCompose(composedCallOf(callee), stateAfterCallee);
            state.selfReferences.merge(stateAfterCallee.selfReferences);
        }
    }

    // Whoever reads a name that holds a self reference, and an accessor that takes one as the stored value, has the object.
    bool reachesSelfReference(const State& stateBeforeOperation, const ConstructionEvent& event, const ResolvedConstructionEvent& resolved) const
    {
        bool reaches = false;
        switch (event.kind) {
        case ConstructionEventKind::Store:
            reaches = storesSelfReference(event, resolved) && !storesOnlyData(resolved);
            break;
        case ConstructionEventKind::Read:
            reaches = stateBeforeOperation.selfReferences.get(bitOf(event));
            break;
        case ConstructionEventKind::Leak:
        case ConstructionEventKind::Call:
        case ConstructionEventKind::Exit:
            break;
        }
        return reaches || std::ranges::any_of(resolved.callees, [&](uint32_t callee) {
            return overlaps(composedCallOf(callee).readNames, stateBeforeOperation.selfReferences);
        });
    }

    void applyStore(State& state, uint32_t position)
    {
        const ConstructionEvent& event = m_summary.events[position];
        const ResolvedConstructionEvent& resolved = m_node.resolvedEvents[position];
        if (!storesOnlyData(resolved))
            return;
        uint32_t bit = bitOf(event);
        if (event.isExact) {
            state.stored.set(bit);
            if (event.nameOfValue)
                state.stored.set(m_bitOfName[*event.nameOfValue]);
        }
        if (storesSelfReference(event, resolved)) {
            state.selfReferences.set(bit);
            m_sets.selfReferences.set(bit);
        }
    }

    void applyRead(const State& stateBeforeOperation, uint32_t position)
    {
        uint32_t bit = bitOf(m_summary.events[position]);
        m_sets.read.set(bit);
        if (!stateBeforeOperation.stored.get(bit))
            m_sets.readEarly.set(bit);
    }

    void applyEvent(State& state, const State& stateBeforeOperation, uint32_t position)
    {
        const ConstructionEvent& event = m_summary.events[position];
        const ResolvedConstructionEvent& resolved = m_node.resolvedEvents[position];
        if (reachesSelfReference(stateBeforeOperation, event, resolved)) {
            applyLeak(stateBeforeOperation.stored);
            m_sets.eventsReachingSelfReference.set(position);
        }

        switch (event.kind) {
        case ConstructionEventKind::Leak:
            if (!resolved.doesNotLeak)
                applyLeak(stateBeforeOperation.stored);
            return;
        case ConstructionEventKind::Store:
            applyPossibleCallees(state, stateBeforeOperation, resolved);
            applyStore(state, position);
            return;
        case ConstructionEventKind::Read:
            applyPossibleCallees(state, stateBeforeOperation, resolved);
            applyRead(stateBeforeOperation, position);
            return;
        case ConstructionEventKind::Call:
            if (event.isExact && resolved.runsOneCallee)
                applyCallees(state, stateBeforeOperation, resolved);
            else
                applyPossibleCallees(state, stateBeforeOperation, resolved);
            return;
        case ConstructionEventKind::Exit:
            m_sets.beforeLeak.filter(state.stored);
            m_sets.atExit.filter(state.stored);
            m_sets.hasExit = true;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What an operation stores through one operand is not stored yet for what it reads or leaks through another.
    void applyEventsAt(State& state, ProgramPoint point)
    {
        std::span<const ConstructionEvent> events = m_summary.eventsAt(point);
        if (events.empty())
            return;
        uint32_t first = m_summary.indexOfFirstEventAt(point);
        State stateBeforeOperation = state;
        for (uint32_t offset = 0; offset < events.size(); ++offset)
            applyEvent(state, stateBeforeOperation, first + offset);
    }

    const ConstructionNode& m_node;
    const ConstructionSummary& m_summary;
    const Function& m_function;
    const ConstructionNames& m_universe;
    Vector<uint32_t> m_bitOfName;
    Vector<ComposedCall> m_composedCalls;
    StoredSets m_sets;
};

} // anonymous namespace

static ConstructionNames collectUniverse(const ConstructionGraph& graph, const ConstructionNode& node)
{
    Vector<ConstructionName> names = node.summary->names;
    for (uint32_t callee : node.callees) {
        const SolvedConstruction& solved = graph.nodes[callee].solved;
        names.append(solved.storedBeforeLeak.span());
        names.append(solved.storedAtExit.span());
        names.append(solved.readBeforeStore.span());
        names.append(solved.readNames.span());
        names.append(solved.selfReferenceNames.span());
    }
    return ConstructionNames(names.span());
}

static ConstructionNames namesOf(const ConstructionNames& universe, const BitVector& bits)
{
    ConstructionNames names;
    for (size_t bit : bits)
        names.add(universe[bit]);
    return names;
}

static void solveStoredNames(ConstructionGraph& graph, uint32_t index)
{
    ConstructionNode& node = graph.nodes[index];
    node.solved = { };
    if (!node.summary)
        return;

    ConstructionNames universe = collectUniverse(graph, node);
    StoredNamesProblem problem(graph, node, universe);
    node.construction->analyses->solveForward(problem);

    const StoredSets& sets = problem.sets();
    node.solved.mayLeak = sets.mayLeak;
    // A function that never returns has still read: its caller may catch what it throws and go on.
    node.solved.readBeforeStore = namesOf(universe, sets.readEarly);
    node.solved.readNames = namesOf(universe, sets.read);
    node.solved.selfReferenceNames = namesOf(universe, sets.selfReferences);
    if (sets.hasExit) {
        BitVector beforeLeak = sets.beforeLeak;
        beforeLeak.exclude(sets.readEarly);
        node.solved.storedBeforeLeak = namesOf(universe, beforeLeak);
        node.solved.storedAtExit = namesOf(universe, sets.atExit);
    }
    for (size_t position : sets.eventsReachingSelfReference)
        node.resolvedEvents[position].doesNotLeak = false;
}

namespace {

struct InitialOrder {
    Vector<ConstructionName> names;
    bool storesOnlyInInitialOrder { false };
};

class InitialOrderBuilder {
public:
    InitialOrderBuilder(const ConstructionGraph& graph, const ConstructionNode& node)
        : m_graph(graph)
        , m_node(node)
        , m_summary(*node.summary)
        , m_analyses(*node.construction->analyses)
        , m_program(m_analyses.program())
        , m_cfg(m_analyses.cfg())
    {
    }

    InitialOrder build(bool mayLeak)
    {
        m_reachableAfter.fill(std::nullopt, m_analyses.function().blocks.size());
        collectOrderPoints();

        InitialOrder order;
        bool everyPointAccepted = true;
        for (uint32_t index = 0; index < m_creatingPoints.size(); ++index) {
            uint32_t position = m_creatingPoints[index];
            if (!isOrderCandidate(position)) {
                everyPointAccepted = false;
                m_passedOverPoints.append(position);
                continue;
            }
            if (!isOrderPointAccepted(index) || !appendCreatedNames(position, order.names)) {
                everyPointAccepted = false;
                break;
            }
        }
        order.storesOnlyInInitialOrder = !mayLeak && everyPointAccepted;
        return order;
    }

private:
    const ConstructionEvent& eventAt(uint32_t position) const { return m_summary.events[position]; }
    uint32_t positionOf(const ConstructionEvent& event) const { return static_cast<uint32_t>(&event - m_summary.events.span().data()); }
    const ResolvedConstructionEvent& resolvedAt(uint32_t position) const { return m_node.resolvedEvents[position]; }
    const ConstructionName& nameAt(uint32_t position) const { return m_summary.names[*eventAt(position).name]; }
    const SolvedConstruction& solvedOf(uint32_t callee) const { return m_graph.nodes[callee].solved; }

    bool createsOwnKey(uint32_t position) const
    {
        if (eventAt(position).kind != ConstructionEventKind::Store || !resolvedAt(position).doesNotLeak)
            return false;
        PropertyKeyID key = nameAt(position).key;
        return key && !isPrivateName(m_program, key);
    }

    bool mayCreateKeys(uint32_t position) const
    {
        return createsOwnKey(position) || std::ranges::any_of(resolvedAt(position).callees, [&](uint32_t callee) {
            const SolvedConstruction& solved = solvedOf(callee);
            return !solved.storesOnlyInInitialOrder || !solved.initialOrder.isEmpty();
        });
    }

    bool mayLeak(uint32_t position) const
    {
        const ResolvedConstructionEvent& resolved = resolvedAt(position);
        return !resolved.doesNotLeak || std::ranges::any_of(resolved.callees, [&](uint32_t callee) {
            return solvedOf(callee).mayLeak;
        });
    }

    bool isSoleCreatingPoint(uint32_t position) const
    {
        return std::ranges::all_of(m_summary.eventsAt(eventAt(position).point), [&](const ConstructionEvent& other) {
            return positionOf(other) == position || !mayCreateKeys(positionOf(other));
        });
    }

    bool mustCreateKeys(uint32_t position) const
    {
        const ConstructionEvent& event = eventAt(position);
        if (!event.isExact || !resolvedAt(position).runsOneCallee || !isSoleCreatingPoint(position))
            return false;
        return resolvedAt(position).callees.isEmpty() || event.kind == ConstructionEventKind::Call;
    }

    void collectOrderPoints()
    {
        std::span<const ConstructionEvent> events = m_summary.events.span();
        for (BlockID block : m_cfg.reversePostOrder()) {
            auto eventsOfBlock = std::ranges::equal_range(events, block, { }, [](const ConstructionEvent& event) {
                return event.point.block;
            });
            uint32_t firstCreatingPoint = m_creatingPoints.size();
            for (const ConstructionEvent& event : eventsOfBlock) {
                uint32_t position = positionOf(event);
                if (mayCreateKeys(position))
                    m_creatingPoints.append(position);
                if (mayLeak(position))
                    m_leakPoints.append(position);
                if (event.kind == ConstructionEventKind::Exit)
                    m_exitPoints.append(position);
            }
            if (firstCreatingPoint < m_creatingPoints.size())
                m_creatingBlocks.append({ block, firstCreatingPoint, static_cast<uint32_t>(m_creatingPoints.size()) });
        }
    }

    const BitVector& blocksReachableAfter(BlockID block)
    {
        std::optional<BitVector>& reachable = m_reachableAfter[block.index()];
        if (!reachable) {
            reachable = BitVector(m_reachableAfter.size());
            for (BlockID successor : m_cfg.successors(block))
                reachable->merge(m_cfg.blocksReachableFrom(successor, { }));
        }
        return *reachable;
    }

    bool reaches(uint32_t from, uint32_t to)
    {
        ProgramPoint fromPoint = eventAt(from).point;
        ProgramPoint toPoint = eventAt(to).point;
        if (fromPoint.block == toPoint.block && fromPoint.index < toPoint.index)
            return true;
        return blocksReachableAfter(fromPoint.block).get(toPoint.block.index());
    }

    bool precedesOnEveryPath(uint32_t position, uint32_t later)
    {
        ProgramPoint point = eventAt(position).point;
        ProgramPoint laterPoint = eventAt(later).point;
        if (point.block == laterPoint.block)
            return point.index < laterPoint.index;
        return m_analyses.dominators().terminatorDominates(point.block, laterPoint.block);
    }

    bool isOrderCandidate(uint32_t position)
    {
        auto comesLater = [&](uint32_t point) { return point == position || precedesOnEveryPath(position, point); };
        return mustCreateKeys(position) && !m_exitPoints.isEmpty() && std::ranges::all_of(m_exitPoints, comesLater) && std::ranges::all_of(m_leakPoints, comesLater);
    }

    bool storesOnlyAccepted(uint32_t point, uint32_t candidate) const
    {
        auto isAccepted = [&](const ConstructionName& name) {
            return m_acceptedNames.contains(name) || (resolvedAt(candidate).callees.isEmpty() && nameAt(candidate) == name);
        };
        if (createsOwnKey(point) && !isAccepted(nameAt(point)))
            return false;
        return std::ranges::all_of(resolvedAt(point).callees, [&](uint32_t callee) {
            const SolvedConstruction& solved = solvedOf(callee);
            return solved.storesOnlyInInitialOrder && std::ranges::all_of(solved.initialOrder, isAccepted);
        });
    }

    // The points before this one are accepted or passed over.
    bool isOrderPointAccepted(uint32_t index)
    {
        uint32_t position = m_creatingPoints[index];
        for (uint32_t leak : m_leakPoints) {
            if (reaches(leak, position))
                return false;
        }
        for (uint32_t point : m_passedOverPoints) {
            if (reaches(point, position) && !storesOnlyAccepted(point, position))
                return false;
        }

        BlockID block = eventAt(position).point.block;
        for (const CreatingBlock& creatingBlock : m_creatingBlocks) {
            if (creatingBlock.end <= index + 1 || !blocksReachableAfter(creatingBlock.block).get(block.index()))
                continue;
            for (uint32_t later = std::max(creatingBlock.begin, index + 1); later < creatingBlock.end; ++later) {
                if (!storesOnlyAccepted(m_creatingPoints[later], position))
                    return false;
            }
        }
        return true;
    }

    const SolvedConstruction* findAgreedCallee(uint32_t position) const
    {
        const Vector<uint32_t, 1>& callees = resolvedAt(position).callees;
        const SolvedConstruction& first = solvedOf(callees[0]);
        bool calleesAgree = callees.size() == 1 || std::ranges::all_of(callees, [&](uint32_t callee) {
            const SolvedConstruction& solved = solvedOf(callee);
            return solved.storesOnlyInInitialOrder && solved.initialOrder == first.initialOrder;
        });
        return calleesAgree ? &first : nullptr;
    }

    // False where something may be created behind the names that were appended.
    bool appendCreatedNames(uint32_t position, Vector<ConstructionName>& order)
    {
        auto addToOrder = [&](const ConstructionName& name) {
            if (m_acceptedNames.add(name))
                order.append(name);
        };
        if (resolvedAt(position).callees.isEmpty()) {
            addToOrder(nameAt(position));
            return true;
        }
        const SolvedConstruction* callee = findAgreedCallee(position);
        if (!callee)
            return false;
        for (const ConstructionName& name : callee->initialOrder)
            addToOrder(name);
        return callee->storesOnlyInInitialOrder;
    }

    struct CreatingBlock {
        BlockID block;
        uint32_t begin { 0 };
        uint32_t end { 0 };
    };

    const ConstructionGraph& m_graph;
    const ConstructionNode& m_node;
    const ConstructionSummary& m_summary;
    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const CFG& m_cfg;
    Vector<uint32_t> m_creatingPoints;
    Vector<CreatingBlock> m_creatingBlocks;
    Vector<uint32_t> m_passedOverPoints;
    Vector<uint32_t> m_leakPoints;
    Vector<uint32_t> m_exitPoints;
    Vector<std::optional<BitVector>> m_reachableAfter;
    ConstructionNames m_acceptedNames;
};

} // anonymous namespace

static void computeInitialOrder(ConstructionGraph& graph, uint32_t index)
{
    ConstructionNode& node = graph.nodes[index];
    if (!node.summary)
        return;
    InitialOrder order = InitialOrderBuilder(graph, node).build(node.solved.mayLeak);
    node.solved.initialOrder = WTF::move(order.names);
    node.solved.storesOnlyInInitialOrder = order.storesOnlyInInitialOrder;
}

static void solveConstructions(ConstructionGraph& graph)
{
    StronglyConnectedComponents components = computeStronglyConnectedComponents(graph.nodes.size(), [&](unsigned node) -> const Vector<uint32_t>& {
        return graph.nodes[node].callees;
    });

    Vector<uint32_t> calleesFirst(graph.nodes.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(calleesFirst, { }, [&](uint32_t node) {
        return std::pair(components.componentOf[node], node);
    });

    for (uint32_t index : calleesFirst) {
        if (components.isInCycle.get(index))
            graph.nodes[index].summary = nullptr;
        solveStoredNames(graph, index);
        computeInitialOrder(graph, index);
    }
}

// The names at the front of the initial order that the parent constructor creates.
static size_t parentOrderLength(const ConstructionGraph& graph, const ConstructionNode& node)
{
    if (node.root.kind != ConstructionRootKind::SuperConstruct || !node.summary)
        return 0;
    std::span<const ConstructionEvent> events = node.summary->events.span();
    auto rootEvent = std::ranges::find_if(events, [&](const ConstructionEvent& event) {
        return event.origin == node.root.definition && event.operand == node.root.definition;
    });
    if (rootEvent == events.end())
        return 0;
    const ResolvedConstructionEvent& resolved = node.resolvedEvents[rootEvent - events.begin()];
    if (resolved.callees.isEmpty())
        return 0;
    return std::min(graph.nodes[resolved.callees[0]].solved.initialOrder.size(), node.solved.initialOrder.size());
}

namespace {

struct OwnFieldOrder {
    FieldOrder order;
    bool isComplete { false };
};

} // anonymous namespace

static std::optional<uint32_t> findFieldOfLayout(const Program& program, LayoutID layout, ConstructionName name)
{
    if (name.key)
        return program.at(layout).findField(name.key);
    if (name.field.layout == layout)
        return name.field.field;
    return std::nullopt;
}

static OwnFieldOrder ownOrderOf(const Program& program, const ConstructionGraph& graph, const ConstructionNode& node, LayoutID layout)
{
    OwnFieldOrder result;
    for (ConstructionName name : node.solved.initialOrder.span().subspan(parentOrderLength(graph, node))) {
        std::optional<uint32_t> field = findFieldOfLayout(program, layout, name);
        if (!field)
            return result;
        result.order.fields.append(*field);
    }
    result.isComplete = true;
    return result;
}

// Reflect.construct(Base, [], Derived) creates an object of Derived that the constructor of Derived never stores to.
static ValueSetID findNewTargetsOfOtherFunctions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<ValueFlowObjectID> newTargetsFound;
    // The class at the upper end of the classes that the program declares above the one that the function constructs. None where that end is no class of the program, and for any other function.
    auto allocatingClassOf = [&](ValueFlowObjectID closure) {
        LayoutID current = program.at(universe.closureFunction(closure)).constructedClass;
        for (size_t steps = 0; current && program.at(current).has(LayoutFlags::HasHeritage) && steps < program.layouts.size(); ++steps)
            current = predictedHeritageOf(program.at(current));
        return current;
    };
    // A bound function constructs its target. Whatever else can be called and is no function of the program or of the table constructs what native code was handed.
    auto withWhatOtherCallablesConstruct = [&](ValueSetID callees) {
        ValueSetMaker& maker = heap.maker();
        callees = heap.expand(callees);
        if (inputs.sets().meet(callees, universe.otherCallables()))
            callees = maker.unite(callees, heap.escaped());
        ValueFlowName target { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() };
        for (ValueSetID bound = maker.intersect(callees, universe.otherCallables()); bound != ValueSets::nothing;) {
            ValueSetID targets = maker.intersect(heap.expand(heap.writtenUnder(bound, target)), ValueSets::complement(callees));
            callees = maker.unite(callees, targets);
            bound = maker.intersect(targets, universe.otherCallables());
        }
        return callees;
    };
    auto add = [&](ValueSetID callees, ValueSetID newTargets) {
        constexpr uint32_t severalCallees = UINT32_MAX;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> calleeByAllocatingClass;
        inputs.sets().forEachMember(heap.maker().intersect(withWhatOtherCallablesConstruct(callees), universe.closures()), [&](ValueFlowObjectID callee) {
            LayoutID allocatingClass = allocatingClassOf(callee);
            if (!allocatingClass)
                return;
            auto entry = calleeByAllocatingClass.add(allocatingClass.index(), callee.index());
            if (!entry.isNewEntry)
                entry.iterator->value = severalCallees;
        });
        inputs.sets().forEachMember(heap.maker().intersect(heap.expand(newTargets), universe.closures()), [&](ValueFlowObjectID newTarget) {
            LayoutID allocatingClass = allocatingClassOf(newTarget);
            auto callee = allocatingClass ? calleeByAllocatingClass.find(allocatingClass.index()) : calleeByAllocatingClass.end();
            if (!allocatingClass || (callee != calleeByAllocatingClass.end() && callee->value != newTarget.index()))
                newTargetsFound.append(newTarget);
        });
    };
    // Unseen code constructs with what it can reach: all that has escaped, and the classes above the instances among that.
    if (unseenCodeCanRun(inputs.solution)) {
        ValueSetID reached = heap.maker().unite(universe.unknownObjectAlone(), inputs.solver().valueOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ConstructorsAboveEscapedInstances)));
        add(reached, reached);
    }
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    Vector<uint32_t> constructions = collectInParallel<uint32_t>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        if (!inputs.isReached(FunctionID(index)))
            return;
        std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
        for (uint32_t position = ranges[index].statement; position < ranges[index + 1].statement; ++position) {
            const ValueFlowStatement& statement = statements[position];
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::Construct && statement.opcode != Opcode::ConstructList))
                continue;
            const Value& construction = program.functions[index].at(statement.site);
            ValueID newTarget = construction.operand(*OperandPosition::newTarget(statement.opcode));
            if (newTarget && newTarget != construction.calleeOperand())
                found.append(position);
        }
    });
    for (uint32_t position : constructions) {
        const ValueFlowStatement& statement = statements[position];
        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
        add(heap.valueOf(operands[0]), heap.valueOf(operands[*OperandPosition::newTarget(statement.opcode)]));
    }
    BitVector builtinsThatConstruct = builtinsWithOverload(program.builtins(), [](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.callbacks, [](const BuiltinCallback& callback) {
            return callback.constructs && callback.newTarget.kind != BuiltinPlaceKind::None;
        });
    });
    forEachBuiltinApplication(inputs, heap, builtinsThatConstruct, [&](const ValueFlowBuiltinApplication& application) {
        for (const BuiltinOverload& overload : program.builtins().overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            for (const BuiltinCallback& callback : overload.flow.callbacks) {
                if (!callback.constructs || callback.newTarget.kind == BuiltinPlaceKind::None)
                    continue;
                bool namesOperand = callback.newTarget.kind == BuiltinPlaceKind::Operand && callback.newTarget.operand;
                add(application.valueAt(callback.callee, heap.maker()), namesOperand ? application.valueAt(*callback.newTarget.operand, heap.maker()) : ValueSets::anything);
            }
        }
    });
    return heap.maker().of(newTargetsFound.span());
}

// The layouts some object of which comes out of something else than an allocation in a constructor of its class that the solve has followed.
static BitVector findLayoutsAllocatedElsewhere(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    BitVector layouts(program.layouts.size());
    Vector<LayoutID> allocated = collectInParallel<LayoutID>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<LayoutID>& found) {
        FunctionID id(index);
        bool isFollowed = inputs.isReached(id);
        if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::NewObject })))
            return;
        if (!isFollowed && !isUnsolvedCode(inputs, tables, id))
            return;
        const Function& function = program.functions[index];
        for (const Value& value : function.values) {
            if (value.opcode != Opcode::NewObject || program.at(value.layout()).kind != LayoutKind::Instance)
                continue;
            if (isFollowed && allocatesObjectOfOwnClass(program, function, value))
                continue;
            found.append(value.layout());
        }
    });
    for (LayoutID layout : allocated) {
        layouts.quickSet(layout.index());
        layouts.quickSet(declaredLayoutOf(program, layout).index());
    }
    return layouts;
}

static BitVector findFunctionsSeeingOnlyFinishedObjects(const Program& program, const ParallelLoops& loops, const ConstructionGraph& graph)
{
    const Vector<ConstructionNode>& nodes = graph.nodes;
    BitVector isReached(nodes.size());
    Vector<uint32_t> nodesToVisit;
    auto reach = [&](uint32_t node) {
        if (!isReached.set(node))
            nodesToVisit.append(node);
    };
    for (uint32_t node : graph.constructorNodes)
        reach(node);

    BitVector holdsUnfinishedObject(program.functions.size());
    while (!nodesToVisit.isEmpty()) {
        const ConstructionNode& node = nodes[nodesToVisit.takeLast()];
        holdsUnfinishedObject.quickSet(node.function.index());
        for (uint32_t callee : node.callees)
            reach(callee);
    }

    BitVector functions(program.functions.size());
    Vector<uint32_t> found = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        bool isMaker = function.constructedClass || function.kind == FunctionKind::Initializer;
        return !isMaker && !holdsUnfinishedObject.quickGet(index);
    });
    for (uint32_t index : found)
        functions.quickSet(index);
    return functions;
}

// The nodes of the code that makes the objects of the layout. None where some of that code is not followed.
static std::optional<Vector<uint32_t, 2>> findMakers(const ValueFlowResultInputs& inputs, const ConstructionGraph& graph, LayoutID id)
{
    const Program& program = inputs.program;
    Vector<FunctionID, 2> bodies;
    if (!appendConstructionBodies(inputs, program.at(id).constructor, bodies))
        return std::nullopt;

    Vector<uint32_t, 2> makers;
    for (FunctionID body : bodies) {
        for (const ConstructionRoot& root : constructorRootsOf(program, program.at(body))) {
            std::optional<uint32_t> node = graph.find(body, root.definition, isConstructorFunctionOfOwnLayout(program, body));
            if (!node)
                return std::nullopt;
            makers.append(*node);
        }
    }
    if (makers.isEmpty())
        return std::nullopt;
    return makers;
}

// A literal stores what it lists in the allocation itself, so nobody sees its object without that.
static void addBirthsOfLiterals(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueFlowConstructions& result)
{
    using KeysByLayout = HashMap<uint32_t, Vector<PropertyKeyID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    const Program& program = inputs.program;
    auto keepCommonKeys = [](KeysByLayout& keysOfEveryLiteral, LayoutID layout, Vector<PropertyKeyID>&& keys) {
        auto result = keysOfEveryLiteral.add(layout.index(), Vector<PropertyKeyID>());
        if (result.isNewEntry) {
            result.iterator->value = WTF::move(keys);
            return;
        }
        result.iterator->value.removeAllMatching([&](PropertyKeyID key) {
            return !std::ranges::binary_search(keys, key);
        });
    };
    Vector<KeysByLayout> keysOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::PieceOutsideSlots })))
            return;
        if (!inputs.isReached(id) && !isUnsolvedCode(inputs, tables, id))
            return;
        const Function& function = program.functions[index];
        for (const Value& value : function.values) {
            // An accessor, and whatever a key that is computed or a spread brings, may lie outside the slots, and the cell of an object with anything outside its slots is marked for good.
            if (value.opcode == Opcode::DefineGetter || value.opcode == Opcode::DefineSetter || value.opcode == Opcode::ElemDefine || value.opcode == Opcode::CopyDataProperties) {
                const Value& object = function.at(value.operand(0));
                if (object.opcode == Opcode::NewObject && program.at(object.layout()).kind == LayoutKind::Record)
                    keepCommonKeys(keysOfWorker[worker], object.layout(), { });
                continue;
            }
            if (value.opcode != Opcode::NewObject)
                continue;
            const Layout& layout = program.at(value.layout());
            if (layout.kind != LayoutKind::Record || layout.isDerived())
                continue;
            Vector<PropertyKeyID> keys;
            for (uint32_t field : value.fields()) {
                if (PropertyKeyID key = layout.fields[field].key)
                    keys.append(key);
            }
            AOT::sort(keys);
            keepCommonKeys(keysOfWorker[worker], value.layout(), WTF::move(keys));
        }
    });
    Vector<std::optional<Vector<PropertyKeyID>>> keysOfEveryLiteral(program.layouts.size());
    for (KeysByLayout& keysOfOneWorker : keysOfWorker) {
        for (auto& [layout, keys] : keysOfOneWorker) {
            std::optional<Vector<PropertyKeyID>>& common = keysOfEveryLiteral[layout];
            if (!common) {
                common = WTF::move(keys);
                continue;
            }
            common->removeAllMatching([&](PropertyKeyID key) {
                return !std::ranges::binary_search(keys, key);
            });
        }
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (keysOfEveryLiteral[index])
            result.layouts[index].keysStoredAtBirth = WTF::move(*keysOfEveryLiteral[index]);
    }
}

ValueFlowConstructions computeConstructions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<std::unique_ptr<FunctionConstruction>> functions(program.functions.size());
    ConstructionGraphBuilder builder(inputs, heap, functions);
    ConstructionGraph graph = builder.build();
    solveConstructions(graph);

    ValueFlowConstructions result;
    result.layouts = Vector<ValueFlowConstructions::OfLayout>(program.layouts.size());
    result.functionsSeeingOnlyFinishedObjects = findFunctionsSeeingOnlyFinishedObjects(program, inputs.loops(), graph);

    ValueSetID newTargetsOfOtherFunctions = findNewTargetsOfOtherFunctions(inputs, heap);
    BitVector isAllocatedElsewhere = findLayoutsAllocatedElsewhere(inputs, tables);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived())
            continue;
        std::optional<Vector<uint32_t, 2>> makers = findMakers(inputs, graph, id);
        if (!makers)
            continue;

        ValueFlowConstructions::OfLayout& construction = result.layouts[index];
        bool createsFieldsOnlyInListedOrders = true;
        ConstructionNames stored = graph.nodes[makers->first()].solved.storedBeforeLeak;
        for (uint32_t maker : *makers) {
            const ConstructionNode& node = graph.nodes[maker];
            stored.intersect(node.solved.storedBeforeLeak);
            OwnFieldOrder own = ownOrderOf(program, graph, node, id);
            createsFieldsOnlyInListedOrders &= !node.solved.mayLeak && node.solved.storesOnlyInInitialOrder && own.isComplete;
            if (!construction.orders.contains(own.order))
                construction.orders.append(WTF::move(own.order));
        }
        AOT::sort(construction.orders, [](const FieldOrder& a, const FieldOrder& b) {
            return std::ranges::lexicographical_compare(a.fields, b.fields);
        });

        // The constructor of a class that is allocated only through the classes below it still creates its own fields, in front of theirs. What follows speaks for the objects of the layout itself.
        if (isAllocatedElsewhere.quickGet(index) || inputs.sets().contains(newTargetsOfOtherFunctions, inputs.universe().closureOf(layout.constructor)))
            continue;
        construction.isFollowed = true;
        construction.createsFieldsOnlyInListedOrders = createsFieldsOnlyInListedOrders;
        for (ConstructionName name : stored) {
            if (name.standsForValue)
                continue;
            if (name.key)
                construction.keysStoredBeforeObjectLeaks.append(name.key);
            else
                construction.fieldsStoredBeforeObjectLeaks.append(name.field);
        }
        AOT::sort(construction.keysStoredBeforeObjectLeaks);
        AOT::sort(construction.fieldsStoredBeforeObjectLeaks);
    }

    // The constructor of a class that is not followed, or another function in its place (Reflect.construct(Base, [], Derived)), still hands the object to the constructors above, and they store by name.
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.has(LayoutFlags::HasHeritage) || result.layouts[index].isFollowed)
            continue;
        // A class gets the fields of the class that it extends once the results are applied.
        Vector<PropertyKeyID>& keys = result.layouts[index].inheritedKeysThatStoresMayNotCreate;
        LayoutID above = predictedHeritageOf(layout);
        for (size_t steps = 0; above && steps < program.layouts.size(); ++steps) {
            for (const Field& field : program.at(above).fields) {
                if (field.key && !keys.contains(field.key) && builder.mayBeStoppedAboveObjectsOf(LayoutID(index), field.key))
                    keys.append(field.key);
            }
            above = predictedHeritageOf(program.at(above));
        }
        AOT::sort(keys);
    }

    ConstructionGraph graphUnderGuards = ConstructionGraphBuilder(inputs, heap, functions, UnknownChains::HoldNothingUnderName).build();
    solveConstructions(graphUnderGuards);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived() || layout.has(LayoutFlags::HasHeritage))
            continue;
        std::optional<Vector<uint32_t, 2>> makers = findMakers(inputs, graphUnderGuards, id);
        if (!makers)
            continue;
        ConstructionNames stored = graphUnderGuards.nodes[makers->first()].solved.storedBeforeLeak;
        for (uint32_t maker : *makers)
            stored.intersect(graphUnderGuards.nodes[maker].solved.storedBeforeLeak);
        ValueFlowConstructions::OfLayout& construction = result.layouts[index];
        for (ConstructionName name : stored) {
            if (name.standsForValue) {
                if (name.key)
                    construction.keysStoredWithValueAtBirth.append(name.key);
                continue;
            }
            if (name.key)
                construction.keysStoredAtBirth.append(name.key);
            else
                construction.fieldsStoredAtBirth.append(name.field);
        }
        AOT::sort(construction.keysStoredWithValueAtBirth);
        AOT::sort(construction.keysStoredAtBirth);
        AOT::sort(construction.fieldsStoredAtBirth);
    }
    addBirthsOfLiterals(inputs, tables, result);
    // The copy of a maker's body that allocates by the tag does to its object what the maker does to `this`.
    for (const TaggedFamily& family : program.taggedFamilies) {
        const ValueFlowConstructions::OfLayout& maker = result.layouts[family.maker.index()];
        for (const TaggedLayout& member : family.members) {
            ValueFlowConstructions::OfLayout& construction = result.layouts[member.layout.index()];
            construction.isFollowed = maker.isFollowed;
            construction.keysStoredBeforeObjectLeaks = maker.keysStoredBeforeObjectLeaks;
            construction.fieldsStoredBeforeObjectLeaks = maker.fieldsStoredBeforeObjectLeaks;
            construction.keysStoredAtBirth = maker.keysStoredAtBirth;
            construction.keysStoredWithValueAtBirth = maker.keysStoredWithValueAtBirth;
        }
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
