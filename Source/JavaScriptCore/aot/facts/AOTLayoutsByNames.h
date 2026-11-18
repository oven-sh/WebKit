/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class LayoutsByNames {
public:
    LayoutsByNames() = default;

    LayoutsByNames(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, BuildContext& build)
    {
        runLanes(build.parallelLoops(), {
            [&] { listNamesOfLayouts(program, summaries, linked, held, build.parallelLoops()); },
            [&] {
                Vector<LayoutID> layoutsOfConstructors = layoutsOfConstructorFunctions(program);
                Vector<FunctionID> creators;
                listWhoWritesLayouts(program, summaries, build.parallelLoops(), layoutsOfConstructors.span(), &creators);
                listLayoutsOfInitializers(program, summaries, build.parallelLoops(), layoutsOfConstructors.span(), creators.span());
            },
        });
        listNamesOfHierarchies(program, build.parallelLoops());
    }

    static LayoutsByNames thatListsWritersAlone(const Program& program, const ThinSummaries& summaries, const ParallelLoops& loops)
    {
        LayoutsByNames lists;
        lists.listWhoWritesLayouts(program, summaries, loops);
        return lists;
    }

private:
    void listNamesOfLayouts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, const ParallelLoops& loops)
    {
        uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
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
        listMethodsByNames(linked);
        listMethodsOfManyParameters(program, linked);
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

        Vector<uint64_t> lateNames = namesStoredThroughParameters(program, summaries, loops);
        BytesSharedByThreads isSparseRecord(numberOfLayouts);
        BytesSharedByThreads isInheritedByInstances(numberOfKeys);
        BytesSharedByThreads isNameOfSomeInstance(numberOfKeys);
        m_nameFilters.fill(0, numberOfLayouts);
        m_firstNameOfLayout.fill(0, numberOfLayouts + 1);
        Vector<uint64_t> pairs = collectInParallel<uint64_t>(loops, numberOfLayouts, [&](unsigned index, Vector<uint64_t>& pairs) {
            const Layout& layout = program.layouts[index];
            if ((layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance) || index >= linked.layouts.size() || !linked.layouts[index].isAllocated)
                return;
            uint64_t holder = static_cast<uint64_t>(index) << 32;
            size_t firstPairOfLayout = pairs.size();
            uint32_t numberRequired = 0;
            uint32_t numberWithoutInitializer = 0;
            for (uint32_t place = 0; place < layout.fields.size(); ++place) {
                const Field& field = layout.fields[place];
                if (!field.key)
                    continue;
                pairs.append(holder | field.key.index());
                if (layout.kind != LayoutKind::Record || !field.flags.contains(FieldFlags::IsRequired) || field.flags.contains(FieldFlags::IsNeverWritten))
                    continue;
                ++numberRequired;
                const LinkedCell* cell = held.findCellOfField({ LayoutID(index), place });
                numberWithoutInitializer += cell && cell->isGivenByNoMaker;
            }
            if (numberWithoutInitializer > numberRequired - numberWithoutInitializer)
                isSparseRecord.set(index, 1);
            for (PropertyKeyID key : linked.appendedKeysOf(LayoutID(index)))
                pairs.append(holder | key.index());
            for (uint64_t late : std::ranges::equal_range(lateNames, index, { }, [](uint64_t name) { return static_cast<uint32_t>(name >> 32); }))
                pairs.append(late);
            size_t firstPairAbove = pairs.size();
            size_t steps = 0;
            auto above = [&](LayoutID current) {
                const Layout& below = program.at(current);
                bool parentIsValue = !below.base && below.has(LayoutFlags::HasHeritage) && current.index() < held.layouts.size();
                return parentIsValue ? held.layouts[current.index()].heritage : below.base;
            };
            for (LayoutID current(index); current && steps < numberOfLayouts; current = above(current), ++steps) {
                for (uint64_t name : namesAbove.span().subspan(firstNameAboveOfLayout[current.index()], firstNameAboveOfLayout[current.index() + 1] - firstNameAboveOfLayout[current.index()])) {
                    pairs.append(holder | static_cast<uint32_t>(name));
                    if (layout.kind == LayoutKind::Instance)
                        isInheritedByInstances.set(static_cast<uint32_t>(name), 1);
                }
            }
            std::span<uint64_t> pairsOfLayout = pairs.mutableSpan().subspan(firstPairOfLayout);
            putInOrder(pairsOfLayout.first(firstPairAbove - firstPairOfLayout));
            putInOrder(pairsOfLayout.subspan(firstPairAbove - firstPairOfLayout));
            std::ranges::inplace_merge(pairsOfLayout, pairsOfLayout.begin() + (firstPairAbove - firstPairOfLayout));
            pairs.shrink(firstPairOfLayout + (std::ranges::unique(pairsOfLayout).begin() - pairsOfLayout.begin()));
            for (uint64_t pair : pairs.span().subspan(firstPairOfLayout)) {
                m_nameFilters[index] |= filterBitOf(static_cast<uint32_t>(pair));
                if (layout.kind == LayoutKind::Instance)
                    isNameOfSomeInstance.set(static_cast<uint32_t>(pair), 1);
            }
            m_firstNameOfLayout[index + 1] = static_cast<uint32_t>(pairs.size() - firstPairOfLayout);
        });
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            if (program.layouts[index].kind == LayoutKind::Instance && index < linked.layouts.size() && linked.layouts[index].isAllocated)
                m_isInstance.set(index);
            if (isSparseRecord.at(index))
                m_sparseRecords.set(index);
        }
        for (size_t name = 0; name < numberOfKeys; ++name) {
            if (isInheritedByInstances.at(name))
                m_isInheritedByInstances.set(name);
            if (isNameOfSomeInstance.at(name))
                m_isNameOfSomeInstance.set(name);
        }

        for (size_t next = 1; next <= numberOfLayouts; ++next)
            m_firstNameOfLayout[next] += m_firstNameOfLayout[next - 1];
        runLanes(loops, {
            [&] { listHoldersOfNames(pairs.span(), numberOfKeys); },
            [&] { listRecordsThatDeclaredTypesName(program); },
        });
    }

    void listHoldersOfNames(std::span<const uint64_t> pairs, size_t numberOfKeys)
    {
        m_firstHolderOfName.fill(0, numberOfKeys + 1);
        m_names.reserveInitialCapacity(pairs.size());
        for (uint64_t pair : pairs) {
            ++m_firstHolderOfName[static_cast<uint32_t>(pair) + 1];
            m_names.append(static_cast<uint32_t>(pair));
        }
        for (size_t next = 1; next <= numberOfKeys; ++next)
            m_firstHolderOfName[next] += m_firstHolderOfName[next - 1];
        m_holders.grow(pairs.size());
        for (uint64_t pair : pairs)
            m_holders[m_firstHolderOfName[static_cast<uint32_t>(pair)]++] = static_cast<uint32_t>(pair >> 32);
        for (size_t next = numberOfKeys; next; --next)
            m_firstHolderOfName[next] = m_firstHolderOfName[next - 1];
        m_firstHolderOfName[0] = 0;
    }

    void listMethodsByNames(const LinkTables& linked)
    {
        for (const LinkedMember& member : linked.members) {
            FunctionID function = member.written ? member.written : member.function;
            if (member.key && member.classLayout && member.side == ClassSide::Prototype && member.kind == MethodKind::Method && function)
                m_methodsByNames.append({ member.key, member.classLayout, function });
        }
        for (const LinkedNameOnPrototype& name : linked.namesOnPrototypes) {
            FunctionID function = name.function ? name.function : name.expected;
            if (name.key && name.layout && function)
                m_methodsByNames.append({ name.key, name.layout, function });
        }
        std::ranges::sort(m_methodsByNames);
    }

    void listMethodsOfManyParameters(const Program& program, const LinkTables& linked)
    {
        for (const LinkedMember& member : linked.members) {
            FunctionID function = member.written ? member.written : member.function;
            if (member.key && member.classLayout && member.kind == MethodKind::Method && function && program.at(function).numberOfPositionalParameters() >= fewestParametersInMemory)
                m_methodsOfManyParameters.append({ member.key, member.classLayout, function });
        }
        std::ranges::sort(m_methodsOfManyParameters);
    }

