/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRParserInternal.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/OptionSet.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

enum class BlockAttribute : uint8_t {
    Role = 1 << 0,
    Subject = 1 << 1,
    Frequency = 1 << 2,
    Handler = 1 << 3,
};

template<typename WireRecord>
static size_t appendRecord(Vector<uint32_t>& code, const WireRecord& record)
{
    size_t position = code.size();
    code.append(spanReinterpretCast<const uint32_t>(singleElementSpan(record)));
    return position;
}

template<typename WireRecord>
static void overwriteRecord(Vector<uint32_t>& code, size_t position, const WireRecord& record)
{
    reinterpretCastSpanStartTo<WireRecord>(code.mutableSpan().subspan(position)) = record;
}

static uint32_t numberOfDefinedValues(ValueID lastDefined)
{
    return lastDefined ? lastDefined.index() + 1 : 0;
}

static void expectID(TextParser& parser, IDPrefix prefix, uint32_t number)
{
    if (!parser.atID(prefix) || parser.token().number != number) {
        parser.fail();
        return;
    }
    parser.advance();
}

static uint32_t parseIDWordBelow(TextParser& parser, IDPrefix prefix, uint32_t limit)
{
    TextPosition position = parser.token().position;
    uint32_t word = parser.parseIDWord(prefix);
    if (word < limit)
        return word;
    parser.fail(InternalErrorKind::IdOutOfRange, position);
    return 0;
}

static bool hasAnyField(const ValueEngineFields& fields)
{
    return fields.typeWord != Wire::none || fields.expectedWord != Wire::none || fields.representation;
}

static bool isAtTerminator(const TextParser& parser)
{
    return parser.at(TokenKind::Word) && parseEnum<TerminatorKind>(parser.token().view());
}

static uint32_t parseEntryFlags(TextParser& parser, std::span<const ASCIILiteral> names)
{
    uint32_t flags = 0;
    if (!parser.at(TokenKind::LeftBracket))
        return flags;
    if (names.empty()) {
        parser.fail();
        return flags;
    }

    parser.parseList(TokenKind::LeftBracket, TokenKind::RightBracket, [&] {
        auto name = std::ranges::find_if(names, [&](ASCIILiteral candidate) {
            return parser.atWord(candidate);
        });
        uint32_t flag = 1u << (name - names.begin());
        if (name == names.end() || flags & flag) {
            parser.fail();
            return;
        }
        flags |= flag;
        parser.advance();
    });
    return flags;
}

static Origin parseOrigin(TextParser& parser)
{
    Origin origin { Wire::none, 0 };
    if (!parser.accept(TokenKind::At))
        return origin;
    origin.position = parser.parseUInt32(Wire::none - 1);
    if (parser.accept(TokenKind::Colon))
        origin.inlineFrame = parser.parseUInt32();
    if (parser.token().isWord("tail"_s)) {
        origin.isTailCall = true;
        parser.advance();
    }
    return origin;
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
    return { builder().appendCode(code().span()), m_body.scan.numberOfBlocks, numberOfDefinedValues(m_body.value) };
}

void TextParser::parseBlock(uint32_t blockIndex, const Wire::Function& function)
{
    m_body.block = BlockID(blockIndex);
    Wire::BlockHead head;
    size_t headPosition = appendRecord(code(), head);
    if (!blockIndex && m_body.scan.leavesOutEntryHead)
        appendEntryParameters(function, head);
    else
        parseBlockHead(blockIndex, head);

    while (!hasFailed() && !isAtTerminator(*this)) {
        parseValue();
        ++head.valueCount;
    }
    parseTerminator();
    overwriteRecord(code(), headPosition, head);
}

