/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Members of CalleeSummarizerForLinkEffects, which AOTLinkEffects.cpp declares. That file includes this one at its foot.
// ONE traversal, callees first, in the place of summarizeEffects(): what a function does by whichever exit, what it has written when it returns, and whose objects it writes.
// On the way it leaves what a call of several functions does, and what a function does where its arguments are as expected. No list here holds functions x functions or members x calls.

#if ENABLE(AOT)

#include <tuple>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

// ---- What neighbors of the port lay into the summaries. Where one of them is not in the tree the scan says what it says today, and each of these answers that there is nothing.

// key-orders-and-copies: Joins that only objects pass into. An operation of the body runs Protocol::OwnKeys or OwnValues on what lies there.
template<typename Summary>
static std::span<const uint32_t> nodesWhoseKeysAreListedForLinkKnownEffects(const Summary& summary)
{
    if constexpr (requires { summary.nodesWhoseKeysAreListed; })
        return summary.nodesWhoseKeysAreListed.span();
    else
        return { };
}

// unions-and-narrowing: whose chain an operation walks, whose keys it lists, in the place of Effects::unknownCode() and SummaryTrait::ListsKeys for the whole body. functor(subject, walksChain) answers whether to go on.
template<typename Summary>
static constexpr bool hasInspectionsForLinkKnownEffects = requires(const Summary& summary) { summary.inspections; };

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

// unions-and-narrowing: of a call by Binding, an argument that a test of the program has cut. The call is entered once something lies there.
template<typename Call>
static uint32_t nodeThatCallWaitsForForLinkKnownEffects(const Call& call)
{
    if constexpr (requires { call.waitsFor; })
        return call.waitsFor;
    else
        return noSummaryNode;
}

// ---- Whose objects are written

