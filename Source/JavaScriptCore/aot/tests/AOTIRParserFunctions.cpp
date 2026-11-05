/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRParserInternal.h"

#if ENABLE(AOT)

#include "AOTTypeNarrowing.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return wireFlagsOf(kind); }), "A terminator has no flags word on the wire.");

static constexpr uint32_t largestPositionInBodyText = Wire::none - 1;

enum class BlockAttributeInBodyText : uint8_t {
    Role = 1 << 0,
    Subject = 1 << 1,
    Handler = 1 << 2,
    Frequency = 1 << 3,
};

template<typename WireHead>
static size_t appendHeadToBodyWords(Vector<uint32_t>& code, const WireHead& head)
{
    size_t position = code.size();
    code.append(spanReinterpretCast<const uint32_t>(singleElementSpan(head)));
    return position;
}

template<typename WireHead>
static void overwriteHeadInBodyWords(Vector<uint32_t>& code, size_t position, const WireHead& head)
{
    reinterpretCastSpanStartTo<WireHead>(code.mutableSpan().subspan(position)) = head;
}

static uint32_t numberOfValuesDefinedInBodyText(ValueID lastDefined)
{
    return lastDefined ? lastDefined.index() + 1 : 0;
}

static void expectIDInBodyText(TextParser& parser, IDPrefix prefix, uint32_t number)
{
    if (!parser.atID(prefix) || parser.token().number != number) {
        parser.fail();
        return;
    }
    parser.advance();
}

static uint32_t parseIDWordInBodyText(TextParser& parser, IDPrefix prefix, uint32_t limit)
{
    TextPosition position = parser.token().position;
    uint32_t word = parser.parseIDWord(prefix);
    if (word < limit)
        return word;
    parser.fail(InternalErrorKind::IdOutOfRange, position);
    return 0;
}

static bool isAtTerminatorInBodyText(const TextParser& parser)
{
    return parser.at(TokenKind::Word) && parseEnum<TerminatorKind>(parser.token().view());
}

static uint32_t parseFlagsInBodyText(TextParser& parser, Opcode opcode)
{
    uint32_t flags = 0;
    if (!parser.at(TokenKind::LeftBracket))
        return flags;

    uint32_t wireFlags = wireFlagsOf(opcode);
    if (!wireFlags) {
        parser.fail();
        return flags;
    }

    std::span<const ASCIILiteral> names = flagNamesOf(opcode);
    parser.parseList(TokenKind::LeftBracket, TokenKind::RightBracket, [&] {
        auto name = std::ranges::find_if(names, [&](ASCIILiteral candidate) {
            return parser.atWord(candidate);
        });
        uint32_t flag = 1u << (name - names.begin());
        if (name == names.end() || !(wireFlags & flag) || flags & flag) {
            parser.fail();
            return;
        }
        flags |= flag;
        parser.advance();
    });
    return flags;
}

Wire::Body TextParser::parseBody(const TextItem& item, const Wire::Function& function)
{
    if (!item.hasBody)
        return { };

    expect(TokenKind::LeftBrace);
    expectEndOfLine();

    m_body.scan = { };
    m_body.function = FunctionID(builder().sections().functions.size());
    m_body.block = { };
    m_body.value = { };
    m_body.code.shrink(0);
    if (hasFailed() || !scanBody(item.body, m_body.scan))
        return { };

    seek(item.body);
    for (uint32_t blockIndex = 0; blockIndex < m_body.scan.numberOfBlocks && !hasFailed(); ++blockIndex)
        parseBlock(blockIndex, function);
    expect(TokenKind::RightBrace);
    if (hasFailed())
        return { };
    return { builder().appendCode(code().span()), m_body.scan.numberOfBlocks, numberOfValuesDefinedInBodyText(m_body.value) };
}

