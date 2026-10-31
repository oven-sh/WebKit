/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpectFieldsByNames.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
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

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(size_t, minNamesAskedOfField, 2);
AOT_THRESHOLD(size_t, maxLayoutsExpectedOfField, 2);
AOT_THRESHOLD(size_t, maxReceiversOfFieldRead, 4);
AOT_THRESHOLD(size_t, maxLayoutsComparedForField, 8);

namespace {

// One of: a name that is read from the field's value, a layout that is stored in it, another field that was filled with the same value, that the allocating code fills the field, or that an array is stored in it.
struct NameAskedOfField {
    FieldReference field;
    PropertyKeyID name;
    LayoutID layoutStored;
    FieldReference fieldWithSameValue;
    bool isFilledByCreator { false };
    bool isGivenArray { false };

    bool namesFieldWithSameValue() const { return !name && !layoutStored && !isFilledByCreator && !isGivenArray; }

    friend auto operator<=>(const NameAskedOfField&, const NameAskedOfField&) = default;
};

} // anonymous namespace

static bool tellsNothingOfWhatItHolds(const Program& program, const Field& field)
{
    return field.declared.written() == TypeTable::anything && !field.declared.isExpected() && field.form.isEmpty() && !field.slotType
        && field.flow.location.deferral == ClosureDeferral::Unknown && !field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden }) && program.at(field.key).kind == PropertyKeyKind::Text;
}

using FieldReferenceList = Vector<FieldReference, 2>;

