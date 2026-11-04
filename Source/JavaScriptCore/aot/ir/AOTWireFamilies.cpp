/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWireTypesInternal.h"

#if ENABLE(AOT)

#include "AOTBodyVerifier.h"
#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include <functional>
#include <optional>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Listed>
static void listSecondsByFirstOnOneThread(std::span<const Vector<uint64_t>> pairsInOrderOfSeconds, Vector<Vector<Listed>>& lists)
{
    Vector<uint32_t> sizes;
    sizes.fill(0u, lists.size());
    for (const Vector<uint64_t>& pairs : pairsInOrderOfSeconds) {
        for (uint64_t pair : pairs)
            ++sizes[pair >> 32];
    }
    for (size_t first = 0; first < lists.size(); ++first) {
        if (!sizes[first])
            continue;
        lists[first] = { };
        lists[first].reserveInitialCapacity(sizes[first]);
    }
    for (const Vector<uint64_t>& pairs : pairsInOrderOfSeconds) {
        for (uint64_t pair : pairs)
            lists[pair >> 32].append(Listed(static_cast<uint32_t>(pair)));
    }
}

namespace {

// Some of the pairs, one list after the other, dealt out by the group of their firsts. Within a group they keep their order.
struct ShareOfPairs {
    size_t firstPiece { 0 };
    size_t endPiece { 0 };
    Vector<uint64_t> pairsByGroup;
    // [group], and one more for the end.
    Vector<uint32_t> firstOfGroup;
};

} // anonymous namespace

// The shares go by the sizes of the lists of pairs alone and the groups by the number of lists alone. One thread fills a list, share after share: the lists are the same at any number of threads.
template<typename Listed>
void listSecondsByFirst(const ParallelLoops& loops, std::span<const Vector<uint64_t>> pairsInOrderOfSeconds, Vector<Vector<Listed>>& lists)
{
    constexpr size_t pairsPerShare = 1 << 15;
    constexpr size_t mostGroups = 256;

    Vector<std::span<const uint64_t>> pieces;
    Vector<ShareOfPairs> shares;
    size_t pairsInLastShare = pairsPerShare;
    for (const Vector<uint64_t>& pairs : pairsInOrderOfSeconds) {
        for (size_t start = 0; start < pairs.size(); start += pairsPerShare) {
            std::span<const uint64_t> piece = pairs.subspan(start, std::min(pairsPerShare, pairs.size() - start));
            if (pairsInLastShare >= pairsPerShare) {
                shares.append(ShareOfPairs { pieces.size(), pieces.size(), { }, { } });
                pairsInLastShare = 0;
            }
            pieces.constructAndAppend(piece);
            shares.last().endPiece = pieces.size();
            pairsInLastShare += piece.size();
        }
    }
    // One thread is through so few before the others have woken up.
    if (shares.size() < 2) {
        listSecondsByFirstOnOneThread(pairsInOrderOfSeconds, lists);
        return;
    }

    size_t numberOfLists = lists.size();
    RELEASE_ASSERT(numberOfLists);
    unsigned shift = 0;
    while ((numberOfLists >> shift) > mostGroups)
        ++shift;
    size_t numberOfGroups = ((numberOfLists - 1) >> shift) + 1;

    forEachIndexInParallel(loops, static_cast<unsigned>(shares.size()), [&](unsigned index) {
        ShareOfPairs& share = shares[index];
        std::span<const std::span<const uint64_t>> piecesOfShare = pieces.subspan(share.firstPiece, share.endPiece - share.firstPiece);
        share.firstOfGroup.fill(0u, numberOfGroups + 1);
        for (std::span<const uint64_t> piece : piecesOfShare) {
            for (uint64_t pair : piece)
                ++share.firstOfGroup[(pair >> 32 >> shift) + 1];
        }
        for (size_t group = 0; group < numberOfGroups; ++group)
            share.firstOfGroup[group + 1] += share.firstOfGroup[group];
        share.pairsByGroup.grow(share.firstOfGroup[numberOfGroups]);
        Vector<uint32_t> next = share.firstOfGroup;
        for (std::span<const uint64_t> piece : piecesOfShare) {
            for (uint64_t pair : piece)
                share.pairsByGroup[next[pair >> 32 >> shift]++] = pair;
        }
    });

    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfGroups), [&](unsigned group) {
        size_t firstList = static_cast<size_t>(group) << shift;
        auto forEachPairOfGroup = [&](NOESCAPE const Invocable<void(uint64_t)> auto& functor) {
            for (const ShareOfPairs& share : shares) {
                for (uint64_t pair : share.pairsByGroup.subspan(share.firstOfGroup[group], share.firstOfGroup[group + 1] - share.firstOfGroup[group]))
                    functor(pair);
            }
        };
        Vector<uint32_t> sizes;
        sizes.fill(0u, std::min(static_cast<size_t>(1) << shift, numberOfLists - firstList));
        forEachPairOfGroup([&](uint64_t pair) {
            ++sizes[(pair >> 32) - firstList];
        });
        for (size_t index = 0; index < sizes.size(); ++index) {
            if (!sizes[index])
                continue;
            lists[firstList + index] = { };
            lists[firstList + index].reserveInitialCapacity(sizes[index]);
        }
        forEachPairOfGroup([&](uint64_t pair) {
            lists[pair >> 32].append(Listed(static_cast<uint32_t>(pair)));
        });
    });
}