public:
    static constexpr unsigned fewestParametersInMemory = 5;

    struct MethodUnderName {
        PropertyKeyID name;
        LayoutID holder;
        FunctionID function;

        friend constexpr auto operator<=>(const MethodUnderName&, const MethodUnderName&) = default;
    };

    std::span<const MethodUnderName> methodsUnder(PropertyKeyID name) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(m_methodsByNames, name, { }, &MethodUnderName::name);
        return m_methodsByNames.span().subspan(found.begin() - m_methodsByNames.begin(), found.size());
    }

    std::span<const MethodUnderName> methodsOfManyParametersUnder(PropertyKeyID name) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(m_methodsOfManyParameters, name, { }, &MethodUnderName::name);
        return m_methodsOfManyParameters.span().subspan(found.begin() - m_methodsOfManyParameters.begin(), found.size());
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

    ALWAYS_INLINE bool hasAll(uint32_t holder, std::span<const PropertyKeyID> names) const
    {
        if (names.size() > namesSoughtOneByOne) [[unlikely]]
            return hasAllOfLongList(holder, names);
        for (PropertyKeyID name : names) {
            if (!has(holder, name))
                return false;
        }
        return true;
    }

    LayoutID layoutWithMostOf(std::span<const PropertyKeyID> names, ModuleID module) const
    {
        static constexpr size_t mostHoldersOfRareName = 4;
        uint32_t own[] = { module.index() };
        Vector<uint32_t, 8> candidates;
        for (PropertyKeyID name : names) {
            std::span<const uint32_t> holders = m_firstHolderOfName.isEmpty() ? std::span<const uint32_t>() : holdersOf(name);
            if (holders.size() > mostHoldersOfRareName)
                continue;
            for (uint32_t holder : holders)
                candidates.appendIfNotContains(holder);
        }
        LayoutID best;
        size_t mostHeld = 0;
        bool isTied = false;
        for (uint32_t candidate : candidates) {
            size_t held = std::ranges::count_if(names, [&](PropertyKeyID name) { return has(candidate, name); });
            if (held < mostHeld || !isWrittenInOneOf(own, candidate))
                continue;
            isTied = held == mostHeld;
            mostHeld = held;
            best = LayoutID(candidate);
        }
        return !isTied && mostHeld * 4 >= names.size() * 3 ? best : LayoutID();
    }

    bool isInstance(LayoutID layout) const { return m_isInstance.get(layout.index()); }
    bool isLayoutOfInitializer(LayoutID layout) const { return m_isLayoutOfInitializer.get(layout.index()); }

    void dropFewSparseRecords(Program& program, std::span<const DeclaredSetForFamilies> declaredSets)
    {
        if (m_sparseRecords.isEmpty())
            return;
        constexpr size_t minOthersOfSparseRecord = 64;
        TypeArena types(program.types, ArenaKeeps::InProgram);
        Vector<LayoutID> sparse;
        m_sparseRecords.forEachSetBit([&](size_t layout) {
            sparse.append(LayoutID(static_cast<uint32_t>(layout)));
        });
        LayoutSetID sparseRecords = types.layoutSets().ofSorted(sparse.span());
        for (const DeclaredSetForFamilies& declared : declaredSets) {
            if (declared.size <= minOthersOfSparseRecord || !overlap(types.at(declared.layouts).untyped(), types.at(sparseRecords).untyped()))
                continue;
            LayoutSetID part = types.layoutSets().differenceOf(declared.layouts, sparseRecords);
            size_t sizeOfPart = types.layoutSets().sizeOf(part);
            size_t numberOfSparse = declared.size - sizeOfPart;
            if (numberOfSparse * minOthersOfSparseRecord >= declared.size)
                continue;
            size_t numberOfUsedOthers = 0;
            bool areFew = types.at(part).anyOf([&](LayoutID layout) {
                numberOfUsedOthers += isInUse(layout);
                return numberOfUsedOthers + numberOfSparse > numberOfSparse * minOthersOfSparseRecord;
            });
            if (!areFew)
                continue;
            for (uint16_t nullish : { uint16_t { 0 }, uint16_t { Primitive::Undefined }, uint16_t { Primitive::Null }, uint16_t { Primitive::UndefinedOrNull } }) {
                TypeID whole = types.withPrimitives(types.forLayoutSet(declared.layouts), nullish);
                m_typesWithFewSparseRecords.set(positionOf(whole));
                m_partsWithoutSparseRecords.add(whole.bits(), types.withPrimitives(types.forLayoutSet(part), nullish));
            }
        }
    }

    TypeID withoutFewSparseRecords(TypeID type) const
    {
        if (!liesInProgram(type) || !m_typesWithFewSparseRecords.get(positionOf(type))) [[likely]]
            return type;
        return m_partsWithoutSparseRecords.get(type.bits());
    }

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
        if (writers.size() == 1 && modules.size() == 1) [[likely]]
            return writers.front() == modules.front();
        std::span<const uint32_t> fewer = modules.size() < writers.size() ? modules : writers;
        std::span<const uint32_t> more = modules.size() < writers.size() ? writers : modules;
        return std::ranges::any_of(fewer, [&](uint32_t member) { return std::ranges::binary_search(more, member); });
    }

    bool isWrittenInImporterOf(ModuleID module, uint32_t layout) const
    {
        return std::ranges::any_of(rowOf(m_firstWriterOfLayout, m_writersOfLayouts, layout), [&](uint32_t writer) {
            return std::ranges::binary_search(modulesSeenBy(ModuleID(writer)), module.index());
        });
    }

    bool isInheritedByInstances(PropertyKeyID name) const { return m_isInheritedByInstances.get(name.index()); }
    bool isNameOfSomeInstance(PropertyKeyID name) const { return m_isNameOfSomeInstance.get(name.index()); }
    bool isNameOfHierarchyIn(ModuleID module, PropertyKeyID name) const { return std::ranges::binary_search(rowOf(m_firstNameOfHierarchiesIn, m_namesOfHierarchies, module.index()), name.index()); }
    bool isNameOfSomeLayout(PropertyKeyID name) const { return !holdersOf(name).empty(); }
    bool severalLayoutsHold(PropertyKeyID name) const { return holdersOf(name).size() > 1; }

    template<typename Predicate>
    bool isAnyOfFewHolders(PropertyKeyID name, const Predicate& predicate) const
    {
        constexpr size_t mostHolders = 4;
        std::span<const uint32_t> holders = holdersOf(name);
        return holders.size() <= mostHolders && std::ranges::any_of(holders, [&](uint32_t holder) { return predicate(LayoutID(holder)); });
    }

    bool isInUse(LayoutID layout) const { return static_cast<size_t>(layout.index()) + 1 < m_firstNameOfLayout.size() && m_firstNameOfLayout[layout.index()] < m_firstNameOfLayout[layout.index() + 1]; }

    ALWAYS_INLINE bool hasAll(LayoutID layout, std::span<const PropertyKeyID> names) const
    {
        if (static_cast<size_t>(layout.index()) + 1 >= m_firstNameOfLayout.size())
            return false;
        return hasAll(layout.index(), names);
    }

    static uint64_t filterOf(std::span<const PropertyKeyID> names)
    {
        uint64_t filter = 0;
        for (PropertyKeyID name : names)
            filter |= filterBitOf(name.index());
        return filter;
    }

    ALWAYS_INLINE bool mayHaveAll(uint32_t layout, uint64_t filterOfNames) const { return (m_nameFilters[layout] & filterOfNames) == filterOfNames; }

    bool hasAnyOf(LayoutID layout, std::span<const PropertyKeyID> ascending, uint64_t filterOfNames) const
    {
        if (layout.index() >= m_nameFilters.size() || !(m_nameFilters[layout.index()] & filterOfNames))
            return false;
        std::span<const uint32_t> namesOfLayout = m_names.span().subspan(m_firstNameOfLayout[layout.index()], m_firstNameOfLayout[layout.index() + 1] - m_firstNameOfLayout[layout.index()]);
        if (namesOfLayout.size() < ascending.size())
            return std::ranges::any_of(namesOfLayout, [&](uint32_t name) { return std::ranges::binary_search(ascending, name, { }, &PropertyKeyID::index); });
        return std::ranges::any_of(ascending, [&](PropertyKeyID name) { return std::ranges::binary_search(namesOfLayout, name.index()); });
    }

    LayoutID findOnlyHolderAmong(PropertyKeyID name, std::span<const LayoutID> ascending) const
    {
        LayoutID only;
        bool isOnly = visitHoldersAmong(name, ascending, [&](LayoutID layout) {
            if (only)
                return false;
            only = layout;
            return true;
        });
        return isOnly ? only : LayoutID();
    }

    bool hasHolderAmong(PropertyKeyID name, std::span<const LayoutID> ascending) const
    {
        return !visitHoldersAmong(name, ascending, [](LayoutID) { return false; });
    }

    template<size_t inlineCapacity>
    bool appendHoldersAmongUnlessMoreThan(size_t most, PropertyKeyID name, std::span<const LayoutID> ascending, Vector<LayoutID, inlineCapacity>& found) const
    {
        return visitHoldersAmong(name, ascending, [&](LayoutID layout) {
            if (found.size() == most)
                return false;
            found.append(layout);
            return true;
        });
    }

    template<size_t inlineCapacity>
    void appendHoldersAmong(PropertyKeyID name, std::span<const LayoutID> ascending, Vector<LayoutID, inlineCapacity>& found) const
    {
        visitHoldersAmong(name, ascending, [&](LayoutID layout) {
            found.append(layout);
            return true;
        });
    }