static FieldReferenceList findUntoldFields(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    FieldReferenceList fields;
    ReceiverCandidates receivers = receiverCandidates(program, function, object);
    const LayoutList& layouts = receivers.isProven() ? receivers.proven : receivers.expected.predicted();
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

static bool isArray(const Program& program, const Function& function, ValueID value)
{
    auto holdsOnlyArrays = [](const Type& type) {
        Type object = type.withoutPrimitives(Primitive::UndefinedOrNull);
        const ClassPart* part = object.singleBuiltinClass();
        return part && part->builtinClass == BuiltinClasses::Array && !(type.primitives & ~Primitive::UndefinedOrNull);
    };
    TypeID expected = function.at(value).expected().predicted();
    return holdsOnlyArrays(program.typeOf(function, value)) || (expected && holdsOnlyArrays(program.at(expected)));
}

static void collectNamesAsked(const Program& program, const Function& function, Vector<NameAskedOfField>& found)
{
    UncheckedKeyHashMap<uint32_t, FieldReferenceList> fieldsRead;
    UncheckedKeyHashMap<uint32_t, FieldReference> firstFieldFilledWith;
    UncheckedKeyHashMap<uint32_t, LayoutID> allocatedInBlock;
    for (const Block& block : function.blocks) {
        allocatedInBlock.clear();
        for (ValueID id : block.values) {
            const Value& access = function.at(id);
            if (access.opcode == Opcode::NewObject && access.layout()) {
                allocatedInBlock.add(id.index() + 1, access.layout());
                std::span<const ValueID> listed = access.variadicOperands();
                for (size_t index = 0; index < listed.size(); ++index) {
                    FieldReference declaration = declarationOf(program, { access.layout(), access.fields()[index] });
                    if (listed[index] && isArray(program, function, listed[index]) && tellsNothingOfWhatItHolds(program, fieldAt(program, declaration)))
                        found.append({ declaration, { }, { }, { }, false, true });
                }
                continue;
            }
            if ((access.opcode == Opcode::PropSet || access.opcode == Opcode::PropDefine) && access.key() && isArray(program, function, access.valueOperand())) {
                for (FieldReference field : findUntoldFields(program, function, access.objectOperand(), access.key()))
                    found.append({ field, { }, { }, { }, false, true });
                continue;
            }
            if (access.opcode == Opcode::PropSet && access.key()) {
                // `const context = Object.create(prototype); context.request = request;`
                LayoutID allocated = allocatedInBlock.get(sameObjectThroughCasts(function, access.objectOperand()).valueInFrontOfFacts().index() + 1);
                std::optional<FieldReference> filled = allocated ? findField(program, allocated, access.key(), BaseChain::Proven) : std::nullopt;
                if (filled && fieldAt(program, *filled).flags.contains(FieldFlags::IsAddedLate))
                    found.append({ *filled, { }, { }, { }, true });
                ValueID stored = sameObjectThroughCasts(function, access.operand(OperandIndex::PropSet::value)).valueInFrontOfFacts();
                ReceiverCandidates candidates = receiverCandidates(program, function, stored);
                const LayoutList& layoutsStored = candidates.isProven() ? candidates.proven : candidates.expected.predicted();
                bool tellsNothing = program.typeOf(function, stored) == Type::anything() && !function.at(stored).expected().predicted();
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
            if (access.opcode != Opcode::PropGet || !access.key() || program.typeOf(function, id) != Type::anything() || access.expected().predicted())
                continue;
            FieldReferenceList fields = findUntoldFields(program, function, access.objectOperand(), access.key());
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
            auto read = fieldsRead.find(sameObjectThroughCasts(function, access.objectOperand()).valueInFrontOfFacts().index() + 1);
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
    Vector<FieldAndGroup> groups = groupFieldsWithSameValue(asked.span());
    asked.removeAllMatching([](const NameAskedOfField& entry) { return entry.namesFieldWithSameValue(); });
    for (NameAskedOfField& entry : asked) {
        auto grouped = std::ranges::lower_bound(groups, entry.field, { }, &FieldAndGroup::first);
        if (grouped != groups.end() && grouped->first == entry.field)
            entry.field = grouped->second;
    }
    AOT::stableSort(groups, { }, &FieldAndGroup::second);
    AOT::sortInLine(asked);
    removeRepeatedElements(asked);

    TypeArena types(program.types);
    Vector<FieldReference> changed;
    auto expect = [&](FieldReference reference, TypeID expected) {
        Field& field = editor.at(reference);
        field.declared.expect(ExpectedType(expected));
        field.flags.remove(FieldFlags::IsEnforced);
        changed.append(reference);
    };
    std::span<const NameAskedOfField> remaining = asked.span();
    while (!remaining.empty()) {
        FieldReference reference = remaining.front().field;
        size_t count = std::ranges::find_if_not(remaining, [&](const NameAskedOfField& entry) { return entry.field == reference; }) - remaining.begin();
        Vector<PropertyKeyID, 16> names;
        LayoutList layouts;
        bool isGivenArray = false;
        for (const NameAskedOfField& entry : remaining.first(count)) {
            if (entry.isGivenArray)
                isGivenArray = true;
            else if (entry.layoutStored)
                layouts.add(entry.layoutStored);
            else if (!program.layoutsWithName(entry.name).empty())
                names.append(entry.name);
        }
        remaining = remaining.subspan(count);
        // What the program is seen to store in the field says more than the names do.
        if (layouts.isEmpty() && !isGivenArray && names.size() >= minNamesAskedOfField)
            layouts = findLayoutsExpectedOf(program, names.span());
        if (isGivenArray ? !layouts.isEmpty() : layouts.isEmpty() || layouts.size() > maxLayoutsExpectedOfField)
            continue;
        Type expected = isGivenArray ? Type::builtinClass(BuiltinClasses::Array).withoutClassProofs() : Type::forLayouts(layouts.span(), types);
        expected.primitives |= Primitive::UndefinedOrNull;
        TypeID expectedID = types.intern(WTF::move(expected));
        auto fieldsOfGroup = std::ranges::equal_range(groups, reference, { }, &FieldAndGroup::second);
        for (auto [field, group] : fieldsOfGroup)
            expect(field, expectedID);
        if (fieldsOfGroup.empty())
            expect(reference, expectedID);
    }
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

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
