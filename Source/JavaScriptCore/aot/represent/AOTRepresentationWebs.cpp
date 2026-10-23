/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationWebs.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t iterationsPerLoop = static_cast<uint64_t>(frequencyConstants.iterationsPerLoop);
static constexpr unsigned maxCountedLoopDepth = 6;

static constexpr uint64_t votesOutsideLoops(BlockFrequency frequency)
{
    switch (frequency) {
    case BlockFrequency::Normal:
        return 64;
    case BlockFrequency::Unlikely:
    case BlockFrequency::Rare:
        return 1;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// A conversion costs where it runs. One in a place that is seldom entered decides only between choices that are level beside it, and one in a loop outweighs those around the loop.
static constexpr uint64_t votesAt(BlockFrequency frequency, unsigned loopDepth)
{
    uint64_t votes = votesOutsideLoops(frequency);
    for (unsigned level = std::min(loopDepth, maxCountedLoopDepth); level--;)
        votes *= iterationsPerLoop;
    return votes;
}

static_assert(iterationsPerLoop > 1);
static_assert(votesAt(BlockFrequency::Rare, 0), "no votes stands for a block that was not asked about");
static_assert(votesAt(BlockFrequency::Normal, maxCountedLoopDepth + 1) == votesAt(BlockFrequency::Normal, maxCountedLoopDepth));
static_assert(votesAt(BlockFrequency::Normal, maxCountedLoopDepth) <= std::numeric_limits<uint64_t>::max() >> 33, "every producer and every use in a function can vote in one web");

static constexpr RepresentationSet webRepresentations {
    Representation::Void,
    Representation::Boolean,
    Representation::Int32,
    Representation::Int64,
    Representation::Double,
    Representation::OptionalInt64,
    Representation::OptionalDouble,
    Representation::Cell,
    Representation::JSValue,
    Representation::LiteralString,
    Representation::RawWord,
};

static constexpr RepresentationSet representationsWithRegister = [] {
    RepresentationSet result;
    for (Representation representation : allMembersOf<Representation>) {
        if (hasRegister(representation))
            result.add(representation);
    }
    return result;
}();

// A cast is a copy of the operand it hands on. An operand it only looks at stays as it is.
template<typename Functor>
static void forEachForwardedOperand(const Value& value, const Functor& functor)
{
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (isForwardedOperand(value, operandIndex))
            functor(operand);
    });
}

RepresentationWebs::RepresentationWebs(const RepresentationInputs& inputs, FunctionAnalyses& analyses)
    : m_inputs(inputs)
    , m_analyses(analyses)
    , m_parent(inputs.function.values.size(), [](size_t index) { return static_cast<uint32_t>(index); })
    , m_webOfRoot(inputs.function.values.size())
    , m_fixed(FillWith { }, inputs.function.values.size(), Representation::Unchosen)
{
    ASSERT(&analyses.function() == &inputs.function);
    const UseLists& useLists = analyses.useLists();
    for (uint32_t index = 0; index < m_fixed.size(); ++index) {
        ValueID id(index);
        if (useLists.locationOf(id).placement == ValuePlacement::Detached)
            continue;
        const Value& value = m_inputs.function.at(id);
        m_fixed[index] = constantsInFormOfUses.contains(value.opcode) ? defaultRepresentationOf(m_inputs.typeOf(id)) : resultRepresentationOf(m_inputs, value);
    }
}

void RepresentationWebs::build()
{
    uniteCopies();
    numberWebs();
    restrictToLegalRepresentations();
    collectProducers();
    collectUses();
    admitNarrowReferences();
}

