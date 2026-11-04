/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// WHICH LAYOUTS HAVE A NAME. `function area(o) { return o.width * o.height; }`: nothing is written of `o` and nothing was seen to arrive at it. But whatever arrives is asked for `width` and for `height`,
// and of the layouts of which the program makes objects one alone may have both. That one is expected. It is a guess of the weakest kind: the walk tests it at the use, so any answer is safe.
// It intersects where Link's flow can only join: the more names a body uses off a value, the fewer layouts are left.
// A layout has a name as a field, as a field that Link appended, as a member of its class or of a class above it, or as what is laid on the prototype object of its constructor function. Every name is like every other.
class LayoutsByNames {
public:
    LayoutsByNames() = default;

    LayoutsByNames(const Program& program, const LinkTables& linked)
    {
        size_t numberOfLayouts = program.layouts.size();
        size_t numberOfKeys = program.types.numberOfPropertyKeys();

        Vector<uint64_t> namesAbove;
        for (const LinkedMember& member : linked.members) {
            if (member.key && member.side == ClassSide::Prototype)
                namesAbove.append(static_cast<uint64_t>(member.classLayout.index()) << 32 | member.key.index());
        }
        for (const LinkedNameOnPrototype& name : linked.namesOnPrototypes) {
            if (name.key)
                namesAbove.append(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.index());
        }
        std::ranges::sort(namesAbove);

        Vector<uint64_t> pairs;
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            const Layout& layout = program.layouts[index];
            if ((layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance) || index >= linked.layouts.size() || !linked.layouts[index].isAllocated)
                continue;
            uint64_t holder = static_cast<uint64_t>(index) << 32;
            if (layout.kind == LayoutKind::Instance)
                m_isInstance.set(index);
            for (const Field& field : layout.fields) {
                if (field.key)
                    pairs.append(holder | field.key.index());
            }
            for (PropertyKeyID key : linked.appendedKeysOf(LayoutID(index)))
                pairs.append(holder | key.index());
            // `class A extends B {}` beside `class B extends A {}` compiles, so the row may be a ring.
            size_t steps = 0;
            for (LayoutID current(index); current && steps < numberOfLayouts; current = program.at(current).base, ++steps) {
                uint64_t first = static_cast<uint64_t>(current.index()) << 32;
                for (auto name = std::ranges::lower_bound(namesAbove, first); name != namesAbove.end() && *name >> 32 == current.index(); ++name)
                    pairs.append(holder | static_cast<uint32_t>(*name));
            }
        }
        std::ranges::sort(pairs);
        pairs.shrink(std::ranges::unique(pairs).begin() - pairs.begin());

