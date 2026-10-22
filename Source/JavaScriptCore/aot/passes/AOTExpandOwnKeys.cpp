/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlannedWalk {
    OwnKeyWalkSite site;
    std::optional<OwnKeyWalkPlan> plan;
    std::optional<AssignPlan> assignPlan;
};

struct SpreadValueOnEdge {
    std::optional<Edge> edge;
    bool isLiteral { false };
    Vector<std::pair<PropertyKeyID, ValueID>, 4> properties;
    Vector<ValueID, 2> definitions;
    ValueID other;
};

struct SpreadOfMergedLiterals {
    ProgramPoint point;
    BlockID merge;
    uint32_t parameterIndex { 0 };
    Vector<ValueID, 2> castsOfParameter;
    Vector<SpreadValueOnEdge, 2> edges;
};

} // anonymous namespace

static bool isExpandedByPlan(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
    case OwnKeyWalk::Copy:
        return true;
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<PlannedWalk, 4> planOwnKeyWalks(FunctionPassContext& context)
{
    Vector<OwnKeyWalkSite, 4> sites = findOwnKeyWalkSites(context);
    sites.removeAllMatching([&](const OwnKeyWalkSite& site) {
        if (context.runsAtMostOnce(site.block))
            return true;
        HeatClass heat = heatClassOf(context, site.block);
        // The operation copies a record by its words. What the arms add to that, the layout of the copy and no call, is worth their bytes in hot code.
        return isCold(heat) || (site.walk == OwnKeyWalk::Copy && !isHot(heat));
    });
    Vector<OwnKeyWalkSite, 4> sitesExpandedByPlan;
    for (const OwnKeyWalkSite& site : sites) {
        if (isExpandedByPlan(site.walk))
            sitesExpandedByPlan.append(site);
    }
    UnchangedAllocations unchanged(context, sitesExpandedByPlan.span());

    Vector<PlannedWalk, 4> walks;
    for (const OwnKeyWalkSite& site : sites) {
        if (!isExpandedByPlan(site.walk)) {
            walks.append(PlannedWalk { site, std::nullopt, std::nullopt });
            continue;
        }
        if (site.walk == OwnKeyWalk::Assign) {
            if (std::optional<AssignPlan> plan = planAssign(context, unchanged, site))
                walks.append(PlannedWalk { site, std::nullopt, WTF::move(plan) });
            continue;
        }
        if (std::optional<OwnKeyWalkPlan> plan = planOwnKeyWalk(context, unchanged, site))
            walks.append(PlannedWalk { site, WTF::move(plan), std::nullopt });
    }
    return walks;
}

static constexpr unsigned maxFieldsOfMergedLiterals = 8;

static std::optional<Use> findOnlyUseThroughCasts(FunctionPassContext& context, ValueID value, Vector<ValueID, 2>& valuesOnTheWay)
{
    const UseLists& useLists = context.analyses.useLists();
    for (;;) {
        std::optional<Use> use = useLists.singleUse(value);
        if (!use || use->kind != Use::Kind::Operand)
            return use;
        Opcode user = context.function.at(use->user).opcode;
        if (!isCastWithoutTest(user) && user != Opcode::FinishObject)
            return use;
        valuesOnTheWay.append(use->user);
        value = use->user;
    }
}

static bool collectLiteralMadeForSpread(FunctionPassContext& context, ValueID allocation, ValueID copy, SpreadValueOnEdge& value)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    const Value& literal = function.at(allocation);
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand())
        return false;
    const Layout& layout = context.program.at(literal.layout());
    if (layout.kind != LayoutKind::Record)
        return false;
    std::span<const ValueID> values = literal.valuesOperands();
    for (size_t index = 0; index < values.size(); ++index) {
        const Field& field = layout.fields[literal.fields()[index]];
        if (!field.form.isEmpty())
            return false;
        value.properties.append({ field.key, values[index] });
    }

    BlockID block = useLists.locationOf(allocation).block;
    for (ValueID current = allocation;;) {
        std::optional<Use> next;
        for (const Use& use : useLists.usesOf(current)) {
            bool isDefinition = use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::PropDefine && use.operandIndex == OperandIndex::PropDefine::object;
            if (isDefinition) {
                if (use.block != block)
                    return false;
                value.definitions.append(use.user);
                continue;
            }
            if (next)
                return false;
            next = use;
        }
        if (!next)
            return false;
        if (next->kind == Use::Kind::EdgeArgument) {
            if (!value.edge || next->edge() != *value.edge)
                return false;
            break;
        }
        if (next->kind != Use::Kind::Operand || next->block != block)
            return false;
        if (next->user == copy) {
            if (value.edge || next->operandIndex != OperandIndex::CopyDataProperties::source)
                return false;
            break;
        }
        Opcode user = function.at(next->user).opcode;
        if (!isCastWithoutTest(user) && user != Opcode::FinishObject)
            return false;
        current = next->user;
    }
    std::ranges::sort(value.definitions, { }, [&](ValueID definition) {
        return useLists.locationOf(definition).index;
    });
    for (ValueID definition : value.definitions)
        value.properties.append({ function.at(definition).key(), function.at(definition).valueOperand() });
    value.isLiteral = true;
    return true;
}