void TextParser::parseBlock(uint32_t blockIndex, const Wire::Function& function)
{
    m_body.block = BlockID(blockIndex);
    Wire::BlockHead head;
    size_t headPosition = appendHeadToBodyWords(code(), head);
    if (!blockIndex && m_body.scan.leavesOutEntryHead)
        appendEntryParameters(function, head);
    else
        parseBlockHead(blockIndex, head);

    while (!hasFailed() && !isAtTerminatorInBodyText(*this)) {
        parseValue();
        ++head.valueCount;
    }
    parseTerminator();
    overwriteHeadInBodyWords(code(), headPosition, head);
}

void TextParser::appendEntryParameters(const Wire::Function& function, Wire::BlockHead& head)
{
    auto append = [&](uint32_t typeWord) {
        appendBlockParameter(typeWord);
        ++head.parameterCount;
        ++m_body.scan.numberOfValues;
    };
    auto appendEach = [&]<typename Position>(Wire::Span positions) {
        for (uint32_t index = 0; index < positions.count; ++index)
            append(builder().sections().listOf<Position>()[positions.start + index].type);
    };

    auto flags = OptionSet<FunctionFlags>::fromRaw(static_cast<OptionSet<FunctionFlags>::StorageType>(function.flags));
    for (EntryParameter kind : allMembersOf<EntryParameter>) {
        bool isPresent = flags.containsAll(presentWithOf(kind));
        switch (kind) {
        case EntryParameter::Capture:
            appendEach.template operator()<Wire::DeclaredCapture>(function.captures);
            break;
        case EntryParameter::Parameter:
            appendEach.template operator()<Wire::DeclaredType>(function.parameters);
            break;
        case EntryParameter::This:
            if (isPresent)
                append(function.thisType.type);
            break;
        case EntryParameter::Callee:
            if (isPresent)
                append(typeWordOfAnyFunction());
            break;
        case EntryParameter::NewTarget:
            if (isPresent)
                append(typeWordOfAnything());
            break;
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            if (isPresent)
                fail();
            break;
        }
    }
}

void TextParser::parseBlockHead(uint32_t blockIndex, Wire::BlockHead& head)
{
    expectIDInBodyText(*this, IDPrefix::Block, blockIndex);
    parseList(TokenKind::LeftParenthesis, TokenKind::RightParenthesis, [&] {
        parseBlockParameter();
        ++head.parameterCount;
    });

    OptionSet<BlockAttributeInBodyText> seenAttributes;
    auto acceptAttribute = [&](ASCIILiteral name, BlockAttributeInBodyText attribute) {
        TextPosition position = token().position;
        if (!acceptField(name))
            return false;
        if (seenAttributes.contains(attribute))
            fail(InternalErrorKind::IRTextError, position);
        seenAttributes.add(attribute);
        return true;
    };
    while (atField()) {
        if (acceptAttribute("role"_s, BlockAttributeInBodyText::Role))
            head.role = static_cast<uint32_t>(parseEnumerator<BlockRole>());
        else if (acceptAttribute("subject"_s, BlockAttributeInBodyText::Subject))
            head.subject = parseValueUseWord();
        else if (acceptAttribute("handler"_s, BlockAttributeInBodyText::Handler))
            parseTarget();
        else if (acceptAttribute("frequency"_s, BlockAttributeInBodyText::Frequency))
            head.frequency = static_cast<uint32_t>(parseEnumerator<BlockFrequency>());
        else
            fail();
    }
    head.hasHandler = seenAttributes.contains(BlockAttributeInBodyText::Handler);
    expect(TokenKind::Colon);
    expectEndOfLine();
}

void TextParser::parseBlockParameter()
{
    expectIDInBodyText(*this, IDPrefix::Value, numberOfValuesDefinedInBodyText(m_body.value));
    appendBlockParameter(accept(TokenKind::Colon) ? parseTypeWord() : Wire::none);
}

void TextParser::appendBlockParameter(uint32_t typeWord)
{
    m_body.value = ValueID(numberOfValuesDefinedInBodyText(m_body.value));
    appendHeadToBodyWords(code(), Wire::ParameterHead { typeWord });
}

