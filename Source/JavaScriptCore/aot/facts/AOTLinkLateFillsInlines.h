/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class WriterOfLinkLayouts::LateFillFinder {
    WTF_MAKE_NONCOPYABLE(LateFillFinder);
public:
    explicit LateFillFinder(const WriterOfLinkLayouts& writer)
        : m_writer(writer)
        , m_flow(writer.m_flow)
        , m_program(writer.m_program)
        , m_bodies(bodiesLookedAt(writer.m_flow.looks))
        , m_push(m_program.keyOf(BuiltinNames::push))
        , m_unshift(m_program.keyOf(BuiltinNames::unshift))
        , m_add(m_program.keyOf(BuiltinNames::add))
        , m_set(m_program.keyOf(BuiltinNames::set))
        , m_forEach(m_program.keyOf(BuiltinNames::forEach))
        , m_map(m_program.keyOf(BuiltinNames::map))
        , m_slice(m_program.keyOf(BuiltinNames::slice))
        , m_sort(m_program.keyOf(BuiltinNames::sort))
        , m_reverse(m_program.keyOf(BuiltinNames::reverse))
        , m_concat(m_program.keyOf(BuiltinNames::concat))
        , m_arrayFromLength(m_program.builtins().builtinFromToken("CompilerHelpersArrayFromLength"_s))
        , m_arrayFromList(m_program.builtins().builtinFromToken("CompilerHelpersArrayFromList"_s))
    {
    }

    template<typename Functor>
    void forEachCaller(FunctionID function, const Functor& functor)
    {
        if (m_firstCallers.isEmpty())
            listCallers();
        const Vector<uint32_t>& firstCalls = m_flow.firstCallOfFunction;
        for (uint32_t place = m_firstCallers[function.index()]; place < m_firstCallers[function.index() + 1]; ++place) {
            uint32_t call = m_callers[place];
            FunctionID caller(static_cast<uint32_t>(std::ranges::upper_bound(firstCalls, call) - firstCalls.begin()) - 1);
            functor(caller, call - firstCalls[caller.index()]);
        }
    }

    bool areObjectsFilled(LayoutID layout, std::span<const LateFieldsFilled> stores, std::span<const LateFillAllocation> allocations)
    {
        m_layout = layout;
        m_fills.clear();
        m_parameterFills.clear();
        m_namedFills.clear();
        m_answers.clear();
        m_filledBlocks.clear();
        m_holdings.clear();
        for (const LateFieldsFilled& filled : stores)
            addStores(filled);
        if (m_fills.isEmpty())
            return false;
        while (!m_pending.isEmpty()) {
            auto [function, fill] = m_pending.takeLast();
            passFill(function, fill);
        }
        if (isFilledByConstructor())
            return true;
        FunctionID constructor = m_program.at(layout).constructor;
        if (m_flow.lostLayouts.get(layout.index()) || (constructor && m_flow.lostFunctions.get(constructor.index())))
            return false;
        return !allocations.empty() && std::ranges::all_of(allocations, [&](const LateFillAllocation& allocation) {
            m_allocatingFunction = allocation.function;
            m_allocatedObject = allocation.object;
            return allocation.object && isFilled(allocation.function, allocation.object, 0, allocation.object);
        });
    }

private:
    static constexpr uint8_t mostArraysAround = 3;
    static constexpr unsigned mostFunctionsFollowed = 48;

    struct Holder {
        enum class Kind : uint8_t {
            Value,
            Key,
            CountedKey,
            Variable,
            EnvironmentField,
        };
        Kind kind { Kind::Value };
        uint8_t depth { 0 };
        uint64_t id { 0 };

        uint64_t bits() const { return static_cast<uint64_t>(kind) << 61 | static_cast<uint64_t>(depth) << 58 | id; }
        friend bool operator==(const Holder&, const Holder&) = default;
    };
    struct Fill {
        Holder holder;
        BlockID block;
        ValueID site;
        ValueID bound;

        friend bool operator==(const Fill&, const Fill&) = default;
    };
    struct Event {
        BlockID block;
        ValueID site;
        bool isLoop { false };
        bool isAtEnd { false };
    };
    struct Use {
        enum class Kind : uint8_t {
            Operand,
            Return,
            Edge,
        };
        Kind kind { Kind::Operand };
        uint32_t user { 0 };
    };
    struct FunctionUses {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionUses);

        Vector<uint32_t> firstUses;
        Vector<Use> uses;
        Vector<std::pair<ValueID, uint32_t>> calls;
    };
    struct Holding {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Holding);

        Vector<ValueID, 8> aliases;
        Vector<Holder, 4> names;
        Vector<std::pair<ValueID, Use>, 8> otherUses;
        Vector<Event, 8> events;
        bool hasEvents { false };
    };
    using WordPair = std::pair<uint64_t, uint64_t>;

    const BodyIndex* bodyOf(FunctionID id) const
    {
        const BodyIndex* body = m_flow.reached.get(id.index()) ? m_bodies.find(id) : nullptr;
        return body && !body->hasIrreducibleFlow() ? body : nullptr;
    }

    auto calleesOf(FunctionID caller, uint32_t number) const { return m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + number].listed(); }
    bool mayEnterOthers(FunctionID caller, uint32_t number) const { return !m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + number].shared().isEmpty(); }

    void listCallers()
    {
        auto callees = m_flow.calleesOfCallsAsSettled();
        uint32_t numberOfFunctions = static_cast<uint32_t>(m_flow.firstCallOfFunction.size()) - 1;
        m_firstCallers.fill(0, numberOfFunctions + 2);
        Vector<std::pair<uint32_t, uint32_t>> callsOfCallees;
        callsOfCallees.reserveInitialCapacity(callees.size() / 4);
        for (uint32_t caller = 0; caller < numberOfFunctions; ++caller) {
            if (!m_flow.reached.get(caller))
                continue;
            for (uint32_t call = m_flow.firstCallOfFunction[caller]; call < m_flow.firstCallOfFunction[caller + 1]; ++call) {
                for (FunctionID callee : callees[call].listed()) {
                    ++m_firstCallers[callee.index() + 2];
                    callsOfCallees.append({ callee.index(), call });
                }
            }
        }
        for (uint32_t function = 2; function < m_firstCallers.size(); ++function)
            m_firstCallers[function] += m_firstCallers[function - 1];
        m_callers.grow(callsOfCallees.size());
        for (auto [callee, call] : callsOfCallees)
            m_callers[m_firstCallers[callee + 1]++] = call;
        for (const LinkFlow::CalleesOfCall& shared : m_flow.calleesOfSharedCallsAsSettled()) {
            for (FunctionID callee : shared.listed())
                m_calleesOfSharedCalls.set(callee.index());
        }
    }

    const FunctionUses& usesOf(FunctionID id)
    {
        auto added = m_usesOfFunctions.add(id.index(), nullptr);
        if (!added.isNewEntry)
            return *added.iterator->value;
        added.iterator->value = makeUnique<FunctionUses>();
        FunctionUses& found = *added.iterator->value;
        const BodyIndex& body = *m_bodies.find(id);
        found.firstUses.fill(0, body.numberOfValues() + 2);
        auto forEachUse = [&](const auto& functor) {
            for (BlockID id : body.blocksInDominatorOrder()) {
                BodyBlock block = body.block(id);
                for (BodyValue value : block.values()) {
                    value.forEachOperand([&](ValueID operand, uint32_t) {
                        if (operand)
                            functor(operand, Use { Use::Kind::Operand, value.id().index() });
                    });
                }
                BodyTerminator end = body.terminator(id);
                if (end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall) {
                    end.forEachOperand([&](ValueID operand, uint32_t) {
                        if (operand)
                            functor(operand, Use { Use::Kind::Return, id.index() });
                    });
                }
                for (BodyTarget target : end.targets()) {
                    uint32_t position = 0;
                    for (ValueID argument : target.arguments()) {
                        ValueID parameter = body.block(target.block()).parameter(position++);
                        if (argument)
                            functor(argument, Use { Use::Kind::Edge, parameter.index() });
                    }
                }
                body.forEachEdgeResult(id, [&](const BodyEdgeResult& result) {
                    functor(result.subject, Use { Use::Kind::Edge, result.parameter.index() });
                });
            }
        };
        forEachUse([&](ValueID value, Use) {
            ++found.firstUses[value.index() + 2];
        });
        for (uint32_t value = 2; value < found.firstUses.size(); ++value)
            found.firstUses[value] += found.firstUses[value - 1];
        found.uses.grow(found.firstUses.last());
        forEachUse([&](ValueID value, Use use) {
            found.uses[found.firstUses[value.index() + 1]++] = use;
        });
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (uint32_t number = 0; number < summary.calls.size(); ++number) {
            const SummaryCall& call = summary.calls[number];
            if (call.site && !call.isMadeByProtocol && !call.isMadeByHost && !call.passesList)
                found.calls.append({ call.site, number });
        }
        std::ranges::sort(found.calls);
        return found;
    }

    std::span<const Use> usesOf(FunctionID id, ValueID value)
    {
        const FunctionUses& uses = usesOf(id);
        return uses.uses.span().subspan(uses.firstUses[value.index()], uses.firstUses[value.index() + 1] - uses.firstUses[value.index()]);
    }

    std::optional<uint32_t> callAt(FunctionID id, ValueID site)
    {
        const FunctionUses& uses = usesOf(id);
        auto found = std::ranges::lower_bound(uses.calls, site, { }, &std::pair<ValueID, uint32_t>::first);
        return found != uses.calls.end() && found->first == site ? std::optional(found->second) : std::nullopt;
    }

    static ValueID rootOf(const BodyIndex& body, ValueID value)
    {
        while (value) {
            if (body.isParameter(value)) {
                ValueID subject = body.isInBlock(value, entryBlock) ? ValueID() : body.valueNarrowedInto(value);
                if (!subject)
                    return value;
                value = subject;
            } else if (inputPassThroughOpcodes.contains(body.opcodeOf(value)) && body.value(value).operand(0))
                value = body.value(value).operand(0);
            else
                return value;
        }
        return value;
    }

    static bool isEntryParameter(const BodyIndex& body, ValueID value) { return body.isParameter(value) && body.isInBlock(value, entryBlock); }

    static Holder holderOf(const BodyIndex& body, ValueID value, uint8_t depth)
    {
        value = rootOf(body, value);
        if (!body.isParameter(value)) {
            switch (body.opcodeOf(value)) {
            case Opcode::PropGet:
                if (!body.value(value).isFreeName() && body.value(value).key())
                    return { Holder::Kind::Key, depth, body.value(value).key().bits() };
                break;
            case Opcode::GetModuleVariable:
                return { Holder::Kind::Variable, depth, body.value(value).variable().index() };
            case Opcode::GetEnvironmentField: {
                auto read = body.value(value).as<Opcode::GetEnvironmentField>();
                return { Holder::Kind::EnvironmentField, depth, static_cast<uint64_t>(read.layout().index()) << 20 | read.field() };
            }
            default:
                break;
            }
        }
        return { Holder::Kind::Value, depth, value.index() };
    }

    static bool dominatesNextIterations(const BodyIndex& body, BlockID block, LoopID loop)
    {
        BlockID head = body.loop(loop).head;
        return std::ranges::all_of(body.membersOf(loop), [&](BlockID member) {
            return body.dominates(block, member) || std::ranges::none_of(body.terminator(member).targets(), [&](BodyTarget target) { return target.block() == head; });
        });
    }

    static bool leavesLoopElsewhere(const BodyIndex& body, LoopID loop, BlockID exit)
    {
        return std::ranges::any_of(body.membersOf(loop), [&](BlockID member) {
            return member != exit && std::ranges::any_of(body.terminator(member).targets(), [&](BodyTarget target) {
                TerminatorKind end = body.terminator(target.block()).kind();
                return !body.isInLoop(target.block(), loop) && end != TerminatorKind::Throw && end != TerminatorKind::Rethrow;
            });
        });
    }

    void addStores(const LateFieldsFilled& filled)
    {
        const BodyIndex* body = bodyOf(filled.function);
        if (!body)
            return;
        struct Group {
            ValueID object;
            Vector<std::pair<PropertyKeyID, ValueID>, 16> stores;
        };
        Vector<Group, 4> groups;
        auto isSameElement = [&](ValueID a, ValueID b) {
            if (a == b || body->isParameter(a) || body->isParameter(b) || body->opcodeOf(a) != Opcode::ElemGet || body->opcodeOf(b) != Opcode::ElemGet)
                return a == b;
            auto first = body->value(a).as<Opcode::ElemGet>();
            auto second = body->value(b).as<Opcode::ElemGet>();
            return body->stripCasts(first.propertyOperand()) == body->stripCasts(second.propertyOperand()) && isSameReadInLinkLayouts(*body, first.objectOperand(), second.objectOperand());
        };
        for (auto [key, site] : filled.stores) {
            if (body->isParameter(site) || body->opcodeOf(site) != Opcode::PropSet || body->value(site).isFreeName())
                continue;
            ValueID object = rootOf(*body, body->value(site).as<Opcode::PropSet>().objectOperand());
            if (!object)
                continue;
            size_t place = groups.findIf([&](const Group& group) { return isSameElement(group.object, object); });
            if (place == notFound) {
                if (groups.size() == longestListGoneThrough)
                    continue;
                place = groups.size();
                groups.append({ object, { } });
            }
            groups[place].stores.append({ key, site });
        }
        Vector<PropertyKeyID, 16> keys;
        for (const Group& group : groups) {
            ValueID last;
            for (size_t place = group.stores.size(); place-- && !last;) {
                BlockID block = body->blockOf(group.stores[place].second);
                keys.shrink(0);
                for (auto [key, site] : group.stores) {
                    if (site <= group.stores[place].second && body->dominates(body->blockOf(site), block) && !keys.contains(key))
                        keys.append(key);
                }
                if (m_writer.fillsMostLateFields(m_layout, keys.span()))
                    last = group.stores[place].second;
            }
            if (last)
                addFill(filled.function, *body, group.object, 0, body->blockOf(last), last);
        }
    }

    void addFill(FunctionID function, const BodyIndex& body, ValueID value, uint8_t depth, BlockID block, ValueID site)
    {
        if (value && depth <= mostArraysAround)
            addFill(function, { holderOf(body, value, depth), block, site });
    }

    void addFill(FunctionID function, const Fill& fill)
    {
        if ((fill.holder.kind == Holder::Kind::Key || fill.holder.kind == Holder::Kind::CountedKey) && !fill.holder.depth)
            return;
        Vector<Fill, 2>& fills = m_fills.add(function.index(), Vector<Fill, 2> { }).iterator->value;
        if (fills.contains(fill))
            return;
        fills.append(fill);
        m_pending.append({ function, fill });
    }

    void passFill(FunctionID function, const Fill& fill)
    {
        const BodyIndex* body = bodyOf(function);
        if (!body || fill.bound)
            return;
        if (fill.holder.kind != Holder::Kind::Value) {
            if (m_writer.runsOnEveryPath(function, *body, fill.block))
                noteNamedFill(function, fill);
            return;
        }
        ValueID value(static_cast<uint32_t>(fill.holder.id));
        if (isEntryParameter(*body, value)) {
            if (m_writer.runsOnEveryPath(function, *body, fill.block))
                noteParameterFill(function, value.index(), fill.holder.depth);
            return;
        }
        if (body->isParameter(value))
            return;
        switch (body->opcodeOf(value)) {
        case Opcode::ElemGet: {
            ValueID bound;
            ElementBound kind = findStoreBoundInLinkLayouts(*body, m_program, fill.block, value, bound);
            if (kind == ElementBound::Unknown)
                return;
            LoopID loop = body->loopOf(body->blockOf(value));
            BlockID head = body->loop(loop).head;
            if (fill.holder.depth < mostArraysAround && !leavesLoopElsewhere(*body, loop, head)) {
                Holder array = holderOf(*body, body->value(value).as<Opcode::ElemGet>().objectOperand(), fill.holder.depth + 1);
                if (kind == ElementBound::CountOfHolder)
                    array.kind = Holder::Kind::CountedKey;
                addFill(function, Fill { array, head, { }, kind == ElementBound::Other ? bound : ValueID() });
            }
            return;
        }
        case Opcode::ResultAt: {
            ValueID step = body->value(value).as<Opcode::ResultAt>().resultsOperand();
            if (body->value(value).as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value) || !step || body->isParameter(step) || body->opcodeOf(step) != Opcode::IteratorResume)
                return;
            ValueID open = body->value(step).operand(0);
            if (!open || body->isParameter(open) || body->opcodeOf(open) != Opcode::IteratorOpen)
                return;
            auto opened = body->value(open).as<Opcode::IteratorOpen>();
            BlockID stepping = body->blockOf(step);
            LoopID loop = body->loopOf(stepping);
            if (opened.iteratorKind() != IteratorKind::Sync || opened.isDestructuring() || opened.isDelegating() || !loop || body->isInLoop(body->blockOf(open), loop) || !body->isInLoop(fill.block, loop))
                return;
            if (dominatesNextIterations(*body, fill.block, loop) && !leavesLoopElsewhere(*body, loop, stepping))
                addFill(function, *body, opened.iterableOperand(), fill.holder.depth + 1, body->loop(loop).head, { });
            return;
        }
        default:
            if (fill.holder.depth) {
                forEachName(function, *body, value, fill.holder.depth, [&](const Holder& name) {
                    addFill(function, Fill { name, fill.block, fill.site });
                });
            }
            return;
        }
    }

    static ValueID argumentAt(const BodyIndex& body, const Function& callee, ValueID site, uint32_t parameter)
    {
        if (body.isParameter(site))
            return { };
        auto positional = [&](std::span<const ValueID> arguments) -> ValueID {
            if (parameter < callee.captures.size())
                return { };
            uint32_t place = parameter - callee.firstParameterIndex();
            return parameter >= callee.firstParameterIndex() && place < callee.numberOfPositionalParameters() && place < arguments.size() ? arguments[place] : ValueID();
        };
        switch (body.opcodeOf(site)) {
        case Opcode::Call: {
            auto call = body.value(site).as<Opcode::Call>();
            return parameter == callee.thisParameterIndex() ? call.thisValueOperand() : positional(call.argumentsOperands());
        }
        case Opcode::Construct:
            return positional(body.value(site).as<Opcode::Construct>().argumentsOperands());
        case Opcode::CallDirect: {
            std::span<const ValueID> arguments = body.value(site).as<Opcode::CallDirect>().argumentsOperands();
            return parameter < arguments.size() ? arguments[parameter] : ValueID();
        }
        default:
            return { };
        }
    }

    static std::optional<uint32_t> parameterAlike(const Function& callee, const Function& other, uint32_t parameter)
    {
        if (parameter == callee.thisParameterIndex())
            return other.thisParameterIndex() ? std::optional<uint32_t>(*other.thisParameterIndex()) : std::nullopt;
        if (parameter < callee.firstParameterIndex())
            return std::nullopt;
        return other.firstParameterIndex() + (parameter - callee.firstParameterIndex());
    }

    static WordPair parameterFillOf(FunctionID function, uint32_t parameter, uint8_t depth) { return { static_cast<uint64_t>(function.index()) << 32 | parameter, depth + 1 }; }

    void noteParameterFill(FunctionID function, uint32_t parameter, uint8_t depth)
    {
        if (!m_parameterFills.add(parameterFillOf(function, parameter, depth)).isNewEntry)
            return;
        const Function& filler = m_program.at(function);
        bool isCapture = parameter < filler.captures.size();
        FunctionID maker = isCapture ? m_flow.findOnlyReferrer(function) : FunctionID();
        ValueID captured = maker ? findCapturedValue(maker, function, parameter) : ValueID();
        forEachCaller(function, [&](FunctionID caller, uint32_t number) {
            const BodyIndex* body = bodyOf(caller);
            const SummaryCall& call = m_flow.summaries.at(caller).calls[number];
            if (!body || !call.site || call.passesList || call.isMadeByProtocol || call.isMadeByHost)
                return;
            if (isCapture && (body->isParameter(call.site) || body->opcodeOf(call.site) != Opcode::CallDirect)) {
                if (caller == maker && captured && calleesOf(caller, number).size() == 1 && !mayEnterOthers(caller, number))
                    addFill(caller, *body, captured, depth, body->blockOf(call.site), call.site);
                return;
            }
            bool fillsAlways = !mayEnterOthers(caller, number) && std::ranges::all_of(calleesOf(caller, number), [&](FunctionID callee) {
                std::optional<uint32_t> alike = callee == function ? std::optional(parameter) : isCapture ? std::nullopt : parameterAlike(filler, m_program.at(callee), parameter);
                return alike && m_parameterFills.contains(parameterFillOf(callee, *alike, depth));
            });
            if (fillsAlways)
                addFill(caller, *body, argumentAt(*body, filler, call.site, parameter), depth, body->blockOf(call.site), call.site);
        });
        if (isCapture || parameter != filler.firstParameterIndex())
            return;
        auto [caller, site] = findCallTakingClosure(function);
        const BodyIndex* body = caller ? bodyOf(caller) : nullptr;
        if (!body)
            return;
        auto call = body->value(site).as<Opcode::Call>();
        if (ValueID list = findIteratedList(*body, call.calleeOperand(), call.thisValueOperand(), call.argumentsOperands()))
            addFill(caller, *body, list, depth + 1, body->blockOf(site), site);
    }

    ValueID findIteratedList(const BodyIndex& body, ValueID callee, ValueID receiver, std::span<const ValueID> arguments) const
    {
        callee = callee ? rootOf(body, callee) : ValueID();
        if (!callee || body.isParameter(callee))
            return { };
        if (body.opcodeOf(callee) == Opcode::PropGet)
            return body.value(callee).key() && (body.value(callee).key() == m_forEach || body.value(callee).key() == m_map) ? receiver : ValueID();
        if (body.opcodeOf(callee) != Opcode::Builtin || arguments.size() < 2)
            return { };
        BuiltinID builtin = body.value(callee).as<Opcode::Builtin>().builtin();
        return builtin && (builtin == m_arrayFromList || builtin == m_program.builtins().canonicalID(Builtins::ArrayFrom)) ? arguments[0] : ValueID();
    }

    bool listsResultsOfCallback(const BodyIndex& body, ValueID callee) const
    {
        callee = callee ? rootOf(body, callee) : ValueID();
        if (!callee || body.isParameter(callee))
            return false;
        if (body.opcodeOf(callee) == Opcode::PropGet)
            return body.value(callee).key() && body.value(callee).key() == m_map;
        if (body.opcodeOf(callee) != Opcode::Builtin)
            return false;
        BuiltinID builtin = body.value(callee).as<Opcode::Builtin>().builtin();
        return builtin && (builtin == m_arrayFromLength || builtin == m_arrayFromList || builtin == m_program.builtins().canonicalID(Builtins::ArrayFrom));
    }

    ValueID findClosure(FunctionID maker, FunctionID function)
    {
        auto added = m_closuresOfMakers.add(maker.index(), Vector<std::pair<FunctionID, ValueID>> { });
        Vector<std::pair<FunctionID, ValueID>>& closures = added.iterator->value;
        if (added.isNewEntry) {
            const BodyIndex* body = bodyOf(maker);
            for (BlockID block : body && body->holdsAnyOf(OpcodeSet { Opcode::NewClosure }) ? body->blocksInDominatorOrder() : std::span<const BlockID> { }) {
                for (BodyValue value : body->block(block).values()) {
                    if (value.opcode() == Opcode::NewClosure)
                        closures.append({ value.as<Opcode::NewClosure>().function(), value.id() });
                }
            }
            std::ranges::sort(closures);
        }
        auto found = std::ranges::equal_range(closures, function, { }, &std::pair<FunctionID, ValueID>::first);
        return found.size() == 1 ? found.front().second : ValueID();
    }

    ValueID findCapturedValue(FunctionID maker, FunctionID function, uint32_t capture)
    {
        ValueID closure = findClosure(maker, function);
        if (!closure)
            return { };
        std::span<const ValueID> captures = m_bodies.find(maker)->value(closure).as<Opcode::NewClosure>().capturesOperands();
        return capture < captures.size() ? captures[capture] : ValueID();
    }

    std::pair<FunctionID, ValueID> findCallTakingClosure(FunctionID function)
    {
        FunctionID maker = m_flow.findOnlyReferrer(function);
        ValueID closure = maker ? findClosure(maker, function) : ValueID();
        if (!closure)
            return { };
        const BodyIndex& body = *m_bodies.find(maker);
        ValueID user = body.onlyUsingValue(closure);
        while (user && !body.isParameter(user) && inputPassThroughOpcodes.contains(body.opcodeOf(user)) && body.value(user).operand(0) == closure) {
            closure = user;
            user = body.onlyUsingValue(user);
        }
        if (!user || body.isParameter(user) || body.opcodeOf(user) != Opcode::Call || !std::ranges::contains(body.value(user).as<Opcode::Call>().argumentsOperands(), closure))
            return { };
        return { maker, user };
    }

    void noteNamedFill(FunctionID function, const Fill& fill)
    {
        const Holder& holder = fill.holder;
        if (holder.kind == Holder::Kind::CountedKey && fill.site)
            return;
        if (!m_namedFills.add(WordPair { function.index() + 1, holder.bits() }).isNewEntry)
            return;
        forEachCaller(function, [&](FunctionID caller, uint32_t number) {
            const BodyIndex* body = bodyOf(caller);
            ValueID site = m_flow.summaries.at(caller).calls[number].site;
            if (!body || !site || body->isParameter(site))
                return;
            bool fillsAlways = !mayEnterOthers(caller, number) && std::ranges::all_of(calleesOf(caller, number), [&](FunctionID callee) {
                return m_namedFills.contains(WordPair { callee.index() + 1, holder.bits() });
            });
            if (fillsAlways)
                addFill(caller, Fill { holder, body->blockOf(site), site });
        });
    }

    template<typename Functor>
    void forEachName(FunctionID function, const BodyIndex& body, ValueID value, uint8_t depth, const Functor& functor)
    {
        for (Use use : usesOf(function, value)) {
            if (use.kind != Use::Kind::Operand)
                continue;
            BodyValue user = body.value(ValueID(use.user));
            if (inputPassThroughOpcodes.contains(user.opcode()))
                forEachName(function, body, user.id(), depth, functor);
            else if (Holder name = findStoredName(user, value, depth); name.kind != Holder::Kind::Value)
                functor(name);
        }
    }

    Holder findStoredName(const BodyValue& user, ValueID value, uint8_t depth) const
    {
        switch (user.opcode()) {
        case Opcode::PropSet: {
            auto store = user.as<Opcode::PropSet>();
            if (store.valueOperand() == value && !user.isFreeName() && depth)
                return { Holder::Kind::Key, depth, user.key().bits() };
            break;
        }
        case Opcode::PutModuleVariable:
            return { Holder::Kind::Variable, depth, user.variable().index() };
        case Opcode::PutEnvironmentField: {
            auto store = user.as<Opcode::PutEnvironmentField>();
            if (store.valueOperand() == value)
                return { Holder::Kind::EnvironmentField, depth, static_cast<uint64_t>(store.layout().index()) << 20 | store.field() };
            break;
        }
        case Opcode::NewObject: {
            auto made = user.as<Opcode::NewObject>();
            std::span<const ValueID> values = made.valuesOperands();
            size_t place = std::ranges::find(values, value) - values.begin();
            if (place == values.size() || place >= made.fields().size() || !depth)
                break;
            const Layout& layout = m_program.at(made.layout());
            uint32_t field = made.fields()[place];
            if (layout.kind == LayoutKind::Environment)
                return { Holder::Kind::EnvironmentField, depth, static_cast<uint64_t>(made.layout().index()) << 20 | field };
            if (field < layout.fields.size() && layout.fields[field].key)
                return { Holder::Kind::Key, depth, layout.fields[field].key.bits() };
            break;
        }
        default:
            break;
        }
        return { };
    }

    bool isFilledByConstructor()
    {
        FunctionID constructor = m_program.at(m_layout).constructor;
        const BodyIndex* body = constructor ? bodyOf(constructor) : nullptr;
        auto found = body ? m_fills.find(constructor.index()) : m_fills.end();
        if (found == m_fills.end())
            return false;
        std::optional<unsigned> own = m_program.at(constructor).thisParameterIndex();
        return std::ranges::any_of(found->value, [&](const Fill& fill) {
            if (fill.holder.kind != Holder::Kind::Value || fill.holder.depth || fill.bound || !m_writer.runsOnEveryPath(constructor, *body, fill.block))
                return false;
            ValueID object(static_cast<uint32_t>(fill.holder.id));
            if (body->isParameter(object))
                return m_program.at(constructor).kind == FunctionKind::Plain && isEntryParameter(*body, object) && own == object.index();
            return m_program.at(constructor).kind == FunctionKind::BaseConstructor && body->opcodeOf(object) == Opcode::NewObject && body->value(object).as<Opcode::NewObject>().layout() == m_layout;
        });
    }

    void appendFills(FunctionID function, const BodyIndex& body, const Holder& holder, Vector<Event, 8>& events) const
    {
        auto found = m_fills.find(function.index());
        if (found == m_fills.end())
            return;
        for (const Fill& fill : found->value) {
            bool isCounted = fill.holder.kind == Holder::Kind::CountedKey;
            bool isOfHolder = fill.holder == holder || (holder.kind == Holder::Kind::CountedKey && fill.holder == Holder { Holder::Kind::Key, holder.depth, holder.id });
            if (!isOfHolder || (isCounted && fill.site && function != m_allocatingFunction))
                continue;
            if (fill.bound && (function != m_allocatingFunction || fill.holder.depth != 1 || !isAllocatedUpToInLinkLayouts(body, m_allocatedObject, fill.bound)))
                continue;
            events.append({ fill.block, fill.site, !fill.site, false });
        }
    }

    bool passesOnEveryPath(const BodyIndex& body, BlockID start, ValueID after, BlockID definition, std::span<const Event> events, BitVector* filledBlocks)
    {
        if (events.empty())
            return false;
        BitVector passed;
        for (const Event& event : events) {
            BlockID block = event.block;
            LoopID loop = body.loopOf(block);
            if (event.isLoop) {
                if (loop && body.isInLoop(start, loop))
                    continue;
                loop = loop ? body.loop(loop).parent : LoopID();
            } else if (block == start && (event.isAtEnd || !after || event.site > after))
                return true;
            for (; loop && !body.isInLoop(start, loop) && dominatesNextIterations(body, block, loop); loop = body.loop(loop).parent)
                block = body.loop(loop).head;
            if (block != start || event.isLoop)
                passed.set(block.index());
        }
        BitVector seen;
        Vector<BlockID, 16> toVisit { start };
        for (bool isStart = true; !toVisit.isEmpty(); isStart = false) {
            BlockID block = toVisit.takeLast();
            if (!isStart) {
                if (block == definition)
                    return false;
                if (passed.get(block.index()) || (filledBlocks && filledBlocks->get(block.index())) || !seen.add(block.index()))
                    continue;
            } else if (passed.get(block.index()) && !after)
                return true;
            BodyTerminator end = body.terminator(block);
            if (end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall)
                return false;
            for (BodyTarget target : end.targets())
                toVisit.append(target.block());
        }
        if (filledBlocks)
            filledBlocks->merge(seen);
        return true;
    }

    BitVector* filledBlocksOf(FunctionID function, const BodyIndex& body, const Holder& holder, BlockID start, std::span<const Event> events)
    {
        LoopID loop = body.loopOf(start);
        LoopID outermost = loop ? body.loop(loop).outermost : LoopID();
        bool hasEvents = outermost && std::ranges::any_of(events, [&](const Event& event) { return body.isInLoop(event.block, outermost); });
        uint64_t word = static_cast<uint64_t>(function.index() + 1) << 32 | (hasEvents ? loop.index() + 1 : 0);
        return &m_filledBlocks.add(WordPair { word, holder.bits() }, BitVector { }).iterator->value;
    }

    template<typename Functor>
    bool answer(FunctionID function, ValueID after, uint64_t bits, const Functor& functor)
    {
        WordPair question { static_cast<uint64_t>(function.index() + 1) << 32 | (after ? after.index() + 1 : 0), bits };
        if (auto added = m_answers.add(question, false); !added.isNewEntry)
            return added.iterator->value;
        if (m_functionsFollowed == mostFunctionsFollowed)
            return false;
        ++m_functionsFollowed;
        bool isTrue = functor();
        --m_functionsFollowed;
        if (isTrue)
            m_answers.set(question, true);
        return isTrue;
    }

    bool isFilled(FunctionID function, ValueID value, uint8_t depth, ValueID after)
    {
        const BodyIndex* body = bodyOf(function);
        if (!body || !value || depth > mostArraysAround)
            return false;
        Holder holder = holderOf(*body, value, depth);
        if (holder.kind == Holder::Kind::Key)
            holder.kind = Holder::Kind::CountedKey;
        if (holder.kind != Holder::Kind::Value)
            return isFilledUnderName(function, holder, after);
        value = ValueID(static_cast<uint32_t>(holder.id));
        return answer(function, after, holder.bits(), [&] {
            Holding& holding = holdingOf(function, *body, value, depth);
            Vector<Event, 8> events;
            for (ValueID alias : holding.aliases)
                appendFills(function, *body, { Holder::Kind::Value, depth, alias.index() }, events);
            for (const Holder& name : holding.names)
                appendFills(function, *body, name, events);
            bool outlivesFunction = !holding.names.isEmpty() || (depth && isEntryParameter(*body, value));
            BlockID start = after ? body->blockOf(after) : entryBlock;
            BlockID definition = outlivesFunction || isEntryParameter(*body, value) ? BlockID() : body->blockOf(value);
            auto passesFill = [&] {
                return passesOnEveryPath(*body, start, after, definition, events.span(), depth ? filledBlocksOf(function, *body, holder, start, events.span()) : nullptr);
            };
            if (passesFill())
                return true;
            findEvents(function, *body, depth, holding);
            events.appendVector(holding.events);
            if (!holding.events.isEmpty() && passesFill())
                return true;
            if (std::ranges::any_of(holding.names, [&](const Holder& name) { return isFilledByCallers(function, name); }))
                return true;
            if (depth && isEntryParameter(*body, value))
                return isArgumentFilledByCallers(function, value.index(), depth);
            if (depth && !body->isParameter(value) && body->opcodeOf(value) == Opcode::ElemGet)
                return isFilled(function, body->value(value).as<Opcode::ElemGet>().objectOperand(), depth + 1, after);
            return false;
        });
    }

    Holding& holdingOf(FunctionID function, const BodyIndex& body, ValueID value, uint8_t depth)
    {
        auto added = m_holdings.add(WordPair { static_cast<uint64_t>(function.index() + 1) << 32 | value.index(), depth }, nullptr);
        if (!added.isNewEntry)
            return *added.iterator->value;
        added.iterator->value = makeUnique<Holding>();
        Holding& holding = *added.iterator->value;
        auto addAlias = [&](ValueID alias) {
            if (!holding.aliases.contains(alias))
                holding.aliases.append(alias);
        };
        addAlias(value);
        for (size_t next = 0; next < holding.aliases.size(); ++next) {
            ValueID alias = holding.aliases[next];
            for (Use use : usesOf(function, alias)) {
                if (use.kind == Use::Kind::Edge || (use.kind == Use::Kind::Operand && returnsList(body.value(ValueID(use.user)), alias, depth)))
                    addAlias(ValueID(use.user));
                else if (Holder name = use.kind == Use::Kind::Operand ? findStoredName(body.value(ValueID(use.user)), alias, depth) : Holder(); name.kind == Holder::Kind::Value)
                    holding.otherUses.append({ alias, use });
                else if (!holding.names.contains(name))
                    holding.names.append(name);
            }
        }
        return holding;
    }

    bool returnsList(const BodyValue& user, ValueID value, uint8_t depth) const
    {
        if (inputPassThroughOpcodes.contains(user.opcode()))
            return true;
        if (!depth || user.opcode() != Opcode::Call || user.as<Opcode::Call>().thisValueOperand() != value)
            return false;
        PropertyKeyID key = user.as<Opcode::Call>().calleeKey();
        return key && (key == m_slice || key == m_sort || key == m_reverse || key == m_concat);
    }

    void findEvents(FunctionID function, const BodyIndex& body, uint8_t depth, Holding& holding)
    {
        if (std::exchange(holding.hasEvents, true))
            return;
        for (auto [value, use] : holding.otherUses)
            visitUse(function, body, value, depth, use, holding.events);
    }

    void visitUse(FunctionID function, const BodyIndex& body, ValueID value, uint8_t depth, Use use, Vector<Event, 8>& events)
    {
        if (use.kind == Use::Kind::Return) {
            if (isResultFilled(function, depth))
                events.append({ BlockID(use.user), { }, false, true });
            return;
        }
        ValueID site(use.user);
        BodyValue user = body.value(site);
        auto noteList = [&](ValueID list) {
            if (isFilled(function, list, depth + 1, site))
                events.append({ body.blockOf(site), site });
        };
        auto noteArgument = [&] {
            if (passesToHolder(function, body, site, value, depth))
                events.append({ body.blockOf(site), site });
        };
        switch (user.opcode()) {
        case Opcode::NewArray:
            return noteList(site);
        case Opcode::ElemSet:
            if (user.as<Opcode::ElemSet>().valueOperand() == value)
                noteList(user.as<Opcode::ElemSet>().objectOperand());
            return;
        case Opcode::Call: {
            auto call = user.as<Opcode::Call>();
            PropertyKeyID key = call.calleeKey();
            std::span<const ValueID> arguments = call.argumentsOperands();
            bool storesArgument = key && call.thisValueOperand() && call.thisValueOperand() != value && calleesAt(function, site).isEmpty();
            if (storesArgument && (key == m_push || key == m_unshift || key == m_add))
                return noteList(call.thisValueOperand());
            if (storesArgument && key == m_set && arguments.size() == 2 && arguments[1] == value)
                return noteList(call.thisValueOperand());
            return noteArgument();
        }
        case Opcode::Construct:
        case Opcode::CallDirect:
            return noteArgument();
        default:
            return;
        }
    }

    LinkFlow::CalleesOfCall::List<FunctionID> calleesAt(FunctionID function, ValueID site)
    {
        std::optional<uint32_t> number = callAt(function, site);
        return number ? calleesOf(function, *number) : LinkFlow::CalleesOfCall::List<FunctionID>(std::span<const FunctionID> { });
    }

    bool passesToHolder(FunctionID caller, const BodyIndex& body, ValueID site, ValueID value, uint8_t depth)
    {
        auto callees = calleesAt(caller, site);
        return callees.size() && std::ranges::all_of(callees, [&](FunctionID callee) {
            const Function& entered = m_program.at(callee);
            const BodyIndex* enteredBody = bodyOf(callee);
            if (!enteredBody)
                return false;
            uint32_t numberOfParameters = enteredBody->block(entryBlock).numberOfParameters();
            for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
                if (argumentAt(body, entered, site, parameter) == value && isFilled(callee, ValueID(parameter), depth, { }))
                    return true;
            }
            return false;
        });
    }

    bool isFilledUnderName(FunctionID function, const Holder& name, ValueID after)
    {
        const BodyIndex* body = bodyOf(function);
        if (!body)
            return false;
        return answer(function, after, name.bits(), [&] {
            Vector<Event, 8> events;
            appendFills(function, *body, name, events);
            BlockID start = after ? body->blockOf(after) : entryBlock;
            return passesOnEveryPath(*body, start, after, { }, events.span(), filledBlocksOf(function, *body, name, start, events.span())) || isFilledByCallers(function, name);
        });
    }

    template<typename Functor>
    bool everyCallerFills(FunctionID function, const Functor& functor)
    {
        if (m_firstCallers.isEmpty())
            listCallers();
        if (!m_writer.everyCallerIsKnown(function) || m_program.at(function).isCoroutine() || m_calleesOfSharedCalls.get(function.index()))
            return false;
        bool isCalled = false;
        bool isFilledByAll = true;
        forEachCaller(function, [&](FunctionID caller, uint32_t number) {
            if (!isFilledByAll)
                return;
            isCalled = true;
            const SummaryCall& call = m_flow.summaries.at(caller).calls[number];
            isFilledByAll = call.site && !call.isMadeByProtocol && !call.isMadeByHost && functor(caller, call.site);
        });
        return isCalled && isFilledByAll;
    }

    static constexpr uint64_t bitsOfCallers = 1ull << 57;

    bool isFilledByCallers(FunctionID function, const Holder& name)
    {
        return answer(function, { }, name.bits() | bitsOfCallers, [&] {
            return everyCallerFills(function, [&](FunctionID caller, ValueID site) {
                return isFilledUnderName(caller, name, site);
            });
        });
    }

    bool isArgumentFilledByCallers(FunctionID function, uint32_t parameter, uint8_t depth)
    {
        const Function& callee = m_program.at(function);
        bool isCapture = parameter < callee.captures.size();
        FunctionID maker = isCapture ? m_flow.findOnlyReferrer(function) : FunctionID();
        return everyCallerFills(function, [&](FunctionID caller, ValueID site) {
            const BodyIndex* body = bodyOf(caller);
            ValueID argument = body ? argumentAt(*body, callee, site, parameter) : ValueID();
            if (!argument && isCapture && caller == maker)
                argument = findCapturedValue(maker, function, parameter);
            return argument && isFilled(caller, argument, depth, site);
        });
    }

    bool isResultFilled(FunctionID function, uint8_t depth)
    {
        return answer(function, { }, static_cast<uint64_t>(depth) << 58 | bitsOfCallers, [&] {
            if (m_program.at(function).isCoroutine())
                return false;
            if (auto [caller, site] = findCallTakingClosure(function); caller) {
                const BodyIndex* body = bodyOf(caller);
                if (body && listsResultsOfCallback(*body, body->value(site).as<Opcode::Call>().calleeOperand()))
                    return isFilled(caller, site, depth + 1, site);
            }
            return everyCallerFills(function, [&](FunctionID caller, ValueID site) {
                return isFilled(caller, site, depth, site);
            });
        });
    }

    const WriterOfLinkLayouts& m_writer;
    const LinkFlow& m_flow;
    const Program& m_program;
    const BodyIndexTable& m_bodies;
    const PropertyKeyID m_push;
    const PropertyKeyID m_unshift;
    const PropertyKeyID m_add;
    const PropertyKeyID m_set;
    const PropertyKeyID m_forEach;
    const PropertyKeyID m_map;
    const PropertyKeyID m_slice;
    const PropertyKeyID m_sort;
    const PropertyKeyID m_reverse;
    const PropertyKeyID m_concat;
    const BuiltinID m_arrayFromLength;
    const BuiltinID m_arrayFromList;
    Vector<uint32_t> m_firstCallers;
    Vector<uint32_t> m_callers;
    BitVector m_calleesOfSharedCalls;
    HashMap<uint32_t, Vector<std::pair<FunctionID, ValueID>>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_closuresOfMakers;
    HashMap<uint32_t, std::unique_ptr<FunctionUses>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_usesOfFunctions;

    LayoutID m_layout;
    HashMap<uint32_t, Vector<Fill, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fills;
    Vector<std::pair<FunctionID, Fill>> m_pending;
    HashSet<WordPair> m_parameterFills;
    HashSet<WordPair> m_namedFills;
    HashMap<WordPair, bool> m_answers;
    HashMap<WordPair, BitVector> m_filledBlocks;
    HashMap<WordPair, std::unique_ptr<Holding>> m_holdings;
    FunctionID m_allocatingFunction;
    ValueID m_allocatedObject;
    unsigned m_functionsFollowed { 0 };
};

