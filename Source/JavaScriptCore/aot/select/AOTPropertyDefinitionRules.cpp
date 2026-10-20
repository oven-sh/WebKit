/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTEffectsOfRules.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include <algorithm>
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
    ValueID value;
    ValueID getter;
    ValueID setter;
    OptionSet<PropertyDescriptorFlags> descriptorFlags;
};

struct AccessorMember {
    ValueID definition;
    ValueID function;
    PropertyKeyID key;
    bool isGetter;
};

struct AccessorMatch {
    SelectionGroup group;
    ValueID object;
    Vector<AccessorMember, 4> members;
};

} // anonymous namespace

static constexpr unsigned numberOfDescriptorCallArguments = 3;

static constexpr OptionSet<PropertyDescriptorFlags> dataDescriptorFlags { PropertyDescriptorFlags::HasValue, PropertyDescriptorFlags::HasWritable };
static constexpr OptionSet<PropertyDescriptorFlags> accessorDescriptorFlags { PropertyDescriptorFlags::HasGet, PropertyDescriptorFlags::HasSet };

static constexpr size_t minMembersInAccessorGroup = 2;
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

static bool isDescriptorLiteral(const Program& program, const Value& literal)
{
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand())
        return false;
    return program.at(literal.layout()).kind == LayoutKind::Record && literal.fields().size() == literal.valuesOperands().size();
}

static BuiltinName propertyNameAt(const Program& program, const Value& literal, size_t index)
{
    PropertyKeyID key = program.at(literal.layout()).fields[literal.fields()[index]].key;
    return key ? program.builtinNameOf(key) : BuiltinName();
}

static std::optional<bool> constantBooleanOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constant = constantThroughCasts(program, function, id);
    if (!constant || function.at(*constant).opcode != Opcode::Boolean)
        return std::nullopt;
    return function.at(*constant).boolean();
}

static bool addBooleanAttribute(const Program& program, const Function& function, ValueID value, PropertyDescriptorFlags isPresent, PropertyDescriptorFlags isSet, DescriptorMatch& match)
{
    std::optional<bool> flag = constantBooleanOf(program, function, value);
    if (!flag)
        return false;
    match.descriptorFlags.add(isPresent);
    match.descriptorFlags.set(isSet, *flag);
    return true;
}

static bool addAttribute(const Program& program, const Function& function, BuiltinName name, ValueID value, DescriptorMatch& match)
{
    if (name == BuiltinNames::writable)
        return addBooleanAttribute(program, function, value, PropertyDescriptorFlags::HasWritable, PropertyDescriptorFlags::IsWritable, match);
    if (name == BuiltinNames::enumerable)
        return addBooleanAttribute(program, function, value, PropertyDescriptorFlags::HasEnumerable, PropertyDescriptorFlags::IsEnumerable, match);
    if (name == BuiltinNames::configurable)
        return addBooleanAttribute(program, function, value, PropertyDescriptorFlags::HasConfigurable, PropertyDescriptorFlags::IsConfigurable, match);

    if (name == BuiltinNames::value) {
        match.descriptorFlags.add(PropertyDescriptorFlags::HasValue);
        match.value = value;
    } else if (name == BuiltinNames::get) {
        match.descriptorFlags.add(PropertyDescriptorFlags::HasGet);
        match.getter = value;
    } else if (name == BuiltinNames::set) {
        match.descriptorFlags.add(PropertyDescriptorFlags::HasSet);
        match.setter = value;
    } else
        return false;
    return true;
}

