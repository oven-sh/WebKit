/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTIdentifierNumbering.h"
#include "AOTImagesBelow.h"
#include "AOTOrderedInterning.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DeclaredFamilies {
    Vector<Vector<LayoutID>> members;
    BitVector hasMembersWithoutLayout;
};

struct WireTypeConversion {
    uint32_t source;
    uint32_t target;
};

struct DeclaredNames {
    bool lacksRequiredName(LayoutID member, uint32_t type) const;

    Vector<Vector<uint32_t>> required;
    Vector<Vector<uint32_t>> declared;
    BitVector declaresEveryName;
    uint32_t firstTaggedLayout { 0 };
    Vector<Vector<uint32_t>> declaredByTaggedLayouts;
};

struct Conversions {
    const Wire::Sections& checkedSections;
    Vector<Vector<uint32_t>> typesMetByNames;
    TellingFields tellingFields;
    DeclaredNames declaredNames;
    std::span<const TaggedFamily> taggedFamilies;
};

} // anonymous namespace

static InternalError inLayout(uint32_t index)
{
    return { .kind = InternalErrorKind::Unknown, .layout = LayoutID(index), .count = index };
}

static Failure checkDeclaredSupertypes(const Reader& reader, uint32_t index)
{
    std::span<const Wire::Layout> layouts = reader.sections().layouts;
    const Wire::Layout& layout = layouts[index];
    RecordChecker checker(reader, inLayout(index));

    checker.enumerator<LayoutKind>(layout.kind);
    checker.require(layout.base == Wire::none || layout.base < layouts.size(), InternalErrorKind::IdOutOfRange);
    for (uint32_t supertype : checker.words(layout.supertypes))
        checker.require(supertype < layouts.size(), InternalErrorKind::IdOutOfRange);
    return checker.takeFailure();
}

static bool isEnvironment(const Wire::Layout& checkedLayout)
{
    return isEnvironmentKind(static_cast<LayoutKind>(checkedLayout.kind));
}

static std::span<const Wire::Field> fieldsOf(const Wire::Sections& sections, const Wire::Layout& layout)
{
    std::span<const Wire::Field> fields = sections.fields;
    if (layout.fields.start > fields.size() || layout.fields.count > fields.size() - layout.fields.start)
        return { };
    return fields.subspan(layout.fields.start, layout.fields.count);
}

static bool isNamedByProgram(const Wire::Field& field)
{
    return field.key != Wire::none && !has(field, FieldFlags::IsPrivate) && !has(field, FieldFlags::IsBrand) && !has(field, FieldFlags::IsHidden);
}

static bool isStructuralType(const Wire::Layout& checkedLayout)
{
    return static_cast<LayoutKind>(checkedLayout.kind) == LayoutKind::Record && checkedLayout.constructor == Wire::none && !has(checkedLayout, LayoutFlags::IsTypeParameter);
}

static bool hasFieldsOfObjects(const Wire::Layout& checkedLayout)
{
    switch (static_cast<LayoutKind>(checkedLayout.kind)) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        return !has(checkedLayout, LayoutFlags::IsTypeParameter);
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    return false;
}

static Vector<uint32_t> collectNames(const Wire::Sections& checkedSections, uint32_t index, NOESCAPE const Invocable<bool(uint32_t)> auto& isWanted)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    Vector<uint32_t> names;
    size_t remainingBases = layouts.size();
    for (uint32_t current = index; current != Wire::none && remainingBases--; current = layouts[current].base) {
        for (const Wire::Field& field : fieldsOf(checkedSections, layouts[current])) {
            if (isNamedByProgram(field) && isWanted(field.key))
                names.append(field.key);
        }
    }
    AOT::sort(names);
    removeRepeatedElements(names);
    return names;
}

static Vector<uint32_t> collectNames(const Wire::Sections& checkedSections, uint32_t index)
{
    return collectNames(checkedSections, index, [](uint32_t) {
        return true;
    });
}

bool DeclaredNames::lacksRequiredName(LayoutID member, uint32_t type) const
{
    const Vector<uint32_t>& names = required[type];
    if (names.isEmpty())
        return false;
    uint32_t index = member.index();
    if (index < declared.size())
        return declaresEveryName.quickGet(index) && !std::ranges::includes(declared[index], names);
    if (index < firstTaggedLayout || index - firstTaggedLayout >= declaredByTaggedLayouts.size())
        return false;
    return !std::ranges::includes(declaredByTaggedLayouts[index - firstTaggedLayout], names);
}

static bool declaresEveryName(const Wire::Layout& checkedLayout)
{
    switch (static_cast<LayoutKind>(checkedLayout.kind)) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return !has(checkedLayout, LayoutFlags::IsTypeParameter);
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

static bool isRequiredField(const Wire::Field& field)
{
    return isNamedByProgram(field) && has(field, FieldFlags::IsRequired) && !has(field, FieldFlags::IsMethod) && !has(field, FieldFlags::IsNeverWritten);
}

static DeclaredNames collectDeclaredNames(const ParallelLoops& loops, const Wire::Sections& checkedSections)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    unsigned numberOfLayouts = static_cast<unsigned>(layouts.size());

    DeclaredNames result;
    result.required.grow(numberOfLayouts);
    result.declared.grow(numberOfLayouts);
    result.declaresEveryName = BitVector(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (declaresEveryName(layouts[index]))
            result.declaresEveryName.quickSet(index);
    }
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        if (result.declaresEveryName.quickGet(index))
            result.declared[index] = collectNames(checkedSections, index);
        if (!isStructuralType(layouts[index]))
            return;
        for (const Wire::Field& field : fieldsOf(checkedSections, layouts[index])) {
            if (isRequiredField(field))
                result.required[index].append(field.key);
        }
        AOT::sort(result.required[index]);
    });
    return result;
}