void WriterOfLinkLayouts::findAllocatingFunctions(LateFillFinder& finder, const BitVector& layouts)
{
    Vector<Allocation, 2> allocations;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> literals;
    BitVector functions;
    for (const AllocatedLayout& allocated : m_allocatedLayouts) {
        if (layouts.get(allocated.layout.index()))
            functions.set(allocated.function.index());
    }
    for (size_t index : layouts) {
        if (FunctionID constructor = m_program.at(LayoutID(index)).constructor) {
            finder.forEachCaller(constructor, [&](FunctionID caller, uint32_t) {
                functions.set(caller.index());
            });
        }
    }
    for (size_t index : functions) {
        FunctionID id(index);
        const ThinSummary& summary = m_flow.summaries.at(id);
        allocations.shrink(0);
        literals.clear();
        for (uint32_t number = 0; number < summary.nodes.size(); ++number) {
            const SummaryNode& node = summary.nodes[number];
            auto append = [&](LayoutID layout, ValueID object) {
                if (layouts.get(layout.index()))
                    allocations.append({ layout, object });
            };
            if (node.kind == SummaryNode::Kind::ResultOfCall && summary.calls[node.first].isConstruction) {
                const LinkType& type = typeAt(id, number);
                if (type.objects == LinkType::Objects::Layout)
                    append(type.layout, summary.calls[node.first].site);
                else if (type.objects == LinkType::Objects::Set) {
                    m_flow.forEachLayoutOf(type, [&](LayoutID layout) {
                        append(layout, { });
                    });
                }
            } else if (node.kind == SummaryNode::Kind::Allocation && layouts.get(node.first) && m_program.at(LayoutID(node.first)).constructor != id)
                literals.add(node.first, 0u);
        }
        const BodyIndex* body = bodiesLookedAt(m_flow.looks).find(id);
        for (uint32_t block = 0; body && !literals.isEmpty() && block < body->numberOfBlocks(); ++block) {
            for (BodyValue value : body->block(BlockID(block)).values()) {
                if (value.opcode() != Opcode::NewObject)
                    continue;
                LayoutID layout = value.as<Opcode::NewObject>().layout();
                if (auto found = literals.find(layout.index()); found != literals.end()) {
                    ++found->value;
                    allocations.append({ layout, value.id() });
                }
            }
        }
        for (auto& literal : literals) {
            if (!literal.value)
                allocations.append({ LayoutID(literal.key), { } });
        }
        if (!allocations.isEmpty())
            m_allocatingFunctions.append({ id, WTF::move(allocations) });
    }
}