static constexpr uint16_t primitivesWithoutProperties = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;

static bool hasNoPropertiesToCopy(FunctionPassContext& context, ValueID value)
{
    return context.program.typeOf(context.function, value).hasOnlyPrimitives(primitivesWithoutProperties);
}

static std::optional<SpreadOfMergedLiterals> findSpreadOfMergedLiterals(FunctionPassContext& context, ProgramPoint point)
{
    const Function& function = context.function;
    const Value& copy = function.at(function.at(point.block).values[point.index]);
    if (copy.opcode != Opcode::CopyDataProperties || !copy.excludedOperands().empty() || !copy.excludedKeys().empty())
        return std::nullopt;

    SpreadOfMergedLiterals spread { .point = point };
    ValueID source = copy.sourceOperand();
    while (isCastWithoutTest(function.at(source).opcode)) {
        spread.castsOfParameter.append(source);
        source = function.at(source).operand(0);
    }
    const UseLists& useLists = context.analyses.useLists();
    ValueLocation location = useLists.locationOf(source);
    if (location.placement == ValuePlacement::InBlock) {
        ValueID allocation = source;
        while (isCastWithoutTest(function.at(allocation).opcode) || function.at(allocation).opcode == Opcode::FinishObject)
            allocation = function.at(allocation).operand(0);
        SpreadValueOnEdge value;
        if (!collectLiteralMadeForSpread(context, allocation, function.at(point.block).values[point.index], value) || value.properties.size() > maxFieldsOfMergedLiterals)
            return std::nullopt;
        spread.castsOfParameter.clear();
        spread.edges.append(WTF::move(value));
        return spread;
    }
    if (location.placement != ValuePlacement::Parameter || function.at(location.block).role != BlockRole::Ordinary)
        return std::nullopt;
    spread.merge = location.block;
    spread.parameterIndex = location.index;
    Vector<ValueID, 2> valuesOnTheWay;
    std::optional<Use> onlyUse = findOnlyUseThroughCasts(context, source, valuesOnTheWay);
    if (!onlyUse || onlyUse->kind != Use::Kind::Operand || onlyUse->user != function.at(point.block).values[point.index] || onlyUse->operandIndex != OperandIndex::CopyDataProperties::source)
        return std::nullopt;

    for (const Edge& edge : context.analyses.edgesFromUnreachableBlocks()) {
        if (targetOf(function, edge).block != spread.merge)
            continue;
        if (edge.isHandlerEdge() || spread.parameterIndex >= targetOf(function, edge).arguments.size())
            return std::nullopt;
        spread.edges.append(SpreadValueOnEdge { .edge = edge });
    }

    unsigned numberOfFields = 0;
    bool hasLiteral = false;
    for (const Edge& edge : context.analyses.cfg().incomingEdges(spread.merge)) {
        if (edge.isHandlerEdge())
            return std::nullopt;
        const Target& target = function.at(edge.from).terminator.targets[edge.targetIndex];
        if (spread.parameterIndex >= target.arguments.size())
            return std::nullopt;
        ValueID argument = target.arguments[spread.parameterIndex];
        SpreadValueOnEdge value { .edge = edge };
        if (hasNoPropertiesToCopy(context, argument)) {
            spread.edges.append(WTF::move(value));
            continue;
        }

        ValueID allocation = argument;
        while (isCastWithoutTest(function.at(allocation).opcode) || function.at(allocation).opcode == Opcode::FinishObject)
            allocation = function.at(allocation).operand(0);
        if (!collectLiteralMadeForSpread(context, allocation, ValueID(), value)) {
            spread.edges.append(SpreadValueOnEdge { .edge = edge, .other = argument });
            continue;
        }
        numberOfFields += value.properties.size();
        hasLiteral = true;
        spread.edges.append(WTF::move(value));
    }
    if (!hasLiteral || numberOfFields > maxFieldsOfMergedLiterals)
        return std::nullopt;
    return spread;
}