static void addNamesOfTaggedLayouts(DeclaredNames& names, const Wire::Sections& checkedSections, std::span<const TaggedFamily> families)
{
    if (families.empty())
        return;
    names.firstTaggedLayout = families[0].members[0].layout.index();
    for (const TaggedFamily& family : families) {
        Vector<uint32_t> namesOfMaker = collectNames(checkedSections, family.maker.index());
        for (const TaggedLayout& member : family.members) {
            Vector<uint32_t> declared = namesOfMaker;
            for (LayoutID type : member.declaringTypes) {
                for (const Wire::Field& field : fieldsOf(checkedSections, checkedSections.layouts[type.index()])) {
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

static void forEachGroupInParallel(const ParallelLoops& loops, std::span<const uint64_t> sortedPairs, NOESCAPE const Invocable<void(uint32_t first, std::span<const uint64_t> group)> auto& functor)
{
    constexpr size_t pairsPerTask = 4096;
    size_t count = sortedPairs.size();
    auto firstOf = [&](size_t index) {
        return static_cast<uint32_t>(sortedPairs[index] >> 32);
    };
    forEachIndexInParallel(loops, static_cast<unsigned>((count + pairsPerTask - 1) / pairsPerTask), [&](unsigned task) {
        size_t index = static_cast<size_t>(task) * pairsPerTask;
        size_t end = std::min(count, index + pairsPerTask);
        while (index < end && index && firstOf(index) == firstOf(index - 1))
            ++index;
        while (index < end) {
            size_t groupEnd = index + 1;
            while (groupEnd < count && firstOf(groupEnd) == firstOf(index))
                ++groupEnd;
            functor(firstOf(index), sortedPairs.subspan(index, groupEnd - index));
            index = groupEnd;
        }
    });
}

static constexpr uint64_t pairOf(uint32_t first, uint32_t second) { return static_cast<uint64_t>(first) << 32 | second; }

template<typename Listed>
static void listSecondsByFirst(std::span<const Vector<uint64_t>> pairsInOrderOfSeconds, Vector<Vector<Listed>>& lists)
{
    Vector<uint32_t> sizes(FillWith { }, lists.size(), 0u);
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

AOT_THRESHOLD(size_t, maxLayoutsMetByNames, 128);

static Vector<Vector<uint32_t>> findTypesMetByNames(const ParallelLoops& loops, const Wire::Sections& checkedSections, const TellingFields& tellingFields)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    unsigned numberOfLayouts = static_cast<unsigned>(layouts.size());

    Vector<Vector<uint32_t>> layoutsWithName(checkedSections.propertyKeys.size());
    auto forEachRequiredField = [&](uint32_t type, NOESCAPE const Invocable<void(const Wire::Field&)> auto& functor) {
        for (const Wire::Field& field : fieldsOf(checkedSections, layouts[type])) {
            if (isNamedByProgram(field) && has(field, FieldFlags::IsRequired) && !has(field, FieldFlags::IsNeverWritten) && field.key < layoutsWithName.size())
                functor(field);
        }
    };
    BitVector isRequiredBySomeType(layoutsWithName.size());
    for (uint32_t type = 0; type < numberOfLayouts; ++type) {
        if (!isStructuralType(layouts[type]))
            continue;
        forEachRequiredField(type, [&](const Wire::Field& field) {
            isRequiredBySomeType.quickSet(field.key);
        });
    }

    Vector<Vector<uint32_t>> names(numberOfLayouts);
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        if (!hasFieldsOfObjects(layouts[index]) || has(layouts[index], LayoutFlags::IsNeverAllocated) || has(layouts[index], LayoutFlags::IsWrittenUnderDictionaryType))
            return;
        names[index] = collectNames(checkedSections, index, [&](uint32_t name) {
            return name < isRequiredBySomeType.size() && isRequiredBySomeType.quickGet(name);
        });
    });
    Vector<uint64_t> layoutsByName = collectInParallel<uint64_t>(loops, numberOfLayouts, [&](unsigned index, Vector<uint64_t>& found) {
        for (uint32_t name : names[index])
            found.append(pairOf(name, index));
    });
    listSecondsByFirst(singleElementSpan(layoutsByName), layoutsWithName);

    Vector<WireTypeConversion> met = collectInParallel<WireTypeConversion>(loops, numberOfLayouts, [&](unsigned type, Vector<WireTypeConversion>& found) {
        if (!isStructuralType(layouts[type]))
            return;
        bool isDeclaredNowhere = layouts[type].name == Wire::none && layouts[type].module == Wire::none;
        Vector<uint32_t, 16> required;
        Vector<uint32_t, 16> requiredFields;
        forEachRequiredField(type, [&](const Wire::Field& field) {
            required.append(field.key);
            if (!isDeclaredNowhere || isRequiredField(field))
                requiredFields.append(field.key);
        });
        const Vector<uint32_t>* fewest = nullptr;
        for (uint32_t name : requiredFields) {
            if (!fewest || layoutsWithName[name].size() < fewest->size())
                fewest = &layoutsWithName[name];
        }
        if (!fewest)
            return;
        AOT::sort(required);
        AOT::sort(requiredFields);
        size_t sizeBefore = found.size();
        for (uint32_t layout : *fewest) {
            if (layout != type && std::ranges::includes(names[layout], declaresEveryName(layouts[layout]) ? required : requiredFields) && !tellingFields.tellApart(layout, type))
                found.append({ layout, type });
        }
        if (found.size() - sizeBefore > maxLayoutsMetByNames)
            found.shrink(sizeBefore);
    });

    Vector<Vector<uint32_t>> typesMetByNames(numberOfLayouts);
    Vector<uint64_t> typesBySource(met.size(), [&](size_t index) {
        return pairOf(met[index].source, met[index].target);
    });
    parallelSort(loops, typesBySource.mutableSpan(), std::less<uint64_t> { });
    forEachGroupInParallel(loops, typesBySource.span(), [&](uint32_t source, std::span<const uint64_t> group) {
        typesMetByNames[source] = Vector<uint32_t>(group.size(), [&](size_t index) {
            return static_cast<uint32_t>(group[index]);
        });
    });
    return typesMetByNames;
}

static void forEachConversionTarget(const Conversions& conversions, uint32_t source, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    const Wire::Sections& checkedSections = conversions.checkedSections;
    const Wire::Layout& layout = checkedSections.layouts[source];
    if (layout.base != Wire::none)
        functor(layout.base);
    for (uint32_t supertype : checkedSections.words.subspan(layout.supertypes.start, layout.supertypes.count))
        functor(supertype);
}

static bool requiresNoName(const Conversions& conversions, uint32_t type)
{
    return isStructuralType(conversions.checkedSections.layouts[type]) && conversions.declaredNames.required[type].isEmpty();
}

static bool passesOn(const Conversions& conversions, WireTypeConversion conversion, LayoutID member)
{
    if (!requiresNoName(conversions, conversion.source) || requiresNoName(conversions, conversion.target))
        return true;
    return member.index() == conversion.source && !has(conversions.checkedSections.layouts[conversion.source], LayoutFlags::IsNeverAllocated);
}

static DeclaredFamilies collectDeclaredMembers(const ParallelLoops& loops, const Conversions& conversions)
{
    constexpr uint32_t layoutsPerTask = 64;
    std::span<const Wire::Layout> layouts = conversions.checkedSections.layouts;
    uint32_t numberOfLayouts = static_cast<uint32_t>(layouts.size());
    DeclaredFamilies families { Vector<Vector<LayoutID>>(layouts.size()), BitVector(layouts.size()) };
    Vector<Vector<uint32_t>> stampsOfWorkers(loops.width());
    Vector<Vector<uint64_t>> foundByTasks((numberOfLayouts + layoutsPerTask - 1) / layoutsPerTask);
    forEachIndexInParallel(loops, static_cast<unsigned>(foundByTasks.size()), [&](unsigned task, unsigned worker) {
        Vector<uint32_t>& stamps = stampsOfWorkers[worker];
        if (stamps.isEmpty())
            stamps.fill(0u, layouts.size());
        Vector<uint64_t>& found = foundByTasks[task];
        Vector<uint32_t, 16> worklist;
        for (uint32_t index = task * layoutsPerTask, end = std::min(numberOfLayouts, index + layoutsPerTask); index < end; ++index) {
            if (isEnvironment(layouts[index])) {
                found.append(pairOf(index, index));
                continue;
            }

            LayoutID member(index);
            uint32_t stamp = index + 1;
            auto visit = [&](uint32_t layout) {
                if (std::exchange(stamps[layout], stamp) != stamp && !isEnvironment(layouts[layout]) && !conversions.tellingFields.tellApart(index, layout))
                    worklist.append(layout);
            };
            visit(index);
            for (uint32_t type : conversions.typesMetByNames[index])
                visit(type);
            while (!worklist.isEmpty()) {
                uint32_t reached = worklist.takeLast();
                found.append(pairOf(reached, index));
                forEachConversionTarget(conversions, reached, [&](uint32_t target) {
                    if (passesOn(conversions, { reached, target }, member))
                        visit(target);
                });
            }
        }
    });
    listSecondsByFirst(foundByTasks.span(), families.members);
    for (const Vector<uint64_t>& found : foundByTasks) {
        for (uint64_t pair : found) {
            uint32_t member = static_cast<uint32_t>(pair);
            if (!isEnvironment(layouts[member]) && has(layouts[member], LayoutFlags::HasMembersWithoutLayout))
                families.hasMembersWithoutLayout.quickSet(pair >> 32);
        }
    }
    return families;
}

static void removeMembersThatLackRequiredNames(const ParallelLoops& loops, const DeclaredNames& names, DeclaredFamilies& families)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(families.members.size()), [&](unsigned type) {
        if (names.required[type].isEmpty())
            return;
        families.members[type].removeAllMatching([&](LayoutID member) {
            return names.lacksRequiredName(member, type);
        });
    });
}

static bool canPassFor(const Conversions& conversions, LayoutID member, uint32_t type)
{
    if (member.index() >= conversions.checkedSections.layouts.size() || conversions.tellingFields.tellApart(member.index(), type) || conversions.declaredNames.lacksRequiredName(member, type))
        return false;
    for (const TaggedFamily& family : conversions.taggedFamilies) {
        if (family.maker != member || member.index() == type)
            continue;
        size_t numberOfTags = 0;
        for (const TaggedLayout& tagged : family.members)
            numberOfTags += tagged.tags.size();
        return !conversions.tellingFields.declaresOnlyLiterals(type, family.tagKey.index(), numberOfTags, [&](const LayoutTag& literal) {
            return family.members.containsIf([&](const TaggedLayout& tagged) {
                return tagged.tags.contains(literal);
            });
        });
    }
    return true;
}

static bool isInsideFamily(const Conversions& conversions, const DeclaredFamilies& families, WireTypeConversion conversion)
{
    bool passesOnEveryMember = !requiresNoName(conversions, conversion.source) || requiresNoName(conversions, conversion.target);
    if (passesOnEveryMember && families.hasMembersWithoutLayout.quickGet(conversion.source) && !families.hasMembersWithoutLayout.quickGet(conversion.target))
        return false;
    const Vector<LayoutID>& listed = families.members[conversion.target];
    if (std::ranges::includes(listed, families.members[conversion.source]))
        return true;
    return std::ranges::all_of(families.members[conversion.source], [&](LayoutID member) {
        return std::ranges::binary_search(listed, member) || !passesOn(conversions, conversion, member) || !canPassFor(conversions, member, conversion.target);
    });
}

static Vector<WireTypeConversion> findConversionsOutsideFamilies(const ParallelLoops& loops, const Conversions& conversions, const DeclaredFamilies& families)
{
    std::span<const Wire::Layout> layouts = conversions.checkedSections.layouts;
    return collectInParallel<WireTypeConversion>(loops, static_cast<unsigned>(layouts.size()), [&](unsigned source, Vector<WireTypeConversion>& found) {
        if (isEnvironment(layouts[source]))
            return;
        ASSERT(std::ranges::is_sorted(families.members[source]));
        forEachConversionTarget(conversions, source, [&](uint32_t target) {
            if (!isEnvironment(layouts[target]) && !isInsideFamily(conversions, families, { source, target }))
                found.append({ source, target });
        });
    });
}

static LayoutID findMemberOutsideFamily(const Conversions& conversions, const DeclaredFamilies& families, WireTypeConversion conversion)
{
    const Vector<LayoutID>& listed = families.members[conversion.target];
    for (LayoutID member : families.members[conversion.source]) {
        if (!std::ranges::binary_search(listed, member) && passesOn(conversions, conversion, member) && canPassFor(conversions, member, conversion.target))
            return member;
    }
    return LayoutID(conversion.source);
}

static void widenFamilies(const Conversions& conversions, DeclaredFamilies& families, Vector<WireTypeConversion>&& worklist)
{
    std::span<const Wire::Layout> layouts = conversions.checkedSections.layouts;
    while (!worklist.isEmpty()) {
        WireTypeConversion conversion = worklist.takeLast();
        if (isInsideFamily(conversions, families, conversion))
            continue;

        Vector<LayoutID>& members = families.members[conversion.target];
        for (LayoutID member : families.members[conversion.source]) {
            if (passesOn(conversions, conversion, member) && canPassFor(conversions, member, conversion.target))
                members.append(member);
        }
        AOT::sort(members);
        removeRepeatedElements(members);
        bool passesOnEveryMember = !requiresNoName(conversions, conversion.source) || requiresNoName(conversions, conversion.target);
        if (passesOnEveryMember && families.hasMembersWithoutLayout.quickGet(conversion.source))
            families.hasMembersWithoutLayout.quickSet(conversion.target);

        forEachConversionTarget(conversions, conversion.target, [&](uint32_t target) {
            if (!isEnvironment(layouts[target]))
                worklist.append({ conversion.target, target });
        });
    }
}

static BitVector findTypeParametersWithoutFamily(std::span<const Wire::Layout> layouts, const DeclaredFamilies& families)
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

// Another image reads the slot as its own code does where nothing is proven: it asks whether the property is there. What stands for a text or a function of that image alone is left to it.
static bool hasSlotsThatOtherImagesRead(const ImagesBelow& below, const ImagesBelow::Record& record)
{
    const LayoutData& layout = *record.layout;
    if (layout.isDerived || layout.hasExtensionData || layout.hasFieldsWithSymbolKeys || !layout.numberOfFields)
        return false;
    constexpr uint16_t flagsOfOtherFields = fieldIsOutOfLine | fieldMayExistWithoutStorage | fieldIsPrivate | fieldIsBrand | fieldAttributeFlags;
    return std::ranges::all_of(layout.fields(), [&](const FieldData& field) {
        SlotTraits traits = traitsOf(field.slot.representation());
        bool isReadByAnyImage = traits.hasAbsent && traits.valueKind != SlotValueKind::LiteralString && traits.valueKind != SlotValueKind::DeferredClosure;
        return isReadByAnyImage && !(field.flags & flagsOfOtherFields) && !below.textOfName(field.name).isNull();
    });
}

// An object that the code of an image below has made comes to a type of this program as one that this program makes: it meets the type by its names.
void Reader::planLayoutsBelow(Vector<Vector<LayoutID>>& members)
{
    const ImagesBelow* below = m_build.imagesBelow();
    if (!below || m_form != IRForm::AsRead)
        return;

    struct TypeWithNames {
        uint32_t type;
        Vector<uint32_t, 8> numbers;
        // Nothing in this program writes them. One that no object below has either is a name that no object ever has, as the mark of a branded type: it is asked of nobody.
        Vector<uint32_t, 8> numbersNeverWrittenHere;
        // Every name that the type declares, required or not, ascending.
        Vector<uint32_t, 8> declaredNumbers;
    };
    std::span<const Wire::Layout> layouts = m_sections.layouts;
    Vector<std::optional<std::optional<uint32_t>>> numberOfKey(m_sections.propertyKeys.size());
    auto numberBelow = [&](uint32_t key) {
        if (!numberOfKey[key])
            numberOfKey[key] = numberInImagesBelow(m_program, PropertyKeyID(key), m_program.at(PropertyKeyID(key)).text, *below);
        return *numberOfKey[key];
    };
    Vector<TypeWithNames> types;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> recordsWithName;
    for (uint32_t type = 0; type < layouts.size(); ++type) {
        // The code below knows nothing of a type that a file of this program declares: it makes no object for it.
        if (!isStructuralType(layouts[type]) || layouts[type].module != Wire::none)
            continue;
        TypeWithNames named { type, { }, { }, { } };
        bool isMetBelow = true;
        for (const Wire::Field& field : fieldsOf(m_sections, layouts[type])) {
            if (!isNamedByProgram(field) || field.key >= numberOfKey.size())
                continue;
            std::optional<uint32_t> number = m_program.at(PropertyKeyID(field.key)).kind == PropertyKeyKind::Text ? numberBelow(field.key) : std::nullopt;
            if (number)
                named.declaredNumbers.append(*number);
            if (!has(field, FieldFlags::IsRequired))
                continue;
            if (number)
                (has(field, FieldFlags::IsNeverWritten) ? named.numbersNeverWrittenHere : named.numbers).append(*number);
            else if (!has(field, FieldFlags::IsNeverWritten)) {
                isMetBelow = false;
                break;
            }
        }
        if (!isMetBelow || (named.numbers.isEmpty() && named.numbersNeverWrittenHere.isEmpty()))
            continue;
        for (uint32_t number : named.numbers)
            recordsWithName.add(number, Vector<uint32_t>());
        for (uint32_t number : named.numbersNeverWrittenHere)
            recordsWithName.add(number, Vector<uint32_t>());
        AOT::sort(named.declaredNumbers);
        types.append(WTF::move(named));
    }
    if (types.isEmpty())
        return;

    Vector<ImagesBelow::Record> records;
    size_t numberLeftOut = 0;
    below->forEachRecord([&](const ImagesBelow::Record& record) {
        bool isAskedFor = false;
        below->forEachNameOf(record, [&](uint32_t name) {
            isAskedFor |= recordsWithName.contains(name);
        });
        if (!isAskedFor)
            return;
        if (!hasSlotsThatOtherImagesRead(*below, record)) {
            ++numberLeftOut;
            return;
        }
        uint32_t index = static_cast<uint32_t>(records.size());
        below->forEachNameOf(record, [&](uint32_t name) {
            auto found = recordsWithName.find(name);
            if (found != recordsWithName.end() && (found->value.isEmpty() || found->value.last() != index))
                found->value.append(index);
        });
        records.append(record);
    });

    uint32_t firstLayout = static_cast<uint32_t>(numberOfRecords<LayoutID>());
    Vector<LayoutID> layoutOfRecord(records.size());
    for (TypeWithNames& named : types) {
        for (uint32_t number : named.numbersNeverWrittenHere) {
            if (!recordsWithName.find(number)->value.isEmpty())
                named.numbers.append(number);
        }
        if (named.numbers.isEmpty())
            continue;
        const Vector<uint32_t>* fewest = nullptr;
        for (uint32_t number : named.numbers) {
            const Vector<uint32_t>& withName = recordsWithName.find(number)->value;
            if (!fewest || withName.size() < fewest->size())
                fewest = &withName;
        }
        Vector<uint32_t, 8> met;
        for (uint32_t record : *fewest) {
            bool hasEveryName = std::ranges::all_of(named.numbers, [&](uint32_t number) {
                return below->findsName(records[record], number);
            });
            // As a literal is held to the type that it is written for. By its names alone a type of one name is met by every object that has the name, whichever type it was made for:
            // flow, which in one program says which of them arrive, sees none of the code that makes these.
            bool hasNoOtherField = std::ranges::all_of(records[record].layout->fields(), [&](const FieldData& field) {
                return std::ranges::binary_search(named.declaredNumbers, static_cast<uint32_t>(field.name));
            });
            if (hasEveryName && hasNoOtherField)
                met.append(record);
        }
        if (met.size() > maxLayoutsMetByNames)
            continue;
        for (uint32_t record : met) {
            if (!layoutOfRecord[record]) {
                layoutOfRecord[record] = LayoutID(firstLayout + static_cast<uint32_t>(m_program.recordsOfImagesBelow.size()));
                m_program.recordsOfImagesBelow.append({ layoutOfRecord[record], records[record].image, records[record].firstShape, records[record].numberOfShapes, records[record].layout->isClass(), { } });
            }
            members[named.type].append(layoutOfRecord[record]);
        }
    }
    // What such an object holds in a field is an object of that image again. Where the image holds the slot to a set of its layouts, they are taken in as well: the read has a type, and the next read a slot.
    UncheckedKeyHashMap<uint32_t, LayoutID> layoutByFirstShape;
    for (const Program::RecordOfImageBelow& taken : m_program.recordsOfImagesBelow)
        layoutByFirstShape.add(taken.firstShape, taken.layout);
    // Many fields are held to one set.
    UncheckedKeyHashMap<uint64_t, std::optional<Vector<ImagesBelow::Record>>> recordsByTest;
    auto recordsPassing = [&](const ImagesBelow::Record& record, const RuntimeCellTest& test) -> const std::optional<Vector<ImagesBelow::Record>>& {
        return recordsByTest.ensure(static_cast<uint64_t>(record.image + 1) << 32 | test.shapeTestOffset(), [&] {
            std::optional<Vector<ImagesBelow::Record>> passing { Vector<ImagesBelow::Record>() };
            below->forEachShapeIn(record, test, [&](uint32_t shape) {
                if (!passing || (!passing->isEmpty() && shape < passing->last().firstShape + passing->last().numberOfShapes))
                    return;
                std::optional<ImagesBelow::Record> member = below->recordWithShape(shape);
                if (!member || member->firstShape != shape || passing->size() == maxLayoutsMetByNames || !hasSlotsThatOtherImagesRead(*below, *member))
                    passing = std::nullopt;
                else
                    passing->append(*member);
            });
            return passing;
        }).iterator->value;
    };
    for (size_t index = 0; index < m_program.recordsOfImagesBelow.size(); ++index) {
        ImagesBelow::Record record = below->recordAt(m_program.recordsOfImagesBelow[index].image, m_program.recordsOfImagesBelow[index].firstShape, m_program.recordsOfImagesBelow[index].numberOfShapes);
        Vector<std::optional<Vector<LayoutID>>> objectsInFields;
        for (const FieldData& field : record.layout->fields()) {
            Vector<ImagesBelow::Record, 8> held;
            bool isListed = true;
            bool holdsObjects = false;
            for (const RuntimeCellTest& test : below->typeOf(record, field).cellTests()) {
                if (test.kind == CellTestKind::IsSymbol)
                    continue;
                holdsObjects = true;
                const std::optional<Vector<ImagesBelow::Record>>* members = test.kind == CellTestKind::ShapeIn ? &recordsPassing(record, test) : nullptr;
                if (!members || !*members) {
                    isListed = false;
                    break;
                }
                held.appendVector(**members);
            }
            if (!isListed || !holdsObjects || held.size() > maxLayoutsMetByNames) {
                objectsInFields.append(std::nullopt);
                continue;
            }
            objectsInFields.append(Vector<LayoutID>(held.size(), [&](size_t position) {
                const ImagesBelow::Record& member = held[position];
                return layoutByFirstShape.ensure(member.firstShape, [&] {
                    LayoutID id(firstLayout + static_cast<uint32_t>(m_program.recordsOfImagesBelow.size()));
                    m_program.recordsOfImagesBelow.append({ id, member.image, member.firstShape, member.numberOfShapes, member.layout->isClass(), { } });
                    return id;
                }).iterator->value;
            }));
        }
        m_program.recordsOfImagesBelow[index].objectsInFields = WTF::move(objectsInFields);
    }
    m_numberOfDerivedLayouts += m_program.recordsOfImagesBelow.size();
    if (Options::logAOTPhaseTimes())
        dataLogLn("AOT records of images below | types that ask ", types.size(), " | records with one of their names ", records.size(), " | and with slots that only their image reads ", numberLeftOut, " | taken in ", m_program.recordsOfImagesBelow.size(), " | of them met by a type ", std::ranges::count_if(layoutOfRecord, [](LayoutID id) { return !!id; }));
}

bool Reader::computeDeclaredMembers()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_sections.layouts.size());
    auto numberOfNamespaceLayouts = std::ranges::count_if(m_sections.modules, [](const Wire::Module& module) {
        return module.kind == static_cast<uint32_t>(ModuleKind::SourceText);
    });
    m_numberOfDerivedLayouts = m_builtins.layoutCount() + static_cast<uint32_t>(numberOfNamespaceLayouts);

    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (auto failure = checkDeclaredSupertypes(*this, index))
            return report(*failure);
    }

    if (m_keepsLayoutsAsWritten) {
        m_typeParametersWithoutFamily = BitVector(numberOfLayouts);
        m_viewsWithoutLayouts = BitVector(numberOfLayouts);
        for (size_t index = 0; index < numberOfRecords<LayoutID>(); ++index)
            m_declaredMembers.append(m_program.types.intern(LayoutSet::complete({ LayoutID(index) })));
        return true;
    }

    TellingFields tellingFields(m_build.parallelLoops(), m_sections);
    Conversions conversions { m_sections, findTypesMetByNames(m_build.parallelLoops(), m_sections, tellingFields), WTF::move(tellingFields), collectDeclaredNames(m_build.parallelLoops(), m_sections), { } };
    DeclaredFamilies families = collectDeclaredMembers(m_build.parallelLoops(), conversions);
    planTaggedLayouts(families.members, conversions.tellingFields);
    conversions.taggedFamilies = m_program.taggedFamilies.span();
    addNamesOfTaggedLayouts(conversions.declaredNames, m_sections, conversions.taggedFamilies);
    removeMembersThatLackRequiredNames(m_build.parallelLoops(), conversions.declaredNames, families);

    Vector<WireTypeConversion> outside = findConversionsOutsideFamilies(m_build.parallelLoops(), conversions, families);
    if (!outside.isEmpty()) {
        m_build.statistics().count(StatisticID::ConversionsOutsideFamily, outside.size());
        if (m_build.options().verifiesAfterEveryStage) {
            WireTypeConversion first = outside.first();
            return report({ .kind = InternalErrorKind::FamilyNotClosed, .layout = findMemberOutsideFamily(conversions, families, first), .count = first.target, .detail = makeString("the type is L"_s, first.target) });
        }
        widenFamilies(conversions, families, WTF::move(outside));
    }
    m_typeParametersWithoutFamily = findTypeParametersWithoutFamily(m_sections.layouts, families);
    planLayoutsBelow(families.members);

    size_t numberOfLayoutIDs = numberOfRecords<LayoutID>();
    m_viewsWithoutLayouts = BitVector(numberOfLayouts);
    Vector<LayoutSet> sets(numberOfLayoutIDs);
    Vector<uint8_t> isViewWithoutLayouts(FillWith { }, numberOfLayouts, static_cast<uint8_t>(0));
    forEachIndexInParallel(m_build.parallelLoops(), static_cast<unsigned>(numberOfLayoutIDs), [&](unsigned index) {
        if (index >= numberOfLayouts) {
            sets[index] = LayoutSet::complete({ LayoutID(index) });
            return;
        }
        Vector<LayoutID>& members = families.members[index];
        members.removeAllMatching([&](LayoutID member) {
            return member.index() < numberOfLayouts && has(m_sections.layouts[member.index()], LayoutFlags::IsNeverAllocated);
        });
        if (members.isEmpty()) {
            isViewWithoutLayouts[index] = 1;
            members.append(LayoutID(index));
        }
        sets[index] = LayoutSet::complete(WTF::move(members));
        canonicalize(sets[index]);
    });
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!isViewWithoutLayouts[index])
            continue;
        if (has(m_sections.layouts[index], LayoutFlags::IsTypeParameter))
            m_typeParametersWithoutFamily.quickSet(index);
        m_viewsWithoutLayouts.quickSet(index);
    }
    m_declaredMembers = internAllInOrder<LayoutSet, LayoutSetID>(m_program.types, m_build.parallelLoops(), sets.mutableSpan());
    m_build.destroyLater(WTF::move(sets));
    return true;
}