template void listSecondsByFirst(const ParallelLoops&, std::span<const Vector<uint64_t>>, Vector<Vector<uint32_t>>&);
template void listSecondsByFirst(const ParallelLoops&, std::span<const Vector<uint64_t>>, Vector<Vector<LayoutID>>&);
template void listSecondsByFirst(const ParallelLoops&, std::span<const Vector<uint64_t>>, Vector<Vector<FunctionID>>&);

template<typename Listed>
void destroyLists(const ParallelLoops& loops, Vector<Vector<Listed>>& lists)
{
    constexpr size_t listsPerTask = 1 << 10;
    forEachIndexInParallel(loops, static_cast<unsigned>((lists.size() + listsPerTask - 1) / listsPerTask), [&](unsigned task) {
        size_t first = task * listsPerTask;
        for (Vector<Listed>& list : lists.mutableSpan().subspan(first, std::min(listsPerTask, lists.size() - first)))
            list = { };
    });
    lists = { };
}

template void destroyLists(const ParallelLoops&, Vector<Vector<uint32_t>>&);
template void destroyLists(const ParallelLoops&, Vector<Vector<uint64_t>>&);
template void destroyLists(const ParallelLoops&, Vector<Vector<LayoutID>>&);

namespace {

// A name is the index of a property key of the wire. Each list ascends and holds no name twice.
class NamesByWireLayout {
public:
    NamesByWireLayout() = default;

    NamesByWireLayout(const ParallelLoops& loops, uint32_t numberOfLayouts, NOESCAPE const Invocable<void(uint32_t layout, Vector<uint32_t>& names)> auto& appendNames)
    {
        m_firstName.grow(numberOfLayouts + 1);
        m_names = collectInParallel<uint32_t>(loops, numberOfLayouts, [&](unsigned layout, Vector<uint32_t>& names) {
            size_t sizeBefore = names.size();
            appendNames(layout, names);
            std::span<uint32_t> appended = names.mutableSpan().subspan(sizeBefore);
            AOT::sort(appended);
            size_t numberOfNames = std::ranges::unique(appended).begin() - appended.begin();
            names.shrink(sizeBefore + numberOfNames);
            m_firstName[layout] = static_cast<uint32_t>(numberOfNames);
        });
        uint32_t numberOfNamesInFront = 0;
        for (uint32_t layout = 0; layout < numberOfLayouts; ++layout)
            m_firstName[layout] = std::exchange(numberOfNamesInFront, numberOfNamesInFront + m_firstName[layout]);
        m_firstName[numberOfLayouts] = numberOfNamesInFront;
    }

    std::span<const uint32_t> of(uint32_t layout) const LIFETIME_BOUND { return m_names.subspan(m_firstName[layout], m_firstName[layout + 1] - m_firstName[layout]); }

private:
    Vector<uint32_t> m_names;
    Vector<uint32_t> m_firstName;
};

struct NamesOfLayoutFamilies {
    bool lacksRequiredName(LayoutID member, uint32_t type) const;

    uint32_t numberOfWireLayouts { 0 };
    NamesByWireLayout required;
    // Of the layouts in declaresEveryName alone.
    NamesByWireLayout declared;
    BitVector declaresEveryName;
    uint32_t firstTaggedLayout { 0 };
    Vector<Vector<uint32_t>> declaredByTaggedLayouts;
};

struct LayoutFamilies {
    Vector<Vector<LayoutID>> members;
    BitVector hasMembersWithoutLayout;
};

struct LayoutConversion {
    uint32_t source;
    uint32_t target;
};

struct LayoutConversions {
    const Wire::Sections& checkedSections;
    Vector<Vector<uint32_t>> typesMetByNames;
    const TellingFields& tellingFields;
    NamesOfLayoutFamilies names;
    std::span<const TaggedFamily> taggedFamilies;
};

struct MemberOutsideFamily {
    LayoutID member;
    uint32_t type;
};

} // anonymous namespace

static bool isWireEnvironment(const Wire::Layout& layout)
{
    return isEnvironmentKind(static_cast<LayoutKind>(layout.kind));
}

