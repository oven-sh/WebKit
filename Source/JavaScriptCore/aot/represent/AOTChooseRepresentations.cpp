/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include "AOTIRBuilder.h"
#include "AOTRepresentationRules.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StringPrintStream.h>


namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t iterationsPerLoop = static_cast<uint64_t>(frequencyConstants.iterationsPerLoop);
static constexpr unsigned maxCountedLoopDepth = 6;

static constexpr uint64_t votesOutsideLoops(Expected<BlockFrequency> frequency)
{
    switch (frequency.predicted()) {
    case BlockFrequency::Normal:
        return 64;
    case BlockFrequency::Unlikely:
    case BlockFrequency::Rare:
        return 1;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr uint64_t votesAt(Expected<BlockFrequency> frequency, unsigned loopDepth)
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

template<typename Functor>
static void forEachForwardedOperand(const Value& value, const Functor& functor)
{
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (isForwardedOperand(value, operandIndex))
            functor(operand);
    });
}

static uint64_t votesOfConversionIn(FunctionAnalyses& analyses, BlockID block)
{
    const Function& function = analyses.function();
    if (function.blocks.size() == 1 && function.at(block).terminator.targets.isEmpty())
        return votesAt(BlockFrequency::Normal, 0);
    return votesAt(analyses.blockFrequencies().classOf(block), analyses.blockCycles().depthOf(block));
}

// `copied` gets each cast and each parameter that the value goes into, `wanted` the form in which each other operation takes it.
template<typename Copied, typename Wanted>
static void forEachTakerOf(const RepresentationInputs& inputs, const UseLists& useLists, ValueID id, const Copied& copied, const Wanted& wanted)
{
    const Function& function = inputs.function;
    for (const Use& use : useLists.usesOf(id)) {
        switch (use.kind) {
        case Use::Kind::Operand:
            if (isForwardedOperand(function.at(use.user), use.operandIndex))
                copied(use.user, use);
            else
                wanted(wantedRepresentationOf(inputs, function.at(use.user), use.operandIndex), use);
            break;
        case Use::Kind::TerminatorOperand:
            wanted(wantedRepresentationOf(inputs, function.at(use.block).terminator, use.operandIndex), use);
            break;
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
            copied(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex], use);
            break;
        case Use::Kind::BlockSubject:
            break;
        }
    }
}

namespace {

class RepresentationWebs {
    WTF_MAKE_NONCOPYABLE(RepresentationWebs);
public:
    RepresentationWebs(const RepresentationInputs& inputs, FunctionAnalyses& analyses)
        : m_inputs(inputs)
        , m_function(inputs.function)
        , m_analyses(analyses)
        , m_cfg(analyses.cfg())
        , m_useLists(analyses.useLists())
        , m_parent(m_function.values.size(), [](size_t index) { return static_cast<uint32_t>(index); })
        , m_webOfRoot(m_function.values.size())
        , m_fixed(FillWith { }, m_function.values.size(), Representation::Unchosen)
    {
        ASSERT(&analyses.function() == &m_function);
        for (uint32_t index = 0; index < m_fixed.size(); ++index) {
            ValueID id(index);
            if (m_useLists.locationOf(id).placement == ValuePlacement::Detached)
                continue;
            const Value& value = m_function.at(id);
            m_fixed[index] = constantsInFormOfUses.contains(value.opcode) ? defaultRepresentationOf(m_inputs.typeOf(id)) : resultRepresentationOf(m_inputs, value);
            // Which doubles hold any NaN is found when every value has its form.
            if (holdsAnyNaN(m_fixed[index]))
                m_fixed[index] = Representation::Double;
        }

        uniteCopies();
        numberWebs();
        restrictToLegalRepresentations();
        findRepeatedArguments();
        collectProducers();
        collectUses();
        admitNarrowReferences();
        choose();
        chooseForConstants();
    }

    Representation representationOf(ValueID id) const
    {
        std::optional<uint32_t> web = webOf(id);
        return web ? m_webs[*web].chosen : m_fixed[id.index()];
    }

private:
    using Votes = std::array<uint64_t, numberOfEnumerators<Representation>>;

    // A value that one edge passes more than once.
    struct RepeatedArgument {
        uint32_t from;
        uint32_t targetIndex;
        uint32_t value;
        uint32_t timesPassed;

        std::tuple<uint32_t, uint32_t, uint32_t> place() const { return { from, targetIndex, value }; }
    };

    struct Web {
        RepresentationSet legal;
        Votes produced { };
        Votes wanted { };
        Representation chosen { Representation::Unchosen };
        bool fitsNarrowReference { false };

        bool hasChoice() const { return fitsNarrowReference || legal != RepresentationSet { chosen }; }
        uint64_t votesFor(Representation representation) const { return produced[std::to_underlying(representation)] + wanted[std::to_underlying(representation)]; }

        uint64_t totalVotes() const
        {
            uint64_t total = 0;
            for (Representation representation : allMembersOf<Representation>)
                total += votesFor(representation);
            return total;
        }

        void addProducer(Representation representation, uint64_t votes)
        {
            ASSERT(representation != Representation::Unchosen);
            produced[std::to_underlying(representation)] += votes;
        }

        void addUse(Representation representation, uint64_t votes)
        {
            if (hasRegister(representation))
                wanted[std::to_underlying(representation)] += votes;
        }
    };

    struct Member {
        ValueID value;
        uint32_t web { 0 };
    };

    struct Link {
        uint32_t voter { 0 };
        uint32_t web { 0 };
        uint64_t votes { 0 };
        bool isProducer { false };
    };

    uint32_t find(uint32_t valueIndex) const
    {
        while (m_parent[valueIndex] != valueIndex) {
            m_parent[valueIndex] = m_parent[m_parent[valueIndex]];
            valueIndex = m_parent[valueIndex];
        }
        return valueIndex;
    }

    std::optional<uint32_t> webOf(ValueID id) const { return m_webOfRoot[find(id.index())]; }

    void uniteCopies()
    {
        for (BlockID covered : m_cfg.reversePostOrder()) {
            const std::optional<Target>& handler = m_function.at(covered).handler;
            if (!handler)
                continue;
            const CompactList<ValueID>& parameters = m_function.at(handler->block).parameters;
            for (uint32_t index = 0; index < handler->arguments.size(); ++index) {
                ValueID argument = handler->arguments[index];
                if (m_fixed[argument.index()] != Representation::Unchosen)
                    continue;
                uint32_t rootOfParameter = find(parameters[index].index());
                uint32_t rootOfArgument = find(argument.index());
                m_parent[std::max(rootOfParameter, rootOfArgument)] = std::min(rootOfParameter, rootOfArgument);
            }
        }
    }