// An object of one of the engine's classes has no bit and needs none: LinkKnownEffects::layoutsWrittenBy().
uint64_t CalleeSummarizerForLinkEffects::bitsOfLayoutsAt(const LinkType& type)
{
    // No layout stands for a closure, for a built-in or for what nobody has seen.
    if (type.functions != LinkType::Functions::None || type.builtin || type.holdsUnseen)
        return objectsOfAnyLayout;
    switch (type.objects) {
    case LinkType::Objects::None:
        return 0;
    case LinkType::Objects::Layout:
        return bitOfLayoutInKnownEffects(m_program, type.layout);
    case LinkType::Objects::Family: {
        // A member brings a bit at least, so nothing there says that nobody has asked.
        uint64_t& bits = m_bitsOfFamilies[type.family.index()];
        if (!bits) {
            for (LayoutID member : membersOf(m_program, m_flow.families.layoutsOf(type.family)))
                bits |= bitOfLayoutInKnownEffects(m_program, member);
        }
        return bits;
    }
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
    // What is put below a prototype has its link written.
    if (change.kind == SummaryChange::Kind::BecomesPrototype && change.other != noSummaryNode)
        layouts |= bitsOfLayoutsAt(typeAt(id, change.other));
    return layouts;
}

// `Object.assign(target, source)`, `Object.freeze(o)` by their rows. `itself`: effectsOfCallItself().
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
                if (parameter && (parameter->uses.containsAny(usesThatChangeOperandInLinkEffects) || parameter->uses.containsAny(usesThatChangeShapeInLinkEffects))) {
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
    // The rows write into objects, and one of them does not say into which.
    if (someRowNamesNoOperand && itself.writes.containsAny(heapsOfObjectsWithLayouts))
        return objectsOfAnyLayout;
    return layouts;
}

// ---- What runs no code

void CalleeSummarizerForLinkEffects::findLayoutsOfPlainObjects()
{
    uint32_t numberOfLayouts = m_program.layouts.size();
    m_layoutsOfPlainObjects.ensureSize(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = m_program.layouts[index];
        // Of any other layout native code makes objects too, a proxy among them. The tables say GetsAccessor of what is done through a lost value as well.
        // Where a layout has no room for what the classes above it declare, it does not list all that its objects have.
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

// Objects of ONE layout of the program that nobody else has got hold of and on which no accessor lies or can come to lie, and nothing else but primitives.
bool CalleeSummarizerForLinkEffects::holdsOnlyPlainObjectsOfOneLayout(const LinkType& type) const
{
    return type.objects == LinkType::Objects::Layout && type.functions == LinkType::Functions::None && !mayBeOtherCallableForLinkEffects(type) && m_layoutsOfPlainObjects.get(type.layout.index());
}

// `for (const k in record)`, `Object.values(record)`, `x instanceof Shape`: only a proxy answers a listing of keys or a step up the chain with code, and only a getter of its own the reading of a value.
bool CalleeSummarizerForLinkEffects::listingOfKeysMayRunCode(FunctionID id, const ThinSummary& summary) const
{
    std::span<const uint32_t> nodes = nodesWhoseKeysAreListedForLinkKnownEffects(summary);
    // The trait is the only record of the operand.
    if (summary.has(SummaryTrait::ListsKeys) && nodes.empty() && !hasInspectionsForLinkKnownEffects<ThinSummary>)
        return true;
    auto mayRunCode = [&](uint32_t node, bool walksChain) {
        LinkType subject = typeAt(id, node);
        if (!mayBeObjectForLinkEffects(subject))
            return false;
        // What stands above such an object is as the classes are written, up to Object.prototype.
        return !holdsOnlyPlainObjectsOfOneLayout(subject) || (walksChain && m_layoutsThatMayFindAnything.get(subject.layout.index()));
    };
    for (uint32_t node : nodes) {
        if (mayRunCode(node, false))
            return true;
    }
    return !forEachInspectionForLinkKnownEffects(summary, [&](uint32_t subject, bool walksChain) {
        return !mayRunCode(subject, walksChain);
    });
}

// As findCodeBehindLookup() has it of a store on the objects of one layout, for whoever may change nothing here. The answers by key are all there: indexHolders().
bool CalleeSummarizerForLinkEffects::storeMayFindCode(LayoutID layout, PropertyKeyID key) const
{
    if (!key || m_program.isIndexLike(key) || m_keysThatObjectPrototypeNeverAnswers[key.index()] != Answer::Yes)
        return true;
    CodeFound found;
    lookInLayout(layout, true, key, MethodKind::Setter, false, found);
    return !found.functions.isEmpty() || found.mayBeLost || found.mayBeUnseen;
}

// `{ ...state }`, `Object.assign(target, state)`. Where the record does not say that the copy defines, it counts as an assignment, which a setter above the target may take.
bool CalleeSummarizerForLinkEffects::copyMayRunCode(FunctionID id, const SummaryChange& change) const
{
    if (change.definesWhatItCopies)
        return copyThatDefinesMayRunCode(id, change);
    if (change.other == noSummaryNode)
        return true;
    LinkType source = typeAt(id, change.other);
    if (!mayBeObjectForLinkEffects(source))
        return false;
    if (!holdsOnlyPlainObjectsOfOneLayout(source))
        return true;
    // A property that lies beside the fields has a name that nobody lists.
    if (m_tables.layouts[source.layout.index()].possibleChanges.contains(LayoutChange::GainsProperty))
        return true;

    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    LinkType target = typeAt(id, change.subject);
    bool targetIsOfOneLayout = target.objects == LinkType::Objects::Layout && target.functions == LinkType::Functions::None && !mayBeOtherCallableForLinkEffects(target) && !(target.primitives & ~primitivesWithoutMembers);
    if (!targetIsOfOneLayout || m_layoutsThatMayFindAnything.get(target.layout.index()))
        return true;
    for (const Field& field : m_program.at(source.layout).fields) {
        if (storeMayFindCode(target.layout, field.key))
            return true;
    }
    for (PropertyKeyID key : m_tables.appendedKeysOf(source.layout)) {
        if (storeMayFindCode(target.layout, key))
            return true;
    }
    return false;
}

// `function add(seen, k) { seen.add(k) }`: instances of ONE class that nobody else has got hold of, on which nothing lies under the name or under a name that nobody knows.
// Whatever changes a prototype or lays a name lets the object flow into unseen code, which fills those slots. So what callObserved() has found in the table is all that the name finds, and the rows speak.
bool CalleeSummarizerForLinkEffects::membersOfInstancesAreAsTableLists(const LinkType& receiver, PropertyKeyID key) const
{
    if (!key || receiver.classes != LinkType::Classes::Site || receiver.objects != LinkType::Objects::None || receiver.functions != LinkType::Functions::None || receiver.builtin || receiver.holdsUnseen)
        return false;
    if (m_flow.hasEscaped(receiver.site))
        return false;
    auto holdsNothing = [&](SlotOfContents slot) {
        FlowNodeID node = m_flow.findContentsOf(receiver, slot);
        return !node || m_flow.typeAt(node).isNothing();
    };
    return holdsNothing(SlotOfContents::under(key)) && holdsNothing(SlotOfContents::underUnknownNames()) && holdsNothing(SlotOfContents::underAnyName());
}

// The row names a function of the library: a call of the built-in enters that function, and what the function does is what the call does.
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
    return awaited != noSummaryNode && typeAt(id, awaited).isNothing();
}

// `if (options.trace) log(x)`: nothing ever arrives at what is called or looked into, so no run comes to the operation.
bool CalleeSummarizerForLinkEffects::callNeverRuns(FunctionID id, const SummaryCall& call) const
{
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        return callStillWaits(id, call);
    case SummaryCall::Callee::Name:
        return typeAt(id, call.receiver).isNothing();
    case SummaryCall::Callee::Value:
        return typeAt(id, call.subject).isNothing();
    case SummaryCall::Callee::Builtin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Effects

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
    // What was entered may have thrown, and the body has caught that and gone on.
    case WayInKnownEffects::ToReturnUnderHandler:
        into.writesOnReturn.merge(all.writes);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CalleeSummarizerForLinkEffects::takeKnownEffectsOfFunction(FunctionID callee, WayInKnownEffects way, EffectsOfComponentInKnownEffects& into) const
{
    const LinkedFunction& linked = m_tables.function(callee);
    takeKnownEffects(linked.effectsOnThrow, linked.effectsOnReturn.writes, m_layoutsWrittenByFunction[callee.index()], way, into);
}

// False: something that is entered stands in the component itself, whose effects are not whole yet.
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

// `(a, b) => a.rank - b.rank`. All or nothing a function.
void CalleeSummarizerForLinkEffects::addEffectsOfOperatorsLeftApart(FunctionID id, const ThinSummary& summary, EffectsOfComponentInKnownEffects& effects) const
{
    const Effects* boundOfOperators = summary.knownEffectsApart.ownEffectsOfOperators();
    if (!boundOfOperators)
        return;
    constexpr uint16_t whatBecomesNumber = LinkType::closed(Primitive::NumberLike);
    constexpr uint16_t plainPrimitives = LinkType::closed(Primitive::NumberLike | Primitive::AnyString);
    bool mayTakeStrings = false;
    bool mayTakeOtherValues = false;
    for (uint32_t node : summary.knownEffectsApart.operandsOfOperatorsLeftApart()) {
        LinkType type = typeAt(id, node);
        // A node that holds nothing says that nobody has followed the value, not that there is none.
        mayTakeOtherValues |= type.isNothing() || mayBeObjectForLinkEffects(type) || (type.primitives & ~plainPrimitives);
        mayTakeStrings |= !!(type.primitives & ~whatBecomesNumber);
    }

    // A number, a boolean, undefined and null become numbers without memory, and no operator turns one of them away.
    if (!mayTakeOtherValues && !mayTakeStrings)
        return;
    Effects ofOperators = *boundOfOperators;
    if (!mayTakeOtherValues && !summary.operatorsLeftApartMayJoinTexts && ofOperators.mayThrow)
        ofOperators.mayThrow = ThrowEffect::onlyForLackOfMemory();
    takeKnownEffects(ofOperators, ofOperators.writes, 0, WayInKnownEffects::ToReturn, effects);
}

// Of an access under a key that enters no code, or whose code is counted beside this. `subject`: what it is taken to look into.
void CalleeSummarizerForLinkEffects::addEffectsOfAccessItself(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, const LinkType& subject, PropertyKeyID key, WayInKnownEffects way, EffectsOfComponentInKnownEffects& effects)
{
    Effects ofLookup = effectsOfLookupItself(access.kind, key);
    uint64_t layouts = 0;
    switch (access.kind) {
    case SummaryAccess::Kind::Read:
    case SummaryAccess::Kind::Has:
        break;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define:
        // `function make(a) { const o = new Box(); o.a = a; return o }`: no caller knows anything of an object that did not exist when it called.
        if (access.isPartOfConstruction && summary.nodes[access.subject].kind == SummaryNode::Kind::Allocation) {
            ofLookup.writes = ofLookup.writes.restrictedTo(heapsWhoseWritesNobodySees);
            break;
        }
        layouts = bitsOfLayoutsAt(subject);
        // `super.x = v`: where the lookup finds no setter the property is laid on the receiver.
        if (access.receiver != noSummaryNode)
            layouts |= bitsOfLayoutsAt(typeAt(id, access.receiver));
        break;
    }
    takeKnownEffects(ofLookup, ofLookup.writes, layouts, way, effects);
}

// ---- Effects given the arguments

std::optional<uint32_t> CalleeSummarizerForLinkEffects::entryParameterAt(const ThinSummary& summary, uint32_t node)
{
    constexpr uint32_t numberOfParametersWithBit = 32;
    if (node == noSummaryNode)
        return std::nullopt;
    const SummaryNode& found = summary.nodes[node];
    return found.kind == SummaryNode::Kind::Parameter && found.first < numberOfParametersWithBit ? std::optional(found.first) : std::nullopt;
}

// Made by hand: LinkFlow::fromTypeID() queues losses and escapes, and the flow has ended. Nothing: more is expected than the objects of one layout beside primitives, or nothing is.
std::optional<LinkType> CalleeSummarizerForLinkEffects::typeExpectedOfParameter(FunctionID id, uint32_t entryParameter) const
{
    std::span<const LinkedParameter> parameters = m_tables.parametersOf(id);
    TypeID expected = entryParameter < parameters.size() ? parameters[entryParameter].expected : TypeID();
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

// What was found for a type that Link has not seen arrive. No edge of the graph of callees leads there, so only a component that is closed has its effects.
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

// As lookInLayout() goes, for what is called where it is found.
void CalleeSummarizerForLinkEffects::addFunctionsCalledUnder(LayoutID layout, PropertyKeyID key, CodeFound& found) const
{
    // A property that lies beside the fields may be any function.
    if (m_tables.layouts[layout.index()].possibleChanges.contains(LayoutChange::GainsProperty))
        found.mayBeUnseen = true;
    if (std::optional<FieldReference> field = findField(m_program, layout, key)) {
        if (!fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor))
            addFunctionsAt(m_flow.typeAt(m_flow.field(*field)), found);
    }
    if (FlowNodeID appended = m_flow.findAppendedField(layout, key))
        addFunctionsAt(m_flow.typeAt(appended), found);
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

// `function width(s: Shape) { return s.w }`, handed to code that nobody has seen: anything arrives at `s`. False: what is expected there says no more.
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

// `each(list, visit)`: the call of `visit` is left out, for whoever knows what he hands over. `s.area()`: what the one layout finds.
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
        std::optional<uint32_t> parameter = entryParameterAt(summary, call.receiver);
        std::optional<LinkType> expected = parameter ? typeExpectedOfParameter(id, *parameter) : std::nullopt;
        // A member that the table lists on Object.prototype is found by callObserved(), for what has arrived and for nothing else.
        if (!expected || !call.key || findOwnMember(m_program, m_builtins.canonicalID(Builtins::ObjectPrototype), call.key))
            return false;
        CodeFound found;
        findCodeBehindLookup(*expected, call.key, SummaryAccess::Kind::Read, true, found);
        addFunctionsCalledUnder(expected->layout, call.key, found);
        if (!mergeKnownEffectsOfCodeFound(found, component, way, effects))
            return false;
        itself.merge(effectsOfLookupItself(SummaryAccess::Kind::Read, call.key));
        takeKnownEffects(itself, itself.writes, 0, way, effects);
        given.parametersTakenAsExpected |= 1u << *parameter;
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Calls of several functions

bool CalleeSummarizerForLinkEffects::mayBeCallOfSeveral(const SummaryCall& call, const CodeEntered& code)
{
    bool findsCalleeAtRunTime = call.callee == SummaryCall::Callee::Name || call.callee == SummaryCall::Callee::Value;
    return call.site && !call.isMadeByProtocol && findsCalleeAtRunTime && !code.mayBeUnseen && !code.isNone();
}

// One look at the nodes and the calls of a summary, at the first call of it that is asked about.
void CalleeSummarizerForLinkEffects::indexCallsOf(FunctionID id, const ThinSummary& summary)
{
    if (m_functionWhoseCallsAreIndexed == id)
        return;
    m_functionWhoseCallsAreIndexed = id;
    m_resultNodesOfCalls.fill(noSummaryNode, summary.calls.size());
    for (uint32_t node = 0; node < summary.nodes.size(); ++node) {
        if (summary.nodes[node].kind == SummaryNode::Kind::ResultOfCall && !summary.nodes[node].second)
            m_resultNodesOfCalls[summary.nodes[node].first] = node;
    }
    m_sitesOfCalls.shrink(0);
    for (const SummaryCall& call : summary.calls)
        m_sitesOfCalls.append(call.site);
    std::ranges::sort(m_sitesOfCalls);
}

// `visitors[node.kind](node)`, `shape.area()` over a family. `ofCall`: what the operation does, but for what it enters in the component of its caller.
void CalleeSummarizerForLinkEffects::noteIfCallOfSeveral(FunctionID id, const ThinSummary& summary, uint32_t index, const CodeEntered& code, const EffectsOfComponentInKnownEffects& ofCall, bool entersOwnComponent)
{
    const SummaryCall& call = summary.calls[index];
    if (!mayBeCallOfSeveral(call, code) || !builtinsEnteredBy(id, summary, index).empty() || m_tables.calleeAt(id, call.site))
        return;
    indexCallsOf(id, summary);
    // The table answers for a site.
    if (std::ranges::equal_range(m_sitesOfCalls, call.site).size() != 1)
        return;

    CallOfSeveralInKnownEffects found;
    found.caller = id;
    found.site = call.site;
    if (uint32_t node = m_resultNodesOfCalls[index]; node != noSummaryNode)
        found.result = m_flow.nodeOf(id, node);
    found.effects = ofCall;
    found.entersOwnComponent = entersOwnComponent;
    // Behind the shared calls stands a place for the lost functions.
    std::span<const FunctionID> entered = functionsOf(code);
    if (entered.size() == 1 && isSharedCall(entered[0].index()) && !code.mayBeLost)
        found.sharesEffectsWith = entered[0].index() - nodeOfSharedCall(0);
    else if (entered.empty())
        found.sharesEffectsWith = static_cast<uint32_t>(m_flow.calleesOfSharedCalls.size());
    m_callsOfSeveral.append(WTF::move(found));
}

// ---- The traversal

// In the place of effectsOfBodyAndOfCodeOutside(). What the members of the component do to one another is left out: it is united with this.
auto CalleeSummarizerForLinkEffects::knownEffectsOfBody(uint32_t function) -> EffectsOfComponentInKnownEffects
{
    FunctionID id(function);
    const ThinSummary& summary = m_flow.summaries.at(id);
    uint32_t component = m_components.componentOfFunction[function];
    // `async function save(o) { set(o, -1) }`: the ramp of a coroutine catches what its body throws, and the call returns with a promise.
    bool catchesWhatIsThrown = summary.someBlockHasHandler || m_program.at(id).isCoroutine();
    auto wayOf = [&](bool liesOnNoWayToReturn) {
        if (liesOnNoWayToReturn)
            return WayInKnownEffects::ToNoReturn;
        return catchesWhatIsThrown ? WayInKnownEffects::ToReturnUnderHandler : WayInKnownEffects::ToReturn;
    };

    bool isKnown = !keepsDefaults(id, summary, { SummaryTrait::Suspends, SummaryTrait::HasDirectEval }) && !summary.ownEffects.mayCallUnknownCode
        && !handsObjectToCodeNobodyNames(id, summary) && !listingOfKeysMayRunCode(id, summary);
    EffectsOfComponentInKnownEffects effects;
    if (isKnown) {
        effects.all = summary.ownEffects;
        const LocationMasks* ownWritesOnWaysToReturn = summary.knownEffectsApart.ownWritesOnWaysToReturn();
        effects.writesOnReturn = ownWritesOnWaysToReturn ? *ownWritesOnWaysToReturn : summary.ownEffects.writes;
        effects.layoutsWritten = summary.layoutsOfOwnWrites;
        addEffectsOfOperatorsLeftApart(id, summary, effects);
        for (const SummaryChange& change : summary.changes) {
            if (changeMayRunCode(id, change)) {
                isKnown = false;
                break;
            }
            effects.layoutsWritten |= layoutsWrittenByChange(id, change);
        }
    }

    // From the first record on that is judged by what is expected of a parameter, or left out because a parameter is called, `effects` holds what the function does given its arguments. By Link's own word it may then do anything.
    GivenArgumentsInKnownEffects given;
    bool entersOwnComponent = false;
    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const SummaryCall& call = summary.calls[index];
        const CodeEntered& code = codeOfCall(id, index);
        // A call of several functions is told of whatever else its caller does.
        if ((!isKnown && !mayBeCallOfSeveral(call, code)) || callNeverRuns(id, call))
            continue;
        WayInKnownEffects way = wayOf(call.liesOnNoWayToReturn);
        Effects itself = code.mayBeUnseen ? Effects::unknownCode() : effectsOfCallItself(id, summary, index);
        if (itself.mayCallUnknownCode) {
            isKnown = isKnown && judgeCallGivenArguments(id, summary, call, component, way, effects, given);
            continue;
        }
        EffectsOfComponentInKnownEffects ofCall { itself, itself.writes, layoutsChangedByRowsOf(id, summary, index, itself) };
        // `new Promise(executor)`, the close of an iterator: native code and the engine's own protocols may catch what the function throws that they have called, and go on.
        bool isEnteredByCallAsWritten = !call.isMadeByProtocol && builtinsEnteredBy(id, summary, index).empty();
        bool everythingStandsOutside = mergeKnownEffectsOfCodeOutside(code, component, isEnteredByCallAsWritten ? WayInKnownEffects::ToReturn : WayInKnownEffects::ToReturnUnderHandler, ofCall);
        noteIfCallOfSeveral(id, summary, index, code, ofCall, !everythingStandsOutside);
        entersOwnComponent |= !everythingStandsOutside;
        if (isKnown)
            takeKnownEffects(ofCall.all, ofCall.writesOnReturn, ofCall.layoutsWritten, way, effects);
    }

    for (uint32_t index = 0; isKnown && index < summary.accesses.size(); ++index) {
        const SummaryAccess& access = summary.accesses[index];
        LinkType subject = typeAt(id, access.subject);
        // ThinSummary::ownEffects has what an access to contents does, and it enters nobody's code. `class Stack extends Array`: what holds the contents may have a layout.
        if (access.isToContents()) {
            if (access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define)
                effects.layoutsWritten |= bitsOfLayoutsAt(subject);
            continue;
        }
        if (subject.isNothing())
            continue;
        const CodeEntered& code = codeOfAccess(id, index);
        PropertyKeyID key = m_flow.keyOf(id, access);
        // The record may stand for a lookup of the engine's own, which catches what an accessor throws and goes on: `then` of what a promise is resolved with.
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
    if (!given.parametersTakenAsExpected && !given.parametersCalled)
        return effects;
    // Only where that says less than Link's own word.
    if (!entersOwnComponent && !effects.all.mayCallUnknownCode) {
        finishKnownEffectsOfCall(effects, false);
        Effects onReturn = effects.all;
        onReturn.writes = effects.writesOnReturn;
        m_effectsGivenArguments.append(LinkedEffectsGivenArguments { id, given.parametersTakenAsExpected, given.parametersCalled, effects.layoutsWritten, onReturn, effects.all });
    }
    return knownEffectsOfUnknownCode();
}

void CalleeSummarizerForLinkEffects::finishKnownEffectsOfCall(EffectsOfComponentInKnownEffects& effects, bool mayEnterItself)
{
    // No caller's check of the stack covers recursive calls: the overflow is an error like any other.
    effects.all.mayThrow |= mayEnterItself ? ThrowEffect(true) : ThrowEffect::onlyStackOverflow();
    // The check of the stack at frame setup is a poll too, and the collector may stop the thread there.
    effects.all.mayCollect = true;
    // A call never suspends its caller. The sweep and the emitters take an operation that may suspend for a suspension of its own frame.
    effects.all.maySuspend = false;
    // One member may catch what another has thrown, and nobody has asked which.
    if (mayEnterItself)
        effects.writesOnReturn = effects.all.writes;
}

void CalleeSummarizerForLinkEffects::summarizeKnownEffects()
{
    const ComponentsOfLinkEffects& components = m_components;
    m_layoutsWrittenByFunction.fill(objectsOfAnyLayout, m_numberOfFunctions);
    m_knownEffectsOfSharedCalls.fill(knownEffectsOfUnknownCode(), m_flow.calleesOfSharedCalls.size());
    m_bitsOfFamilies.fill(0, m_flow.families.size());
    findLayoutsOfPlainObjects();

    uint32_t componentOfLostFunctions = components.componentOfFunction[nodeOfLostFunctions()];
    for (uint32_t component = 0; component < components.size(); ++component) {
        std::span<const uint32_t> members = components.membersOf(component);
        // A function that is not summarized was given no callees, so it is alone in its component.
        if (isFunction(members.front()) && !isSummarized(members.front()))
            continue;

        size_t firstCallOfSeveral = m_callsOfSeveral.size();
        EffectsOfComponentInKnownEffects effects;
        for (size_t place = components.firstMember[component]; place < components.firstMember[component + 1]; ++place) {
            prefetchSummaryOfMemberBehind(place);
            uint32_t member = components.members[place];
            if (isFunction(member)) {
                EffectsOfComponentInKnownEffects ofBody = knownEffectsOfBody(member);
                takeKnownEffects(ofBody.all, ofBody.writesOnReturn, ofBody.layoutsWritten, WayInKnownEffects::ToReturn, effects);
                continue;
            }
            for (FunctionID listed : calleesOf(member)) {
                if (components.componentOfFunction[listed.index()] != component)
                    takeKnownEffectsOfFunction(listed, WayInKnownEffects::ToReturn, effects);
            }
        }
        bool mayEnterItself = members.size() > 1 || std::ranges::any_of(calleesOf(members.front()), [&](FunctionID callee) {
            return callee.index() == members.front();
        });
        finishKnownEffectsOfCall(effects, mayEnterItself);

        if (component == componentOfLostFunctions)
            m_knownEffectsOfLostFunctions = effects;
        Effects onReturn = effects.all;
        onReturn.writes = effects.writesOnReturn;
        for (uint32_t member : members) {
            if (isSharedCall(member))
                m_knownEffectsOfSharedCalls[member - nodeOfSharedCall(0)] = effects;
            if (!isFunction(member))
                continue;
            LinkedFunction& linked = m_tables.functions[member];
            linked.effectsOnReturn = onReturn;
            linked.effectsOnThrow = effects.all;
            m_layoutsWrittenByFunction[member] = effects.layoutsWritten;
        }
        for (size_t place = firstCallOfSeveral; place < m_callsOfSeveral.size(); ++place) {
            if (m_callsOfSeveral[place].entersOwnComponent)
                takeKnownEffects(effects.all, effects.writesOnReturn, effects.layoutsWritten, WayInKnownEffects::ToReturn, m_callsOfSeveral[place].effects);
        }
    }
}

// On one thread, behind the lanes: it makes types.
void CalleeSummarizerForLinkEffects::writeKnownEffects()
{
    constexpr uint32_t none = UINT32_MAX;
    LinkKnownEffects& known = m_tables.known;
    known.layoutsWrittenByFunction = WTF::move(m_layoutsWrittenByFunction);
    known.givenArguments = WTF::move(m_effectsGivenArguments);
    std::ranges::sort(known.givenArguments, { }, &LinkedEffectsGivenArguments::function);

    // In which order components are closed follows from the graph of callees alone, and the table goes by caller and site.
    m_callsOfSeveral.removeAllMatching([](const CallOfSeveralInKnownEffects& call) {
        return call.effects.all.mayCallUnknownCode;
    });
    std::ranges::sort(m_callsOfSeveral, { }, [](const CallOfSeveralInKnownEffects& call) {
        return std::tuple { call.caller, call.site };
    });

    // By shared call, and one more for the lost functions: where the effects of the newest call lie that enters nothing else.
    Vector<uint32_t> effectsThatAreShared;
    effectsThatAreShared.fill(none, m_flow.calleesOfSharedCalls.size() + 1);
    known.callsOfSeveral.reserveInitialCapacity(m_callsOfSeveral.size());
    for (const CallOfSeveralInKnownEffects& call : m_callsOfSeveral) {
        Effects onReturn = call.effects.all;
        onReturn.maySuspend = false;
        Effects onThrow = onReturn;
        onReturn.writes = call.effects.writesOnReturn;

        // What the operation does in front of its callee may differ from call to call: a list that is passed, a name that is looked up.
        uint32_t place = call.sharesEffectsWith != none ? effectsThatAreShared[call.sharesEffectsWith] : none;
        if (place == none || known.effectsOfCallsOfSeveral[place] != onReturn || known.effectsOfCallsOfSeveral[place + 1] != onThrow) {
            place = static_cast<uint32_t>(known.effectsOfCallsOfSeveral.size());
            known.effectsOfCallsOfSeveral.append(onReturn);
            known.effectsOfCallsOfSeveral.append(onThrow);
            if (call.sharesEffectsWith != none)
                effectsThatAreShared[call.sharesEffectsWith] = place;
        }

        if (call.result) {
            LinkType type = m_flow.withoutObjectsThatHaveEscaped(m_flow.typeAt(call.result));
            if (!type.isNothing() && !type.isAnything())
                m_typesFound.resultsOfCallsOfSeveral.append({ static_cast<uint32_t>(known.callsOfSeveral.size()), type });
        }
        // Its result: writeTypesFoundOfCallees().
        known.callsOfSeveral.append(LinkedCallOfSeveral { call.caller, call.site, { }, call.effects.layoutsWritten, place });
    }
    m_callsOfSeveral.clear();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