static SortedSet<BuiltinID> readBuiltins(RecordChecker&, Wire::Span);

static bool isSignature(const Wire::Function& function)
{
    return function.role == static_cast<uint32_t>(FunctionRole::Signature);
}

static Failure checkFunctionTypes(const Reader& reader, uint32_t index)
{
    std::span<const Wire::Function> functions = reader.sections().functions;
    RecordChecker checker(reader, { .kind = InternalErrorKind::Unknown, .function = FunctionID(index), .count = index });
    for (uint32_t functionType : checker.words(functions[index].functionTypes))
        checker.require(functionType < functions.size() && isSignature(functions[functionType]), InternalErrorKind::IdOutOfRange);
    checker.require(!functions[index].builtinMembers.count || isSignature(functions[index]), InternalErrorKind::IdOutOfRange);
    return checker.takeFailure();
}

static std::span<const uint32_t> functionTypesOf(const Wire::Sections& checkedSections, uint32_t index)
{
    Wire::Span functionTypes = checkedSections.functions[index].functionTypes;
    return checkedSections.words.subspan(functionTypes.start, functionTypes.count);
}

static void forEachFunctionTypeAbove(const Wire::Sections& checkedSections, uint32_t start, Vector<uint32_t>& stamps, const BitVector& leftOut, NOESCAPE const Invocable<void(uint32_t)> auto& visit)
{
    uint32_t stamp = start + 1;
    Vector<uint32_t, 16> worklist { start };
    while (!worklist.isEmpty()) {
        uint32_t reachedIndex = worklist.takeLast();
        visit(reachedIndex);
        for (uint32_t functionType : functionTypesOf(checkedSections, reachedIndex)) {
            if (std::exchange(stamps[functionType], stamp) != stamp && !leftOut.quickGet(functionType))
                worklist.append(functionType);
        }
    }
}