static std::span<const Wire::Field> wireFieldsOf(const Wire::Sections& sections, const Wire::Layout& layout)
{
    std::span<const Wire::Field> fields = sections.fields;
    if (layout.fields.start > fields.size() || layout.fields.count > fields.size() - layout.fields.start)
        return { };
    return fields.subspan(layout.fields.start, layout.fields.count);
}

static std::span<const uint32_t> wireSupertypesOf(const Wire::Sections& checkedSections, const Wire::Layout& layout)
{
    return checkedSections.words.subspan(layout.supertypes.start, layout.supertypes.count);
}

static std::optional<InternalErrorKind> findErrorInSupertypesOfWireLayout(const Wire::Sections& sections, const Wire::Layout& layout)
{
    std::span<const Wire::Layout> layouts = sections.layouts;
    std::span<const uint32_t> words = sections.words;
    if (layout.kind >= numberOfEnumerators<LayoutKind>)
        return InternalErrorKind::UnknownEnumerator;
    if (layout.base != Wire::none && layout.base >= layouts.size())
        return InternalErrorKind::IdOutOfRange;
    if (layout.supertypes.start > words.size() || layout.supertypes.count > words.size() - layout.supertypes.start)
        return InternalErrorKind::ListOutOfBounds;
    for (uint32_t supertype : wireSupertypesOf(sections, layout)) {
        if (supertype >= layouts.size())
            return InternalErrorKind::IdOutOfRange;
    }
    return std::nullopt;
}

// The error of the first layout that has one.
static std::optional<InternalError> findWireLayoutWithInvalidSupertypes(const ParallelLoops& loops, const Wire::Sections& sections)
{
    Vector<InternalError> errors = collectInParallel<InternalError>(loops, static_cast<unsigned>(sections.layouts.size()), [&](unsigned index, Vector<InternalError>& found) {
        if (auto kind = findErrorInSupertypesOfWireLayout(sections, sections.layouts[index]))
            found.append(InternalError { .kind = *kind, .layout = LayoutID(index), .count = index });
    });
    if (errors.isEmpty())
        return std::nullopt;
    return WTF::move(errors.first());
}

// A layout or a function that a type names disappears into the interned type, so nothing behind the read can report it. The error of the first type that has one.
static std::optional<InternalError> findWireTypeWithInvalidLists(const ParallelLoops& loops, const Wire::Sections& sections, uint32_t numberOfAllLayouts)
{
    std::span<const uint32_t> words = sections.words;
    auto findErrorInList = [&](Wire::Span list, size_t numberOfRecords) -> std::optional<InternalErrorKind> {
        if (list.start > words.size() || list.count > words.size() - list.start)
            return InternalErrorKind::ListOutOfBounds;
        for (uint32_t id : words.subspan(list.start, list.count)) {
            if (id >= numberOfRecords)
                return InternalErrorKind::IdOutOfRange;
        }
        return std::nullopt;
    };
    Vector<InternalError> errors = collectInParallel<InternalError>(loops, static_cast<unsigned>(sections.types.size()), [&](unsigned index, Vector<InternalError>& found) {
        const Wire::Type& type = sections.types[index];
        std::optional<InternalErrorKind> kind = findErrorInList(type.layouts, numberOfAllLayouts);
        if (!kind)
            kind = findErrorInList(type.functions, sections.functions.size());
        if (kind)
            found.append(InternalError { .kind = *kind, .count = index });
    });
    if (errors.isEmpty())
        return std::nullopt;
    return WTF::move(errors.first());
}

static bool isWireFieldNamedByProgram(const Wire::Field& field)
{
    return field.key != Wire::none && !has(field, FieldFlags::IsPrivate) && !has(field, FieldFlags::IsBrand) && !has(field, FieldFlags::IsHidden);
}

static bool isRequiredWireField(const Wire::Field& field)
{
    return isWireFieldNamedByProgram(field) && has(field, FieldFlags::IsRequired) && !has(field, FieldFlags::IsMethod) && !has(field, FieldFlags::IsNeverWritten);
}

static bool isStructuralWireType(const Wire::Layout& layout)
{
    return static_cast<LayoutKind>(layout.kind) == LayoutKind::Record && layout.constructor == Wire::none && !has(layout, LayoutFlags::IsTypeParameter);
}

static bool isWireLayoutOfObjects(const Wire::Layout& layout)
{
    switch (static_cast<LayoutKind>(layout.kind)) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        return !has(layout, LayoutFlags::IsTypeParameter);
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    return false;
}

static bool wireLayoutDeclaresEveryName(const Wire::Layout& layout)
{
    switch (static_cast<LayoutKind>(layout.kind)) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return !has(layout, LayoutFlags::IsTypeParameter);
    case LayoutKind::Instance:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    return false;
}