// A web votes in the choices of its neighbors with the form it has at the time: at first the default of its type.
void RepresentationWebs::choose()
{
    constexpr unsigned maxRounds = 8;
    Vector<uint32_t> firstLinkOf(FillWith { }, m_webs.size() + 1, 0u);
    for (const Link& link : m_links)
        ++firstLinkOf[link.voter + 1];
    for (size_t index = 1; index < firstLinkOf.size(); ++index)
        firstLinkOf[index] += firstLinkOf[index - 1];
    Vector<Link> linksByVoter(FillWith { }, m_links.size(), Link { });
    {
        Vector<uint32_t> next = firstLinkOf;
        for (const Link& link : m_links)
            linksByVoter[next[link.voter]++] = link;
    }

    auto linksOf = [&](uint32_t voter) {
        return linksByVoter.span().subspan(firstLinkOf[voter], firstLinkOf[voter + 1] - firstLinkOf[voter]);
    };
    auto move = [&](uint32_t index, Representation chosen) {
        Representation before = std::exchange(m_webs[index].preferred, chosen);
        for (const Link& link : linksOf(index)) {
            Votes& votes = link.isProducer ? m_webs[link.web].produced : m_webs[link.web].wanted;
            if (link.isProducer || hasRegister(before))
                votes[std::to_underlying(before)] -= link.votes;
            if (link.isProducer || hasRegister(chosen))
                votes[std::to_underlying(chosen)] += link.votes;
        }
    };
    auto votesBetween = [&](uint32_t voter, uint32_t web, bool isProducer) {
        uint64_t votes = 0;
        for (const Link& link : linksOf(voter)) {
            if (link.web == web && link.isProducer == isProducer)
                votes += link.votes;
        }
        return votes;
    };
    // Each of two webs, one of which is copied into the other, can be held where it is by the other alone.
    auto moveTogether = [&](uint32_t source, uint32_t destination) {
        Web& from = m_webs[source];
        Web& to = m_webs[destination];
        uint64_t votesAsProducer = votesBetween(source, destination, true);
        uint64_t votesAsUse = votesBetween(destination, source, false);
        // Where the two differ today, both count the one conversion between them.
        uint64_t fewest = conversionsNeededBy(from, from.preferred) + conversionsNeededBy(to, to.preferred);
        if (from.preferred != to.preferred && hasRegister(to.preferred))
            fewest -= votesAsUse;
        std::optional<Representation> best;
        for (Representation candidate : allMembersOf<Representation>) {
            if (!hasRegister(candidate) || !from.legal.contains(candidate) || !to.legal.contains(candidate) || candidate == from.preferred || candidate == to.preferred)
                continue;
            uint64_t needed = conversionsNeededBy(from, candidate) + conversionsNeededBy(to, candidate) - votesAsProducer - (hasRegister(to.preferred) ? votesAsUse : 0);
            if (needed < fewest) {
                best = candidate;
                fewest = needed;
            }
        }
        if (!best)
            return false;
        move(source, *best);
        move(destination, *best);
        return true;
    };

    // A web without a choice stays as it is, and it votes nowhere by a link.
    Vector<uint32_t, 32> websWithChoice;
    for (uint32_t index = 0; index < m_webs.size(); ++index) {
        if (m_webs[index].hasChoice())
            websWithChoice.append(index);
    }

    for (unsigned round = 0; round < maxRounds; ++round) {
        bool changed = false;
        for (uint32_t index : websWithChoice) {
            Representation chosen = chooseFor(m_webs[index]);
            if (chosen == m_webs[index].preferred)
                continue;
            move(index, chosen);
            changed = true;
        }
        if (changed)
            continue;
        for (uint32_t index : websWithChoice) {
            uint32_t previous = UINT32_MAX;
            for (const Link& link : linksOf(index)) {
                if (!link.isProducer || link.web == previous || !m_webs[link.web].hasChoice())
                    continue;
                previous = link.web;
                if (moveTogether(index, link.web)) {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed)
            break;
    }
    for (Web& web : m_webs)
        web.chosen = web.preferred;
    chooseForConstants();
}

Representation RepresentationWebs::representationOf(ValueID id) const
{
    std::optional<uint32_t> web = m_webOfRoot[find(id.index())];
    return web ? m_webs[*web].chosen : m_fixed[id.index()];
}

uint32_t RepresentationWebs::find(uint32_t valueIndex) const
{
    while (m_parent[valueIndex] != valueIndex) {
        m_parent[valueIndex] = m_parent[m_parent[valueIndex]];
        valueIndex = m_parent[valueIndex];
    }
    return valueIndex;
}

void RepresentationWebs::unite(ValueID a, ValueID b)
{
    uint32_t rootOfA = find(a.index());
    uint32_t rootOfB = find(b.index());
    auto [smaller, larger] = std::minmax(rootOfA, rootOfB);
    m_parent[larger] = smaller;
}

void RepresentationWebs::uniteCopies()
{
    const Function& function = m_inputs.function;
    for (BlockID covered : m_analyses.cfg().reversePostOrder()) {
        const std::optional<Target>& handler = function.at(covered).handler;
        if (!handler)
            continue;
        const CompactList<ValueID>& parameters = function.at(handler->block).parameters;
        for (uint32_t index = 0; index < handler->arguments.size(); ++index) {
            ValueID argument = handler->arguments[index];
            if (m_fixed[argument.index()] == Representation::Unchosen)
                unite(parameters[index], argument);
        }
    }
}

void RepresentationWebs::numberWebs()
{
    const UseLists& useLists = m_analyses.useLists();
    for (uint32_t index = 0; index < m_fixed.size(); ++index) {
        // The root of a web is the member with the lowest number.
        uint32_t root = find(index);
        bool picks = root == index && m_fixed[index] == Representation::Unchosen && useLists.locationOf(ValueID(index)).placement != ValuePlacement::Detached;
        if (picks) {
            m_webOfRoot[index] = static_cast<uint32_t>(m_webs.size());
            m_webs.append(Web { });
        }
        if (std::optional<uint32_t> web = m_webOfRoot[root])
            m_members.append(Member { ValueID(index), *web });
    }
}

void RepresentationWebs::restrictToLegalRepresentations()
{
    const Function& function = m_inputs.function;
    auto forEachMember = [&](const auto& functor) {
        for (Member member : m_members)
            functor(member.value, m_webs[member.web]);
    };
    // Neighbors have few types between them.
    struct LegalRepresentationsOfType {
        TypeID type;
        RepresentationSet representations;
    };
    std::array<LegalRepresentationsOfType, 16> remembered { };
    auto legalRepresentationsOfValue = [&](ValueID id) {
        TypeID type = function.at(id).type;
        LegalRepresentationsOfType& entry = remembered[type.index() % remembered.size()];
        if (entry.type != type || !type)
            entry = { type, legalRepresentationsOf(m_inputs.typeOf(id)) };
        return entry.representations;
    };
    auto representationsConvertibleFrom = [&](ValueID operand) {
        RepresentationSet representations = legalRepresentationsOfValue(operand);
        representations &= representationsWithRegister;
        if (!hasRegister(m_fixed[operand.index()]))
            representations.add(m_fixed[operand.index()]);
        return representations;
    };

    for (Web& web : m_webs)
        web.legal = webRepresentations | RepresentationSet { Representation::Cell32 };

    forEachMember([&](ValueID member, Web& web) {
        web.legal &= legalRepresentationsOfValue(member);
        forEachForwardedOperand(function.at(member), [&](ValueID operand) {
            web.legal &= representationsConvertibleFrom(operand);
        });
    });

    // Types that fit do not make a narrow reference legal. admitNarrowReferences() asks the rest, of the webs that have a use for one.
    for (Web& web : m_webs) {
        web.fitsNarrowReference = web.legal.contains(Representation::Cell32);
        web.legal &= webRepresentations;
    }

    forEachMember([&](ValueID member, Web& web) {
        auto tryDefaultOf = [&](ValueID id) {
            if (web.preferred != Representation::Unchosen)
                return;
            Representation representation = defaultRepresentationOf(m_inputs.typeOf(id));
            if (web.legal.contains(representation))
                web.preferred = representation;
        };
        tryDefaultOf(member);
        forEachForwardedOperand(function.at(member), tryDefaultOf);
    });

    for (Web& web : m_webs) {
        if (web.preferred == Representation::Unchosen)
            web.preferred = Representation::JSValue;
        RELEASE_ASSERT(web.legal.contains(web.preferred));
    }
}

// No type has a narrow reference for its default, so a web is preferred in another form and leaves it only for one that gets more votes.
void RepresentationWebs::admitNarrowReferences()
{
    constexpr size_t narrowReference = std::to_underlying(Representation::Cell32);
    BitVector websWithVotes(m_webs.size());
    for (uint32_t index = 0; index < m_webs.size(); ++index) {
        const Web& web = m_webs[index];
        if (web.fitsNarrowReference && web.produced[narrowReference] + web.wanted[narrowReference])
            websWithVotes.quickSet(index);
    }
    if (websWithVotes.isEmpty())
        return;

    findFirstCollectingValues();
    BitVector narrowWebs = findNarrowWebsAmong(websWithVotes);
    if (m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]] {
        BitVector websThatFit(m_webs.size());
        for (uint32_t index = 0; index < m_webs.size(); ++index)
            websThatFit.quickSet(index, m_webs[index].fitsNarrowReference);
        BitVector narrowWebsAmongAll = findNarrowWebsAmong(websThatFit);
        narrowWebsAmongAll.filter(websWithVotes);
        RELEASE_ASSERT(narrowWebsAmongAll == narrowWebs);
    }
    narrowWebs.forEachSetBit([&](size_t index) {
        m_webs[index].legal.add(Representation::Cell32);
    });
}

// The types of the given webs fit a narrow reference. What is said of one of them does not hang on which others are asked about.
BitVector RepresentationWebs::findNarrowWebsAmong(const BitVector& webs)
{
    BitVector narrowWebs = findWebsThatStayNarrow(webs);
    if (!narrowWebs.isEmpty())
        excludeWebsThatMayLieInFrames(narrowWebs);
    return narrowWebs;
}

BitVector RepresentationWebs::findWebsThatStayNarrow(const BitVector& webs)
{
    findWhereCollectorMayHaveRun(webs);
    BitVector narrowWebs = webs;
    for (Member member : m_members) {
        if (narrowWebs.quickGet(member.web) && !mayStayNarrow(member.value))
            narrowWebs.quickClear(member.web);
    }
    return narrowWebs;
}

void RepresentationWebs::excludeWebsThatMayLieInFrames(BitVector& narrowWebs)
{
    BitVector mayLieInFrame = findValuesThatMayLieInFrames();
    if (mayLieInFrame.isEmpty())
        return;
    for (Member member : m_members) {
        if (mayLieInFrame.quickGet(member.value.index()))
            narrowWebs.quickClear(member.web);
    }
}

// A narrow reference counts from the base of the cage of cells, and a cell of a frame lies outside of the cage.
BitVector RepresentationWebs::findValuesThatMayLieInFrames() const
{
    const Function& function = m_inputs.function;
    BitVector values(function.values.size());
    for (ValueID parameter : function.at(entryBlock).parameters) {
        if (mayBeReference(m_inputs.typeOf(parameter)))
            values.quickSet(parameter.index());
    }
    std::span<const BlockID> blocks = m_analyses.cfg().reversePostOrder();
    for (BlockID block : blocks) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            bool readsWideSlot = readsFollowedByFramePointers.contains(value.opcode) && m_fixed[id.index()] != Representation::Cell32 && mayBeReference(m_inputs.typeOf(id));
            if (hasFrameStorage(value) || readsWideSlot)
                values.quickSet(id.index());
        }
    }
    if (values.isEmpty())
        return values;

    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : blocks) {
            for (ValueID id : function.at(block).values) {
                forEachReturnedOperand(function, function.at(id), [&](ValueID operand) {
                    if (values.quickGet(operand.index()))
                        changed |= !values.quickSet(id.index());
                });
            }
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                const CompactList<ValueID>& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index) {
                    ValueID passed = valuePassedOn(function, edge, index);
                    if (passed && values.quickGet(passed.index()))
                        changed |= !values.quickSet(parameters[index].index());
                }
            });
        }
    }
    return values;
}