void WriterOfLinkLayouts::findLateFieldsFilledTogether()
{
    findInlineCapacities();
    if (m_lateFills == LateFills::FoundByOtherReading)
        return;
    m_lateFieldsOfMethods.removeAllMatching([&](const LateFieldOfMethod& field) {
        return !std::ranges::contains(m_tables.appendedKeysOf(field.layout), field.key);
    });
    m_lateFieldsFilled.removeAllMatching([&](const LateFieldsFilled& filled) {
        Vector<PropertyKeyID, 16> keys;
        for (auto [key, site] : filled.stores) {
            if (!keys.contains(key))
                keys.append(key);
        }
        return !fillsMostLateFields(filled.layout, keys.span());
    });
    BitVector layouts;
    for (const LateFieldsFilled& filled : m_lateFieldsFilled)
        layouts.set(filled.layout.index());
    for (const LateFieldOfMethod& field : m_lateFieldsOfMethods)
        layouts.set(field.layout.index());
    if (layouts.isEmpty())
        return;
    LateFillFinder finder(*this);
    findAllocatingFunctions(finder, layouts);
    noteLateFieldsOfMethods();
    Vector<uint64_t>& storedThroughThis = m_tables.lateFieldsStoredThroughThis;
    std::ranges::sort(storedThroughThis);
    storedThroughThis.shrink(storedThroughThis.size() - std::ranges::unique(storedThroughThis).size());
    AOT::stableSort(m_lateFieldsFilled, { }, [](const LateFieldsFilled& filled) {
        return filled.layout.index();
    });
    Vector<LateFillAllocation> allocations;
    for (const AllocatingFunction& allocating : m_allocatingFunctions) {
        for (const Allocation& allocation : allocating.allocations)
            allocations.append({ allocation.layout, allocating.function, allocation.object });
    }
    AOT::stableSort(allocations, { }, [](const LateFillAllocation& allocation) {
        return allocation.layout.index();
    });
    std::span<const LateFieldsFilled> stores = m_lateFieldsFilled.span();
    for (size_t first = 0, end = 0; first < stores.size(); first = end) {
        LayoutID layout = stores[first].layout;
        while (end < stores.size() && stores[end].layout == layout)
            ++end;
        auto allocationsOfLayout = std::ranges::equal_range(allocations, layout.index(), { }, [](const LateFillAllocation& allocation) { return allocation.layout.index(); });
        if (finder.areObjectsFilled(layout, stores.subspan(first, end - first), allocationsOfLayout))
            m_tables.layoutsFillingLateFieldsTogether.set(layout.index());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
