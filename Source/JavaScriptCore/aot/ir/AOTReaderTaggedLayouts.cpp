/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include <algorithm>
#include <atomic>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class TaggedLayoutPlanner {
public:
    TaggedLayoutPlanner(const ParallelLoops& loops, const Wire::Sections& checkedSections, const ExternalFunctionBodies* externalBodies, Vector<Vector<LayoutID>>& members, const TellingFields& tellingFields, uint32_t firstLayout)
        : m_loops(loops)
        , m_sections(checkedSections)
        , m_externalBodies(externalBodies)
        , m_layouts(checkedSections.layouts)
        , m_members(members)
        , m_tellingFields(tellingFields)
        , m_nextLayout(firstLayout)
    {
    }

    Vector<TaggedFamily> plan();

private:
    struct LiteralField {
        uint32_t key;
        LayoutTag tag;
    };

    struct Addition {
        uint32_t type;
        LayoutID member;
        uint32_t tagKey;
        LayoutTag tag;
    };

    std::span<const Wire::Field> fieldsOf(const Wire::Layout&) const;
    std::span<const uint32_t> supertypesOf(const Wire::Layout& layout) const { return m_sections.words.subspan(layout.supertypes.start, layout.supertypes.count); }
    std::span<const LiteralField> literalFieldsOf(uint32_t type) const { return m_literalFields.subspan(m_firstLiteralField[type], m_firstLiteralField[type + 1] - m_firstLiteralField[type]); }
    bool isMaker(const Wire::Layout&) const;
    std::optional<uint32_t> typeNamedByRecordOf(size_t function, uint32_t type) const;
    Vector<uint8_t> findClassesWithInitializer() const;
    bool isDeclaredType(const Wire::Layout&) const;
    std::optional<LayoutTag> literalOf(const Wire::Type&) const;
    std::optional<LayoutTag> tagOf(uint32_t type, uint32_t key) const;

    void collectLiteralFields();
    Vector<uint32_t> collectTypesWithLiterals(const Wire::Layout& maker) const;
    uint32_t chooseTagKey(const Wire::Layout& maker, std::span<const uint32_t> types) const;
    Vector<uint32_t> collectDeclaredNames(const TaggedLayout&) const;
    void joinMembersWithSameNames(TaggedFamily&) const;
    void planFamily(uint32_t maker);
    bool makesTaggedObjectsOf(uint32_t type, std::span<const Addition* const> addedBelowMember, uint32_t member) const;
    void applyAdditions();

    const ParallelLoops& m_loops;
    const Wire::Sections& m_sections;
    const ExternalFunctionBodies* m_externalBodies;
    std::span<const Wire::Layout> m_layouts;
    Vector<Vector<LayoutID>>& m_members;
    const TellingFields& m_tellingFields;
    uint32_t m_nextLayout;
    Vector<LiteralField> m_literalFields;
    Vector<uint32_t> m_firstLiteralField;
    Vector<Addition> m_additions;
    Vector<TaggedFamily> m_families;
};

} // anonymous namespace

static bool has(const Wire::Layout& layout, LayoutFlags flag)
{
    return layout.flags & static_cast<uint32_t>(flag);
}

static bool has(const Wire::Field& field, FieldFlags flag)
{
    return field.flags & static_cast<uint32_t>(flag);
}

// Turns the number of entries of each index into the place of its first entry. One more place follows the last.
static void sumUp(Vector<uint32_t>& counts)
{
    uint32_t sum = 0;
    for (uint32_t& count : counts)
        count = std::exchange(sum, sum + count);
    counts.append(sum);
}