    void numberWebs()
    {
        for (uint32_t index = 0; index < m_fixed.size(); ++index) {
            // The root of a web is the member with the lowest number.
            uint32_t root = find(index);
            bool picks = root == index && m_fixed[index] == Representation::Unchosen && m_useLists.locationOf(ValueID(index)).placement != ValuePlacement::Detached;
            if (picks) {
                m_webOfRoot[index] = static_cast<uint32_t>(m_webs.size());
                m_webs.append(Web { });
            }
            if (std::optional<uint32_t> web = m_webOfRoot[root])
                m_members.append(Member { ValueID(index), *web });
        }
    }

    void restrictToLegalRepresentations()
    {
        struct LegalRepresentationsOfType {
            TypeID type;
            RepresentationSet representations;
        };
        std::array<LegalRepresentationsOfType, 16> remembered { };
        auto legalRepresentationsOfValue = [&](ValueID id) {
            TypeID type = m_function.at(id).type;
            LegalRepresentationsOfType& entry = remembered[type.index() % remembered.size()];
            if (entry.type != type || !type)
                entry = { type, legalRepresentationsOf(m_inputs.typeOf(id)) };
            return entry.representations;
        };

        for (Web& web : m_webs)
            web.legal = webRepresentations | RepresentationSet { Representation::Cell32 };

        for (Member member : m_members) {
            Web& web = m_webs[member.web];
            web.legal &= legalRepresentationsOfValue(member.value);
            forEachForwardedOperand(m_function.at(member.value), [&](ValueID operand) {
                RepresentationSet convertible = legalRepresentationsOfValue(operand);
                convertible &= representationsWithRegister;
                if (!hasRegister(m_fixed[operand.index()]))
                    convertible.add(m_fixed[operand.index()]);
                web.legal &= convertible;
            });
        }

        for (Web& web : m_webs) {
            web.fitsNarrowReference = web.legal.contains(Representation::Cell32);
            web.legal &= webRepresentations;
        }

        for (Member member : m_members) {
            Web& web = m_webs[member.web];
            auto tryDefaultOf = [&](ValueID id) {
                if (web.chosen != Representation::Unchosen)
                    return;
                Representation representation = defaultRepresentationOf(m_inputs.typeOf(id));
                if (web.legal.contains(representation))
                    web.chosen = representation;
            };
            tryDefaultOf(member.value);
            forEachForwardedOperand(m_function.at(member.value), tryDefaultOf);
        }

        for (Web& web : m_webs) {
            if (web.chosen == Representation::Unchosen)
                web.chosen = Representation::JSValue;
            RELEASE_ASSERT(web.legal.contains(web.chosen));
        }
    }

    void collectProducers()
    {
        auto addCopyOf = [&](ValueID source, Member member, uint64_t votes) {
            if (constantsInFormOfUses.contains(m_function.at(source).opcode))
                return;
            std::optional<uint32_t> web = webOf(source);
            if (web == member.web)
                return;
            if (web && m_webs[*web].hasChoice())
                m_links.append(Link { *web, member.web, votes, true });
            m_webs[member.web].addProducer(web ? m_webs[*web].chosen : m_fixed[source.index()], votes);
        };

        auto addIncomingValue = [&](Member parameter, const IncomingValue& incoming, Edge edge) {
            if (edge.isHandlerEdge())
                return addCopyOf(incoming.argument, parameter, votesOf(m_useLists.blockOf(incoming.argument)));
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                return addCopyOf(incoming.argument, parameter, votesOfArgument(edge, incoming.argument));
            case IncomingValue::Kind::EdgeResult:
                break;
            }

            EdgeResult result = edgeResultAt(m_function, incoming.result);
            switch (result.kind) {
            case EdgeResultKind::NarrowedOperand:
                return addCopyOf(result.subject, parameter, votesOf(edge));
            case EdgeResultKind::Guard:
                return m_webs[parameter.web].addProducer(Representation::Void, votesOf(edge));
            case EdgeResultKind::DeclaredResult:
                return m_webs[parameter.web].addProducer(edgeResultRepresentationOf(m_inputs, incoming.result), votesOf(edge));
            }
        };

        for (Member member : m_members) {
            if (!m_webs[member.web].hasChoice())
                continue;
            ValueLocation location = m_useLists.locationOf(member.value);
            switch (location.placement) {
            case ValuePlacement::Parameter:
                for (Edge edge : m_cfg.incomingEdges(location.block))
                    addIncomingValue(member, incomingValueOf(m_function, edge, location.index), edge);
                break;
            case ValuePlacement::InBlock:
                forEachForwardedOperand(m_function.at(member.value), [&](ValueID operand) {
                    addCopyOf(operand, member, votesOf(location.block));
                });
                break;
            case ValuePlacement::Detached:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    }

    // The back end adds in 32 bits then, whichever width the operands come in.
    bool isSumOfWhichOnlyLowBitsAreUsed(ValueID id) const
    {
        const Value& sum = m_function.at(id);
        if ((sum.opcode != Opcode::Add && sum.opcode != Opcode::Sub) || m_fixed[id.index()] != Representation::Int64)
            return false;
        auto uses = m_useLists.usesOf(id);
        return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) {
            if (use.kind != Use::Kind::Operand)
                return false;
            Opcode opcode = m_function.at(use.user).opcode;
            return (opcode == Opcode::BitOr || opcode == Opcode::BitAnd || opcode == Opcode::BitXor) && m_fixed[use.user.index()] == Representation::Int32;
        });
    }

    void collectUses()
    {
        for (Member member : m_members) {
            Web& web = m_webs[member.web];
            if (!web.hasChoice())
                continue;
            forEachTakerOf(m_inputs, m_useLists, member.value, [&](ValueID copy, const Use& use) {
                uint32_t webOfCopy = *webOf(copy);
                ASSERT(use.kind != Use::Kind::HandlerArgument || webOfCopy == member.web);
                if (webOfCopy == member.web)
                    return;
                uint64_t votes = use.kind == Use::Kind::Operand ? votesOf(use.block) : votesOfArgument(use.edge(), member.value);
                if (m_webs[webOfCopy].hasChoice())
                    m_links.append(Link { webOfCopy, member.web, votes, false });
                web.addUse(m_webs[webOfCopy].chosen, votes);
            }, [&](Representation wanted, const Use& use) {
                if (use.kind != Use::Kind::Operand || !isSumOfWhichOnlyLowBitsAreUsed(use.user))
                    web.addUse(wanted, votesOf(use.block));
            });
        }
    }