private:
    static Vector<uint64_t> namesStoredThroughParameters(const Program& program, const ThinSummaries& summaries, const ParallelLoops& loops)
    {
        TypeReader types(program);
        unsigned numberOfFunctions = static_cast<unsigned>(std::min<size_t>(summaries.size(), program.functions.size()));
        Vector<uint64_t> names = collectInParallel<uint64_t>(loops, numberOfFunctions, [&](unsigned index, Vector<uint64_t>& names) {
            const Function& function = program.functions[index];
            if (function.has(FunctionFlags::IsChecked))
                return;
            const ThinSummary& summary = summaries.at(FunctionID(index));
            for (const SummaryAccess& access : summary.accesses) {
                if (access.kind != SummaryAccess::Kind::Store || !access.key || access.isToContents() || summary.nodes[access.subject].kind != SummaryNode::Kind::Parameter)
                    continue;
                const DeclaredType* declared = function.declaredEntryParameter(summary.nodes[access.subject].first);
                if (!declared || !declared->isExpected() || !declared->written())
                    continue;
                Type written = types.at(declared->written());
                std::span<const LayoutID> layouts = written.hasOnlyLayouts() ? types.listOf(written.layouts) : std::span<const LayoutID>();
                if (layouts.size() == 1 && program.at(layouts[0]).kind == LayoutKind::Instance && !program.at(layouts[0]).findField(access.key))
                    names.append(static_cast<uint64_t>(layouts[0].index()) << 32 | access.key.index());
            }
        });
        sortAndRemoveDuplicates(loops, names);
        return names;
    }

    void listWhoWritesLayouts(const Program& program, const ThinSummaries& summaries, const ParallelLoops& loops, std::span<const LayoutID> layoutsOfConstructors = { }, Vector<FunctionID>* creatorsOfConstructors = nullptr)
    {
        uint32_t numberOfFunctions = static_cast<uint32_t>(std::min<size_t>(summaries.size(), program.functions.size()));
        BytesSharedByThreads createsConstructors(numberOfFunctions);
        Vector<uint64_t> written = collectInParallel<uint64_t>(loops, numberOfFunctions, [&](unsigned function, Vector<uint64_t>& written) {
            ModuleID module = program.functions[function].module;
            bool createsConstructor = false;
            for (const SummaryNode& node : summaries.at(FunctionID(function)).nodes.span()) {
                if (node.kind == SummaryNode::Kind::Allocation) {
                    if (module && module.index() < program.modules.size() && program.at(LayoutID(node.first)).kind == LayoutKind::Record)
                        written.append(static_cast<uint64_t>(module.index()) << 32 | node.first);
                } else if (node.kind == SummaryNode::Kind::Closure)
                    createsConstructor |= node.first < layoutsOfConstructors.size() && layoutsOfConstructors[node.first];
            }
            if (createsConstructor)
                createsConstructors.set(function, 1);
        });
        for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
            if (ModuleID module = program.layouts[layout].module; module && module.index() < program.modules.size())
                written.append(static_cast<uint64_t>(module.index()) << 32 | layout);
        }
        for (uint32_t function = 0; creatorsOfConstructors && function < numberOfFunctions; ++function) {
            if (createsConstructors.at(function))
                creatorsOfConstructors->append(FunctionID(function));
        }
        layRows(loops, written, program.modules.size(), m_firstLayoutWrittenIn, m_layoutsWritten);

        for (uint64_t& pair : written)
            pair = pair << 32 | pair >> 32;
        layRows(loops, written, program.layouts.size(), m_firstWriterOfLayout, m_writersOfLayouts);

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
        layRows(loops, seen, program.modules.size(), m_firstModuleSeenBy, m_modulesSeen);

        m_numberWrittenInSight.fill(0, program.modules.size());
        for (uint64_t pair : seen)
            m_numberWrittenInSight[pair >> 32] += layoutsWrittenIn(ModuleID(static_cast<uint32_t>(pair))).size();
    }

    void listNamesOfHierarchies(const Program& program, const ParallelLoops& loops)
    {
        Vector<uint64_t> listed;
        Vector<uint32_t> names;
        for (uint32_t module = 0; module < program.modules.size(); ++module) {
            names.shrink(0);
            size_t numberOfSubclasses = 0;
            for (uint32_t layout : layoutsWrittenIn(ModuleID(module))) {
                if (!m_isInstance.get(layout) || !program.layouts[layout].base)
                    continue;
                names.append(rowOf(m_firstNameOfLayout, m_names, layout));
                ++numberOfSubclasses;
            }
            if (numberOfSubclasses < 2)
                continue;
            std::ranges::sort(names);
            for (size_t first = 0; first < names.size();) {
                size_t end = std::ranges::upper_bound(names, names[first]) - names.begin();
                if (end - first > 1)
                    listed.append(static_cast<uint64_t>(module) << 32 | names[first]);
                first = end;
            }
        }
        layRows(loops, listed, program.modules.size(), m_firstNameOfHierarchiesIn, m_namesOfHierarchies);
    }

    static Vector<LayoutID> layoutsOfConstructorFunctions(const Program& program)
    {
        Vector<LayoutID> layouts;
        layouts.grow(program.functions.size());
        for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
            const Layout& record = program.layouts[layout];
            if (record.kind == LayoutKind::Instance && record.has(LayoutFlags::IsConstructorFunction) && record.constructor && record.constructor.index() < layouts.size())
                layouts[record.constructor.index()] = LayoutID(layout);
        }
        return layouts;
    }

    void listLayoutsOfInitializers(const Program& program, const ThinSummaries& summaries, const ParallelLoops& loops, std::span<const LayoutID> layoutsOfConstructors, std::span<const FunctionID> creatorsOfConstructors)
    {
        if (creatorsOfConstructors.empty())
            return;
        PropertyKeyID apply = program.keyOf(BuiltinNames::apply);
        PropertyKeyID call = program.keyOf(BuiltinNames::call);
        BitVector appliedKeys;
        unsigned numberOfFunctions = static_cast<unsigned>(std::min<size_t>(summaries.size(), program.functions.size()));
        Vector<PropertyKeyID> applied = collectInParallel<PropertyKeyID>(loops, numberOfFunctions, [&](unsigned function, Vector<PropertyKeyID>& applied) {
            const ThinSummary& summary = summaries.at(FunctionID(function));
            for (const SummaryCall& made : summary.calls) {
                if ((made.key != apply && made.key != call) || !made.key || made.callee != SummaryCall::Callee::Name || made.isConstruction || made.continuesConstruction || !made.numberOfArguments || made.receiver == noSummaryNode)
                    continue;
                const SummaryNode& callee = summary.nodes[made.receiver];
                if (callee.kind != SummaryNode::Kind::ResultOfAccess)
                    continue;
                const SummaryAccess& read = summary.accesses[callee.first];
                if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents() || read.subject != summary.arguments[made.firstArgument])
                    continue;
                applied.append(read.key);
            }
        });
        if (applied.isEmpty())
            return;
        for (PropertyKeyID key : applied)
            appliedKeys.set(key.index());
        for (FunctionID creator : creatorsOfConstructors) {
            const ThinSummary& summary = summaries.at(creator);
            auto note = [&](PropertyKeyID key, uint32_t value) {
                const SummaryNode& node = summary.nodes[value];
                if (node.kind != SummaryNode::Kind::Closure || !key || !appliedKeys.get(key.index()))
                    return;
                if (LayoutID layout = layoutsOfConstructors[node.first])
                    m_isLayoutOfInitializer.set(layout.index());
            };
            for (const SummaryAccess& access : summary.accesses) {
                if (access.laysValueInSightUnderName() && access.value != noSummaryNode)
                    note(access.key, access.value);
            }
            for (const SummaryEdge& edge : summary.edges) {
                if (summary.nodes[edge.from].kind != SummaryNode::Kind::Closure)
                    continue;
                const SummaryNode& to = summary.nodes[edge.to];
                if (to.kind == SummaryNode::Kind::Field && to.first < program.layouts.size() && to.second < program.layouts[to.first].fields.size())
                    note(program.layouts[to.first].fields[to.second].key, edge.from);
            }
        }
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

    static void layRows(const ParallelLoops& loops, Vector<uint64_t>& pairs, size_t rowCount, Vector<uint32_t>& firstOfRow, Vector<uint32_t>& members)
    {
        sortAndRemoveDuplicates(loops, pairs);
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

    static uint64_t filterBitOf(uint32_t name) { return 1ull << (name * 0x9e3779b9u >> 26); }

    static constexpr size_t namesSoughtOneByOne = 16;

    NEVER_INLINE bool hasAllOfLongList(uint32_t holder, std::span<const PropertyKeyID> names) const
    {
        std::span<const uint32_t> own = m_names.span().subspan(m_firstNameOfLayout[holder], m_firstNameOfLayout[holder + 1] - m_firstNameOfLayout[holder]);
        size_t next = 0;
        uint32_t last = 0;
        for (PropertyKeyID name : names) {
            if (name.index() < last) {
                if (!has(holder, name))
                    return false;
                continue;
            }
            last = name.index();
            if (!(m_nameFilters[holder] & filterBitOf(last)))
                return false;
            if (next < own.size() && own[next] < last)
                ++next;
            if (next < own.size() && own[next] < last) {
                size_t step = 1;
                for (; next + step < own.size() && own[next + step] < last; step *= 2)
                    next += step;
                std::span<const uint32_t> rest = own.subspan(next, std::min(step + 1, own.size() - next));
                next += std::ranges::lower_bound(rest, last) - rest.begin();
            }
            if (next == own.size() || own[next] != last)
                return false;
        }
        return true;
    }

    ALWAYS_INLINE bool has(uint32_t layout, PropertyKeyID name) const
    {
        if (!(m_nameFilters[layout] & filterBitOf(name.index()))) [[likely]]
            return false;
        return std::ranges::binary_search(m_names.span().subspan(m_firstNameOfLayout[layout], m_firstNameOfLayout[layout + 1] - m_firstNameOfLayout[layout]), name.index());
    }

    template<typename Visit>
    ALWAYS_INLINE bool visitHoldersAmong(PropertyKeyID name, std::span<const LayoutID> ascending, const Visit& visit) const
    {
        std::span<const uint32_t> holders = holdersOf(name);
        if (holders.size() * std::bit_width(ascending.size()) < ascending.size()) {
            for (uint32_t holder : holders) {
                if (std::ranges::binary_search(ascending, holder, { }, [](LayoutID layout) { return layout.index(); }) && !visit(LayoutID(holder)))
                    return false;
            }
            return true;
        }
        if (holders.empty())
            return true;
        std::span<const uint64_t> filters = m_nameFilters.span();
        uint64_t filter = filterBitOf(name.index());
        for (LayoutID layout : ascending) {
            if (layout.index() < filters.size() && (filters[layout.index()] & filter) && has(layout.index(), name) && !visit(layout))
                return false;
        }
        return true;
    }

    Vector<uint32_t> m_firstNameOfLayout;
    Vector<uint32_t> m_names;
    Vector<uint64_t> m_nameFilters;
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
    Vector<MethodUnderName> m_methodsByNames;
    Vector<MethodUnderName> m_methodsOfManyParameters;
    BitVector m_isLayoutOfInitializer;
    BitVector m_sparseRecords;
    BitVector m_typesWithFewSparseRecords;
    UncheckedKeyHashMap<uint32_t, TypeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_partsWithoutSparseRecords;
    BitVector m_isNameOfSomeInstance;
    Vector<uint32_t> m_firstNameOfHierarchiesIn;
    Vector<uint32_t> m_namesOfHierarchies;
    BitVector m_isInheritedByInstances;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