void RepresentationWebs::findFirstCollectingValues()
{
    const Function& function = m_inputs.function;
    m_firstCollectingValue.fill(0, function.blocks.size());
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const auto& values = function.at(block).values;
        auto first = std::ranges::find_if(values, [&](ValueID id) {
            return mayCollect(function.at(id).opcode);
        });
        m_firstCollectingValue[block.index()] = static_cast<uint32_t>(first - values.begin());
    }
}

// For every block with a parameter in one of the webs: at the head of which blocks the collector may have run since that block was entered last.
void RepresentationWebs::findWhereCollectorMayHaveRun(const BitVector& webs)
{
    const Function& function = m_inputs.function;
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();

    uint32_t numberOfBlocksWithReference = 0;
    m_indexOfBlockWithReference.fill(std::nullopt, function.blocks.size());
    for (Member member : m_members) {
        if (!webs.quickGet(member.web))
            continue;
        ValueLocation location = useLists.locationOf(member.value);
        if (location.placement != ValuePlacement::Parameter)
            continue;
        std::optional<uint32_t>& indexOfBlock = m_indexOfBlockWithReference[location.block.index()];
        if (!indexOfBlock)
            indexOfBlock = numberOfBlocksWithReference++;
    }

    m_mayHaveCollectedSinceEntryOf.fill(BitVector(), function.blocks.size());
    if (!numberOfBlocksWithReference)
        return;
    BitVector everyBlock;
    for (uint32_t index = 0; index < numberOfBlocksWithReference; ++index)
        everyBlock.set(index);

    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : cfg.reversePostOrder()) {
            BitVector atHead = m_mayHaveCollectedSinceEntryOf[block.index()];
            for (Edge edge : cfg.incomingEdges(block)) {
                if (m_firstCollectingValue[edge.from.index()] < function.at(edge.from).values.size()) {
                    atHead = everyBlock;
                    break;
                }
                BitVector atEnd = m_mayHaveCollectedSinceEntryOf[edge.from.index()];
                if (std::optional<uint32_t> entered = m_indexOfBlockWithReference[edge.from.index()])
                    atEnd.clear(*entered);
                atHead.merge(atEnd);
            }
            if (atHead != m_mayHaveCollectedSinceEntryOf[block.index()]) {
                m_mayHaveCollectedSinceEntryOf[block.index()] = WTF::move(atHead);
                changed = true;
            }
        }
    }
}