// Where every function comes to a function type by one way, the members of the types right below it add up.
static BitVector findFunctionTypesWithMoreMembersThan(const Wire::Sections& checkedSections, uint64_t maxMembers)
{
    std::span<const Wire::Function> functions = checkedSections.functions;
    uint32_t numberOfFunctions = static_cast<uint32_t>(functions.size());
    Vector<uint32_t> numberLeftBelow(FillWith { }, numberOfFunctions, 0u);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        for (uint32_t functionType : functionTypesOf(checkedSections, index))
            ++numberLeftBelow[functionType];
    }

    BitVector result(numberOfFunctions);
    BitVector isReachedBySeveralWays(numberOfFunctions);
    Vector<uint32_t> numberOfMembers(FillWith { }, numberOfFunctions, 0u);
    Vector<uint32_t> worklist;
    Vector<uint32_t> found;
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (!numberLeftBelow[index])
            worklist.append(index);
    }
    while (!worklist.isEmpty()) {
        uint32_t index = worklist.takeLast();
        if (!isSignature(functions[index]))
            ++numberOfMembers[index];
        bool isCounted = !isReachedBySeveralWays.quickGet(index);
        if (isCounted && numberOfMembers[index] > maxMembers) {
            result.quickSet(index);
            found.append(index);
        }
        std::span<const uint32_t> functionTypes = functionTypesOf(checkedSections, index);
        for (uint32_t functionType : functionTypes) {
            if (isCounted && functionTypes.size() == 1)
                numberOfMembers[functionType] += numberOfMembers[index];
            else
                isReachedBySeveralWays.quickSet(functionType);
            if (!--numberLeftBelow[functionType])
                worklist.append(functionType);
        }
    }
    while (!found.isEmpty()) {
        for (uint32_t functionType : functionTypesOf(checkedSections, found.takeLast())) {
            if (!result.quickSet(functionType))
                found.append(functionType);
        }
    }
    return result;
}

