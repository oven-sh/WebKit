/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <tuple>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

template<typename Summary, typename Functor>
static bool forEachInspectionForLinkKnownEffects(const Summary& summary, const Functor& functor)
{
    if constexpr (requires { summary.inspections; }) {
        for (const auto& inspection : summary.inspections) {
            using Kind = std::remove_cvref_t<decltype(inspection.kind)>;
            if (!functor(inspection.subject, inspection.kind == Kind::WalksChain))
                return false;
        }
    }
    return true;
}

template<typename Call>
static uint32_t nodeThatCallWaitsForForLinkKnownEffects(const Call& call)
{
    if constexpr (requires { call.waitsFor; })
        return call.waitsFor;
    else
        return noSummaryNode;
}

uint64_t CalleeSummarizerForLinkEffects::bitsOfLayoutsAt(const LinkType& type)
{
    if (type.functions != LinkType::Functions::None || type.builtin || type.hasUnknownObjects)
        return objectsOfAnyLayout;
    switch (type.objects) {
    case LinkType::Objects::None:
        return 0;
    case LinkType::Objects::Layout:
        return m_bitsOfLayouts[type.layout.index()];
    case LinkType::Objects::Set: {
        uint64_t bits = 0;
        for (LayoutID member : m_flow.layoutsOfSet(type))
            bits |= m_bitsOfLayouts[member.index()];
        return bits;
    }
    case LinkType::Objects::Family:
        return m_bitsOfFamilies[type.family.index()];
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return objectsOfAnyLayout;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t CalleeSummarizerForLinkEffects::layoutsWrittenByChange(FunctionID id, const SummaryChange& change)
{
    uint64_t layouts = bitsOfLayoutsAt(typeAt(id, change.subject));
    if (change.kind == SummaryChange::Kind::BecomesPrototype && change.other != noSummaryNode)
        layouts |= bitsOfLayoutsAt(typeAt(id, change.other));
    return layouts;
}

uint64_t CalleeSummarizerForLinkEffects::layoutsChangedByRowsOf(FunctionID id, const ThinSummary& summary, uint32_t index, const Effects& itself)
{
    const SummaryCall& call = summary.calls[index];
    std::span<const uint32_t> arguments = argumentsOf(summary, call);
    uint64_t layouts = 0;
    bool someRowNamesNoOperand = false;
    for (BuiltinID builtin : builtinsEnteredBy(id, summary, index)) {
        forEachRowOf(builtin, call, [&](const BuiltinOverload& row) {
            bool namesOperand = false;
            auto lookAt = [&](BuiltinOperand position, uint32_t node) {
                const BuiltinParameter* parameter = m_builtins.parameterFor(row, position);
                if (parameter && (parameter->uses.containsAny(usesThatChangeOperand) || parameter->uses.containsAny(usesThatChangeShape))) {
                    namesOperand = true;
                    layouts |= bitsOfLayoutsAt(typeAt(id, node));
                }
            };
            if (call.receiver != noSummaryNode)
                lookAt(BuiltinOperand::receiver(), call.receiver);
            for (uint32_t argument = 0; argument < arguments.size() && argument <= BuiltinOperand::maxArgumentIndex; ++argument)
                lookAt(BuiltinOperand::argument(argument), arguments[argument]);
            someRowNamesNoOperand |= !namesOperand;
            return true;
        });
    }
    if (someRowNamesNoOperand && itself.writes.containsAny(heapsOfObjectsWithLayouts))
        return objectsOfAnyLayout;
    return layouts;
}

void CalleeSummarizerForLinkEffects::findLayoutsOfPlainObjects()
{
    uint32_t numberOfLayouts = m_program.layouts.size();
    m_layoutsOfPlainObjects.ensureSize(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = m_program.layouts[index];
        bool isPlain = isFoundByShapeTest(layout) && !m_flow.escapedLayouts.get(index) && !m_flow.layoutsWithKnownAccessors.get(index)
            && (!writtenBaseOf(layout) || layout.inheritedFields == InheritedFields::AllOfRow)
            && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::GetsAccessor)
            && std::ranges::none_of(layout.fields, [](const Field& field) {
                return field.form.contains(FieldForm::IsAccessor);
            });
        if (isPlain)
            m_layoutsOfPlainObjects.quickSet(index);
    }
}

std::span<const LayoutID> CalleeSummarizerForLinkEffects::layoutsNamedBy(const LinkType& type) const
{
    switch (type.objects) {
    case LinkType::Objects::Layout:
        return std::span<const LayoutID>(&type.layout, 1);
    case LinkType::Objects::Set:
        return m_flow.layoutsOfSet(type);
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CalleeSummarizerForLinkEffects::holdsOnlyPlainObjectsOfNamedLayouts(const LinkType& type) const
{
    std::span<const LayoutID> layouts = layoutsNamedBy(type);
    return !layouts.empty() && type.functions == LinkType::Functions::None && !mayBeOtherCallableForLinkEffects(type) && std::ranges::all_of(layouts, [&](LayoutID layout) {
        return m_layoutsOfPlainObjects.get(layout.index());
    });
}

bool CalleeSummarizerForLinkEffects::listingOfKeysMayRunCode(FunctionID id, const ThinSummary& summary) const
{
    auto mayRunCode = [&](uint32_t node, bool walksChain) {
        LinkType subject = typeAt(id, node);
        if (!mayBeObjectForLinkEffects(subject))
            return false;
        return !holdsOnlyPlainObjectsOfNamedLayouts(subject) || (walksChain && mayFindAnything(subject));
    };
    return !forEachInspectionForLinkKnownEffects(summary, [&](uint32_t subject, bool walksChain) {
        return !mayRunCode(subject, walksChain);
    });
}

bool CalleeSummarizerForLinkEffects::storeMayFindCode(LayoutID layout, PropertyKeyID key) const
{
    if (!key || m_program.isIndexLike(key) || m_keysThatObjectPrototypeNeverAnswers[key.index()] != Answer::Yes)
        return true;
    CodeFound found;
    lookInLayout(layout, true, key, MethodKind::Setter, false, found);
    return !found.functions.isEmpty() || found.mayBeLost || found.mayBeUnseen;
}

bool CalleeSummarizerForLinkEffects::copyMayRunCode(FunctionID id, const SummaryChange& change) const
{
    if (change.definesWhatItCopies)
        return copyThatDefinesMayRunCode(id, change);
    if (change.other == noSummaryNode)
        return true;
    LinkType source = typeAt(id, change.other);
    if (!mayBeObjectForLinkEffects(source))
        return false;
    if (!holdsOnlyPlainObjectsOfNamedLayouts(source))
        return true;
    std::span<const LayoutID> layoutsOfSource = layoutsNamedBy(source);
    for (LayoutID layout : layoutsOfSource) {
        if (m_tables.layouts[layout.index()].possibleChanges.contains(LayoutChange::GainsProperty))
            return true;
    }

    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    LinkType target = typeAt(id, change.subject);
    std::span<const LayoutID> layoutsOfTarget = layoutsNamedBy(target);
    bool targetIsOfNamedLayouts = !layoutsOfTarget.empty() && target.functions == LinkType::Functions::None && !mayBeOtherCallableForLinkEffects(target) && !(target.primitives & ~primitivesWithoutMembers);
    if (!targetIsOfNamedLayouts || mayFindAnything(target))
        return true;
    for (LayoutID from : layoutsOfSource) {
        for (LayoutID into : layoutsOfTarget) {
            for (const Field& field : m_program.at(from).fields) {
                if (storeMayFindCode(into, field.key))
                    return true;
            }
            for (PropertyKeyID key : m_tables.appendedKeysOf(from)) {
                if (storeMayFindCode(into, key))
                    return true;
            }
        }
    }
    return false;
}

bool CalleeSummarizerForLinkEffects::membersOfInstancesAreAsTableLists(const LinkType& receiver, PropertyKeyID key) const
{
    if (!key || receiver.classes != LinkType::Classes::Site || receiver.objects != LinkType::Objects::None || receiver.functions != LinkType::Functions::None || receiver.builtin || receiver.hasUnknownObjects)
        return false;
    if (m_flow.hasEscaped(receiver.site))
        return false;
    auto holdsNothing = [&](SlotOfContents slot) {
        FlowNodeID node = m_flow.findContentsOf(receiver, slot);
        return !node || m_flow.typeAt(node).isNever();
    };
    return holdsNothing(SlotOfContents::under(key)) && holdsNothing(SlotOfContents::underUnknownNames()) && holdsNothing(SlotOfContents::underAnyName());
}

bool CalleeSummarizerForLinkEffects::addLibraryFunctionBehindRow(const BuiltinOverload& row, CodeFound& found) const
{
    const RuntimeFunctionEntry* held = findRuntimeFunction(m_program, row.libraryFunction());
    if (!held || !held->function)
        return false;
    found.add(held->function);
    return true;
}

bool CalleeSummarizerForLinkEffects::callStillWaits(FunctionID id, const SummaryCall& call) const
{
    uint32_t awaited = nodeThatCallWaitsForForLinkKnownEffects(call);
    return awaited != noSummaryNode && typeAt(id, awaited).isNever();
}

bool CalleeSummarizerForLinkEffects::isEnteredByNobody(FunctionID id) const
{
    if (m_tables.function(id).everyCallerIsKnown)
        return false;
    const Function& function = m_program.at(id);
    for (uint32_t index = 0; index < function.numberOfEntryParameters(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if ((kind == EntryParameter::This || kind == EntryParameter::Parameter) && m_flow.typeAt(m_flow.parameter(id, index)).isNever())
            return true;
    }
    return false;
}

bool CalleeSummarizerForLinkEffects::callNeverRuns(FunctionID id, const SummaryCall& call) const
{
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        return callStillWaits(id, call);
    case SummaryCall::Callee::Name:
        return typeAt(id, call.receiver).isNever();
    case SummaryCall::Callee::Value:
        return typeAt(id, call.subject).isNever();
    case SummaryCall::Callee::Builtin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CalleeSummarizerForLinkEffects::takeKnownEffects(const Effects& all, const LocationMasks& writesOnReturn, uint64_t layoutsWritten, WayInKnownEffects way, EffectsOfComponentInKnownEffects& into)
{
    into.all.merge(all);
    into.layoutsWritten |= layoutsWritten;
    switch (way) {
    case WayInKnownEffects::ToNoReturn:
        return;
    case WayInKnownEffects::ToReturn:
        into.writesOnReturn.merge(writesOnReturn);
        return;
    case WayInKnownEffects::ToReturnUnderHandler:
        into.writesOnReturn.merge(all.writes);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CalleeSummarizerForLinkEffects::takeKnownEffectsOfFunction(FunctionID callee, WayInKnownEffects way, EffectsOfComponentInKnownEffects& into) const
{
    const LinkedFunction& linked = m_tables.function(callee);
    takeKnownEffects(linked.effectsOnThrow(), linked.effectsOnReturn().writes, m_layoutsWrittenByFunction[callee.index()], way, into);
}

bool CalleeSummarizerForLinkEffects::mergeKnownEffectsOfCodeOutside(const CodeEntered& code, uint32_t component, WayInKnownEffects way, EffectsOfComponentInKnownEffects& effects) const
{
    bool everythingStandsOutside = true;
    for (FunctionID callee : functionsOf(code)) {
        if (m_components.componentOfFunction[callee.index()] == component) {
            everythingStandsOutside = false;
            continue;
        }
        if (isSharedCall(callee.index())) {
            const EffectsOfComponentInKnownEffects& shared = m_knownEffectsOfSharedCalls[callee.index() - nodeOfSharedCall(0)];
            takeKnownEffects(shared.all, shared.writesOnReturn, shared.layoutsWritten, way, effects);
            continue;
        }
        takeKnownEffectsOfFunction(callee, way, effects);
    }
    if (!code.mayBeLost)
        return everythingStandsOutside;
    if (m_components.componentOfFunction[nodeOfLostFunctions()] == component)
        return false;
    takeKnownEffects(m_knownEffectsOfLostFunctions.all, m_knownEffectsOfLostFunctions.writesOnReturn, m_knownEffectsOfLostFunctions.layoutsWritten, way, effects);
    return everythingStandsOutside;
}

std::optional<uint16_t> CalleeSummarizerForLinkEffects::expectedParameterPrimitives(FunctionID id, uint32_t entryParameter) const
{
    std::span<const LinkedParameter> parameters = m_tables.parametersOf(id);
    TypeID expected = entryParameter < parameters.size() ? parameters[entryParameter].writtenOrSeenToArrive() : TypeID();
    if (!expected || !holdsOnlyPrimitives(expected))
        return std::nullopt;
    Type type = m_program.types.at(expected);
    return LinkType::closed(LinkType::primitivesOf(type.primitives, type.integers));
}

uint32_t CalleeSummarizerForLinkEffects::findSoleObjectParameters(FunctionID id, const ThinSummary& summary, uint32_t node, GivenArgumentsInKnownEffects& given) const
{
    if (node == noSummaryNode || summary.nodes[node].kind != SummaryNode::Kind::Join)
        return 0;
    static constexpr uint32_t othersBringObjects = ~0u;
    if (given.soleObjectParameters.isEmpty()) {
        given.soleObjectParameters.fill(0, summary.nodes.size());
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to >= summary.nodes.size() || summary.nodes[edge.to].kind != SummaryNode::Kind::Join)
                continue;
            uint32_t& parameters = given.soleObjectParameters[edge.to];
            if (parameters == othersBringObjects)
                continue;
            std::optional<uint32_t> parameter = edge.passes == TypeTable::object ? entryParameterAt(summary, edge.from) : std::nullopt;
            parameters = parameter && expectedParameterPrimitives(id, *parameter) ? parameters | 1u << *parameter : othersBringObjects;
        }
    }
    uint32_t parameters = given.soleObjectParameters[node];
    return parameters == othersBringObjects ? 0 : parameters;
}

void CalleeSummarizerForLinkEffects::addEffectsOfOperatorsLeftApart(FunctionID id, const ThinSummary& summary, EffectsOfComponentInKnownEffects& effects, GivenArgumentsInKnownEffects& given) const
{
    std::optional<Effects> boundOfOperators = summary.ownEffectsOfOperators();
    if (!boundOfOperators)
        return;
    constexpr uint16_t whatBecomesNumber = LinkType::closed(Primitive::NumberLike);
    constexpr uint16_t plainPrimitives = LinkType::closed(Primitive::NumberLike | Primitive::AnyString);
    bool mayTakeStrings = false;
    bool mayTakeOtherValues = false;
    for (uint32_t node : summary.operandsOfOperatorsLeftApart) {
        LinkType type = typeAt(id, summary, node);
        if (type.isNever() || mayBeObjectForLinkEffects(type) || (type.primitives & ~plainPrimitives)) {
            std::optional<uint32_t> parameter = type.isNever() ? std::nullopt : entryParameterAt(summary, node);
            std::optional<uint16_t> expected = parameter ? expectedParameterPrimitives(id, *parameter) : std::nullopt;
            if (expected && !(*expected & ~plainPrimitives)) {
                given.parametersTakenAsExpected |= 1u << *parameter;
                mayTakeStrings |= !!(*expected & ~whatBecomesNumber);
                continue;
            }
            mayTakeOtherValues = true;
        }
        mayTakeStrings |= !!(type.primitives & ~whatBecomesNumber);
    }

    if (!mayTakeOtherValues && !mayTakeStrings)
        return;
    Effects ofOperators = *boundOfOperators;
    if (!mayTakeOtherValues && !summary.operatorsLeftApartMayJoinTexts && ofOperators.mayThrow)
        ofOperators.mayThrow = ThrowEffect::onlyForLackOfMemory();
    takeKnownEffects(ofOperators, ofOperators.writes, 0, WayInKnownEffects::ToReturn, effects);
}

bool CalleeSummarizerForLinkEffects::readMayMakeClosureOfField(const LinkType& subject, PropertyKeyID key) const
{
    if (!m_keysOfFieldsThatHoldOneClosure.quickGet(key.index()))
        return false;
    auto holdsClosures = [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(m_program, layout, key);
        return field && m_flow.typeAt(m_flow.field(*field)).functions == LinkType::Functions::Closure;
    };
    switch (subject.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
        return holdsClosures(subject.layout);
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(subject), holdsClosures);
    case LinkType::Objects::Family:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CalleeSummarizerForLinkEffects::addEffectsOfAccessItself(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, const LinkType& subject, PropertyKeyID key, WayInKnownEffects way, EffectsOfComponentInKnownEffects& effects)
{
    Effects ofLookup = effectsOfLookupItself(access.kind, key);
    uint64_t layouts = 0;
    switch (access.kind) {
    case SummaryAccess::Kind::Read:
        if (thenLookupCannotThrow(subject, key))
            ofLookup.mayThrow = ThrowEffect(false);
        if (key && readMayMakeClosureOfField(subject, key)) {
            ofLookup.writes.add(m_program, MemoryLocation::objectField(key));
            layouts = bitsOfLayoutsAt(subject);
        }
        break;
    case SummaryAccess::Kind::Has:
        break;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define:
        if (access.isPartOfConstruction && summary.nodes[access.subject].kind == SummaryNode::Kind::Allocation) {
            ofLookup.writes = ofLookup.writes.restrictedTo(unobservableWriteHeaps);
            break;
        }
        layouts = bitsOfLayoutsAt(subject);
        if (access.receiver != noSummaryNode)
            layouts |= bitsOfLayoutsAt(typeAt(id, access.receiver));
        break;
    }
    takeKnownEffects(ofLookup, ofLookup.writes, layouts, way, effects);
}

std::optional<uint32_t> CalleeSummarizerForLinkEffects::entryParameterAt(const ThinSummary& summary, uint32_t node)
{
    constexpr uint32_t numberOfParametersWithBit = 32;
    if (node == noSummaryNode)
        return std::nullopt;
    const SummaryNode& found = summary.nodes[node];
    return found.kind == SummaryNode::Kind::Parameter && found.first < numberOfParametersWithBit ? std::optional(found.first) : std::nullopt;
}

std::optional<LinkType> CalleeSummarizerForLinkEffects::typeExpectedOfParameter(FunctionID id, uint32_t entryParameter) const
{
    std::span<const LinkedParameter> parameters = m_tables.parametersOf(id);
    TypeID expected = entryParameter < parameters.size() ? parameters[entryParameter].writtenOrSeenToArrive() : TypeID();
    if (!expected || holdsOnlyPrimitives(expected))
        return std::nullopt;
    TypeReader reader(m_program);
    Type type = m_program.types.at(expected);
    if (type.opaque || type.environment || type.objects || type.hasUnboundedUnknown() || !classPartsOf(reader, type).empty())
        return std::nullopt;
    std::span<const LayoutID> layouts = membersOf(reader, type);
    if (layouts.size() != 1)
        return std::nullopt;
    LinkType result;
    result.primitives = LinkType::closed(LinkType::primitivesOf(type.primitives, type.integers));
    result.objects = LinkType::Objects::Layout;
    result.layout = layouts[0];
    return result;
}

bool CalleeSummarizerForLinkEffects::mergeKnownEffectsOfCodeFound(const CodeFound& found, uint32_t component, WayInKnownEffects way, EffectsOfComponentInKnownEffects& effects) const
{
    if (found.mayBeUnseen || found.mayBeLost)
        return false;
    for (FunctionID callee : found.functions) {
        if (!isFunction(callee.index()) || m_components.componentOfFunction[callee.index()] >= component)
            return false;
        takeKnownEffectsOfFunction(callee, way, effects);
    }
    return true;
}

void CalleeSummarizerForLinkEffects::addFunctionsCalledUnder(LayoutID layout, PropertyKeyID key, CodeFound& found) const
{
    if (m_tables.layouts[layout.index()].possibleChanges.contains(LayoutChange::GainsProperty))
        found.mayBeUnseen = true;
    LinkType subject;
    subject.objects = LinkType::Objects::Layout;
    subject.layout = layout;
    if (mayFindFunctionInFieldUnder(subject, key))
        found.mayBeUnseen = true;
    for (const LinkFlow::FoundMember& member : m_flow.findMembers(layout, ClassSide::Prototype, key)) {
        if (member.kind == MethodKind::Method)
            addFunctionsAt(m_flow.typeAt(m_flow.member(member.classLayout, ClassSide::Prototype, member.index)), found);
    }
    uint32_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
        if (const LinkFlow::NameOnPrototype* name = m_flow.findNameOnPrototype(current, key))
            addFunctionsAt(m_flow.typeAt(name->stored), found);
    }
}

bool CalleeSummarizerForLinkEffects::judgeAccessGivenArguments(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key, uint32_t component, WayInKnownEffects way, EffectsOfComponentInKnownEffects& effects, GivenArgumentsInKnownEffects& given)
{
    std::optional<uint32_t> parameter = entryParameterAt(summary, access.subject);
    std::optional<LinkType> expected = parameter ? typeExpectedOfParameter(id, *parameter) : std::nullopt;
    if (!expected || access.receiver != noSummaryNode)
        return false;
    CodeFound found;
    findCodeBehindLookup(*expected, key, access.kind, false, found);
    if (!mergeKnownEffectsOfCodeFound(found, component, way, effects))
        return false;
    addEffectsOfAccessItself(id, summary, access, *expected, key, way, effects);
    given.parametersTakenAsExpected |= 1u << *parameter;
    return true;
}

bool CalleeSummarizerForLinkEffects::judgeCallGivenArguments(FunctionID id, const ThinSummary& summary, const SummaryCall& call, uint32_t component, WayInKnownEffects way, EffectsOfComponentInKnownEffects& effects, GivenArgumentsInKnownEffects& given)
{
    if (call.isConstruction || call.newTarget != noSummaryNode)
        return false;
    Effects itself = Effects::none();
    itself.mayThrow = ThrowEffect(true);
    if (call.passesList)
        itself.reads.add(AbstractHeapSet { AbstractHeap::Elements, AbstractHeap::ArrayStorage });

    switch (call.callee) {
    case SummaryCall::Callee::Binding:
    case SummaryCall::Callee::Builtin:
        return false;
    case SummaryCall::Callee::Value: {
        std::optional<uint32_t> parameter = entryParameterAt(summary, call.subject);
        if (!parameter)
            return false;
        takeKnownEffects(itself, itself.writes, 0, way, effects);
        given.parametersCalled |= 1u << *parameter;
        return true;
    }
    case SummaryCall::Callee::Name: {
        if (uint32_t bringers = call.isMadeByProtocol ? findSoleObjectParameters(id, summary, call.receiver, given) : 0) {
            given.parametersTakenAsExpected |= bringers;
            return true;
        }
        std::optional<uint32_t> parameter = entryParameterAt(summary, call.receiver);
        std::optional<LinkType> expected = parameter ? typeExpectedOfParameter(id, *parameter) : std::nullopt;
        if (!expected || !call.key || findOwnMember(m_program, m_builtins.canonicalID(Builtins::ObjectPrototype), call.key))
            return false;
        CodeFound found;
        findCodeBehindLookup(*expected, call.key, SummaryAccess::Kind::Read, true, found);
        addFunctionsCalledUnder(expected->layout, call.key, found);
        if (!mergeKnownEffectsOfCodeFound(found, component, way, effects))
            return false;
        itself.merge(effectsOfLookupItself(SummaryAccess::Kind::Read, call.key));
        bool mayMakeClosure = readMayMakeClosureOfField(*expected, call.key);
        if (mayMakeClosure)
            itself.writes.add(m_program, MemoryLocation::objectField(call.key));
        takeKnownEffects(itself, itself.writes, mayMakeClosure ? bitsOfLayoutsAt(*expected) : 0, way, effects);
        given.parametersTakenAsExpected |= 1u << *parameter;
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addEffectsOfSettledPromiseInKnownEffects(Effects& all, LocationMasks& writesOnReturn)
{
    Effects ofPromise = Effects::none();
    ofPromise.reads = LocationMasks::top();
    ofPromise.writes.add(AbstractHeap::AllocatorState);
    ofPromise.writes.add(AbstractHeap::RuntimeState);
    ofPromise.writes.add(AbstractHeap::PromiseState);
    ofPromise.writes.add(AbstractHeap::ExternalState);
    ofPromise.mayCollect = true;
    all.merge(ofPromise);
    writesOnReturn.merge(ofPromise.writes);
}

auto CalleeSummarizerForLinkEffects::knownEffectsOfBody(uint32_t function) -> EffectsOfComponentInKnownEffects
{
    FunctionID id(function);
    const ThinSummary& summary = m_flow.summaries.at(id);
    uint32_t component = m_components.componentOfFunction[function];
    bool catchesWhatIsThrown = summary.someBlockHasHandler || m_program.at(id).isCoroutine();
    auto wayOf = [&](bool liesOnNoWayToReturn) {
        if (liesOnNoWayToReturn)
            return WayInKnownEffects::ToNoReturn;
        return catchesWhatIsThrown ? WayInKnownEffects::ToReturnUnderHandler : WayInKnownEffects::ToReturn;
    };

    bool isKnown = !keepsDefaults(id, summary, { SummaryTrait::Suspends, SummaryTrait::HasDirectEval }) && !summary.ownEffectsMayCallUnknownCode()
        && !handsObjectToCodeNobodyNames(id, summary) && !listingOfKeysMayRunCode(id, summary) && !isEnteredByNobody(id);
    EffectsOfComponentInKnownEffects effects;
    GivenArgumentsInKnownEffects given;
    if (isKnown) {
        Effects ownEffects = summary.ownEffects();
        effects.all = ownEffects;
        effects.writesOnReturn = summary.ownWritesOnWaysToReturn.isEmpty() ? ownEffects.writes : summary.ownWritesOnWaysToReturn[0];
        effects.layoutsWritten = summary.layoutsOfOwnWrites();
        if (m_tables.function(id).runsAtMostOnce) {
            LocationMasks wordsInPlaceOfCells;
            if (!summary.sitesThatHandOverCaptures.isEmpty())
                wordsInPlaceOfCells.add(AbstractHeap::ModuleVariables, locationMaskOfBit(Program::locationBitOfVariablesThatPlanAdds));
            if (!summary.environmentsMade.isEmpty())
                wordsInPlaceOfCells.add(AbstractHeap::EnvironmentFields);
            effects.all.writes.merge(wordsInPlaceOfCells);
            effects.writesOnReturn.merge(wordsInPlaceOfCells);
        }
        if (ownEffects.reads.contains(AbstractHeap::ModuleVariables)) {
            for (const SummaryNode& node : summary.nodes) {
                if (node.kind != SummaryNode::Kind::Variable || !m_variablesThatFirstReadMayFill.quickGet(node.first))
                    continue;
                effects.all.writes.add(m_program, MemoryLocation::moduleVariable(VariableID(node.first)));
                effects.writesOnReturn.add(m_program, MemoryLocation::moduleVariable(VariableID(node.first)));
            }
        }
        addEffectsOfOperatorsLeftApart(id, summary, effects, given);
        for (const SummaryChange& change : summary.changes) {
            if (changeMayRunCode(id, change)) {
                isKnown = false;
                break;
            }
            effects.layoutsWritten |= layoutsWrittenByChange(id, change);
        }
    }

    bool entersOwnComponent = false;
    for (uint32_t index = 0; isKnown && index < summary.calls.size(); ++index) {
        const SummaryCall& call = summary.calls[index];
        const CodeEntered& code = codeOfCall(id, index);
        if (callNeverRuns(id, call))
            continue;
        WayInKnownEffects way = wayOf(call.liesOnNoWayToReturn);
        Effects itself = code.mayBeUnseen ? Effects::unknownCode() : effectsOfCallItself(id, summary, index);
        if (itself.mayCallUnknownCode) {
            isKnown = judgeCallGivenArguments(id, summary, call, component, way, effects, given);
            continue;
        }
        EffectsOfComponentInKnownEffects ofCall { itself, itself.writes, layoutsChangedByRowsOf(id, summary, index, itself) };
        if (call.callee == SummaryCall::Callee::Name && call.key && readMayMakeClosureOfField(typeAt(id, call.receiver), call.key))
            ofCall.layoutsWritten |= bitsOfLayoutsAt(typeAt(id, call.receiver));
        bool isEnteredByCallAsWritten = !call.isMadeByProtocol && builtinsEnteredBy(id, summary, index).empty();
        entersOwnComponent |= !mergeKnownEffectsOfCodeOutside(code, component, isEnteredByCallAsWritten ? WayInKnownEffects::ToReturn : WayInKnownEffects::ToReturnUnderHandler, ofCall);
        takeKnownEffects(ofCall.all, ofCall.writesOnReturn, ofCall.layoutsWritten, way, effects);
    }

    for (uint32_t index = 0; isKnown && index < summary.accesses.size(); ++index) {
        const SummaryAccess& access = summary.accesses[index];
        LinkType subject = typeAt(id, summary, access.subject);
        if (access.isToContents()) {
            if (access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define)
                effects.layoutsWritten |= bitsOfLayoutsAt(subject);
            continue;
        }
        if (subject.isNever())
            continue;
        const CodeEntered& code = codeOfAccess(id, index);
        PropertyKeyID key = m_flow.keyOf(id, access);
        WayInKnownEffects way = access.liesOnNoWayToReturn ? WayInKnownEffects::ToNoReturn : WayInKnownEffects::ToReturnUnderHandler;
        if (code.mayBeUnseen) {
            isKnown = judgeAccessGivenArguments(id, summary, access, key, component, way, effects, given);
            continue;
        }
        addEffectsOfAccessItself(id, summary, access, subject, key, way, effects);
        entersOwnComponent |= !mergeKnownEffectsOfCodeOutside(code, component, way, effects);
    }

    if (!isKnown)
        return knownEffectsOfUnknownCode();
    if (m_program.at(id).isCoroutine())
        addEffectsOfSettledPromiseInKnownEffects(effects.all, effects.writesOnReturn);
    if (!given.parametersTakenAsExpected && !given.parametersCalled)
        return effects;
    if (!entersOwnComponent && !effects.all.mayCallUnknownCode) {
        finishKnownEffectsOfCall(effects, false);
        Effects onReturn = effects.all;
        onReturn.writes = effects.writesOnReturn;
        s_hand->effectsGivenArguments.append(LinkedEffectsGivenArguments { id, given.parametersTakenAsExpected, given.parametersCalled, effects.layoutsWritten, onReturn, effects.all });
    }
    return knownEffectsOfUnknownCode();
}

void CalleeSummarizerForLinkEffects::finishKnownEffectsOfCall(EffectsOfComponentInKnownEffects& effects, bool mayEnterItself)
{
    effects.all.mayThrow |= mayEnterItself ? ThrowEffect(true) : ThrowEffect::onlyStackOverflow();
    // The check of the stack at frame setup is a poll too, and the collector may stop the thread there.
    effects.all.mayCollect = true;
    effects.all.maySuspend = false;
    if (mayEnterItself)
        effects.writesOnReturn = effects.all.writes;
}

void CalleeSummarizerForLinkEffects::prepareKnownEffects()
{
    m_layoutsWrittenByFunction.fill(objectsOfAnyLayout, m_numberOfFunctions);
    m_knownEffectsOfSharedCalls.fill(knownEffectsOfUnknownCode(), m_flow.calleesOfSharedCalls.size());
    m_bitsOfLayouts = bitsOfLayoutsInKnownEffects(m_program);
    const Families& families = m_flow.families;
    m_bitsOfFamilies.fill(0, families.size());
    for (uint32_t layout = 0; layout < m_bitsOfLayouts.size(); ++layout) {
        if (FamilyID nearest = families.nearestFamilyOf(LayoutID(layout)))
            m_bitsOfFamilies[nearest.index()] |= m_bitsOfLayouts[layout];
    }
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (FamilyID above = families.nearestFamilyAbove(FamilyID(family)))
            m_bitsOfFamilies[above.index()] |= m_bitsOfFamilies[family];
    }
    findLayoutsOfPlainObjects();
    m_variablesThatFirstReadMayFill.ensureSize(m_program.variables.size());
    for (const Module& module : m_program.modules) {
        if (!module.body)
            continue;
        for (const SummaryDefinition& definition : m_flow.summaries.at(module.body).definitions) {
            if (!definition.makesClosure)
                m_variablesThatFirstReadMayFill.quickSet(definition.variable.index());
        }
    }
}

void CalleeSummarizerForLinkEffects::findComponentsThatExpectationsLeadTo()
{
    m_componentsThatExpectationsLeadTo = collectInParallel<uint64_t>(m_flow.loops, m_numberOfFunctions, [&](unsigned index, Vector<uint64_t>& found) {
        if (!isSummarized(index))
            return;
        FunctionID id(index);
        const ThinSummary& summary = m_flow.summaries.at(id);
        uint32_t component = m_components.componentOfFunction[index];
        auto list = [&](const CodeFound& code) {
            if (code.mayBeUnseen || code.mayBeLost)
                return;
            for (FunctionID callee : code.functions) {
                if (isFunction(callee.index()) && m_components.componentOfFunction[callee.index()] < component)
                    found.append(static_cast<uint64_t>(component) << 32 | m_components.componentOfFunction[callee.index()]);
            }
        };
        for (const SummaryCall& call : summary.calls) {
            if (call.callee != SummaryCall::Callee::Name || !call.key || call.isConstruction || call.newTarget != noSummaryNode)
                continue;
            std::optional<uint32_t> parameter = entryParameterAt(summary, call.receiver);
            std::optional<LinkType> expected = parameter ? typeExpectedOfParameter(id, *parameter) : std::nullopt;
            if (!expected)
                continue;
            CodeFound code;
            findCodeBehindLookup(*expected, call.key, SummaryAccess::Kind::Read, true, code);
            addFunctionsCalledUnder(expected->layout, call.key, code);
            list(code);
        }
        for (uint32_t place = 0; place < summary.accesses.size(); ++place) {
            const SummaryAccess& access = summary.accesses[place];
            if (access.isToContents() || access.receiver != noSummaryNode || !codeOfAccess(id, place).mayBeUnseen)
                continue;
            std::optional<uint32_t> parameter = entryParameterAt(summary, access.subject);
            std::optional<LinkType> expected = parameter ? typeExpectedOfParameter(id, *parameter) : std::nullopt;
            if (!expected)
                continue;
            CodeFound code;
            findCodeBehindLookup(*expected, m_flow.keyOf(id, access), access.kind, false, code);
            list(code);
        }
    });
    std::ranges::sort(m_componentsThatExpectationsLeadTo);
}

void CalleeSummarizerForLinkEffects::rankComponents()
{
    const ComponentsOfLinkEffects& components = m_components;
    Vector<uint32_t> rankOfComponent;
    rankOfComponent.fill(0, components.size());
    Vector<uint32_t> numberOfRank;
    size_t nextExpectation = 0;
    for (uint32_t component = 0; component < components.size(); ++component) {
        uint32_t rank = 0;
        for (uint32_t member : components.membersOf(component)) {
            for (FunctionID callee : calleesOf(member)) {
                uint32_t ledTo = components.componentOfFunction[callee.index()];
                if (ledTo != component)
                    rank = std::max(rank, rankOfComponent[ledTo] + 1);
            }
        }
        for (; nextExpectation < m_componentsThatExpectationsLeadTo.size() && m_componentsThatExpectationsLeadTo[nextExpectation] >> 32 == component; ++nextExpectation)
            rank = std::max(rank, rankOfComponent[static_cast<uint32_t>(m_componentsThatExpectationsLeadTo[nextExpectation])] + 1);
        rankOfComponent[component] = rank;
        while (numberOfRank.size() <= rank)
            numberOfRank.append(0);
        ++numberOfRank[rank];
    }

    m_firstComponentOfRank.reserveInitialCapacity(numberOfRank.size() + 1);
    uint32_t first = 0;
    for (uint32_t number : numberOfRank) {
        m_firstComponentOfRank.append(first);
        first += number;
    }
    m_firstComponentOfRank.append(first);
    Vector<uint32_t> next = m_firstComponentOfRank;
    m_componentsInRanks.grow(components.size());
    for (uint32_t component = 0; component < components.size(); ++component)
        m_componentsInRanks[next[rankOfComponent[component]]++] = component;
    m_firstMemberInRanks.reserveInitialCapacity(components.size() + 1);
    m_membersInRanks.reserveInitialCapacity(components.members.size());
    for (uint32_t component : m_componentsInRanks) {
        m_firstMemberInRanks.append(m_membersInRanks.size());
        m_membersInRanks.append(components.membersOf(component));
    }
    m_firstMemberInRanks.append(m_membersInRanks.size());
}

void CalleeSummarizerForLinkEffects::closeKnownEffectsOf(uint32_t component, std::span<const uint32_t> members, EffectsOfComponentInKnownEffects& effects)
{
    bool mayEnterItself = members.size() > 1 || std::ranges::any_of(calleesOf(members.front()), [&](FunctionID callee) {
        return callee.index() == members.front();
    });
    finishKnownEffectsOfCall(effects, mayEnterItself);

    if (component == m_components.componentOfFunction[nodeOfLostFunctions()])
        m_knownEffectsOfLostFunctions = effects;
    const Effects* onReturn = nullptr;
    const Effects* onThrow = nullptr;
    for (uint32_t member : members) {
        if (isSharedCall(member))
            m_knownEffectsOfSharedCalls[member - nodeOfSharedCall(0)] = effects;
        if (!isFunction(member))
            continue;
        if (!onThrow) {
            Effects returned = effects.all;
            returned.writes = effects.writesOnReturn;
            onThrow = s_hand->effectsOfFunctions.keep(effects.all);
            onReturn = returned == effects.all ? onThrow : s_hand->effectsOfFunctions.keep(returned);
        }
        LinkedFunction& linked = m_tables.functions[member];
        linked.keptEffectsOnReturn = onReturn;
        linked.keptEffectsOnThrow = onThrow;
        m_layoutsWrittenByFunction[member] = effects.layoutsWritten;
    }
}

auto CalleeSummarizerForLinkEffects::knownEffectsOfMember(uint32_t component, uint32_t member) -> EffectsOfComponentInKnownEffects
{
    if (isFunction(member))
        return knownEffectsOfBody(member);
    EffectsOfComponentInKnownEffects effects;
    for (FunctionID listed : calleesOf(member)) {
        if (m_components.componentOfFunction[listed.index()] != component)
            takeKnownEffectsOfFunction(listed, WayInKnownEffects::ToReturn, effects);
    }
    return effects;
}

void CalleeSummarizerForLinkEffects::summarizeKnownEffectsOf(uint32_t component, std::span<const uint32_t> members, size_t positionOfFirstMember)
{
    EffectsOfComponentInKnownEffects effects;
    for (size_t place = 0; place < members.size(); ++place) {
        prefetchSummaryOfMemberBehindInRanks(positionOfFirstMember + place);
        EffectsOfComponentInKnownEffects ofMember = knownEffectsOfMember(component, members[place]);
        takeKnownEffects(ofMember.all, ofMember.writesOnReturn, ofMember.layoutsWritten, WayInKnownEffects::ToReturn, effects);
    }
    closeKnownEffectsOf(component, members, effects);
}

void CalleeSummarizerForLinkEffects::summarizeKnownEffects()
{
    prepareKnownEffects();
    findComponentsThatExpectationsLeadTo();
    rankComponents();

    static constexpr uint32_t membersOfLargeComponent = 256;
    static constexpr uint32_t membersInStretch = 32;
    const ParallelLoops& loops = m_flow.loops;
    Vector<HandOfSummarizer> hands(loops.width());
    struct Stretch {
        enum class Kind : uint8_t { Components, MembersOfLargeComponent };
        Kind kind;
        uint32_t first;
        uint32_t end;
        uint32_t firstMember { 0 };
        uint32_t endOfMembers { 0 };
    };
    Vector<Stretch> stretches;
    Vector<uint32_t> largeComponents;
    Vector<EffectsOfComponentInKnownEffects> effectsOfStretches;
    auto isSkipped = [&](uint32_t place) {
        uint32_t front = m_membersInRanks[m_firstMemberInRanks[place]];
        return isFunction(front) && !isSummarized(front);
    };

    for (uint32_t rank = 0; rank + 1 < m_firstComponentOfRank.size(); ++rank) {
        stretches.shrink(0);
        largeComponents.shrink(0);
        uint32_t endOfRank = m_firstComponentOfRank[rank + 1];
        for (uint32_t place = m_firstComponentOfRank[rank]; place < endOfRank; ++place) {
            if (m_firstMemberInRanks[place + 1] - m_firstMemberInRanks[place] < membersOfLargeComponent)
                continue;
            largeComponents.append(place);
            for (uint32_t member = m_firstMemberInRanks[place]; member < m_firstMemberInRanks[place + 1]; member += membersInStretch)
                stretches.append(Stretch { Stretch::Kind::MembersOfLargeComponent, place, place + 1, member, std::min(member + membersInStretch, m_firstMemberInRanks[place + 1]) });
        }
        for (uint32_t place = m_firstComponentOfRank[rank]; place < endOfRank;) {
            uint32_t first = place;
            uint32_t numberOfMembers = 0;
            for (; place < endOfRank && numberOfMembers < membersInStretch; ++place) {
                uint32_t size = m_firstMemberInRanks[place + 1] - m_firstMemberInRanks[place];
                if (size >= membersOfLargeComponent)
                    break;
                numberOfMembers += size;
            }
            if (place > first)
                stretches.append(Stretch { Stretch::Kind::Components, first, place });
            else
                ++place;
        }
        effectsOfStretches.fill(EffectsOfComponentInKnownEffects { }, largeComponents.isEmpty() ? 0 : stretches.size());

        forEachIndexInParallel(loops, static_cast<unsigned>(stretches.size()), [&](unsigned index, unsigned worker) {
            s_hand = &hands[worker];
            const Stretch& stretch = stretches[index];
            switch (stretch.kind) {
            case Stretch::Kind::Components:
                for (uint32_t place = stretch.first; place < stretch.end; ++place) {
                    if (isSkipped(place))
                        continue;
                    std::span<const uint32_t> members = m_membersInRanks.span().subspan(m_firstMemberInRanks[place], m_firstMemberInRanks[place + 1] - m_firstMemberInRanks[place]);
                    summarizeKnownEffectsOf(m_componentsInRanks[place], members, m_firstMemberInRanks[place]);
                }
                break;
            case Stretch::Kind::MembersOfLargeComponent:
                for (uint32_t position = stretch.firstMember; position < stretch.endOfMembers; ++position) {
                    prefetchSummaryOfMemberBehindInRanks(position);
                    EffectsOfComponentInKnownEffects ofMember = knownEffectsOfMember(m_componentsInRanks[stretch.first], m_membersInRanks[position]);
                    takeKnownEffects(ofMember.all, ofMember.writesOnReturn, ofMember.layoutsWritten, WayInKnownEffects::ToReturn, effectsOfStretches[index]);
                }
                break;
            }
            s_hand = nullptr;
        });

        s_hand = &hands[0];
        for (uint32_t place : largeComponents) {
            uint32_t component = m_componentsInRanks[place];
            EffectsOfComponentInKnownEffects effects;
            for (size_t other = 0; other < stretches.size(); ++other) {
                if (stretches[other].kind == Stretch::Kind::MembersOfLargeComponent && stretches[other].first == place)
                    takeKnownEffects(effectsOfStretches[other].all, effectsOfStretches[other].writesOnReturn, effectsOfStretches[other].layoutsWritten, WayInKnownEffects::ToReturn, effects);
            }
            closeKnownEffectsOf(component, m_membersInRanks.span().subspan(m_firstMemberInRanks[place], m_firstMemberInRanks[place + 1] - m_firstMemberInRanks[place]), effects);
        }
        s_hand = nullptr;
    }

    for (HandOfSummarizer& hand : hands) {
        m_effectsGivenArguments.appendVector(WTF::move(hand.effectsGivenArguments));
        m_blocksOfDistinctEffects.appendVector(hand.effectsOfFunctions.takeBlocks());
    }
}

void CalleeSummarizerForLinkEffects::writeKnownEffects()
{
    LinkKnownEffects& known = m_tables.known;
    known.layoutsWrittenByFunction = WTF::move(m_layoutsWrittenByFunction);
    known.givenArguments = WTF::move(m_effectsGivenArguments);
    known.blocksOfDistinctEffects = WTF::move(m_blocksOfDistinctEffects);
    std::ranges::sort(known.givenArguments, { }, &LinkedEffectsGivenArguments::function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