    uint64_t votesOf(BlockID block) const
    {
        if (m_votesOfBlock.isEmpty())
            m_votesOfBlock.fill(0, m_function.blocks.size());
        uint64_t& votes = m_votesOfBlock[block.index()];
        if (!votes)
            votes = votesOfConversionIn(m_analyses, block);
        return votes;
    }

    uint64_t votesOf(Edge edge) const
    {
        ASSERT(!edge.isHandlerEdge());
        BlockID to = targetOf(m_function, edge).block;
        const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
        const BlockCycles::Forest& cycles = m_analyses.blockCycles().forest();
        Expected<BlockFrequency> frequency = rarerOf(frequencies.classOf(edge), rarerOf(frequencies.classOf(edge.from), frequencies.classOf(to)));
        return votesAt(frequency, cycles.depth(cycles.nearestCommon(cycles.innermost(edge.from), cycles.innermost(to))));
    }

    uint64_t votesOfArgument(Edge edge, ValueID argument) const
    {
        uint64_t timesPassed = 1;
        if (edge.isHandlerEdge())
            timesPassed = std::ranges::count(targetOf(m_function, edge).arguments, argument);
        else if (!m_repeatedArguments.isEmpty()) {
            RepeatedArgument key { edge.from.index(), edge.targetIndex, argument.index(), 0 };
            auto found = std::ranges::lower_bound(m_repeatedArguments, key.place(), { }, &RepeatedArgument::place);
            if (found != m_repeatedArguments.end() && found->place() == key.place())
                timesPassed = found->timesPassed;
        }
        return std::max<uint64_t>(votesOf(edge) / std::max<uint64_t>(timesPassed, 1), 1);
    }