static void appendNamesAlongWireBases(const Wire::Sections& checkedSections, uint32_t layout, Vector<uint32_t>& names, NOESCAPE const Invocable<bool(uint32_t)> auto& isWanted)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    // Stops at a cycle of bases.
    size_t remainingBases = layouts.size();
    for (uint32_t current = layout; current != Wire::none && remainingBases--; current = layouts[current].base) {
        for (const Wire::Field& field : wireFieldsOf(checkedSections, layouts[current])) {
            if (isWireFieldNamedByProgram(field) && isWanted(field.key))
                names.append(field.key);
        }
    }
}

static void appendNamesAlongWireBases(const Wire::Sections& checkedSections, uint32_t layout, Vector<uint32_t>& names)
{
    appendNamesAlongWireBases(checkedSections, layout, names, [](uint32_t) {
        return true;
    });
}

bool NamesOfLayoutFamilies::lacksRequiredName(LayoutID member, uint32_t type) const
{
    std::span<const uint32_t> names = required.of(type);
    if (names.empty())
        return false;
    uint32_t index = member.index();
    if (index < numberOfWireLayouts)
        return declaresEveryName.quickGet(index) && !std::ranges::includes(declared.of(index), names);
    if (index < firstTaggedLayout || index - firstTaggedLayout >= declaredByTaggedLayouts.size())
        return false;
    return !std::ranges::includes(declaredByTaggedLayouts[index - firstTaggedLayout], names);
}

static NamesOfLayoutFamilies collectNamesOfLayoutFamilies(const ParallelLoops& loops, const Wire::Sections& checkedSections)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());

    NamesOfLayoutFamilies result;
    result.numberOfWireLayouts = numberOfLayouts;
    result.declaresEveryName = BitVector(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (wireLayoutDeclaresEveryName(layouts[index]))
            result.declaresEveryName.quickSet(index);
    }
    result.declared = NamesByWireLayout(loops, numberOfLayouts, [&](uint32_t index, Vector<uint32_t>& names) {
        if (result.declaresEveryName.quickGet(index))
            appendNamesAlongWireBases(checkedSections, index, names);
    });
    result.required = NamesByWireLayout(loops, numberOfLayouts, [&](uint32_t index, Vector<uint32_t>& names) {
        if (!isStructuralWireType(layouts[index]))
            return;
        for (const Wire::Field& field : wireFieldsOf(checkedSections, layouts[index])) {
            if (isRequiredWireField(field))
                names.append(field.key);
        }
    });
    return result;
}

static void addNamesOfTaggedFamilies(NamesOfLayoutFamilies& names, const Wire::Sections& checkedSections, std::span<const TaggedFamily> taggedFamilies)
{
    if (taggedFamilies.empty())
        return;
    names.firstTaggedLayout = taggedFamilies[0].members[0].layout.index();
    for (const TaggedFamily& family : taggedFamilies) {
        Vector<uint32_t> namesOfMaker;
        appendNamesAlongWireBases(checkedSections, family.maker.index(), namesOfMaker);
        for (const TaggedLayout& member : family.members) {
            Vector<uint32_t> declared = namesOfMaker;
            for (LayoutID type : member.declaringTypes) {
                for (const Wire::Field& field : wireFieldsOf(checkedSections, checkedSections.layouts[type.index()])) {
                    if (field.key != Wire::none && !has(field, FieldFlags::IsMethod) && !has(field, FieldFlags::IsNeverWritten))
                        declared.append(field.key);
                }
            }
            AOT::sort(declared);
            removeRepeatedElements(declared);
            RELEASE_ASSERT(member.layout.index() - names.firstTaggedLayout == names.declaredByTaggedLayouts.size());
            names.declaredByTaggedLayouts.append(WTF::move(declared));
        }
    }
}