TellingFields::TellingFields(const ParallelLoops& loops, const Wire::Sections& checkedSections)
{
    constexpr uint32_t nullish = static_cast<uint32_t>(Wire::Primitives::Undefined) | static_cast<uint32_t>(Wire::Primitives::Null);
    std::span<const Wire::Field> fields = checkedSections.fields;
    std::span<const uint32_t> words = checkedSections.words;
    m_firstField.reserveInitialCapacity(checkedSections.layouts.size() + 1);
    m_firstField.grow(checkedSections.layouts.size());
    m_bases.grow(checkedSections.layouts.size());
    m_fields = collectInParallel<Field>(loops, static_cast<unsigned>(checkedSections.layouts.size()), [&](unsigned index, Vector<Field>& found) {
        const Wire::Layout& layout = checkedSections.layouts[index];
        size_t sizeBefore = found.size();
        auto countFields = makeScopeExit([&] {
            m_firstField[index] = static_cast<uint32_t>(found.size() - sizeBefore);
        });
        m_bases[index] = layout.base < checkedSections.layouts.size() ? layout.base : Wire::none;
        if (layout.fields.start > fields.size() || layout.fields.count > fields.size() - layout.fields.start)
            return;
        for (const Wire::Field& field : fields.subspan(layout.fields.start, layout.fields.count)) {
            if (field.key == Wire::none || field.declared.type >= checkedSections.types.size())
                continue;
            if (field.declared.flags & static_cast<uint32_t>(PositionFlags::IsExpected))
                continue;
            const Wire::Type& type = checkedSections.types[field.declared.type];
            bool hasOtherParts = (type.primitives & ~nullish)
                || type.opaque != Wire::none
                || type.environment != Wire::none
                || type.layouts.count
                || type.classes.count
                || type.builtins.count
                || type.functions.count;
            if (hasOtherParts)
                continue;
            if (type.flags == static_cast<uint32_t>(TypeFlags::HasIntegers) && !type.strings.count) {
                found.append({ field.key, type.primitives & nullish, false, type.minimum, type.maximum, { } });
                continue;
            }
            if (type.flags || !type.strings.count || type.strings.start > words.size() || type.strings.count > words.size() - type.strings.start)
                continue;
            found.append({ field.key, type.primitives & nullish, true, 0, 0, words.subspan(type.strings.start, type.strings.count) });
        }
    });
    sumUp(m_firstField);
}

bool TellingFields::haveNothingInCommon(const Field& a, const Field& b)
{
    if (a.nullish & b.nullish)
        return false;
    if (a.holdsStrings != b.holdsStrings)
        return true;
    if (!a.holdsStrings)
        return a.maximum < b.minimum || b.maximum < a.minimum;
    return std::ranges::none_of(a.strings, [&](uint32_t string) {
        return std::ranges::find(b.strings, string) != b.strings.end();
    });
}

// An object has the fields of its bases too. The bases of a program that has not been checked may form a ring.
bool TellingFields::tellApart(uint32_t layout, uint32_t otherLayout) const
{
    size_t remainingBases = m_bases.size();
    for (uint32_t current = layout; current != Wire::none && remainingBases--; current = m_bases[current]) {
        for (const Field& field : fieldsOf(current)) {
            for (const Field& otherField : fieldsOf(otherLayout)) {
                if (field.key == otherField.key && haveNothingInCommon(field, otherField))
                    return true;
            }
        }
    }
    return false;
}

bool TellingFields::excludes(uint32_t layout, uint32_t key, const LayoutTag& tag) const
{
    for (const Field& field : fieldsOf(layout)) {
        if (field.key != key)
            continue;
        if (const int64_t* integer = std::get_if<int64_t>(&tag))
            return field.holdsStrings || *integer < field.minimum || *integer > field.maximum;
        return !field.holdsStrings || std::ranges::find(field.strings, std::get<StringID>(tag).index()) == field.strings.end();
    }
    return false;
}

std::span<const Wire::Field> TaggedLayoutPlanner::fieldsOf(const Wire::Layout& layout) const
{
    std::span<const Wire::Field> fields = m_sections.fields;
    if (layout.fields.start > fields.size() || layout.fields.count > fields.size() - layout.fields.start)
        return { };
    return fields.subspan(layout.fields.start, layout.fields.count);
}

// A constructor function, or a class without heritage: its constructor allocates the objects.
bool TaggedLayoutPlanner::isMaker(const Wire::Layout& layout) const
{
    return static_cast<LayoutKind>(layout.kind) == LayoutKind::Instance
        && !has(layout, LayoutFlags::HasHeritage)
        && layout.constructor != Wire::none
        && layout.supertypes.count;
}

