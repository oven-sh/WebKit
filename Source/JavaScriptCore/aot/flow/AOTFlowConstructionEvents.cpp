/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowConstruction.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses confinedRetentionUses { OperandUse::Inspects, OperandUse::Retains };
// Whether an accessor takes the stored value is decided from the links of the store.
static constexpr OperandUses namedStoreUses = confinedRetentionUses | OperandUses { OperandUse::PassesToUserCode };
// A store by name is an event of its own. Any other write may create a property under any name.
static constexpr OperandUses unnamedWritingUses = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };
static constexpr OperandUses contentsLeakingUses = callingUses | OperandUses { OperandUse::RetainsContents, OperandUse::AppliesTransitively, OperandUse::AppliesToContents };
static constexpr Protocols enumeratingProtocols { Protocol::OwnKeys, Protocol::OwnValues };

static std::pair<BlockID, uint32_t> orderOfPoint(ProgramPoint point)
{
    return { point.block, point.index };
}

// What leaks through another operand of a call leaks when the call is entered, before anything the callee stores.
static unsigned orderWithinPoint(FlowConstructionEventKind kind)
{
    switch (kind) {
    case FlowConstructionEventKind::Store:
        return 0;
    case FlowConstructionEventKind::Read:
        return 1;
    case FlowConstructionEventKind::Leak:
        return 2;
    case FlowConstructionEventKind::Call:
        return 3;
    case FlowConstructionEventKind::Exit:
        return 4;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static auto orderOfEvent(const FlowConstructionEvent& event)
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
    switch (kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<FlowConstructionEventKind> accessEventOf(Opcode opcode, uint32_t operandIndex)
{
    if (receiverOf(opcode) != operandIndex)
        return std::nullopt;
    switch (accessOf(opcode)) {
    case Access::Store:
    case Access::Define:
        return FlowConstructionEventKind::Store;
    case Access::Read:
    case Access::Has:
        return FlowConstructionEventKind::Read;
    case Access::None:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowConstructionName fieldName(const Program& program, FieldReference reference)
{
    const Field& field = fieldAt(program, reference);
    if (field.key)
        return { field.key, { } };
    return { { }, declarationOf(program, reference) };
}

std::span<const FlowConstructionEvent> FlowConstructionSummary::eventsAt(ProgramPoint point) const
{
    auto found = std::ranges::equal_range(events, orderOfPoint(point), { }, [](const FlowConstructionEvent& event) {
        return orderOfPoint(event.point);
    });
    return events.subspan(found.begin() - events.begin(), found.size());
}

uint32_t FlowConstructionSummary::indexOfFirstEventAt(ProgramPoint point) const
{
    return static_cast<uint32_t>(eventsAt(point).data() - events.span().data());
}

std::optional<uint32_t> FlowConstructionSummary::indexOf(const FlowConstructionName& name) const
{
    auto found = std::ranges::lower_bound(names, name);
    if (found == names.end() || *found != name)
        return std::nullopt;
    return static_cast<uint32_t>(found - names.begin());
}

FlowFunctionConstruction::FlowFunctionConstruction(const AnalysisContext& context, FunctionID id, const Function& body)
    : function(id)
    , analyses(context, body)
{
}

const FlowConstructionSummary* FlowFunctionConstruction::find(ValueID rootDefinition) const
{
    auto found = std::ranges::lower_bound(summaries, rootDefinition, { }, [](const FlowConstructionSummary& summary) {
        return summary.root.definition;
    });
    if (found == summaries.end() || found->root.definition != rootDefinition)
        return nullptr;
    return &*found;
}

FlowReadOfCreatedObject* FlowFunctionConstruction::findReadOfCreatedObject(ValueID read)
{
    auto found = std::ranges::lower_bound(readsOfCreatedObject, read, { }, &FlowReadOfCreatedObject::read);
    if (found == readsOfCreatedObject.end() || found->read != read)
        return nullptr;
    return &*found;
}

FlowFieldDefinition* FlowFunctionConstruction::findFieldDefinition(ValueID definition)
{
    auto found = std::ranges::lower_bound(fieldDefinitions, definition, { }, &FlowFieldDefinition::definition);
    if (found == fieldDefinitions.end() || found->definition != definition)
        return nullptr;
    return &*found;
}

bool allocatesConstructedObject(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::NewObject && function.kind == FunctionKind::BaseConstructor && program.at(value.layout()).kind == LayoutKind::Instance;
}

Vector<FlowConstructionRoot, 2> constructorRootsOf(const Program& program, const Function& function)
{
    Vector<FlowConstructionRoot, 2> roots;
    if (!function.hasBody() || function.isUnsplitCoroutine() || function.constructorForm() == ConstructorForm::NotConstructor)
        return roots;
    if (std::optional<unsigned> index = function.thisParameterIndex()) {
        roots.append({ FlowConstructionRootKind::EntryParameter, function.at(entryBlock).parameters[*index] });
        return roots;
    }
    if (function.kind == FunctionKind::Plain)
        return roots;

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (allocatesConstructedObject(program, function, value))
                roots.append({ FlowConstructionRootKind::Allocation, id });
            else if (OperandPosition::parent(value.opcode))
                roots.append({ FlowConstructionRootKind::SuperConstruct, id });
        }
    }
    return roots;
}

// A constructor returns its completion value if that is an object, and else the constructed object: a merge point of the two.
static bool isConstructedObject(const Program& program, FunctionAnalyses& analyses, std::span<const FlowConstructionRoot> constructorRoots, ValueID result)
{
    const Function& constructor = analyses.function();
    BitVector isVisited(constructor.values.size());
    Vector<ValueID, 4> valuesToVisit { result };
    while (!valuesToVisit.isEmpty()) {
        ValueID value = valuesToVisit.takeLast();
        if (isVisited.set(value.index()) || !program.typeOf(constructor, value).hasObjects())
            continue;

        ValueID object = sameObjectThroughCasts(constructor, value);
        bool isRoot = std::ranges::any_of(constructorRoots, [&](const FlowConstructionRoot& root) {
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

static bool returnsOnlyConstructedObject(const Program& program, FunctionAnalyses& analyses, std::span<const FlowConstructionRoot> constructorRoots)
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

FlowConstructionName constructionNameOf(const Program& program, const Value& access)
{
    RELEASE_ASSERT(accessOf(access.opcode) != Access::None);
    if (Value::opcodesWithField.contains(access.opcode))
        return fieldName(program, { access.layout(), access.field() });
    RELEASE_ASSERT(Value::opcodesWithKey.contains(access.opcode));
    return { access.key(), { } };
}

bool isReadOfCreatedObject(const Function& function, const FlowConstructionRoot& root, const FlowConstructionEvent& event)
{
    if (root.kind == FlowConstructionRootKind::EntryParameter || event.kind != FlowConstructionEventKind::Read || !event.isExact)
        return false;
    const Value& read = function.at(event.origin);
    return read.opcode == Opcode::PropGet && !read.isFreeName();
}

namespace {

class ConstructionEventCollector {
public:
    ConstructionEventCollector(const Program& program, FunctionAnalyses& analyses, FlowConstructionRoot root)
        : m_program(program)
        , m_analyses(analyses)
        , m_function(analyses.function())
        , m_escape(analyses.escape())
        , m_useLists(analyses.useLists())
        , m_root(root)
    {
    }

    FlowConstructionSummary collect()
    {
        FlowConstructionSummary summary { m_root, FlowConstructionTracking::Unknown, { }, { } };
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
    void addEvent(const FlowConstructionEvent& event, std::optional<FlowConstructionName> name = std::nullopt)
    {
        m_events.append(event);
        m_nameOfEvent.append(name);
    }

    FlowConstructionEvent eventAt(FlowConstructionEventKind kind, const Use& use) const
    {
        return { .kind = kind, .point = m_useLists.pointOf(use), .origin = use.user, .operand = usedValueOf(m_function, use), .operandIndex = use.operandIndex };
    }

    FlowConstructionEvent eventAtDefinition(FlowConstructionEventKind kind) const
    {
        ValueLocation location = m_useLists.locationOf(m_root.definition);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        return { .kind = kind, .isExact = true, .point = { location.block, location.index }, .origin = m_root.definition, .operand = m_root.definition };
    }

    void addLeak(const Use& use) { addEvent(eventAt(FlowConstructionEventKind::Leak, use)); }

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
        case FlowConstructionRootKind::EntryParameter:
            return;
        case FlowConstructionRootKind::Allocation: {
            const Value& allocation = m_function.at(m_root.definition);
            for (uint32_t field : allocation.fields())
                addEvent(eventAtDefinition(FlowConstructionEventKind::Store), fieldName(m_program, { allocation.layout(), field }));
            return;
        }
        case FlowConstructionRootKind::SuperConstruct:
            addEvent(eventAtDefinition(FlowConstructionEventKind::Call));
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
            return m_root.kind == FlowConstructionRootKind::EntryParameter;
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
            return false;
        case TerminatorKind::Jump:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void addAccess(FlowConstructionEventKind kind, ObjectID object, const Use& use)
    {
        const Value& access = m_function.at(use.user);
        FlowConstructionEvent event = eventAt(kind, use);
        event.isExact = m_escape.singleObject(usedValueOf(m_function, use)) == object;
        if (kind == FlowConstructionEventKind::Store) {
            ValueID stored = access.operand(*storedOf(access.opcode));
            event.storesSelfReference = mayHoldObject(object, stored);
            if (!event.storesSelfReference) {
                event.holder = holderOf(stored);
                event.storesSelfReference = !!event.holder;
            }
        }
        addEvent(event, constructionNameOf(m_program, access));
    }

    void addCall(ObjectID object, const Use& use)
    {
        FlowConstructionEvent event = eventAt(FlowConstructionEventKind::Call, use);
        event.isExact = m_escape.isExactUse(object, use);
        addEvent(event);
    }

    void addOperandUse(ObjectID object, const ObjectUse& objectUse)
    {
        const Use& use = objectUse.use;
        Opcode opcode = m_function.at(use.user).opcode;
        if (isCast(opcode))
            return;
        if (std::optional<FlowConstructionEventKind> access = accessEventOf(opcode, use.operandIndex)) {
            // A store to a private name creates nothing and throws where the field is missing, so it observes the field as a read does.
            if (opcode == Opcode::PrivateSet)
                addAccess(FlowConstructionEventKind::Read, object, use);
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
        if (storedOf(user.opcode) != use.operandIndex || !receiver || accessEventOf(user.opcode, *receiver) != FlowConstructionEventKind::Store)
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
                FlowConstructionEvent event = eventAt(FlowConstructionEventKind::Leak, use);
                event.holder = call;
                addEvent(event);
            }
        }
    }

    void addExits()
    {
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (isExit(m_function.at(block).terminator.kind))
                addEvent({ .kind = FlowConstructionEventKind::Exit, .point = pointBeforeTerminatorOf(m_function, block) });
        }
    }

    void finish(FlowConstructionSummary& summary)
    {
        Vector<FlowConstructionName> usedNames;
        for (const std::optional<FlowConstructionName>& name : m_nameOfEvent) {
            if (name)
                usedNames.append(*name);
        }
        summary.names = Vector<FlowConstructionName>(FlowConstructionNames(usedNames.span()).span());
        for (size_t index = 0; index < m_events.size(); ++index) {
            if (m_nameOfEvent[index])
                m_events[index].name = summary.indexOf(*m_nameOfEvent[index]);
        }
        std::ranges::sort(m_events, { }, orderOfEvent);
        removeRepeatedElements(m_events);
        summary.events = WTF::move(m_events);
        summary.tracking = FlowConstructionTracking::Tracked;
    }

    const Program& m_program;
    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const EscapeAnalysis& m_escape;
    const UseLists& m_useLists;
    FlowConstructionRoot m_root;
    Vector<ObjectID, 4> m_containers;
    BitVector m_isContainer;
    SortedSet<ValueID> m_callsTakingObject;
    Vector<FlowConstructionEvent> m_events;
    Vector<std::optional<FlowConstructionName>> m_nameOfEvent;
};

} // anonymous namespace

// The static fields of a class are variables, and its initializer takes the class.
static bool isFieldDefinition(const Program& program, const Function& initializer, const FlowConstructionRoot& root, const FlowConstructionEvent& event)
{
    std::optional<unsigned> thisParameter = initializer.thisParameterIndex();
    if (initializer.kind != FunctionKind::Initializer || !thisParameter || root.definition != initializer.at(entryBlock).parameters[*thisParameter])
        return false;
    if (program.typeOf(initializer, root.definition).isSubsetOf(Type::function(), program))
        return false;
    if (event.kind != FlowConstructionEventKind::Store || !event.isExact)
        return false;
    const Value& definition = initializer.at(event.origin);
    if (definition.opcode != Opcode::PropDefine && definition.opcode != Opcode::PrivateDefine)
        return false;
    const Type& defined = program.typeOf(initializer, definition.valueOperand());
    return defined.isJavaScriptValue() && !defined.hasObjects();
}

std::unique_ptr<FlowFunctionConstruction> buildFunctionConstruction(const Program& program, BuildContext& build, FunctionID id)
{
    const Function& function = program.at(id);
    auto result = makeUnique<FlowFunctionConstruction>(AnalysisContext { build, program, nullptr, nullptr, id, CallResults::NotFollowed }, id, function);
    if (!function.hasBody())
        return result;

    Vector<FlowConstructionRoot, 2> roots = constructorRootsOf(program, function);
    result->returnsOnlyConstructedObject = returnsOnlyConstructedObject(program, result->analyses, roots.span());
    for (ValueID parameter : function.at(entryBlock).parameters) {
        if (!mayBeReference(program.typeOf(function, parameter)))
            continue;
        bool isRoot = roots.containsIf([&](const FlowConstructionRoot& root) {
            return root.definition == parameter;
        });
        if (!isRoot)
            roots.append({ FlowConstructionRootKind::EntryParameter, parameter });
    }

    result->summaries.reserveInitialCapacity(roots.size());
    bool leavesOutDefinitions = build.isEnabled(FlowResultKind::Initialization, id.index());
    Vector<ValueID> readsOfCreatedObject;
    Vector<ValueID> fieldDefinitions;
    for (const FlowConstructionRoot& root : roots) {
        result->summaries.append(ConstructionEventCollector(program, result->analyses, root).collect());
        for (const FlowConstructionEvent& event : result->summaries.last().events) {
            if (isReadOfCreatedObject(function, root, event))
                readsOfCreatedObject.append(event.origin);
            if (leavesOutDefinitions && isFieldDefinition(program, function, root, event))
                fieldDefinitions.append(event.origin);
        }
    }
    for (ValueID read : SortedSet<ValueID>(readsOfCreatedObject.span()))
        result->readsOfCreatedObject.append({ read });
    for (ValueID definition : SortedSet<ValueID>(fieldDefinitions.span()))
        result->fieldDefinitions.append({ definition });
    std::ranges::sort(result->summaries, { }, [](const FlowConstructionSummary& summary) {
        return summary.root.definition;
    });
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
