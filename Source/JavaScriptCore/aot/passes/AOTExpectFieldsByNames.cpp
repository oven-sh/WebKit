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

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(size_t, minNamesAskedOfField, 2);
AOT_THRESHOLD(size_t, maxLayoutsExpectedOfField, 2);
AOT_THRESHOLD(size_t, maxReceiversOfFieldRead, 4);

namespace {

struct NameAskedOfField {
    FieldReference field;
    PropertyKeyID name;

    friend auto operator<=>(const NameAskedOfField&, const NameAskedOfField&) = default;
};

} // anonymous namespace

static bool tellsNothingOfWhatItHolds(const Program& program, const Field& field)
{
    return field.declared.type == TypeTable::anything && !field.declared.flags.contains(PositionFlags::IsExpected) && field.form.isEmpty() && !field.slotType
        && field.flow.location.deferral == ClosureDeferral::Unknown && !field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden }) && program.at(field.key).kind == PropertyKeyKind::Text;
}

static void collectNamesAsked(const Program& program, const Function& function, Vector<NameAskedOfField>& found)
{
    UncheckedKeyHashMap<uint32_t, Vector<FieldReference, 2>> fieldsRead;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& read = function.at(id);
            if (read.opcode != Opcode::PropGet || !read.key() || program.typeOf(function, id) != Type::anything() || read.expected())
                continue;
            ReceiverCandidates receivers = receiverCandidates(program, function, read.objectOperand());
            const LayoutList& layouts = receivers.isProven() ? receivers.proven : receivers.predicted;
            if (layouts.isEmpty() || layouts.size() > maxReceiversOfFieldRead)
                continue;
            Vector<FieldReference, 2> fields;
            for (LayoutID layout : layouts) {
                std::optional<FieldReference> field = findField(program, layout, read.key(), BaseChain::Predicted);
                if (!field)
                    continue;
                FieldReference declaration = declarationOf(program, *field);
                if (tellsNothingOfWhatItHolds(program, fieldAt(program, declaration)) && !fields.contains(declaration))
                    fields.append(declaration);
            }
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
                found.append({ field, access.key() });
        }
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
    AOT::sort(asked);
    removeRepeatedElements(asked);

    ProgramEditor editor(program);
    TypeArena types(program.types);
    Vector<FieldReference> changed;
    std::span<const NameAskedOfField> remaining = asked.span();
    while (!remaining.empty()) {
        FieldReference reference = remaining.front().field;
        size_t count = std::ranges::find_if_not(remaining, [&](const NameAskedOfField& entry) { return entry.field == reference; }) - remaining.begin();
        Vector<PropertyKeyID, 16> names;
        for (const NameAskedOfField& entry : remaining.first(count)) {
            if (!program.layoutsWithName(entry.name).empty())
                names.append(entry.name);
        }
        remaining = remaining.subspan(count);
        if (names.size() < minNamesAskedOfField)
            continue;
        LayoutList layouts = allocatedLayoutsWith(program, names.span(), maxLayoutsExpectedOfField);
        if (layouts.isEmpty() || layouts.size() > maxLayoutsExpectedOfField)
            continue;
        Type expected = Type::forLayouts(layouts.span(), types);
        expected.primitives |= Primitive::UndefinedOrNull;
        Field& field = editor.at(reference);
        field.declared.type = types.intern(WTF::move(expected));
        field.declared.flags.add(PositionFlags::IsExpected);
        field.flags.remove(FieldFlags::IsEnforced);
        changed.append(reference);
    }
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
