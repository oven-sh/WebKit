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
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include <compare>
#include <functional>
#include <optional>
#include <utility>
#include <wtf/Atomics.h>
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

struct ShareOfPairs {
    size_t firstPiece { 0 };
    size_t endPiece { 0 };
    Vector<uint64_t> pairsByGroup;
    Vector<uint32_t> firstOfGroup;
};

} // anonymous namespace

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

static constexpr size_t leastNamesHeldThroughAnotherType = 16;

struct NamesOfLayoutFamilies {
    bool lacksRequiredName(LayoutID member, uint32_t type) const;
    bool lacksOneOf(LayoutID member, std::span<const uint32_t> names) const;

    uint32_t numberOfWireLayouts { 0 };
    NamesByWireLayout required;
    NamesByWireLayout declared;
    BitVector declaresEveryName;
    uint32_t firstTaggedLayout { 0 };
    Vector<Vector<uint32_t>> declaredByTaggedLayouts;
};

struct LayoutFamilies {
    Vector<LayoutSetID> members;
    BitVector hasMembersWithoutLayout;
};

struct LayoutConversion {
    uint32_t source;
    uint32_t target;
};

struct LayoutsThatMeetTypesByNames {
    static constexpr uint32_t noGroup = UINT32_MAX;

    bool meetNone(uint32_t type) const { return heldAgainstType[type].isEmpty() && (groupOfType[type] == noGroup || withoutTellingFields[groupOfType[type]].isEmpty()); }

    Vector<uint32_t> groupOfType;
    Vector<Vector<LayoutID>> withoutTellingFields;
    Vector<Vector<LayoutID>> heldAgainstType;
};

