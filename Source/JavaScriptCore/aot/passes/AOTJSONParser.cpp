/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONEmission.h"

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowResults.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTSort.h"
#include "AOTValidatorBuilder.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// In the order in which a union looks for the arms of a class.
static constexpr std::pair<JSONValueClass, ASCIILiteral> firstBytesOfClasses[] {
    { JSONValueClass::String, "\""_s },
    { JSONValueClass::Number, "-0123456789"_s },
    { JSONValueClass::Boolean, "tf"_s },
    { JSONValueClass::Null, "n"_s },
    { JSONValueClass::Array, "["_s },
    { JSONValueClass::Object, "{"_s },
};

static Type testableTypeOf(const Program& program, const ResultNode& node)
{
    return program.at(node.type).withoutClassProofs().withUntestableBuiltinsWidened();
}

static TypeID valueTypeOf(const EmissionInputs& inputs, const ResultNode& node)
{
    return inputs.types.intern(testableTypeOf(inputs.program, node).withPrimitives(Primitive::Undefined));
}

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

static bool hasShapeOf(const ResultNode& node, const ResultLiteral& literal)
{
    switch (node.shape) {
    case ResultShape::Unknown:
        return true;
    case ResultShape::Null:
        return literal.kind == ResultLiteral::Kind::Null;
    case ResultShape::String:
        return literal.kind == ResultLiteral::Kind::String;
    case ResultShape::Number:
        return literal.kind == ResultLiteral::Kind::Number && std::isfinite(literal.number);
    case ResultShape::Boolean:
        return literal.kind == ResultLiteral::Kind::Boolean;
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        return node.literal == literal;
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::AnyObject:
    case ResultShape::BigInt:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct JSONParserBuilder final : Emission {
    struct ParsedValue {
        ValueID value;
        ValueID next;
    };

    struct ParseExits {
        BlockID rejected;
        BlockID unknown;
        // Set where the object is one of several types that are tried in turn: a member that this type does not declare says that the text is of another.
        BlockID undeclaredMember { };
    };

    struct ValueMerge {
        BlockID block;
        ParsedValue merged;
    };

    enum class MissingMember : uint8_t { Absent, Rejected, Default, Unknown };

    struct MemberParse {
        const ResultMember* member { nullptr };
        const Field* field { nullptr };
        StringID name;
        // None where the key needs escapes.
        StringID quotedKey;
        StringID quotedKeyAndColon;
        TypeID valueType;
        MissingMember missing { MissingMember::Unknown };
    };

    // What the blocks that parse the members hand on: the value of each member and whether the text had it, the cursor, the key order and the extra members.
    struct ObjectState {
        ValueID& value(size_t member) { return slots[2 * member]; }
        ValueID& seen(size_t member) { return slots[2 * member + 1]; }
        ValueID& cursor() { return slots[slots.size() - 3]; }
        ValueID& order() { return slots[slots.size() - 2]; }
        ValueID& extra() { return slots[slots.size() - 1]; }

        Vector<ValueID, 19> slots;
    };

    struct ObjectParse {
        const ResultNode& node;
        Vector<MemberParse, 8> members;
        BlockID anyOrder;
        BlockID closing;
        ParseExits exits;
    };

    struct ClassGroup {
        Vector<ResultNodeRef, 4> arms;
        OptionSet<JSONValueClass> classes;
        BlockID block;
    };

    TypeID cursorType() { return inputs().opaqueType(OpaqueKind::Cursor); }
    TypeID keyOrderType() { return inputs().opaqueType(OpaqueKind::KeyOrder); }
    NodeDecisionInputs decisionInputs() const { return { program(), inputs().flow, inputs().build }; }
    static ASCIILiteral wordOf(const ResultLiteral& literal) { return literal.kind == ResultLiteral::Kind::Null ? "null"_s : literal.boolean ? "true"_s : "false"_s; }

    // The text in quotes, as JSON spells it without escapes. None where it needs escapes.
    StringID quotedIfPlain(StringID text, bool isFollowedByColon = false)
    {
        std::span<const uint8_t> bytes = program().string(text);
        if (!std::ranges::all_of(bytes, [](uint8_t byte) { return byte >= 0x20 && byte <= 0x7E && byte != '"' && byte != '\\'; }))
            return { };
        Vector<uint8_t> quoted;
        quoted.reserveInitialCapacity(bytes.size() + 3);
        quoted.append('"');
        quoted.append(bytes);
        quoted.append('"');
        if (isFollowedByColon)
            quoted.append(':');
        return inputs().strings.intern(quoted.span());
    }

    ValueMerge createMerge(TypeID valueType)
    {
        BlockID block = builder().addBlock();
        ValueID value = builder().addParameter(block, valueType);
        return { block, { value, builder().addParameter(block, cursorType()) } };
    }

    ValueMerge createMergeOf(std::span<const ResultNodeRef> arms)
    {
        TypeID valueType = TypeTable::never;
        for (ResultNodeRef arm : arms)
            valueType = inputs().types.unionOf(valueType, valueTypeOf(inputs(), describeNode(program(), arm)));
        return createMerge(valueType);
    }

    void jumpTo(const ValueMerge& merge, ParsedValue parsed) { builder().emitJump(merge.block, { parsed.value, parsed.next }); }

    ParsedValue continueAt(const ValueMerge& merge)
    {
        builder().moveToEndOf(merge.block);
        return merge.merged;
    }

    ParsedValue emitParse(Opcode opcode, ValueID cursor, BlockID whenNotParsed)
    {
        ValueID results = builder().append(opcode, { cursor });
        emitJumpUnless(appendResultAt(results, 0), whenNotParsed);
        return { appendResultAt(results, 1), appendResultAt(results, 2) };
    }

    ValueID emitExpectText(StringID text, ValueID cursor, BlockID otherwise, bool isLikely)
    {
        Value test = make<&Value::setString, &Value::setIsLikely>(Opcode::JSONStartsWith, { cursor }, text, isLikely);
        IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(builder().append(WTF::move(test)), 0, BlockFrequency::Normal, isLikely ? BlockFrequency::Unlikely : BlockFrequency::Normal);

        builder().moveToEndOf(branch.notTaken);
        builder().emitJump(otherwise);

        builder().moveToEndOf(branch.taken);
        return builder().append(make<&Value::setString>(Opcode::JSONAdvance, { cursor, branch.provenOnTaken }, text));
    }

    ValueID emitExpectWord(ASCIILiteral word, ValueID cursor, BlockID otherwise) { return emitExpectText(inputs().strings.intern(word), cursor, otherwise, false); }

    void emitJumpIfStartsWith(StringID text, ValueID cursor, BlockID target)
    {
        BlockID otherwise = builder().addBlock();
        builder().emitBranch(builder().append(make<&Value::setString>(Opcode::JSONStartsWith, { cursor }, text)), Target { target, { }, { } }, Target { otherwise, { }, { } });
        builder().moveToEndOf(otherwise);
    }

    void emitBranchOnByte(ValueID cursor, uint8_t byte, BlockID whenEqual, BlockID otherwise)
    {
        ValueID actual = builder().append(Opcode::JSONGetByte, { cursor });
        ValueID isEqual = builder().append(Opcode::StrictEq, { actual, builder().appendInteger(byte) });
        builder().emitBranch(isEqual, Target { whenEqual, { }, { } }, Target { otherwise, { }, { } });
    }

    // Goes on where the byte is the next, and answers the block for where it is not.
    BlockID emitBranchOnByte(ValueID cursor, uint8_t byte)
    {
        BlockID whenEqual = builder().addBlock();
        BlockID otherwise = builder().addBlock();
        emitBranchOnByte(cursor, byte, whenEqual, otherwise);
        builder().moveToEndOf(whenEqual);
        return otherwise;
    }

    void emitRequireByte(ValueID cursor, uint8_t byte, BlockID otherwise)
    {
        BlockID matched = builder().addBlock();
        emitBranchOnByte(cursor, byte, matched, otherwise);
        builder().moveToEndOf(matched);
    }

    ValueID appendSkipWhitespace(ValueID cursor) { return builder().append(Opcode::JSONSkipWhitespace, { cursor }); }
    ValueID appendSkipPunctuator(ValueID cursor) { return appendSkipWhitespace(builder().append(Opcode::JSONSkipByte, { cursor })); }

    ValueID emitExpectColon(ValueID cursor, BlockID rejected)
    {
        ValueID colon = appendSkipWhitespace(cursor);
        emitRequireByte(colon, ':', rejected);
        return appendSkipPunctuator(colon);
    }

    ParsedValue emitNode(ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
    {
        ResultNode node = describeNode(program(), ref);
        NodeDecision decision = decideNodeCode(decisionInputs(), ref, NodeUse::ParsedFromText);
        switch (decision.code) {
        case NodeCode::Interpreted:
            addInterpretedNodeDecline(decisionInputs(), firstLayoutOf(program(), ref), decision.cause);
            return emitGenericValueFor(ref, node, cursor, exits);
        case NodeCode::Compiled:
            break;
        }
        bool hasOwnFunction = node.isContainer() && (ref.isValidated() || !ref.part);
        if (!hasOwnFunction)
            return emitNodeBody(ref, cursor, exits);

        ValueID results = appendCallOf(inputs().functions.ensure(parserKeyOf(ref)), { cursor });
        BlockID parsed = builder().addBlock();
        emitBranchOnOutcome(appendResultAt(results, 0), parsed, exits.rejected, exits.unknown);

        builder().moveToEndOf(parsed);
        return { appendResultAt(results, 1), appendResultAt(results, 2) };
    }

    ParsedValue emitNodeOrGenericValue(ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
    {
        ResultNode node = describeNode(program(), ref);
        bool fastPathOfValidationCanMiss = node.shape == ResultShape::Number || node.shape == ResultShape::NumberLiteral || node.isContainer();
        bool canMiss = ref.isValidated() ? fastPathOfValidationCanMiss : node.shape != ResultShape::Unknown;
        if (!canMiss || decideNodeCode(decisionInputs(), ref, NodeUse::ParsedFromText).code == NodeCode::Interpreted)
            return emitNode(ref, cursor, exits);

        BlockID again = builder().addBlock(BlockFrequency::Rare);
        ValueMerge merge = createMerge(valueTypeOf(inputs(), node));
        jumpTo(merge, emitNode(ref, cursor, { ref.isValidated() ? exits.rejected : again, again }));

        builder().moveToEndOf(again);
        jumpTo(merge, emitGenericValueFor(ref, node, cursor, exits));
        return continueAt(merge);
    }

    ParsedValue emitNodeBody(ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
    {
        ResultNode node = describeNode(program(), ref);
        ParsedValue parsed;
        switch (node.shape) {
        case ResultShape::Never:
        case ResultShape::Undefined:
        case ResultShape::BigInt:
            return emitArmsInOrder({ }, cursor, exits);
        case ResultShape::AnyObject: {
            BlockID isOpened = builder().addBlock();
            BlockID isNotBrace = builder().addBlock();
            emitBranchOnByte(cursor, '{', isOpened, isNotBrace);
            builder().moveToEndOf(isNotBrace);
            emitBranchOnByte(cursor, '[', isOpened, exits.rejected);
            builder().moveToEndOf(isOpened);
            [[fallthrough]];
        }
        case ResultShape::Unknown:
            parsed = emitParse(Opcode::JSONParseValue, cursor, exits.unknown);
            break;
        case ResultShape::Boolean: {
            ValueMerge merge = createMerge(TypeTable::boolean);
            BlockID isNotTrue = builder().addBlock();
            ValueID afterTrue = emitExpectWord("true"_s, cursor, isNotTrue);
            jumpTo(merge, { builder().appendBoolean(true), afterTrue });

            builder().moveToEndOf(isNotTrue);
            ValueID afterFalse = emitExpectWord("false"_s, cursor, exits.rejected);
            jumpTo(merge, { builder().appendBoolean(false), afterFalse });
            parsed = continueAt(merge);
            break;
        }
        case ResultShape::Number:
            parsed = emitNumber(node, cursor, exits);
            break;
        case ResultShape::String:
            parsed = emitString(cursor, exits);
            break;
        case ResultShape::Null:
        case ResultShape::BooleanLiteral:
        case ResultShape::NumberLiteral:
        case ResultShape::StringLiteral:
            parsed = emitLiteral(node.literal, cursor, exits);
            break;
        case ResultShape::Tuple:
            parsed = emitTuple(node, cursor, exits);
            break;
        case ResultShape::Array:
            parsed = emitArray(node, cursor, exits);
            break;
        case ResultShape::Dictionary:
            emitRequireByte(cursor, '{', exits.rejected);
            parsed = emitGenericValueFor(ref, node, cursor, exits);
            break;
        case ResultShape::Object:
            parsed = emitObject(ref, node, cursor, exits);
            break;
        case ResultShape::Union:
            parsed = emitUnion(node, cursor, exits);
            break;
        }
        // The other containers test their rules themselves, or leave them to the validator.
        if (!node.isContainer() || node.shape == ResultShape::Tuple) {
            RELEASE_ASSERT(!node.isContainer() || !hasRuleOnGivenValue(node));
            m_rules.emitRules(node, parsed.value, parsed.value, exits.rejected);
        }
        return { emitTypeTest(parsed.value, valueTypeOf(inputs(), node), exits.unknown), parsed.next };
    }

    ParsedValue emitGenericValueFor(ResultNodeRef ref, const ResultNode& node, ValueID cursor, const ParseExits& exits)
    {
        ParsedValue generic = emitParse(Opcode::JSONParseValue, cursor, exits.unknown);
        if (!ref.isValidated())
            return { emitTypeTest(generic.value, inputs().types.intern(testableTypeOf(program(), node)), exits.unknown), generic.next };

        ValueID value = ValidatorBuilder(builder(), inputs()).emitNode(ref, generic.value, { exits.rejected, exits.unknown });
        return { emitTypeTest(value, valueTypeOf(inputs(), node), exits.unknown), generic.next };
    }

    ParsedValue emitParseNumber(Opcode opcode, ValueID cursor, const ParseExits& exits)
    {
        ValueID results = builder().append(opcode, { cursor });
        IRBuilder::BranchBlocks branch = builder().emitBranch(appendResultAt(results, 0), BlockFrequency::Normal, BlockFrequency::Rare);

        builder().moveToEndOf(branch.notTaken);
        if (exits.rejected == exits.unknown)
            builder().emitJump(exits.unknown);
        else {
            Vector<std::pair<int64_t, Target>> numberStarts;
            for (char byte : "-0123456789"_s.span())
                numberStarts.append({ int64_t { byte }, Target { exits.unknown, { }, { } } });
            builder().emitSwitch(builder().append(Opcode::JSONGetByte, { cursor }), WTF::move(numberStarts), Target { exits.rejected, { }, { } });
        }

        builder().moveToEndOf(branch.taken);
        return { appendResultAt(results, 1), appendResultAt(results, 2) };
    }

    ParsedValue emitNumber(const ResultNode& node, ValueID cursor, const ParseExits& exits)
    {
        Type numbers = program().at(node.type).withoutObjects().withoutPrimitives(static_cast<uint16_t>(Primitive::Defined & ~Primitive::Number));
        bool isInteger = hasIntegerRule(node) || numbers.isInteger();
        ParsedValue parsed = emitParseNumber(isInteger ? Opcode::JSONParseInteger : Opcode::JSONParseNumber, cursor, exits);
        return { emitTypeTest(parsed.value, inputs().types.intern(numbers), exits.unknown), parsed.next };
    }

    ParsedValue emitString(ValueID cursor, const ParseExits& exits)
    {
        emitRequireByte(cursor, '"', exits.rejected);
        return emitParse(Opcode::JSONParseString, cursor, exits.unknown);
    }

    ParsedValue emitLiteral(const ResultLiteral& literal, ValueID cursor, const ParseExits& exits)
    {
        ValueID constant = m_rules.appendLiteral(literal);
        switch (literal.kind) {
        case ResultLiteral::Kind::Null:
        case ResultLiteral::Kind::Boolean:
            return { constant, emitExpectWord(wordOf(literal), cursor, exits.rejected) };
        case ResultLiteral::Kind::Number: {
            ParsedValue parsed = emitParseNumber(Opcode::JSONParseNumber, cursor, exits);
            ValueID isEqual = builder().append(Opcode::StrictEq, { parsed.value, constant });
            IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(isEqual, 0, BlockFrequency::Normal, BlockFrequency::Rare);

            builder().moveToEndOf(branch.notTaken);
            builder().emitJump(exits.rejected);

            builder().moveToEndOf(branch.taken);
            return { branch.provenOnTaken, parsed.next };
        }
        case ResultLiteral::Kind::String: {
            BlockID merge = builder().addBlock();
            ValueID next = builder().addParameter(merge, cursorType());
            if (StringID quoted = quotedIfPlain(literal.string)) {
                BlockID escapedSpelling = builder().addBlock(BlockFrequency::Rare);
                builder().emitJump(merge, { emitExpectText(quoted, cursor, escapedSpelling, true) });
                builder().moveToEndOf(escapedSpelling);
            }

            ParsedValue parsed = emitString(cursor, exits);
            emitJumpUnless(builder().append(Opcode::StrictEq, { parsed.value, constant }), exits.rejected);
            builder().emitJump(merge, { parsed.next });

            builder().moveToEndOf(merge);
            return { constant, next };
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ParsedValue emitArray(const ResultNode& node, ValueID cursor, const ParseExits& exits)
    {
        RELEASE_ASSERT(node.children.size() == 1);
        emitRequireByte(cursor, '[', exits.rejected);
        ValueID afterBracket = appendSkipPunctuator(cursor);
        ValueID array = builder().append(makeArrayOf(program(), node, { }));
        BlockID header = builder().addBlock();
        BlockID done = builder().addBlock();
        ValueID cursorAtHeader = builder().addParameter(header, cursorType());
        ValueID cursorAtDone = builder().addParameter(done, cursorType());

        BlockID hasElements = emitBranchOnByte(afterBracket, ']');
        builder().emitJump(done, { afterBracket });

        builder().moveToEndOf(hasElements);
        builder().emitJump(header, { afterBracket });

        builder().moveToEndOf(header);
        builder().append(Opcode::LoopHint, { });
        ParsedValue element = emitNodeOrGenericValue(node.children[0], cursorAtHeader, exits);
        builder().append(Opcode::AppendElement, { array, element.value });
        ValueID afterElement = appendSkipWhitespace(element.next);
        BlockID hasNoComma = emitBranchOnByte(afterElement, ',');
        builder().emitJump(header, { appendSkipPunctuator(afterElement) });

        builder().moveToEndOf(hasNoComma);
        emitRequireByte(afterElement, ']', exits.rejected);
        builder().emitJump(done, { afterElement });

        builder().moveToEndOf(done);
        ValueID next = builder().append(Opcode::JSONSkipByte, { cursorAtDone });
        RELEASE_ASSERT(!hasRuleOnGivenValue(node));
        m_rules.emitRules(node, array, array, exits.rejected);
        return { array, next };
    }

    ParsedValue emitTuple(const ResultNode& node, ValueID cursor, const ParseExits& exits)
    {
        RELEASE_ASSERT(node.hasFixedLength());
        emitRequireByte(cursor, '[', exits.rejected);
        ValueID position = appendSkipPunctuator(cursor);

        Vector<ValueID, 4> elements;
        for (ResultNodeRef child : node.children) {
            if (!elements.isEmpty()) {
                emitRequireByte(position, ',', exits.rejected);
                position = appendSkipPunctuator(position);
            }
            ParsedValue element = emitNodeOrGenericValue(child, position, exits);
            elements.append(element.value);
            position = appendSkipWhitespace(element.next);
        }

        emitRequireByte(position, ']', exits.rejected);
        ValueID next = builder().append(Opcode::JSONSkipByte, { position });
        return { builder().append(makeArrayOf(program(), node, elements.span())), next };
    }

    // Flow fills the fields of a typed result with the arrays its parsers create, and those of an unvalidated result with the plain data of the generic parser too.
    ProofsByOrigin proofsByOriginOf(const ResultMember& member)
    {
        if (!member.node.isValidated())
            return ProofsByOrigin::AboutEveryClass;
        return createsEveryArrayItself(decisionInputs(), member.node, NodeUse::ParsedFromText) ? ProofsByOrigin::AboutArrays : ProofsByOrigin::None;
    }

    // The slot of a field of a function type holds fewer functions than the type admits.
    TypeID typeStoredInSlotOf(const Field& field)
    {
        if (field.slotType && program().at(field.slotType).isSubsetOf(program().at(field.declared.type), program()))
            return field.slotType;
        return field.declared.type;
    }

    TypeID testedFieldTypeOf(const MemberParse& member) { return fieldTypeToTest(typeStoredInSlotOf(*member.field), proofsByOriginOf(*member.member)); }
    bool canFillField(const MemberParse& member) { return !passesOnlyUndefinedIntoStoredType(program().at(member.valueType), testedFieldTypeOf(member)); }
    ValueID emitRequireTypeOfMember(ValueID value, const MemberParse& member, BlockID otherwise) { return emitRequireFieldType(value, typeStoredInSlotOf(*member.field), proofsByOriginOf(*member.member), otherwise); }
    ValueID appendObjectKey(ValueID order, LayoutID layout, const MemberParse& member) { return builder().append(make<&Value::setLayout, &Value::setField>(Opcode::JSONObjectKey, { order }, layout, *member.member->field)); }

    ParsedValue emitObject(ResultNodeRef ref, const ResultNode& node, ValueID cursor, const ParseExits& exits)
    {
        emitRequireByte(cursor, '{', exits.rejected);
        bool isModeled = allocatesLayout(decisionInputs(), node);
        bool hasNamedFields = std::ranges::all_of(node.members, [&](const ResultMember& member) {
            return member.field && program().at(member.key).kind == PropertyKeyKind::Text && !program().isIndexLike(member.key);
        });
        if (node.layout && !isModeled)
            addInterpretedNodeDecline(decisionInputs(), node.layout, InterpretedCause::LayoutIsNotModeled);
        else if (isModeled && !hasNamedFields)
            addInterpretedNodeDecline(decisionInputs(), node.layout, InterpretedCause::MemberIsNotNamedField);
        bool canAllocateLayout = isModeled && hasNamedFields
            && !inputs().build.exceeds(LimitID::MaxParsedObjectMembers, node.members.size(), Subject { .layout = node.layout });
        if (!canAllocateLayout)
            return emitGenericValueFor(ref, node, cursor, exits);

        ValueID firstKey = appendSkipPunctuator(cursor);
        ObjectParse parse { node, resolveMembers(node), builder().addBlock(), builder().addBlock(), exits };
        for (BlockID block : { parse.anyOrder, parse.closing }) {
            for (const MemberParse& member : parse.members) {
                builder().addParameter(block, member.valueType);
                builder().addParameter(block, TypeTable::boolean);
            }
            builder().addParameter(block, cursorType());
            builder().addParameter(block, keyOrderType());
            builder().addParameter(block, TypeTable::anything);
        }

        ValueID undefined = builder().appendUndefined();
        ValueID notSeen = builder().appendBoolean(false);
        ObjectState state;
        for (size_t index = 0; index < parse.members.size(); ++index)
            state.slots.appendList({ undefined, notSeen });
        state.slots.appendList({ firstKey, builder().append(make<&Value::setLayout>(Opcode::JSONObjectBegin, { }, node.layout)), undefined });

        emitOrderedMembers(parse, state);
        emitJumpWithState(parse.anyOrder, state);
        emitUnorderedMembers(parse);
        return emitObjectClosing(parse);
    }

    Vector<MemberParse, 8> resolveMembers(const ResultNode& node)
    {
        Vector<MemberParse, 8> members;
        members.reserveInitialCapacity(node.members.size());
        for (const ResultMember& member : node.members) {
            ResultNode memberNode = describeNode(program(), member.node);
            MemberParse parse { .member = &member, .field = &fieldOf(program(), node, member), .name = program().at(member.key).text };
            parse.quotedKey = quotedIfPlain(parse.name);
            if (parse.quotedKey)
                parse.quotedKeyAndColon = quotedIfPlain(parse.name, true);
            parse.valueType = valueTypeOf(inputs(), memberNode);
            parse.missing = classifyMissingMember(member, memberNode);

            const Type& fieldType = program().at(parse.field->declared.type);
            if (parse.missing == MissingMember::Absent && !(fieldType.primitives & Primitive::Undefined))
                parse.missing = MissingMember::Unknown;
            if (parse.missing == MissingMember::Default && !typeOfLiteral(*memberNode.defaultValue).isSubsetOf(fieldType, program()))
                parse.missing = MissingMember::Unknown;
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

    MissingMember classifyMissingMember(const ResultMember& member, const ResultNode& memberNode) const
    {
        if (!memberNode.defaultValue) {
            // An arm of a union may have a default, which a validator applies to a missing member whether or not the member is optional.
            if (appliesDefault(program(), member.node))
                return MissingMember::Unknown;
            return member.isOptional ? MissingMember::Absent : MissingMember::Rejected;
        }
        if (memberNode.shape == ResultShape::Union)
            return MissingMember::Unknown;
        return hasShapeOf(memberNode, *memberNode.defaultValue) ? MissingMember::Default : MissingMember::Rejected;
    }

    // A block without parameters goes on with the state of the one way that leads to it.
    void emitJumpWithState(BlockID block, const ObjectState& state)
    {
        builder().emitJump(block, function().at(block).parameters.isEmpty() ? Arguments() : Arguments(state.slots));
    }

    void emitOrderedMembers(const ObjectParse& parse, ObjectState& state)
    {
        for (size_t index = 0; index < parse.members.size(); ++index) {
            const MemberParse& member = parse.members[index];
            bool mayBeMissing = member.missing == MissingMember::Absent || member.missing == MissingMember::Default;
            bool isExpectedInOrder = member.quotedKey && canFillField(member);
            if (!isExpectedInOrder) {
                if (mayBeMissing)
                    continue;
                break;
            }

            ObjectState stateAtMiss = state;
            BlockID miss = builder().addBlock();
            BlockID afterMember = builder().addBlock();
            ValueID afterKey = emitExpectText(member.quotedKeyAndColon, state.cursor(), miss, true);
            emitMember(parse, index, appendSkipWhitespace(afterKey), state, afterMember);

            builder().moveToEndOf(miss);
            if (!mayBeMissing) {
                emitJumpWithState(parse.anyOrder, stateAtMiss);
                builder().moveToEndOf(afterMember);
                continue;
            }

            BlockID next = builder().addBlock();
            ObjectState merged = state;
            merged.value(index) = builder().addParameter(next, member.valueType);
            merged.seen(index) = builder().addParameter(next, TypeTable::boolean);
            merged.cursor() = builder().addParameter(next, cursorType());
            merged.order() = builder().addParameter(next, keyOrderType());
            builder().emitJump(next, { stateAtMiss.value(index), stateAtMiss.seen(index), stateAtMiss.cursor(), stateAtMiss.order() });
            builder().moveToEndOf(afterMember);
            builder().emitJump(next, { state.value(index), state.seen(index), state.cursor(), state.order() });

            builder().moveToEndOf(next);
            state = WTF::move(merged);
        }
    }

    void emitUnorderedMembers(const ObjectParse& parse)
    {
        const ParseExits& exits = parse.exits;
        builder().moveToEndOf(parse.anyOrder);
        ObjectState state { function().at(parse.anyOrder).parameters.span() };
        RELEASE_ASSERT(state.slots.size() == 2 * parse.members.size() + 3);
        builder().append(Opcode::LoopHint, { });

        BlockID more = emitBranchOnByte(state.cursor(), '}');
        emitJumpWithState(parse.closing, state);

        builder().moveToEndOf(more);
        emitRequireByte(state.cursor(), '"', exits.rejected);
        emitJumpIf(builder().append(Opcode::JSONKeyHasEscapes, { state.cursor() }), exits.unknown);

        auto emitMatchedMember = [&](size_t index, ValueID valueStart) {
            if (!inputs().build.isSabotaged(Sabotage::ParsedMembersComeOnce))
                emitJumpIf(state.seen(index), exits.unknown);
            ObjectState stateOfMember = state;
            emitMember(parse, index, valueStart, stateOfMember, parse.anyOrder);
        };

        for (size_t index = 0; index < parse.members.size(); ++index) {
            if (!parse.members[index].quotedKey)
                continue;
            BlockID nextTest = builder().addBlock();
            ValueID afterKey = emitExpectText(parse.members[index].quotedKey, state.cursor(), nextTest, false);
            emitMatchedMember(index, emitExpectColon(afterKey, exits.rejected));
            builder().moveToEndOf(nextTest);
        }

        if (parse.members.containsIf([](const MemberParse& member) { return !member.quotedKey; })) {
            ParsedValue key = emitParse(Opcode::JSONParseKey, state.cursor(), exits.unknown);
            ValueID valueStart = emitExpectColon(key.next, exits.rejected);
            for (size_t index = 0; index < parse.members.size(); ++index) {
                if (parse.members[index].quotedKey)
                    continue;
                ValueID isEqual = builder().append(Opcode::StrictEq, { key.value, appendString(parse.members[index].name) });
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
                emitExtraMembers(parse, state);
            return;
        case UndeclaredMembers::Rejected:
            builder().emitJump(exits.rejected);
            return;
        case UndeclaredMembers::Validated:
            emitValidatedExtraMember(parse, state);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ParsedValue emitObjectClosing(const ObjectParse& parse)
    {
        LayoutID layout = parse.node.layout;
        builder().moveToEndOf(parse.closing);
        ObjectState state { function().at(parse.closing).parameters.span() };
        RELEASE_ASSERT(state.slots.size() == 2 * parse.members.size() + 3);
        for (size_t index = 0; index < parse.members.size(); ++index)
            emitMissingMember(parse, index, state);
        ValueID next = builder().append(Opcode::JSONSkipByte, { state.cursor() });

        Vector<uint32_t, 8> fields;
        Vector<ValueID, 8> values;
        for (size_t index = 0; index < parse.members.size(); ++index) {
            const MemberParse& member = parse.members[index];
            if (member.missing == MissingMember::Absent)
                continue;
            fields.append(*member.member->field);
            values.append(emitRequireTypeOfMember(state.value(index), member, parse.exits.unknown));
        }

        Value allocation = make<&Value::setLayout, &Value::setFields, &Value::setValuesOperands>(Opcode::NewObject, { }, layout, fields.span(), values.span());
        if (AllocationSiteID site = inputs().flow.typedResultSite(layout)) {
            allocation.setAllocationSite(site);
            allocation.setAllocationClass(inputs().flow.at(site).allocationClass);
        }
        ValueID object = builder().append(WTF::move(allocation));
        for (size_t index = 0; index < parse.members.size(); ++index) {
            if (parse.members[index].missing == MissingMember::Absent)
                emitStoreOfMemberIfSeen(parse, parse.members[index], state.seen(index), state.value(index), object);
        }

        BlockID finished = builder().addBlock();
        ValueID outcome = builder().append(make<&Value::setLayout>(Opcode::JSONObjectFinish, { state.order(), object, state.extra() }, layout));
        emitBranchOnOutcome(outcome, finished, parse.exits.unknown, parse.exits.unknown);

        builder().moveToEndOf(finished);
        RELEASE_ASSERT(!hasRuleOnGivenValue(parse.node));
        m_rules.emitRules(parse.node, object, object, parse.exits.rejected);
        return { object, next };
    }

    // A field exists when its slot is filled, so the slot of a member that the text leaves out stays as the allocation made it: absent.
    void emitStoreOfMemberIfSeen(const ObjectParse& parse, const MemberParse& member, ValueID seen, ValueID parsed, ValueID object)
    {
        // The parse gives up at a member whose field takes no value, and a field that nothing reads has no slot.
        if (!canFillField(member) || isRemovedField(program(), { parse.node.layout, *member.member->field }))
            return;
        IRBuilder::BranchBlocks branch = builder().emitBranch(seen);
        BlockID next = builder().addBlock();
        ValueID value = emitRequireTypeOfMember(parsed, member, parse.exits.unknown);
        // The allocation is the guard of its own shape.
        Value store = IRBuilder::make(Opcode::PutSlot, { object, object, value });
        store.setKey(member.field->key);
        store.setReceivers(layoutSetOf(singleElementSpan(parse.node.layout)));
        store.setLevel(KnowledgeLevel::Expected);
        builder().append(WTF::move(store));
        builder().emitJump(next);

        builder().moveToEndOf(branch.notTaken);
        builder().emitJump(next);

        builder().moveToEndOf(next);
    }

    // A member that comes again replaces the one before it, so the text is rejected for a value only where its key does not repeat.
    ParsedValue emitValueOfMember(ResultNodeRef node, ValueID valueStart, const ParseExits& exits, Value&& repeats)
    {
        BlockID valueRejected = builder().addBlock(BlockFrequency::Rare);
        ParsedValue parsed = emitNodeOrGenericValue(node, valueStart, { valueRejected, exits.unknown });
        BlockID accepted = builder().currentBlock();

        builder().moveToEndOf(valueRejected);
        emitRejectionUnless(WTF::move(repeats), exits);

        builder().moveToEndOf(accepted);
        return parsed;
    }

    void emitRejectionUnless(Value&& repeats, const ParseExits& exits)
    {
        builder().emitBranch(builder().append(WTF::move(repeats)), Target { exits.unknown, { }, { } }, Target { exits.rejected, { }, { } });
    }

    static Value makeMemberRepeats(StringID name, ValueID valueStart) { return make<&Value::setString>(Opcode::JSONMemberRepeats, { valueStart }, name); }

    void emitMember(const ObjectParse& parse, size_t index, ValueID valueStart, ObjectState& state, BlockID whenMoreMembers)
    {
        const MemberParse& member = parse.members[index];
        if (!canFillField(member)) {
            addInterpretedNodeDecline(decisionInputs(), parse.node.layout, InterpretedCause::FieldTypeIsNotTestable);
            builder().emitJump(parse.exits.unknown);
            return;
        }

        ParsedValue parsed = emitValueOfMember(member.member->node, valueStart, parse.exits, makeMemberRepeats(member.name, valueStart));
        state.value(index) = emitRequireStoredType(parsed.value, testedFieldTypeOf(member), parse.exits.unknown);
        state.seen(index) = builder().appendBoolean(true);
        state.order() = appendObjectKey(state.order(), parse.node.layout, member);
        emitMemberSeparator(parse, parsed.next, state, whenMoreMembers);
    }

    void emitMemberSeparator(const ObjectParse& parse, ValueID next, ObjectState& state, BlockID whenMoreMembers)
    {
        ValueID separator = appendSkipWhitespace(next);
        BlockID more = emitBranchOnByte(separator, '}');
        state.cursor() = separator;
        emitJumpWithState(parse.closing, state);

        builder().moveToEndOf(more);
        emitRequireByte(separator, ',', parse.exits.rejected);
        ValueID key = appendSkipPunctuator(separator);
        emitRequireByte(key, '"', parse.exits.rejected);
        state.cursor() = key;
        emitJumpWithState(whenMoreMembers, state);
    }

    void emitMissingMember(const ObjectParse& parse, size_t index, ObjectState& state)
    {
        const MemberParse& member = parse.members[index];
        switch (member.missing) {
        case MissingMember::Absent:
            return;
        case MissingMember::Rejected:
            emitJumpUnless(state.seen(index), parse.exits.rejected);
            return;
        case MissingMember::Unknown:
            emitJumpUnless(state.seen(index), parse.exits.unknown);
            return;
        case MissingMember::Default:
            break;
        }

        ResultNode memberNode = describeNode(program(), member.member->node);
        IRBuilder::BranchBlocks branch = builder().emitBranch(state.seen(index));
        BlockID filled = builder().addBlock();

        builder().moveToEndOf(branch.notTaken);
        ValueID defaultValue = m_rules.appendLiteral(*memberNode.defaultValue);
        m_rules.emitRules(memberNode, defaultValue, defaultValue, parse.exits.rejected);
        ValueID orderWithDefault = appendObjectKey(state.order(), parse.node.layout, member);
        ValueID value = builder().addParameter(filled, inputs().types.unionOf(member.valueType, function().at(defaultValue).type));
        ValueID order = builder().addParameter(filled, keyOrderType());
        builder().emitJump(filled, { defaultValue, orderWithDefault });

        builder().moveToEndOf(branch.taken);
        builder().emitJump(filled, { state.value(index), state.order() });

        builder().moveToEndOf(filled);
        state.value(index) = value;
        state.order() = order;
    }

    void emitExtraMembers(const ObjectParse& parse, ObjectState& state)
    {
        ValueID results = builder().append(make<&Value::setLayout>(Opcode::JSONParseExtraMembers, { state.cursor(), state.extra(), state.order() }, parse.node.layout));
        BlockID parsed = builder().addBlock();
        emitBranchOnOutcome(appendResultAt(results, 0), parsed, parse.exits.unknown, parse.exits.unknown);

        builder().moveToEndOf(parsed);
        state.extra() = appendResultAt(results, 1);
        ValueID next = appendResultAt(results, 2);
        state.order() = appendResultAt(results, 3);
        emitMemberSeparator(parse, next, state, parse.anyOrder);
    }

    void emitValidatedExtraMember(const ObjectParse& parse, ObjectState& state)
    {
        RELEASE_ASSERT(parse.node.undeclaredValue);
        ValueID keyStart = state.cursor();
        ParsedValue key = emitParse(Opcode::JSONParseKey, keyStart, parse.exits.unknown);
        ValueID valueStart = emitExpectColon(key.next, parse.exits.rejected);
        ParsedValue parsed = emitValueOfMember(*parse.node.undeclaredValue, valueStart, parse.exits, IRBuilder::make(Opcode::JSONKeyRepeats, { keyStart, valueStart }));

        ValueID results = builder().append(make<&Value::setLayout>(Opcode::JSONAddExtraMember, { state.extra(), key.value, parsed.value, state.order() }, parse.node.layout));
        state.extra() = appendResultAt(results, 0);
        state.order() = appendResultAt(results, 1);
        emitMemberSeparator(parse, parsed.next, state, parse.anyOrder);
    }

    Vector<ClassGroup, 6> groupArmsByClass(std::span<const ResultNodeRef> arms)
    {
        Vector<OptionSet<JSONValueClass>, 4> classesOfArm(arms.size(), [&](size_t index) { return valueClassesOf(program(), arms[index]); });
        Vector<ClassGroup, 6> groups;
        for (auto [valueClass, firstBytes] : firstBytesOfClasses) {
            Vector<ResultNodeRef, 4> candidates;
            for (size_t index = 0; index < arms.size(); ++index) {
                if (classesOfArm[index].contains(valueClass))
                    candidates.append(arms[index]);
            }
            if (candidates.isEmpty())
                continue;

            size_t position = groups.findIf([&](const ClassGroup& group) { return group.arms == candidates; });
            if (position == notFound) {
                position = groups.size();
                groups.append(ClassGroup { WTF::move(candidates), { }, { } });
            }
            groups[position].classes.add(valueClass);
        }
        return groups;
    }

    std::optional<Vector<StringID, 8>> unconstrainedStringLiteralsOf(std::span<const ResultNodeRef> arms)
    {
        Vector<StringID, 8> literals;
        for (ResultNodeRef arm : arms) {
            ResultNode node = describeNode(program(), arm);
            bool isTested = std::ranges::any_of(node.rules, [](const ValidationRule& rule) { return subjectOf(rule) != RuleSubject::Nothing; });
            if (node.shape != ResultShape::StringLiteral || isTested)
                return std::nullopt;
            literals.appendIfNotContains(node.literal.string);
        }
        return literals;
    }

    void emitStringLiteralArms(std::span<const StringID> literals, const ValueMerge& merge, ValueID cursor, const ParseExits& exits)
    {
        auto emitJumpWithLiteral = [&](StringID literal, ValueID next) {
            jumpTo(merge, { emitTypeTest(appendString(literal), function().at(merge.merged.value).type, exits.unknown), next });
        };
        for (StringID literal : literals) {
            StringID quoted = quotedIfPlain(literal);
            if (!quoted)
                continue;
            BlockID nextLiteral = builder().addBlock();
            emitJumpWithLiteral(literal, emitExpectText(quoted, cursor, nextLiteral, false));
            builder().moveToEndOf(nextLiteral);
        }

        ParsedValue text = emitParse(Opcode::JSONParseString, cursor, exits.unknown);
        Vector<BlockID, 8> matches(literals.size(), [&](size_t) { return builder().addBlock(); });
        Vector<std::pair<StringID, Target>> cases(literals.size(), [&](size_t index) { return std::pair { literals[index], Target { matches[index], { }, { } } }; });
        builder().emitSwitchString(text.value, WTF::move(cases), Target { exits.rejected, { }, { } });
        for (size_t index = 0; index < literals.size(); ++index) {
            builder().moveToEndOf(matches[index]);
            emitJumpWithLiteral(literals[index], text.next);
        }
    }

    ParsedValue emitUnion(const ResultNode& node, ValueID cursor, const ParseExits& exits)
    {
        Vector<ClassGroup, 6> groups = groupArmsByClass(node.children.span());
        if (groups.isEmpty())
            return emitArmsInOrder({ }, cursor, exits);
        if (groups.size() == 1 && groups[0].arms.size() == 1) {
            ParsedValue parsed = emitNode(groups[0].arms[0], cursor, exits);
            RELEASE_ASSERT(!hasRuleOnGivenValue(node));
            m_rules.emitRules(node, parsed.value, parsed.value, exits.rejected);
            return parsed;
        }

        ValueMerge merge = createMergeOf(node.children.span());
        Vector<std::pair<int64_t, Target>> cases;
        for (ClassGroup& group : groups) {
            group.block = builder().addBlock();
            for (JSONValueClass valueClass : group.classes) {
                for (char byte : std::ranges::find(firstBytesOfClasses, valueClass, &std::pair<JSONValueClass, ASCIILiteral>::first)->second.span())
                    cases.append({ int64_t { byte }, Target { group.block, { }, { } } });
            }
        }
        builder().emitSwitch(builder().append(Opcode::JSONGetByte, { cursor }), WTF::move(cases), Target { exits.rejected, { }, { } });

        for (const ClassGroup& group : groups) {
            builder().moveToEndOf(group.block);
            std::optional<Vector<StringID, 8>> literals;
            if (group.arms.size() > 1)
                literals = unconstrainedStringLiteralsOf(group.arms.span());
            if (literals)
                emitStringLiteralArms(literals->span(), merge, cursor, exits);
            else if (group.classes.contains(JSONValueClass::Object))
                jumpTo(merge, emitObjectArms(group.arms.span(), cursor, exits));
            else
                jumpTo(merge, emitArmsInOrder(group.arms.span(), cursor, exits));
        }

        ParsedValue merged = continueAt(merge);
        RELEASE_ASSERT(!hasRuleOnGivenValue(node));
        m_rules.emitRules(node, merged.value, merged.value, exits.rejected);
        return merged;
    }

    ParsedValue emitArmsInOrder(std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits& exits)
    {
        if (arms.empty()) {
            builder().emitJump(exits.rejected);
            builder().moveToEndOf(builder().addBlock(BlockFrequency::Rare));
            return { builder().appendUndefined(), cursor };
        }
        if (arms.size() == 1)
            return emitNode(arms[0], cursor, exits);

        ValueMerge merge = createMergeOf(arms);
        for (const ResultNodeRef& arm : arms) {
            bool isLast = &arm == &arms.back();
            BlockID rejected = isLast ? exits.rejected : builder().addBlock();
            // What a validator makes of a member that it does not declare is its own rule.
            jumpTo(merge, emitNode(arm, cursor, { rejected, exits.unknown, arm.isValidated() ? BlockID() : rejected }));
            if (!isLast)
                builder().moveToEndOf(rejected);
        }
        return continueAt(merge);
    }

    ParsedValue emitObjectArms(std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits& exits)
    {
        std::optional<TaggedArms> split = findTaggedArms(program(), arms);
        if (!split || program().at(split->discriminant.key).kind != PropertyKeyKind::Text)
            return emitArmsInOrder(arms, cursor, exits);
        if (split->untagged.isEmpty())
            return emitArmsByDiscriminant(split->discriminant, split->tagged.span(), cursor, exits);

        ValueMerge merge = createMergeOf(arms);
        BlockID untagged = builder().addBlock();
        jumpTo(merge, emitArmsByDiscriminant(split->discriminant, split->tagged.span(), cursor, { untagged, exits.unknown }));

        builder().moveToEndOf(untagged);
        jumpTo(merge, emitArmsInOrder(split->untagged.span(), cursor, exits));
        return continueAt(merge);
    }

    ParsedValue emitArmsByDiscriminant(const ResultDiscriminant& discriminant, std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits& exits)
    {
        using Kind = ResultLiteral::Kind;
        std::span<const ResultLiteral> tags = discriminant.tagOfArm.span();
        RELEASE_ASSERT(arms.size() == tags.size());
        StringID name = program().at(discriminant.key).text;
        ValueMerge merge = createMergeOf(arms);

        ValueID member = builder().append(make<&Value::setString>(Opcode::JSONFindMember, { appendSkipPunctuator(cursor) }, name));
        BlockID found = builder().addBlock();
        Vector<std::pair<int64_t, Target>> searchResults;
        searchResults.append({ static_cast<int64_t>(JSONMemberSearch::Found), Target { found, { }, { } } });
        searchResults.append({ static_cast<int64_t>(JSONMemberSearch::Absent), Target { exits.rejected, { }, { } } });
        builder().emitSwitch(appendResultAt(member, 0), WTF::move(searchResults), Target { exits.unknown, { }, { } });

        builder().moveToEndOf(found);
        Vector<BlockID, 4> armBlocks(arms.size(), [&](size_t) { return builder().addBlock(); });
        ValueID tagValue = appendResultAt(member, 1);
        BlockID noTagMatches = builder().addBlock();

        for (size_t index = 0; index < tags.size(); ++index) {
            if (StringID quoted = tags[index].kind == Kind::String ? quotedIfPlain(tags[index].string) : StringID())
                emitJumpIfStartsWith(quoted, tagValue, armBlocks[index]);
        }
        if (std::ranges::contains(tags, Kind::Number, &ResultLiteral::kind)) {
            ResultNode numberNode;
            numberNode.shape = ResultShape::Number;
            numberNode.type = TypeTable::number;
            BlockID isNotNumber = builder().addBlock();
            ValueID number = emitNumber(numberNode, tagValue, { isNotNumber, exits.unknown }).value;
            for (size_t index = 0; index < tags.size(); ++index) {
                if (tags[index].kind != Kind::Number)
                    continue;
                ValueID isEqual = builder().append(Opcode::StrictEq, { number, m_rules.appendLiteral(tags[index]) });
                BlockID nextTest = builder().addBlock();
                builder().emitBranch(isEqual, Target { armBlocks[index], { }, { } }, Target { nextTest, { }, { } });
                builder().moveToEndOf(nextTest);
            }
            builder().emitJump(noTagMatches);
            builder().moveToEndOf(isNotNumber);
        }
        for (size_t index = 0; index < tags.size(); ++index) {
            if (tags[index].kind == Kind::Null || tags[index].kind == Kind::Boolean)
                emitJumpIfStartsWith(inputs().strings.intern(wordOf(tags[index])), tagValue, armBlocks[index]);
        }
        if (std::ranges::contains(tags, Kind::String, &ResultLiteral::kind)) {
            BlockID isString = builder().addBlock(BlockFrequency::Unlikely);
            emitBranchOnByte(tagValue, '"', isString, noTagMatches);
            builder().moveToEndOf(isString);
            ValueID string = emitParse(Opcode::JSONParseString, tagValue, exits.unknown).value;
            Vector<std::pair<StringID, Target>> cases;
            for (size_t index = 0; index < tags.size(); ++index) {
                if (tags[index].kind == Kind::String)
                    cases.append({ tags[index].string, Target { armBlocks[index], { }, { } } });
            }
            builder().emitSwitchString(string, WTF::move(cases), Target { noTagMatches, { }, { } });
        } else
            builder().emitJump(noTagMatches);

        for (size_t index = 0; index < arms.size(); ++index) {
            builder().moveToEndOf(armBlocks[index]);
            jumpTo(merge, emitNode(arms[index], cursor, { noTagMatches, exits.unknown }));
        }

        builder().moveToEndOf(noTagMatches);
        emitRejectionUnless(makeMemberRepeats(name, tagValue), exits);
        return continueAt(merge);
    }

    ValidationRuleEmitter m_rules { *this };
};

} // anonymous namespace

DerivedSignature signatureOfParser(const EmissionInputs& inputs, const ResultNode& node)
{
    return { { inputs.opaqueType(OpaqueKind::Cursor) }, { inputs.outcomeType(), valueTypeOf(inputs, node), inputs.opaqueType(OpaqueKind::Cursor) } };
}

Function buildParser(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    EmissionInputs inputs(derived, { });
    ResultNodeRef ref = nodeOfKey(key);
    ResultNode node = describeNode(inputs.program, ref);
    CreatorRecord::Parser creator { .type = node.type, .layout = node.layout, .form = TypedResultForm::ParsedText };
    if (ref.isValidated()) {
        creator.expectedResult = ref.expectedResult();
        creator.node = ref.part;
    }
    DerivedFunctionHead head = createDerivedFunction(inputs, key.role, creator, signatureOfParser(inputs, node));

    IRBuilder builder(inputs.analysisContext(), head.function, inputs.types);
    builder.moveToEndOf(head.entry);
    JSONParserBuilder parser { { builder, inputs } };
    ValueID cursor = head.parameters[0];
    BlockID rejected = builder.addBlock(BlockFrequency::Rare);
    BlockID unknown = builder.addBlock(BlockFrequency::Rare);

    if (node.isContainer()) {
        builder.emitFailureOfPredictions(Target { unknown, { }, { } });
        parser.emitJumpUnless(builder.append(Opcode::JSONHasStackSpace, { }), unknown);
    }
    JSONParserBuilder::ParsedValue parsed = [&] {
        switch (decideNodeCode(parser.decisionInputs(), ref, NodeUse::ParsedFromText).code) {
        case NodeCode::Interpreted:
            return parser.emitNode(ref, cursor, { rejected, unknown });
        case NodeCode::Compiled:
            return parser.emitNodeBody(ref, cursor, { rejected, unknown });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }();
    parser.emitReturnOf({ parser.appendOutcome(JSONOutcome::Parsed), parsed.value, parsed.next });

    builder.moveToEndOf(rejected);
    parser.emitReturnOf({ parser.appendOutcome(JSONOutcome::Rejected), builder.appendUndefined(), cursor });

    builder.moveToEndOf(unknown);
    parser.emitReturnOf({ parser.appendOutcome(JSONOutcome::Unknown), builder.appendUndefined(), cursor });
    return WTF::move(head.function);
}

Function buildValidatingParser(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    return buildParser(key, derived);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