        m_firstNameOfLayout.fill(0, numberOfLayouts + 1);
        m_firstHolderOfName.fill(0, numberOfKeys + 1);
        m_names.reserveInitialCapacity(pairs.size());
        for (uint64_t pair : pairs) {
            ++m_firstNameOfLayout[(pair >> 32) + 1];
            ++m_firstHolderOfName[static_cast<uint32_t>(pair) + 1];
            m_names.append(static_cast<uint32_t>(pair));
            if (m_isInstance.get(pair >> 32))
                m_isNameOfSomeInstance.set(static_cast<uint32_t>(pair));
        }
        for (size_t next = 1; next <= numberOfLayouts; ++next)
            m_firstNameOfLayout[next] += m_firstNameOfLayout[next - 1];
        for (size_t next = 1; next <= numberOfKeys; ++next)
            m_firstHolderOfName[next] += m_firstHolderOfName[next - 1];
        // Each list is filled from its begin, which moves to its end, the begin of the next.
        m_holders.grow(pairs.size());
        for (uint64_t pair : pairs)
            m_holders[m_firstHolderOfName[static_cast<uint32_t>(pair)]++] = static_cast<uint32_t>(pair >> 32);
        for (size_t next = numberOfKeys; next; --next)
            m_firstHolderOfName[next] = m_firstHolderOfName[next - 1];
        m_firstHolderOfName[0] = 0;
    }

    // Null: no layout in use has all the names, or several have.
    LayoutID findOnlyLayoutWithAll(std::span<const PropertyKeyID> names) const
    {
        if (names.empty() || m_firstHolderOfName.isEmpty())
            return { };
        std::span<const uint32_t> fewest = holdersOf(names[0]);
        for (PropertyKeyID name : names.subspan(1)) {
            if (std::span<const uint32_t> holders = holdersOf(name); holders.size() < fewest.size())
                fewest = holders;
        }
        LayoutID found;
        for (uint32_t holder : fewest) {
            if (!std::ranges::all_of(names, [&](PropertyKeyID name) { return has(holder, name); }))
                continue;
            if (found)
                return { };
            found = LayoutID(holder);
        }
        // The names of an instance come from its constructor alone. Those of a record say nothing of who made it: any literal, a parse or a spread makes an object with the same names.
        return found && m_isInstance.get(found.index()) ? found : LayoutID();
    }

    // False: findOnlyLayoutWithAll() finds nothing for any names that this one is among: what it finds is an instance that has them all.
    bool isNameOfSomeInstance(PropertyKeyID name) const { return m_isNameOfSomeInstance.get(name.index()); }

    bool isInUse(LayoutID layout) const { return layout.index() + 1 < m_firstNameOfLayout.size() && m_firstNameOfLayout[layout.index()] < m_firstNameOfLayout[layout.index() + 1]; }

    // A name that no layout in use has says nothing about layouts.
    bool isHeldBySomeLayout(PropertyKeyID name) const { return !holdersOf(name).empty(); }

    bool hasAll(LayoutID layout, std::span<const PropertyKeyID> names) const
    {
        return layout.index() + 1 < m_firstNameOfLayout.size() && std::ranges::all_of(names, [&](PropertyKeyID name) { return has(layout.index(), name); });
    }

    // Of the layouts listed, which ascend: the one in use that has the name. Null: none has it, or several do. It goes by the shorter list and ends at the second holder.
    LayoutID findOnlyHolderAmong(PropertyKeyID name, std::span<const LayoutID> ascending) const
    {
        LayoutID only;
        std::span<const uint32_t> holders = holdersOf(name);
        if (holders.size() < ascending.size()) {
            for (uint32_t holder : holders) {
                if (!std::ranges::binary_search(ascending, holder, { }, [](LayoutID layout) { return layout.index(); }))
                    continue;
                if (only)
                    return { };
                only = LayoutID(holder);
            }
            return only;
        }
        for (LayoutID layout : ascending) {
            if (!isInUse(layout) || !has(layout.index(), name))
                continue;
            if (only)
                return { };
            only = layout;
        }
        return only;
    }

    // Of the layouts listed, which ascend: all those in use that have the name, ascending. It goes by the shorter list.
    template<size_t inlineCapacity>
    void appendHoldersAmong(PropertyKeyID name, std::span<const LayoutID> ascending, Vector<LayoutID, inlineCapacity>& found) const
    {
        std::span<const uint32_t> holders = holdersOf(name);
        if (holders.size() < ascending.size()) {
            for (uint32_t holder : holders) {
                if (std::ranges::binary_search(ascending, holder, { }, [](LayoutID layout) { return layout.index(); }))
                    found.append(LayoutID(holder));
            }
            return;
        }
        for (LayoutID layout : ascending) {
            if (isInUse(layout) && has(layout.index(), name))
                found.append(layout);
        }
    }

private:
    std::span<const uint32_t> holdersOf(PropertyKeyID name) const
    {
        if (name.index() + 1 >= m_firstHolderOfName.size())
            return { };
        return m_holders.span().subspan(m_firstHolderOfName[name.index()], m_firstHolderOfName[name.index() + 1] - m_firstHolderOfName[name.index()]);
    }

    bool has(uint32_t layout, PropertyKeyID name) const
    {
        return std::ranges::binary_search(m_names.span().subspan(m_firstNameOfLayout[layout], m_firstNameOfLayout[layout + 1] - m_firstNameOfLayout[layout]), name.index());
    }

    // By layout, and one more. The names of a layout ascend.
    Vector<uint32_t> m_firstNameOfLayout;
    Vector<uint32_t> m_names;
    // By name, and one more. The holders of a name ascend.
    Vector<uint32_t> m_firstHolderOfName;
    Vector<uint32_t> m_holders;
    BitVector m_isInstance;
    // By name.
    BitVector m_isNameOfSomeInstance;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
