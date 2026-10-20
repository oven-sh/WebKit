/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using KeyList = Vector<uint32_t, 16>;

class TaggedLayoutPlanner {
public:
    TaggedLayoutPlanner(const Wire::Sections& checkedSections, Vector<Vector<LayoutID>>& members, uint32_t firstLayout)
        : m_sections(checkedSections)
        , m_layouts(checkedSections.layouts)
        , m_members(members)
        , m_nextLayout(firstLayout)
    {
    }

    Vector<TaggedFamily> plan();

private:
    struct Addition {
        uint32_t type;
        LayoutID member;
    };

    std::span<const Wire::Field> fieldsOf(const Wire::Layout&) const;
    std::span<const uint32_t> supertypesOf(const Wire::Layout& layout) const { return m_sections.words.subspan(layout.supertypes.start, layout.supertypes.count); }
    bool isMaker(const Wire::Layout&) const;
    bool isDeclaredType(const Wire::Layout&) const;
    KeyList keysOf(const Wire::Layout&, bool onlyRequired = false) const;
    std::optional<int64_t> tagOf(const Wire::Layout&, uint32_t key) const;

    Vector<uint32_t> collectTypesBelowDeclaredThis(const Wire::Layout& maker) const;
    uint32_t chooseTagKey(const Wire::Layout& maker, std::span<const uint32_t> types) const;
    void planFamily(uint32_t maker);
    void addByNames(LayoutID member, const KeyList& keysOfMember);
    void applyAdditions();

    const Wire::Sections& m_sections;
    std::span<const Wire::Layout> m_layouts;
    Vector<Vector<LayoutID>>& m_members;
    uint32_t m_nextLayout;
    Vector<uint32_t> m_declaredTypes;
    Vector<KeyList> m_keysOfDeclaredTypes;
    Vector<Addition> m_additions;
    Vector<Addition> m_additionsByNames;
    Vector<TaggedFamily> m_families;
};

} // anonymous namespace

std::span<const Wire::Field> TaggedLayoutPlanner::fieldsOf(const Wire::Layout& layout) const
{
    std::span<const Wire::Field> fields = m_sections.fields;
    if (layout.fields.start > fields.size() || layout.fields.count > fields.size() - layout.fields.start)
        return { };
    return fields.subspan(layout.fields.start, layout.fields.count);
}

bool TaggedLayoutPlanner::isMaker(const Wire::Layout& layout) const
{
    return static_cast<LayoutKind>(layout.kind) == LayoutKind::Instance
        && (layout.flags & static_cast<uint32_t>(LayoutFlags::IsConstructorFunction))
        && layout.constructor != Wire::none
        && layout.supertypes.count;
}

bool TaggedLayoutPlanner::isDeclaredType(const Wire::Layout& layout) const
{
    return static_cast<LayoutKind>(layout.kind) == LayoutKind::Record && layout.constructor == Wire::none;
}

KeyList TaggedLayoutPlanner::keysOf(const Wire::Layout& layout, bool onlyRequired) const
{
    KeyList keys;
    for (const Wire::Field& field : fieldsOf(layout)) {
        if (field.key == Wire::none || (field.flags & static_cast<uint32_t>(FieldFlags::IsMethod)))
            continue;
        if (!onlyRequired || (field.flags & static_cast<uint32_t>(FieldFlags::IsRequired)))
            keys.append(field.key);
    }
    std::ranges::sort(keys);
    return keys;
}

std::optional<int64_t> TaggedLayoutPlanner::tagOf(const Wire::Layout& layout, uint32_t key) const
{
    for (const Wire::Field& field : fieldsOf(layout)) {
        if (field.key != key)
            continue;
        if (field.declared.predicted >= m_sections.types.size())
            return std::nullopt;
        const Wire::Type& type = m_sections.types[field.declared.predicted];
        bool hasOnlyIntegers = type.flags == static_cast<uint32_t>(TypeFlags::HasIntegers)
            && !type.primitives
            && type.opaque == Wire::none
            && type.environment == Wire::none
            && !type.strings.count
            && !type.layouts.count
            && !type.classes.count
            && !type.builtins.count;
        int64_t minimum = type.minimum;
        int64_t maximum = type.maximum;
        if (!hasOnlyIntegers || minimum != maximum)
            return std::nullopt;
        return minimum;
    }
    return std::nullopt;
}

Vector<uint32_t> TaggedLayoutPlanner::collectTypesBelowDeclaredThis(const Wire::Layout& maker) const
{
    BitVector isBelow(m_layouts.size());
    KeyList required;
    for (uint32_t declaredThis : supertypesOf(maker)) {
        for (LayoutID below : m_members[declaredThis]) {
            if (isDeclaredType(m_layouts[below.index()]))
                isBelow.quickSet(below.index());
        }
        required.appendVector(keysOf(m_layouts[declaredThis], true));
    }
    std::ranges::sort(required);

    Vector<uint32_t> types;
    for (size_t position = 0; position < m_declaredTypes.size(); ++position) {
        uint32_t type = m_declaredTypes[position];
        if (isBelow.quickGet(type) || (!required.isEmpty() && std::ranges::includes(m_keysOfDeclaredTypes[position], required)))
            types.append(type);
    }
    return types;
}