    void findRepeatedArguments()
    {
        constexpr uint32_t notRepeated = UINT32_MAX;
        Vector<uint32_t> lastEdgePassing(FillWith { }, m_function.values.size(), 0u);
        Vector<uint32_t> entryOfValue(FillWith { }, m_function.values.size(), notRepeated);
        uint32_t edgeNumber = 0;
        for (BlockID block : m_cfg.reversePostOrder()) {
            const auto& targets = m_function.at(block).terminator.targets;
            for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
                ++edgeNumber;
                for (ValueID argument : targets[targetIndex].arguments) {
                    if (!argument)
                        continue;
                    uint32_t& entry = entryOfValue[argument.index()];
                    if (std::exchange(lastEdgePassing[argument.index()], edgeNumber) != edgeNumber) {
                        entry = notRepeated;
                        continue;
                    }
                    if (entry == notRepeated) {
                        entry = m_repeatedArguments.size();
                        m_repeatedArguments.append({ block.index(), targetIndex, argument.index(), 1 });
                    }
                    ++m_repeatedArguments[entry].timesPassed;
                }
            }
        }
        AOT::sort(m_repeatedArguments, { }, &RepeatedArgument::place);
    }

    void admitNarrowReferences()
    {
        BitVector websWithVotes(m_webs.size());
        for (uint32_t index = 0; index < m_webs.size(); ++index)
            websWithVotes.quickSet(index, m_webs[index].fitsNarrowReference && m_webs[index].votesFor(Representation::Cell32));
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
    BitVector findNarrowWebsAmong(const BitVector& webs)
    {
        findWhereCollectorMayHaveRun(webs);
        BitVector narrowWebs = webs;
        for (Member member : m_members) {
            if (narrowWebs.quickGet(member.web) && !mayStayNarrow(member.value))
                narrowWebs.quickClear(member.web);
        }
        if (narrowWebs.isEmpty())
            return narrowWebs;

        BitVector mayLieInFrame = findValuesThatMayLieInFrames();
        for (Member member : m_members) {
            if (mayLieInFrame.quickGet(member.value.index()))
                narrowWebs.quickClear(member.web);
        }
        return narrowWebs;
    }

    // A narrow reference counts from the base of the cage of cells, and a cell of a frame lies outside of the cage.
    BitVector findValuesThatMayLieInFrames() const
    {
        BitVector values(m_function.values.size());
        for (ValueID parameter : m_function.at(entryBlock).parameters) {
            if (mayBeReference(m_inputs.typeOf(parameter)))
                values.quickSet(parameter.index());
        }
        std::span<const BlockID> blocks = m_cfg.reversePostOrder();
        for (BlockID block : blocks) {
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                bool readsWideSlot = readsFollowedByFramePointers.contains(value.opcode) && m_fixed[id.index()] != Representation::Cell32 && mayBeReference(m_inputs.typeOf(id));
                if (hasFrameStorage(value) || readsWideSlot)
                    values.quickSet(id.index());
            }
        }

        for (bool changed = !values.isEmpty(); changed;) {
            changed = false;
            for (BlockID block : blocks) {
                for (ValueID id : m_function.at(block).values) {
                    forEachReturnedOperand(m_function, m_function.at(id), [&](ValueID operand) {
                        if (values.quickGet(operand.index()))
                            changed |= !values.quickSet(id.index());
                    });
                }
                forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
                    const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
                    for (uint32_t index = 0; index < parameters.size(); ++index) {
                        ValueID passed = valuePassedOn(m_function, edge, index);
                        if (passed && values.quickGet(passed.index()))
                            changed |= !values.quickSet(parameters[index].index());
                    }
                });
            }
        }
        return values;
    }

    void findFirstCollectingValues()
    {
        m_firstCollectingValue.fill(0, m_function.blocks.size());
        for (BlockID block : m_cfg.reversePostOrder()) {
            const auto& values = m_function.at(block).values;
            auto first = std::ranges::find_if(values, [&](ValueID id) {
                return mayCollect(m_function.at(id).opcode);
            });
            m_firstCollectingValue[block.index()] = static_cast<uint32_t>(first - values.begin());
        }
    }

    bool runsNoCollector(BlockID block) const { return m_firstCollectingValue[block.index()] == m_function.at(block).values.size(); }

    void findWhereCollectorMayHaveRun(const BitVector& webs)
    {
        uint32_t numberOfBlocksWithReference = 0;
        m_indexOfBlockWithReference.fill(std::nullopt, m_function.blocks.size());
        for (Member member : m_members) {
            if (!webs.quickGet(member.web))
                continue;
            ValueLocation location = m_useLists.locationOf(member.value);
            if (location.placement != ValuePlacement::Parameter)
                continue;
            std::optional<uint32_t>& indexOfBlock = m_indexOfBlockWithReference[location.block.index()];
            if (!indexOfBlock)
                indexOfBlock = numberOfBlocksWithReference++;
        }

        // A block that defines a reference dominates its uses. If no collection can lie between a dominator of a block and the block, none can lie between a dominator below that one and the block:
        // one depth in the dominator tree says it for all dominators of the block.
        m_depthOfHighestDominatorWithoutCollectionSince.fill(0, m_function.blocks.size());
        if (!numberOfBlocksWithReference)
            return;
        const Dominators& dominators = m_analyses.dominators();
        for (bool changed = true; changed;) {
            changed = false;
            for (BlockID block : m_cfg.reversePostOrder()) {
                uint32_t depthOfBlock = dominators.depthOf(block);
                uint32_t depth = m_depthOfHighestDominatorWithoutCollectionSince[block.index()];
                for (Edge edge : m_cfg.incomingEdges(block)) {
                    if (edge.isHandlerEdge() || !runsNoCollector(edge.from)) {
                        depth = depthOfBlock;
                        break;
                    }
                    depth = std::max(depth, std::min(depthOfBlock, m_depthOfHighestDominatorWithoutCollectionSince[edge.from.index()]));
                }
                if (depth != m_depthOfHighestDominatorWithoutCollectionSince[block.index()]) {
                    m_depthOfHighestDominatorWithoutCollectionSince[block.index()] = depth;
                    changed = true;
                }
            }
        }
    }

    bool isReachedBeforeCollectorCanRun(const Use& use, BlockID definingBlock) const
    {
        if (use.block != definingBlock && m_analyses.dominators().depthOf(definingBlock) < m_depthOfHighestDominatorWithoutCollectionSince[use.block.index()])
            return false;

        switch (use.kind) {
        case Use::Kind::Operand:
            // A test hands its operand on at the end of the block.
            if (provesOf(m_function.at(use.user).opcode) != Proof::None)
                return runsNoCollector(use.block);
            return m_useLists.locationOf(use.user).index <= m_firstCollectingValue[use.block.index()];
        case Use::Kind::TerminatorOperand:
        case Use::Kind::EdgeArgument:
            return runsNoCollector(use.block);
        case Use::Kind::HandlerArgument:
            return false;
        case Use::Kind::BlockSubject:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // A narrow reference lives from a load to its Decode.
    bool mayStayNarrow(ValueID member) const
    {
        ValueLocation location = m_useLists.locationOf(member);
        switch (location.placement) {
        case ValuePlacement::Parameter: {
            bool isCaught = std::ranges::any_of(m_cfg.incomingEdges(location.block), [](Edge edge) {
                return edge.isHandlerEdge();
            });
            if (isCaught || !m_indexOfBlockWithReference[location.block.index()])
                return false;
            return std::ranges::all_of(m_useLists.usesOf(member), [&](const Use& use) {
                return isReachedBeforeCollectorCanRun(use, location.block);
            });
        }
        case ValuePlacement::InBlock:
            return isCast(m_function.at(member).opcode);
        case ValuePlacement::Detached:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    template<typename VotesFor, typename IsLegal>
    static Representation formWithMostVotes(Representation before, const VotesFor& votesFor, const IsLegal& isLegal)
    {
        Representation best = before;
        for (Representation candidate : allMembersOf<Representation>) {
            if (votesFor(candidate) > votesFor(best) && isLegal(candidate))
                best = candidate;
        }
        return best;
    }

    void choose()
    {
        AOT_THRESHOLD(unsigned, maxRounds, 8);
        Vector<uint32_t> firstLinkOf;
        Vector<Link> linksByVoter;
        groupByIndex(m_webs.size(), firstLinkOf, linksByVoter, [&](const auto& add) {
            for (const Link& link : m_links)
                add(link.voter, link);
        });
        auto linksOf = [&](uint32_t voter) {
            return entriesOfGroup(firstLinkOf, linksByVoter, voter);
        };
        auto move = [&](uint32_t index, Representation chosen) {
            Representation before = std::exchange(m_webs[index].chosen, chosen);
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
        auto moveTogether = [&](uint32_t source, uint32_t destination) {
            Web& from = m_webs[source];
            Web& to = m_webs[destination];
            RepresentationSet candidates = from.legal;
            candidates &= to.legal;
            candidates &= representationsWithRegister;
            bool hasCandidate = false;
            candidates.forEach([&](Representation candidate) {
                hasCandidate |= candidate != from.chosen && candidate != to.chosen;
            });
            if (!hasCandidate)
                return false;
            uint64_t votesAsProducer = votesBetween(source, destination, true);
            uint64_t votesAsUse = votesBetween(destination, source, false);
            uint64_t totalVotes = from.totalVotes() + to.totalVotes();
            uint64_t fewest = totalVotes - from.votesFor(from.chosen) - to.votesFor(to.chosen);
            if (from.chosen != to.chosen && hasRegister(to.chosen))
                fewest -= votesAsUse;
            std::optional<Representation> best;
            candidates.forEach([&](Representation candidate) {
                if (candidate == from.chosen || candidate == to.chosen)
                    return;
                uint64_t needed = totalVotes - from.votesFor(candidate) - to.votesFor(candidate) - votesAsProducer - (hasRegister(to.chosen) ? votesAsUse : 0);
                if (needed < fewest) {
                    best = candidate;
                    fewest = needed;
                }
            });
            if (!best)
                return false;
            move(source, *best);
            move(destination, *best);
            return true;
        };

        Vector<uint32_t, 32> websWithChoice;
        for (uint32_t index = 0; index < m_webs.size(); ++index) {
            if (m_webs[index].hasChoice())
                websWithChoice.append(index);
        }

        for (unsigned round = 0; round < maxRounds; ++round) {
            bool changed = false;
            for (uint32_t index : websWithChoice) {
                const Web& web = m_webs[index];
                Representation chosen = web.chosen;
                web.legal.forEach([&](Representation candidate) {
                    if (web.votesFor(candidate) > web.votesFor(chosen))
                        chosen = candidate;
                });
                if (chosen == web.chosen)
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
    }

    void chooseForConstants()
    {
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (constantsInFormOfUses.contains(m_function.at(id).opcode))
                    m_fixed[id.index()] = chooseForConstant(id);
            }
        }
    }

    Representation chooseForConstant(ValueID constant) const
    {
        const Representation natural = m_fixed[constant.index()];
        Web uses;
        bool staysNatural = false;
        forEachTakerOf(m_inputs, m_useLists, constant, [&](ValueID copy, const Use& use) {
            staysNatural |= use.kind == Use::Kind::Operand && !hasRegister(representationOf(copy));
            uses.addUse(representationOf(copy), use.kind == Use::Kind::EdgeArgument ? votesOf(use.edge()) : votesOf(use.kind == Use::Kind::Operand ? use.block : m_useLists.blockOf(constant)));
        }, [&](Representation wanted, const Use& use) {
            TerminatorKind kind = m_function.at(use.block).terminator.kind;
            staysNatural |= use.kind == Use::Kind::TerminatorOperand && (kind == TerminatorKind::TestAndBranch || kind == TerminatorKind::ReturnCall);
            uses.addUse(wanted, votesOf(use.block));
        });
        if (staysNatural)
            return natural;

        const Type& type = m_inputs.typeOf(constant);
        return formWithMostVotes(natural, [&](Representation candidate) {
            return uses.votesFor(candidate);
        }, [&](Representation candidate) {
            return isLegalRepresentation(type, candidate) && !(candidate == Representation::Cell32 && type.hasCells());
        });
    }

    const RepresentationInputs& m_inputs;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    mutable Vector<uint32_t> m_parent;
    Vector<std::optional<uint32_t>> m_webOfRoot;
    Vector<Representation> m_fixed;
    mutable Vector<uint64_t, 16> m_votesOfBlock;
    Vector<uint32_t> m_firstCollectingValue;
    Vector<std::optional<uint32_t>> m_indexOfBlockWithReference;
    Vector<uint32_t> m_depthOfHighestDominatorWithoutCollectionSince;
    Vector<Web> m_webs;
    // By ascending value.
    Vector<Member> m_members;
    Vector<Link> m_links;
    Vector<RepeatedArgument> m_repeatedArguments;
};

struct UsesOfCopies {
    Vector<ValueID, 16> copies;
    uint64_t votesForBoxed { 0 };
    uint64_t votesForOthers { 0 };
};

} // anonymous namespace

static void uncoverEntryBlock(FunctionPassContext& pass, IRBuilder& builder)
{
    const Block& entry = pass.function.at(entryBlock);
    if (!entry.handler)
        return;

    builder.setOrigin(entry.terminator.origin());
    builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    pass.analyses.didChangeControlFlow();
}

AOT_THRESHOLD(unsigned, maxCopiesFollowed, 4);

static bool isMadeBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id, unsigned copiesFollowed = 0)
{
    const Function& function = inputs.function;
    const Value& value = function.at(id);
    if (!picksOwnRepresentation(inputs, id))
        return resultRepresentationOf(inputs, value) == Representation::JSValue;
    if (copiesFollowed == maxCopiesFollowed || constantsInFormOfUses.contains(value.opcode) || value.opcode == Opcode::Unspecified)
        return false;

    if (value.opcode != Opcode::Parameter) {
        bool isBoxed = true;
        forEachForwardedOperand(value, [&](ValueID operand) {
            isBoxed = isBoxed && isMadeBoxed(pass, inputs, operand, copiesFollowed + 1);
        });
        return isBoxed;
    }

    ValueLocation location = pass.analyses.useLists().locationOf(id);
    bool hasBoxedSource = false;
    for (Edge edge : pass.analyses.cfg().incomingEdges(location.block)) {
        ValueID source = edge.isHandlerEdge() ? ValueID() : valuePassedOn(function, edge, location.index);
        if (!source)
            return false;
        if (constantsInFormOfUses.contains(function.at(source).opcode))
            continue;
        if (!isMadeBoxed(pass, inputs, source, copiesFollowed + 1))
            return false;
        hasBoxedSource = true;
    }
    return hasBoxedSource;
}

static void countUsesOfCopies(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id, UsesOfCopies& uses)
{
    AOT_THRESHOLD(size_t, maxCopies, 16);
    if (uses.copies.size() == maxCopies || uses.copies.contains(id))
        return;
    uses.copies.append(id);

    auto count = [&](Representation wanted, const Use& use) {
        if (hasRegister(wanted))
            (wanted == Representation::JSValue ? uses.votesForBoxed : uses.votesForOthers) += votesOfConversionIn(pass.analyses, use.block);
    };
    forEachTakerOf(inputs, pass.analyses.useLists(), id, [&](ValueID copy, const Use& use) {
        if (use.kind != Use::Kind::Operand && defaultRepresentationOf(inputs.typeOf(copy)) == Representation::JSValue)
            count(Representation::JSValue, use);
        else
            countUsesOfCopies(pass, inputs, copy, uses);
    }, count);
}

template<typename Predicate>
static bool everyOperandBesideInt32(const RepresentationInputs& inputs, const Value& sum, const Predicate& holds)
{
    bool hasSuchOperand = false;
    bool holdsForAll = true;
    sum.forEachOperand([&](ValueID operand, uint32_t) {
        if (inputs.typeOf(operand).isInt32())
            return;
        hasSuchOperand = true;
        holdsForAll = holdsForAll && holds(operand);
    });
    return hasSuchOperand && holdsForAll;
}

static Vector<ValueID, 8> keepSumsOfBoxedNumbersBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Function& function = pass.function;
    Vector<ValueID, 8> kept;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (!sumsOfNumbers.contains(value.opcode) || value.form() != OperatorForm::Numbers)
                continue;
            bool takesBoxedNumbers = everyOperandBesideInt32(inputs, value, [&](ValueID operand) {
                return inputs.typeOf(operand).primitives & Primitive::Integer && isMadeBoxed(pass, inputs, operand);
            });
            if (!takesBoxedNumbers)
                continue;
            UsesOfCopies uses;
            countUsesOfCopies(pass, inputs, id, uses);
            if (uses.votesForBoxed > uses.votesForOthers)
                kept.append(id);
        }
    }
    for (ValueID id : kept)
        function.edit(id, ValueEdit::Type).representation = Representation::JSValue;
    return kept;
}