struct LayoutConversions {
    const Wire::Sections& checkedSections;
    LayoutsThatMeetTypesByNames layoutsThatMeetTypesByNames;
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

static bool holdsEveryValue(const Wire::Sections& sections, uint32_t type)
{
    constexpr uint32_t everyPrimitive = static_cast<uint32_t>(Wire::Primitives::Undefined) | static_cast<uint32_t>(Wire::Primitives::Null) | static_cast<uint32_t>(Wire::Primitives::False) | static_cast<uint32_t>(Wire::Primitives::True)
        | static_cast<uint32_t>(Wire::Primitives::Number) | static_cast<uint32_t>(Wire::Primitives::String) | static_cast<uint32_t>(Wire::Primitives::Symbol) | static_cast<uint32_t>(Wire::Primitives::BigInt);
    return type < sections.types.size() && (sections.types[type].primitives & everyPrimitive) == everyPrimitive && (sections.types[type].flags & static_cast<uint32_t>(TypeFlags::AnyObject));
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
    size_t remainingBases = layouts.size();
    for (uint32_t current = layout; current != Wire::none && remainingBases--; current = layouts[current].base) {
        for (const Wire::Field& field : wireFieldsOf(checkedSections, layouts[current])) {
            if (isWireFieldNamedByProgram(field) && isWanted(field.key))
                names.append(field.key);
        }
    }
}

static void appendNamesAlongWireBases(const Wire::Sections& checkedSections, std::span<const LayoutID> nearestAboveWithWantedName, uint32_t layout, Vector<uint32_t>& names, NOESCAPE const Invocable<bool(uint32_t)> auto& isWanted)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    size_t remainingBases = layouts.size();
    for (LayoutID current(layout); current && remainingBases--; current = nearestAboveWithWantedName[current.index()]) {
        for (const Wire::Field& field : wireFieldsOf(checkedSections, layouts[current.index()])) {
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

static uint64_t addNamesToHashOfWireLayout(uint64_t hash, std::span<const uint32_t> names)
{
    hash = addToInternedHash(hash, names.size());
    for (uint32_t name : names)
        hash = addToInternedHash(hash, name);
    return hash;
}

static uint64_t addNamesButOneToHashOfWireLayout(uint64_t hash, std::span<const uint32_t> names, uint32_t leftOut)
{
    hash = addToInternedHash(hash, names.size() - 1);
    for (uint32_t name : names) {
        if (name != leftOut)
            hash = addToInternedHash(hash, name);
    }
    return hash;
}

static bool areNamesButOne(std::span<const uint32_t> fewer, std::span<const uint32_t> names, uint32_t leftOut)
{
    if (fewer.size() + 1 != names.size())
        return false;
    size_t place = 0;
    for (uint32_t name : names) {
        if (name == leftOut)
            continue;
        if (place == fewer.size() || fewer[place++] != name)
            return false;
    }
    return place == fewer.size();
}

static bool holdsAllNames(std::span<const uint32_t> list, std::span<const uint32_t> names)
{
    return names.size() == 1 ? std::ranges::binary_search(list, names[0]) : std::ranges::includes(list, names);
}

bool NamesOfLayoutFamilies::lacksRequiredName(LayoutID member, uint32_t type) const
{
    return lacksOneOf(member, required.of(type));
}

bool NamesOfLayoutFamilies::lacksOneOf(LayoutID member, std::span<const uint32_t> names) const
{
    if (names.empty())
        return false;
    uint32_t index = member.index();
    if (index < numberOfWireLayouts)
        return declaresEveryName.quickGet(index) && !holdsAllNames(declared.of(index), names);
    if (index < firstTaggedLayout || index - firstTaggedLayout >= declaredByTaggedLayouts.size())
        return false;
    return !holdsAllNames(declaredByTaggedLayouts[index - firstTaggedLayout].span(), names);
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

static LayoutsThatMeetTypesByNames findLayoutsThatMeetWireTypesByNames(const BuildContext& build, const Wire::Sections& checkedSections, const TellingFields& tellingFields)
{
    const ParallelLoops& loops = build.parallelLoops();
    StepTimes steps(build, "ReadAndVerify: types met by names"_s);
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());
    size_t numberOfKeys = checkedSections.propertyKeys.size();

    auto isAsked = [&](const Wire::Field& field) {
        return isWireFieldNamedByProgram(field) && !has(field, FieldFlags::IsNeverWritten) && field.key < numberOfKeys;
    };
    auto isKnownByNamesAlone = [&](uint32_t type) {
        return std::ranges::all_of(wireFieldsOf(checkedSections, layouts[type]), [&](const Wire::Field& field) {
            bool nothingIsBelievedOfIt = holdsEveryValue(checkedSections, field.declared.type) || (field.declared.flags & static_cast<uint32_t>(PositionFlags::IsExpected));
            return !isAsked(field) || (!has(field, FieldFlags::IsRequired) && nothingIsBelievedOfIt);
        });
    };
    auto forEachRequiredMember = [&](uint32_t type, NOESCAPE const Invocable<void(const Wire::Field&)> auto& functor) {
        bool asksForEveryName = isKnownByNamesAlone(type);
        for (const Wire::Field& field : wireFieldsOf(checkedSections, layouts[type])) {
            if (isAsked(field) && (has(field, FieldFlags::IsRequired) || asksForEveryName))
                functor(field);
        }
    };
    Vector<uint8_t> isRequiredBySomeType(FillWith { }, numberOfKeys, static_cast<uint8_t>(0));
    forEachRangeOfIndicesInParallel(loops, numberOfLayouts, [&](size_t first, size_t end) {
        for (uint32_t type = static_cast<uint32_t>(first); type < end; ++type) {
            if (!isStructuralWireType(layouts[type]))
                continue;
            forEachRequiredMember(type, [&](const Wire::Field& field) {
                AtomicRef(isRequiredBySomeType[field.key]).store(1, std::memory_order_relaxed);
            });
        }
    });

    steps.didFinish("names that some type requires"_s);

    auto isWantedName = [&](uint32_t name) {
        return name < numberOfKeys && isRequiredBySomeType[name];
    };
    Vector<LayoutID> nearestAboveWithWantedName = findNearestWantedAbove(numberOfLayouts, [&](LayoutID id) {
        return layouts[id.index()].base == Wire::none ? LayoutID() : LayoutID(layouts[id.index()].base);
    }, [&](LayoutID id) {
        return std::ranges::any_of(wireFieldsOf(checkedSections, layouts[id.index()]), [&](const Wire::Field& field) {
            return isWireFieldNamedByProgram(field) && isWantedName(field.key);
        });
    });
    NamesByWireLayout names(loops, numberOfLayouts, [&](uint32_t index, Vector<uint32_t>& found) {
        const Wire::Layout& layout = layouts[index];
        if (!isWireLayoutOfObjects(layout) || has(layout, LayoutFlags::IsNeverAllocated) || has(layout, LayoutFlags::IsWrittenUnderDictionaryType))
            return;
        appendNamesAlongWireBases(checkedSections, nearestAboveWithWantedName.span(), index, found, isWantedName);
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

    auto isToldByNameThatTypeLacks = [&](uint32_t layout, uint32_t type) {
        bool lacksOne = false;
        tellingFields.forEachKeyOfOwnTellingField(layout, [&](uint32_t key) {
            lacksOne = lacksOne || !std::ranges::contains(wireFieldsOf(checkedSections, layouts[type]), key, &Wire::Field::key);
        });
        return lacksOne;
    };
    NamesByWireLayout requiredOfRecords(loops, numberOfLayouts, [&](uint32_t type, Vector<uint32_t>& found) {
        if (!isStructuralWireType(layouts[type]))
            return;
        forEachRequiredMember(type, [&](const Wire::Field& field) {
            found.append(field.key);
        });
    });
    NamesByWireLayout requiredOfInstances(loops, numberOfLayouts, [&](uint32_t type, Vector<uint32_t>& found) {
        if (!isStructuralWireType(layouts[type]))
            return;
        bool isDeclaredNowhere = layouts[type].name == Wire::none && layouts[type].module == Wire::none;
        forEachRequiredMember(type, [&](const Wire::Field& field) {
            if (!isDeclaredNowhere || isRequiredWireField(field))
                found.append(field.key);
        });
    });
    Vector<uint64_t> typesByHashOfNames;
    for (uint32_t type = 0; type < numberOfLayouts; ++type) {
        if (requiredOfInstances.of(type).empty())
            continue;
        uint64_t hash = addNamesToHashOfWireLayout(addNamesToHashOfWireLayout(0, requiredOfInstances.of(type)), requiredOfRecords.of(type));
        typesByHashOfNames.append(static_cast<uint64_t>(finishInternedHash(hash)) << 32 | type);
    }
    AOT::sort(typesByHashOfNames);
    Vector<uint32_t> typesByNames = typesByHashOfNames.map([](uint64_t hashAndType) {
        return static_cast<uint32_t>(hashAndType);
    });
    auto requireSameNames = [&](uint32_t first, uint32_t second) {
        return std::ranges::equal(requiredOfInstances.of(first), requiredOfInstances.of(second)) && std::ranges::equal(requiredOfRecords.of(first), requiredOfRecords.of(second));
    };
    Vector<uint32_t> firstOfGroup;
    for (uint32_t place = 0; place < typesByNames.size(); ++place) {
        if (!place || !requireSameNames(typesByNames[place - 1], typesByNames[place]))
            firstOfGroup.append(place);
    }
    uint32_t numberOfGroups = static_cast<uint32_t>(firstOfGroup.size());
    firstOfGroup.append(static_cast<uint32_t>(typesByNames.size()));
    steps.didFinish("types by the names that they require"_s);

    LayoutsThatMeetTypesByNames result;
    result.groupOfType.fill(LayoutsThatMeetTypesByNames::noGroup, numberOfLayouts);
    result.withoutTellingFields.grow(numberOfGroups);
    result.heldAgainstType.grow(numberOfLayouts);
    Vector<Vector<uint32_t>> withTellingFieldsOfGroup(numberOfGroups);
    auto rarestNameOf = [&](std::span<const uint32_t> ofInstances) {
        uint32_t rarest = ofInstances[0];
        for (uint32_t name : ofInstances) {
            if (layoutsWithName[name].size() < layoutsWithName[rarest].size())
                rarest = name;
        }
        return rarest;
    };
    Vector<uint32_t> groupOfPlace(FillWith { }, typesByNames.size(), 0u);
    for (uint32_t group = 0; group < numberOfGroups; ++group) {
        for (uint32_t place = firstOfGroup[group]; place < firstOfGroup[group + 1]; ++place)
            groupOfPlace[place] = group;
    }
    Vector<uint32_t> groupThatRequiresAllButRarestName(FillWith { }, numberOfGroups, LayoutsThatMeetTypesByNames::noGroup);
    forEachIndexInParallel(loops, numberOfGroups, [&](unsigned group) {
        uint32_t firstType = typesByNames[firstOfGroup[group]];
        std::span<const uint32_t> required = requiredOfInstances.of(firstType);
        if (required.size() < leastNamesHeldThroughAnotherType || !std::ranges::equal(required, requiredOfRecords.of(firstType)))
            return;
        uint32_t rarest = rarestNameOf(required);
        uint64_t hash = static_cast<uint64_t>(finishInternedHash(addNamesButOneToHashOfWireLayout(addNamesButOneToHashOfWireLayout(0, required, rarest), required, rarest))) << 32;
        for (size_t place = std::ranges::lower_bound(typesByHashOfNames, hash) - typesByHashOfNames.begin(); place < typesByHashOfNames.size() && typesByHashOfNames[place] >> 32 == hash >> 32; ++place) {
            uint32_t other = typesByNames[place];
            if (areNamesButOne(requiredOfInstances.of(other), required, rarest) && areNamesButOne(requiredOfRecords.of(other), required, rarest)) {
                groupThatRequiresAllButRarestName[group] = groupOfPlace[place];
                return;
            }
        }
    });
    auto takeLayoutThatMeetsGroup = [&](unsigned group, uint32_t layout) {
        if (tellingFields.tellsNothingApartFrom(layout))
            result.withoutTellingFields[group].append(LayoutID(layout));
        else
            withTellingFieldsOfGroup[group].append(layout);
    };
    auto signatureOf = [](std::span<const uint32_t> namesOfLayout) {
        uint64_t signature = 0;
        for (uint32_t name : namesOfLayout)
            signature |= 1ull << (name * 0x9e3779b1u >> 26);
        return signature;
    };
    Vector<uint64_t> signatures;
    signatures.grow(numberOfLayouts);
    forEachRangeOfIndicesInParallel(loops, numberOfLayouts, [&](size_t first, size_t end) {
        for (uint32_t layout = static_cast<uint32_t>(first); layout < end; ++layout)
            signatures[layout] = signatureOf(names.of(layout));
    });
    forEachIndexInParallel(loops, numberOfGroups, [&](unsigned group) {
        for (uint32_t place = firstOfGroup[group]; place < firstOfGroup[group + 1]; ++place)
            result.groupOfType[typesByNames[place]] = group;
        if (groupThatRequiresAllButRarestName[group] != LayoutsThatMeetTypesByNames::noGroup)
            return;
        uint32_t firstType = typesByNames[firstOfGroup[group]];
        std::span<const uint32_t> ofRecords = requiredOfRecords.of(firstType);
        std::span<const uint32_t> ofInstances = requiredOfInstances.of(firstType);
        uint64_t signatureOfRecords = signatureOf(ofRecords);
        uint64_t signatureOfInstances = signatureOf(ofInstances);
        for (uint32_t layout : layoutsWithName[rarestNameOf(ofInstances)]) {
            bool declaresEveryName = wireLayoutDeclaresEveryName(layouts[layout]);
            if ((declaresEveryName ? signatureOfRecords : signatureOfInstances) & ~signatures[layout])
                continue;
            if (std::ranges::includes(names.of(layout), declaresEveryName ? ofRecords : ofInstances))
                takeLayoutThatMeetsGroup(group, layout);
        }
    });
    Vector<uint32_t> groupsMetThroughOthers;
    for (uint32_t group = 0; group < numberOfGroups; ++group) {
        if (groupThatRequiresAllButRarestName[group] != LayoutsThatMeetTypesByNames::noGroup)
            groupsMetThroughOthers.append(group);
    }
    auto numberOfNamesOf = [&](uint32_t group) {
        return requiredOfInstances.of(typesByNames[firstOfGroup[group]]).size();
    };
    AOT::stableSort(groupsMetThroughOthers, { }, numberOfNamesOf);
    for (uint32_t group : groupsMetThroughOthers) {
        uint32_t other = groupThatRequiresAllButRarestName[group];
        std::span<const LayoutID> without = result.withoutTellingFields[other].span();
        std::span<const uint32_t> with = withTellingFieldsOfGroup[other].span();
        for (uint32_t layout : layoutsWithName[rarestNameOf(requiredOfInstances.of(typesByNames[firstOfGroup[group]]))]) {
            while (!without.empty() && without.front().index() < layout)
                without = without.subspan(1);
            while (!with.empty() && with.front() < layout)
                with = with.subspan(1);
            if ((!without.empty() && without.front().index() == layout) || (!with.empty() && with.front() == layout))
                takeLayoutThatMeetsGroup(group, layout);
        }
    }
    steps.didFinish("layouts by group"_s);
    forEachIndexInParallel(loops, static_cast<unsigned>(typesByNames.size()), [&](unsigned place) {
        uint32_t type = typesByNames[place];
        uint32_t group = result.groupOfType[type];
        const Vector<uint32_t>& withTellingFields = withTellingFieldsOfGroup[group];
        Vector<LayoutID>& found = result.heldAgainstType[type];
        auto holdAgainstType = [&](uint32_t layout) {
            if (layout != type && !tellingFields.tellApart(layout, type) && !isToldByNameThatTypeLacks(layout, type))
                found.append(LayoutID(layout));
        };
        if (tellingFields.tellsNothingApartFrom(type)) {
            for (uint32_t layout : withTellingFields)
                holdAgainstType(layout);
            return;
        }
        result.groupOfType[type] = LayoutsThatMeetTypesByNames::noGroup;
        size_t next = 0;
        for (LayoutID layout : result.withoutTellingFields[group]) {
            for (; next < withTellingFields.size() && withTellingFields[next] < layout.index(); ++next)
                holdAgainstType(withTellingFields[next]);
            holdAgainstType(layout.index());
        }
        for (; next < withTellingFields.size(); ++next)
            holdAgainstType(withTellingFields[next]);
    });
    steps.didFinish("layouts by type"_s);
    destroyLists(loops, layoutsWithName);
    destroyLists(loops, withTellingFieldsOfGroup);
    steps.didFinish("free the lists"_s);
    return result;
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

static bool passesEveryMemberOnToFamily(const LayoutConversions& conversions, LayoutConversion conversion)
{
    return !wireTypeRequiresNoName(conversions, conversion.source) || wireTypeRequiresNoName(conversions, conversion.target);
}

static bool passesOnToFamily(const LayoutConversions& conversions, LayoutConversion conversion, LayoutID member)
{
    if (passesEveryMemberOnToFamily(conversions, conversion))
        return true;
    return member.index() == conversion.source && !has(conversions.checkedSections.layouts[conversion.source], LayoutFlags::IsNeverAllocated);
}

static LayoutFamilies collectLayoutFamilies(const BuildContext& build, const LayoutConversions& conversions, ArenasOfWorkers& arenas)
{
    StepTimes steps(build, "ReadAndVerify: collect families"_s);
    const Wire::Sections& checkedSections = conversions.checkedSections;
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());

    auto forEachTargetOfObjects = [&](uint32_t source, NOESCAPE const Invocable<void(uint32_t)> auto& functor) {
        if (isWireEnvironment(layouts[source]))
            return;
        forEachWireConversionTarget(checkedSections, source, [&](uint32_t target) {
            if (!isWireEnvironment(layouts[target]))
                functor(target);
        });
    };
    constexpr uint32_t layoutsPerTask = 512;
    unsigned numberOfTasks = (numberOfLayouts + layoutsPerTask - 1) / layoutsPerTask;
    Vector<Vector<uint64_t>> sourcesOfTargets(numberOfTasks);
    forEachIndexInParallel(build.parallelLoops(), numberOfTasks, [&](unsigned task) {
        for (uint32_t index = task * layoutsPerTask, end = std::min(numberOfLayouts, index + layoutsPerTask); index < end; ++index) {
            forEachTargetOfObjects(index, [&](uint32_t target) {
                sourcesOfTargets[task].append(pairOfWireIndexes(target, index));
            });
        }
    });
    Vector<Vector<uint32_t>> sources(numberOfLayouts);
    listSecondsByFirst(build.parallelLoops(), sourcesOfTargets.span(), sources);
    const LayoutsThatMeetTypesByNames& metByNames = conversions.layoutsThatMeetTypesByNames;
    Vector<LayoutSetID> metByNamesOfGroup(metByNames.withoutTellingFields.size());
    forEachIndexInParallel(build.parallelLoops(), static_cast<unsigned>(metByNamesOfGroup.size()), [&](unsigned group, unsigned worker) {
        metByNamesOfGroup[group] = arenas.of(worker).layoutSets().ofSorted(metByNames.withoutTellingFields[group].span());
    });
    Vector<uint32_t> numberOfSourcesLeft(numberOfLayouts, [&](size_t index) {
        return static_cast<uint32_t>(sources[index].size());
    });
    steps.didFinish("what converts to each type"_s);

    LayoutFamilies families { Vector<LayoutSetID>(numberOfLayouts), BitVector(numberOfLayouts) };
    Vector<LayoutSetID> membersAsked(numberOfLayouts);
    auto makeFamilyOf = [&](uint32_t type, LayoutSets& sets) {
        bool standsAlone = sources[type].isEmpty() && metByNames.meetNone(type) && conversions.tellingFields.tellsNothingApartFrom(type);
        if (standsAlone || isWireEnvironment(layouts[type]))
            return LayoutSets::of(LayoutID(type));
        LayoutSetID metByNamesOfItsGroup = metByNames.groupOfType[type] == LayoutsThatMeetTypesByNames::noGroup ? LayoutSetID() : metByNamesOfGroup[metByNames.groupOfType[type]];
        if (sources[type].isEmpty() && metByNames.heldAgainstType[type].isEmpty() && metByNamesOfItsGroup)
            return sets.contains(metByNamesOfItsGroup, LayoutID(type)) ? metByNamesOfItsGroup : sets.with(metByNamesOfItsGroup, LayoutID(type));
        Vector<LayoutSetID, 8> wholeFamilies;
        if (metByNamesOfItsGroup)
            wholeFamilies.append(metByNamesOfItsGroup);
        Vector<LayoutID, 8> membersByThemselves;
        membersByThemselves.append(LayoutID(type));
        membersByThemselves.append(metByNames.heldAgainstType[type].span());
        for (uint32_t source : sources[type]) {
            if (passesEveryMemberOnToFamily(conversions, { source, type }))
                wholeFamilies.append(families.members[source]);
            else if (passesOnToFamily(conversions, { source, type }, LayoutID(source)) && sets.contains(families.members[source], LayoutID(source)))
                membersByThemselves.append(LayoutID(source));
        }
        wholeFamilies.append(sets.ofUnsorted(membersByThemselves.span()));
        LayoutSetID all = sets.unionOf(wholeFamilies.span());
        if (conversions.tellingFields.tellsNothingApartFrom(type))
            return all;
        LayoutSetID notAskedYet = sets.differenceOf(all, std::exchange(membersAsked[type], all));
        return sets.unionOf(families.members[type], sets.filtered(notAskedYet, [&](LayoutID member) {
            return !conversions.tellingFields.tellApart(member.index(), type);
        }));
    };

    Vector<uint32_t> ready;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!numberOfSourcesLeft[index])
            ready.append(index);
    }
    constexpr size_t typesPerTask = 64;
    Vector<uint32_t> readyNext;
    while (!ready.isEmpty()) {
        forEachIndexInParallel(build.parallelLoops(), static_cast<unsigned>((ready.size() + typesPerTask - 1) / typesPerTask), [&](unsigned task, unsigned worker) {
            LayoutSets& sets = arenas.of(worker).layoutSets();
            for (size_t place = task * typesPerTask, end = std::min(ready.size(), place + typesPerTask); place < end; ++place)
                families.members[ready[place]] = makeFamilyOf(ready[place], sets);
        });
        for (uint32_t type : ready) {
            forEachTargetOfObjects(type, [&](uint32_t target) {
                if (!--numberOfSourcesLeft[target])
                    readyNext.append(target);
            });
        }
        std::swap(ready, readyNext);
        readyNext.shrink(0);
    }
    LayoutSets& sets = arenas.of(0).layoutSets();
    Vector<uint32_t> waiting;
    BitVector isWaiting(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (numberOfSourcesLeft[index]) {
            waiting.append(index);
            isWaiting.quickSet(index);
        }
    }
    for (size_t place = 0; place < waiting.size(); ++place) {
        uint32_t type = waiting[place];
        isWaiting.quickClear(type);
        LayoutSetID family = makeFamilyOf(type, sets);
        if (std::exchange(families.members[type], family) == family)
            continue;
        forEachTargetOfObjects(type, [&](uint32_t target) {
            if (!isWaiting.quickSet(target))
                waiting.append(target);
        });
    }
    steps.didFinish("families"_s);

    Vector<LayoutID> layoutsWithMembersWithoutLayout;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!isWireEnvironment(layouts[index]) && has(layouts[index], LayoutFlags::HasMembersWithoutLayout))
            layoutsWithMembersWithoutLayout.append(LayoutID(index));
    }
    if (LayoutSetID all = sets.ofSorted(layoutsWithMembersWithoutLayout.span())) {
        for (uint32_t type = 0; type < numberOfLayouts; ++type) {
            if (sets.overlap(families.members[type], all))
                families.hasMembersWithoutLayout.quickSet(type);
        }
    }
    destroyLists(build.parallelLoops(), sources);
    return families;
}

static void removeFamilyMembersThatLackRequiredNames(const ParallelLoops& loops, ArenasOfWorkers& arenas, const NamesOfLayoutFamilies& names, LayoutFamilies& families)
{
    Vector<uint64_t> typesByHash;
    for (uint32_t type = 0; type < families.members.size(); ++type) {
        if (names.required.of(type).empty())
            continue;
        uint64_t hash = addNamesToHashOfWireLayout(families.members[type].bits(), names.required.of(type));
        typesByHash.append(static_cast<uint64_t>(finishInternedHash(hash)) << 32 | type);
    }
    AOT::sort(typesByHash);
    Vector<uint32_t> types = typesByHash.map([](uint64_t hashAndType) {
        return static_cast<uint32_t>(hashAndType);
    });
    auto loseSameMembers = [&](uint32_t first, uint32_t second) {
        return families.members[first] == families.members[second] && std::ranges::equal(names.required.of(first), names.required.of(second));
    };
    Vector<uint32_t> firstOfGroup;
    for (uint32_t place = 0; place < types.size(); ++place) {
        if (!place || !loseSameMembers(types[place - 1], types[place]))
            firstOfGroup.append(place);
    }
    unsigned numberOfGroups = static_cast<unsigned>(firstOfGroup.size());
    firstOfGroup.append(static_cast<uint32_t>(types.size()));

    constexpr uint32_t noGroup = UINT32_MAX;
    uint32_t endOfNames = 0;
    Vector<uint64_t> longTypesByHashOfNames;
    Vector<uint32_t> groupOfType(FillWith { }, families.members.size(), noGroup);
    for (unsigned group = 0; group < numberOfGroups; ++group) {
        uint32_t type = types[firstOfGroup[group]];
        groupOfType[type] = group;
        std::span<const uint32_t> required = names.required.of(type);
        if (required.size() + 1 < leastNamesHeldThroughAnotherType)
            continue;
        endOfNames = std::max(endOfNames, required.back() + 1);
        longTypesByHashOfNames.append(static_cast<uint64_t>(finishInternedHash(addNamesToHashOfWireLayout(0, required))) << 32 | type);
    }
    AOT::sort(longTypesByHashOfNames);
    Vector<uint32_t> numberOfLongTypesThatRequire(FillWith { }, endOfNames, 0u);
    for (uint64_t hashAndType : longTypesByHashOfNames) {
        for (uint32_t name : names.required.of(static_cast<uint32_t>(hashAndType)))
            ++numberOfLongTypesThatRequire[name];
    }
    struct OtherGroup {
        uint32_t group { noGroup };
        uint32_t nameBeside { 0 };
    };
    Vector<OtherGroup> groupThatRequiresAllButOneName(numberOfGroups);
    Vector<uint8_t> isAskedByAnotherGroup(FillWith { }, numberOfGroups, static_cast<uint8_t>(0));
    forEachIndexInParallel(loops, numberOfGroups, [&](unsigned group) {
        std::span<const uint32_t> required = names.required.of(types[firstOfGroup[group]]);
        if (required.size() < leastNamesHeldThroughAnotherType)
            return;
        uint32_t rarest = required[0];
        for (uint32_t name : required) {
            if (numberOfLongTypesThatRequire[name] < numberOfLongTypesThatRequire[rarest])
                rarest = name;
        }
        uint64_t hash = static_cast<uint64_t>(finishInternedHash(addNamesButOneToHashOfWireLayout(0, required, rarest))) << 32;
        for (size_t place = std::ranges::lower_bound(longTypesByHashOfNames, hash) - longTypesByHashOfNames.begin(); place < longTypesByHashOfNames.size() && longTypesByHashOfNames[place] >> 32 == hash >> 32; ++place) {
            uint32_t other = static_cast<uint32_t>(longTypesByHashOfNames[place]);
            if (areNamesButOne(names.required.of(other), required, rarest)) {
                groupThatRequiresAllButOneName[group] = { groupOfType[other], rarest };
                AtomicRef(isAskedByAnotherGroup[groupOfType[other]]).store(1, std::memory_order_relaxed);
                return;
            }
        }
    });
    Vector<Vector<uint32_t>> membersLeftInGroup(numberOfGroups);
    auto removeFrom = [&](unsigned group, unsigned worker) {
        uint32_t type = types[firstOfGroup[group]];
        OtherGroup other = groupThatRequiresAllButOneName[group];
        LayoutSetID left = arenas.of(worker).layoutSets().filtered(families.members[type], [&](LayoutID member) {
            bool holdsNamesOfOther = other.group != noGroup && std::ranges::binary_search(membersLeftInGroup[other.group], member.index());
            bool stays = holdsNamesOfOther ? !names.lacksOneOf(member, singleElementSpan(other.nameBeside)) : !names.lacksRequiredName(member, type);
            if (stays && isAskedByAnotherGroup[group])
                membersLeftInGroup[group].append(member.index());
            return stays;
        });
        for (uint32_t sameAs : types.subspan(firstOfGroup[group], firstOfGroup[group + 1] - firstOfGroup[group]))
            families.members[sameAs] = left;
    };
    forEachIndexInParallel(loops, numberOfGroups, [&](unsigned group, unsigned worker) {
        if (groupThatRequiresAllButOneName[group].group == noGroup)
            removeFrom(group, worker);
    });
    Vector<uint32_t> groupsHeldThroughOthers;
    for (unsigned group = 0; group < numberOfGroups; ++group) {
        if (groupThatRequiresAllButOneName[group].group != noGroup)
            groupsHeldThroughOthers.append(group);
    }
    AOT::stableSort(groupsHeldThroughOthers, { }, [&](uint32_t group) {
        return names.required.of(types[firstOfGroup[group]]).size();
    });
    for (uint32_t group : groupsHeldThroughOthers)
        removeFrom(group, 0);
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

static LayoutID findMemberOutsideFamily(const TypeTable& types, const LayoutConversions& conversions, const LayoutFamilies& families, LayoutConversion conversion)
{
    IDSetView converted = types.at(families.members[conversion.source]).untyped();
    IDSetView listed = types.at(families.members[conversion.target]).untyped();
    if (isSubset(converted, listed))
        return { };
    LayoutID outside;
    forEachAddedMember(converted, listed, [&](uint32_t member) {
        if (!outside && passesOnToFamily(conversions, conversion, LayoutID(member)) && canPassForWireType(conversions, LayoutID(member), conversion.target))
            outside = LayoutID(member);
    });
    return outside;
}

static Vector<MemberOutsideFamily> findConversionsOutsideFamilies(const ParallelLoops& loops, const TypeTable& types, const LayoutConversions& conversions, const LayoutFamilies& families)
{
    std::span<const Wire::Layout> layouts = conversions.checkedSections.layouts;
    return collectInParallel<MemberOutsideFamily>(loops, static_cast<unsigned>(layouts.size()), [&](unsigned source, Vector<MemberOutsideFamily>& found) {
        if (isWireEnvironment(layouts[source]))
            return;
        forEachWireConversionTarget(conversions.checkedSections, source, [&](uint32_t target) {
            if (isWireEnvironment(layouts[target]))
                return;
            if (LayoutID member = findMemberOutsideFamily(types, conversions, families, { source, target }))
                found.append({ member, target });
        });
    });
}

static BitVector findTypeParametersWithoutFamily(const TypeTable& types, std::span<const Wire::Layout> layouts, const LayoutFamilies& families)
{
    BitVector result(layouts.size());
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        if (!has(layouts[index], LayoutFlags::IsTypeParameter))
            continue;
        bool hasArgumentWithLayout = types.at(families.members[index]).anyOf([&](LayoutID member) {
            return member.index() >= layouts.size() || !has(layouts[member.index()], LayoutFlags::IsTypeParameter);
        });
        if (!hasArgumentWithLayout || families.hasMembersWithoutLayout.quickGet(index))
            result.quickSet(index);
    }
    return result;
}

static bool isWireClass(const Wire::Layout& layout)
{
    return static_cast<LayoutKind>(layout.kind) == LayoutKind::Instance && layout.constructor != Wire::none && !has(layout, LayoutFlags::IsConstructorFunction);
}

static Vector<LayoutSetID> uniteMembersOfParentsOfOneClass(LayoutSets& sets, const Wire::Sections& checkedSections, std::span<const LayoutSetID> members)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    Vector<uint32_t> leaders;
    auto leaderOf = [&](uint32_t index) {
        while (leaders[index] != index)
            index = leaders[index] = leaders[leaders[index]];
        return index;
    };
    Vector<uint32_t> parents;
    for (const Wire::Layout& layout : layouts) {
        if (layout.base != Wire::none || !has(layout, LayoutFlags::HasHeritage) || !isWireClass(layout))
            continue;
        uint32_t earlier = Wire::none;
        for (uint32_t parent : wireSupertypesOf(checkedSections, layout)) {
            if (!isWireClass(layouts[parent]))
                continue;
            if (leaders.isEmpty()) {
                leaders = Vector<uint32_t>(layouts.size(), [](size_t index) {
                    return static_cast<uint32_t>(index);
                });
            }
            parents.append(parent);
            if (earlier != Wire::none)
                leaders[leaderOf(parent)] = leaderOf(earlier);
            earlier = parent;
        }
    }
    AOT::sort(parents, { }, [&](uint32_t parent) {
        return std::pair { leaderOf(parent), parent };
    });
    parents.shrink(parents.size() - std::ranges::unique(parents).size());