static void defineFieldsOfMergedLiterals(FunctionPassContext& context, IRBuilder& builder, const SpreadOfMergedLiterals& spread)
{
    Function& function = context.function;
    ValueID copy = function.at(spread.point.block).values[spread.point.index];
    ValueID target = function.at(copy).targetOperand();
    Origin origin = function.at(copy).origin();
    builder.setOrigin(origin);

    if (!spread.merge) {
        const SpreadValueOnEdge& literal = spread.edges[0];
        for (auto [key, held] : literal.properties) {
            Value definition = IRBuilder::make(Opcode::PropDefine, { target, held });
            definition.setKey(key);
            builder.insertBefore(spread.point, WTF::move(definition));
        }
        becomeUndefined(function, copy);
        for (ValueID definition : literal.definitions)
            becomeUndefined(function, definition);
        builder.applyInsertions();
        return;
    }

    struct Literal {
        int64_t number;
        Vector<PropertyKeyID, 4> keys;
        Vector<ValueID, 4> parameters;
        Vector<TypeID, 4> types;
    };
    Vector<Literal, 2> literals;
    ValueID which = builder.addParameter(spread.merge, TypeTable::int32);
    for (const SpreadValueOnEdge& value : spread.edges) {
        if (value.properties.isEmpty())
            continue;
        Literal literal { static_cast<int64_t>(literals.size() + 1), { }, { }, { } };
        for (auto [key, held] : value.properties) {
            literal.keys.append(key);
            literal.types.append(function.at(held).type);
            literal.parameters.append(builder.addParameter(spread.merge, context.types.unionOf(literal.types.last(), TypeTable::undefined)));
        }
        literals.append(WTF::move(literal));
    }
    const int64_t numberOfOther = literals.size() + 1;
    TypeID typeOfOther = TypeTable::undefined;
    bool hasOther = false;
    for (const SpreadValueOnEdge& value : spread.edges) {
        if (!value.other)
            continue;
        hasOther = true;
        typeOfOther = context.types.unionOf(typeOfOther, function.at(value.other).type);
    }
    ValueID other = hasOther ? builder.addParameter(spread.merge, typeOfOther) : ValueID();

    size_t nextLiteral = 0;
    for (const SpreadValueOnEdge& value : spread.edges) {
        bool hasFields = !value.properties.isEmpty();
        size_t ownLiteral = hasFields ? nextLiteral++ : notFound;
        ProgramPoint end = pointBeforeTerminatorOf(function, value.edge->from);
        Value number = IRBuilder::make(Opcode::Integer);
        number.setInteger(hasFields ? literals[ownLiteral].number : value.other ? numberOfOther : 0);
        builder.appendArgument(*value.edge, builder.insertBefore(end, WTF::move(number)));
        ValueID nothing;
        for (size_t index = 0; index < literals.size(); ++index) {
            for (size_t field = 0; field < literals[index].parameters.size(); ++field) {
                if (index == ownLiteral) {
                    builder.appendArgument(*value.edge, value.properties[field].second);
                    continue;
                }
                if (!nothing)
                    nothing = builder.insertBefore(end, IRBuilder::make(Opcode::Undefined));
                builder.appendArgument(*value.edge, nothing);
            }
        }
        if (!hasOther)
            continue;
        if (!value.other && !nothing)
            nothing = builder.insertBefore(end, IRBuilder::make(Opcode::Undefined));
        builder.appendArgument(*value.edge, value.other ? value.other : nothing);
    }
    builder.applyInsertions();

    IRBuilder::Expansion expansion = builder.beginExpansion(spread.point);
    Vector<std::pair<int64_t, Target>> cases;
    Vector<BlockID, 2> arms;
    BlockFrequency frequency = function.at(builder.currentBlock()).frequency;
    for (const Literal& literal : literals) {
        BlockID arm = builder.addBlock(frequency);
        arms.append(arm);
        cases.append({ literal.number, Target { arm, { }, { } } });
    }
    BlockID copiesOther;
    if (hasOther) {
        copiesOther = builder.addBlock(frequency);
        cases.append({ numberOfOther, Target { copiesOther, { }, { } } });
    }
    BlockID nothingToCopy = builder.addBlock(frequency);
    builder.emitSwitch(which, WTF::move(cases), Target { nothingToCopy, { }, { } });
    for (size_t index = 0; index < literals.size(); ++index) {
        builder.moveToEndOf(arms[index]);
        for (size_t field = 0; field < literals[index].keys.size(); ++field) {
            Value held = IRBuilder::make(Opcode::AssumeType, { literals[index].parameters[field] });
            held.setCastType(literals[index].types[field]);
            Value definition = IRBuilder::make(Opcode::PropDefine, { target, builder.append(WTF::move(held)) });
            definition.setKey(literals[index].keys[field]);
            builder.append(WTF::move(definition));
        }
        builder.endExpansion(expansion, std::span<const ValueID> { });
    }
    if (hasOther) {
        builder.moveToEndOf(copiesOther);
        IRBuilder::BranchBlocks hasNothing = builder.emitBranch(builder.appendTypeTest(other, Type::fromPrimitives(primitivesWithoutProperties)), frequency, frequency);
        builder.endExpansion(expansion, std::span<const ValueID> { });
        builder.moveToEndOf(hasNothing.notTaken);
        builder.append(Opcode::CopyDataProperties, { target, other });
        builder.endExpansion(expansion, std::span<const ValueID> { });
    }
    builder.moveToEndOf(nothingToCopy);
    builder.endExpansion(expansion, std::span<const ValueID> { });
    for (ValueID cast : spread.castsOfParameter)
        becomeUndefined(function, cast);
    for (const SpreadValueOnEdge& value : spread.edges) {
        for (ValueID definition : value.definitions)
            becomeUndefined(function, definition);
    }
}