static void writeRepresentations(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Function& function = pass.function;
    Vector<ValueID, 8> sumsKeptBoxed = keepSumsOfBoxedNumbersBoxed(pass, inputs);
    RepresentationWebs webs(inputs, pass.analyses);
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters)
            function.edit(parameter, ValueEdit::Type).representation = webs.representationOf(parameter);
        for (ValueID value : block.values)
            function.edit(value, ValueEdit::Type).representation = webs.representationOf(value);
    }

    for (ValueID id : sumsKeptBoxed) {
        bool operandsStayedBoxed = everyOperandBesideInt32(inputs, function.at(id), [&](ValueID operand) {
            return function.at(operand).representation == Representation::JSValue;
        });
        if (!operandsStayedBoxed)
            function.edit(id, ValueEdit::Type).representation = Representation::Double;
    }
}

// A double read out of the bytes of a buffer may be a NaN with any sign and payload, and so may whatever is computed from it or is a copy of it. The marker of undefined leaves its payload in what is computed from it.
static bool findDoublesWithAnyNaN(FunctionPassContext& pass, const RepresentationInputs& inputs, const BitVector& keptWithTheOneNaN)
{
    Function& function = pass.function;
    Vector<ValueID, 16> worklist;
    auto canBeWidened = [&](ValueID id) {
        Representation representation = function.at(id).representation;
        return representation == Representation::Double || representation == Representation::DoubleWithPayload;
    };
    auto widen = [&](ValueID id, Representation form) {
        if (!holdsAnyNaN(form) || !canBeWidened(id) || function.at(id).representation >= form || keptWithTheOneNaN.get(id.index()))
            return;
        function.edit(id, ValueEdit::Type).representation = form;
        worklist.append(id);
    };
    auto widenToResult = [&](ValueID id) {
        if (canBeWidened(id))
            widen(id, resultRepresentationOf(inputs, function.at(id)));
    };
    auto forEachResultOnEdgeFrom = [&](BlockID block, const auto& functor) {
        forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
            const CompactList<ValueID>& parameters = function.at(target.block).parameters;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                IncomingValue incoming = incomingValueOf(function, edge, index);
                if (incoming.kind == IncomingValue::Kind::EdgeResult && canBeWidened(parameters[index]))
                    functor(parameters[index], incoming.result);
            }
        });
    };
    auto markResultsOfTerminator = [&](BlockID block) {
        if (function.at(block).terminator.kind != TerminatorKind::TryCallInline)
            return;
        forEachResultOnEdgeFrom(block, [&](ValueID parameter, EdgeResultPosition result) {
            widen(parameter, edgeResultRepresentationOf(inputs, result));
        });
    };

    std::span<const BlockID> blocks = pass.analyses.cfg().reversePostOrder();
    for (BlockID block : blocks) {
        for (ValueID value : function.at(block).values)
            widenToResult(value);
        markResultsOfTerminator(block);
    }
    if (worklist.isEmpty())
        return false;

    Vector<std::pair<ValueID, ValueID>, 8> narrowedCopies;
    for (BlockID block : blocks) {
        forEachResultOnEdgeFrom(block, [&](ValueID parameter, EdgeResultPosition position) {
            EdgeResult result = edgeResultAt(function, position);
            if (result.kind == EdgeResultKind::NarrowedOperand)
                narrowedCopies.append({ result.subject, parameter });
        });
    }
    AOT::sort(narrowedCopies);

    const UseLists& useLists = pass.analyses.useLists();
    while (!worklist.isEmpty()) {
        ValueID widened = worklist.takeLast();
        Representation form = function.at(widened).representation;
        auto* copy = std::lower_bound(narrowedCopies.begin(), narrowedCopies.end(), std::pair { widened, ValueID(0u) });
        for (; copy != narrowedCopies.end() && copy->first == widened; ++copy)
            widen(copy->second, form);
        for (const Use& use : useLists.usesOf(widened)) {
            switch (use.kind) {
            case Use::Kind::Operand:
                if (isForwardedOperand(function.at(use.user), use.operandIndex))
                    widen(use.user, form);
                else
                    widenToResult(use.user);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
                widen(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex], form);
                break;
            case Use::Kind::TerminatorOperand:
                markResultsOfTerminator(use.block);
                break;
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return true;
}

// A NaN out of bytes becomes the one NaN where it leaves arithmetic. Where each run of a read is followed by one such place, the read does it, at the same price, and so does the edge that brings a payload.
static BitVector findDoublesKeptWithTheOneNaN(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    const Function& function = pass.function;
    const CFG& cfg = pass.analyses.cfg();
    const UseLists& useLists = pass.analyses.useLists();
    auto isImpure = [&](ValueID id) {
        return function.at(id).representation == Representation::ImpureDouble;
    };
    auto dropsBitsOfNaN = [](Representation wanted) {
        return wanted == Representation::Double || wanted == Representation::OptionalDouble || wanted == Representation::JSValue;
    };

    BitVector result;
    BitVector isVisited;
    BitVector blocksOfReads;
    BitVector blocksWhereBitsAreDropped;
    Vector<ValueID, 16> web;
    Vector<std::pair<ValueID, BlockID>, 4> reads;
    auto visitWebOf = [&](ValueID first) {
        web.shrink(0);
        reads.shrink(0);
        blocksOfReads.clearAll();
        blocksWhereBitsAreDropped.clearAll();
        bool isFree = true;
        bool computes = false;
        auto add = [&](ValueID id) {
            if (!isVisited.set(id.index()))
                web.append(id);
        };
        auto addInput = [&](ValueID input) {
            if (isImpure(input))
                add(input);
            else if (holdsAnyNaN(function.at(input).representation) || (inputs.typeOf(input).isJavaScriptValue() && !inputs.typeOf(input).isNumber()))
                isFree = false;
        };
        auto addRead = [&](ValueID read, BlockID block) {
            reads.append({ read, block });
            if (blocksOfReads.set(block.index()))
                isFree = false;
        };
        add(first);
        for (size_t index = 0; index < web.size(); ++index) {
            ValueID id = web[index];
            ValueLocation location = useLists.locationOf(id);
            if (location.placement != ValuePlacement::InBlock) {
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    IncomingValue incoming = incomingValueOf(function, edge, location.index);
                    if (incoming.kind == IncomingValue::Kind::Argument && incoming.argument && function.at(incoming.argument).representation == Representation::DoubleWithPayload && !edge.isHandlerEdge() && function.at(edge.from).terminator.kind == TerminatorKind::Jump)
                        addRead(id, edge.from);
                    else if (incoming.kind == IncomingValue::Kind::Argument && incoming.argument)
                        addInput(incoming.argument);
                    else if (incoming.kind == IncomingValue::Kind::EdgeResult && readsDoubleOutOfBytes(function.at(edge.from).terminator) && edgeResultAt(function, incoming.result).kind == EdgeResultKind::DeclaredResult)
                        addRead(id, location.block);
                    else
                        isFree = false;
                }
            } else if (const Value& value = function.at(id); readsDoubleOutOfBytes(value))
                addRead(id, location.block);
            else {
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    addInput(operand);
                    computes |= !isForwardedOperand(value, operandIndex);
                });
            }

            for (const Use& use : useLists.usesOf(id)) {
                Representation wanted = Representation::Unchosen;
                switch (use.kind) {
                case Use::Kind::Operand: {
                    const Value& user = function.at(use.user);
                    if (isImpure(use.user) && !readsDoubleOutOfBytes(user)) {
                        add(use.user);
                        continue;
                    }
                    wanted = wantedRepresentationOf(inputs, user, use.operandIndex);
                    break;
                }
                case Use::Kind::TerminatorOperand: {
                    const Terminator& terminator = function.at(use.block).terminator;
                    if (!terminator.targets.isEmpty())
                        isFree = false;
                    wanted = wantedRepresentationOf(inputs, terminator, use.operandIndex);
                    break;
                }
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument: {
                    ValueID parameter = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
                    if (isImpure(parameter)) {
                        add(parameter);
                        continue;
                    }
                    if (function.at(use.block).terminator.kind == TerminatorKind::Jump && use.kind == Use::Kind::EdgeArgument)
                        wanted = function.at(parameter).representation;
                    break;
                }
                case Use::Kind::BlockSubject:
                    isFree = false;
                    break;
                }
                if (dropsBitsOfNaN(wanted))
                    blocksWhereBitsAreDropped.set(use.block.index());
            }
        }
        if (!isFree || !computes)
            return;

        for (auto [read, block] : reads) {
            for (size_t steps = 0; !blocksWhereBitsAreDropped.get(block.index()); ++steps) {
                const Terminator& terminator = function.at(block).terminator;
                if (terminator.kind != TerminatorKind::Jump || steps == function.blocks.size())
                    return;
                block = terminator.targets[0].block;
                if (blocksOfReads.get(block.index()))
                    return;
            }
        }
        for (auto [read, block] : reads)
            result.set(read.index());
    };

    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters) {
            if (isImpure(parameter) && !isVisited.get(parameter.index()))
                visitWebOf(parameter);
        }
        for (ValueID value : block.values) {
            if (isImpure(value) && !isVisited.get(value.index()))
                visitWebOf(value);
        }
    }
    return result;
}