// By layout, ascending: the structural types whose required names its objects have.
static Vector<Vector<uint32_t>> findWireTypesMetByNames(const BuildContext& build, const Wire::Sections& checkedSections, const TellingFields& tellingFields)
{
    const ParallelLoops& loops = build.parallelLoops();
    StepTimes steps(build, "ReadAndVerify: types met by names"_s);
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());
    size_t numberOfKeys = checkedSections.propertyKeys.size();

    auto forEachRequiredMember = [&](uint32_t type, NOESCAPE const Invocable<void(const Wire::Field&)> auto& functor) {
        for (const Wire::Field& field : wireFieldsOf(checkedSections, layouts[type])) {
            if (isWireFieldNamedByProgram(field) && has(field, FieldFlags::IsRequired) && !has(field, FieldFlags::IsNeverWritten) && field.key < numberOfKeys)
                functor(field);
        }
    };
    BitVector isRequiredBySomeType(numberOfKeys);
    for (uint32_t type = 0; type < numberOfLayouts; ++type) {
        if (!isStructuralWireType(layouts[type]))
            continue;
        forEachRequiredMember(type, [&](const Wire::Field& field) {
            isRequiredBySomeType.quickSet(field.key);
        });
    }

    steps.didFinish("names that some type requires"_s);

    NamesByWireLayout names(loops, numberOfLayouts, [&](uint32_t index, Vector<uint32_t>& found) {
        const Wire::Layout& layout = layouts[index];
        if (!isWireLayoutOfObjects(layout) || has(layout, LayoutFlags::IsNeverAllocated) || has(layout, LayoutFlags::IsWrittenUnderDictionaryType))
            return;
        appendNamesAlongWireBases(checkedSections, index, found, [&](uint32_t name) {
            return name < numberOfKeys && isRequiredBySomeType.quickGet(name);
        });
    });
    steps.didFinish("names of layouts"_s);
    Vector<uint64_t> layoutsByName = collectInParallel<uint64_t>(loops, numberOfLayouts, [&](unsigned index, Vector<uint64_t>& found) {
        for (uint32_t name : names.of(index))
            found.append(pairOfWireIndexes(name, index));
    });
    steps.didFinish("pairs of name and layout"_s);
    Vector<Vector<uint32_t>> layoutsWithName(numberOfKeys);
    listSecondsByFirst(loops, singleElementSpan(layoutsByName), layoutsWithName);
    steps.didFinish("layouts by name"_s);

    // A list for each type, not for each few hundred: one type may have tens of thousands of layouts to look at and its neighbors as many.
    Vector<Vector<uint64_t>> typesByLayout(numberOfLayouts);
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned type) {
        Vector<uint64_t>& found = typesByLayout[type];
        if (!isStructuralWireType(layouts[type]))
            return;
        bool isDeclaredNowhere = layouts[type].name == Wire::none && layouts[type].module == Wire::none;
        Vector<uint32_t, 16> requiredOfRecords;
        Vector<uint32_t, 16> requiredOfInstances;
        forEachRequiredMember(type, [&](const Wire::Field& field) {
            requiredOfRecords.append(field.key);
            if (!isDeclaredNowhere || isRequiredWireField(field))
                requiredOfInstances.append(field.key);
        });
        const Vector<uint32_t>* fewest = nullptr;
        for (uint32_t name : requiredOfInstances) {
            if (!fewest || layoutsWithName[name].size() < fewest->size())
                fewest = &layoutsWithName[name];
        }
        if (!fewest)
            return;
        AOT::sort(requiredOfRecords);
        removeRepeatedElements(requiredOfRecords);
        AOT::sort(requiredOfInstances);
        removeRepeatedElements(requiredOfInstances);
        for (uint32_t layout : *fewest) {
            if (layout == type)
                continue;
            std::span<const uint32_t> required = wireLayoutDeclaresEveryName(layouts[layout]) ? requiredOfRecords.span() : requiredOfInstances.span();
            if (std::ranges::includes(names.of(layout), required) && !tellingFields.tellApart(layout, type))
                found.append(pairOfWireIndexes(layout, type));
        }
    });
    steps.didFinish("pairs of layout and type"_s);
    Vector<Vector<uint32_t>> typesMetByNames(numberOfLayouts);
    listSecondsByFirst(loops, typesByLayout.span(), typesMetByNames);
    steps.didFinish("types by layout"_s);
    destroyLists(loops, typesByLayout);
    destroyLists(loops, layoutsWithName);
    steps.didFinish("free the lists"_s);
    return typesMetByNames;
}

static void forEachWireConversionTarget(const Wire::Sections& checkedSections, uint32_t source, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    const Wire::Layout& layout = checkedSections.layouts[source];
    if (layout.base != Wire::none)
        functor(layout.base);
    for (uint32_t supertype : wireSupertypesOf(checkedSections, layout))
        functor(supertype);
}

static bool wireTypeRequiresNoName(const LayoutConversions& conversions, uint32_t type)
{
    return isStructuralWireType(conversions.checkedSections.layouts[type]) && conversions.names.required.of(type).empty();
}

// Every object converts to a type that requires no name. A member that reached such a type only by that conversion is not passed on to a type above it that requires names.
static bool passesOnToFamily(const LayoutConversions& conversions, LayoutConversion conversion, LayoutID member)
{
    if (!wireTypeRequiresNoName(conversions, conversion.source) || wireTypeRequiresNoName(conversions, conversion.target))
        return true;
    return member.index() == conversion.source && !has(conversions.checkedSections.layouts[conversion.source], LayoutFlags::IsNeverAllocated);
}