void TextParser::appendEntryParameters(const Wire::Function& function, Wire::BlockHead& head)
{
    auto append = [&](uint32_t typeWord) {
        appendBlockParameter(ValueID(numberOfDefinedValues(m_body.value)), typeWord, { });
        ++head.parameterCount;
        ++m_body.scan.numberOfValues;
    };
    auto appendEach = [&](Wire::Span declaredTypes) {
        for (uint32_t index = 0; index < declaredTypes.count; ++index)
            append(builder().sections().listOf<Wire::DeclaredType>()[declaredTypes.start + index].type);
    };

    auto flags = OptionSet<FunctionFlags>::fromRaw(static_cast<OptionSet<FunctionFlags>::StorageType>(function.flags));
    for (EntryParameter kind : allMembersOf<EntryParameter>) {
        bool isPresent = flags.containsAll(presentWithOf(kind));
        switch (kind) {
        case EntryParameter::Capture:
            appendEach(function.captures);
            break;
        case EntryParameter::Parameter:
            appendEach(function.parameters);
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
    expectID(*this, IDPrefix::Block, blockIndex);
    parseList(TokenKind::LeftParenthesis, TokenKind::RightParenthesis, [&] {
        parseBlockParameter();
        ++head.parameterCount;
    });

    OptionSet<BlockAttribute> seenAttributes;
    auto acceptAttribute = [&](ASCIILiteral name, BlockAttribute attribute) {
        TextPosition position = token().position;
        if (!acceptField(name))
            return false;
        if (seenAttributes.contains(attribute))
            fail(InternalErrorKind::IRTextError, position);
        seenAttributes.add(attribute);
        return true;
    };
    while (atField()) {
        if (acceptAttribute("role"_s, BlockAttribute::Role))
            head.role = static_cast<uint32_t>(parseEnumerator<BlockRole>());
        else if (acceptAttribute("subject"_s, BlockAttribute::Subject))
            head.subject = parseValueUseWord();
        else if (acceptAttribute("frequency"_s, BlockAttribute::Frequency))
            head.frequency = static_cast<uint32_t>(parseEnumerator<BlockFrequency>());
        else if (acceptAttribute("handler"_s, BlockAttribute::Handler))
            parseTarget();
        else
            fail();
    }
    head.hasHandler = seenAttributes.contains(BlockAttribute::Handler);
    expect(TokenKind::Colon);
    expectEndOfLine();
}

void TextParser::parseBlockParameter()
{
    ValueID id(numberOfDefinedValues(m_body.value));
    expectID(*this, IDPrefix::Value, id.index());
    m_body.value = id;

    uint32_t typeWord = accept(TokenKind::Colon) ? parseTypeWord() : Wire::none;
    if (at(TokenKind::Colon)) {
        fail();
        return;
    }

    ValueEngineFields engineFields;
    parseEngineValueFields(engineFields);
    appendBlockParameter(id, typeWord, WTF::move(engineFields));
}

void TextParser::appendBlockParameter(ValueID id, uint32_t typeWord, ValueEngineFields&& engineFields)
{
    m_body.value = id;
    appendRecord(code(), Wire::ParameterHead { typeWord });
    engineFields.function = m_body.function;
    engineFields.value = id;
    if (m_form != IRForm::AsRead)
        engineFields.typeWord = typeWord;
    if (hasAnyField(engineFields))
        m_valueEngineFields.append(engineFields);
}

void TextParser::parseValue()
{
    ValueID id(numberOfDefinedValues(m_body.value));
    m_body.value = id;

    bool hasResult = atID(IDPrefix::Value);
    if (hasResult) {
        ValueEngineFields engineFields;
        engineFields.function = m_body.function;
        engineFields.value = id;
        expectID(*this, IDPrefix::Value, id.index());
        parseEngineValueFields(engineFields);
        expect(TokenKind::Equal);
        if (hasAnyField(engineFields))
            m_valueEngineFields.append(engineFields);
    }

    TextPosition opcodePosition = token().position;
    Opcode opcode = parseEnumerator<Opcode>();
    if (hasResult != (resultRuleOf(opcode) != ResultRule::None))
        fail(InternalErrorKind::IRTextError, opcodePosition);

    size_t headPosition = appendRecord(code(), Wire::ValueHead { });
    uint32_t wireFlags = wireFlagsOf(opcode);
    size_t flagsPosition = code().size();
    if (wireFlags)
        code().append(0);
    parseFields(*this, opcode);

    uint32_t flags = parseEntryFlags(*this, flagNamesOf(opcode));
    if (wireFlags)
        code()[flagsPosition] = flags & wireFlags;
    auto engineFlags = static_cast<uint16_t>(flags & ~wireFlags);

    Origin origin = parseOrigin(*this);
    uint32_t writtenPositionFollows = 0;
    if (accept(TokenKind::At)) {
        writtenPositionFollows = Wire::WrittenPositionFollows;
        code().append(parseUInt32());
    }
    overwriteRecord(code(), headPosition, Wire::ValueHead { static_cast<uint32_t>(opcode) | writtenPositionFollows, origin.position });
    if (engineFlags || origin.inlineFrame || origin.isTailCall) {
        addPatch([function = m_body.function, id, engineFlags, origin](PatchContext& context) {
            Value& value = context.program.at(function).edit(id);
            value.flags |= engineFlags;
            value.setOrigin(origin);
        });
    }
    expectEndOfLine();
}

void TextParser::parseEngineValueFields(ValueEngineFields& fields)
{
    {
        SetForScope layoutsAreExact(m_layoutsAreExact, true);
        if (accept(TokenKind::Colon))
            fields.typeWord = parseTypeWord();
        if (accept(TokenKind::Tilde))
            fields.expectedWord = parseTypeWord();
    }
    if (accept(TokenKind::At))
        fields.representation = parseEnumerator<Representation>();
}

void TextParser::parseTerminator()
{
    TerminatorKind kind = parseEnumerator<TerminatorKind>();
    size_t headPosition = appendRecord(code(), Wire::TerminatorHead { });
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

    Origin origin = parseOrigin(*this);
    overwriteRecord(code(), headPosition, Wire::TerminatorHead { static_cast<uint32_t>(kind), origin.position, static_cast<uint32_t>(targetNames.size()) });
    if (origin.inlineFrame || origin.isTailCall) {
        addPatch([function = m_body.function, block = m_body.block, origin](PatchContext& context) {
            context.program.at(function).edit(block).terminator.setOrigin(origin);
        });
    }
    expectEndOfLine();
}

void TextParser::parseTarget()
{
    Wire::TargetHead head;
    head.block = parseBlockUseWord();
    size_t headPosition = appendRecord(code(), head);

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
    overwriteRecord(code(), headPosition, head);
}

uint32_t TextParser::parseBlockUseWord()
{
    return parseIDWordBelow(*this, IDPrefix::Block, m_body.scan.numberOfBlocks);
}

uint32_t TextParser::parseValueUseWord()
{
    return parseIDWordBelow(*this, IDPrefix::Value, m_body.scan.numberOfValues);
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