// The sign of a NaN that arithmetic has made counts, and that of a NaN out of bytes does not. Behind a join nobody can tell the two apart, so each edge into such a join makes of its NaN what is to be made of it.
// What a loop carries round is computed from both, as a sum of the two is, and stays bits of bytes.
static bool keepJoinsOfBytesAndArithmeticWithTheOneNaN(FunctionPassContext& pass, const RepresentationInputs& inputs, BitVector& keptWithTheOneNaN)
{
    const Function& function = pass.function;
    const CFG& cfg = pass.analyses.cfg();
    bool hasFoundJoin = false;
    for (BlockID id : cfg.reversePostOrder()) {
        const CompactList<ValueID>& parameters = function.at(id).parameters;
        std::optional<bool> isLoopHeader;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            if (function.at(parameters[index]).representation != Representation::ImpureDouble)
                continue;
            if (!isLoopHeader) {
                isLoopHeader = std::ranges::any_of(cfg.incomingEdges(id), [&](Edge edge) {
                    return cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(id);
                });
            }
            if (*isLoopHeader)
                break;
            bool takesNaNOfArithmetic = std::ranges::any_of(cfg.incomingEdges(id), [&](Edge edge) {
                IncomingValue incoming = incomingValueOf(function, edge, index);
                if (incoming.kind == IncomingValue::Kind::EdgeResult) {
                    EdgeResult result = edgeResultAt(function, incoming.result);
                    bool isCopy = result.kind == EdgeResultKind::NarrowedOperand;
                    return (isCopy ? function.at(result.subject).representation : edgeResultRepresentationOf(inputs, incoming.result)) != Representation::ImpureDouble;
                }
                if (!incoming.argument || function.at(incoming.argument).representation == Representation::ImpureDouble)
                    return false;
                return inputs.typeOf(incoming.argument).mayBeNaN() && !makesTheOneNaNOfBytes(inputs, function.at(incoming.argument));
            });
            if (!takesNaNOfArithmetic)
                continue;
            keptWithTheOneNaN.set(parameters[index].index());
            hasFoundJoin = true;
        }
    }
    return hasFoundJoin;
}