// AllocateTaggedLayouts leaves the constructor of such a class as it is.
std::optional<uint32_t> TaggedLayoutPlanner::typeNamedByRecordOf(size_t function, uint32_t type) const
{
    if (!m_externalBodies)
        return type;
    if (function >= m_externalBodies->bodyCount)
        return std::nullopt;
    uint32_t unit = m_externalBodies->bodies[function].unitIndex;
    if (unit >= m_externalBodies->unitCount)
        return std::nullopt;
    const IDRemapTable& types = m_externalBodies->units[unit].tables[static_cast<size_t>(RemappedIDKind::Type)];
    if (type >= types.size || !types.entries[type])
        return std::nullopt;
    return types.entries[type] - 1;
}

Vector<uint8_t> TaggedLayoutPlanner::findClassesWithInitializer() const
{
    constexpr size_t functionsPerTask = 2048;
    Vector<uint8_t> classes(FillWith { }, m_layouts.size(), static_cast<uint8_t>(0));
    std::span<const uint32_t> words = m_sections.words;
    std::span<const Wire::Function> functions = m_sections.functions;
    // Whichever thread finds a class writes the same.
    forEachIndexInParallel(m_loops, static_cast<unsigned>((functions.size() + functionsPerTask - 1) / functionsPerTask), [&](unsigned task) {
        for (size_t index = task * functionsPerTask, end = std::min(functions.size(), index + functionsPerTask); index < end; ++index) {
            const Wire::Function& function = functions[index];
            if (function.kind != static_cast<uint32_t>(FunctionKind::Initializer))
                continue;
            std::optional<uint32_t> thisType = typeNamedByRecordOf(index, function.thisType.type);
            if (!thisType || *thisType >= m_sections.types.size())
                continue;
            Wire::Span named = m_sections.types[*thisType].layouts;
            if (named.start > words.size() || named.count > words.size() - named.start)
                continue;
            for (uint32_t layout : words.subspan(named.start, named.count)) {
                if (layout < m_layouts.size())
                    std::atomic_ref(classes[layout]).store(1, std::memory_order_relaxed);
            }
        }
    });
    return classes;
}

bool TaggedLayoutPlanner::isDeclaredType(const Wire::Layout& layout) const
{
    return static_cast<LayoutKind>(layout.kind) == LayoutKind::Record && layout.constructor == Wire::none;
}

// undefined and null are members of every type that holds a reference, so also of the type of one string.
std::optional<LayoutTag> TaggedLayoutPlanner::literalOf(const Wire::Type& type) const
{
    constexpr uint32_t nullish = static_cast<uint32_t>(Wire::Primitives::Undefined) | static_cast<uint32_t>(Wire::Primitives::Null);
    bool hasOtherParts = (type.primitives & ~nullish)
        || type.opaque != Wire::none
        || type.environment != Wire::none
        || type.layouts.count
        || type.classes.count
        || type.builtins.count
        || type.functions.count;
    if (hasOtherParts)
        return std::nullopt;

    if (type.flags == static_cast<uint32_t>(TypeFlags::HasIntegers) && !type.strings.count) {
        int64_t minimum = type.minimum;
        int64_t maximum = type.maximum;
        if (minimum != maximum)
            return std::nullopt;
        return LayoutTag { minimum };
    }

    if (type.flags || type.strings.count != 1 || type.strings.start >= m_sections.words.size())
        return std::nullopt;
    uint32_t string = m_sections.words[type.strings.start];
    if (string >= m_sections.strings.size())
        return std::nullopt;
    return LayoutTag { StringID(string) };
}

std::optional<LayoutTag> TaggedLayoutPlanner::tagOf(uint32_t type, uint32_t key) const
{
    for (const LiteralField& field : literalFieldsOf(type)) {
        if (field.key == key)
            return field.tag;
    }
    return std::nullopt;
}

