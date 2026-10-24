/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParserBuilder.h"

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowResults.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTValidatorBuilder.h"
#include "AOTVector.h"
#include <algorithm>
#include <cmath>

namespace JSC { namespace AOT { namespace Typed {

static Type typeOfLiteral(const ResultLiteral& literal)
{
    switch (literal.kind) {
    case ResultLiteral::Kind::Null:
        return Type::fromPrimitives(Primitive::Null);
    case ResultLiteral::Kind::Boolean:
        return Type::fromPrimitives(literal.boolean ? Primitive::True : Primitive::False);
    case ResultLiteral::Kind::Number:
        return Type::number(literal.number);
    case ResultLiteral::Kind::String:
        return Type::stringLiteral(literal.string);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool membersAreNamedFields(const Program& program, const ResultNode& node)
{
    return std::ranges::all_of(node.members, [&](const ResultMember& member) {
        return member.field && program.at(member.key).kind == PropertyKeyKind::Text && !program.isIndexLike(member.key);
    });
}

static ResultNodeRef nodeRefOf(const Function& parser, const ResultNode& node)
{
    const auto& creator = parser.creator.as<CreatorRecord::Parser>();
    bool isValidated = !!creator.expectedResult;
    if (isValidated)
        return ResultNodeRef::forValidation(creator.expectedResult, creator.node);
    return ResultNodeRef::forType(node.type);
}

static StringView plainTextOf(const EmissionInputs& inputs, StringID string)
{
    return byteCast<Latin1Character>(inputs.strings.at(string));
}

// Flow fills the fields of a typed result with the arrays its parsers create, and those of an unvalidated result with the plain data of the generic parser too.
static Emission::ProofsByOrigin proofsByOriginOf(Emission& emission, const ResultMember& member)
{
    if (!member.node.isValidated())
        return Emission::ProofsByOrigin::AboutEveryClass;
    const EmissionInputs& inputs = emission.inputs();
    bool createsArrays = createsEveryArrayItself({ inputs.program, inputs.flow, inputs.build }, member.node, NodeUse::ParsedFromText);
    return createsArrays ? Emission::ProofsByOrigin::AboutArrays : Emission::ProofsByOrigin::None;
}

// The slot of a field of a function type holds fewer functions than the type admits.
static TypeID typeStoredInSlotOf(const Program& program, const Field& field)
{
    if (field.slotType && program.at(field.slotType).isSubsetOf(program.at(field.declared.type), program))
        return field.slotType;
    return field.declared.type;
}

static TypeID testedFieldTypeOf(Emission& emission, const ResultMember& member, const Field& field)
{
    return emission.fieldTypeToTest(typeStoredInSlotOf(emission.program(), field), proofsByOriginOf(emission, member));
}

static bool canFillField(Emission& emission, TypeID parsedType, const ResultMember& member, const Field& field)
{
    return !emission.passesOnlyUndefinedIntoStoredType(emission.program().at(parsedType), testedFieldTypeOf(emission, member, field));
}

static ValueID emitTestFieldType(Emission& emission, ValueID value, const ResultMember& member, const Field& field, BlockID otherwise)
{
    return emission.emitRequireStoredType(value, testedFieldTypeOf(emission, member, field), otherwise);
}

static ValueID emitRequireTypeOfMember(Emission& emission, ValueID value, const ResultMember& member, const Field& field, BlockID otherwise)
{
    return emission.emitRequireFieldType(value, typeStoredInSlotOf(emission.program(), field), proofsByOriginOf(emission, member), otherwise);
}

static ValueID appendObjectKey(IRBuilder& builder, ValueID order, LayoutID layout, uint32_t field)
{
    Value key = IRBuilder::make(Opcode::JSONObjectKey, { order });
    key.setLayout(layout);
    key.setField(field);
    return builder.append(WTF::move(key));
}

ParsedValue JSONParserBuilder::emitObject(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    emitRequireByte(cursor, '{', exits.rejected);
    bool isModeled = allocatesLayout(decisionInputs(), node);
    bool hasNamedFields = membersAreNamedFields(program(), node);
    if (node.layout && !isModeled)
        addInterpretedNodeDecline(decisionInputs(), node.layout, InterpretedCause::LayoutIsNotModeled);
    else if (isModeled && !hasNamedFields)
        addInterpretedNodeDecline(decisionInputs(), node.layout, InterpretedCause::MemberIsNotNamedField);
    bool canAllocateLayout = isModeled && hasNamedFields
        && !inputs().build.exceeds(LimitID::MaxParsedObjectMembers, node.members.size(), Subject { .layout = node.layout });
    if (!canAllocateLayout)
        return emitGenericValueFor(nodeRefOf(builder().function(), node), node, cursor, exits);

    ValueID firstKey = appendSkipPunctuator(cursor);
    ObjectParse parse { node, resolveMembers(node), builder().addBlock(), builder().addBlock(), exits };
    bindState(parse, parse.anyOrder);
    bindState(parse, parse.closing);

    ObjectState state = emitOrderedMembers(parse, createInitialState(parse, firstKey));
    emitJumpWithState(parse.anyOrder, state);
    emitUnorderedMembers(parse);
    return emitObjectClosing(parse);
}

ParsedValue JSONParserBuilder::emitDictionary(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    emitRequireByte(cursor, '{', exits.rejected);
    return emitGenericValueFor(nodeRefOf(builder().function(), node), node, cursor, exits);
}

auto JSONParserBuilder::resolveMembers(const ResultNode& node) -> Vector<MemberParse, 8>
{
    Vector<MemberParse, 8> members;
    members.reserveInitialCapacity(node.members.size());
    for (const ResultMember& member : node.members) {
        ResultNode memberNode = describeNode(program(), member.node);
        MemberParse parse;
        parse.member = &member;
        parse.name = program().at(member.key).text;
        if (std::optional<Vector<uint8_t>> quoted = quotedIfPlain(program().string(parse.name))) {
            parse.hasPlainKey = true;
            parse.quotedKey = inputs().strings.intern(quoted->span());
            quoted->append(':');
            parse.quotedKeyAndColon = inputs().strings.intern(quoted->span());
        }
        parse.valueType = inputs().types.intern(program().at(memberNode.type).withoutClassProofs().withUntestableBuiltinsWidened().withPrimitives(Primitive::Undefined));
        parse.missing = classifyMissingMember(member, memberNode);

        const Type& fieldType = program().at(fieldOf(program(), node, member).declared.type);
        switch (parse.missing) {
        case MissingMember::Absent:
            if (!(fieldType.primitives & Primitive::Undefined))
                parse.missing = MissingMember::Unknown;
            break;
        case MissingMember::Default:
            if (!typeOfLiteral(*memberNode.defaultValue).isSubsetOf(fieldType, program()))
                parse.missing = MissingMember::Unknown;
            break;
        case MissingMember::Rejected:
        case MissingMember::Unknown:
            break;
        }
        members.append(parse);
    }

    if (!node.layout)
        return members;
    // A text is most likely written from an object that the program's own code has made, or else by somebody who read the declaration of the type.
    ExpectedKeyOrders expected = expectedKeyOrdersOf(program(), node.layout, KeyOrderLimits { 1, program().at(node.layout).fields.size() });
    bool isCreatedByProgram = !expected.orders.isEmpty() && !expected.orders.first().isEmpty();
    std::span<const uint32_t> order = isCreatedByProgram ? expected.orders.first().span() : program().at(node.layout).declaredOrder.span();
    AOT::stableSort(members, { }, [&](const MemberParse& parse) {
        return parse.member->field ? static_cast<size_t>(std::ranges::find(order, *parse.member->field) - order.begin()) : order.size();
    });
    return members;
}

auto JSONParserBuilder::classifyMissingMember(const ResultMember& member, const ResultNode& memberNode) const -> MissingMember
{
    if (!memberNode.defaultValue) {
        // An arm of a union may have a default, which a validator applies to a missing member whether or not the member is optional.
        if (appliesDefault(program(), member.node))
            return MissingMember::Unknown;
        return member.isOptional ? MissingMember::Absent : MissingMember::Rejected;
    }

    const ResultLiteral& defaultValue = *memberNode.defaultValue;
    bool hasShapeOfNode = false;
    switch (memberNode.shape) {
    case ResultShape::Unknown:
        hasShapeOfNode = true;
        break;
    case ResultShape::Null:
        hasShapeOfNode = defaultValue.kind == ResultLiteral::Kind::Null;
        break;
    case ResultShape::String:
        hasShapeOfNode = defaultValue.kind == ResultLiteral::Kind::String;
        break;
    case ResultShape::Number:
        hasShapeOfNode = defaultValue.kind == ResultLiteral::Kind::Number && std::isfinite(defaultValue.number);
        break;
    case ResultShape::Boolean:
        hasShapeOfNode = defaultValue.kind == ResultLiteral::Kind::Boolean;
        break;
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        hasShapeOfNode = memberNode.literal == defaultValue;
        break;
    case ResultShape::Union:
        return MissingMember::Unknown;
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::AnyObject:
    case ResultShape::BigInt:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
        break;
    }
    return hasShapeOfNode ? MissingMember::Default : MissingMember::Rejected;
}

auto JSONParserBuilder::createInitialState(const ObjectParse& parse, ValueID firstKey) -> ObjectState
{
    ValueID undefined = builder().appendUndefined();
    ValueID notSeen = builder().appendBoolean(false);
    Value begin = IRBuilder::make(Opcode::JSONObjectBegin);
    begin.setLayout(parse.node.layout);

    ObjectState state;
    state.values.fill(undefined, parse.members.size());
    state.seen.fill(notSeen, parse.members.size());
    state.cursor = firstKey;
    state.order = builder().append(WTF::move(begin));
    state.extra = undefined;
    return state;
}

auto JSONParserBuilder::bindState(const ObjectParse& parse, BlockID block) -> ObjectState
{
    if (builder().function().at(block).parameters.isEmpty()) {
        for (const MemberParse& member : parse.members) {
            builder().addParameter(block, member.valueType);
            builder().addParameter(block, TypeTable::boolean);
        }
        builder().addParameter(block, cursorType());
        builder().addParameter(block, keyOrderType());
        builder().addParameter(block, TypeTable::anything);
    }

    const CompactList<ValueID>& parameters = builder().function().at(block).parameters;
    RELEASE_ASSERT(parameters.size() == 2 * parse.members.size() + 3);
    ObjectState state;
    size_t position = 0;
    for (size_t index = 0; index < parse.members.size(); ++index) {
        state.values.append(parameters[position++]);
        state.seen.append(parameters[position++]);
    }
    state.cursor = parameters[position++];
    state.order = parameters[position++];
    state.extra = parameters[position++];
    return state;
}

void JSONParserBuilder::emitJumpWithState(BlockID block, const ObjectState& state)
{
    bool isBound = !builder().function().at(block).parameters.isEmpty();
    if (!isBound) {
        builder().emitJump(block);
        return;
    }

    Arguments arguments;
    arguments.reserveInitialCapacity(2 * state.values.size() + 3);
    for (size_t index = 0; index < state.values.size(); ++index) {
        arguments.append(state.values[index]);
        arguments.append(state.seen[index]);
    }
    arguments.append(state.cursor);
    arguments.append(state.order);
    arguments.append(state.extra);
    builder().emitJump(block, WTF::move(arguments));
}

auto JSONParserBuilder::emitOrderedMembers(const ObjectParse& parse, ObjectState state) -> ObjectState
{
    for (size_t index = 0; index < parse.members.size(); ++index) {
        const MemberParse& member = parse.members[index];
        bool mayBeMissing = member.missing == MissingMember::Absent || member.missing == MissingMember::Default;
        bool isExpectedInOrder = member.hasPlainKey && canFillField(*this, member.valueType, *member.member, fieldOf(program(), parse.node, *member.member));
        if (!isExpectedInOrder) {
            if (mayBeMissing)
                continue;
            break;
        }

        ObjectState stateAtMiss = state;
        BlockID miss = builder().addBlock();
        BlockID afterMember = builder().addBlock();
        ValueID afterKey = emitExpectText(plainTextOf(inputs(), member.quotedKeyAndColon), state.cursor, miss, true);
        emitMember(parse, index, appendSkipWhitespace(afterKey), state, afterMember);

        builder().moveToEndOf(miss);
        if (!mayBeMissing) {
            emitJumpWithState(parse.anyOrder, stateAtMiss);
            builder().moveToEndOf(afterMember);
            continue;
        }

        BlockID next = builder().addBlock();
        ValueID value = builder().addParameter(next, member.valueType);
        ValueID seen = builder().addParameter(next, TypeTable::boolean);
        ValueID cursor = builder().addParameter(next, cursorType());
        ValueID order = builder().addParameter(next, keyOrderType());
        builder().emitJump(next, { stateAtMiss.values[index], stateAtMiss.seen[index], stateAtMiss.cursor, stateAtMiss.order });
        builder().moveToEndOf(afterMember);
        builder().emitJump(next, { state.values[index], state.seen[index], state.cursor, state.order });

        builder().moveToEndOf(next);
        state.values[index] = value;
        state.seen[index] = seen;
        state.cursor = cursor;
        state.order = order;
    }
    return state;
}

void JSONParserBuilder::emitUnorderedMembers(const ObjectParse& parse)
{
    const ParseExits& exits = parse.exits;
    builder().moveToEndOf(parse.anyOrder);
    ObjectState state = bindState(parse, parse.anyOrder);
    builder().append(Opcode::LoopHint, { });

    BlockID end = builder().addBlock();
    BlockID more = builder().addBlock();
    emitBranchOnByte(state.cursor, '}', end, more);
    builder().moveToEndOf(end);
    emitJumpWithState(parse.closing, state);

    builder().moveToEndOf(more);
    emitRequireByte(state.cursor, '"', exits.rejected);
    emitJumpIf(builder().append(Opcode::JSONKeyHasEscapes, { state.cursor }), exits.unknown);

    auto emitMatchedMember = [&](size_t index, ValueID valueStart) {
        emitJumpIf(state.seen[index], exits.unknown);
        ObjectState stateOfMember = state;
        emitMember(parse, index, valueStart, stateOfMember, parse.anyOrder);
    };

    bool hasOtherKeys = false;
    for (size_t index = 0; index < parse.members.size(); ++index) {
        const MemberParse& member = parse.members[index];
        if (!member.hasPlainKey) {
            hasOtherKeys = true;
            continue;
        }
        BlockID nextTest = builder().addBlock();
        ValueID afterKey = emitExpectText(plainTextOf(inputs(), member.quotedKey), state.cursor, nextTest, false);
        emitMatchedMember(index, emitExpectColon(afterKey, exits.rejected));
        builder().moveToEndOf(nextTest);
    }

    if (hasOtherKeys) {
        ParsedValue key = emitParsedOrExit(builder().append(Opcode::JSONParseKey, { state.cursor }), exits.unknown);
        ValueID valueStart = emitExpectColon(key.next, exits.rejected);
        for (size_t index = 0; index < parse.members.size(); ++index) {
            const MemberParse& member = parse.members[index];
            if (member.hasPlainKey)
                continue;
            ValueID isEqual = builder().append(Opcode::StrictEq, { key.value, appendString(member.name) });
            IRBuilder::BranchBlocks branch = builder().emitBranch(isEqual);
            emitMatchedMember(index, valueStart);
            builder().moveToEndOf(branch.notTaken);
        }
    }

    switch (parse.node.undeclared) {
    case UndeclaredMembers::Allowed:
        if (exits.undeclaredMember)
            builder().emitJump(exits.undeclaredMember);
        else
            emitExtraMembers(parse, state, parse.anyOrder);
        return;
    case UndeclaredMembers::Rejected:
        builder().emitJump(exits.rejected);
        return;
    case UndeclaredMembers::Validated:
        emitValidatedExtraMember(parse, state, parse.anyOrder);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ParsedValue JSONParserBuilder::emitObjectClosing(const ObjectParse& parse)
{
    LayoutID layout = parse.node.layout;
    builder().moveToEndOf(parse.closing);
    ObjectState state = bindState(parse, parse.closing);
    for (size_t index = 0; index < parse.members.size(); ++index)
        emitMissingMember(parse, index, state);
    ValueID next = builder().append(Opcode::JSONSkipByte, { state.cursor });

    Vector<uint32_t, 8> fields;
    Vector<ValueID, 8> values;
    for (size_t index = 0; index < parse.members.size(); ++index) {
        if (parse.members[index].missing == MissingMember::Absent)
            continue;
        const ResultMember& member = *parse.members[index].member;
        fields.append(*member.field);
        values.append(emitRequireTypeOfMember(*this, state.values[index], member, fieldOf(program(), parse.node, member), parse.exits.unknown));
    }

    Value allocation = IRBuilder::make(Opcode::NewObject);
    allocation.setLayout(layout);
    allocation.setFields(fields.span());
    allocation.setValuesOperands(values.span());
    if (AllocationSiteID site = inputs().flow.typedResultSite(layout)) {
        allocation.setAllocationSite(site);
        allocation.setAllocationClass(inputs().flow.at(site).allocationClass);
    }
    ValueID object = builder().append(WTF::move(allocation));
    for (size_t index = 0; index < parse.members.size(); ++index) {
        if (parse.members[index].missing == MissingMember::Absent)
            emitStoreOfMemberIfSeen(parse, index, state, object);
    }

    Value finish = IRBuilder::make(Opcode::JSONObjectFinish, { state.order, object, state.extra });
    finish.setLayout(layout);
    BlockID finished = builder().addBlock();
    emitBranchOnOutcome(builder().append(WTF::move(finish)), finished, parse.exits.unknown, parse.exits.unknown);

    builder().moveToEndOf(finished);
    m_rules.emitRules(parse.node, object, parse.exits.rejected);
    return { object, next };
}

// A field exists when its slot is filled, so the slot of a member that the text leaves out stays as the allocation made it: absent.
void JSONParserBuilder::emitStoreOfMemberIfSeen(const ObjectParse& parse, size_t index, const ObjectState& state, ValueID object)
{
    const ResultMember& member = *parse.members[index].member;
    const Field& field = fieldOf(program(), parse.node, member);
    // The parse gives up at a member whose field takes no value, and a field that nothing reads has no slot.
    if (!canFillField(*this, parse.members[index].valueType, member, field) || isRemovedField(program(), { parse.node.layout, *member.field }))
        return;
    IRBuilder::BranchBlocks branch = builder().emitBranch(state.seen[index]);
    BlockID next = builder().addBlock();

    builder().moveToEndOf(branch.taken);
    ValueID value = emitRequireTypeOfMember(*this, state.values[index], member, field, parse.exits.unknown);
    // The allocation is the guard of its own shape.
    Value store = IRBuilder::make(Opcode::PutSlot, { object, object, value });
    store.setKey(field.key);
    store.setReceivers(layoutSetOf(singleElementSpan(parse.node.layout)));
    store.setLevel(KnowledgeLevel::Expected);
    builder().append(WTF::move(store));
    builder().emitJump(next);

    builder().moveToEndOf(branch.notTaken);
    builder().emitJump(next);

    builder().moveToEndOf(next);
}

void JSONParserBuilder::emitMember(const ObjectParse& parse, size_t index, ValueID valueStart, ObjectState& state, BlockID whenMoreMembers)
{
    const MemberParse& member = parse.members[index];
    const Field& field = fieldOf(program(), parse.node, *member.member);
    if (!canFillField(*this, member.valueType, *member.member, field)) {
        addInterpretedNodeDecline(decisionInputs(), parse.node.layout, InterpretedCause::FieldTypeIsNotTestable);
        builder().emitJump(parse.exits.unknown);
        return;
    }

    BlockID memberRejected = builder().addBlock(BlockFrequency::Rare);
    ParsedValue parsed = emitNodeOrGenericValue(member.member->node, valueStart, { memberRejected, parse.exits.unknown });
    BlockID accepted = builder().currentBlock();

    builder().moveToEndOf(memberRejected);
    emitRejectionOfMember(member.name, valueStart, parse.exits);

    builder().moveToEndOf(accepted);
    state.values[index] = emitTestFieldType(*this, parsed.value, *member.member, field, parse.exits.unknown);
    state.seen[index] = builder().appendBoolean(true);
    state.order = appendObjectKey(builder(), state.order, parse.node.layout, *member.member->field);
    emitMemberSeparator(parse, parsed.next, state, whenMoreMembers);
}

void JSONParserBuilder::emitMemberSeparator(const ObjectParse& parse, ValueID next, ObjectState& state, BlockID whenMoreMembers)
{
    ValueID separator = appendSkipWhitespace(next);
    BlockID end = builder().addBlock();
    BlockID more = builder().addBlock();
    emitBranchOnByte(separator, '}', end, more);

    builder().moveToEndOf(end);
    state.cursor = separator;
    emitJumpWithState(parse.closing, state);

    builder().moveToEndOf(more);
    emitRequireByte(separator, ',', parse.exits.rejected);
    ValueID key = appendSkipPunctuator(separator);
    emitRequireByte(key, '"', parse.exits.rejected);
    state.cursor = key;
    emitJumpWithState(whenMoreMembers, state);
}

void JSONParserBuilder::emitMissingMember(const ObjectParse& parse, size_t index, ObjectState& state)
{
    const MemberParse& member = parse.members[index];
    switch (member.missing) {
    case MissingMember::Absent:
        return;
    case MissingMember::Rejected:
        emitJumpUnless(state.seen[index], parse.exits.rejected);
        return;
    case MissingMember::Unknown:
        emitJumpUnless(state.seen[index], parse.exits.unknown);
        return;
    case MissingMember::Default:
        break;
    }

    ResultNode memberNode = describeNode(program(), member.member->node);
    IRBuilder::BranchBlocks branch = builder().emitBranch(state.seen[index]);
    BlockID filled = builder().addBlock();

    builder().moveToEndOf(branch.notTaken);
    ValueID defaultValue = m_rules.appendLiteral(*memberNode.defaultValue);
    m_rules.emitRules(memberNode, defaultValue, parse.exits.rejected);
    ValueID orderWithDefault = appendObjectKey(builder(), state.order, parse.node.layout, *member.member->field);
    ValueID value = builder().addParameter(filled, inputs().types.unionOf(member.valueType, builder().function().at(defaultValue).type));
    ValueID order = builder().addParameter(filled, keyOrderType());
    builder().emitJump(filled, { defaultValue, orderWithDefault });

    builder().moveToEndOf(branch.taken);
    builder().emitJump(filled, { state.values[index], state.order });

    builder().moveToEndOf(filled);
    state.values[index] = value;
    state.order = order;
}

void JSONParserBuilder::emitExtraMembers(const ObjectParse& parse, ObjectState& state, BlockID whenMoreMembers)
{
    Value parseMembers = IRBuilder::make(Opcode::JSONParseExtraMembers, { state.cursor, state.extra, state.order });
    parseMembers.setLayout(parse.node.layout);
    ValueID results = builder().append(WTF::move(parseMembers));
    BlockID parsed = builder().addBlock();
    emitBranchOnOutcome(appendResultAt(results, 0), parsed, parse.exits.unknown, parse.exits.unknown);

    builder().moveToEndOf(parsed);
    state.extra = appendResultAt(results, 1);
    ValueID next = appendResultAt(results, 2);
    state.order = appendResultAt(results, 3);
    emitMemberSeparator(parse, next, state, whenMoreMembers);
}

void JSONParserBuilder::emitValidatedExtraMember(const ObjectParse& parse, ObjectState& state, BlockID whenMoreMembers)
{
    RELEASE_ASSERT(parse.node.undeclaredValue);
    ValueID keyStart = state.cursor;
    ParsedValue key = emitParsedOrExit(builder().append(Opcode::JSONParseKey, { keyStart }), parse.exits.unknown);
    ValueID valueStart = emitExpectColon(key.next, parse.exits.rejected);

    BlockID valueRejected = builder().addBlock(BlockFrequency::Rare);
    ParsedValue parsed = emitNodeOrGenericValue(*parse.node.undeclaredValue, valueStart, { valueRejected, parse.exits.unknown });
    BlockID accepted = builder().currentBlock();

    builder().moveToEndOf(valueRejected);
    emitRejectionOfKey(keyStart, valueStart, parse.exits);

    builder().moveToEndOf(accepted);
    Value addMember = IRBuilder::make(Opcode::JSONAddExtraMember, { state.extra, key.value, parsed.value, state.order });
    addMember.setLayout(parse.node.layout);
    ValueID results = builder().append(WTF::move(addMember));
    state.extra = appendResultAt(results, 0);
    state.order = appendResultAt(results, 1);
    emitMemberSeparator(parse, parsed.next, state, whenMoreMembers);
}

void JSONParserBuilder::emitRejectionOfMember(StringID name, ValueID valueStart, const ParseExits& exits)
{
    Value repeats = IRBuilder::make(Opcode::JSONMemberRepeats, { valueStart });
    repeats.setString(name);
    builder().emitBranch(builder().append(WTF::move(repeats)), Target { exits.unknown, { }, { } }, Target { exits.rejected, { }, { } });
}

void JSONParserBuilder::emitRejectionOfKey(ValueID keyStart, ValueID valueStart, const ParseExits& exits)
{
    ValueID repeats = builder().append(Opcode::JSONKeyRepeats, { keyStart, valueStart });
    builder().emitBranch(repeats, Target { exits.unknown, { }, { } }, Target { exits.rejected, { }, { } });
}

ValueID JSONParserBuilder::emitExpectColon(ValueID cursor, BlockID rejected)
{
    ValueID colon = appendSkipWhitespace(cursor);
    emitRequireByte(colon, ':', rejected);
    return appendSkipPunctuator(colon);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
