/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFreshResults.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinsInReach.h"
#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTOpcodeSets.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class AliasSets {
public:
    static constexpr uint32_t noSet = UINT32_MAX;
    static constexpr OpcodeSet allocations { Opcode::NewObject, Opcode::Construct, Opcode::Call };

    static bool passesObjectThrough(Opcode opcode) { return inputPassThroughOpcodes.contains(opcode); }

    enum class UseKind : uint8_t { Operand, Subject, Terminator };

    struct Use {
        ValueID user() const { return ValueID(place); }
        BlockID block() const { return BlockID(place); }

        uint32_t place { 0 };
        uint32_t operandIndex { 0 };
        UseKind kind { UseKind::Operand };
    };

    explicit AliasSets(const BodyIndex& index)
    {
        uniteNames(index);
        numberSets(index);
        listDefinitionsAndUses(index);
    }

    uint32_t setOf(ValueID value) const { return m_sets[value.index()]; }
    bool isPassedOutOfSight(uint32_t set) const { return m_isPassedOutOfSight[set]; }
    std::span<const ValueID> definitionsOf(uint32_t set) const { return m_definitions.of(set); }
    std::span<const Use> usesOf(uint32_t set) const { return m_uses.of(set); }
    bool hasLoop() const { return m_hasLoop; }

private:
    static constexpr OpcodeSet definitionsOfInterest = allocations | OpcodeSet { Opcode::Data, Opcode::PropGet, Opcode::PrivateGet };

    enum Flag : uint8_t {
        IsPassedOutOfSight = 1 << 0,
        HasOtherDefinition = 1 << 1,
        HasEntryParameter = 1 << 2,
    };

    uint32_t rootOf(uint32_t value)
    {
        while (m_sets[value] != value) {
            m_sets[value] = m_sets[m_sets[value]];
            value = m_sets[value];
        }
        return value;
    }

    void unite(ValueID first, ValueID second)
    {
        uint32_t firstRoot = rootOf(first.index());
        uint32_t secondRoot = rootOf(second.index());
        m_sets[std::max(firstRoot, secondRoot)] = std::min(firstRoot, secondRoot);
    }

    template<typename Functor>
    static void forEachReachableBlock(const BodyIndex& index, const Functor& functor)
    {
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (index.isReachable(BlockID(number)))
                functor(index.block(BlockID(number)));
        }
    }

    void uniteNames(const BodyIndex& index)
    {
        m_sets = Vector<uint32_t>(index.numberOfValues(), [](size_t value) {
            return static_cast<uint32_t>(value);
        });
        m_flags.fill(0, index.numberOfValues());
        BodyBlock entry = index.block(entryBlock);
        for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter)
            m_flags[entry.parameter(parameter).index()] |= HasEntryParameter;
        forEachReachableBlock(index, [&](BodyBlock block) {
            m_hasLoop |= !!index.loopOf(block.id());
            for (BodyValue value : block.values()) {
                if (passesObjectThrough(value.opcode()) && value.operand(0))
                    unite(value.id(), value.operand(0));
                else if (!definitionsOfInterest.contains(value.opcode()))
                    m_flags[value.id().index()] |= HasOtherDefinition;
            }
            block.forEachOutgoingEdge([&](BodyTarget target, bool isHandler) {
                BodyBlock to = index.block(target.block());
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    ValueID argument = arguments[position];
                    bool hasParameter = position < to.numberOfParameters();
                    if (hasParameter && argument && !isHandler) {
                        unite(argument, to.parameter(position));
                        continue;
                    }
                    if (argument)
                        m_flags[argument.index()] |= IsPassedOutOfSight;
                    if (hasParameter)
                        m_flags[to.parameter(position).index()] |= IsPassedOutOfSight;
                }
            });
            index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
                unite(result.subject, result.parameter);
            });
        });
    }

    void numberSets(const BodyIndex& index)
    {
        for (uint32_t value = 0; value < index.numberOfValues(); ++value) {
            m_sets[value] = rootOf(value);
            m_flags[m_sets[value]] |= m_flags[value];
        }
        for (uint32_t value = 0; value < index.numberOfValues(); ++value) {
            if (m_sets[value] != value) {
                m_sets[value] = m_sets[m_sets[value]];
                continue;
            }
            bool isOfInterest = m_flags[value] & HasEntryParameter || !(m_flags[value] & (IsPassedOutOfSight | HasOtherDefinition));
            m_sets[value] = isOfInterest ? m_isPassedOutOfSight.size() : noSet;
            if (isOfInterest)
                m_isPassedOutOfSight.append(!!(m_flags[value] & IsPassedOutOfSight));
        }
        m_flags.clear();
    }

    template<typename Item>
    struct Grouped {
        void add(uint32_t set, const Item& item)
        {
            if (set == noSet)
                return;
            sets.append(set);
            items.append(item);
        }

        void group(uint32_t numberOfSets)
        {
            first.fill(0, numberOfSets + 2);
            for (uint32_t set : sets)
                ++first[set + 2];
            for (uint32_t set = 0; set < numberOfSets; ++set)
                first[set + 2] += first[set + 1];
            Vector<Item> grouped(items.size());
            for (uint32_t place = 0; place < items.size(); ++place)
                grouped[first[sets[place] + 1]++] = items[place];
            items = WTF::move(grouped);
            sets.clear();
        }

        std::span<const Item> of(uint32_t set) const { return items.span().subspan(first[set], first[set + 1] - first[set]); }

        Vector<uint32_t> first;
        Vector<uint32_t> sets;
        Vector<Item> items;
    };

    void listDefinitionsAndUses(const BodyIndex& index)
    {
        BodyBlock entry = index.block(entryBlock);
        for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter)
            m_definitions.add(setOf(entry.parameter(parameter)), entry.parameter(parameter));
        auto use = [&](ValueID name, uint32_t place, uint32_t operandIndex, UseKind kind) {
            if (name)
                m_uses.add(setOf(name), Use { place, operandIndex, kind });
        };
        forEachReachableBlock(index, [&](BodyBlock block) {
            use(block.subject(), block.id().index(), 0, UseKind::Subject);
            for (BodyValue user : block.values()) {
                if (!passesObjectThrough(user.opcode()) || !user.operand(0))
                    m_definitions.add(setOf(user.id()), user.id());
                user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    use(operand, user.id().index(), operandIndex, UseKind::Operand);
                });
            }
            block.terminator().forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                use(operand, block.id().index(), operandIndex, UseKind::Terminator);
            });
        });
        m_definitions.group(m_isPassedOutOfSight.size());
        m_uses.group(m_isPassedOutOfSight.size());
    }

    Vector<uint32_t> m_sets;
    Vector<uint8_t> m_flags;
    Vector<bool> m_isPassedOutOfSight;
    Grouped<ValueID> m_definitions;
    Grouped<Use> m_uses;
    bool m_hasLoop { false };
};

struct NamesOfObject {
    template<typename Functor>
    bool allUses(const Functor& functor) const
    {
        return std::ranges::all_of(sets, [&](uint32_t set) {
            return std::ranges::all_of(aliases->usesOf(set), functor);
        });
    }

    const AliasSets* aliases { nullptr };
    Vector<uint32_t, 2> sets;
    Vector<ValueID, 8> allocations;
    Vector<ValueID, 2> entryParameters;
};

class FreshResults {
public:
    FreshResults(const LinkFlow& flow, const LinkTables& tables)
        : m_flow(flow)
        , m_tables(tables)
        , m_program(flow.program)
        , m_bodies(bodiesLookedAt(flow.looks))
    {
    }