uint32_t TaggedLayoutPlanner::chooseTagKey(const Wire::Layout& maker, std::span<const uint32_t> types) const
{
    uint32_t bestKey = Wire::none;
    size_t bestCount = 1;
    for (const Wire::Field& field : fieldsOf(maker)) {
        if (field.key == Wire::none)
            continue;
        Vector<int64_t, 16> tags;
        for (uint32_t type : types) {
            if (std::optional<int64_t> tag = tagOf(m_layouts[type], field.key))
                tags.append(*tag);
        }
        std::ranges::sort(tags);
        size_t count = std::ranges::unique(tags).begin() - tags.begin();
        if (count > bestCount) {
            bestCount = count;
            bestKey = field.key;
        }
    }
    return bestKey;
}

void TaggedLayoutPlanner::addByNames(LayoutID member, const KeyList& keysOfMember)
{
    for (size_t position = 0; position < m_declaredTypes.size(); ++position) {
        const KeyList& keys = m_keysOfDeclaredTypes[position];
        if (!keys.isEmpty() && std::ranges::includes(keysOfMember, keys))
            m_additionsByNames.append({ m_declaredTypes[position], member });
    }
}

void TaggedLayoutPlanner::planFamily(uint32_t makerIndex)
{
    const Wire::Layout& maker = m_layouts[makerIndex];
    Vector<uint32_t> types = collectTypesBelowDeclaredThis(maker);
    uint32_t tagKey = chooseTagKey(maker, types.span());
    if (tagKey == Wire::none)
        return;

    TaggedFamily family { LayoutID(makerIndex), PropertyKeyID(tagKey), { } };
    for (uint32_t type : types) {
        std::optional<int64_t> tag = tagOf(m_layouts[type], tagKey);
        if (!tag)
            continue;
        size_t position = family.members.findIf([&](const TaggedLayout& member) {
            return member.tag == *tag;
        });
        if (position == notFound) {
            position = family.members.size();
            family.members.append({ *tag, { }, { } });
        }
        family.members[position].declaringTypes.append(LayoutID(type));
    }
    std::ranges::sort(family.members, { }, &TaggedLayout::tag);

    KeyList keysOfMaker = keysOf(maker);
    for (TaggedLayout& member : family.members) {
        member.layout = LayoutID(m_nextLayout++);
        KeyList keys = keysOfMaker;
        for (LayoutID type : member.declaringTypes) {
            keys.appendVector(keysOf(m_layouts[type.index()]));
            m_additions.append({ type.index(), member.layout });
        }
        m_additions.append({ makerIndex, member.layout });
        std::ranges::sort(keys);
        addByNames(member.layout, keys);
    }
    m_families.append(WTF::move(family));
}

void TaggedLayoutPlanner::applyAdditions()
{
    Vector<Vector<LayoutID, 2>> addedBelow(m_layouts.size());
    for (const Addition& addition : m_additions)
        addedBelow[addition.type].append(addition.member);

    auto sortAndRemoveDuplicates = [](Vector<LayoutID>& list) {
        std::ranges::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    };

    for (Vector<LayoutID>& membersOfLayout : m_members) {
        size_t sizeBefore = membersOfLayout.size();
        for (size_t position = 0; position < sizeBefore; ++position)
            membersOfLayout.appendVector(addedBelow[membersOfLayout[position].index()]);
        if (membersOfLayout.size() != sizeBefore)
            sortAndRemoveDuplicates(membersOfLayout);
    }

    BitVector isChanged(m_layouts.size());
    for (const Addition& addition : m_additionsByNames) {
        m_members[addition.type].append(addition.member);
        isChanged.quickSet(addition.type);
    }
    for (size_t type : isChanged)
        sortAndRemoveDuplicates(m_members[type]);
}

Vector<TaggedFamily> TaggedLayoutPlanner::plan()
{
    Vector<uint32_t, 8> makers;
    for (uint32_t index = 0; index < m_layouts.size(); ++index) {
        if (isMaker(m_layouts[index]))
            makers.append(index);
    }
    if (makers.isEmpty())
        return { };

    for (uint32_t index = 0; index < m_layouts.size(); ++index) {
        if (!isDeclaredType(m_layouts[index]))
            continue;
        m_declaredTypes.append(index);
        m_keysOfDeclaredTypes.append(keysOf(m_layouts[index]));
    }

    for (uint32_t maker : makers) {
        addByNames(LayoutID(maker), keysOf(m_layouts[maker]));
        planFamily(maker);
    }
    applyAdditions();
    return WTF::move(m_families);
}

void Reader::planTaggedLayouts(Vector<Vector<LayoutID>>& members)
{
    if (m_form != IRForm::AsRead)
        return;
    uint32_t firstLayout = static_cast<uint32_t>(numberOfRecords<LayoutID>());
    m_taggedFamilies = TaggedLayoutPlanner(m_sections, members, firstLayout).plan();
    for (const TaggedFamily& family : m_taggedFamilies)
        m_numberOfDerivedLayouts += family.members.size();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