static BitVector findFunctionsThatTypesName(const Wire::Sections& sections)
{
    BitVector result(sections.functions.size());
    for (const Wire::Type& type : sections.types) {
        Wire::Span named = type.functions;
        if (named.start > sections.words.size() || named.count > sections.words.size() - named.start)
            continue;
        for (uint32_t function : sections.words.subspan(named.start, named.count)) {
            if (function < sections.functions.size())
                result.quickSet(function);
        }
    }
    return result;
}

struct FunctionFamilies {
    Vector<Vector<FunctionID>> membersOfNamed;
    Vector<uint32_t> numberOfMembers;
};

static FunctionFamilies collectFunctionFamilies(const ParallelLoops& loops, const Wire::Sections& checkedSections, const BitVector& leftOut)
{
    constexpr uint32_t functionsPerTask = 512;
    std::span<const Wire::Function> functions = checkedSections.functions;
    uint32_t numberOfFunctions = static_cast<uint32_t>(functions.size());
    BitVector isNamedByType = findFunctionsThatTypesName(checkedSections);
    Vector<Vector<FunctionID>> families(functions.size());
    Vector<Vector<uint32_t>> stampsOfWorkers(loops.width());
    Vector<Vector<uint32_t>> countsOfWorkers(loops.width());
    Vector<Vector<uint64_t>> foundByTasks((numberOfFunctions + functionsPerTask - 1) / functionsPerTask);
    forEachIndexInParallel(loops, static_cast<unsigned>(foundByTasks.size()), [&](unsigned task, unsigned worker) {
        Vector<uint32_t>& stamps = stampsOfWorkers[worker];
        Vector<uint32_t>& counts = countsOfWorkers[worker];
        Vector<uint64_t>& found = foundByTasks[task];
        for (uint32_t index = task * functionsPerTask, end = std::min(numberOfFunctions, index + functionsPerTask); index < end; ++index) {
            if (isSignature(functions[index]))
                continue;
            families[index] = { FunctionID(index) };
            if (!functions[index].functionTypes.count)
                continue;
            if (stamps.isEmpty()) {
                stamps.fill(0u, functions.size());
                counts.fill(0u, functions.size());
            }
            forEachFunctionTypeAbove(checkedSections, index, stamps, leftOut, [&](uint32_t reachedIndex) {
                if (reachedIndex == index)
                    return;
                ++counts[reachedIndex];
                if (isNamedByType.quickGet(reachedIndex))
                    found.append(pairOf(reachedIndex, index));
            });
        }
    });
    listSecondsByFirst(foundByTasks.span(), families);

    Vector<uint32_t> numberOfMembers(FillWith { }, functions.size(), 0u);
    for (const Vector<uint32_t>& counts : countsOfWorkers) {
        for (size_t index = 0; index < counts.size(); ++index)
            numberOfMembers[index] += counts[index];
    }
    return { WTF::move(families), WTF::move(numberOfMembers) };
}

static Vector<Vector<BuiltinID>> collectBuiltinMembers(const Wire::Sections& checkedSections, std::span<const SortedSet<BuiltinID>> convertedBuiltins, const BitVector& leftOut)
{
    Vector<Vector<BuiltinID>> members(convertedBuiltins.size());
    Vector<uint32_t> stamps(FillWith { }, convertedBuiltins.size(), 0u);
    for (uint32_t index = 0; index < convertedBuiltins.size(); ++index) {
        if (convertedBuiltins[index].isEmpty() || leftOut.quickGet(index))
            continue;
        forEachFunctionTypeAbove(checkedSections, index, stamps, leftOut, [&](uint32_t reachedIndex) {
            members[reachedIndex].append(convertedBuiltins[index].span());
        });
    }
    for (Vector<BuiltinID>& list : members) {
        AOT::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    }
    return members;
}

static StatisticID statisticForFamilyOf(size_t numberOfMembers, uint64_t maxDispatchCases)
{
    if (!numberOfMembers)
        return StatisticID::FunctionTypesWithoutMembers;
    if (numberOfMembers == 1)
        return StatisticID::FunctionTypesWithOneMember;
    return numberOfMembers <= maxDispatchCases ? StatisticID::FunctionTypesWithFewMembers : StatisticID::FunctionTypesWithManyMembers;
}

