/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTDescriptorLiteral.h"
#include "AOTEffectsOfRules.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DescriptorCallOperands {
    ValueID object;
    ValueID key;
    ValueID descriptor;
};

struct DescriptorMatch {
    SelectionGroup group;
    ValueID object;
    ValueID key;
    DescriptorLiteral descriptor;
};

// A descriptor that reaches its call as a parameter of the block: on every way in it is a literal of one layout, made for this call alone.
struct DescriptorsMeetingInParameter {
    ValueID call;
    uint32_t operandIndex { 0 };
    ValueID parameter;
    uint32_t parameterIndex { 0 };
    // One for each edge into the block, in the order of the edges.
    Vector<ValueID, 2> finishes;
    Vector<ValueID, 2> literals;
    // The one member that differs from way to way.
    std::optional<size_t> differingMember;
};

struct AccessorMatch {
    SelectionGroup group;
};

} // anonymous namespace

static constexpr unsigned numberOfDescriptorCallArguments = 3;

AOT_THRESHOLD(size_t, minMembersInAccessorGroup, 2);
static constexpr uint64_t maxKeysInAccessorMasks = std::numeric_limits<uint32_t>::digits;

static std::optional<unsigned> descriptorCallArgumentOf(const std::optional<BuiltinOperand>& operand)
{
    if (!operand || operand->kind() != BuiltinOperand::Kind::Argument || operand->argumentIndex() >= numberOfDescriptorCallArguments)
        return std::nullopt;
    return operand->argumentIndex();
}

static bool returnsOperand(const BuiltinTable& builtins, const BuiltinOverload& overload, BuiltinOperand operand)
{
    if (!overload.result.type)
        return false;
    const BuiltinType& result = builtins.at(overload.result.type);
    if (!result.flags.isEmpty() || !result.primitives.isEmpty() || !result.stringLiterals.empty() || result.parts.size() != 1)
        return false;
    const BuiltinTypePart& part = result.parts.front();
    return part.kind == BuiltinTypePartKind::Operand && part.operand == operand;
}

static std::optional<DescriptorCallOperands> descriptorCallOperandsOf(const Program& program, const Value& call)
{
    const BuiltinOverload& overload = overloadOf(program, call);
    std::span<const BuiltinPropertyAction> actions = overload.flow.propertyActions;
    if (actions.size() != 1)
        return std::nullopt;

    const BuiltinPropertyAction& action = actions.front();
    if (action.kind != BuiltinPropertyActionKind::TakesDescriptor || action.key.kind != BuiltinKeyKind::Operand || action.value.kind != BuiltinPlaceKind::Operand)
        return std::nullopt;

    std::optional<unsigned> object = descriptorCallArgumentOf(action.object);
    std::optional<unsigned> key = descriptorCallArgumentOf(action.key.operand);
    std::optional<unsigned> descriptor = descriptorCallArgumentOf(action.value.operand);
    if (!object || !key || !descriptor || *object == *key || *object == *descriptor || *key == *descriptor)
        return std::nullopt;

    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() != numberOfDescriptorCallArguments || !returnsOperand(program.builtins(), overload, action.object))
        return std::nullopt;
    return DescriptorCallOperands { arguments[*object], arguments[*key], arguments[*descriptor] };
}

static ValueID findClosureMadeOnDemand(const Function& function, ValueID value)
{
    if (!value)
        return { };
    ValueID definition = definitionThroughUpcasts(function, value);
    const Value& closure = function.at(definition);
    return closure.opcode == Opcode::NewClosure && closure.isMadeOnDemand() ? definition : ValueID();
}

static bool canCreateClosuresAtDefinition(SelectionContext& context, const DescriptorMatch& match)
{
    const Program& program = context.program();
    const Function& function = context.function();
    const DescriptorLiteral& descriptor = match.descriptor;
    return std::ranges::all_of(std::array { descriptor.value, descriptor.getter, descriptor.setter }, [&](ValueID value) {
        ValueID closure = findClosureMadeOnDemand(function, value);
        if (!closure)
            return true;
        const Function& callee = program.at(function.at(closure).function());
        if (callee.captures.isEmpty())
            return true;
        if (!descriptor.closureEnvironment || !program.at(callee.captures[0].written()).environment)
            return false;
        FunctionAnalyses& analyses = context.analyses();
        return analyses.dominates(descriptor.closureEnvironment, analyses.useLists().pointOf(closure));
    });
}

