/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationWebs.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

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

template<typename Predicate>
static RepresentationSet subsetOf(RepresentationSet candidates, const Predicate& isMember)
{
    RepresentationSet result;
    for (Representation candidate : allMembersOf<Representation>) {
        if (candidates.contains(candidate) && isMember(candidate))
            result.add(candidate);
    }
    return result;
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
        if (useLists.locationOf(id).placement != ValuePlacement::Detached)
            m_fixed[index] = resultRepresentationOf(m_inputs, m_inputs.function.at(id));
    }
}

void RepresentationWebs::build()
{
    uniteCopies();
    numberWebs();
    findWhereCollectorMayHaveRun();
    restrictToLegalRepresentations();
    collectProducers();
    collectUses();
}

void RepresentationWebs::choose()
{
    for (Web& web : m_webs)
        web.chosen = chooseFor(web);
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
        bool picks = m_fixed[index] == Representation::Unchosen && useLists.locationOf(ValueID(index)).placement != ValuePlacement::Detached;
        if (!picks || find(index) != index)
            continue;
        m_webOfRoot[index] = static_cast<uint32_t>(m_webs.size());
        m_webs.append(Web { });
    }
}

void RepresentationWebs::restrictToLegalRepresentations()
{
    const Function& function = m_inputs.function;
    auto forEachMember = [&](const auto& functor) {
        for (uint32_t index = 0; index < m_fixed.size(); ++index) {
            if (std::optional<uint32_t> web = m_webOfRoot[find(index)])
                functor(ValueID(index), m_webs[*web]);
        }
    };
    auto canConvertTo = [&](ValueID operand, Representation candidate) {
        if (!hasRegister(candidate))
            return m_fixed[operand.index()] == candidate;
        return isLegalRepresentation(m_inputs.typeOf(operand), candidate);
    };

    for (Web& web : m_webs)
        web.legal = webRepresentations | RepresentationSet { Representation::Cell32 };

    forEachMember([&](ValueID member, Web& web) {
        bool staysNarrow = mayStayNarrow(member);
        web.legal = subsetOf(web.legal, [&](Representation candidate) {
            if (candidate == Representation::Cell32 && !staysNarrow)
                return false;
            return isLegalRepresentation(m_inputs.typeOf(member), candidate);
        });
        function.at(member).forEachChild([&](ValueID operand) {
            web.legal = subsetOf(web.legal, [&](Representation candidate) {
                return canConvertTo(operand, candidate);
            });
        });
    });

    forEachMember([&](ValueID member, Web& web) {
        auto tryDefaultOf = [&](ValueID id) {
            if (web.preferred != Representation::Unchosen)
                return;
            Representation representation = defaultRepresentationOf(m_inputs.typeOf(id));
            if (web.legal.contains(representation))
                web.preferred = representation;
        };
        tryDefaultOf(member);
        function.at(member).forEachChild(tryDefaultOf);
    });

    for (Web& web : m_webs) {
        if (web.preferred == Representation::Unchosen)
            web.preferred = Representation::JSValue;
        RELEASE_ASSERT(web.legal.contains(web.preferred));
    }
}