static LayoutFamilies collectLayoutFamilies(const BuildContext& build, const LayoutConversions& conversions)
{
    const ParallelLoops& loops = build.parallelLoops();
    StepTimes steps(build, "ReadAndVerify: collect families"_s);
    constexpr uint32_t layoutsPerTask = 64;
    const Wire::Sections& checkedSections = conversions.checkedSections;
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());
    unsigned numberOfTasks = (numberOfLayouts + layoutsPerTask - 1) / layoutsPerTask;

    Vector<Vector<uint32_t>> stampsOfWorkers(loops.width());
    Vector<Vector<uint64_t>> membersByTask(numberOfTasks);
    Vector<Vector<uint32_t>> typesOfMembersWithoutLayoutByTask(numberOfTasks);
    forEachIndexInParallel(loops, numberOfTasks, [&](unsigned task, unsigned worker) {
        Vector<uint32_t>& stamps = stampsOfWorkers[worker];
        if (stamps.isEmpty())
            stamps.fill(0u, numberOfLayouts);
        Vector<uint64_t>& members = membersByTask[task];
        Vector<uint32_t>& typesOfMembersWithoutLayout = typesOfMembersWithoutLayoutByTask[task];
        Vector<uint32_t, 16> reachedTypes;
        for (uint32_t index = task * layoutsPerTask, end = std::min(numberOfLayouts, index + layoutsPerTask); index < end; ++index) {
            if (isWireEnvironment(layouts[index])) {
                members.append(pairOfWireIndexes(index, index));
                continue;
            }

            bool hasMembersWithoutLayout = has(layouts[index], LayoutFlags::HasMembersWithoutLayout);
            uint32_t stamp = index + 1;
            auto visit = [&](uint32_t type) {
                if (std::exchange(stamps[type], stamp) != stamp && !isWireEnvironment(layouts[type]) && !conversions.tellingFields.tellApart(index, type))
                    reachedTypes.append(type);
            };
            visit(index);
            // `{ kind: 0, source, target }` is one of a union that its tag tells apart. `interface Edge { source: Node; target: Node }` lists no such name: it was not written for it, and names alone make no family of the two:
            // a family of those two cuts across the union, which then is none. What the program says itself (a base, a supertype) stands.
            auto isToldByNameThatTypeLacks = [&](uint32_t type) {
                bool lacksOne = false;
                conversions.tellingFields.forEachKeyOfOwnTellingField(index, [&](uint32_t key) {
                    lacksOne = lacksOne || !std::ranges::contains(wireFieldsOf(checkedSections, layouts[type]), key, &Wire::Field::key);
                });
                return lacksOne;
            };
            for (uint32_t type : conversions.typesMetByNames[index]) {
                if (!isToldByNameThatTypeLacks(type))
                    visit(type);
            }
            while (!reachedTypes.isEmpty()) {
                uint32_t reached = reachedTypes.takeLast();
                members.append(pairOfWireIndexes(reached, index));
                if (hasMembersWithoutLayout)
                    typesOfMembersWithoutLayout.append(reached);
                forEachWireConversionTarget(checkedSections, reached, [&](uint32_t target) {
                    if (passesOnToFamily(conversions, { reached, target }, LayoutID(index)))
                        visit(target);
                });
            }
        }
    });

    steps.didFinish("pairs of type and member"_s);
    LayoutFamilies families { Vector<Vector<LayoutID>>(numberOfLayouts), BitVector(numberOfLayouts) };
    listSecondsByFirst(loops, membersByTask.span(), families.members);
    steps.didFinish("members by type"_s);
    for (const Vector<uint32_t>& types : typesOfMembersWithoutLayoutByTask) {
        for (uint32_t type : types)
            families.hasMembersWithoutLayout.quickSet(type);
    }
    return families;
}

static void removeFamilyMembersThatLackRequiredNames(const ParallelLoops& loops, const NamesOfLayoutFamilies& names, LayoutFamilies& families)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(families.members.size()), [&](unsigned type) {
        if (names.required.of(type).empty())
            return;
        families.members[type].removeAllMatching([&](LayoutID member) {
            return names.lacksRequiredName(member, type);
        });
    });
}

static bool canPassForWireType(const LayoutConversions& conversions, LayoutID member, uint32_t type)
{
    if (member.index() >= conversions.checkedSections.layouts.size() || conversions.tellingFields.tellApart(member.index(), type) || conversions.names.lacksRequiredName(member, type))
        return false;
    if (member.index() == type)
        return true;
    auto family = std::ranges::lower_bound(conversions.taggedFamilies, member, { }, &TaggedFamily::maker);
    if (family == conversions.taggedFamilies.end() || family->maker != member)
        return true;
    size_t numberOfTags = 0;
    for (const TaggedLayout& tagged : family->members)
        numberOfTags += tagged.tags.size();
    return !conversions.tellingFields.declaresOnlyLiterals(type, family->tagKey.index(), numberOfTags, [&](const LayoutTag& literal) {
        return family->members.containsIf([&](const TaggedLayout& tagged) {
            return tagged.tags.contains(literal);
        });
    });
}

