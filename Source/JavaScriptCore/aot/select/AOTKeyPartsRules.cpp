/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTInlineBuiltinForms.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct KeyPartsMatch {
    SelectionGroup group;
    Opcode lookup;
};

} // anonymous namespace

static std::optional<Opcode> lookupOpcodeFor(InlineBuiltin builtin)
{
    switch (builtin) {
    case InlineBuiltin::MapGet:
        return Opcode::MapGetByKeyParts;
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
        return Opcode::HasByKeyParts;
    default:
        return std::nullopt;
    }
}

static bool isKeyPart(const Type& type)
{
    return type.isAnyString() || type.isInt32();
}

static bool isOwnedByReturnCall(SelectionContext& context, ValueID id)
{
    BlockID block = context.analyses().useLists().blockOf(id);
    if (!block)
        return false;
    const Terminator& terminator = context.function().at(block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id;
}

static std::optional<KeyPartsMatch> findKeyPartsLookup(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    const Value& call = function.at(id);
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;

    std::optional<InlineBuiltinSite> site = findInlineBuiltinSite(context, id);
    std::optional<Opcode> lookup = site ? lookupOpcodeFor(site->builtin) : std::nullopt;
    if (!lookup || chooseInlineForm(*site) != InlineForm::Whole)
        return std::nullopt;

    ValueID collection = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!collection || !context.typeOf(collection).isObject() || arguments.size() != 1)
        return std::nullopt;

    ValueID key = arguments[0];
    const Value& concatenation = function.at(key);
    if (concatenation.opcode != Opcode::Strcat || context.singleUser(key) != id || isOwnedByReturnCall(context, id))
        return std::nullopt;

    std::span<const ValueID> parts = concatenation.partsOperands();
    bool hasOnlyKeyParts = std::ranges::all_of(parts, [&](ValueID part) {
        return isKeyPart(context.typeOf(part));
    });
    if (!hasOnlyKeyParts || context.pass().exceeds(LimitID::MaxKeyParts, parts.size(), id))
        return std::nullopt;
    return KeyPartsMatch { groupOf({ key, id }), *lookup };
}

static void setLookupOperands(Value& lookup, ValueID collection, std::span<const ValueID> parts)
{
    static_assert(OperandIndex::MapGetByKeyParts::map == OperandIndex::HasByKeyParts::collection);
    lookup.setOperand(OperandIndex::HasByKeyParts::collection, collection);
    lookup.setPartsOperands(parts);
}

static void rewriteKeyPartsLookup(GroupRewriter& rewriter, const KeyPartsMatch& match)
{
    const Value& concatenation = rewriter.original(match.group.members.first().value);
    const Value& call = rewriter.original(match.group.root);
    ValueID collection = call.receiverOperand();
    std::span<const ValueID> parts = concatenation.partsOperands();

    bool hasProvenValueType = match.lookup == Opcode::MapGetByKeyParts && call.type != TypeTable::anything;
    if (!hasProvenValueType) {
        Value& root = rewriter.replaceRoot(match.lookup);
        setLookupOperands(root, collection, parts);
        root.setOrigin(concatenation.origin());
        return;
    }

    Value lookup = IRBuilder::make(match.lookup);
    setLookupOperands(lookup, collection, parts);
    rewriter.builder().setOrigin(concatenation.origin());
    ValueID lookupID = rewriter.insert(WTF::move(lookup));

    Value& root = rewriter.replaceRoot(Opcode::AssumeType);
    root.setValueOperand(lookupID);
    root.setCastType(call.type);
}

void selectKeyPartsLookup(SelectionContext& context)
{
    Vector<KeyPartsMatch> matches;
    for (ValueID id : context.candidates(Opcode::CallBuiltin)) {
        std::optional<KeyPartsMatch> match = findKeyPartsLookup(context, id);
        if (match && context.tryClaim(SelectionRuleID::KeyPartsLookup, match->group) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    context.rewriteGroups(SelectionRuleID::KeyPartsLookup, matches, rewriteKeyPartsLookup);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
