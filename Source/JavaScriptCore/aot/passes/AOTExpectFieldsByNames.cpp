/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpectFieldsByNames.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTPropertyAccess.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(size_t, minNamesAskedOfField, 2);
AOT_THRESHOLD(size_t, maxLayoutsExpectedOfField, 2);
AOT_THRESHOLD(size_t, maxReceiversOfFieldRead, 4);
AOT_THRESHOLD(size_t, maxLayoutsComparedForField, 8);

namespace {

// One of: a name that is asked of what the field holds, a layout that is stored in it, another field that was filled with the same value, that the code that allocates the object fills the field,
// the built-in class of a value that is stored in it, or another field that its value was read from.
struct NameAskedOfField {
    FieldReference field;
    PropertyKeyID name;
    LayoutID layoutStored;
    FieldReference fieldWithSameValue;
    bool isFilledByCreator { false };
    BuiltinClassID classStored;
    bool storesOtherKind { false };
    bool isCopyOfField { false };

    bool namesFieldWithSameValue() const { return !name && !layoutStored && !isFilledByCreator && !tellsKindStored(); }
    bool tellsKindStored() const { return classStored || storesOtherKind; }

    friend auto operator<=>(const NameAskedOfField&, const NameAskedOfField&) = default;
};

} // anonymous namespace

static bool tellsNothingOfWhatItHolds(const Program& program, const Field& field)
{
    return field.declared.type == TypeTable::anything && !field.declared.flags.contains(PositionFlags::IsExpected) && field.form.isEmpty() && !field.slotType
        && field.flow.location.deferral == ClosureDeferral::Unknown && !field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden }) && program.at(field.key).kind == PropertyKeyKind::Text;
}

using FieldList = Vector<FieldReference, 2>;

static FieldList findUntoldFields(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    FieldList fields;
    ReceiverCandidates receivers = receiverCandidates(program, function, object);
    const LayoutList& layouts = receivers.isProven() ? receivers.proven : receivers.predicted;
    if (layouts.isEmpty() || layouts.size() > maxReceiversOfFieldRead)
        return fields;
    for (LayoutID layout : layouts) {
        std::optional<FieldReference> field = findField(program, layout, key, BaseChain::Predicted);
        if (!field)
            continue;
        FieldReference declaration = declarationOf(program, *field);
        if (tellsNothingOfWhatItHolds(program, fieldAt(program, declaration)) && !fields.contains(declaration))
            fields.append(declaration);
    }
    return fields;
}

static void collectKindStored(const Program& program, const Function& function, const UncheckedKeyHashMap<uint32_t, FieldList>& fieldsRead, ValueID object, PropertyKeyID key, ValueID stored, Vector<NameAskedOfField>& found)
{
    stored = sameObjectThroughCasts(function, stored);
    // `{ items: other.items }`
    if (auto read = fieldsRead.find(stored.index() + 1); read != fieldsRead.end()) {
        for (FieldReference field : findUntoldFields(program, function, object, key)) {
            for (FieldReference source : read->value) {
                if (source != field)
                    found.append({ source, { }, { }, field, false, { }, false, true });
            }
        }
        return;
    }
    TypeID expected = function.at(stored).expected();
    Type type = (expected ? program.at(expected) : program.typeOf(function, stored)).withoutNullish();
    if (type.anyObject || type.isNever() || type.layouts)
        return;
    BuiltinReceiver receiver = builtinReceiverOf(program, type);
    BuiltinClassID classStored = receiver.kind == BuiltinReceiverKind::Instance && receiver.builtinClass != BuiltinClasses::Function ? receiver.builtinClass : BuiltinClassID();
    for (FieldReference field : findUntoldFields(program, function, object, key))
        found.append({ field, { }, { }, { }, false, classStored, !classStored });
}