// Both ascending. A type that every part of a program converts to lists as many members as the program has parts, and each part brings a few: those few are looked up, and the list is not gone through.
static bool listsAllMembersOfFamily(std::span<const LayoutID> listed, std::span<const LayoutID> converted)
{
    if (converted.size() > listed.size())
        return false;
    if (converted.size() * WTF::fastLog2(static_cast<unsigned>(listed.size() + 1)) >= listed.size())
        return std::ranges::includes(listed, converted);
    for (LayoutID member : converted) {
        listed = listed.subspan(std::ranges::lower_bound(listed, member) - listed.begin());
        if (listed.empty() || listed.front() != member)
            return false;
    }
    return true;
}

static LayoutID findMemberOutsideFamily(const LayoutConversions& conversions, const LayoutFamilies& families, LayoutConversion conversion)
{
    const Vector<LayoutID>& converted = families.members[conversion.source];
    const Vector<LayoutID>& listed = families.members[conversion.target];
    if (listsAllMembersOfFamily(listed.span(), converted.span()))
        return { };
    for (LayoutID member : converted) {
        if (!std::ranges::binary_search(listed, member) && passesOnToFamily(conversions, conversion, member) && canPassForWireType(conversions, member, conversion.target))
            return member;
    }
    return { };
}

static Vector<MemberOutsideFamily> findConversionsOutsideFamilies(const ParallelLoops& loops, const LayoutConversions& conversions, const LayoutFamilies& families)
{
    std::span<const Wire::Layout> layouts = conversions.checkedSections.layouts;
    return collectInParallel<MemberOutsideFamily>(loops, static_cast<unsigned>(layouts.size()), [&](unsigned source, Vector<MemberOutsideFamily>& found) {
        if (isWireEnvironment(layouts[source]))
            return;
        forEachWireConversionTarget(conversions.checkedSections, source, [&](uint32_t target) {
            if (isWireEnvironment(layouts[target]))
                return;
            if (LayoutID member = findMemberOutsideFamily(conversions, families, { source, target }))
                found.append({ member, target });
        });
    });
}

static BitVector findTypeParametersWithoutFamily(std::span<const Wire::Layout> layouts, const LayoutFamilies& families)
{
    BitVector result(layouts.size());
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        if (!has(layouts[index], LayoutFlags::IsTypeParameter))
            continue;
        bool hasArgumentWithLayout = std::ranges::any_of(families.members[index], [&](LayoutID member) {
            return member.index() >= layouts.size() || !has(layouts[member.index()], LayoutFlags::IsTypeParameter);
        });
        if (!hasArgumentWithLayout || families.hasMembersWithoutLayout.quickGet(index))
            result.quickSet(index);
    }
    return result;
}