static void createClosuresAtDefinition(const Program& program, Function& function, const DescriptorMatch& match)
{
    const DescriptorLiteral& descriptor = match.descriptor;
    for (ValueID value : { descriptor.value, descriptor.getter, descriptor.setter }) {
        ValueID id = findClosureMadeOnDemand(function, value);
        if (!id)
            continue;
        Value& closure = function.edit(id);
        closure.setIsMadeOnDemand(false);
        if (!program.at(closure.function()).captures.isEmpty())
            closure.setCapturesOperands(singleElementSpan(descriptor.closureEnvironment));
    }
}

static void setDefinitionOperands(Value& definition, const DescriptorMatch& match)
{
    definition.setOperand(OperandIndex::DefineOwnProperty::object, match.object);
    definition.setOperand(OperandIndex::DefineOwnProperty::computedKey, match.key);
    definition.setOperand(OperandIndex::DefineOwnProperty::value, match.descriptor.value);
    definition.setOperand(OperandIndex::DefineOwnProperty::getter, match.descriptor.getter);
    definition.setOperand(OperandIndex::DefineOwnProperty::setter, match.descriptor.setter);
    definition.setDescriptorFlags(match.descriptor.flags);
}

static bool hasEffectsOfDefinition(SelectionContext& context, ValueID call, const DescriptorMatch& match)
{
    FunctionAnalyses& analyses = context.analyses();
    Value definition = IRBuilder::make(Opcode::DefineOwnProperty);
    setDefinitionOperands(definition, match);
    return effectsOf(analyses.effectContext(), context.function(), definition).isSubsetOf(analyses.effectsOf(call));
}

static std::optional<DescriptorMatch> matchDescriptorCall(SelectionContext& context, ValueID id)
{
    const Program& program = context.program();
    const Function& function = context.function();
    const Value& call = function.at(id);
    if (context.isOnSlowPath(id) || (call.builtinCallForm() != BuiltinCallForm::Whole && call.builtinCallForm() != BuiltinCallForm::AnyReceiver))
        return std::nullopt;

    std::optional<DescriptorCallOperands> operands = descriptorCallOperandsOf(program, call);
    if (!operands || !context.typeOf(operands->key).hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol))
        return std::nullopt;
    // The operation hands what is no object to the built-in, which refuses it. Its result is its operand, which says less than the result of the call where the operand may be something else.
    const Type& typeOfObject = context.typeOf(operands->object);
    if (!typeOfObject.hasObjects() || !typeOfObject.isJavaScriptValue() || (!typeOfObject.isObject() && context.analyses().useLists().useCount(id)))
        return std::nullopt;

    ValueID finishID = operands->descriptor;
    const Value& finish = function.at(finishID);
    if (finish.opcode != Opcode::FinishObject || context.singleUser(finishID) != id)
        return std::nullopt;
    ValueID literalID = finish.objectOperand();
    if (context.singleUser(literalID) != finishID)
        return std::nullopt;
    std::optional<DescriptorLiteral> descriptor = readDescriptorLiteral(program, function, function.at(literalID));
    if (!descriptor)
        return std::nullopt;

    DescriptorMatch match { .group = groupOf({ literalID, finishID, id }), .object = operands->object, .key = operands->key, .descriptor = *descriptor };
    if (!canCreateClosuresAtDefinition(context, match) || !hasEffectsOfDefinition(context, id, match))
        return std::nullopt;
    return match;
}

static bool areSameMember(const Program& program, const Function& function, ValueID a, ValueID b)
{
    if (a == b)
        return true;
    std::optional<bool> flagOfA = constantBooleanOf(program, function, a);
    return flagOfA && flagOfA == constantBooleanOf(program, function, b);
}

