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
            size_t firstPairOfLayout = pairs.size();
            if (layout.kind == LayoutKind::Instance)
                m_isInstance.set(index);
            for (const Field& field : layout.fields) {
                if (field.key)
                    pairs.append(holder | field.key.index());
            }
            for (PropertyKeyID key : linked.appendedKeysOf(LayoutID(index)))
                pairs.append(holder | key.index());
            size_t steps = 0;
            for (LayoutID current(index); current && steps < numberOfLayouts; current = program.at(current).base, ++steps) {
                uint64_t first = static_cast<uint64_t>(current.index()) << 32;
                for (auto name = std::ranges::lower_bound(namesAbove, first); name != namesAbove.end() && *name >> 32 == current.index(); ++name)
                    pairs.append(holder | static_cast<uint32_t>(*name));
            }
            std::span<uint64_t> pairsOfLayout = pairs.mutableSpan().subspan(firstPairOfLayout);
            if (!std::ranges::is_sorted(pairsOfLayout))
                std::ranges::sort(pairsOfLayout);
            pairs.shrink(firstPairOfLayout + (std::ranges::unique(pairsOfLayout).begin() - pairsOfLayout.begin()));
        }

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
        m_holders.grow(pairs.size());
        for (uint64_t pair : pairs)
            m_holders[m_firstHolderOfName[static_cast<uint32_t>(pair)]++] = static_cast<uint32_t>(pair >> 32);
        for (size_t next = numberOfKeys; next; --next)
            m_firstHolderOfName[next] = m_firstHolderOfName[next - 1];
        m_firstHolderOfName[0] = 0;
    }

    LayoutID findOnlyLayoutWithAll(std::span<const PropertyKeyID> names, bool recordsAnswer) const
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
        return found && (recordsAnswer || m_isInstance.get(found.index())) ? found : LayoutID();
    }

    bool isNameOfSomeInstance(PropertyKeyID name) const { return m_isNameOfSomeInstance.get(name.index()); }
    bool isNameOfSomeLayout(PropertyKeyID name) const { return !holdersOf(name).empty(); }

    template<size_t inlineCapacity>
    void appendHolders(PropertyKeyID name, Vector<LayoutID, inlineCapacity>& found) const
    {
        for (uint32_t holder : holdersOf(name))
            found.append(LayoutID(holder));
    }

    bool isInUse(LayoutID layout) const { return layout.index() + 1 < m_firstNameOfLayout.size() && m_firstNameOfLayout[layout.index()] < m_firstNameOfLayout[layout.index() + 1]; }

    ALWAYS_INLINE bool hasAll(LayoutID layout, std::span<const PropertyKeyID> names) const
    {
        if (layout.index() + 1 >= m_firstNameOfLayout.size())
            return false;
        if (names.empty())
            return true;
        std::span<const uint32_t> namesOfLayout = m_names.span().subspan(m_firstNameOfLayout[layout.index()], m_firstNameOfLayout[layout.index() + 1] - m_firstNameOfLayout[layout.index()]);
        return std::ranges::all_of(names, [&](PropertyKeyID name) { return std::ranges::binary_search(namesOfLayout, name.index()); });
    }

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

    Vector<uint32_t> m_firstNameOfLayout;
    Vector<uint32_t> m_names;
    Vector<uint32_t> m_firstHolderOfName;
    Vector<uint32_t> m_holders;
    BitVector m_isInstance;
    BitVector m_isNameOfSomeInstance;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