    Vector<LayoutSetID> united;
    for (size_t begin = 0; begin < parents.size();) {
        size_t end = begin;
        LayoutSetID all;
        for (; end < parents.size() && leaderOf(parents[end]) == leaderOf(parents[begin]); ++end)
            all = sets.unionOf(all, members[parents[end]]);
        if (end - begin > 1)
            united.append(all);
        begin = end;
    }
    return united;
}

bool WireTypeReader::computeDeclaredMembers()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    std::span<const Wire::Layout> layouts = m_sections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());

    auto numberOfNamespaceLayouts = std::ranges::count_if(m_sections.modules, [](const Wire::Module& module) {
        return module.kind == static_cast<uint32_t>(ModuleKind::SourceText) || module.kind == static_cast<uint32_t>(ModuleKind::RuntimeSource);
    });
    m_numberOfDerivedLayouts += static_cast<uint32_t>(numberOfNamespaceLayouts);

    StepTimes steps(m_build, "ReadAndVerify: families of layouts"_s);
    if (std::optional<InternalError> error = findWireLayoutWithInvalidSupertypes(loops, m_sections)) {
        m_build.diagnostics().add(*error);
        return false;
    }
    steps.didFinish("supertypes are valid"_s);

    TellingFields tellingFields(loops, m_sections);
    steps.didFinish("telling fields"_s);
    LayoutsThatMeetTypesByNames layoutsThatMeetTypesByNames;
    NamesOfLayoutFamilies namesOfFamilies;
    runLanes(loops, {
        [&] {
            layoutsThatMeetTypesByNames = findLayoutsThatMeetWireTypesByNames(m_build, m_sections, tellingFields);
        },
        [&] {
            namesOfFamilies = collectNamesOfLayoutFamilies(m_build.parallelLoops(), m_sections);
        },
    });
    steps.didFinish("types met by names beside names of families"_s);
    LayoutConversions conversions { m_sections, WTF::move(layoutsThatMeetTypesByNames), tellingFields, WTF::move(namesOfFamilies), { } };
    ArenasOfWorkers arenas(m_program.types, loops);
    LayoutFamilies families = collectLayoutFamilies(m_build, conversions, arenas);
    steps.didFinish("collect families"_s);
    planTaggedLayouts(arenas, families.members, tellingFields);
    steps.didFinish("plan tagged layouts"_s);
    conversions.taggedFamilies = m_program.taggedFamilies.span();
    addNamesOfTaggedFamilies(conversions.names, m_sections, conversions.taggedFamilies);
    removeFamilyMembersThatLackRequiredNames(loops, arenas, conversions.names, families);
    steps.didFinish("remove members that lack names"_s);

    if (std::optional<InternalError> error = findWireTypeWithInvalidLists(loops, m_sections, numberOfLayouts + m_numberOfDerivedLayouts)) {
        m_build.diagnostics().add(*error);
        return false;
    }
    steps.didFinish("lists of types are valid"_s);
    Vector<MemberOutsideFamily> outside = findConversionsOutsideFamilies(loops, m_program.types, conversions, families);
    steps.didFinish("families are closed"_s);
    if (!outside.isEmpty()) {
        MemberOutsideFamily first = outside.first();
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::FamilyNotClosed, .layout = first.member, .count = first.type, .detail = makeString("the type is L"_s, first.type) });
        return false;
    }
    m_typeParametersWithoutFamily = findTypeParametersWithoutFamily(m_program.types, layouts, families);

    Vector<LayoutID> layoutsNeverAllocated;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (has(layouts[index], LayoutFlags::IsNeverAllocated))
            layoutsNeverAllocated.append(LayoutID(index));
    }
    LayoutSetID allNeverAllocated = arenas.of(0).layoutSets().ofSorted(layoutsNeverAllocated.span());
    Vector<uint8_t> isViewWithoutLayouts;
    isViewWithoutLayouts.fill(static_cast<uint8_t>(0), numberOfLayouts);
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
        LayoutSetID& members = families.members[index];
        LayoutSets& sets = arenas.of(worker).layoutSets();
        LayoutSets::View before = sets.at(members);
        bool holdsOneNeverAllocated = !before.isList() || before.anyOf([&](LayoutID member) {
            return member.index() < numberOfLayouts && has(layouts[member.index()], LayoutFlags::IsNeverAllocated);
        });
        if (holdsOneNeverAllocated)
            members = sets.differenceOf(members, allNeverAllocated);
        if (has(layouts[index], LayoutFlags::IsNeverAllocated) && std::ranges::any_of(wireFieldsOf(conversions.checkedSections, layouts[index]), [](const Wire::Field& field) { return has(field, FieldFlags::WasRequiredAndRead); }))
            members = { };
        if (!members) {
            isViewWithoutLayouts[index] = 1;
            members = LayoutSets::of(LayoutID(index));
        }
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

    ++m_numberOfDerivedLayouts;

    m_program.setMembersOfParentsOfOneClass(uniteMembersOfParentsOfOneClass(arenas.of(0).layoutSets(), m_sections, families.members.span()));
    m_declaredMembers = WTF::move(families.members);
    m_declaredMembers.grow(numberOfLayouts + m_numberOfDerivedLayouts);
    for (uint32_t index = 0; index < m_numberOfDerivedLayouts; ++index)
        m_declaredMembers[numberOfLayouts + index] = LayoutSets::of(LayoutID(numberOfLayouts + index));
    destroyLists(loops, conversions.layoutsThatMeetTypesByNames.withoutTellingFields);
    destroyLists(loops, conversions.layoutsThatMeetTypesByNames.heldAgainstType);
    steps.didFinish("free the lists"_s);
    return true;
}

bool WireTypeReader::reportErrorInFunctionFamilies()
{
    if (m_errorInFunctionFamilies)
        m_build.diagnostics().add(*m_errorInFunctionFamilies);
    return !m_build.diagnostics().hasErrors();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