static void collectNamesAsked(const Program& program, const Function& function, Vector<NameAskedOfField>& found)
{
    UncheckedKeyHashMap<uint32_t, FieldList> fieldsRead;
    UncheckedKeyHashMap<uint32_t, FieldReference> firstFieldFilledWith;
    UncheckedKeyHashMap<uint32_t, LayoutID> allocatedInBlock;
    for (const Block& block : function.blocks) {
        allocatedInBlock.clear();
        for (ValueID id : block.values) {
            const Value& access = function.at(id);
            if (access.opcode == Opcode::NewObject && access.layout()) {
                allocatedInBlock.add(id.index() + 1, access.layout());
                const Vector<Field>& fields = program.at(declaredLayoutOf(program, access.layout())).fields;
                std::span<const uint32_t> listed = access.fields();
                for (size_t position = 0; position < listed.size(); ++position)
                    collectKindStored(program, function, fieldsRead, id, fields[listed[position]].key, access.operand(OperandIndex::NewObject::values + position), found);
                continue;
            }
            if ((access.opcode == Opcode::PropSet || access.opcode == Opcode::PropDefine) && access.key())
                collectKindStored(program, function, fieldsRead, access.operand(OperandIndex::PropSet::object), access.key(), access.operand(OperandIndex::PropSet::value), found);
            if (access.opcode == Opcode::PropSet && access.key()) {
                // `const context = Object.create(prototype); context.request = request;`
                LayoutID allocated = allocatedInBlock.get(sameObjectThroughCasts(function, access.objectOperand()).index() + 1);
                std::optional<FieldReference> filled = allocated ? findField(program, allocated, access.key(), BaseChain::Proven) : std::nullopt;
                if (filled && fieldAt(program, *filled).flags.contains(FieldFlags::IsAddedLate))
                    found.append({ *filled, { }, { }, { }, true });
                ValueID stored = sameObjectThroughCasts(function, access.operand(OperandIndex::PropSet::value));
                ReceiverCandidates candidates = receiverCandidates(program, function, stored);
                const LayoutList& layoutsStored = candidates.isProven() ? candidates.proven : candidates.predicted;
                bool tellsNothing = program.typeOf(function, stored) == Type::anything() && !function.at(stored).expected();
                bool listsLayouts = !layoutsStored.isEmpty() && layoutsStored.size() <= maxLayoutsExpectedOfField;
                if (!tellsNothing && !listsLayouts)
                    continue;
                for (FieldReference field : findUntoldFields(program, function, access.objectOperand(), access.key())) {
                    if (listsLayouts) {
                        for (LayoutID layout : layoutsStored)
                            found.append({ field, { }, layout, { } });
                        continue;
                    }
                    // `context.res = response.res = res`
                    FieldReference first = firstFieldFilledWith.add(stored.index() + 1, field).iterator->value;
                    if (first != field)
                        found.append({ first, { }, { }, field });
                }
                continue;
            }
            if (access.opcode != Opcode::PropGet || !access.key() || program.typeOf(function, id) != Type::anything() || access.expected())
                continue;
            FieldList fields = findUntoldFields(program, function, access.objectOperand(), access.key());
            if (!fields.isEmpty())
                fieldsRead.add(id.index() + 1, WTF::move(fields));
        }
    }
    if (fieldsRead.isEmpty())
        return;

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& access = function.at(id);
            if (access.opcode != Opcode::PropGet || !access.key())
                continue;
            auto read = fieldsRead.find(sameObjectThroughCasts(function, access.objectOperand()).index() + 1);
            if (read == fieldsRead.end())
                continue;
            for (FieldReference field : read->value)
                found.append({ field, access.key(), { }, { } });
        }
    }
}

// Every instance has a field that its constructor fills. One that is added later is there on some.
static bool everyInstanceHas(const Program& program, LayoutID layout, PropertyKeyID name)
{
    std::optional<FieldReference> field = findField(program, layout, name, BaseChain::Proven);
    return !field || !fieldAt(program, *field).flags.contains(FieldFlags::IsAddedLate);
}

static LayoutList findLayoutsExpectedOf(const Program& program, std::span<const PropertyKeyID> names)
{
    LayoutList layouts = allocatedLayoutsWith(program, names, maxLayoutsComparedForField);
    if (layouts.size() <= maxLayoutsExpectedOfField || layouts.size() > maxLayoutsComparedForField)
        return layouts;
    LayoutList likeliest;
    for (LayoutID layout : layouts) {
        if (std::ranges::all_of(names, [&](PropertyKeyID name) { return everyInstanceHas(program, layout, name); }))
            likeliest.add(layout);
    }
    return likeliest.isEmpty() ? layouts : likeliest;
}

