/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkOrderOfScopes.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static uint64_t keyForOrderOfScopes(uint32_t first, uint32_t second)
{
    return static_cast<uint64_t>(first) << 32 | second;
}

bool OrderOfScopes::isFilledWhenever(FunctionID runs, FieldReference field) const
{
    auto store = stores.find(keyForOrderOfScopes(field.layout.index(), field.field));
    if (store == stores.end())
        return false;
    if (store->value.lastBlockBelow == SummaryOrderOfScope::aboveAll)
        return true;
    auto found = entries.find(keyForOrderOfScopes(runs.index(), field.layout.index()));
    if (found == entries.end())
        return false;
    const Store& stored = store->value;
    const Entries& entered = found->value;
    if (entered.firstBlock < stored.block || entered.lastBlock > stored.lastBlockBelow)
        return false;
    return entered.firstBlock != stored.block || stored.position < entered.positionInFirstBlock;
}

namespace {

class FinderOfOrderOfScopes {
public:
    FinderOfOrderOfScopes(const Program& program, const ThinSummaries& summaries)
        : m_program(program)
        , m_summaries(summaries)
    {
    }

    OrderOfScopes find();

private:
    using Entry = SummaryOrderOfScope::Entry;
    using Hold = SummaryOrderOfScope::Hold;

    void follow(const SummaryOrderOfScope&);
    bool captures(FunctionID, LayoutID) const;
    // Each thing is visited once: with the first of the entries, in the order in which they are handed in, that leads to it.
    void visitFrom(const SummaryOrderOfScope&, std::span<const Entry>, const Invocable<void(FunctionID, const Entry&)> auto& functor);