static void setDefinitionOperands(Value& definition, const DescriptorMatch& match)
{
    definition.setOperand(OperandIndex::DefineOwnProperty::object, match.object);
    definition.setOperand(OperandIndex::DefineOwnProperty::computedKey, match.key);
    definition.setOperand(OperandIndex::DefineOwnProperty::value, match.value);
    definition.setOperand(OperandIndex::DefineOwnProperty::getter, match.getter);
    definition.setOperand(OperandIndex::DefineOwnProperty::setter, match.setter);
    definition.setDescriptorFlags(match.descriptorFlags);
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
    if (context.isOnSlowPath(id) || call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;

    std::optional<DescriptorCallOperands> operands = descriptorCallOperandsOf(program, call);
    if (!operands)
        return std::nullopt;
    if (!context.typeOf(operands->object).isObject())
        return std::nullopt;
    if (!context.typeOf(operands->key).hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol))
        return std::nullopt;

    ValueID finishID = operands->descriptor;
    const Value& finish = function.at(finishID);
    if (finish.opcode != Opcode::FinishObject || context.singleUser(finishID) != id)
        return std::nullopt;
    ValueID literalID = finish.objectOperand();
    const Value& literal = function.at(literalID);
    if (context.singleUser(literalID) != finishID || !isDescriptorLiteral(program, literal))
        return std::nullopt;

    DescriptorMatch match;
    match.group = groupOf({ literalID, finishID, id });
    match.object = operands->object;
    match.key = operands->key;
    std::span<const ValueID> values = literal.valuesOperands();
    for (size_t index = 0; index < values.size(); ++index) {
        if (!addAttribute(program, function, propertyNameAt(program, literal, index), values[index], match))
            return std::nullopt;
    }

    if (match.descriptorFlags.containsAny(dataDescriptorFlags) && match.descriptorFlags.containsAny(accessorDescriptorFlags))
        return std::nullopt;
    if (!hasEffectsOfDefinition(context, id, match))
        return std::nullopt;
    return match;
}

void selectDescriptorLiteral(SelectionContext& context)
{
    Vector<DescriptorMatch> matches;
    for (ValueID call : context.candidates(Opcode::CallBuiltin)) {
        std::optional<DescriptorMatch> match = matchDescriptorCall(context, call);
        if (match && context.tryClaim(SelectionRuleID::DescriptorLiteral, match->group) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    context.rewriteGroups(SelectionRuleID::DescriptorLiteral, matches, [](GroupRewriter& rewriter, const DescriptorMatch& match) {
        setDefinitionOperands(rewriter.replaceRoot(Opcode::DefineOwnProperty), match);
    });
}

static std::optional<AccessorMember> accessorMemberOf(SelectionContext& context, ValueID id)
{
    const Value& definition = context.function().at(id);
    if (context.isClaimed(id) || !context.typeOf(definition.objectOperand()).isObject())
        return std::nullopt;

    PropertyKeyID key = definition.key();
    if (!key || !isOrdinaryKey(context.program(), key))
        return std::nullopt;
    return AccessorMember { id, definition.functionOperand(), key, definition.opcode == Opcode::DefineGetter };
}

static void closeAccessorList(SelectionContext& context, ValueID object, std::span<const AccessorMember> list, Vector<AccessorMatch>& matches)
{
    for (; list.size() >= minMembersInAccessorGroup; list = list.subspan(1)) {
        AccessorMatch match;
        match.group.root = list.back().definition;
        match.object = object;
        for (const AccessorMember& member : list) {
            match.group.members.append(GroupMember { member.definition });
            match.members.append(member);
        }
        if (context.tryClaim(SelectionRuleID::AccessorGroup, match.group) == ClaimResult::Claimed) {
            matches.append(WTF::move(match));
            return;
        }
    }
}

static void rewriteAccessorGroup(GroupRewriter& rewriter, const AccessorMatch& match)
{
    RELEASE_ASSERT(match.members.size() <= maxKeysInAccessorMasks);

    Vector<PropertyKeyID, 4> keys;
    Vector<ValueID, 4> functions;
    uint32_t getterMask = 0;
    uint32_t enumerableMask = 0;
    for (uint32_t index = 0; index < match.members.size(); ++index) {
        const AccessorMember& member = match.members[index];
        keys.append(member.key);
        functions.append(member.function);
        if (member.isGetter)
            getterMask |= 1u << index;
        enumerableMask |= 1u << index;
    }

    Value& root = rewriter.replaceRoot(Opcode::DefineAccessors);
    root.setObjectOperand(match.object);
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
        Vector<AccessorMember, 4> list;
        ValueID listObject;
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::DefineGetter && value.opcode != Opcode::DefineSetter)
                continue;

            std::optional<AccessorMember> member = accessorMemberOf(context, id);
            ValueID object = value.objectOperand();
            if (!member || object != listObject || list.size() >= maxMembers) {
                closeAccessorList(context, listObject, list.span(), matches);
                list.shrink(0);
            }
            if (member) {
                listObject = object;
                list.append(*member);
            }
        }
        closeAccessorList(context, listObject, list.span(), matches);
    }

    context.rewriteGroups(SelectionRuleID::AccessorGroup, matches, rewriteAccessorGroup);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