// Few declared types write a literal for a field, and every maker asks about all the types below its own.
void TaggedLayoutPlanner::collectLiteralFields()
{
    m_firstLiteralField.reserveInitialCapacity(m_layouts.size() + 1);
    m_firstLiteralField.fill(0u, m_layouts.size());
    m_literalFields = collectInParallel<LiteralField>(m_loops, static_cast<unsigned>(m_layouts.size()), [&](unsigned index, Vector<LiteralField>& found) {
        const Wire::Layout& layout = m_layouts[index];
        if (!isDeclaredType(layout))
            return;
        for (const Wire::Field& field : fieldsOf(layout)) {
            if (field.key == Wire::none || field.declared.type >= m_sections.types.size())
                continue;
            if (std::optional<LayoutTag> tag = literalOf(m_sections.types[field.declared.type])) {
                found.append({ field.key, *tag });
                ++m_firstLiteralField[index];
            }
        }
    });
    sumUp(m_firstLiteralField);
}

// Ascending. The families are closed over the conversions of the program, so those of the maker's supertypes hold every type whose objects the program takes for one of them.
Vector<uint32_t> TaggedLayoutPlanner::collectTypesWithLiterals(const Wire::Layout& maker) const
{
    Vector<uint32_t> types;
    for (uint32_t supertype : supertypesOf(maker)) {
        for (LayoutID below : m_members[supertype]) {
            if (!literalFieldsOf(below.index()).empty())
                types.append(below.index());
        }
    }
    std::ranges::sort(types);
    removeRepeatedElements(types);
    return types;
}

// The field of the maker for which the types write the most literals, the first of several. One literal tells no two objects apart.
uint32_t TaggedLayoutPlanner::chooseTagKey(const Wire::Layout& maker, std::span<const uint32_t> types) const
{
    Vector<LiteralField, 32> literals;
    for (uint32_t type : types)
        literals.append(literalFieldsOf(type));
    std::sort(literals.begin(), literals.end(), [](const LiteralField& a, const LiteralField& b) {
        return a.key != b.key ? a.key < b.key : a.tag < b.tag;
    });
    removeRepeatedElements(literals, [](const LiteralField& a, const LiteralField& b) {
        return a.key == b.key && a.tag == b.tag;
    });

    uint32_t bestKey = Wire::none;
    size_t bestCount = 1;
    for (const Wire::Field& field : fieldsOf(maker)) {
        if (field.key == Wire::none)
            continue;
        size_t count = std::ranges::equal_range(literals, field.key, { }, &LiteralField::key).size();
        if (count > bestCount) {
            bestCount = count;
            bestKey = field.key;
        }
    }
    return bestKey;
}

// What appendFieldsOfTaggedLayouts() takes from the declaring types. Ascending.
Vector<uint32_t> TaggedLayoutPlanner::collectDeclaredNames(const TaggedLayout& member) const
{
    Vector<uint32_t> names;
    for (LayoutID type : member.declaringTypes) {
        for (const Wire::Field& field : fieldsOf(m_layouts[type.index()])) {
            if (field.key != Wire::none && !has(field, FieldFlags::IsMethod) && !has(field, FieldFlags::IsNeverWritten))
                names.append(field.key);
        }
    }
    std::ranges::sort(names);
    removeRepeatedElements(names);
    return names;
}

void TaggedLayoutPlanner::joinMembersWithSameNames(TaggedFamily& family) const
{
    Vector<Vector<uint32_t>> namesOfKeptMembers;
    for (size_t index = 0; index < family.members.size(); ++index) {
        Vector<uint32_t> names = collectDeclaredNames(family.members[index]);
        size_t position = namesOfKeptMembers.find(names);
        if (position != notFound) {
            family.members[position].tags.appendVector(family.members[index].tags);
            family.members[position].declaringTypes.appendVector(family.members[index].declaringTypes);
            continue;
        }
        if (namesOfKeptMembers.size() != index)
            family.members[namesOfKeptMembers.size()] = WTF::move(family.members[index]);
        namesOfKeptMembers.append(WTF::move(names));
    }
    family.members.shrink(namesOfKeptMembers.size());
}