void TextParser::parseValue()
{
    ValueID id(numberOfValuesDefinedInBodyText(m_body.value));
    m_body.value = id;

    bool hasResult = atID(IDPrefix::Value);
    if (hasResult) {
        expectIDInBodyText(*this, IDPrefix::Value, id.index());
        expect(TokenKind::Equal);
    }

    TextPosition opcodePosition = token().position;
    Opcode opcode = parseEnumerator<Opcode>();
    if (hasResult != (resultRuleOf(opcode) != ResultRule::None))
        fail(InternalErrorKind::IRTextError, opcodePosition);

    Wire::ValueHead head { static_cast<uint32_t>(opcode), Wire::none };
    size_t headPosition = appendHeadToBodyWords(code(), head);
    bool hasFlagsWord = wireFlagsOf(opcode);
    size_t flagsPosition = code().size();
    if (hasFlagsWord)
        code().append(0u);
    parseFields(*this, opcode);

    uint32_t flags = parseFlagsInBodyText(*this, opcode);
    if (hasFlagsWord)
        code()[flagsPosition] = flags;

    if (accept(TokenKind::At)) {
        head.position = parseUInt32(largestPositionInBodyText);
        if (acceptWord("tail"_s))
            head.opcode |= Wire::StandsInTailPosition;
        acceptWord("constructs"_s);
        if (accept(TokenKind::At)) {
            head.opcode |= Wire::WrittenPositionFollows;
            code().append(parseUInt32(largestPositionInBodyText));
            if (acceptWord("callers"_s))
                head.opcode |= Wire::WrittenPositionIsForCallers;
        }
    }
    overwriteHeadInBodyWords(code(), headPosition, head);
    expectEndOfLine();
}

void TextParser::parseTerminator()
{
    TerminatorKind kind = parseEnumerator<TerminatorKind>();
    size_t headPosition = appendHeadToBodyWords(code(), Wire::TerminatorHead { });
    parseFields(*this, kind);

    Vector<Token, 4> targetNames;
    if (accept(TokenKind::Arrow)) {
        do {
            targetNames.append(token());
            expect(TokenKind::Word);
            parseTarget();
        } while (accept(TokenKind::Comma));
    }
    auto numberOfTargets = static_cast<uint32_t>(targetNames.size());
    for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
        const TargetInfo* info = infoOfTarget(kind, targetIndex, numberOfTargets);
        if (!info || !targetNames[targetIndex].isWord(info->name))
            fail(InternalErrorKind::IRTextError, targetNames[targetIndex].position);
    }

    uint32_t position = accept(TokenKind::At) ? parseUInt32(largestPositionInBodyText) : Wire::none;
    overwriteHeadInBodyWords(code(), headPosition, Wire::TerminatorHead { static_cast<uint32_t>(kind), position, numberOfTargets });
    expectEndOfLine();
}

void TextParser::parseTarget()
{
    Wire::TargetHead head;
    head.block = parseBlockUseWord();
    size_t headPosition = appendHeadToBodyWords(code(), head);

    expect(TokenKind::LeftParenthesis);
    if (atID(IDPrefix::Value)) {
        do {
            code().append(parseValueUseWord());
            ++head.argumentCount;
        } while (accept(TokenKind::Comma));
    }
    if (accept(TokenKind::Bar)) {
        do {
            code().append(parseUInt32());
            ++head.proofCount;
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RightParenthesis);
    overwriteHeadInBodyWords(code(), headPosition, head);
}

uint32_t TextParser::parseBlockUseWord()
{
    return parseIDWordInBodyText(*this, IDPrefix::Block, m_body.scan.numberOfBlocks);
}

uint32_t TextParser::parseValueUseWord()
{
    return parseIDWordInBodyText(*this, IDPrefix::Value, m_body.scan.numberOfValues);
}

uint32_t TextParser::parseOptionalValueUseWord()
{
    if (accept(TokenKind::Minus))
        return Wire::none;
    return parseValueUseWord();
}

bool TextParser::atOperand() const
{
    return atID(IDPrefix::Value) || (at(TokenKind::Minus) && nextToken().kind == TokenKind::Comma);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