// For every block with a parameter that a narrow reference fits: at the head of which blocks the collector may have run since that block was entered last.
void RepresentationWebs::findWhereCollectorMayHaveRun()
{
    const Function& function = m_inputs.function;
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();

    m_firstCollectingValue.fill(0, function.blocks.size());
    for (BlockID block : cfg.reversePostOrder()) {
        const auto& values = function.at(block).values;
        auto first = std::ranges::find_if(values, [&](ValueID id) {
            return mayCollect(function.at(id).opcode);
        });
        m_firstCollectingValue[block.index()] = static_cast<uint32_t>(first - values.begin());
    }

    uint32_t numberOfBlocksWithReference = 0;
    m_indexOfBlockWithReference.fill(std::nullopt, function.blocks.size());
    for (uint32_t index = 0; index < m_fixed.size(); ++index) {
        ValueLocation location = useLists.locationOf(ValueID(index));
        if (location.placement != ValuePlacement::Parameter || !m_webOfRoot[find(index)] || !isLegalRepresentation(m_inputs.typeOf(ValueID(index)), Representation::Cell32))
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

// A narrow reference lives from a load to its Decode. A proof or a test on the way leaves the Decode fewer cases, so it stays narrow through a proof and into the blocks behind a jump or a branch.
// Where the collector can run, a pointer names the cell: the reference is decoded before.
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
        return m_inputs.function.at(member).opcode == Opcode::AssumeType;
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

    auto addCopyOf = [&](ValueID source, ValueID member, BlockID block) {
        uint32_t root = find(source.index());
        if (root == find(member.index()))
            return;
        std::optional<uint32_t> web = m_webOfRoot[root];
        addProducer(member, web ? m_webs[*web].preferred : m_fixed[source.index()], block);
    };

    auto addIncomingValue = [&](ValueID parameter, const IncomingValue& incoming, BlockID from) {
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            addCopyOf(incoming.argument, parameter, from);
            return;
        case IncomingValue::Kind::EdgeResult:
            break;
        }

        EdgeResult result = edgeResultAt(function, incoming.result);
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand:
            addCopyOf(result.subject, parameter, from);
            return;
        case EdgeResultKind::Guard:
            addProducer(parameter, Representation::Void, from);
            return;
        case EdgeResultKind::DeclaredResult:
            addProducer(parameter, edgeResultRepresentationOf(m_inputs, incoming.result), from);
            return;
        }
    };

    for (uint32_t index = 0; index < m_fixed.size(); ++index) {
        if (!m_webOfRoot[find(index)])
            continue;
        ValueID member(index);
        ValueLocation location = useLists.locationOf(member);
        switch (location.placement) {
        case ValuePlacement::Parameter:
            for (Edge edge : cfg.incomingEdges(location.block))
                addIncomingValue(member, incomingValueOf(function, edge, location.index), edge.from);
            break;
        case ValuePlacement::InBlock:
            function.at(member).forEachChild([&](ValueID operand) {
                addCopyOf(operand, member, location.block);
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

    auto addCopyTo = [&](ValueID destination, ValueID member, BlockID block) {
        uint32_t root = find(destination.index());
        if (root != find(member.index()))
            addUse(member, m_webs[*m_webOfRoot[root]].preferred, block);
    };

    for (uint32_t index = 0; index < m_fixed.size(); ++index) {
        if (!m_webOfRoot[find(index)])
            continue;
        ValueID member(index);
        for (const Use& use : useLists.usesOf(member)) {
            switch (use.kind) {
            case Use::Kind::Operand:
                if (m_fixed[use.user.index()] == Representation::Unchosen)
                    addCopyTo(use.user, member, use.block);
                else
                    addUse(member, wantedRepresentationOf(m_inputs, function.at(use.user), use.operandIndex), use.block);
                break;
            case Use::Kind::TerminatorOperand:
                addUse(member, wantedRepresentationOf(m_inputs, function.at(use.block).terminator, use.operandIndex), use.block);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
                addCopyTo(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex], member, use.block);
                break;
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
}

// A conversion costs where it runs: one in a block that is seldom entered decides only between choices that are level elsewhere.
uint32_t RepresentationWebs::votesOf(BlockID block) const
{
    switch (m_analyses.blockFrequencies().classOf(block)) {
    case BlockFrequency::Normal:
        return 64;
    case BlockFrequency::Unlikely:
    case BlockFrequency::Rare:
        return 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RepresentationWebs::addProducer(ValueID member, Representation representation, BlockID block)
{
    ASSERT(representation != Representation::Unchosen);
    m_webs[*m_webOfRoot[find(member.index())]].produced[std::to_underlying(representation)] += votesOf(block);
}

void RepresentationWebs::addUse(ValueID member, Representation representation, BlockID block)
{
    if (hasRegister(representation))
        m_webs[*m_webOfRoot[find(member.index())]].wanted[std::to_underlying(representation)] += votesOf(block);
}

uint32_t RepresentationWebs::conversionsNeededBy(const Web& web, Representation representation)
{
    uint32_t numberOfSites = 0;
    for (size_t index = 0; index < web.produced.size(); ++index)
        numberOfSites += web.produced[index] + web.wanted[index];
    return numberOfSites - web.produced[std::to_underlying(representation)] - web.wanted[std::to_underlying(representation)];
}

Representation RepresentationWebs::chooseFor(const Web& web) const
{
    Representation best = web.preferred;
    uint32_t fewest = conversionsNeededBy(web, best);
    for (Representation candidate : allMembersOf<Representation>) {
        if (!web.legal.contains(candidate))
            continue;
        uint32_t needed = conversionsNeededBy(web, candidate);
        if (needed < fewest) {
            best = candidate;
            fewest = needed;
        }
    }
    return best;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