void TaggedLayoutPlanner::planFamily(uint32_t makerIndex)
{
    const Wire::Layout& maker = m_layouts[makerIndex];
    Vector<uint32_t> types = collectTypesWithLiterals(maker);
    uint32_t tagKey = chooseTagKey(maker, types.span());
    if (tagKey == Wire::none)
        return;

    TaggedFamily family { LayoutID(makerIndex), PropertyKeyID(tagKey), { } };
    for (uint32_t type : types) {
        std::optional<LayoutTag> tag = tagOf(type, tagKey);
        if (!tag)
            continue;
        size_t position = family.members.findIf([&](const TaggedLayout& member) {
            return member.tags[0] == *tag;
        });
        if (position == notFound) {
            position = family.members.size();
            family.members.append({ { *tag }, { }, { } });
        }
        family.members[position].declaringTypes.append(LayoutID(type));
    }
    std::sort(family.members.begin(), family.members.end(), [](const TaggedLayout& a, const TaggedLayout& b) {
        return a.tags[0] < b.tags[0];
    });
    joinMembersWithSameNames(family);

    for (TaggedLayout& member : family.members) {
        member.layout = LayoutID(m_nextLayout++);
        for (LayoutID type : member.declaringTypes)
            m_additions.append({ type.index(), member.layout, tagKey, *tagOf(type.index(), tagKey) });
        for (const LayoutTag& tag : member.tags)
            m_additions.append({ makerIndex, member.layout, tagKey, tag });
    }
    m_families.append(WTF::move(family));
}

// A maker gives an object the layout of its tag wherever there is one.
bool TaggedLayoutPlanner::makesTaggedObjectsOf(uint32_t type, std::span<const Addition* const> addedBelowMember, uint32_t member) const
{
    if (addedBelowMember.empty() || member == type || !isMaker(m_layouts[member]))
        return false;
    return m_tellingFields.declaresOnlyLiterals(type, addedBelowMember[0]->tagKey, addedBelowMember.size(), [&](const LayoutTag& literal) {
        return std::ranges::any_of(addedBelowMember, [&](const Addition* addition) {
            return addition->tag == literal;
        });
    });
}

// A tagged layout is below its maker and its declaring types, and so below everything that one of them is below, but for a type that writes other literals for the tag.
void TaggedLayoutPlanner::applyAdditions()
{
    Vector<Vector<const Addition*, 2>> addedBelow(m_layouts.size());
    for (const Addition& addition : m_additions)
        addedBelow[addition.type].append(&addition);

    forEachIndexInParallel(m_loops, static_cast<unsigned>(m_members.size()), [&](unsigned type) {
        Vector<LayoutID>& membersOfLayout = m_members[type];
        size_t sizeBefore = membersOfLayout.size();
        Vector<LayoutID, 4> makersToLeaveOut;
        for (size_t position = 0; position < sizeBefore; ++position) {
            uint32_t member = membersOfLayout[position].index();
            for (const Addition* addition : addedBelow[member]) {
                if (!m_tellingFields.excludes(type, addition->tagKey, addition->tag))
                    membersOfLayout.append(addition->member);
            }
            if (makesTaggedObjectsOf(type, addedBelow[member].span(), member))
                makersToLeaveOut.append(LayoutID(member));
        }
        if (membersOfLayout.size() == sizeBefore)
            return;
        membersOfLayout.removeAllMatching([&](LayoutID member) {
            return makersToLeaveOut.contains(member);
        });
        std::ranges::sort(membersOfLayout);
        removeRepeatedElements(membersOfLayout);
    });
}

Vector<TaggedFamily> TaggedLayoutPlanner::plan()
{
    collectLiteralFields();
    if (m_literalFields.isEmpty())
        return { };

    Vector<uint8_t> classesWithInitializer = findClassesWithInitializer();
    for (uint32_t index = 0; index < m_layouts.size(); ++index) {
        if (isMaker(m_layouts[index]) && !classesWithInitializer[index])
            planFamily(index);
    }
    if (!m_families.isEmpty())
        applyAdditions();
    return WTF::move(m_families);
}

// The families come in the order of their makers.
void Reader::planTaggedLayouts(Vector<Vector<LayoutID>>& members, const TellingFields& tellingFields)
{
    if (m_form != IRForm::AsRead)
        return;
    uint32_t firstLayout = static_cast<uint32_t>(numberOfRecords<LayoutID>());
    m_program.taggedFamilies = TaggedLayoutPlanner(m_build.parallelLoops(), m_sections, m_externalBodies, members, tellingFields, firstLayout).plan();
    for (const TaggedFamily& family : m_program.taggedFamilies)
        m_numberOfDerivedLayouts += family.members.size();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