using FieldAndGroup = std::pair<FieldReference, FieldReference>;

// Fields that were filled with one value hold the same thing. Each of them is listed with the first field of its group.
static Vector<FieldAndGroup> groupFieldsWithSameValue(std::span<const NameAskedOfField> asked)
{
    Vector<FieldReference> fields;
    for (const NameAskedOfField& entry : asked) {
        if (entry.namesFieldWithSameValue())
            fields.appendList({ entry.field, entry.fieldWithSameValue });
    }
    AOT::sort(fields);
    removeRepeatedElements(fields);

    Vector<uint32_t> parents(fields.size(), [](size_t index) { return static_cast<uint32_t>(index); });
    auto rootOf = [&](FieldReference field) {
        uint32_t index = static_cast<uint32_t>(std::ranges::lower_bound(fields, field) - fields.begin());
        while (parents[index] != index)
            index = parents[index] = parents[parents[index]];
        return index;
    };
    for (const NameAskedOfField& entry : asked) {
        if (!entry.namesFieldWithSameValue())
            continue;
        auto [first, second] = std::minmax({ rootOf(entry.field), rootOf(entry.fieldWithSameValue) });
        parents[second] = first;
    }
    return Vector<FieldAndGroup>(fields.size(), [&](size_t index) { return FieldAndGroup { fields[index], fields[rootOf(fields[index])] }; });
}

// Fields that are copies of one another hold the same thing. Where arrays are all that the program stores in them, an array is expected.
static void expectClassesStored(Vector<NameAskedOfField>& kindsStored, std::span<const FieldReference> fieldsWithExpectation, NOESCAPE const Invocable<void(FieldReference, BuiltinClassID)> auto& expect)
{
    for (FieldReference field : fieldsWithExpectation)
        kindsStored.append({ field, { }, { }, { }, false, { }, true });
    Vector<FieldAndGroup> groups = groupFieldsWithSameValue(kindsStored.span());
    kindsStored.removeAllMatching([](const NameAskedOfField& entry) { return entry.isCopyOfField; });
    for (NameAskedOfField& entry : kindsStored) {
        auto grouped = std::ranges::lower_bound(groups, entry.field, { }, &FieldAndGroup::first);
        if (grouped != groups.end() && grouped->first == entry.field)
            entry.field = grouped->second;
    }
    AOT::stableSort(groups, { }, &FieldAndGroup::second);
    AOT::sort(kindsStored);
    std::span<const NameAskedOfField> remaining = kindsStored.span();
    while (!remaining.empty()) {
        FieldReference reference = remaining.front().field;
        size_t count = std::ranges::find_if_not(remaining, [&](const NameAskedOfField& entry) { return entry.field == reference; }) - remaining.begin();
        BuiltinClassID classStored = remaining.front().classStored;
        bool storesOneClass = std::ranges::all_of(remaining.first(count), [&](const NameAskedOfField& entry) { return !entry.storesOtherKind && entry.classStored == classStored; });
        remaining = remaining.subspan(count);
        if (!storesOneClass)
            continue;
        auto fieldsOfGroup = std::ranges::equal_range(groups, reference, { }, &FieldAndGroup::second);
        for (auto [field, group] : fieldsOfGroup)
            expect(field, classStored);
        if (fieldsOfGroup.empty())
            expect(reference, classStored);
    }
}

