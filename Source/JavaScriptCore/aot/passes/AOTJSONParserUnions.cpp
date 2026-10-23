/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParserBuilder.h"

#if ENABLE(AOT)

#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr JSONValueClass valueClasses[] {
    JSONValueClass::String,
    JSONValueClass::Number,
    JSONValueClass::Boolean,
    JSONValueClass::Null,
    JSONValueClass::Array,
    JSONValueClass::Object,
};

static ASCIILiteral firstBytesOf(JSONValueClass valueClass)
{
    switch (valueClass) {
    case JSONValueClass::Null:
        return "n"_s;
    case JSONValueClass::Boolean:
        return "tf"_s;
    case JSONValueClass::Number:
        return "-0123456789"_s;
    case JSONValueClass::String:
        return "\""_s;
    case JSONValueClass::Array:
        return "["_s;
    case JSONValueClass::Object:
        return "{"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct ClassGroup {
    Vector<ResultNodeRef, 4> arms;
    OptionSet<JSONValueClass> classes;
    BlockID block;
};

struct ValueMerge {
    BlockID block;
    TypeID valueType;
    ParsedValue merged;
};

struct TagTests {
    std::span<const ResultLiteral> tags;
    Vector<BlockID, 4> armBlocks;
    ValueID tagValue;
    BlockID noTagMatches;
};

} // anonymous namespace

static Vector<ClassGroup, 6> groupArmsByClass(const Program& program, std::span<const ResultNodeRef> arms)
{
    Vector<OptionSet<JSONValueClass>, 4> classesOfArm(arms.size(), [&](size_t index) {
        return valueClassesOf(program, arms[index]);
    });

    Vector<ClassGroup, 6> groups;
    for (JSONValueClass valueClass : valueClasses) {
        Vector<ResultNodeRef, 4> candidates;
        for (size_t index = 0; index < arms.size(); ++index) {
            if (classesOfArm[index].contains(valueClass))
                candidates.append(arms[index]);
        }
        if (candidates.isEmpty())
            continue;

        size_t position = groups.findIf([&](const ClassGroup& group) {
            return group.arms == candidates;
        });
        if (position == notFound) {
            position = groups.size();
            groups.append(ClassGroup { WTF::move(candidates), { }, { } });
        }
        groups[position].classes.add(valueClass);
    }
    return groups;
}

static std::optional<Vector<StringID, 8>> unconstrainedStringLiteralsOf(const Program& program, std::span<const ResultNodeRef> arms)
{
    Vector<StringID, 8> literals;
    for (ResultNodeRef arm : arms) {
        ResultNode node = describeNode(program, arm);
        bool isTested = std::ranges::any_of(node.rules, [](const ValidationRule& rule) {
            return subjectOf(rule) != RuleSubject::Nothing;
        });
        if (node.shape != ResultShape::StringLiteral || isTested)
            return std::nullopt;
        literals.appendIfNotContains(node.literal.string);
    }
    return literals;
}

static ValueMerge createValueMerge(JSONParserBuilder& parser, std::span<const ResultNodeRef> arms)
{
    TypeArena& types = parser.inputs().types;
    TypeID valueType = TypeTable::never;
    for (ResultNodeRef arm : arms)
        valueType = types.unionOf(valueType, JSONParserBuilder::signatureOf(parser, describeNode(parser.program(), arm)).results[1]);

    IRBuilder& builder = parser.builder();
    BlockID block = builder.addBlock();
    ValueID value = builder.addParameter(block, valueType);
    return { block, valueType, { value, builder.addParameter(block, parser.cursorType()) } };
}

static void emitSwitchOnFirstByte(IRBuilder& builder, Vector<ClassGroup, 6>& groups, ValueID cursor, BlockID otherwise)
{
    Vector<std::pair<int64_t, Target>> cases;
    for (ClassGroup& group : groups) {
        group.block = builder.addBlock();
        for (JSONValueClass valueClass : group.classes) {
            for (char byte : firstBytesOf(valueClass).span())
                cases.append({ int64_t { byte }, Target { group.block, { }, { } } });
        }
    }
    builder.emitSwitch(builder.append(Opcode::JSONGetByte, { cursor }), WTF::move(cases), Target { otherwise, { }, { } });
}

static void emitSwitchOnString(IRBuilder& builder, ValueID string, std::span<const StringID> strings, std::span<const BlockID> targets, BlockID otherwise)
{
    Vector<std::pair<StringID, Target>> cases;
    for (size_t index = 0; index < strings.size(); ++index)
        cases.append({ strings[index], Target { targets[index], { }, { } } });
    builder.emitSwitchString(string, WTF::move(cases), Target { otherwise, { }, { } });
}

static bool hasTagOfKind(const TagTests& tests, ResultLiteral::Kind kind)
{
    return std::ranges::contains(tests.tags, kind, &ResultLiteral::kind);
}

static void emitJumpIfStartsWith(JSONParserBuilder& parser, StringView text, ValueID cursor, BlockID target)
{
    IRBuilder& builder = parser.builder();
    Value test = IRBuilder::make(Opcode::JSONStartsWith, { cursor });
    test.setString(parser.inputs().strings.intern(text));
    BlockID otherwise = builder.addBlock();
    builder.emitBranch(builder.append(WTF::move(test)), Target { target, { }, { } }, Target { otherwise, { }, { } });
    builder.moveToEndOf(otherwise);
}

static void emitPlainStringTagTests(JSONParserBuilder& parser, const TagTests& tests)
{
    for (size_t index = 0; index < tests.tags.size(); ++index) {
        const ResultLiteral& tag = tests.tags[index];
        if (tag.kind != ResultLiteral::Kind::String)
            continue;
        if (std::optional<Vector<uint8_t>> quoted = quotedIfPlain(parser.program().string(tag.string)))
            emitJumpIfStartsWith(parser, byteCast<Latin1Character>(quoted->span()), tests.tagValue, tests.armBlocks[index]);
    }
}

static void emitSwitchOnStringTags(IRBuilder& builder, ValueID string, const TagTests& tests)
{
    Vector<StringID, 4> strings;
    Vector<BlockID, 4> targets;
    for (size_t index = 0; index < tests.tags.size(); ++index) {
        if (tests.tags[index].kind != ResultLiteral::Kind::String)
            continue;
        strings.append(tests.tags[index].string);
        targets.append(tests.armBlocks[index]);
    }
    emitSwitchOnString(builder, string, strings.span(), targets.span(), tests.noTagMatches);
}

static void emitNumberTagTests(IRBuilder& builder, ValidationRuleEmitter& rules, const TagTests& tests, ValueID number)
{
    for (size_t index = 0; index < tests.tags.size(); ++index) {
        if (tests.tags[index].kind != ResultLiteral::Kind::Number)
            continue;
        ValueID isEqual = builder.append(Opcode::StrictEq, { number, rules.appendLiteral(tests.tags[index]) });
        BlockID nextTest = builder.addBlock();
        builder.emitBranch(isEqual, Target { tests.armBlocks[index], { }, { } }, Target { nextTest, { }, { } });
        builder.moveToEndOf(nextTest);
    }
    builder.emitJump(tests.noTagMatches);
}

static void emitWordTagTests(JSONParserBuilder& parser, const TagTests& tests)
{
    for (size_t index = 0; index < tests.tags.size(); ++index) {
        const ResultLiteral& tag = tests.tags[index];
        switch (tag.kind) {
        case ResultLiteral::Kind::Null:
            emitJumpIfStartsWith(parser, "null"_s, tests.tagValue, tests.armBlocks[index]);
            break;
        case ResultLiteral::Kind::Boolean:
            emitJumpIfStartsWith(parser, tag.boolean ? "true"_s : "false"_s, tests.tagValue, tests.armBlocks[index]);
            break;
        case ResultLiteral::Kind::Number:
        case ResultLiteral::Kind::String:
            break;
        }
    }
}

ParsedValue JSONParserBuilder::emitUnion(ResultNodeRef, const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    Vector<ClassGroup, 6> groups = groupArmsByClass(program(), node.children.span());
    if (groups.isEmpty())
        return emitArmsInOrder({ }, cursor, exits);
    if (groups.size() == 1 && groups[0].arms.size() == 1) {
        ParsedValue parsed = emitNode(groups[0].arms[0], cursor, exits);
        m_rules.emitRules(node, parsed.value, exits.rejected);
        return parsed;
    }

    ValueMerge merge = createValueMerge(*this, node.children.span());
    auto emitJumpWithLiteral = [&](StringID literal, ValueID next) {
        builder().emitJump(merge.block, { emitTypeTest(appendString(literal), merge.valueType, exits.unknown), next });
    };
    auto emitStringLiteralArms = [&](std::span<const StringID> literals) {
        for (StringID literal : literals) {
            std::optional<Vector<uint8_t>> quoted = quotedIfPlain(program().string(literal));
            if (!quoted)
                continue;
            BlockID nextLiteral = builder().addBlock();
            emitJumpWithLiteral(literal, emitExpectText(byteCast<Latin1Character>(quoted->span()), cursor, nextLiteral, false));
            builder().moveToEndOf(nextLiteral);
        }

        ParsedValue text = emitParsedOrExit(builder().append(Opcode::JSONParseString, { cursor }), exits.unknown);
        Vector<BlockID, 8> matches(literals.size(), [&](size_t) {
            return builder().addBlock();
        });
        emitSwitchOnString(builder(), text.value, literals, matches.span(), exits.rejected);
        for (size_t index = 0; index < literals.size(); ++index) {
            builder().moveToEndOf(matches[index]);
            emitJumpWithLiteral(literals[index], text.next);
        }
    };

    emitSwitchOnFirstByte(builder(), groups, cursor, exits.rejected);
    for (const ClassGroup& group : groups) {
        builder().moveToEndOf(group.block);
        std::optional<Vector<StringID, 8>> literals;
        if (group.arms.size() > 1)
            literals = unconstrainedStringLiteralsOf(program(), group.arms.span());
        if (literals) {
            emitStringLiteralArms(literals->span());
            continue;
        }
        bool mayBeObject = group.classes.contains(JSONValueClass::Object);
        ParsedValue parsed = mayBeObject ? emitObjectArms(group.arms.span(), cursor, exits) : emitArmsInOrder(group.arms.span(), cursor, exits);
        builder().emitJump(merge.block, { parsed.value, parsed.next });
    }

    builder().moveToEndOf(merge.block);
    m_rules.emitRules(node, merge.merged.value, exits.rejected);
    return merge.merged;
}

ParsedValue JSONParserBuilder::emitArmsInOrder(std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits& exits)
{
    if (arms.empty()) {
        builder().emitJump(exits.rejected);
        builder().moveToEndOf(builder().addBlock(BlockFrequency::Rare));
        return { builder().appendUndefined(), cursor };
    }
    if (arms.size() == 1)
        return emitNode(arms[0], cursor, exits);

    ValueMerge merge = createValueMerge(*this, arms);
    for (ResultNodeRef arm : arms.first(arms.size() - 1)) {
        BlockID nextArm = builder().addBlock();
        ParsedValue parsed = emitNode(arm, cursor, { nextArm, exits.unknown });
        builder().emitJump(merge.block, { parsed.value, parsed.next });
        builder().moveToEndOf(nextArm);
    }
    ParsedValue parsed = emitNode(arms.back(), cursor, exits);
    builder().emitJump(merge.block, { parsed.value, parsed.next });

    builder().moveToEndOf(merge.block);
    return merge.merged;
}

ParsedValue JSONParserBuilder::emitObjectArms(std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits& exits)
{
    std::optional<TaggedArms> split = findTaggedArms(program(), arms);
    if (!split || program().at(split->discriminant.key).kind != PropertyKeyKind::Text)
        return emitArmsInOrder(arms, cursor, exits);
    if (split->untagged.isEmpty())
        return emitArmsByDiscriminant(split->discriminant, split->tagged.span(), cursor, exits);

    ValueMerge merge = createValueMerge(*this, arms);
    BlockID untagged = builder().addBlock();
    ParsedValue parsed = emitArmsByDiscriminant(split->discriminant, split->tagged.span(), cursor, { untagged, exits.unknown });
    builder().emitJump(merge.block, { parsed.value, parsed.next });

    builder().moveToEndOf(untagged);
    parsed = emitArmsInOrder(split->untagged.span(), cursor, exits);
    builder().emitJump(merge.block, { parsed.value, parsed.next });

    builder().moveToEndOf(merge.block);
    return merge.merged;
}

ParsedValue JSONParserBuilder::emitArmsByDiscriminant(const ResultDiscriminant& discriminant, std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits& exits)
{
    RELEASE_ASSERT(arms.size() == discriminant.tagOfArm.size());
    StringID name = program().at(discriminant.key).text;
    ValueMerge merge = createValueMerge(*this, arms);

    Value search = IRBuilder::make(Opcode::JSONFindMember, { appendSkipPunctuator(cursor) });
    search.setString(name);
    ValueID member = builder().append(WTF::move(search));
    BlockID found = builder().addBlock();
    Vector<std::pair<int64_t, Target>> searchResults;
    searchResults.append({ static_cast<int64_t>(JSONMemberSearch::Found), Target { found, { }, { } } });
    searchResults.append({ static_cast<int64_t>(JSONMemberSearch::Absent), Target { exits.rejected, { }, { } } });
    builder().emitSwitch(appendResultAt(member, 0), WTF::move(searchResults), Target { exits.unknown, { }, { } });

    builder().moveToEndOf(found);
    Vector<BlockID, 4> armBlocks(arms.size(), [&](size_t) {
        return builder().addBlock();
    });
    TagTests tests { discriminant.tagOfArm.span(), WTF::move(armBlocks), appendResultAt(member, 1), builder().addBlock() };

    emitPlainStringTagTests(*this, tests);
    if (hasTagOfKind(tests, ResultLiteral::Kind::Number)) {
        ResultNode number;
        number.shape = ResultShape::Number;
        number.type = TypeTable::number;
        BlockID isNotNumber = builder().addBlock();
        emitNumberTagTests(builder(), m_rules, tests, emitNumber(number, tests.tagValue, { isNotNumber, exits.unknown }).value);
        builder().moveToEndOf(isNotNumber);
    }
    emitWordTagTests(*this, tests);
    if (hasTagOfKind(tests, ResultLiteral::Kind::String)) {
        BlockID isString = builder().addBlock(BlockFrequency::Unlikely);
        emitBranchOnByte(tests.tagValue, '"', isString, tests.noTagMatches);
        builder().moveToEndOf(isString);
        emitSwitchOnStringTags(builder(), emitParsedOrExit(builder().append(Opcode::JSONParseString, { tests.tagValue }), exits.unknown).value, tests);
    } else
        builder().emitJump(tests.noTagMatches);

    for (size_t index = 0; index < arms.size(); ++index) {
        builder().moveToEndOf(tests.armBlocks[index]);
        ParsedValue parsed = emitNode(arms[index], cursor, { tests.noTagMatches, exits.unknown });
        builder().emitJump(merge.block, { parsed.value, parsed.next });
    }

    builder().moveToEndOf(tests.noTagMatches);
    emitRejectionOfMember(name, tests.tagValue, exits);

    builder().moveToEndOf(merge.block);
    return merge.merged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