Failure Reader::computeFunctionFamilies()
{
    constexpr size_t functionsPerChunk = 1024;
    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t numberOfFunctions = static_cast<uint32_t>(m_sections.functions.size());
    auto firstOf = [](const Vector<Failure>& failures) -> Failure {
        for (const Failure& failure : failures) {
            if (failure)
                return failure;
        }
        return std::nullopt;
    };
    if (Failure failure = firstOf(readInChunks(loops, numberOfFunctions, functionsPerChunk, [&](uint32_t index) { return checkFunctionTypes(*this, index); })))
        return failure;

    Vector<SortedSet<BuiltinID>> convertedBuiltins(numberOfFunctions);
    Failure failureInBuiltins = firstOf(readInChunks(loops, numberOfFunctions, functionsPerChunk, [&](uint32_t index) -> Failure {
        RecordChecker checker(*this, { .kind = InternalErrorKind::Unknown, .function = FunctionID(index), .count = index });
        convertedBuiltins[index] = readBuiltins(checker, m_sections.functions[index].builtinMembers);
        for (BuiltinID member : convertedBuiltins[index])
            checker.require(isCallableBuiltin(m_builtins, member), InternalErrorKind::IdOutOfRange);
        return checker.takeFailure();
    }));
    if (failureInBuiltins)
        return failureInBuiltins;

    uint64_t maxDispatchCases = m_build.limit(LimitID::MaxDispatchCases);
    m_functionTypesThatNoTypeLists = findFunctionTypesWithMoreMembersThan(m_sections, std::max(m_build.limit(LimitID::MaxFunctionsInType), maxDispatchCases));
    FunctionFamilies families = collectFunctionFamilies(loops, m_sections, m_functionTypesThatNoTypeLists);
    Vector<Vector<BuiltinID>> builtinMembers = collectBuiltinMembers(m_sections, convertedBuiltins.span(), m_functionTypesThatNoTypeLists);
    Vector<CalleeSet> sets(numberOfFunctions);
    forEachIndexInParallel(loops, static_cast<unsigned>((numberOfFunctions + functionsPerChunk - 1) / functionsPerChunk), [&](unsigned chunk) {
        for (size_t index = chunk * functionsPerChunk, end = std::min<size_t>(numberOfFunctions, index + functionsPerChunk); index < end; ++index) {
            if (m_functionTypesThatNoTypeLists.quickGet(index))
                m_build.statistics().count(StatisticID::FunctionTypesWithManyMembers, 1);
            else if (isSignature(m_sections.functions[index]))
                m_build.statistics().count(statisticForFamilyOf(families.numberOfMembers[index] + builtinMembers[index].size(), maxDispatchCases), 1);
            sets[index] = CalleeSet::complete(WTF::move(families.membersOfNamed[index]), WTF::move(builtinMembers[index]));
            canonicalize(sets[index]);
        }
    });
    m_functionFamilies = internAllInOrder<CalleeSet, CalleeSetID>(m_program.types, loops, sets.mutableSpan());
    m_build.destroyLater(WTF::move(sets));
    return std::nullopt;
}

void Reader::makeRoomForFunctions()
{
    size_t numberOfFunctions = m_sections.functions.size();
    m_program.functions.reserveInitialCapacity(numberOfFunctions + numberOfFunctions * 3 / 2);
    growFromEmpty(m_build.parallelLoops(), m_program.functions, numberOfFunctions);
}

bool Reader::computeFamilies()
{
    bool hasDeclaredMembers = false;
    Failure failureInFunctionFamilies;
    runTasksInParallel(m_build.parallelLoops(), {
        [&] {
            hasDeclaredMembers = computeDeclaredMembers();
        },
        [&] {
            failureInFunctionFamilies = computeFunctionFamilies();
        },
        [&] {
            makeRoomForFunctions();
        },
    });
    if (!hasDeclaredMembers)
        return false;
    if (failureInFunctionFamilies)
        return report(*failureInFunctionFamilies);
    return true;
}

template<typename IDType>
static Vector<IDType, 8> collectAscendingIDs(RecordChecker& checker, std::span<const uint32_t> words)
{
    Vector<IDType, 8> ids;
    ids.reserveInitialCapacity(words.size());
    for (uint32_t word : words) {
        IDType id = checker.id<IDType>(word);
        if (!ids.isEmpty())
            checker.requireAscending(ids.last(), id);
        ids.append(id);
    }
    return ids;
}

static void readPrimitives(RecordChecker& checker, const Wire::Type& wire, OptionSet<TypeFlags> flags, Type& type)
{
    type = Type::fromPrimitives(fromWire(checker.flags<Primitives>(wire.primitives)));
    if (!flags.contains(TypeFlags::HasIntegers))
        return;
    type.primitives |= Primitive::Integer;
    type.integers = type.integers.unionWith(Range { wire.minimum, wire.maximum }.intersectWith(Range::full()));
}

static SortedSet<BuiltinID> readBuiltins(RecordChecker& checker, Wire::Span span)
{
    const BuiltinTable& builtins = checker.reader().builtins();
    Vector<BuiltinID, 4> canonicalIDs;
    BuiltinID previous;
    for (uint32_t word : checker.words(span)) {
        BuiltinID builtin = checker.external<BuiltinID>(word);
        if (checker.hasFailed())
            break;
        if (previous)
            checker.requireAscending(previous, builtin);
        previous = builtin;
        // A constant is a primitive value. A type says which kind of primitive.
        checker.require(builtins.at(builtin).kind != BuiltinKind::Constant, InternalErrorKind::IdOutOfRange);
        canonicalIDs.append(builtins.canonicalID(builtin));
    }
    return SortedSet<BuiltinID>(canonicalIDs.span());
}

bool Reader::readsAsAnything(std::span<const LayoutID> named) const
{
    return m_form == IRForm::AsRead && std::ranges::any_of(named, [&](LayoutID layout) {
        return m_typeParametersWithoutFamily.get(layout.index());
    });
}

void Reader::addLayouts(TypeAsRead& result, std::span<const LayoutID> named) const
{
    if (result.type.anyObject || named.empty())
        return;
    if (m_form != IRForm::AsRead) {
        result.layoutsToIntern = LayoutSet::complete(Vector<LayoutID>(named));
        return;
    }

    if (named.size() == 1) {
        result.type.layouts = m_declaredMembers[named[0].index()];
        return;
    }

    Vector<LayoutID> members;
    for (LayoutID layout : named) {
        const LayoutSet& family = m_program.at(m_declaredMembers[layout.index()]);
        members.append(family.completeMembers().value());
    }
    LayoutSet families = LayoutSet::complete(WTF::move(members));
    canonicalize(families);
    result.layoutsToIntern = WTF::move(families);
}

void Reader::addFunctions(Type& type, std::span<const FunctionID> named) const
{
    if (named.empty())
        return;
    if (m_form != IRForm::AsRead || m_keepsLayoutsAsWritten) {
        type.functions = SortedSet<FunctionID>(named);
        return;
    }

    Vector<FunctionID, 8> members;
    Vector<BuiltinID, 4> builtinMembers(type.builtins.span());
    for (FunctionID function : named) {
        type.anyFunction |= isSignature(m_sections.functions[function.index()]);
        Callees family = *m_program.at(m_functionFamilies[function.index()]).all();
        if (family.isEmpty() || m_functionTypesThatNoTypeLists.quickGet(function.index())) {
            type.anyFunction = true;
            return;
        }
        members.append(family.functions);
        builtinMembers.append(family.builtins);
    }
    if (members.size() > m_build.limit(LimitID::MaxFunctionsInType)) {
        type.anyFunction = true;
        return;
    }
    type.functions = SortedSet<FunctionID>(members.span());
    type.builtins = SortedSet<BuiltinID>(builtinMembers.span());
}