    Vector<LinkedFreshResult> find()
    {
        Vector<LinkedFreshResult> results;
        for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
            FunctionID function(number);
            const LinkedFunction& linked = m_tables.function(function);
            if (!linked.isReached || !linked.everyCallHasOneCallee || !returnsObjectOfOneLayout(function))
                continue;
            if (const LinkedFreshResult* found = findResult(function))
                results.append(*found);
            m_aliases.clear();
        }
        return results;
    }

    Vector<LinkedInspectedParameter> findParameters()
    {
        Vector<LinkedInspectedParameter> parameters;
        findWhatMayBeScalarized();
        if (m_mayBeScalarized.isEmpty())
            return parameters;
        Vector<CallThatPasses> calls;
        for (const LinkedCall& call : m_tables.calls) {
            if (m_functionsThatMayBeScalarized.quickGet(call.callee.index()))
                lookAtCall(call, calls);
        }
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            considerAll(Places { m_candidates[number].placesPassedTo });
            for (uint32_t field = 0; field < m_candidates[number].inspectedFields.size(); ++field)
                considerAll(Places { m_candidates[number].inspectedFields[field].placesPassedTo });
        }
        dropCandidatesPassedOn();
        dropSharedParameters(calls);
        dropFieldsWrittenMeanwhile();
        for (const Candidate& candidate : m_candidates) {
            if (mayBecomeParameter(candidate))
                parameters.append(LinkedInspectedParameter { candidate.function, candidate.entryParameter, candidate.layout, inspectedFieldsOf(candidate) });
        }
        std::ranges::sort(parameters, { }, [](const LinkedInspectedParameter& parameter) { return std::pair { parameter.function.index(), parameter.entryParameter }; });
        return parameters;
    }

