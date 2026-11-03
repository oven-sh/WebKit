/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTypes.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTLoopEntry.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct KeyPartsMatch {
    SelectionGroup group;
    Opcode lookup;
};

struct LookupInLoop {
    SelectionGroup group;
    InvariantOperand collection;
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
    if (type.isNever() || type.isUndefinedOrNull())
        return false;
    return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Integer | Primitive::UndefinedOrNull) && type.integers.isSubsetOf(Range::int32());
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
    root.setProvenType(Proven::byRulesWithoutChecks(call.type));
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

static bool isLookup(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin)
        return value.opcode == Opcode::MoveTableToHeap;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, value);
    return builtin && lookupOpcodeFor(*builtin);
}

// Where the loop writes to a table otherwise, the code generator leaves its lookups to the stub.
static bool writesToTablesOnlyInLookups(SelectionContext& context, const NaturalLoop& loop)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    auto writesToTables = [](const Effects& effects) { return effects.writes.isTop() || effects.writes.contains(AbstractHeap::CollectionStorage); };
    for (unsigned i = 0; i < loop.size(); ++i) {
        BlockID block = loop.at(i);
        for (ValueID id : function.at(block).values) {
            if (!isLookup(context.program(), function.at(id)) && writesToTables(analyses.effectsOf(id).onAnyEdge()))
                return false;
        }
        if (writesToTables(analyses.effectsOfTerminator(block).onAnyEdge()))
            return false;
    }
    return true;
}

using LoopsAsked = Vector<std::pair<BlockID, bool>, 4>;

static std::optional<LookupInLoop> findLookupInLoop(SelectionContext& context, ValueID id, LoopsAsked& loopsAsked)
{
    const Function& function = context.function();
    const Value& call = function.at(id);
    std::optional<InlineBuiltinSite> site = findInlineBuiltinSite(context, id);
    if (!site || !lookupOpcodeFor(site->builtin) || site->shape != CodeShape::Fast || chooseInlineForm(*site) != InlineForm::Whole)
        return std::nullopt;

    // Only strings are keys of a table in the image.
    ValueID receiver = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!receiver || arguments.size() != 1 || !context.typeOf(arguments[0]).isAnyStringOrNullish() || makersOfStrings.contains(function.at(arguments[0]).opcode) || !hasOnlyMapsAndSets(context.program(), context.typeOf(receiver)))
        return std::nullopt;

    BlockID block = context.analyses().useLists().blockOf(id);
    const NaturalLoop* loop = context.analyses().naturalLoops().innerMostLoopOf(block);
    if (!loop)
        return std::nullopt;
    size_t asked = loopsAsked.findIf([&](const auto& earlier) { return earlier.first == loop->header(); });
    if (asked == notFound) {
        asked = loopsAsked.size();
        loopsAsked.append({ loop->header(), writesToTablesOnlyInLookups(context, *loop) });
    }
    if (!loopsAsked[asked].second)
        return std::nullopt;

    std::optional<InvariantOperand> collection = findOutermostInvariantLoop(context, block, receiver, [&](ValueID candidate) {
        return function.at(candidate).type == function.at(receiver).type;
    });
    if (!collection || !canEmitOnLoopEntry(context, collection->header) || loopEntryGetsNewHandlerEdge(context, collection->header))
        return std::nullopt;
    return LookupInLoop { groupOfOne(id), *collection };
}

void selectMoveLookupTablesToHeap(SelectionContext& context)
{
    if (context.build().recordedOptions().useWideReferences() || !context.analyses().cfg().hasCycle())
        return;

    Vector<LookupInLoop> lookups;
    LoopsAsked loopsAsked;
    for (ValueID id : context.candidates(Opcode::CallBuiltin)) {
        std::optional<LookupInLoop> lookup = findLookupInLoop(context, id, loopsAsked);
        if (lookup && context.tryClaim(SelectionRuleID::MoveLookupTablesToHeap, lookup->group) == ClaimResult::Claimed)
            lookups.append(WTF::move(*lookup));
    }

    struct MovedCollection {
        InvariantOperand collection;
        ValueID moved;
    };
    Vector<MovedCollection, 4> movedCollections;
    context.rewriteGroups(SelectionRuleID::MoveLookupTablesToHeap, lookups, [&](GroupRewriter& rewriter, const LookupInLoop& lookup) {
        size_t index = movedCollections.findIf([&](const MovedCollection& earlier) {
            return earlier.collection.header == lookup.collection.header && earlier.collection.value == lookup.collection.value;
        });
        if (index == notFound) {
            index = movedCollections.size();
            movedCollections.append({ lookup.collection, { } });
            emitOnLoopEntry(context, rewriter.builder(), lookup.collection.header, [&](IRBuilder& atEntry) {
                movedCollections[index].moved = atEntry.append(Opcode::MoveTableToHeap, { lookup.collection.value });
            });
        }
        if (ValueID moved = movedCollections[index].moved) {
            rewriter.function().edit(lookup.group.root).setReceiverOperand(moved);
            context.analyses().didChangeValues();
        }
        rewriter.leaveUnclaimed(lookup.group.root);
    });
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