bool WireTypeReader::computeDeclaredMembers()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    std::span<const Wire::Layout> layouts = m_sections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());

    auto numberOfNamespaceLayouts = std::ranges::count_if(m_sections.modules, [](const Wire::Module& module) {
        return module.kind == static_cast<uint32_t>(ModuleKind::SourceText);
    });
    m_numberOfDerivedLayouts = m_program.builtins().layoutCount() + static_cast<uint32_t>(numberOfNamespaceLayouts);

    // The reader's line of the families counts all of these.
    StepTimes steps(m_build, "ReadAndVerify: families of layouts"_s);
    if (std::optional<InternalError> error = findWireLayoutWithInvalidSupertypes(loops, m_sections)) {
        m_build.diagnostics().add(*error);
        return false;
    }
    steps.didFinish("supertypes are valid"_s);

    TellingFields tellingFields(loops, m_sections);
    steps.didFinish("telling fields"_s);
    Vector<Vector<uint32_t>> typesMetByNames = findWireTypesMetByNames(m_build, m_sections, tellingFields);
    steps.didFinish("types met by names"_s);
    LayoutConversions conversions { m_sections, WTF::move(typesMetByNames), tellingFields, collectNamesOfLayoutFamilies(loops, m_sections), { } };
    steps.didFinish("names of families"_s);
    LayoutFamilies families = collectLayoutFamilies(m_build, conversions);
    steps.didFinish("collect families"_s);
    // Numbers the tagged layouts after the derived layouts counted so far, and adds them to that count.
    planTaggedLayouts(families.members, tellingFields);
    steps.didFinish("plan tagged layouts"_s);
    conversions.taggedFamilies = m_program.taggedFamilies.span();
    addNamesOfTaggedFamilies(conversions.names, m_sections, conversions.taggedFamilies);
    removeFamilyMembersThatLackRequiredNames(loops, conversions.names, families);
    steps.didFinish("remove members that lack names"_s);

    if (std::optional<InternalError> error = findWireTypeWithInvalidLists(loops, m_sections, numberOfLayouts + m_numberOfDerivedLayouts)) {
        m_build.diagnostics().add(*error);
        return false;
    }
    steps.didFinish("lists of types are valid"_s);
    Vector<MemberOutsideFamily> outside = findConversionsOutsideFamilies(loops, conversions, families);
    steps.didFinish("families are closed"_s);
    if (!outside.isEmpty()) {
        MemberOutsideFamily first = outside.first();
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::FamilyNotClosed, .layout = first.member, .count = first.type, .detail = makeString("the type is L"_s, first.type) });
        return false;
    }
    m_typeParametersWithoutFamily = findTypeParametersWithoutFamily(layouts, families);

    Vector<uint8_t> isViewWithoutLayouts;
    isViewWithoutLayouts.fill(static_cast<uint8_t>(0), numberOfLayouts);
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        Vector<LayoutID>& members = families.members[index];
        members.removeAllMatching([&](LayoutID member) {
            return member.index() < numberOfLayouts && has(layouts[member.index()], LayoutFlags::IsNeverAllocated);
        });
        if (members.isEmpty()) {
            isViewWithoutLayouts[index] = 1;
            members.append(LayoutID(index));
        }
        ASSERT(std::ranges::adjacent_find(members, std::ranges::greater_equal { }) == members.end());
    });
    m_viewsWithoutLayouts = BitVector(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!isViewWithoutLayouts[index])
            continue;
        if (has(layouts[index], LayoutFlags::IsTypeParameter))
            m_typeParametersWithoutFamily.quickSet(index);
        m_viewsWithoutLayouts.quickSet(index);
    }

    steps.didFinish("type parameters and views"_s);

    // The layout of closures comes last. No type of the wire names it.
    ++m_numberOfDerivedLayouts;

    // The id of a set follows arrival, here as where readTypes() unites these sets: it is only compared for equality.
    constexpr uint32_t familiesPerTask = 64;
    ArenasOfWorkers arenas(m_program.types, loops);
    m_declaredMembers.grow(numberOfLayouts + m_numberOfDerivedLayouts);
    forEachIndexInParallel(loops, (numberOfLayouts + familiesPerTask - 1) / familiesPerTask, [&](unsigned task, unsigned worker) {
        LayoutSets& layoutSets = arenas.of(worker).layoutSets();
        for (uint32_t index = task * familiesPerTask, end = std::min(numberOfLayouts, index + familiesPerTask); index < end; ++index)
            m_declaredMembers[index] = layoutSets.ofSorted(families.members[index].span());
    });
    for (uint32_t index = 0; index < m_numberOfDerivedLayouts; ++index)
        m_declaredMembers[numberOfLayouts + index] = LayoutSets::of(LayoutID(numberOfLayouts + index));
    steps.didFinish("intern the families"_s);
    destroyLists(loops, families.members);
    destroyLists(loops, conversions.typesMetByNames);
    steps.didFinish("free the lists"_s);
    return true;
}

// A family of layouts is a set of layouts and is made of the wire's layouts alone.
// The objects of functions, and the families of functions, which are sets of those, ask for no layout: no object has one until finishObjects(). Neither row interns a type or a key.
bool WireTypeReader::makeObjectsAndComputeFamilies()
{
    constexpr ASCIILiteral stepsSideBySide = "ReadAndVerify: read, side by side"_s;
    bool hasDeclaredMembers = false;
    runLanes(m_build.parallelLoops(), {
        [&] {
            StepTimes steps(m_build, stepsSideBySide);
            hasDeclaredMembers = computeDeclaredMembers();
            // With what it frees on its way out.
            steps.didFinish("families of layouts"_s);
        },
        [&] {
            StepTimes steps(m_build, stepsSideBySide);
            makeObjects();
            steps.didFinish("objects of functions"_s);
            computeFunctionFamilies();
            steps.didFinish("families of functions"_s);
        },
    });
    // The error that one thread meets first that goes through the layouts and then through the functions.
    if (!hasDeclaredMembers)
        return false;
    if (m_errorInFunctionFamilies)
        m_build.diagnostics().add(*m_errorInFunctionFamilies);
    // Any error on record fails the build, so an earlier one may stop the reader too.
    return !m_build.diagnostics().hasErrors();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