void expectFieldsByNamesAsked(Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<NameAskedOfField> asked = collectInParallel<NameAskedOfField>(loops, program.functions.size(), [&](unsigned index, Vector<NameAskedOfField>& found) {
        if (program.functions[index].hasBody())
            collectNamesAsked(program, program.functions[index], found);
    });
    if (asked.isEmpty())
        return;
    ProgramEditor editor(program);
    for (const NameAskedOfField& entry : asked) {
        if (entry.isFilledByCreator)
            editor.at(entry.field).flags.add(FieldFlags::IsFilledByCreator);
    }
    asked.removeAllMatching([](const NameAskedOfField& entry) { return entry.isFilledByCreator; });
    Vector<NameAskedOfField> kindsStored;
    asked.removeAllMatching([&](const NameAskedOfField& entry) {
        if (entry.layoutStored)
            kindsStored.append({ entry.field, { }, { }, { }, false, { }, true });
        if (!entry.tellsKindStored() && !entry.isCopyOfField)
            return false;
        kindsStored.append(entry);
        return true;
    });
    Vector<FieldAndGroup> groups = groupFieldsWithSameValue(asked.span());
    asked.removeAllMatching([](const NameAskedOfField& entry) { return entry.namesFieldWithSameValue(); });
    for (NameAskedOfField& entry : asked) {
        auto grouped = std::ranges::lower_bound(groups, entry.field, { }, &FieldAndGroup::first);
        if (grouped != groups.end() && grouped->first == entry.field)
            entry.field = grouped->second;
    }
    AOT::stableSort(groups, { }, &FieldAndGroup::second);
    AOT::sort(asked);
    removeRepeatedElements(asked);

    TypeArena types(program.types);
    Vector<FieldReference> changed;
    auto expect = [&](FieldReference reference, TypeID expected) {
        Field& field = editor.at(reference);
        field.declared.type = expected;
        field.declared.flags.add(PositionFlags::IsExpected);
        field.flags.remove(FieldFlags::IsEnforced);
        changed.append(reference);
    };
    std::span<const NameAskedOfField> remaining = asked.span();
    while (!remaining.empty()) {
        FieldReference reference = remaining.front().field;
        size_t count = std::ranges::find_if_not(remaining, [&](const NameAskedOfField& entry) { return entry.field == reference; }) - remaining.begin();
        Vector<PropertyKeyID, 16> names;
        LayoutList layouts;
        for (const NameAskedOfField& entry : remaining.first(count)) {
            if (entry.layoutStored)
                layouts.add(entry.layoutStored);
            else if (!program.layoutsWithName(entry.name).empty())
                names.append(entry.name);
        }
        remaining = remaining.subspan(count);
        // What the program is seen to store in the field says more than the names do.
        if (layouts.isEmpty() && names.size() >= minNamesAskedOfField)
            layouts = findLayoutsExpectedOf(program, names.span());
        if (layouts.isEmpty() || layouts.size() > maxLayoutsExpectedOfField)
            continue;
        Type expected = Type::forLayouts(layouts.span(), types);
        expected.primitives |= Primitive::UndefinedOrNull;
        TypeID expectedID = types.intern(WTF::move(expected));
        auto fieldsOfGroup = std::ranges::equal_range(groups, reference, { }, &FieldAndGroup::second);
        for (auto [field, group] : fieldsOfGroup)
            expect(field, expectedID);
        if (fieldsOfGroup.empty())
            expect(reference, expectedID);
    }
    expectClassesStored(kindsStored, changed.span(), [&](FieldReference field, BuiltinClassID classStored) {
        Type expected = Type::builtinClass(classStored);
        expected.primitives |= Primitive::UndefinedOrNull;
        expect(field, types.intern(WTF::move(expected)));
    });
    AOT::sort(changed);
    if (changed.isEmpty())
        return;

    Vector<FieldReference> copies = collectInParallel<FieldReference>(loops, program.layouts.size(), [&](unsigned layoutIndex, Vector<FieldReference>& found) {
        const Vector<Field>& fields = program.layouts[layoutIndex].fields;
        for (uint32_t index = 0; index < fields.size(); ++index) {
            const Field& field = fields[index];
            if (field.flags.contains(FieldFlags::IsInherited) && field.originLayout && field.originField && std::ranges::binary_search(changed, FieldReference { field.originLayout, *field.originField }))
                found.append({ LayoutID(layoutIndex), index });
        }
    });
    for (FieldReference copy : copies) {
        Field& field = editor.at(copy);
        field.declared = fieldAt(program, { field.originLayout, *field.originField }).declared;
        field.flags.remove(FieldFlags::IsEnforced);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
