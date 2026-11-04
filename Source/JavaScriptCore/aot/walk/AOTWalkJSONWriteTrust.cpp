/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkJSONWritePlan.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "JSString.h"
#include <algorithm>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

// What JSON.stringify does to all that its operand reaches, by proofs alone: the proven type of the operand, and what Link has seen arrive at each field. Nothing here is tested, so nothing here is guessed.

// A value that reaches more types than this is left to the row. It also bounds how deep objects lie in one another where no type is reached from itself.
static constexpr size_t mostTypesReachedByJSONStringify = 32;

namespace {

struct OwnFieldReachedByJSONStringify {
    PropertyKeyID key;
    TypeID type;
};
using OwnFieldsReachedByJSONStringify = Vector<OwnFieldReachedByJSONStringify, 8>;

} // anonymous namespace

// Every own property of every object of the layout is one of its fields, a plain value in its slot, and neither the object nor anything above it has a toJSON, in every run.
static bool appendOwnFieldsOfLayoutReachedByJSONStringify(const AccessContext& context, LayoutID layout, OwnFieldsReachedByJSONStringify& fields)
{
    JSONObjectPlan looks;
    if (!planLooksAtJSONObject(context, layout, looks) || !looks.looksAtCell.isEmpty() || !looks.classesGuardedAgainstToJSON.isEmpty())
        return false;

    const LinkTables& link = (*context.facts).linked();
    unsigned numberOfFields = context.plan.layout(layout).numberOfFields();
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        PropertyKeyID key = context.plan.slots().keyOf(context.program, { layout, field });
        // In front of a lookup under a symbol that the program made, the run time may call a function of the library.
        if (key && context.program.at(key).kind == PropertyKeyKind::CreatedSymbol)
            return false;
        switch (listingOfJSONField(context, layout, field)) {
        case JSONFieldListing::Unknown:
            return false;
        case JSONFieldListing::LeftOut:
            continue;
        case JSONFieldListing::Listed:
            break;
        }
        const LinkedCell* cell = link.findCellOfField({ layout, field });
        if (!cell || !cell->everyWriterIsKnown || !cell->proven)
            return false;
        fields.append(OwnFieldReachedByJSONStringify { key, provenTypeOfJSONMember(context, layout, field) });
    }
    return true;
}

// False for whatever is no ordinary object of the program. A list is as long as the program makes it and nothing proves what it holds, a function may have got a toJSON, and an instance of a built-in class answers by the rules of that class.
static bool appendOwnFieldsReachedByJSONStringify(const AccessContext& context, TypeID id, OwnFieldsReachedByJSONStringify& fields)
{
    TypeArena& types = context.types;
    Type type = types.at(id);
    if (!type.isJavaScriptValue() || type.mayBeUnspecified() || type.hasUnboundedUnknown())
        return false;
    if (!type.hasObjects())
        return true;
    Type objects = types.at(types.withOnlyObjects(id));
    if (!objects.hasOnlyLayouts() || objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return false;

    // A copy: a list of the thread's arena need not outlive what is asked while it is gone through.
    Vector<LayoutID, 8> layouts;
    layouts.append(membersOf(types, objects));
    return std::ranges::all_of(layouts, [&](LayoutID layout) {
        // A written type names layouts of which no object is ever made. Such a layout stands for no value.
        return !context.plan.isInUse(layout) || appendOwnFieldsOfLayoutReachedByJSONStringify(context, layout, fields);
    });
}

static bool runsNoCodeOnValuesReachedByJSONStringify(const AccessContext& context, TypeID operand)
{
    Vector<TypeID, 8> reached;
    reached.append(operand);
    // The list grows while it is gone through.
    for (size_t index = 0; index < reached.size(); ++index) {
        if (reached.size() > mostTypesReachedByJSONStringify)
            return false;
        // Of the primitives a BigInt alone is asked for a toJSON.
        if (context.types.at(reached[index]).primitives & Primitive::BigInt)
            return false;
        OwnFieldsReachedByJSONStringify fields;
        if (!appendOwnFieldsReachedByJSONStringify(context, reached[index], fields))
            return false;
        for (const OwnFieldReachedByJSONStringify& field : fields) {
            if (!reached.contains(field.type))
                reached.append(field.type);
        }
    }
    return true;
}

namespace {

// How long the text is that JSON.stringify makes of a value, at the most, by the types of all that the value reaches through ordinary objects of the program.
// A type is asked once, however many fields have it. A type that is reached from itself may be that of an object that holds itself.
class TextWrittenByJSONStringify {
public:
    explicit TextWrittenByJSONStringify(const AccessContext& context)
        : m_context(context)
    {
    }

    bool fitsInString(TypeID type) { return longestOf(type).value_or(std::numeric_limits<uint64_t>::max()) <= JSString::MaxLength; }

private:
    struct MeasuredType {
        TypeID type;
        // Nothing: it is being measured, or no bound was found.
        std::optional<uint64_t> longest;
    };

    // A text of the program has a length that the build knows. Any other string may be as long as a string can be, and a BigInt throws.
    static constexpr uint16_t primitivesWithBoundedText = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::LiteralString | Primitive::AnySymbol;
    static constexpr uint64_t charactersOfGap = 10;
    static constexpr uint64_t charactersOfLineStart = "\n"_s.length() + charactersOfGap * mostTypesReachedByJSONStringify;

    static uint64_t quoted(uint64_t characters) { return "\"\""_s.length() + "\\u0000"_s.length() * characters; }

    std::optional<uint64_t> longestOf(TypeID);

    const AccessContext& m_context;
    Vector<MeasuredType, 8> m_measured;
};

std::optional<uint64_t> TextWrittenByJSONStringify::longestOf(TypeID id)
{
    const Program& program = m_context.program;
    Type type = m_context.types.at(id);
    if (!type.isJavaScriptValue() || type.primitives & ~primitivesWithBoundedText)
        return std::nullopt;
    uint64_t longestOfPrimitives = quoted(longestTextOf(m_context.types, type));
    if (!type.hasObjects())
        return longestOfPrimitives;

    if (auto measured = std::ranges::find(m_measured, id, &MeasuredType::type); measured != m_measured.end())
        return measured->longest;
    if (m_measured.size() >= mostTypesReachedByJSONStringify)
        return std::nullopt;
    size_t place = m_measured.size();
    m_measured.append(MeasuredType { id, std::nullopt });

    OwnFieldsReachedByJSONStringify fields;
    if (!appendOwnFieldsReachedByJSONStringify(m_context, id, fields))
        return std::nullopt;

    uint64_t longest = longestOfPrimitives + "{}"_s.length() + charactersOfLineStart;
    for (const OwnFieldReachedByJSONStringify& field : fields) {
        std::optional<uint64_t> value = field.key ? longestOf(field.type) : std::nullopt;
        if (!value)
            return std::nullopt;
        StringID name = program.at(field.key).text;
        longest += charactersOfLineStart + quoted(name ? program.string(name).size() : 0) + ": ,"_s.length() + *value;
        if (longest > JSString::MaxLength)
            return std::nullopt;
    }
    m_measured[place].longest = longest;
    return longest;
}

} // anonymous namespace

CodeRunByJSONStringify codeRunByJSONStringifyOf(const AccessContext& context, TypeID operand)
{
    CodeRunByJSONStringify code;
    if (!operand)
        return code;
    code.runsNoCode = runsNoCodeOnValuesReachedByJSONStringify(context, operand);
    code.cannotThrow = code.runsNoCode && TextWrittenByJSONStringify(context).fitsInString(operand);
    return code;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