static std::optional<Value> makeOnlyValueOf(const Type& type)
{
    if (type.isInteger() && type.integers.isSingleValue()) {
        Value constant = IRBuilder::make(Opcode::Integer);
        constant.setInteger(type.integers.minimum);
        return constant;
    }
    if (type.isBoolean() && type.isSingleValue()) {
        Value constant = IRBuilder::make(Opcode::Boolean);
        constant.setBoolean(type.primitives & Primitive::True);
        return constant;
    }
    return std::nullopt;
}

namespace {

// The code generator converts what an operation takes. It hands an operation each of its operands by the id of the value, in one form, so an operation that takes one value in two forms gets its conversions here.
struct ConversionInserter {
    const RepresentationInputs& inputs;
    FunctionPassContext& pass;
    IRBuilder& builder;
    Function& function { pass.function };
    uint32_t numberOfConversions { 0 };
    bool hasEdited { false };

    bool needsConversion(ValueID value, Representation wanted) const
    {
        return hasRegister(wanted) && function.at(value).representation != wanted;
    }

    template<typename Operation>
    bool takesOneValueInTwoForms(const Operation& user) const
    {
        AOT_THRESHOLD(uint32_t, maxOperandsComparedInPairs, 6);
        uint32_t numberOfOperands = user.numberOfOperands();
        if (numberOfOperands < 2)
            return false;

        auto formTakenAt = [&](uint32_t operandIndex) {
            ValueID operand = user.operand(operandIndex);
            Representation wanted = wantedRepresentationOf(inputs, user, operandIndex);
            return needsConversion(operand, wanted) ? wanted : function.at(operand).representation;
        };
        if (numberOfOperands <= maxOperandsComparedInPairs) {
            for (uint32_t later = 1; later < numberOfOperands; ++later) {
                ValueID operand = user.operand(later);
                for (uint32_t earlier = 0; operand && earlier < later; ++earlier) {
                    if (user.operand(earlier) == operand && formTakenAt(earlier) != formTakenAt(later))
                        return true;
                }
            }
            return false;
        }

        Vector<std::pair<ValueID, uint32_t>, 16> operands;
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            if (ValueID operand = user.operand(operandIndex))
                operands.append({ operand, operandIndex });
        }
        AOT::sort(operands);
        for (size_t index = 1; index < operands.size(); ++index) {
            if (operands[index - 1].first == operands[index].first && formTakenAt(operands[index - 1].second) != formTakenAt(operands[index].second))
                return true;
        }
        return false;
    }