static bool defineFieldsOfMergedLiterals(FunctionPassContext& context, IRBuilder& builder)
{
    bool changed = false;
    for (;;) {
        std::optional<SpreadOfMergedLiterals> found;
        const Function& function = context.function;
        for (uint32_t block = 0; block < function.blocks.size() && !found; ++block) {
            const BlockValues& values = function.at(BlockID(block)).values;
            for (uint32_t index = 0; index < values.size() && !found; ++index) {
                if (function.at(values[index]).opcode == Opcode::CopyDataProperties)
                    found = findSpreadOfMergedLiterals(context, ProgramPoint { BlockID(block), index });
            }
        }
        if (!found)
            return changed;
        defineFieldsOfMergedLiterals(context, builder, *found);
        if (found->merge) {
            BitVector parameter(context.function.at(found->merge).parameters.size());
            parameter.set(found->parameterIndex);
            builder.removeParameters(found->merge, parameter);
        }
        context.analyses.didChangeValues();
        context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, 1);
        changed = true;
    }
}

PassOutcome expandOwnKeys(FunctionPassContext& context)
{
    const OpcodeSet opcodes = context.analyses.opcodesOfValues();
    bool mayHaveWalks = opcodes.intersects(opcodesOfWalks);
    bool mayHaveForInLoops = opcodes.contains(Opcode::ForInStep);
    bool mayHaveKeyOrderTests = opcodes.intersects(opcodesOfKeyOrderTests) && opcodes.contains(Opcode::NewObject);
    if (!mayHaveWalks && !mayHaveForInLoops && !mayHaveKeyOrderTests)
        return PassOutcome::Unchanged;

    EmissionInputs inputs(context);
    IRBuilder builder(inputs.analysisContext(), context.function, context.types, &context.analyses);
    bool changed = false;

    if (opcodes.contains(Opcode::CopyDataProperties))
        changed |= defineFieldsOfMergedLiterals(context, builder);

    OwnKeyWalkExpander expander(builder, inputs, context);
    // The sites are listed last first: an expansion moves only the values after its point.
    Vector<PlannedWalk, 4> walks;
    if (mayHaveWalks)
        walks = planOwnKeyWalks(context);
    for (const PlannedWalk& walk : walks) {
        uint64_t numberOfWalks = 1;
        if (walk.assignPlan) {
            expander.expandAssign(walk.site, *walk.assignPlan);
            numberOfWalks = walk.assignPlan->walkedSources.size();
        } else if (walk.plan)
            expander.expand(walk.site, *walk.plan);
        else if (!expander.expandStringify(walk.site))
            continue;
        context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, numberOfWalks);
        changed = true;
    }

    if (mayHaveForInLoops)
        changed |= expandForInLoops(context, builder, inputs);
    if (changed || mayHaveKeyOrderTests)
        changed |= foldKeyOrderTests(context);
    return changed ? PassOutcome::Changed : PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