static std::optional<DescriptorsMeetingInParameter> findDescriptorsMeetingInParameter(FunctionPassContext& pass, ValueID id)
{
    const Program& program = pass.program;
    const Function& function = pass.function;
    const Value& call = function.at(id);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    std::optional<DescriptorCallOperands> operands = descriptorCallOperandsOf(program, call);
    if (!operands || function.at(operands->descriptor).opcode != Opcode::Parameter)
        return std::nullopt;

    const UseLists& uses = pass.analyses.useLists();
    DescriptorsMeetingInParameter found { .call = id, .parameter = operands->descriptor };
    ValueLocation location = uses.locationOf(found.parameter);
    std::optional<Use> use = uses.singleUse(found.parameter);
    if (location.placement != ValuePlacement::Parameter || location.block != uses.blockOf(id) || location.block == entryBlock || !use || use->kind != Use::Kind::Operand || function.at(location.block).isHandlerBlock())
        return std::nullopt;
    found.operandIndex = use->operandIndex;
    found.parameterIndex = location.index;

    std::span<const Edge> edges = pass.analyses.cfg().incomingEdges(location.block);
    if (edges.size() < 2)
        return std::nullopt;
    for (Edge edge : edges) {
        const Target& target = targetOf(function, edge);
        if (edge.isHandlerEdge() || function.at(edge.from).terminator.kind != TerminatorKind::Jump || found.parameterIndex >= target.arguments.size())
            return std::nullopt;
        ValueID finishID = target.arguments[found.parameterIndex];
        const Value& finish = function.at(finishID);
        if (finish.opcode != Opcode::FinishObject || uses.useCount(finishID) != 1)
            return std::nullopt;
        ValueID literalID = finish.objectOperand();
        const Value& literal = function.at(literalID);
        if (uses.useCount(literalID) != 1 || !listsMembersOfDescriptor(program, literal) || uses.blockOf(finishID) == location.block || uses.blockOf(literalID) == location.block)
            return std::nullopt;
        if (!found.literals.isEmpty()) {
            const Value& first = function.at(found.literals[0]);
            if (!literal.hasSamePayloadAs(first) || literal.valuesOperands().size() != first.valuesOperands().size())
                return std::nullopt;
            for (size_t index = 0; index < literal.valuesOperands().size(); ++index) {
                if (areSameMember(program, function, literal.valuesOperands()[index], first.valuesOperands()[index]))
                    continue;
                if (found.differingMember && *found.differingMember != index)
                    return std::nullopt;
                found.differingMember = index;
            }
        }
        found.finishes.append(finishID);
        found.literals.append(literalID);
    }
    if (!found.differingMember)
        return std::nullopt;
    // A closure that is made on demand is made where the literal stands.
    for (ValueID literal : found.literals) {
        ValueID member = function.at(literal).valuesOperands()[*found.differingMember];
        if (findClosureMadeOnDemand(function, member))
            return std::nullopt;
    }
    return found;
}

PassOutcome makeDescriptorLiteralsBehindJoins(FunctionPassContext& pass)
{
    Function& function = pass.function;
    if (!pass.analyses.opcodesOfValues().contains(Opcode::CallBuiltin))
        return PassOutcome::Unchanged;

    Vector<DescriptorsMeetingInParameter, 1> all;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        if (function.at(block).parameters.isEmpty())
            continue;
        for (ValueID id : function.at(block).values) {
            if (std::optional<DescriptorsMeetingInParameter> found = findDescriptorsMeetingInParameter(pass, id))
                all.append(WTF::move(*found));
        }
    }
    if (all.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), function, pass.types, &pass.analyses);
    for (const DescriptorsMeetingInParameter& found : all) {
        const UseLists& uses = pass.analyses.useLists();
        ProgramPoint point = uses.pointOf(found.call);
        size_t differing = *found.differingMember;
        Vector<Edge, 2> edges(pass.analyses.cfg().incomingEdges(point.block));
        Vector<ProgramPoint, 4> dead;
        TypeID type;
        for (size_t index = 0; index < edges.size(); ++index) {
            ValueID member = function.at(found.literals[index]).valuesOperands()[differing];
            type = type ? pass.types.unionOf(type, function.at(member).type) : function.at(member).type;
            dead.append(uses.pointOf(found.finishes[index]));
            dead.append(uses.pointOf(found.literals[index]));
        }

        Value literal = function.at(found.literals[0]);
        Vector<ValueID, 4> members(literal.valuesOperands());
        builder.setOrigin(literal.origin());
        for (size_t index = 0; index < members.size(); ++index) {
            if (index == differing) {
                members[index] = found.parameter;
                continue;
            }
            if (std::optional<bool> flag = constantBooleanOf(pass.program, function, members[index])) {
                Value constant = IRBuilder::make(Opcode::Boolean);
                constant.setBoolean(*flag);
                members[index] = builder.insertBefore(point, WTF::move(constant));
            }
        }
        for (size_t index = 0; index < edges.size(); ++index)
            editTarget(function, edges[index]).arguments[found.parameterIndex] = function.at(found.literals[index]).valuesOperands()[differing];
        function.edit(found.parameter, ValueEdit::Type).type = type;
        literal.setValuesOperands(members.span());
        ValueID made = builder.insertBefore(point, WTF::move(literal));
        ValueID finished = builder.insertBefore(point, IRBuilder::make(Opcode::FinishObject, { made }));
        function.edit(found.call, ValueEdit::Definition).setOperand(found.operandIndex, finished);
        builder.applyInsertions();

        AOT::sort(dead, [](ProgramPoint a, ProgramPoint b) {
            return std::pair { a.block, a.index } < std::pair { b.block, b.index };
        });
        for (size_t first = 0; first < dead.size();) {
            BitVector indexes;
            size_t next = first;
            for (; next < dead.size() && dead[next].block == dead[first].block; ++next)
                indexes.set(dead[next].index);
            builder.eraseValues(dead[first].block, indexes);
            first = next;
        }
        pass.analyses.didChangeControlFlow();
    }
    return PassOutcome::Changed;
}