    ValueID convert(ValueID source, Representation wanted, ProgramPoint before)
    {
        auto insert = [&](Value&& value, Representation representation) {
            ValueID made = builder.insertBefore(before, WTF::move(value));
            function.edit(made, ValueEdit::Type).representation = representation;
            ++numberOfConversions;
            return made;
        };
        const Type& type = pass.program.typeOf(function, source);
        if (std::optional<Value> constant = makeOnlyValueOf(type); constant && isLegalRepresentation(type, wanted))
            return insert(WTF::move(*constant), wanted);

        std::optional<ConversionPath> path = findConversion(type, function.at(source).representation, wanted);
        if (!path) {
            StringPrintStream detail;
            detail.print("a value"_s);
            detail.print(" kept as "_s, nameOf(function.at(source).representation), " is wanted as "_s, nameOf(wanted));
            pass.build.diagnostics().add(InternalError { .kind = InternalErrorKind::UnsupportedConversion, .function = pass.id, .block = before.block, .value = source, .detail = detail.toString() });
            return source;
        }
        for (ConversionStep step : *path)
            source = insert(IRBuilder::make(step.opcode, { source }), step.to);
        return source;
    }

    // A value that is made moves the others in memory: `user` finds the operation again, and `edit` gives it to write to.
    template<typename User, typename Edit>
    void convertOperandsOf(const User& user, ProgramPoint before, const Edit& edit)
    {
        if (!takesOneValueInTwoForms(user()))
            return;
        uint32_t numberOfOperands = user().numberOfOperands();
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID operand = user().operand(operandIndex);
            if (!operand)
                continue;
            Representation wanted = wantedRepresentationOf(inputs, user(), operandIndex);
            if (!needsConversion(operand, wanted))
                continue;
            hasEdited = true;
            builder.setOrigin(user().origin());
            ValueID converted = convert(operand, wanted, before);
            edit().setOperand(operandIndex, converted);
        }
    }

    void run(std::span<const BlockID> reversePostOrder)
    {
        RELEASE_ASSERT(&builder.function() == &pass.function);
        for (BlockID block : reversePostOrder) {
            const BlockValues& values = function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                ValueID user = values[index];
                convertOperandsOf([&]() -> const Value& {
                    return function.at(user);
                }, { block, index }, [&]() -> Value& {
                    return function.edit(user, ValueEdit::Definition);
                });
            }
            convertOperandsOf([&]() -> const Terminator& {
                return function.at(block).terminator;
            }, pointBeforeTerminatorOf(function, block), [&]() -> Terminator& {
                Terminator& terminator = function.edit(block, BlockEdit::Terminator).terminator;
                RELEASE_ASSERT(terminator.kind != TerminatorKind::TestAndBranch && terminator.kind != TerminatorKind::ReturnCall);
                return terminator;
            });
        }
        if (hasEdited)
            builder.applyInsertions();
    }
};

} // anonymous namespace

static void countRepresentations(FunctionPassContext& pass, uint32_t numberOfConversions)
{
    const Function& function = pass.function;
    std::array<uint64_t, numberOfEnumerators<Representation>> numberOfValuesIn { };
    auto countValue = [&](ValueID id) {
        ++numberOfValuesIn[std::to_underlying(function.at(id).representation)];
    };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            countValue(parameter);
        for (ValueID value : block.values)
            countValue(value);
    }

    auto count = [&](StatisticID statistic, uint64_t amount) {
        if (amount)
            pass.build.statistics().count(statistic, amount);
    };
    count(StatisticID::ConversionsInserted, numberOfConversions);
    count(StatisticID::ValuesInInt32, numberOfValuesIn[std::to_underlying(Representation::Int32)]);
    count(StatisticID::ValuesInInt64, numberOfValuesIn[std::to_underlying(Representation::Int64)]);
    count(StatisticID::ValuesInDouble, numberOfValuesIn[std::to_underlying(Representation::Double)] + numberOfValuesIn[std::to_underlying(Representation::DoubleWithPayload)] + numberOfValuesIn[std::to_underlying(Representation::ImpureDouble)]);
    count(StatisticID::ValuesBoxed, numberOfValuesIn[std::to_underlying(Representation::JSValue)]);
    count(StatisticID::ReferencesCompressed, numberOfValuesIn[std::to_underlying(Representation::Cell32)]);
}

PassOutcome chooseRepresentations(FunctionPassContext& pass)
{
    RELEASE_ASSERT(pass.inputs.placement && pass.inputs.callingConventions && !pass.function.representationsAreChosen);
    Function& function = pass.function;
    size_t numberOfBlocksBefore = function.blocks.size();

    IRBuilder builder(pass.analyses.context(), function, pass.types);
    uncoverEntryBlock(pass, builder);

    RepresentationInputs inputs { pass.program, pass.id, function, *pass.inputs.placement, *pass.inputs.callingConventions };
    writeRepresentations(pass, inputs);
    BitVector keptWithTheOneNaN;
    bool mustChooseAgain = false;
    if (findDoublesWithAnyNaN(pass, inputs, keptWithTheOneNaN)) {
        keptWithTheOneNaN = findDoublesKeptWithTheOneNaN(pass, inputs);
        mustChooseAgain = keepJoinsOfBytesAndArithmeticWithTheOneNaN(pass, inputs, keptWithTheOneNaN) || !keptWithTheOneNaN.isEmpty();
    }
    // What is computed from a join that is kept is bits of bytes no more, and may meet such bits at the next join.
    while (mustChooseAgain) {
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            if (holdsAnyNaN(function.at(ValueID(index)).representation))
                function.edit(ValueID(index), ValueEdit::Type).representation = Representation::Double;
        }
        findDoublesWithAnyNaN(pass, inputs, keptWithTheOneNaN);
        mustChooseAgain = keepJoinsOfBytesAndArithmeticWithTheOneNaN(pass, inputs, keptWithTheOneNaN);
    }

    ConversionInserter inserter { inputs, pass, builder };
    inserter.run(pass.analyses.cfg().reversePostOrder());

    // No analysis reads a representation, and only a block that is added changes an edge.
    if (function.blocks.size() != numberOfBlocksBefore)
        pass.analyses.didChangeControlFlow();
    else if (inserter.hasEdited) {
        pass.analyses.didChangeValues();
        pass.keptAnalyses = controlFlowAnalyses;
    } else
        pass.keptAnalyses = allAnalyses;

    function.representationsAreChosen = true;
    countRepresentations(pass, inserter.numberOfConversions);

    if (pass.build.options().verifiesProgram) {
        Vector<InternalError> errors;
        checkRepresentations(inputs, errors);
        if (!errors.isEmpty())
            pass.build.diagnostics().append(WTF::move(errors));
    }
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