private:
    static constexpr size_t maxFields = 8;

    struct Place {
        FunctionID function;
        uint32_t entryParameter { 0 };
    };
    using Places = Vector<Place, 4>;

    struct InspectedField {
        uint32_t field { 0 };
        LayoutID held;
        Places placesPassedTo;
        bool isRead { false };
    };

    struct Candidate {
        FunctionID function;
        uint32_t entryParameter { 0 };
        LayoutID layout;
        bool isInspected { true };
        bool isScalarizable { true };
        bool hasLoop { false };
        Places placesPassedTo;
        Vector<InspectedField, 2> inspectedFields;
    };

    struct PassedObject {
        LayoutID layout;
        NamesOfObject names;
        Places placesPassedTo;
        Vector<Place, 1> givers;
        Vector<Place, 1> takers;
        LoopID loopOfAllocations;
        bool canPassFields { false };
        std::optional<bool> isInspected { };
        bool isConsidered { false };
    };

    using PassedObjectsBySet = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    struct CallThatPasses {
        Place taker;
        uint32_t object { 0 };
    };

    enum class State : uint8_t { InProgress, IsFresh, IsNot };

    struct Answer {
        State state { State::InProgress };
        LinkedFreshResult result;
    };

    template<typename Kept> using ByFunction = HashMap<uint32_t, Kept, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    bool returnsObjectOfOneLayout(FunctionID function) const
    {
        return isObjectOfOneLayout(m_flow.typeAt(m_flow.result(function, 0)));
    }

    static bool isObjectOfOneLayout(const LinkType& type)
    {
        return type.objects == LinkType::Objects::Layout && !type.primitives && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin && !type.hasUnknownObjects;
    }

    uint32_t placeOf(const Place& place) const { return m_tables.function(place.function).firstParameter + place.entryParameter; }
    bool mayBeScalarized(const Place& place) const { return m_mayBeScalarized.quickGet(placeOf(place)); }
    bool mayBecomeParameter(const Candidate& candidate) const { return candidate.isInspected && candidate.isScalarizable && mayBeScalarized({ candidate.function, candidate.entryParameter }); }

    const AliasSets& aliasesOf(FunctionID function, const BodyIndex& index)
    {
        std::unique_ptr<AliasSets>& aliases = m_aliases.add(function.index(), nullptr).iterator->value;
        if (!aliases)
            aliases = makeUniqueWithoutFastMallocCheck<AliasSets>(index);
        return *aliases;
    }

    static std::optional<NamesOfObject> collectNames(const BodyIndex& index, const AliasSets& aliases, std::span<const ValueID> values, std::span<const ValueID> given)
    {
        NamesOfObject names;
        names.aliases = &aliases;
        for (ValueID value : values) {
            uint32_t set = aliases.setOf(value);
            if (set == AliasSets::noSet || aliases.isPassedOutOfSight(set))
                return std::nullopt;
            names.sets.append(set);
        }
        std::ranges::sort(names.sets);
        names.sets.shrink(std::ranges::unique(names.sets).begin() - names.sets.begin());
        for (uint32_t set : names.sets) {
            for (ValueID definition : aliases.definitionsOf(set)) {
                if (std::ranges::binary_search(given, definition))
                    continue;
                if (index.isParameter(definition))
                    names.entryParameters.append(definition);
                else if (AliasSets::allocations.contains(index.value(definition).opcode()))
                    names.allocations.append(definition);
                else
                    return std::nullopt;
            }
        }
        return names;
    }

    std::optional<NamesOfObject> collectNamesOfGiven(FunctionID function, const BodyIndex& index, std::span<const ValueID> given)
    {
        std::optional<NamesOfObject> names = collectNames(index, aliasesOf(function, index), given, given);
        return names && names->allocations.isEmpty() && names->entryParameters.isEmpty() ? names : std::nullopt;
    }

    bool collectPlacesThatTake(FunctionID function, const BodyValue& user, uint32_t operandIndex, Places& places) const
    {
        if (user.opcode() == Opcode::CallDirect) {
            uint32_t named = numberOfNamedOperandsOf(Opcode::CallDirect);
            FunctionID callee = user.as<Opcode::CallDirect>().function();
            if (operandIndex < named || operandIndex - named >= m_tables.function(callee).numberOfParameters)
                return false;
            places.append({ callee, operandIndex - named });
            return true;
        }
        uint32_t named = numberOfNamedOperandsOf(Opcode::Call);
        bool isReceiver = operandIndex == OperandIndex::Call::thisValue;
        if (user.opcode() != Opcode::Call || (!isReceiver && operandIndex < named))
            return false;
        std::optional<Vector<FunctionID, 4>> callees = calleesOf(function, user.id());
        if (!callees)
            return false;
        for (FunctionID callee : *callees) {
            const Function& entered = m_program.at(callee);
            const ThinSummary& summary = m_flow.summaries.at(callee);
            uint32_t position = operandIndex - named;
            if (!entered.hasBody() || entered.isCoroutine() || entered.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount }) || summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval))
                return false;
            if (isReceiver) {
                if (std::optional<unsigned> parameter = entered.thisParameterIndex())
                    places.append({ callee, *parameter });
            } else if (position < entered.numberOfPositionalParameters())
                places.append({ callee, entered.firstParameterIndex() + position });
        }
        return true;
    }

    bool mayBeInspected(const Function& function) const
    {
        bool isKindOfCallee = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow || function.kind == FunctionKind::Method || function.kind == FunctionKind::Getter;
        return isKindOfCallee && function.hasBody() && !function.isCoroutine() && !function.isModuleBody() && !function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount });
    }

    bool isLayoutOfScalars(LayoutID id) const
    {
        const Layout& layout = m_program.at(id);
        return (layout.kind == LayoutKind::Instance || layout.kind == LayoutKind::Record) && !layout.fields.isEmpty() && layout.fields.size() <= maxFields;
    }

    void findWhatMayBeScalarized()
    {
        m_functionsThatMayBeScalarized = BitVector(m_program.functions.size());
        BitVector places(m_tables.parameters.size());
        bool hasSome = false;
        for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
            FunctionID id(number);
            if (!m_tables.function(id).isReached || !isEnteredByListedCallsAlone(id))
                continue;
            const Function& function = m_program.at(id);
            if (!mayBeInspected(function))
                continue;
            for (uint32_t parameter = 0; parameter < function.numberOfPositionalParameters(); ++parameter) {
                Place place { id, function.firstParameterIndex() + parameter };
                const LinkType& type = m_flow.typeAt(m_flow.parameter(id, place.entryParameter));
                if (!isObjectOfOneLayout(type) || !isLayoutOfScalars(type.layout))
                    continue;
                places.quickSet(placeOf(place));
                m_functionsThatMayBeScalarized.quickSet(number);
                hasSome = true;
            }
        }
        if (!hasSome)
            return;
        BitVector called(m_tables.parameters.size());
        for (const LinkedCall& call : m_tables.calls) {
            if (!m_functionsThatMayBeScalarized.quickGet(call.callee.index()))
                continue;
            const Function& function = m_program.at(call.callee);
            const BodyIndex* index = call.site ? m_bodies.find(call.caller) : nullptr;
            for (uint32_t parameter = 0; parameter < function.numberOfPositionalParameters(); ++parameter) {
                Place place { call.callee, function.firstParameterIndex() + parameter };
                called.quickSet(placeOf(place));
                if (places.quickGet(placeOf(place)) && !mayPassObjectOfItsOwn(index, call, place))
                    places.quickClear(placeOf(place));
            }
        }
        places.filter(called);
        if (!places.isEmpty())
            m_mayBeScalarized = WTF::move(places);
    }

    static ValueID argumentAt(const BodyIndex& index, const LinkedCall& call, const Function& function, uint32_t entryParameter)
    {
        if (index.isParameter(call.site))
            return { };
        BodyValue user = index.value(call.site);
        bool isDirect = user.opcode() == Opcode::CallDirect;
        if (!isDirect && user.opcode() != Opcode::Call)
            return { };
        std::span<const ValueID> arguments = isDirect ? user.as<Opcode::CallDirect>().argumentsOperands() : user.as<Opcode::Call>().argumentsOperands();
        uint32_t position = isDirect ? entryParameter : entryParameter - function.firstParameterIndex();
        return position < arguments.size() ? arguments[position] : ValueID();
    }

    bool mayPassObjectOfItsOwn(const BodyIndex* index, const LinkedCall& call, const Place& place) const
    {
        ValueID passed = index ? argumentAt(*index, call, m_program.at(place.function), place.entryParameter) : ValueID();
        while (passed && !index->isParameter(passed)) {
            BodyValue definition = index->value(passed);
            if (!inputPassThroughOpcodes.contains(definition.opcode()))
                return AliasSets::allocations.contains(definition.opcode());
            passed = definition.operand(0);
        }
        return !!passed;
    }

    void considerAll(const Places& places)
    {
        for (const Place& place : places)
            consider(place);
    }

    void consider(const Place& place)
    {
        if (!m_candidatesByPlace.add(placeOf(place), notCandidate).isNewEntry)
            return;
        const Function& function = m_program.at(place.function);
        const ThinSummary& summary = m_flow.summaries.at(place.function);
        std::optional<unsigned> receiver = function.thisParameterIndex();
        bool isReceiver = receiver && *receiver == place.entryParameter;
        bool isPositional = place.entryParameter >= function.firstParameterIndex() && place.entryParameter < function.firstParameterIndex() + function.numberOfPositionalParameters();
        if (!m_tables.function(place.function).isReached || !mayBeInspected(function) || (!isReceiver && !isPositional) || (isReceiver && function.kind != FunctionKind::Method && function.kind != FunctionKind::Getter))
            return;
        LinkType type = m_flow.typeAt(m_flow.parameter(place.function, place.entryParameter));
        type.primitives &= ~Primitive::UndefinedOrNull;
        const BodyIndex* index = m_bodies.find(place.function);
        if (!isObjectOfOneLayout(type) || !isLayoutOfScalars(type.layout) || !index || place.entryParameter >= index->block(entryBlock).numberOfParameters() || summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval))
            return;
        Candidate candidate { place.function, place.entryParameter, type.layout };
        candidate.isScalarizable = mayBeScalarized(place) && !index->isUnused(index->block(entryBlock).parameter(place.entryParameter));
        candidate.hasLoop = aliasesOf(place.function, *index).hasLoop();
        if (!onlyReadsParameter(*index, candidate))
            return;
        m_candidatesByPlace.set(placeOf(place), m_candidates.size());
        m_candidates.append(WTF::move(candidate));
    }

    bool onlyReadsParameter(const BodyIndex& index, Candidate& candidate)
    {
        ValueID parameter = index.block(entryBlock).parameter(candidate.entryParameter);
        std::optional<NamesOfObject> names = collectNamesOfGiven(candidate.function, index, singleElementSpan(parameter));
        if (!names || !onlyReadsNames(index, *names, candidate.function, candidate.layout, candidate.placesPassedTo))
            return false;
        findInspectedFields(index, *names, candidate);
        return true;
    }

    void findInspectedFields(const BodyIndex& index, const NamesOfObject& objects, Candidate& candidate)
    {
        const Layout& layout = m_program.at(candidate.layout);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            InspectedField inspected { field, layoutHeldBy({ candidate.layout, field }), { }, false };
            if (!inspected.held)
                continue;
            Vector<ValueID> reads;
            objects.allUses([&](const AliasSets::Use& use) {
                if (use.kind == AliasSets::UseKind::Operand && !use.operandIndex && readsField(index.value(use.user()), candidate.layout, field))
                    reads.append(use.user());
                return true;
            });
            std::ranges::sort(reads);
            inspected.isRead = !reads.isEmpty();
            std::optional<NamesOfObject> names = collectNamesOfGiven(candidate.function, index, reads.span());
            if (names && onlyReadsNames(index, *names, candidate.function, inspected.held, inspected.placesPassedTo))
                candidate.inspectedFields.append(WTF::move(inspected));
        }
    }

    bool readsField(const BodyValue& value, LayoutID layout, uint32_t field) const
    {
        static_assert(!OperandIndex::PropGet::object && !OperandIndex::PrivateGet::object);
        if (value.opcode() == Opcode::PrivateGet)
            return value.as<Opcode::PrivateGet>().layout() == layout && value.as<Opcode::PrivateGet>().field() == field;
        if (value.opcode() != Opcode::PropGet)
            return false;
        if (value.key() == m_program.at(layout).fields[field].key)
            return true;
        FunctionID getter = findGetter(layout, value.key());
        return getter && fieldThatGetterReturns(layout, getter) == field;
    }

    std::optional<uint32_t> fieldThatGetterReturns(LayoutID layout, FunctionID getter) const
    {
        const BodyIndex* index = m_bodies.find(getter);
        std::optional<unsigned> receiver = m_program.at(getter).thisParameterIndex();
        if (!index || !receiver || index->numberOfBlocks() != 1 || index->terminator(entryBlock).kind() != TerminatorKind::Return)
            return std::nullopt;
        static constexpr OpcodeSet others { Opcode::GetModuleVariable, Opcode::CheckInitialized };
        ValueID returned = index->terminator(entryBlock).operand(0);
        while (returned && !index->isParameter(returned) && inputPassThroughOpcodes.contains(index->value(returned).opcode()))
            returned = index->value(returned).operand(0);
        std::optional<uint32_t> found;
        for (BodyValue value : index->block(entryBlock).values()) {
            if (others.contains(value.opcode()) || inputPassThroughOpcodes.contains(value.opcode()))
                continue;
            ValueID parameter = index->block(entryBlock).parameter(*receiver);
            if (found || value.id() != returned || value.operand(0) != parameter)
                return std::nullopt;
            static_assert(OperandIndex::PropGet::object == 0 && OperandIndex::PrivateGet::object == 0);
            const Vector<Field>& fields = m_program.at(layout).fields;
            if (value.opcode() == Opcode::PrivateGet && isPlainPrivateField(layout, value.as<Opcode::PrivateGet>().layout(), value.as<Opcode::PrivateGet>().field()))
                found = value.as<Opcode::PrivateGet>().field();
            else if (value.opcode() == Opcode::PropGet && isPlainField(layout, value.key()))
                found = static_cast<uint32_t>(std::ranges::find(fields, value.key(), &Field::key) - fields.begin());
            else
                return std::nullopt;
        }
        return found;
    }

    FunctionID findGetter(LayoutID layout, PropertyKeyID key) const
    {
        Places places;
        return collectReceiversOfRead(layout, key, places, LooksThroughGetters::No) && places.size() == 1 ? places[0].function : FunctionID();
    }

    LayoutID layoutHeldBy(FieldReference field)
    {
        LinkType type = m_flow.typeAt(m_flow.field(field));
        type.primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent);
        if (!isObjectOfOneLayout(type))
            return { };
        const Layout& held = m_program.at(type.layout);
        FunctionID constructor = held.constructor;
        if (held.kind != LayoutKind::Instance || !constructor || held.fields.size() > maxFields || m_program.at(constructor).kind != FunctionKind::BaseConstructor || m_program.at(constructor).constructedClass != type.layout || !constructorKeepsNothing(constructor, type.layout))
            return { };
        return type.layout;
    }

    struct WritersOfLayout {
        LayoutID layout;
        bool mayBeEnteredOutOfSight { false };
        Vector<FunctionID, 4> writers;
        BitVector leading;
    };

    void dropFieldsWrittenMeanwhile()
    {
        HashMap<uint32_t, WritersOfLayout, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> byLayout;
        for (const Candidate& candidate : m_candidates) {
            for (const InspectedField& inspected : candidate.inspectedFields) {
                if (mayBecomeParameter(candidate) && inspected.isRead)
                    byLayout.add(inspected.held.index(), WritersOfLayout { inspected.held });
            }
        }
        if (byLayout.isEmpty())
            return;
        HashMap<PropertyKeyID, Vector<uint32_t, 2>> layoutsUnderKey;
        for (const auto& entry : byLayout) {
            for (const Field& field : m_program.at(entry.value.layout).fields)
                layoutsUnderKey.add(field.key, Vector<uint32_t, 2>()).iterator->value.append(entry.key);
        }
        for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
            FunctionID id(number);
            if (!m_tables.function(id).isReached)
                continue;
            auto noteWriter = [&](uint32_t layout, const LinkType& subject) {
                WritersOfLayout& writers = byLayout.find(layout)->value;
                if (mayBeOf(subject, writers.layout) && (writers.writers.isEmpty() || writers.writers.last() != id))
                    writers.writers.append(id);
            };
            auto noteWriterUnderKey = [&](PropertyKeyID key, const LinkType& subject) {
                if (!key) {
                    for (uint32_t layout : byLayout.keys())
                        noteWriter(layout, subject);
                } else if (auto found = layoutsUnderKey.find(key); found != layoutsUnderKey.end()) {
                    for (uint32_t layout : found->value)
                        noteWriter(layout, subject);
                }
            };
            const ThinSummary& summary = m_flow.summaries.at(id);
            for (const SummaryAccess& access : summary.accesses) {
                if (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define)
                    continue;
                PropertyKeyID key = m_flow.keyOf(id, access);
                if ((!key || layoutsUnderKey.contains(key)) && !writesObjectInBirth(id, access))
                    noteWriterUnderKey(key, m_flow.typeAt(m_flow.nodeOf(id, access.subject)));
            }
            for (const SummaryChange& change : summary.changes) {
                if (change.kind != SummaryChange::Kind::BecomesPrototype)
                    noteWriterUnderKey({ }, m_flow.typeAt(m_flow.nodeOf(id, change.subject)));
            }
        }
        for (auto& entry : byLayout)
            findFunctionsThatLeadTo(entry.value);
        for (Candidate& candidate : m_candidates) {
            if (!mayBecomeParameter(candidate))
                continue;
            candidate.inspectedFields.removeAllMatching([&](const InspectedField& inspected) {
                auto found = byLayout.find(inspected.held.index());
                return found == byLayout.end() || found->value.mayBeEnteredOutOfSight || found->value.leading.get(candidate.function.index());
            });
        }
    }

    bool writesObjectInBirth(FunctionID id, const SummaryAccess& access)
    {
        const Function& function = m_program.at(id);
        const BodyIndex* index = access.site && (function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::Initializer) ? m_bodies.find(id) : nullptr;
        if (!index || index->isParameter(access.site) || m_flow.mayStoreIntoOlderObjectThatSuperCallAnswered(id, access))
            return false;
        BodyValue store = index->value(access.site);
        static_assert(OperandIndex::PropSet::object == OperandIndex::PropDefine::object && OperandIndex::PropSet::object == OperandIndex::PrivateSet::object && OperandIndex::PropSet::object == OperandIndex::PrivateDefine::object);
        static constexpr OpcodeSet stores { Opcode::PropSet, Opcode::PropDefine, Opcode::PrivateSet, Opcode::PrivateDefine };
        if (!stores.contains(store.opcode()))
            return false;
        ValueID object = store.operand(OperandIndex::PropSet::object);
        for (;;) {
            if (!object)
                return false;
            if (index->isParameter(object)) {
                std::optional<unsigned> parameter = function.thisParameterIndex();
                return function.kind == FunctionKind::Initializer && parameter && object == index->block(entryBlock).parameter(*parameter) && isObjectOfOneLayout(m_flow.typeAt(m_flow.nodeOf(id, access.subject)));
            }
            BodyValue definition = index->value(object);
            if (!inputPassThroughOpcodes.contains(definition.opcode()))
                return function.kind == FunctionKind::BaseConstructor && definition.opcode() == Opcode::NewObject;
            object = definition.operand(0);
        }
    }

    bool mayBeOf(const LinkType& type, LayoutID layout) const
    {
        if (type.hasUnknownObjects && m_flow.escapedLayouts.get(layout.index()))
            return true;
        switch (type.objects) {
        case LinkType::Objects::None:
        case LinkType::Objects::Prototype:
        case LinkType::Objects::Prototypes:
            return false;
        case LinkType::Objects::Layout:
            return type.layout == layout;
        case LinkType::Objects::Set:
            return std::ranges::contains(m_flow.layoutsOfSet(type), layout);
        case LinkType::Objects::Family:
            return std::ranges::contains(m_flow.layoutsOfFamily(type.family), layout);
        case LinkType::Objects::Lost:
            return m_flow.lostLayouts.get(layout.index());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void findFunctionsThatLeadTo(WritersOfLayout& writers)
    {
        writers.leading = BitVector(m_program.functions.size());
        Vector<FunctionID> toVisit;
        auto add = [&](FunctionID id) {
            if (writers.leading.quickSet(id.index()))
                return;
            const Function& function = m_program.at(id);
            toVisit.append(id);
            if (!function.isModuleBody())
                writers.mayBeEnteredOutOfSight |= function.isCoroutine() || !isEnteredByListedCallsAlone(id);
        };
        for (FunctionID writer : writers.writers)
            add(writer);
        while (!toVisit.isEmpty() && !writers.mayBeEnteredOutOfSight) {
            for (FunctionID caller : callersOf(toVisit.takeLast()))
                add(caller);
        }
    }

    bool isEnteredByListedCallsAlone(FunctionID id) const
    {
        const LinkedFunction& linked = m_tables.function(id);
        FunctionKind kind = m_program.at(id).kind;
        bool isKindOfCallee = kind == FunctionKind::Plain || kind == FunctionKind::Arrow || kind == FunctionKind::Method;
        return isKindOfCallee && linked.everyCallerIsKnown && linked.everyCallHasOneCallee && !linked.isIterationCallback && !linked.isRepeatedCallback && !linked.isTrapOfProxy;
    }

    std::span<const FunctionID> callersOf(FunctionID callee)
    {
        if (m_firstCallerOf.isEmpty()) {
            m_firstCallerOf.fill(0, m_program.functions.size() + 2);
            for (const LinkedCall& call : m_tables.calls)
                ++m_firstCallerOf[call.callee.index() + 2];
            for (uint32_t function = 0; function < m_program.functions.size(); ++function)
                m_firstCallerOf[function + 2] += m_firstCallerOf[function + 1];
            m_callers.grow(m_tables.calls.size());
            for (const LinkedCall& call : m_tables.calls)
                m_callers[m_firstCallerOf[call.callee.index() + 1]++] = call.caller;
        }
        return m_callers.span().subspan(m_firstCallerOf[callee.index()], m_firstCallerOf[callee.index() + 1] - m_firstCallerOf[callee.index()]);
    }

    Vector<LinkedInspectedField, 2> inspectedFieldsOf(const Candidate& candidate) const
    {
        Vector<LinkedInspectedField, 2> fields;
        for (const InspectedField& inspected : candidate.inspectedFields) {
            if (inspected.isRead)
                fields.append({ inspected.field, inspected.held });
        }
        return fields;
    }

    bool onlyReadsNames(const BodyIndex& index, const NamesOfObject& names, FunctionID function, LayoutID layout, Places& placesPassedTo) const
    {
        bool onlyReads = names.allUses([&](const AliasSets::Use& use) {
            return use.kind == AliasSets::UseKind::Operand && onlyReadsOperand(function, layout, placesPassedTo, index.value(use.user()), use.operandIndex);
        });
        dropRepeatedPlaces(placesPassedTo);
        return onlyReads;
    }

    void dropRepeatedPlaces(Places& places) const
    {
        std::ranges::sort(places, { }, [&](const Place& place) { return placeOf(place); });
        places.shrink(std::ranges::unique(places, { }, [&](const Place& place) { return placeOf(place); }).begin() - places.begin());
    }

    bool onlyReadsOperand(FunctionID function, LayoutID layout, Places& placesPassedTo, const BodyValue& user, uint32_t operandIndex) const
    {
        Opcode opcode = user.opcode();
        if (inputPassThroughOpcodes.contains(opcode))
            return !operandIndex;
        switch (opcode) {
        case Opcode::ToBoolean:
        case Opcode::TypeTest:
            return true;
        case Opcode::PropGet:
            return operandIndex == OperandIndex::PropGet::object && !user.isFreeName() && (isPlainField(layout, user.key()) || collectReceiversOfRead(layout, user.key(), placesPassedTo));
        case Opcode::PrivateGet:
            return operandIndex == OperandIndex::PrivateGet::object && isPlainPrivateField(layout, user.as<Opcode::PrivateGet>().layout(), user.as<Opcode::PrivateGet>().field());
        case Opcode::Call:
        case Opcode::CallDirect:
            return collectPlacesThatTake(function, user, operandIndex, placesPassedTo);
        default:
            return false;
        }
    }

    enum class LooksThroughGetters : bool { No, Yes };
    bool collectReceiversOfRead(LayoutID layout, PropertyKeyID key, Places& places, LooksThroughGetters looksThrough = LooksThroughGetters::Yes) const
    {
        if (!key || std::ranges::contains(m_program.at(layout).fields, key, &Field::key) || m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype) || m_flow.escapedLayouts.get(layout.index()))
            return false;
        if (m_tables.layouts[layout.index()].possibleChanges.containsAny({ LayoutChange::GetsAccessor, LayoutChange::ChangesPrototype, LayoutChange::GainsProperty }))
            return false;
        uint32_t stepsLeft = m_program.layouts.size();
        for (LayoutID current = layout; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
            if (m_flow.findNameOnPrototype(current, key))
                return false;
        }
        LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, key);
        if (!members.mayAnswer() || members.lookupGoesOnAboveClasses())
            return false;
        for (const LinkFlow::FoundMember& found : members) {
            if (found.kind != MethodKind::Getter)
                continue;
            const LinkType getter = m_flow.typeAt(m_flow.member(found.classLayout, ClassSide::Prototype, found.index));
            if (getter.functions != LinkType::Functions::Closure || getter.classes != LinkType::Classes::None || getter.builtin || getter.hasUnknownObjects || getter.objects != LinkType::Objects::None)
                return false;
            const Function& entered = m_program.at(getter.function);
            if (entered.kind != FunctionKind::Getter || !entered.hasBody())
                return false;
            if (std::optional<unsigned> parameter = entered.thisParameterIndex(); parameter && (looksThrough == LooksThroughGetters::No || !fieldThatGetterReturns(layout, getter.function)))
                places.append({ getter.function, *parameter });
        }
        return true;
    }

    bool isPlainPrivateField(LayoutID layout, LayoutID declaring, uint32_t field) const
    {
        constexpr OptionSet<FieldFlags> brands { FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods };
        const Vector<Field>& fields = m_program.at(layout).fields;
        return layout == declaring && field < fields.size() && fields[field].form.isEmpty() && !fields[field].flags.containsAny(brands);
    }

    bool areInspected(std::span<const Place> places, LayoutID layout) const
    {
        return std::ranges::all_of(places, [&](const Place& place) {
            const Candidate* taker = findCandidate(place);
            return taker && taker->isInspected && taker->layout == layout;
        });
    }

    Vector<Vector<uint32_t, 2>> collectGivers() const
    {
        Vector<Vector<uint32_t, 2>> givers(m_candidates.size());
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            for (const Place& place : m_candidates[number].placesPassedTo) {
                if (findCandidate(place))
                    givers[m_candidatesByPlace.get(placeOf(place))].append(number);
            }
        }
        return givers;
    }

    void dropCandidatesPassedOn()
    {
        Vector<Vector<uint32_t, 2>> givers = collectGivers();
        Vector<uint32_t> toVisit;
        auto drop = [&](uint32_t number) {
            if (!std::exchange(m_candidates[number].isInspected, false))
                return;
            toVisit.append(number);
        };
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            const Candidate& candidate = m_candidates[number];
            bool isInspected = std::ranges::all_of(candidate.placesPassedTo, [&](const Place& place) {
                const Candidate* taker = findCandidate(place);
                return taker && taker->layout == candidate.layout;
            });
            if (!isInspected)
                drop(number);
        }
        while (!toVisit.isEmpty()) {
            for (uint32_t giver : givers[toVisit.takeLast()])
                drop(giver);
        }
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            Candidate& candidate = m_candidates[number];
            candidate.hasLoop |= !std::ranges::all_of(candidate.placesPassedTo, [&](const Place& place) {
                return !!findCandidate(place);
            });
            if (candidate.hasLoop)
                toVisit.append(number);
        }
        while (!toVisit.isEmpty()) {
            for (uint32_t giver : givers[toVisit.takeLast()]) {
                if (!std::exchange(m_candidates[giver].hasLoop, true))
                    toVisit.append(giver);
            }
        }
        dropFieldsPassedOn(givers);
        propagateFieldReads(givers);
    }

    void dropFieldsPassedOn(const Vector<Vector<uint32_t, 2>>& givers)
    {
        struct Lost {
            uint32_t candidate { 0 };
            uint32_t field { 0 };
        };
        Vector<Lost> lost;
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            Candidate& candidate = m_candidates[number];
            candidate.inspectedFields.removeAllMatching([&](const InspectedField& inspected) {
                bool stays = staysInspected(inspected);
                if (!stays)
                    lost.append({ number, inspected.field });
                return !stays;
            });
        }
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            Candidate& candidate = m_candidates[number];
            candidate.inspectedFields.removeAllMatching([&](const InspectedField& inspected) {
                bool stays = std::ranges::all_of(candidate.placesPassedTo, [&](const Place& place) {
                    const Candidate* taker = findCandidate(place);
                    return taker && std::ranges::contains(taker->inspectedFields, inspected.field, &InspectedField::field);
                });
                if (!stays)
                    lost.append({ number, inspected.field });
                return !stays;
            });
        }
        while (!lost.isEmpty()) {
            Lost last = lost.takeLast();
            for (uint32_t giver : givers[last.candidate]) {
                bool hadField = m_candidates[giver].inspectedFields.removeFirstMatching([&](const InspectedField& inspected) {
                    return inspected.field == last.field;
                });
                if (hadField)
                    lost.append({ giver, last.field });
            }
        }
    }

    void propagateFieldReads(const Vector<Vector<uint32_t, 2>>& givers)
    {
        struct Read {
            uint32_t candidate { 0 };
            uint32_t field { 0 };
        };
        Vector<Read> toVisit;
        for (uint32_t number = 0; number < m_candidates.size(); ++number) {
            for (const InspectedField& inspected : m_candidates[number].inspectedFields) {
                if (inspected.isRead)
                    toVisit.append({ number, inspected.field });
            }
        }
        while (!toVisit.isEmpty()) {
            Read last = toVisit.takeLast();
            for (uint32_t giver : givers[last.candidate]) {
                auto inspected = std::ranges::find(m_candidates[giver].inspectedFields, last.field, &InspectedField::field);
                if (inspected != m_candidates[giver].inspectedFields.end() && !std::exchange(inspected->isRead, true))
                    toVisit.append({ giver, last.field });
            }
        }
    }

    bool staysInspected(const InspectedField& inspected) const
    {
        return std::ranges::all_of(inspected.placesPassedTo, [&](const Place& place) {
            const Candidate* taker = findCandidate(place);
            return taker && taker->isInspected && taker->layout == inspected.held && !taker->hasLoop;
        });
    }

    const Candidate* findCandidate(const Place& place) const
    {
        auto found = m_candidatesByPlace.find(placeOf(place));
        return found == m_candidatesByPlace.end() || found->value == notCandidate ? nullptr : &m_candidates[found->value];
    }

    void lookAtCall(const LinkedCall& call, Vector<CallThatPasses>& calls)
    {
        const Function& function = m_program.at(call.callee);
        for (uint32_t parameter = 0; parameter < function.numberOfPositionalParameters(); ++parameter) {
            Place place { call.callee, function.firstParameterIndex() + parameter };
            if (!mayBeScalarized(place))
                continue;
            consider(place);
            const Candidate* candidate = findCandidate(place);
            CallThatPasses found { place };
            if (!candidate || !candidate->isScalarizable || !passesUnsharedObject(call, *candidate, found)) {
                m_mayBeScalarized.quickClear(placeOf(place));
                continue;
            }
            PassedObject& object = m_passedObjects[found.object];
            if (!std::exchange(object.isConsidered, true)) {
                considerAll(object.placesPassedTo);
                for (const Place& giver : object.givers)
                    consider(giver);
            }
            calls.append(found);
        }
    }

    bool passesUnsharedObject(const LinkedCall& call, const Candidate& candidate, CallThatPasses& found)
    {
        const BodyIndex* index = call.site ? m_bodies.find(call.caller) : nullptr;
        ValueID passed = index ? argumentAt(*index, call, m_program.at(candidate.function), candidate.entryParameter) : ValueID();
        if (!passed || m_flow.summaries.at(call.caller).has(SummaryTrait::HasDirectEval))
            return false;
        std::optional<uint32_t> number = findPassedObject(call.caller, *index, passed, candidate.layout);
        if (!number)
            return false;
        found.object = *number;
        PassedObject& object = m_passedObjects[*number];
        LoopID loop = index->loopOf(index->blockOf(call.site));
        if (!object.canPassFields || (!object.names.allocations.isEmpty() && object.loopOfAllocations != loop) || (!object.givers.isEmpty() && loop))
            return false;
        if (!object.isInspected) {
            object.isInspected = isOnlyInspected(*index, object.names, candidate.layout, MayReturn::Yes, call.caller, &object.placesPassedTo);
            dropRepeatedPlaces(object.placesPassedTo);
        }
        return *object.isInspected;
    }

    std::optional<uint32_t> findPassedObject(FunctionID caller, const BodyIndex& index, ValueID passed, LayoutID layout)
    {
        const AliasSets& aliases = aliasesOf(caller, index);
        uint32_t set = aliases.setOf(passed);
        if (set == AliasSets::noSet)
            return std::nullopt;
        auto& objects = m_passedObjectsOfFunction.add(caller.index(), PassedObjectsBySet()).iterator->value;
        auto added = objects.add(static_cast<uint64_t>(set) << 32 | layout.index(), static_cast<uint32_t>(m_passedObjects.size()));
        if (!added.isNewEntry)
            return added.iterator->value;
        PassedObject object;
        object.layout = layout;
        if (std::optional<NamesOfObject> names = collectNames(index, aliases, singleElementSpan(passed), { })) {
            object.names = WTF::move(*names);
            for (ValueID parameter : object.names.entryParameters)
                object.givers.append({ caller, parameter.index() - index.block(entryBlock).firstParameter().index() });
            object.canPassFields = areAllocatedInOneLoop(caller, index, object);
        }
        m_passedObjects.append(WTF::move(object));
        return added.iterator->value;
    }

    bool areAllocatedInOneLoop(FunctionID caller, const BodyIndex& index, PassedObject& object)
    {
        if (object.names.allocations.isEmpty())
            return !object.names.entryParameters.isEmpty();
        object.loopOfAllocations = index.loopOf(index.blockOf(object.names.allocations[0]));
        return std::ranges::all_of(object.names.allocations, [&](ValueID allocation) {
            return index.loopOf(index.blockOf(allocation)) == object.loopOfAllocations;
        }) && std::ranges::all_of(object.names.allocations, [&](ValueID allocation) {
            std::optional<LinkedFreshResult> allocated = lookAtAllocation(caller, index, allocation);
            return allocated && allocated->layout == object.layout;
        });
    }

    void dropSharedParameters(const Vector<CallThatPasses>& calls)
    {
        ByFunction<Vector<uint32_t, 1>> objectsByGiver;
        for (uint32_t number = 0; number < m_passedObjects.size(); ++number) {
            for (const Place& giver : m_passedObjects[number].givers)
                objectsByGiver.add(placeOf(giver), Vector<uint32_t, 1>()).iterator->value.append(number);
        }
        Vector<uint32_t> dropped;
        auto drop = [&](const Place& place) {
            if (!mayBeScalarized(place))
                return;
            m_mayBeScalarized.quickClear(placeOf(place));
            dropped.append(placeOf(place));
        };
        for (const CallThatPasses& call : calls) {
            if (!findCandidate(call.taker))
                drop(call.taker);
            else
                m_passedObjects[call.object].takers.append(call.taker);
        }
        auto share = [&](PassedObject& object) {
            for (const Place& taker : std::exchange(object.takers, { }))
                drop(taker);
        };
        for (PassedObject& object : m_passedObjects) {
            bool isUnshared = object.takers.isEmpty() || (areInspected(object.placesPassedTo.span(), object.layout) && std::ranges::all_of(object.givers, [&](const Place& giver) {
                const Candidate* given = findCandidate(giver);
                return given && mayBecomeParameter(*given) && given->layout == object.layout;
            }));
            if (!isUnshared)
                share(object);
        }
        while (!dropped.isEmpty()) {
            auto objects = objectsByGiver.find(dropped.takeLast());
            if (objects == objectsByGiver.end())
                continue;
            for (uint32_t number : objects->value)
                share(m_passedObjects[number]);
        }
    }

    const LinkedFreshResult* findResult(FunctionID function)
    {
        auto known = m_answers.find(function.index());
        if (known != m_answers.end())
            return known->value->state == State::IsFresh ? &known->value->result : nullptr;
        Answer* answer = m_answers.add(function.index(), makeUniqueWithoutFastMallocCheck<Answer>()).iterator->value.get();
        std::optional<LinkedFreshResult> result = lookAtFunction(function);
        answer->state = result ? State::IsFresh : State::IsNot;
        if (result)
            answer->result = WTF::move(*result);
        return result ? &answer->result : nullptr;
    }

    std::optional<LinkedFreshResult> lookAtFunction(FunctionID id)
    {
        const Function& function = m_program.at(id);
        bool isKindOfCallee = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow || function.kind == FunctionKind::Method;
        if (!isKindOfCallee || !function.hasBody() || function.isCoroutine() || function.isModuleBody() || function.results.size() != 1 || !returnsObjectOfOneLayout(id))
            return std::nullopt;
        const ThinSummary& summary = m_flow.summaries.at(id);
        if (summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval) || summary.has(SummaryTrait::NeverReturns))
            return std::nullopt;
        const BodyIndex* index = m_bodies.find(id);
        if (!index)
            return std::nullopt;
        std::optional<NamesOfObject> names = namesOfReturnedObject(id, *index);
        if (!names || names->allocations.isEmpty())
            return std::nullopt;
        std::optional<LinkedFreshResult> found;
        for (ValueID allocation : names->allocations) {
            std::optional<LinkedFreshResult> allocated = lookAtAllocation(id, *index, allocation);
            if (!allocated)
                return std::nullopt;
            if (!allocated->layout)
                continue;
            if (found && (found->layout != allocated->layout || found->fields != allocated->fields))
                return std::nullopt;
            found = WTF::move(allocated);
        }
        if (!found)
            return std::nullopt;
        found->function = id;
        if (!isOnlyInspected(*index, *names, found->layout, MayReturn::Yes))
            return std::nullopt;
        return found;
    }

    std::optional<LinkedFreshResult> lookAtAllocation(FunctionID function, const BodyIndex& index, ValueID allocation)
    {
        BodyValue made = index.value(allocation);
        switch (made.opcode()) {
        case Opcode::NewObject: {
            auto literal = made.as<Opcode::NewObject>();
            const Layout& layout = m_program.at(literal.layout());
            if (literal.prototypeOperand() || layout.kind != LayoutKind::Record || layout.fields.size() < 2 || layout.fields.size() > maxFields || literal.fields().size() != layout.fields.size() || literal.valuesOperands().size() != layout.fields.size())
                return std::nullopt;
            LinkedFreshResult result { function, literal.layout(), { } };
            for (uint32_t field : literal.fields())
                result.fields.append(field);
            return result;
        }
        case Opcode::Construct: {
            std::optional<Vector<FunctionID, 4>> callees = calleesOf(function, allocation);
            if (made.as<Opcode::Construct>().newTargetOperand() || !callees || callees->size() != 1)
                return std::nullopt;
            const Function& constructor = m_program.at(callees->first());
            LayoutID constructed = constructor.constructedClass;
            if (constructor.kind != FunctionKind::BaseConstructor || !constructed || !constructorKeepsNothing(callees->first(), constructed))
                return std::nullopt;
            const Layout& layout = m_program.at(constructed);
            if (layout.kind != LayoutKind::Instance || layout.fields.size() < 2 || layout.fields.size() > maxFields)
                return std::nullopt;
            return LinkedFreshResult { function, constructed, { } };
        }
        case Opcode::Call: {
            std::optional<Vector<FunctionID, 4>> callees = calleesOf(function, allocation);
            if (!callees)
                return std::nullopt;
            LinkedFreshResult found;
            for (FunctionID callee : *callees) {
                if (callee == function || m_tables.function(callee).neverReturns)
                    continue;
                const LinkedFreshResult* result = findResult(callee);
                if (!result || (found.layout && (found.layout != result->layout || found.fields != result->fields)))
                    return std::nullopt;
                found = *result;
            }
            return found;
        }
        default:
            return std::nullopt;
        }
    }

    std::optional<uint32_t> findOnlyCallAt(FunctionID function, const ThinSummary& summary, ValueID site) const
    {
        Vector<uint32_t>& order = m_callsBySite.add(function.index(), Vector<uint32_t>()).iterator->value;
        auto siteOf = [&](uint32_t call) { return summary.calls[call].site.bits(); };
        if (order.isEmpty()) {
            order = Vector<uint32_t>(summary.calls.size(), [](size_t call) {
                return static_cast<uint32_t>(call);
            });
            std::ranges::sort(order, { }, siteOf);
        }
        auto found = std::ranges::equal_range(order, site.bits(), { }, siteOf);
        return found.size() == 1 ? std::optional(found.front()) : std::nullopt;
    }

    std::optional<Vector<FunctionID, 4>> calleesOf(FunctionID function, ValueID site) const
    {
        const ThinSummary& summary = m_flow.summaries.at(function);
        std::optional<uint32_t> place = findOnlyCallAt(function, summary, site);
        if (!place || summary.calls[*place].passesList || summary.calls[*place].isMadeByProtocol)
            return std::nullopt;
        Vector<FunctionID, 4> callees;
        if (summary.calls[*place].callee == SummaryCall::Callee::Binding) {
            callees.append(summary.calls[*place].function);
            return callees;
        }
        const LinkFlow::CalleesOfCall& entered = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[function.index()] + *place];
        if (!append(entered, callees))
            return std::nullopt;
        for (uint32_t shared : entered.shared()) {
            if (!append(m_flow.calleesOfSharedCallsAsSettled()[shared], callees) || !m_flow.calleesOfSharedCallsAsSettled()[shared].shared().isEmpty())
                return std::nullopt;
        }
        if (callees.isEmpty())
            return std::nullopt;
        return callees;
    }

    static bool append(const LinkFlow::CalleesOfCall& entered, Vector<FunctionID, 4>& callees)
    {
        static constexpr size_t maxCallees = 8;
        if (entered.mayEnterLost || entered.mayEnterUnseen || !entered.builtins().isEmpty() || callees.size() + entered.listed().size() > maxCallees)
            return false;
        for (FunctionID callee : entered.listed())
            callees.append(callee);
        return true;
    }

    bool constructorKeepsNothing(FunctionID id, LayoutID constructed)
    {
        auto added = m_constructorsThatKeepNothing.add(id.index(), false);
        if (!added.isNewEntry)
            return added.iterator->value;
        const BodyIndex* index = m_bodies.find(id);
        const ThinSummary& summary = m_flow.summaries.at(id);
        if (!index || m_program.at(id).isCoroutine() || summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval))
            return false;
        std::optional<NamesOfObject> names = namesOfReturnedObject(id, *index);
        if (!names || names->allocations.size() != 1)
            return false;
        BodyValue made = index->value(names->allocations[0]);
        if (made.opcode() != Opcode::NewObject || made.as<Opcode::NewObject>().layout() != constructed || !isOnlyInspected(*index, *names, constructed, MayReturn::Yes))
            return false;
        m_constructorsThatKeepNothing.set(id.index(), true);
        return true;
    }

    bool initializerKeepsNothing(FunctionID id, LayoutID constructed)
    {
        const Function& function = m_program.at(id);
        const BodyIndex* index = m_bodies.find(id);
        std::optional<unsigned> parameter = function.thisParameterIndex();
        if (function.kind != FunctionKind::Initializer || !index || !parameter || *parameter >= index->block(entryBlock).numberOfParameters())
            return false;
        ValueID receiver = index->block(entryBlock).parameter(*parameter);
        std::optional<NamesOfObject> names = collectNamesOfGiven(id, *index, singleElementSpan(receiver));
        return names && isOnlyInspected(*index, *names, constructed, MayReturn::No);
    }

    std::optional<NamesOfObject> namesOfReturnedObject(FunctionID id, const BodyIndex& index)
    {
        Vector<ValueID, 8> returned;
        BitVector seen(index.numberOfValues());
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (!index.isReachable(BlockID(number)))
                continue;
            BodyTerminator end = index.terminator(BlockID(number));
            if (end.kind() == TerminatorKind::ReturnCall)
                return std::nullopt;
            if (end.kind() != TerminatorKind::Return)
                continue;
            if (!end.operand(0) || !mayBeAllocated(index, end.operand(0), seen))
                return std::nullopt;
            returned.append(end.operand(0));
        }
        if (returned.isEmpty())
            return std::nullopt;
        std::optional<NamesOfObject> names = collectNames(index, aliasesOf(id, index), returned.span(), { });
        return names && names->entryParameters.isEmpty() ? names : std::nullopt;
    }

    static bool mayBeAllocated(const BodyIndex& index, ValueID value, BitVector& seen)
    {
        while (value && !seen.quickSet(value.index())) {
            if (index.isParameter(value))
                return index.blockOf(value) != entryBlock;
            BodyValue definition = index.value(value);
            if (!inputPassThroughOpcodes.contains(definition.opcode()))
                return AliasSets::allocations.contains(definition.opcode());
            value = definition.operand(0);
        }
        return !!value;
    }

    enum class MayReturn : bool { No, Yes };

    bool isOnlyInspected(const BodyIndex& index, const NamesOfObject& names, LayoutID layout, MayReturn mayReturn, FunctionID caller = { }, Places* placesPassedTo = nullptr)
    {
        return names.allUses([&](const AliasSets::Use& use) {
            switch (use.kind) {
            case AliasSets::UseKind::Operand:
                return onlyInspects(index, index.value(use.user()), use.operandIndex, layout, caller, placesPassedTo);
            case AliasSets::UseKind::Subject:
                return false;
            case AliasSets::UseKind::Terminator:
                return mayReturn == MayReturn::Yes && index.terminator(use.block()).kind() == TerminatorKind::Return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    }

    bool isPlainField(LayoutID layout, PropertyKeyID key) const
    {
        return key && std::ranges::any_of(m_program.at(layout).fields, [&](const Field& field) { return field.key == key && field.form.isEmpty(); });
    }

    bool onlyInspects(const BodyIndex& index, const BodyValue& user, uint32_t operandIndex, LayoutID layout, FunctionID caller, Places* placesPassedTo)
    {
        Opcode opcode = user.opcode();
        if (placesPassedTo && (opcode == Opcode::Call || opcode == Opcode::CallDirect) && collectPlacesThatTake(caller, user, operandIndex, *placesPassedTo))
            return true;
        if (placesPassedTo && opcode == Opcode::PropGet && operandIndex == OperandIndex::PropGet::object && !user.isFreeName() && collectReceiversOfRead(layout, user.key(), *placesPassedTo))
            return true;
        if (inputPassThroughOpcodes.contains(opcode))
            return !operandIndex;
        switch (opcode) {
        case Opcode::ToBoolean:
        case Opcode::TypeTest:
        case Opcode::TypeOf:
        case Opcode::StrictEq:
            return true;
        case Opcode::PropGet:
            return operandIndex == OperandIndex::PropGet::object && !user.isFreeName() && isPlainField(layout, user.key());
        case Opcode::PropSet:
            return operandIndex == OperandIndex::PropSet::object && !user.isFreeName() && isPlainField(layout, user.key());
        case Opcode::PropDefine:
            return operandIndex == OperandIndex::PropDefine::object && isPlainField(layout, user.key());
        case Opcode::PrivateGet:
            return operandIndex == OperandIndex::PrivateGet::object && isPlainPrivateField(layout, user.as<Opcode::PrivateGet>().layout(), user.as<Opcode::PrivateGet>().field());
        case Opcode::PrivateSet:
            return operandIndex == OperandIndex::PrivateSet::object && isPlainPrivateField(layout, user.as<Opcode::PrivateSet>().layout(), user.as<Opcode::PrivateSet>().field());
        case Opcode::PrivateDefine:
            return operandIndex == OperandIndex::PrivateDefine::object && user.as<Opcode::PrivateDefine>().layout() == layout;
        case Opcode::CallDirect: {
            auto call = user.as<Opcode::CallDirect>();
            return operandIndex == numberOfNamedOperandsOf(Opcode::CallDirect) && call.argumentsOperands().size() == 1 && index.isUnused(user.id()) && initializerKeepsNothing(call.function(), layout);
        }
        default:
            return false;
        }
    }

    const LinkFlow& m_flow;
    const LinkTables& m_tables;
    const Program& m_program;
    const BodyIndexTable& m_bodies;
    ByFunction<std::unique_ptr<Answer>> m_answers;
    ByFunction<bool> m_constructorsThatKeepNothing;
    ByFunction<std::unique_ptr<AliasSets>> m_aliases;
    mutable ByFunction<Vector<uint32_t>> m_callsBySite;
    ByFunction<PassedObjectsBySet> m_passedObjectsOfFunction;
    Vector<PassedObject> m_passedObjects;
    Vector<uint32_t> m_firstCallerOf;
    Vector<FunctionID> m_callers;
    static constexpr uint32_t notCandidate = UINT32_MAX;
    BitVector m_functionsThatMayBeScalarized;
    BitVector m_mayBeScalarized;
    Vector<Candidate> m_candidates;
    ByFunction<uint32_t> m_candidatesByPlace;
};

} // anonymous namespace

Vector<LinkedFreshResult> findFreshResults(const LinkFlow& flow, const LinkTables& tables)
{
    if (!flow.mechanisms.containsAll({ Mechanism::EscapeAnalysis, Mechanism::DirectCalls }) || tables.functions.size() != flow.program.functions.size())
        return { };
    return FreshResults(flow, tables).find();
}

Vector<LinkedInspectedParameter> findInspectedParameters(const LinkFlow& flow, const LinkTables& tables)
{
    if (!flow.mechanisms.containsAll({ Mechanism::EscapeAnalysis, Mechanism::DirectCalls }) || tables.functions.size() != flow.program.functions.size())
        return { };
    return FreshResults(flow, tables).findParameters();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
