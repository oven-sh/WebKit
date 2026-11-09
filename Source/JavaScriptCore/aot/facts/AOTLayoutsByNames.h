/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class LayoutsByNames {
public:
    LayoutsByNames() = default;

    LayoutsByNames(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held)
    {
        size_t numberOfLayouts = program.layouts.size();
        listWhoWritesLayouts(program, summaries);
        size_t numberOfKeys = program.types.numberOfPropertyKeys();

        Vector<uint64_t> namesAbove;
        for (const LinkedMember& member : linked.members) {
            if (member.key && member.classLayout && member.side == ClassSide::Prototype)
                namesAbove.append(static_cast<uint64_t>(member.classLayout.index()) << 32 | member.key.index());
        }
        for (const LinkedNameOnPrototype& name : linked.namesOnPrototypes) {
            if (name.key && name.layout)
                namesAbove.append(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.index());
        }
        std::ranges::sort(namesAbove);
        Vector<uint32_t> firstNameAboveOfLayout;
        firstNameAboveOfLayout.fill(0, numberOfLayouts + 1);
        for (uint64_t name : namesAbove)
            ++firstNameAboveOfLayout[(name >> 32) + 1];
        for (size_t next = 1; next <= numberOfLayouts; ++next)
            firstNameAboveOfLayout[next] += firstNameAboveOfLayout[next - 1];
        auto putInOrder = [](std::span<uint64_t> run) {
            if (std::ranges::is_sorted(run))
                return;
            if (std::ranges::is_sorted(run, std::ranges::greater { }))
                std::ranges::reverse(run);
            else
                std::ranges::sort(run);
        };

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
            size_t firstPairAbove = pairs.size();
            size_t steps = 0;
            auto above = [&](LayoutID current) {
                const Layout& below = program.at(current);
                bool parentIsValue = !below.base && below.has(LayoutFlags::HasHeritage) && current.index() < held.layouts.size();
                return parentIsValue ? held.layouts[current.index()].heritage : below.base;
            };
            for (LayoutID current(index); current && steps < numberOfLayouts; current = above(current), ++steps) {
                for (uint64_t name : namesAbove.span().subspan(firstNameAboveOfLayout[current.index()], firstNameAboveOfLayout[current.index() + 1] - firstNameAboveOfLayout[current.index()]))
                    pairs.append(holder | static_cast<uint32_t>(name));
            }
            std::span<uint64_t> pairsOfLayout = pairs.mutableSpan().subspan(firstPairOfLayout);
            putInOrder(pairsOfLayout.first(firstPairAbove - firstPairOfLayout));
            putInOrder(pairsOfLayout.subspan(firstPairAbove - firstPairOfLayout));
            std::ranges::inplace_merge(pairsOfLayout, pairsOfLayout.begin() + (firstPairAbove - firstPairOfLayout));
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
        listRecordsThatDeclaredTypesName(program);
    }

    static LayoutsByNames thatListsWritersAlone(const Program& program, const ThinSummaries& summaries)
    {
        LayoutsByNames lists;
        lists.listWhoWritesLayouts(program, summaries);
        return lists;
    }

    std::span<const uint32_t> holdersOfRarestOf(std::span<const PropertyKeyID> names) const
    {
        if (names.empty() || m_firstHolderOfName.isEmpty())
            return { };
        std::span<const uint32_t> fewest = holdersOf(names[0]);
        for (PropertyKeyID name : names.subspan(1)) {
            if (fewest.empty())
                break;
            if (std::span<const uint32_t> holders = holdersOf(name); holders.size() < fewest.size())
                fewest = holders;
        }
        return fewest;
    }

    bool hasAll(uint32_t holder, std::span<const PropertyKeyID> names) const
    {
        return std::ranges::all_of(names, [&](PropertyKeyID name) { return has(holder, name); });
    }

    bool isInstance(LayoutID layout) const { return m_isInstance.get(layout.index()); }

    std::span<const uint32_t> layoutsWrittenIn(ModuleID module) const
    {
        return rowOf(m_firstLayoutWrittenIn, m_layoutsWritten, module.index());
    }

    std::span<const uint32_t> modulesSeenBy(ModuleID module) const { return rowOf(m_firstModuleSeenBy, m_modulesSeen, module.index()); }
    size_t numberWrittenInSightOf(ModuleID module) const { return module.index() < m_numberWrittenInSight.size() ? m_numberWrittenInSight[module.index()] : 0; }
    bool isWrittenInSightOf(ModuleID module, uint32_t layout) const { return isWrittenInOneOf(modulesSeenBy(module), layout); }
    struct RecordOfDeclaredParameter {
        uint64_t function { 0 };
        uint32_t parameter { 0 };
        LayoutID record;
        auto place() const { return std::pair(function, parameter); }
    };
    std::span<const RecordOfDeclaredParameter> recordsOfDeclaredParameter(ModuleID module, uint32_t positionOfFunction, uint32_t parameter) const
    {
        auto found = std::ranges::equal_range(m_recordsOfDeclaredParameters, std::pair(static_cast<uint64_t>(module.index()) << 32 | positionOfFunction, parameter), { }, &RecordOfDeclaredParameter::place);
        return { found.begin(), found.end() };
    }

    bool isWrittenInOneOf(std::span<const uint32_t> modules, uint32_t layout) const
    {
        std::span<const uint32_t> writers = rowOf(m_firstWriterOfLayout, m_writersOfLayouts, layout);
        std::span<const uint32_t> fewer = modules.size() < writers.size() ? modules : writers;
        std::span<const uint32_t> more = modules.size() < writers.size() ? writers : modules;
        return std::ranges::any_of(fewer, [&](uint32_t member) { return std::ranges::binary_search(more, member); });
    }

    bool isNameOfSomeInstance(PropertyKeyID name) const { return m_isNameOfSomeInstance.get(name.index()); }
    bool isNameOfSomeLayout(PropertyKeyID name) const { return !holdersOf(name).empty(); }
    bool severalLayoutsHold(PropertyKeyID name) const { return holdersOf(name).size() > 1; }

    bool isInUse(LayoutID layout) const { return static_cast<size_t>(layout.index()) + 1 < m_firstNameOfLayout.size() && m_firstNameOfLayout[layout.index()] < m_firstNameOfLayout[layout.index() + 1]; }

    ALWAYS_INLINE bool hasAll(LayoutID layout, std::span<const PropertyKeyID> names) const
    {
        if (static_cast<size_t>(layout.index()) + 1 >= m_firstNameOfLayout.size())
            return false;
        if (names.empty())
            return true;
        std::span<const uint32_t> namesOfLayout = m_names.span().subspan(m_firstNameOfLayout[layout.index()], m_firstNameOfLayout[layout.index() + 1] - m_firstNameOfLayout[layout.index()]);
        return std::ranges::all_of(names, [&](PropertyKeyID name) { return std::ranges::binary_search(namesOfLayout, name.index()); });
    }

    bool hasAnyOf(LayoutID layout, std::span<const PropertyKeyID> ascending) const
    {
        if (static_cast<size_t>(layout.index()) + 1 >= m_firstNameOfLayout.size())
            return false;
        std::span<const uint32_t> namesOfLayout = m_names.span().subspan(m_firstNameOfLayout[layout.index()], m_firstNameOfLayout[layout.index() + 1] - m_firstNameOfLayout[layout.index()]);
        if (namesOfLayout.size() < ascending.size())
            return std::ranges::any_of(namesOfLayout, [&](uint32_t name) { return std::ranges::binary_search(ascending, name, { }, &PropertyKeyID::index); });
        return std::ranges::any_of(ascending, [&](PropertyKeyID name) { return std::ranges::binary_search(namesOfLayout, name.index()); });
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
    bool appendHoldersAmongUnlessMoreThan(size_t most, PropertyKeyID name, std::span<const LayoutID> ascending, Vector<LayoutID, inlineCapacity>& found) const
    {
        std::span<const uint32_t> holders = holdersOf(name);
        if (holders.size() < ascending.size()) {
            for (uint32_t holder : holders) {
                if (!std::ranges::binary_search(ascending, holder, { }, [](LayoutID layout) { return layout.index(); }))
                    continue;
                if (found.size() == most)
                    return false;
                found.append(LayoutID(holder));
            }
            return true;
        }
        for (LayoutID layout : ascending) {
            if (!isInUse(layout) || !has(layout.index(), name))
                continue;
            if (found.size() == most)
                return false;
            found.append(layout);
        }
        return true;
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
    void listWhoWritesLayouts(const Program& program, const ThinSummaries& summaries)
    {
        Vector<uint64_t> written;
        for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
            if (ModuleID module = program.layouts[layout].module; module && module.index() < program.modules.size())
                written.append(static_cast<uint64_t>(module.index()) << 32 | layout);
        }
        for (uint32_t function = 0; function < summaries.size() && function < program.functions.size(); ++function) {
            ModuleID module = program.functions[function].module;
            for (const SummaryNode& node : summaries.at(FunctionID(function)).nodes.span()) {
                if (module && module.index() < program.modules.size() && node.kind == SummaryNode::Kind::Allocation && program.at(LayoutID(node.first)).kind == LayoutKind::Record)
                    written.append(static_cast<uint64_t>(module.index()) << 32 | node.first);
            }
        }
        layRows(written, program.modules.size(), m_firstLayoutWrittenIn, m_layoutsWritten);

        for (uint64_t& pair : written)
            pair = pair << 32 | pair >> 32;
        layRows(written, program.layouts.size(), m_firstWriterOfLayout, m_writersOfLayouts);

        Vector<uint64_t> seen;
        auto see = [&](uint32_t module, ModuleID other) {
            if (module < program.modules.size() && other && other.index() < program.modules.size())
                seen.append(static_cast<uint64_t>(module) << 32 | other.index());
        };
        for (uint32_t module = 0; module < program.modules.size(); ++module) {
            see(module, ModuleID(module));
            for (const ModuleRequest& request : program.modules[module].requests)
                see(module, request.target);
            for (const RequiredName& required : program.modules[module].requiredNames)
                see(module, required.module);
        }
        layRows(seen, program.modules.size(), m_firstModuleSeenBy, m_modulesSeen);

        m_numberWrittenInSight.fill(0, program.modules.size());
        for (uint64_t pair : seen)
            m_numberWrittenInSight[pair >> 32] += layoutsWrittenIn(ModuleID(static_cast<uint32_t>(pair))).size();
    }

    static uint64_t wordOfName(PropertyKeyID name)
    {
        uint64_t word = (static_cast<uint64_t>(name.index()) + 1) * 0x9e3779b97f4a7c15ull;
        return word ^ (word >> 29);
    }

    void listRecordsThatDeclaredTypesName(const Program& program)
    {
        struct RecordByNames {
            uint64_t wordOfNames;
            uint32_t holder;
            auto operator<=>(const RecordByNames&) const = default;
        };
        Vector<RecordByNames> recordsByNames;
        for (uint32_t holder = 0; !program.declaredRecords.isEmpty() && holder + 1 < m_firstNameOfLayout.size(); ++holder) {
            const auto& written = program.layouts[holder].fields;
            if (m_isInstance.get(holder) || m_firstNameOfLayout[holder] == m_firstNameOfLayout[holder + 1] || !std::ranges::all_of(written, [](const Field& field) { return !!field.key; }))
                continue;
            uint64_t wordOfNames = 0;
            for (const Field& field : written)
                wordOfNames += wordOfName(field.key);
            recordsByNames.append({ wordOfNames, holder });
        }
        std::ranges::sort(recordsByNames);
        for (const DeclaredRecord& declared : program.declaredRecords) {
            uint64_t function = static_cast<uint64_t>(declared.module.index()) << 32 | declared.position;
            m_recordsOfDeclaredParameters.append({ function, declared.parameter, LayoutID() });
            auto isDeclared = [&](const Field& field) { return std::ranges::binary_search(declared.names, field.key); };
            uint64_t wordOfNames = 0;
            for (PropertyKeyID name : declared.names)
                wordOfNames += wordOfName(name);
            for (const RecordByNames& found : std::ranges::equal_range(recordsByNames, wordOfNames, { }, &RecordByNames::wordOfNames)) {
                const auto& written = program.layouts[found.holder].fields;
                if (written.size() == declared.names.size() && std::ranges::all_of(written, isDeclared))
                    m_recordsOfDeclaredParameters.append({ function, declared.parameter, LayoutID(found.holder) });
            }
        }
        AOT::stableSort(m_recordsOfDeclaredParameters, { }, &RecordOfDeclaredParameter::place);
    }

    static void layRows(Vector<uint64_t>& pairs, size_t rowCount, Vector<uint32_t>& firstOfRow, Vector<uint32_t>& members)
    {
        std::ranges::sort(pairs);
        pairs.shrink(std::ranges::unique(pairs).begin() - pairs.begin());
        firstOfRow.fill(0, rowCount + 1);
        members.reserveInitialCapacity(pairs.size());
        for (uint64_t pair : pairs) {
            ++firstOfRow[(pair >> 32) + 1];
            members.append(static_cast<uint32_t>(pair));
        }
        for (size_t next = 1; next <= rowCount; ++next)
            firstOfRow[next] += firstOfRow[next - 1];
    }

    static std::span<const uint32_t> rowOf(const Vector<uint32_t>& firstOfRow, const Vector<uint32_t>& members, uint32_t row)
    {
        if (static_cast<size_t>(row) + 1 >= firstOfRow.size())
            return { };
        return members.span().subspan(firstOfRow[row], firstOfRow[row + 1] - firstOfRow[row]);
    }

    std::span<const uint32_t> holdersOf(PropertyKeyID name) const
    {
        if (static_cast<size_t>(name.index()) + 1 >= m_firstHolderOfName.size())
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
    Vector<uint32_t> m_firstLayoutWrittenIn;
    Vector<uint32_t> m_layoutsWritten;
    Vector<uint32_t> m_firstWriterOfLayout;
    Vector<uint32_t> m_writersOfLayouts;
    Vector<uint32_t> m_firstModuleSeenBy;
    Vector<uint32_t> m_modulesSeen;
    Vector<RecordOfDeclaredParameter> m_recordsOfDeclaredParameters;
    Vector<uint32_t> m_numberWrittenInSight;
    BitVector m_isInstance;
    BitVector m_isNameOfSomeInstance;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