    const Program& m_program;
    const ThinSummaries& m_summaries;
    OrderOfScopes m_order;
    BitVector m_layoutsSeen;
    BitVector m_layoutsGivenUp;
    // Or at a place of which the scan says nothing.
    BitVector m_layoutsMadeAtSeveralPlaces;
    Vector<Hold> m_holds;
    Vector<Entry> m_entries;
    Vector<uint32_t> m_toVisit;
    BitVector m_functionsVisited;
    BitVector m_slotsVisited;
    BitVector m_slotsThatHold;
    Vector<uint32_t> m_functionsToForget;
};

bool FinderOfOrderOfScopes::captures(FunctionID function, LayoutID layout) const
{
    return std::ranges::any_of(m_program.at(function).captures, [&](const DeclaredCapture& capture) {
        TypeID written = capture.written();
        return written && m_program.at(written).environment == layout;
    });
}

void FinderOfOrderOfScopes::visitFrom(const SummaryOrderOfScope& order, std::span<const Entry> entries, const Invocable<void(FunctionID, const Entry&)> auto& functor)
{
    m_slotsVisited.clearAll();
    for (uint32_t function : m_functionsToForget)
        m_functionsVisited.quickClear(function);
    m_functionsToForget.shrink(0);
    for (const Entry& entry : entries) {
        m_toVisit.append(entry.thing);
        while (!m_toVisit.isEmpty()) {
            uint32_t thing = m_toVisit.takeLast();
            auto [first, end] = std::ranges::equal_range(m_holds, thing, { }, &Hold::holder);
            if (thing & SummaryOrderOfScope::slotBit) {
                if (m_slotsVisited.set(thing & ~SummaryOrderOfScope::slotBit))
                    continue;
                for (const Hold& hold : std::span(first, end))
                    m_toVisit.append(hold.held);
                continue;
            }
            FunctionID function(thing);
            if (m_functionsVisited.quickSet(thing))
                continue;
            m_functionsToForget.append(thing);
            // `const read = () => limit; const start = () => [0].map(read)`: `start` takes the closure by value and captures no scope.
            for (const Hold& hold : std::span(first, end))
                m_toVisit.append(hold.held);
            // What does not capture the scope reads none of its slots, and gets at nothing more that does but through a thing which has left.
            if (!captures(function, order.layout))
                continue;
            functor(function, entry);
            const ThinSummary& summary = m_summaries.at(function);
            // The text names whatever stands around the call.
            if (summary.traits.contains(SummaryTrait::HasDirectEval))
                m_layoutsGivenUp.set(order.layout.index());
            for (FunctionID named : summary.functionsNamed)
                m_toVisit.append(named.index());
            for (const SummaryNode& node : summary.nodes) {
                if (node.kind == SummaryNode::Kind::Field && node.first == order.layout.index() && m_slotsThatHold.get(node.second))
                    m_toVisit.append(SummaryOrderOfScope::slotBit | node.second);
            }
        }
    }
}

void FinderOfOrderOfScopes::follow(const SummaryOrderOfScope& order)
{
    // One place makes the scope, or nothing is said.
    if (m_layoutsSeen.set(order.layout.index()) || order.isGivenUp) {
        m_layoutsMadeAtSeveralPlaces.set(order.layout.index());
        return;
    }
    for (const SummaryOrderOfScope::Store& store : order.stores)
        m_order.stores.set(keyForOrderOfScopes(order.layout.index(), store.field), OrderOfScopes::Store { store.block, store.lastBlockBelow, store.position });
    if (order.entries.isEmpty())
        return;
    m_holds = order.holds;
    std::ranges::sort(m_holds, { }, &Hold::holder);
    m_slotsThatHold.clearAll();
    for (const Hold& hold : m_holds) {
        // What lies there is not known.
        if (hold.held == SummaryOrderOfScope::anyone || hold.holder == SummaryOrderOfScope::anyone) {
            m_layoutsGivenUp.set(order.layout.index());
            return;
        }
        if (hold.holder & SummaryOrderOfScope::slotBit)
            m_slotsThatHold.set(hold.holder & ~SummaryOrderOfScope::slotBit);
    }

    // Where the scope itself has gone, everybody over it may be entered.
    m_entries.shrink(0);
    std::optional<Entry> firstOfAnyone;
    uint32_t lastBlockOfAnyone = 0;
    auto standsInFront = [](const Entry& first, const Entry& second) {
        return std::tie(first.block, first.position) < std::tie(second.block, second.position);
    };
    for (const Entry& entry : order.entries) {
        if (entry.thing != SummaryOrderOfScope::anyone) {
            m_entries.append(entry);
            continue;
        }
        if (!firstOfAnyone || standsInFront(entry, *firstOfAnyone))
            firstOfAnyone = entry;
        lastBlockOfAnyone = std::max(lastBlockOfAnyone, entry.block);
    }

    std::ranges::sort(m_entries, standsInFront);
    visitFrom(order, m_entries.span(), [&](FunctionID function, const Entry& entry) {
        m_order.entries.set(keyForOrderOfScopes(function.index(), order.layout.index()), OrderOfScopes::Entries { entry.block, entry.position, entry.block });
    });
    std::ranges::sort(m_entries, std::ranges::greater { }, &Entry::block);
    visitFrom(order, m_entries.span(), [&](FunctionID function, const Entry& entry) {
        auto found = m_order.entries.find(keyForOrderOfScopes(function.index(), order.layout.index()));
        RELEASE_ASSERT(found != m_order.entries.end());
        OrderOfScopes::Entries& entered = found->value;
        entered.lastBlock = entry.block;
        if (!firstOfAnyone)
            return;
        if (std::tie(firstOfAnyone->block, firstOfAnyone->position) < std::tie(entered.firstBlock, entered.positionInFirstBlock)) {
            entered.firstBlock = firstOfAnyone->block;
            entered.positionInFirstBlock = firstOfAnyone->position;
        }
        entered.lastBlock = std::max(entered.lastBlock, lastBlockOfAnyone);
    });
}

OrderOfScopes FinderOfOrderOfScopes::find()
{
    m_functionsVisited.ensureSize(m_program.functions.size());
    // A function with more captures than fit takes them in one pack, a scope whose slots hold them: it is over a scope that none of its captures is written to be.
    for (const Layout& layout : m_program.layouts) {
        if (!layout.isEnvironment())
            continue;
        for (const Field& field : layout.fields) {
            TypeID written = field.declared.written();
            if (LayoutID held = written ? m_program.at(written).environment : LayoutID())
                m_layoutsGivenUp.set(held.index());
        }
    }
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        for (const SummaryOrderOfScope& order : m_summaries.at(FunctionID(index)).ordersOfScopes)
            follow(order);
    }
    // What the scan has found by itself hangs on none of this, but for the one place.
    m_order.stores.removeIf([&](const auto& store) {
        uint32_t layout = static_cast<uint32_t>(store.key >> 32);
        return m_layoutsMadeAtSeveralPlaces.get(layout) || (m_layoutsGivenUp.get(layout) && store.value.lastBlockBelow != SummaryOrderOfScope::aboveAll);
    });
    return WTF::move(m_order);
}

} // anonymous namespace

OrderOfScopes findOrderOfScopes(const Program& program, const ThinSummaries& summaries)
{
    return FinderOfOrderOfScopes(program, summaries).find();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