bool Reader::readTypes()
{
    if (m_completeType)
        return readTypesOneByOne();

    constexpr size_t typesPerChunk = 64;
    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t numberOfTypes = static_cast<uint32_t>(m_sections.types.size());
    unsigned numberOfChunks = static_cast<unsigned>((numberOfTypes + typesPerChunk - 1) / typesPerChunk);
    auto forEachTypeInParallel = [&](const auto& functor) {
        forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
            for (uint32_t index = chunk * typesPerChunk, end = std::min<uint32_t>(numberOfTypes, index + typesPerChunk); index < end; ++index)
                functor(index);
        });
    };

    Vector<Vector<TypeAsRead>> chunksAsRead(numberOfChunks);
    auto typeAsRead = [&](uint32_t index) -> TypeAsRead& {
        return chunksAsRead[index / typesPerChunk][index % typesPerChunk];
    };
    Vector<Failure> failures(numberOfChunks);
    forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
        uint32_t first = chunk * typesPerChunk;
        chunksAsRead[chunk].grow(std::min<size_t>(typesPerChunk, numberOfTypes - first));
        for (uint32_t offset = 0; offset < chunksAsRead[chunk].size() && !failures[chunk]; ++offset)
            failures[chunk] = readType(first + offset, chunksAsRead[chunk][offset]);
    });
    if (!reportFirst(failures.span()))
        return false;

    Vector<uint32_t> typesWithNewSets = findIndicesInParallel(loops, numberOfTypes, [&](unsigned index) {
        return typeAsRead(index).layoutsToIntern.has_value();
    });
    Vector<LayoutSet> sets(typesWithNewSets.size(), [&](size_t position) {
        return WTF::move(*typeAsRead(typesWithNewSets[position]).layoutsToIntern);
    });
    Vector<LayoutSetID> setIDs = internAllInOrder<LayoutSet, LayoutSetID>(m_program.types, loops, sets.mutableSpan());
    for (size_t position = 0; position < setIDs.size(); ++position)
        typeAsRead(typesWithNewSets[position]).type.layouts = setIDs[position];

    auto contentsOf = [&](uint32_t index, size_t classIndex) {
        Wire::Span checkedContents = m_sections.classParts[m_sections.types[index].classes.start + classIndex].contents;
        return m_sections.words.subspan(checkedContents.start, checkedContents.count);
    };
    ParallelArray<Type> types(loops, numberOfTypes, [&](unsigned index) {
        return WTF::move(typeAsRead(index).type);
    });
    Vector<uint8_t> nestingDepths(FillWith { }, numberOfTypes, static_cast<uint8_t>(0));
    forEachTypeInParallel([&](uint32_t index) {
        for (size_t classIndex = 0; classIndex < types[index].classes.size(); ++classIndex)
            nestingDepths[index] |= !contentsOf(index, classIndex).empty();
    });
    Vector<Vector<uint32_t>> typesByDepth(1);
    for (uint32_t index = 0; index < numberOfTypes; ++index) {
        if (nestingDepths[index]) {
            unsigned depth = 0;
            for (size_t classIndex = 0; classIndex < types[index].classes.size(); ++classIndex) {
                for (uint32_t earlierType : contentsOf(index, classIndex))
                    depth = std::max<unsigned>(depth, nestingDepths[earlierType]);
            }
            nestingDepths[index] = static_cast<uint8_t>(std::min(depth + 1, 255u));
            if (nestingDepths[index] == 255)
                return readTypesOneByOne();
            if (nestingDepths[index] >= typesByDepth.size())
                typesByDepth.grow(nestingDepths[index] + 1);
        }
        typesByDepth[nestingDepths[index]].append(index);
    }

    auto& table = m_program.types.recordsForOrderedInterning<Type>();
    OrderedInterning<Type, TypeID, std::remove_reference_t<decltype(table)>> interning(table, loops, types.mutableSpan());
    for (const Vector<uint32_t>& typesAtDepth : typesByDepth) {
        forEachIndexInParallel(loops, static_cast<unsigned>((typesAtDepth.size() + typesPerChunk - 1) / typesPerChunk), [&](unsigned chunk) {
            for (size_t position = chunk * typesPerChunk, end = std::min(typesAtDepth.size(), position + typesPerChunk); position < end; ++position) {
                uint32_t index = typesAtDepth[position];
                Type& type = types[index];
                for (size_t classIndex = 0; classIndex < type.classes.size(); ++classIndex) {
                    auto& contents = type.classes[classIndex].writtenContents;
                    std::span<const uint32_t> earlierTypes = contentsOf(index, classIndex);
                    contents.reserveInitialCapacity(earlierTypes.size());
                    for (uint32_t earlierType : earlierTypes) {
                        TypeID existing = interning.existingID(earlierType);
                        contents.append(existing ? existing : TypeID(TypeTable::provisionalBit | interning.equivalenceClass(earlierType)));
                    }
                }
                type.canonicalize();
                interning.setHash(index, TypeTable::hashOfCanonical(type));
            }
        });
        interning.classify(typesAtDepth.span());
    }
    interning.assignIDs();
    forEachTypeInParallel([&](uint32_t index) {
        if (!nestingDepths[index] || !interning.isNew(index))
            return;
        bool hasChanged = false;
        for (ClassPart& part : types[index].classes) {
            for (TypeID& contents : part.writtenContents) {
                if (!TypeTable::isProvisional(contents))
                    continue;
                contents = interning.idAt(contents.index() & ~TypeTable::provisionalBit);
                hasChanged = true;
            }
        }
        if (hasChanged)
            interning.setHash(index, TypeTable::hashOfCanonical(types[index]));
    });
    interning.commit();
    m_types = interning.takeIDs();
    m_build.destroyLater(WTF::move(chunksAsRead));
    m_build.destroyLater(WTF::move(types));
    m_build.destroyLater(WTF::move(sets));
    return true;
}

bool Reader::readTypesOneByOne()
{
    constexpr size_t typesPerChunk = 64;
    constexpr size_t typesPerRound = 1 << 16;
    size_t numberOfTypes = m_sections.types.size();
    m_types.reserveInitialCapacity(numberOfTypes);
    for (size_t first = 0; first < numberOfTypes; first += typesPerRound) {
        Vector<TypeAsRead> types(std::min(typesPerRound, numberOfTypes - first));
        auto failures = readInChunks(m_build.parallelLoops(), types.size(), typesPerChunk, [&](uint32_t position) {
            return readType(static_cast<uint32_t>(first + position), types[position]);
        });
        if (!reportFirst(failures.span()))
            return false;
        internLayoutSetsInOrder(types.mutableSpan());
        if (!m_completeType) {
            forEachIndexInParallel(m_build.parallelLoops(), static_cast<unsigned>((types.size() + typesPerChunk - 1) / typesPerChunk), [&](unsigned chunk) {
                size_t end = std::min(types.size(), (static_cast<size_t>(chunk) + 1) * typesPerChunk);
                for (size_t position = static_cast<size_t>(chunk) * typesPerChunk; position < end; ++position)
                    hashTypeWithoutContents(static_cast<uint32_t>(first + position), types[position]);
            });
        }
        constexpr size_t typesAhead = 8;
        for (size_t position = 0; position < types.size(); ++position) {
            if (position + typesAhead < types.size() && types[position + typesAhead].hashOfType)
                m_program.types.prefetchForType(*types[position + typesAhead].hashOfType);
            m_types.append(internType(static_cast<uint32_t>(first + position), WTF::move(types[position])));
        }
        m_build.destroyLater(WTF::move(types));
    }
    return true;
}

Failure Reader::readType(uint32_t index, TypeAsRead& result) const
{
    const Wire::Type& wire = m_sections.types[index];
    RecordChecker checker(*this, inRecord(index));
    Type& type = result.type;

    auto flags = checker.flags<TypeFlags>(wire.flags);
    readPrimitives(checker, wire, flags, type);
    type.anyObject = flags.contains(TypeFlags::AnyObject);
    type.anyFunction = flags.contains(TypeFlags::AnyFunction);

    type.stringLiterals = SortedSet<StringID>(collectAscendingIDs<StringID>(checker, checker.words(wire.strings)).span());
    auto layouts = collectAscendingIDs<LayoutID>(checker, checker.words(wire.layouts));
    auto functions = collectAscendingIDs<FunctionID>(checker, checker.words(wire.functions));

    auto classParts = checker.slice<Wire::ClassPart>(wire.classes);
    type.classes.reserveInitialCapacity(classParts.size());
    for (auto& wirePart : classParts) {
        ClassPart part;
        if (auto failure = readClassPart(checker, index, wirePart, part))
            return failure;
        if (!type.classes.isEmpty())
            checker.requireAscending(type.classes.last().builtinClass, part.builtinClass);
        type.classes.append(WTF::move(part));
    }

    type.builtins = readBuiltins(checker, wire.builtins);
    type.opaque = checker.optionalEnumerator<OpaqueKind>(wire.opaque);
    type.environment = checker.optionalID<LayoutID>(wire.environment);
    if (auto failure = checker.takeFailure())
        return failure;

    if (readsAsAnything(layouts.span())) {
        type = Type::anything().withPrimitives(type.primitives & Primitive::Absent);
        return std::nullopt;
    }
    if (m_form == IRForm::AsRead && std::ranges::any_of(layouts, [&](LayoutID layout) { return m_viewsWithoutLayouts.get(layout.index()); }))
        type.anyObject = true;
    addLayouts(result, layouts.span());
    addFunctions(type, functions.span());
    if (result.layoutsToIntern) {
        canonicalize(*result.layoutsToIntern);
        result.hashOfLayouts = TypeTable::hashOfCanonical(*result.layoutsToIntern);
    }
    return std::nullopt;
}

void Reader::internLayoutSetsInOrder(std::span<TypeAsRead> types)
{
    constexpr size_t typesAhead = 8;
    for (size_t position = 0; position < types.size(); ++position) {
        if (position + typesAhead < types.size() && types[position + typesAhead].layoutsToIntern)
            m_program.types.prefetchForLayoutSet(types[position + typesAhead].hashOfLayouts);
        TypeAsRead& read = types[position];
        if (!read.layoutsToIntern)
            continue;
        read.type.layouts = m_program.types.internCanonicalWithHash(WTF::move(*read.layoutsToIntern), read.hashOfLayouts);
        read.layoutsToIntern = std::nullopt;
    }
}