void selectDescriptorLiteral(SelectionContext& context)
{
    Vector<DescriptorMatch> matches;
    for (ValueID call : context.candidates(Opcode::CallBuiltin)) {
        std::optional<DescriptorMatch> match = matchDescriptorCall(context, call);
        if (match && context.tryClaim(SelectionRuleID::DescriptorLiteral, match->group) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    context.rewriteGroups(SelectionRuleID::DescriptorLiteral, matches, [&](GroupRewriter& rewriter, const DescriptorMatch& match) {
        createClosuresAtDefinition(context.program(), rewriter.function(), match);
        setDefinitionOperands(rewriter.replaceRoot(Opcode::DefineOwnProperty), match);
    });
}

static bool canJoinAccessorGroup(SelectionContext& context, ValueID id)
{
    const Value& definition = context.function().at(id);
    if (context.isClaimed(id) || !context.typeOf(definition.objectOperand()).isObject())
        return false;
    return definition.key() && isOrdinaryKey(context.program(), definition.key());
}

static bool tryClaimAccessorGroup(SelectionContext& context, std::span<const ValueID> list, Vector<AccessorMatch>& matches)
{
    AccessorMatch match;
    match.group.root = list.back();
    for (ValueID definition : list)
        match.group.members.append(GroupMember { definition });
    if (context.tryClaim(SelectionRuleID::AccessorGroup, match.group) != ClaimResult::Claimed)
        return false;
    matches.append(WTF::move(match));
    return true;
}

// Another member of the literal between two definitions keeps them apart. The list falls into the longest groups that end at its last definition, at the last one in front of that group, and so on.
static void closeAccessorList(SelectionContext& context, std::span<const ValueID> list, Vector<AccessorMatch>& matches)
{
    while (list.size() >= minMembersInAccessorGroup) {
        size_t first = 0;
        size_t lastStart = list.size() - minMembersInAccessorGroup;
        while (first <= lastStart && !tryClaimAccessorGroup(context, list.subspan(first), matches))
            ++first;
        list = list.first(first <= lastStart ? first : list.size() - 1);
    }
}

static void rewriteAccessorGroup(GroupRewriter& rewriter, const AccessorMatch& match)
{
    std::span<const GroupMember> members = match.group.members.span();
    RELEASE_ASSERT(members.size() <= maxKeysInAccessorMasks);

    Vector<PropertyKeyID, 4> keys;
    Vector<ValueID, 4> functions;
    uint32_t getterMask = 0;
    uint32_t enumerableMask = 0;
    for (uint32_t index = 0; index < members.size(); ++index) {
        const Value& definition = rewriter.original(members[index].value);
        keys.append(definition.key());
        functions.append(definition.functionOperand());
        if (definition.opcode == Opcode::DefineGetter)
            getterMask |= 1u << index;
        enumerableMask |= 1u << index;
    }

    ValueID object = rewriter.original(match.group.root).objectOperand();
    Value& root = rewriter.replaceRoot(Opcode::DefineAccessors);
    root.setObjectOperand(object);
    root.setKeys(keys.span());
    root.setFunctionsOperands(functions.span());
    root.setGetterMask(getterMask);
    root.setEnumerableMask(enumerableMask);
}

void selectAccessorGroup(SelectionContext& context)
{
    if (context.candidates(Opcode::DefineGetter).size() + context.candidates(Opcode::DefineSetter).size() < minMembersInAccessorGroup)
        return;

    const Function& function = context.function();
    uint64_t maxMembers = std::min(context.pass().limit(LimitID::MaxSelectionGroupMembers), maxKeysInAccessorMasks);
    Vector<AccessorMatch> matches;
    for (BlockID block : context.analyses().cfg().reversePostOrder()) {
        Vector<ValueID, 4> list;
        ValueID listObject;
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::DefineGetter && value.opcode != Opcode::DefineSetter)
                continue;

            bool canJoin = canJoinAccessorGroup(context, id);
            if (!canJoin || value.objectOperand() != listObject || list.size() >= maxMembers) {
                closeAccessorList(context, list.span(), matches);
                list.shrink(0);
            }
            if (canJoin) {
                listObject = value.objectOperand();
                list.append(id);
            }
        }
        closeAccessorList(context, list.span(), matches);
    }

    context.rewriteGroups(SelectionRuleID::AccessorGroup, matches, rewriteAccessorGroup);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