bool RepresentationWebs::isReachedBeforeCollectorCanRun(const Use& use, BlockID definingBlock) const
{
    const Function& function = m_inputs.function;
    if (use.block != definingBlock && m_mayHaveCollectedSinceEntryOf[use.block.index()].get(*m_indexOfBlockWithReference[definingBlock.index()]))
        return false;

    uint32_t firstCollectingValue = m_firstCollectingValue[use.block.index()];
    bool runsNoCollector = firstCollectingValue == function.at(use.block).values.size();
    switch (use.kind) {
    case Use::Kind::Operand:
        // A test hands its operand on at the end of the block.
        if (provesOf(function.at(use.user).opcode) != Proof::None)
            return runsNoCollector;
        return m_analyses.useLists().locationOf(use.user).index <= firstCollectingValue;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return runsNoCollector;
    case Use::Kind::HandlerArgument:
        return false;
    case Use::Kind::BlockSubject:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A narrow reference lives from a load to its Decode.
bool RepresentationWebs::mayStayNarrow(ValueID member) const
{
    ValueLocation location = m_analyses.useLists().locationOf(member);
    switch (location.placement) {
    case ValuePlacement::Parameter: {
        bool isCaught = std::ranges::any_of(m_analyses.cfg().incomingEdges(location.block), [](Edge edge) {
            return edge.isHandlerEdge();
        });
        if (isCaught || !m_indexOfBlockWithReference[location.block.index()])
            return false;
        return std::ranges::all_of(m_analyses.useLists().usesOf(member), [&](const Use& use) {
            return isReachedBeforeCollectorCanRun(use, location.block);
        });
    }
    case ValuePlacement::InBlock:
        return isCast(m_inputs.function.at(member).opcode);
    case ValuePlacement::Detached:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RepresentationWebs::collectProducers()
{
    const Function& function = m_inputs.function;
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();

    auto addCopyOf = [&](ValueID source, Member member, uint64_t votes) {
        if (constantsInFormOfUses.contains(function.at(source).opcode))
            return;
        std::optional<uint32_t> web = m_webOfRoot[find(source.index())];
        if (web == member.web)
            return;
        if (web && m_webs[*web].hasChoice())
            m_links.append(Link { *web, member.web, votes, true });
        addProducer(m_webs[member.web], web ? m_webs[*web].preferred : m_fixed[source.index()], votes);
    };

    auto addIncomingValue = [&](Member parameter, const IncomingValue& incoming, Edge edge) {
        // What a handler takes is converted behind its definition.
        if (edge.isHandlerEdge()) {
            addCopyOf(incoming.argument, parameter, votesOf(useLists.blockOf(incoming.argument)));
            return;
        }

        uint64_t votes = votesOf(edge);
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            addCopyOf(incoming.argument, parameter, votesOfArgument(edge, incoming.argument));
            return;
        case IncomingValue::Kind::EdgeResult:
            break;
        }

        EdgeResult result = edgeResultAt(function, incoming.result);
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand:
            addCopyOf(result.subject, parameter, votes);
            return;
        case EdgeResultKind::Guard:
            addProducer(m_webs[parameter.web], Representation::Void, votes);
            return;
        case EdgeResultKind::DeclaredResult:
            addProducer(m_webs[parameter.web], edgeResultRepresentationOf(m_inputs, incoming.result), votes);
            return;
        }
    };

    for (Member member : m_members) {
        if (!m_webs[member.web].hasChoice())
            continue;
        ValueLocation location = useLists.locationOf(member.value);
        switch (location.placement) {
        case ValuePlacement::Parameter:
            for (Edge edge : cfg.incomingEdges(location.block))
                addIncomingValue(member, incomingValueOf(function, edge, location.index), edge);
            break;
        case ValuePlacement::InBlock:
            forEachForwardedOperand(function.at(member.value), [&](ValueID operand) {
                addCopyOf(operand, member, votesOf(location.block));
            });
            break;
        case ValuePlacement::Detached:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
}

void RepresentationWebs::collectUses()
{
    const Function& function = m_inputs.function;
    const UseLists& useLists = m_analyses.useLists();

    auto addCopyTo = [&](ValueID destination, Member member, uint64_t votes) {
        uint32_t web = *m_webOfRoot[find(destination.index())];
        if (web == member.web)
            return;
        if (m_webs[web].hasChoice())
            m_links.append(Link { web, member.web, votes, false });
        addUse(m_webs[member.web], m_webs[web].preferred, votes);
    };

    for (Member member : m_members) {
        Web& web = m_webs[member.web];
        if (!web.hasChoice())
            continue;
        for (const Use& use : useLists.usesOf(member.value)) {
            switch (use.kind) {
            case Use::Kind::Operand:
                if (isForwardedOperand(function.at(use.user), use.operandIndex))
                    addCopyTo(use.user, member, votesOf(use.block));
                else
                    addUse(web, wantedRepresentationOf(m_inputs, function.at(use.user), use.operandIndex), votesOf(use.block));
                break;
            case Use::Kind::TerminatorOperand:
                addUse(web, wantedRepresentationOf(m_inputs, function.at(use.block).terminator, use.operandIndex), votesOf(use.block));
                break;
            case Use::Kind::EdgeArgument:
                addCopyTo(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex], member, votesOfArgument(use.edge(), member.value));
                break;
            case Use::Kind::HandlerArgument:
                ASSERT(m_webOfRoot[find(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex].index())] == member.web);
                break;
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
}

uint64_t votesOfConversionIn(FunctionAnalyses& analyses, BlockID block)
{
    // Votes are only compared with each other, and all votes in a lone block are equal.
    const Function& function = analyses.function();
    if (function.blocks.size() == 1 && function.at(block).terminator.targets.isEmpty())
        return votesAt(BlockFrequency::Normal, 0);
    return votesAt(analyses.blockFrequencies().classOf(block), analyses.blockCycles().depthOf(block));
}

uint64_t RepresentationWebs::votesOf(BlockID block) const
{
    if (m_votesOfBlock.isEmpty())
        m_votesOfBlock.fill(0, m_inputs.function.blocks.size());
    uint64_t& votes = m_votesOfBlock[block.index()];
    if (!votes)
        votes = votesOfConversionIn(m_analyses, block);
    return votes;
}

// What an edge passes is converted on the edge: no more often than either end is entered, and in the loops that hold both ends.
uint64_t RepresentationWebs::votesOfArgument(Edge edge, ValueID argument) const
{
    uint64_t timesPassed = 0;
    for (ValueID passed : targetOf(m_inputs.function, edge).arguments)
        timesPassed += passed == argument;
    return std::max<uint64_t>(votesOf(edge) / std::max<uint64_t>(timesPassed, 1), 1);
}

uint64_t RepresentationWebs::votesOf(Edge edge) const
{
    ASSERT(!edge.isHandlerEdge());
    BlockID to = targetOf(m_inputs.function, edge).block;
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    const BlockCycles::Forest& cycles = m_analyses.blockCycles().forest();
    BlockFrequency frequency = rarerOf(frequencies.classOf(edge), rarerOf(frequencies.classOf(edge.from), frequencies.classOf(to)));
    return votesAt(frequency, cycles.depth(cycles.nearestCommon(cycles.innermost(edge.from), cycles.innermost(to))));
}

void RepresentationWebs::addProducer(Web& web, Representation representation, uint64_t votes)
{
    ASSERT(representation != Representation::Unchosen);
    web.produced[std::to_underlying(representation)] += votes;
}

void RepresentationWebs::addUse(Web& web, Representation representation, uint64_t votes)
{
    if (hasRegister(representation))
        web.wanted[std::to_underlying(representation)] += votes;
}

uint64_t RepresentationWebs::conversionsNeededBy(const Web& web, Representation representation)
{
    uint64_t allVotes = 0;
    for (size_t index = 0; index < web.produced.size(); ++index)
        allVotes += web.produced[index] + web.wanted[index];
    return allVotes - web.produced[std::to_underlying(representation)] - web.wanted[std::to_underlying(representation)];
}

// The form that needs the fewest conversions is the one that most votes are for: conversionsNeededBy() takes them off one sum.
Representation RepresentationWebs::chooseFor(const Web& web) const
{
    auto votesFor = [&](Representation representation) {
        return web.produced[std::to_underlying(representation)] + web.wanted[std::to_underlying(representation)];
    };
    Representation best = web.preferred;
    uint64_t most = votesFor(best);
    for (Representation candidate : allMembersOf<Representation>) {
        if (!web.legal.contains(candidate))
            continue;
        uint64_t votes = votesFor(candidate);
        if (votes > most) {
            best = candidate;
            most = votes;
        }
    }
    return best;
}

void RepresentationWebs::chooseForConstants()
{
    const Function& function = m_inputs.function;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (constantsInFormOfUses.contains(function.at(id).opcode))
                m_fixed[id.index()] = chooseForConstant(id);
        }
    }
}

Representation RepresentationWebs::chooseForConstant(ValueID constant) const
{
    const Function& function = m_inputs.function;
    const Representation natural = m_fixed[constant.index()];

    Votes wanted { };
    auto addUse = [&](Representation representation, uint64_t votes) {
        if (hasRegister(representation))
            wanted[std::to_underlying(representation)] += votes;
    };
    auto parameterTaking = [&](const Use& use) {
        return function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
    };
    for (const Use& use : m_analyses.useLists().usesOf(constant)) {
        switch (use.kind) {
        case Use::Kind::Operand: {
            const Value& user = function.at(use.user);
            if (!isForwardedOperand(user, use.operandIndex)) {
                addUse(wantedRepresentationOf(m_inputs, user, use.operandIndex), votesOf(use.block));
                break;
            }
            if (!hasRegister(representationOf(use.user)))
                return natural;
            addUse(representationOf(use.user), votesOf(use.block));
            break;
        }
        case Use::Kind::TerminatorOperand: {
            const Terminator& terminator = function.at(use.block).terminator;
            if (terminator.kind == TerminatorKind::TestAndBranch || terminator.kind == TerminatorKind::ReturnCall)
                return natural;
            addUse(wantedRepresentationOf(m_inputs, terminator, use.operandIndex), votesOf(use.block));
            break;
        }
        case Use::Kind::EdgeArgument:
            addUse(representationOf(parameterTaking(use)), votesOf(use.edge()));
            break;
        case Use::Kind::HandlerArgument:
            addUse(representationOf(parameterTaking(use)), votesOf(m_analyses.useLists().blockOf(constant)));
            break;
        case Use::Kind::BlockSubject:
            break;
        }
    }

    const Type& type = m_inputs.typeOf(constant);
    Representation best = natural;
    uint64_t most = wanted[std::to_underlying(natural)];
    for (Representation candidate : allMembersOf<Representation>) {
        uint64_t votes = wanted[std::to_underlying(candidate)];
        if (votes <= most || !isLegalRepresentation(type, candidate))
            continue;
        if (candidate == Representation::Cell32 && type.hasCells())
            continue;
        best = candidate;
        most = votes;
    }
    return best;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