void Reader::hashTypeWithoutContents(uint32_t index, TypeAsRead& read) const
{
    Wire::Span checkedClasses = m_sections.types[index].classes;
    for (size_t position = 0; position < read.type.classes.size(); ++position) {
        if (m_sections.classParts[checkedClasses.start + position].contents.count)
            return;
    }
    read.type.canonicalize();
    read.hashOfType = TypeTable::hashOfCanonical(read.type);
}

TypeID Reader::internType(uint32_t index, TypeAsRead&& read)
{
    Type& type = read.type;
    if (read.hashOfType)
        return m_program.types.internCanonicalWithHash(WTF::move(type), *read.hashOfType);
    Wire::Span checkedClasses = m_sections.types[index].classes;
    for (size_t position = 0; position < type.classes.size(); ++position) {
        Wire::Span checkedContents = m_sections.classParts[checkedClasses.start + position].contents;
        auto& contents = type.classes[position].writtenContents;
        contents.reserveInitialCapacity(checkedContents.count);
        for (uint32_t earlierType : m_sections.words.subspan(checkedContents.start, checkedContents.count))
            contents.append(m_types[earlierType]);
    }
    if (m_completeType)
        m_completeType(index, type);
    return m_program.types.intern(WTF::move(type));
}

Failure Reader::readClassPart(RecordChecker& checker, uint32_t typeIndex, const Wire::ClassPart& wire, ClassPart& part) const
{
    part.builtinClass = checker.external<BuiltinClassID>(wire.builtinClass);
    if (auto failure = checker.takeFailure())
        return failure;
    checker.require(wire.contents.count == m_builtins.numberOfSlotsInType(part.builtinClass), InternalErrorKind::ListOutOfBounds);
    for (uint32_t word : checker.words(wire.contents))
        checker.require(word < typeIndex, InternalErrorKind::TypeRefersForward);
    return checker.takeFailure();
}

static void internStringTagKeys(Program& program, const ParallelLoops& loops)
{
    Vector<StringID> literals = collectInParallel<StringID>(loops, program.layouts.size(), [&](unsigned index, Vector<StringID>& found) {
        for (const Field& field : program.layouts[index].fields) {
            if (!field.declared.written())
                continue;
            for (StringID literal : program.at(field.declared.written()).stringLiterals)
                found.append(literal);
        }
    });
    for (StringID literal : literals)
        program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = literal });
}

// What every store into the slot is held to, by the code of that image and by the run time. A set of objects of that image is no set of this program: it is any object here.
static TypeID typeInSlotBelow(TypeArena& types, const RuntimeType& recorded, const std::optional<Vector<LayoutID>>& objects)
{
    uint16_t primitives = recorded.primitives;
    if (primitives & Primitive::Unspecified)
        return TypeTable::anything;
    if (primitives & Primitive::LiteralString)
        primitives = (primitives & ~Primitive::LiteralString) | Primitive::String;
    if (primitives & Primitive::LiteralSymbol)
        primitives = (primitives & ~Primitive::LiteralSymbol) | Primitive::Symbol;
    Type type = Type::fromPrimitives(primitives);
    if (std::optional<std::pair<int64_t, int64_t>> range = recorded.integerRange(); range && primitives & Primitive::Integer)
        type.integers = Range { range->first, range->second };
    for (const RuntimeCellTest& test : recorded.cellTests()) {
        if (test.kind == CellTestKind::IsSymbol) {
            type.primitives |= Primitive::Symbol;
            continue;
        }
        if (objects)
            continue;
        type.anyObject = true;
        type.anyFunction = true;
    }
    if (objects)
        type.layouts = types.completeLayoutSet(objects->span());
    return types.intern(WTF::move(type));
}

static void appendLayoutsBelow(Program& program, BuildContext& build)
{
    if (program.recordsOfImagesBelow.isEmpty())
        return;
    const ImagesBelow& below = *build.imagesBelow();
    TypeArena types(program.types);
    for (const Program::RecordOfImageBelow& planned : program.recordsOfImagesBelow) {
        ImagesBelow::Record record = below.recordAt(planned.image, planned.firstShape, planned.numberOfShapes);
        Layout& layout = program.at(planned.layout);
        layout.kind = LayoutKind::Record;
        layout.flags = { LayoutFlags::IsOfImageBelow };
        for (const FieldData& data : record.layout->fields()) {
            Field field;
            field.key = program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = program.strings.intern(below.textOfName(data.name)) });
            TypeID type = typeInSlotBelow(types, below.typeOf(record, data), planned.objectsInFields[record.layout->indexOf(data)]);
            field.declared = DeclaredType(Proven::byPosition(type));
            field.slotType = type;
            if (data.isRequired())
                field.flags.add(FieldFlags::IsRequired);
            layout.fields.append(WTF::move(field));
        }
    }
}

static void appendDerivedLayouts(Program& program, BuildContext& build, std::span<const LayoutSetID> declaredMembers)
{
    size_t numberOfLayoutsAsRead = program.layouts.size();
    program.recordCountsAsRead();

    program.internBuiltinNameKeys();
    internStringTagKeys(program, build.parallelLoops());
    program.appendBuiltinLayouts();
    program.appendNamespaceLayouts();
    appendTaggedLayouts(program, program.taggedFamilies.span());
    for (const Program::RecordOfImageBelow& planned : program.recordsOfImagesBelow) {
        RELEASE_ASSERT(planned.layout.index() == program.layouts.size());
        program.layouts.append(Layout { });
        program.setDeclaredMembers(planned.layout, program.types.intern(LayoutSet::complete({ planned.layout })));
    }
    appendLayoutsBelow(program, build);
    RELEASE_ASSERT(program.layouts.size() == declaredMembers.size());

    for (size_t index = 0; index < numberOfLayoutsAsRead; ++index)
        program.setDeclaredMembers(LayoutID(index), declaredMembers[index]);
    initializeLayouts(program, build);
    appendFieldsOfTaggedLayouts(program, program.taggedFamilies.span());
}

bool Reader::readLayouts()
{
    constexpr size_t layoutsPerChunk = 64;
    size_t numberOfLayoutIDs = numberOfRecords<LayoutID>();
    m_program.layouts.reserveCapacity(numberOfLayoutIDs + numberOfLayoutIDs / 2);
    growFromEmpty(m_build.parallelLoops(), m_program.layouts, m_sections.layouts.size());
    auto failures = readInChunks(m_build.parallelLoops(), m_sections.layouts.size(), layoutsPerChunk, [&](uint32_t index) {
        RecordChecker checker(*this, inLayout(index));
        read(checker, m_sections.layouts[index], static_cast<LayoutRecord&>(m_program.layouts[index]));
        return checker.takeFailure();
    });
    if (!reportFirst(failures.span()))
        return false;

    appendDerivedLayouts(m_program, m_build, m_declaredMembers.span());
    return checkTypesAgainstLayouts();
}

bool Reader::checkTypesAgainstLayouts()
{
    constexpr size_t recordsPerChunk = 256;
    const ParallelLoops& loops = m_build.parallelLoops();
    Vector<uint8_t> isNamedByType(FillWith { }, m_program.types.numberOfLayoutSets(), static_cast<uint8_t>(0));
    forEachIndexInParallel(loops, static_cast<unsigned>((m_types.size() + recordsPerChunk - 1) / recordsPerChunk), [&](unsigned chunk) {
        for (size_t index = chunk * recordsPerChunk, end = std::min<size_t>(m_types.size(), index + recordsPerChunk); index < end; ++index) {
            if (LayoutSetID set = m_program.at(m_types[index]).layouts)
                AtomicRef(isNamedByType[set.index()]).store(1, std::memory_order_relaxed);
        }
    });
    Vector<LayoutID> environmentInSet(isNamedByType.size());
    forEachIndexInParallel(loops, static_cast<unsigned>((isNamedByType.size() + recordsPerChunk - 1) / recordsPerChunk), [&](unsigned chunk) {
        for (size_t index = chunk * recordsPerChunk, end = std::min<size_t>(isNamedByType.size(), index + recordsPerChunk); index < end; ++index) {
            if (!isNamedByType[index])
                continue;
            for (LayoutID layout : m_program.at(LayoutSetID(static_cast<uint32_t>(index))).written()) {
                if (m_program.at(layout).isEnvironment()) {
                    environmentInSet[index] = layout;
                    break;
                }
            }
        }
    });
    auto failures = readInChunks(loops, m_types.size(), recordsPerChunk, [&](uint32_t index) -> Failure {
        const Type& type = m_program.at(m_types[index]);
        if (LayoutID layout = type.layouts ? environmentInSet[type.layouts.index()] : LayoutID())
            return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = layout, .count = index };
        if (type.environment && !m_program.at(type.environment).isEnvironment())
            return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = type.environment, .count = index };
        return std::nullopt;
    });
    return reportFirst(failures.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
