/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParserBuilder.h"

#if ENABLE(AOT)

#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTValidatorBuilder.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isPlain(uint8_t byte)
{
    return byte >= 0x20 && byte <= 0x7E && byte != '"' && byte != '\\';
}

std::optional<Vector<uint8_t>> quotedIfPlain(std::span<const uint8_t> text)
{
    if (!std::ranges::all_of(text, isPlain))
        return std::nullopt;
    Vector<uint8_t> quoted;
    quoted.reserveInitialCapacity(text.size() + 2);
    quoted.append('"');
    quoted.append(text);
    quoted.append('"');
    return quoted;
}

static TypeID valueTypeOf(Emission& emission, const ResultNode& node)
{
    return emission.inputs().types.intern(emission.program().at(node.type).withoutProofs().withPrimitives(Primitive::Undefined));
}

static Type numberPartOf(const Type& type)
{
    return type.withoutObjects().withoutPrimitives(static_cast<uint16_t>(Primitive::Defined & ~Primitive::Number));
}

static bool fastPathCanMiss(ResultShape shape)
{
    switch (shape) {
    case ResultShape::Number:
    case ResultShape::NumberLiteral:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        return true;
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::Null:
    case ResultShape::AnyObject:
    case ResultShape::String:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
    case ResultShape::BooleanLiteral:
    case ResultShape::StringLiteral:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ParsedValue emitParseNumber(Emission& emission, Opcode opcode, ValueID cursor, const ParseExits& exits)
{
    IRBuilder& builder = emission.builder();
    ValueID results = builder.append(opcode, { cursor });
    IRBuilder::BranchBlocks branch = builder.emitBranch(emission.appendResultAt(results, 0), BlockFrequency::Normal, BlockFrequency::Rare);

    builder.moveToEndOf(branch.notTaken);
    if (exits.rejected == exits.unknown)
        builder.emitJump(exits.unknown);
    else {
        Vector<std::pair<int64_t, Target>> numberStarts;
        numberStarts.append({ int64_t { '-' }, Target { exits.unknown, { }, { } } });
        for (int64_t digit = '0'; digit <= '9'; ++digit)
            numberStarts.append({ digit, Target { exits.unknown, { }, { } } });
        builder.emitSwitch(builder.append(Opcode::JSONGetByte, { cursor }), WTF::move(numberStarts), Target { exits.rejected, { }, { } });
    }

    builder.moveToEndOf(branch.taken);
    return { emission.appendResultAt(results, 1), emission.appendResultAt(results, 2) };
}

JSONParserBuilder::JSONParserBuilder(IRBuilder& builder, const EmissionInputs& inputs)
    : Emission(builder, inputs)
    , m_rules(*this)
{
}

DerivedSignature JSONParserBuilder::signatureOf(Emission& emission, const ResultNode& node)
{
    return { { emission.cursorType() }, { emission.outcomeType(), valueTypeOf(emission, node), emission.cursorType() } };
}

ParsedValue JSONParserBuilder::emitNode(ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
{
    ResultNode node = describeNode(program(), ref);
    NodeDecision decision = decideNodeCode(decisionInputs(), ref, NodeUse::ParsedFromText);
    switch (decision.code) {
    case NodeCode::Interpreted:
        addInterpretedNodeDecline(decisionInputs(), firstLayoutOf(program(), ref), decision.cause);
        return emitInterpretedNode(ref, node, cursor, exits);
    case NodeCode::Compiled:
        break;
    }
    if (node.isContainer())
        return emitCallOfParser(ref, node, cursor, exits);
    return emitNodeBody(ref, cursor, exits);
}

ParsedValue JSONParserBuilder::emitNodeOrGenericValue(ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
{
    ResultNode node = describeNode(program(), ref);
    bool canMiss = ref.isValidated() ? fastPathCanMiss(node.shape) : node.shape != ResultShape::Unknown;
    if (!canMiss || decideNodeCode(decisionInputs(), ref, NodeUse::ParsedFromText).code == NodeCode::Interpreted)
        return emitNode(ref, cursor, exits);

    BlockID again = builder().addBlock(BlockFrequency::Rare);
    BlockID merge = builder().addBlock();
    ParsedValue merged { builder().addParameter(merge, valueTypeOf(*this, node)), builder().addParameter(merge, cursorType()) };

    ParsedValue typed = emitNode(ref, cursor, { ref.isValidated() ? exits.rejected : again, again });
    builder().emitJump(merge, { typed.value, typed.next });

    builder().moveToEndOf(again);
    ParsedValue generic = emitGenericValueFor(ref, node, cursor, exits);
    builder().emitJump(merge, { generic.value, generic.next });

    builder().moveToEndOf(merge);
    return merged;
}

ParsedValue JSONParserBuilder::emitNodeBody(ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
{
    ResultNode node = describeNode(program(), ref);
    auto narrowToValueType = [&](ParsedValue parsed) -> ParsedValue {
        return { emitTypeTest(parsed.value, valueTypeOf(*this, node), exits.unknown), parsed.next };
    };

    ParsedValue parsed;
    switch (node.shape) {
    case ResultShape::Unknown:
        parsed = emitGenericValue(cursor, exits.unknown);
        break;
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::BigInt:
        builder().emitJump(exits.rejected);
        builder().moveToEndOf(builder().addBlock(BlockFrequency::Rare));
        return { builder().appendUndefined(), cursor };
    case ResultShape::AnyObject: {
        BlockID isOpened = builder().addBlock();
        BlockID isNotBrace = builder().addBlock();
        emitBranchOnByte(cursor, '{', isOpened, isNotBrace);
        builder().moveToEndOf(isNotBrace);
        emitBranchOnByte(cursor, '[', isOpened, exits.rejected);
        builder().moveToEndOf(isOpened);
        parsed = emitGenericValue(cursor, exits.unknown);
        break;
    }
    case ResultShape::Null:
    case ResultShape::String:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        parsed = emitScalar(node, cursor, exits);
        break;
    case ResultShape::Tuple:
        parsed = emitTuple(node, cursor, exits);
        break;
    case ResultShape::Array:
        return narrowToValueType(emitArray(node, cursor, exits));
    case ResultShape::Dictionary:
        return narrowToValueType(emitDictionary(node, cursor, exits));
    case ResultShape::Object:
        return narrowToValueType(emitObject(node, cursor, exits));
    case ResultShape::Union:
        return narrowToValueType(emitUnion(ref, node, cursor, exits));
    }
    m_rules.emitRules(node, parsed.value, exits.rejected);
    return narrowToValueType(parsed);
}

ParsedValue JSONParserBuilder::emitCallOfParser(ResultNodeRef ref, const ResultNode&, ValueID cursor, const ParseExits& exits)
{
    bool hasOwnFunction = ref.isValidated() || !ref.part;
    if (!hasOwnFunction)
        return emitNodeBody(ref, cursor, exits);

    ValueID results = appendCallOf(inputs().functions.ensure(parserKeyOf(ref)), { cursor });
    BlockID parsed = builder().addBlock();
    emitBranchOnOutcome(appendResultAt(results, 0), parsed, exits.rejected, exits.unknown);

    builder().moveToEndOf(parsed);
    return { appendResultAt(results, 1), appendResultAt(results, 2) };
}

ParsedValue JSONParserBuilder::emitInterpretedNode(ResultNodeRef ref, const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    return emitGenericValueFor(ref, node, cursor, exits);
}

ParsedValue JSONParserBuilder::emitGenericValue(ValueID cursor, BlockID unknown)
{
    return emitParsedOrExit(builder().append(Opcode::JSONParseValue, { cursor }), unknown);
}

ParsedValue JSONParserBuilder::emitGenericValueFor(ResultNodeRef ref, const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    ParsedValue generic = emitGenericValue(cursor, exits.unknown);
    if (!ref.isValidated())
        return { emitTypeTest(generic.value, inputs().types.intern(program().at(node.type).withoutProofs()), exits.unknown), generic.next };

    ValueID value = ValidatorBuilder(builder(), inputs()).emitNode(ref, generic.value, { exits.rejected, exits.unknown });
    return { emitTypeTest(value, valueTypeOf(*this, node), exits.unknown), generic.next };
}

ParsedValue JSONParserBuilder::emitScalar(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    switch (node.shape) {
    case ResultShape::Boolean: {
        BlockID merge = builder().addBlock();
        ParsedValue merged { builder().addParameter(merge, TypeTable::boolean), builder().addParameter(merge, cursorType()) };
        BlockID isNotTrue = builder().addBlock();

        ValueID afterTrue = emitExpectText("true"_s, cursor, isNotTrue, false);
        builder().emitJump(merge, { builder().appendBoolean(true), afterTrue });

        builder().moveToEndOf(isNotTrue);
        ValueID afterFalse = emitExpectText("false"_s, cursor, exits.rejected, false);
        builder().emitJump(merge, { builder().appendBoolean(false), afterFalse });

        builder().moveToEndOf(merge);
        return merged;
    }
    case ResultShape::Number:
        return emitNumber(node, cursor, exits);
    case ResultShape::String:
        return emitString(node, cursor, exits);
    case ResultShape::Null:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        return emitLiteral(node, cursor, exits);
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::AnyObject:
    case ResultShape::BigInt:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ParsedValue JSONParserBuilder::emitNumber(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    Type numbers = numberPartOf(program().at(node.type));
    bool isInteger = hasIntegerRule(node) || numbers.isInteger();
    ParsedValue parsed = emitParseNumber(*this, isInteger ? Opcode::JSONParseInteger : Opcode::JSONParseNumber, cursor, exits);
    return { emitTypeTest(parsed.value, inputs().types.intern(numbers), exits.unknown), parsed.next };
}

ParsedValue JSONParserBuilder::emitString(const ResultNode&, ValueID cursor, const ParseExits& exits)
{
    emitRequireByte(cursor, '"', exits.rejected);
    return emitParsedOrExit(builder().append(Opcode::JSONParseString, { cursor }), exits.unknown);
}

ParsedValue JSONParserBuilder::emitLiteral(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    ValueID constant = m_rules.appendLiteral(node.literal);
    switch (node.literal.kind) {
    case ResultLiteral::Kind::Null:
        return { constant, emitExpectText("null"_s, cursor, exits.rejected, false) };
    case ResultLiteral::Kind::Boolean:
        return { constant, emitExpectText(node.literal.boolean ? "true"_s : "false"_s, cursor, exits.rejected, false) };
    case ResultLiteral::Kind::Number: {
        ParsedValue parsed = emitParseNumber(*this, Opcode::JSONParseNumber, cursor, exits);
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
        if (std::optional<Vector<uint8_t>> quoted = quotedIfPlain(program().string(node.literal.string))) {
            BlockID escapedSpelling = builder().addBlock(BlockFrequency::Rare);
            ValueID afterText = emitExpectText(byteCast<Latin1Character>(quoted->span()), cursor, escapedSpelling, true);
            builder().emitJump(merge, { afterText });
            builder().moveToEndOf(escapedSpelling);
        }

        ParsedValue parsed = emitString(node, cursor, exits);
        emitJumpUnless(builder().append(Opcode::StrictEq, { parsed.value, constant }), exits.rejected);
        builder().emitJump(merge, { parsed.next });

        builder().moveToEndOf(merge);
        return { constant, next };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ParsedValue JSONParserBuilder::emitParsedOrExit(ValueID results, BlockID whenNotParsed)
{
    emitJumpUnless(appendResultAt(results, 0), whenNotParsed);
    return { appendResultAt(results, 1), appendResultAt(results, 2) };
}

ValueID JSONParserBuilder::emitExpectText(StringView text, ValueID cursor, BlockID otherwise, bool isLikely)
{
    StringID string = inputs().strings.intern(text);
    Value test = IRBuilder::make(Opcode::JSONStartsWith, { cursor });
    test.setString(string);
    test.setIsLikely(isLikely);
    IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(builder().append(WTF::move(test)), 0, BlockFrequency::Normal, isLikely ? BlockFrequency::Unlikely : BlockFrequency::Normal);

    builder().moveToEndOf(branch.notTaken);
    builder().emitJump(otherwise);

    builder().moveToEndOf(branch.taken);
    Value advance = IRBuilder::make(Opcode::JSONAdvance, { cursor, branch.provenOnTaken });
    advance.setString(string);
    return builder().append(WTF::move(advance));
}

void JSONParserBuilder::emitRequireByte(ValueID cursor, uint8_t byte, BlockID otherwise)
{
    BlockID matched = builder().addBlock();
    emitBranchOnByte(cursor, byte, matched, otherwise);
    builder().moveToEndOf(matched);
}

void JSONParserBuilder::emitBranchOnByte(ValueID cursor, uint8_t byte, BlockID whenEqual, BlockID otherwise)
{
    ValueID actual = builder().append(Opcode::JSONGetByte, { cursor });
    ValueID isEqual = builder().append(Opcode::StrictEq, { actual, builder().appendInteger(byte) });
    builder().emitBranch(isEqual, Target { whenEqual, { }, { } }, Target { otherwise, { }, { } });
}

ValueID JSONParserBuilder::appendSkipPunctuator(ValueID cursor)
{
    return appendSkipWhitespace(builder().append(Opcode::JSONSkipByte, { cursor }));
}

ValueID JSONParserBuilder::appendSkipWhitespace(ValueID cursor)
{
    return builder().append(Opcode::JSONSkipWhitespace, { cursor });
}

void JSONParserBuilder::emitRequireStackSpace(BlockID unknown)
{
    emitJumpUnless(builder().append(Opcode::JSONHasStackSpace, { }), unknown);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
