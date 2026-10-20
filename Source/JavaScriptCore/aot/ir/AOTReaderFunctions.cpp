/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTVerifierInternal.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Record>
static constexpr uint32_t numberOfWordsIn = sizeof(Record) / sizeof(uint32_t);

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return wireFlagsOf(kind); }), "A terminator has no flags word on the wire.");

BodyDecoder::BodyDecoder(const Reader& reader, FunctionID id, const Wire::Body& wire, std::span<const uint32_t> code, Function& function)
    : m_id(id)
    , m_wire(wire)
    , m_code(code)
    , m_function(function)
    , m_checker(reader, InternalError { .kind = InternalErrorKind::Unknown, .function = id, .count = id.index() })
{
}

Failure BodyDecoder::run()
{
    constexpr uint32_t minWordsPerBlock = numberOfWordsIn<Wire::BlockHead> + numberOfWordsIn<Wire::TerminatorHead>;
    if (!hasWords(static_cast<uint64_t>(m_wire.blockCount) * minWordsPerBlock) || !hasWords(m_wire.valueCount)) {
        fail(InternalErrorKind::ListOutOfBounds);
        return m_failure;
    }

    m_function.blocks.grow(m_wire.blockCount);
    m_function.values.grow(m_wire.valueCount);
    for (uint32_t index = 0; index < m_wire.blockCount && !hasFailed(); ++index)
        decodeBlock(BlockID(index));

    m_block = { };
    m_value = { };
    if (m_offset != m_code.size() || m_nextValue != m_wire.valueCount)
        fail(InternalErrorKind::ListOutOfBounds);
    return m_failure;
}

void BodyDecoder::fail(InternalErrorKind kind)
{
    if (hasFailed())
        return;
    m_failure = InternalError { .kind = kind, .function = m_id, .block = m_block, .value = m_value, .count = m_id.index() };
    // No word is left, so whatever still runs reads zeros and empty lists.
    m_offset = m_code.size();
}

void BodyDecoder::adoptCheckerFailure()
{
    if (!m_checker.hasFailed()) [[likely]]
        return;
    fail(m_checker.takeFailure()->kind);
}

uint32_t BodyDecoder::next()
{
    if (m_offset < m_code.size()) [[likely]]
        return m_code[m_offset++];
    fail(InternalErrorKind::ListOutOfBounds);
    return 0;
}

uint64_t BodyDecoder::nextPair()
{
    uint64_t low = next();
    uint64_t high = next();
    return high << 32 | low;
}

template<typename Record>
Record BodyDecoder::nextRecord()
{
    static_assert(alignof(Record) == alignof(uint32_t) && !(sizeof(Record) % sizeof(uint32_t)));
    if (!hasWords(numberOfWordsIn<Record>)) {
        fail(InternalErrorKind::ListOutOfBounds);
        return { };
    }
    Record record = reinterpretCastSpanStartTo<Record>(m_code.subspan(m_offset));
    m_offset += numberOfWordsIn<Record>;
    return record;
}

uint32_t BodyDecoder::checkedCount(uint32_t count, uint32_t minWordsPerElement)
{
    if (hasWords(static_cast<uint64_t>(count) * minWordsPerElement))
        return count;
    fail(InternalErrorKind::ListOutOfBounds);
    return 0;
}

ValueID BodyDecoder::allocateValueID()
{
    if (m_nextValue >= m_wire.valueCount) {
        fail(InternalErrorKind::IdOutOfRange);
        return { };
    }
    m_value = ValueID(m_nextValue++);
    return m_value;
}

ValueID BodyDecoder::valueID(uint32_t word)
{
    if (word < m_wire.valueCount)
        return ValueID(word);
    fail(InternalErrorKind::IdOutOfRange);
    return { };
}

ValueID BodyDecoder::nextOperand()
{
    uint32_t word = next();
    if (word != Wire::none)
        return valueID(word);
    fail(InternalErrorKind::OperandMissing);
    return { };
}

ValueID BodyDecoder::nextOptionalOperand()
{
    uint32_t word = next();
    if (word != Wire::none)
        return valueID(word);
    return { };
}

void BodyDecoder::decodeVariadicOperands(Instruction& instruction)
{
    uint32_t count = nextCount();
    instruction.arguments.reserveCapacity(instruction.arguments.size() + count);
    for (uint32_t index = 0; index < count && !hasFailed(); ++index)
        instruction.appendVariadicOperand(valueID(next()));
}

uint16_t BodyDecoder::nextFlags(uint32_t wireMask, uint32_t declaredMask)
{
    uint32_t word = next();
    if (word & ~declaredMask) {
        fail(InternalErrorKind::UnknownFlag);
        return 0;
    }
    if (word & ~wireMask && !m_checker.acceptsEngineMembers()) {
        fail(InternalErrorKind::EngineOnlyOnWire);
        return 0;
    }
    return static_cast<uint16_t>(word);
}

void BodyDecoder::decodeBlock(BlockID id)
{
    m_block = id;
    m_value = { };
    Block& block = m_function.at(id);

    auto head = nextRecord<Wire::BlockHead>();
    block.role = m_checker.enumerator<BlockRole>(head.role);
    block.frequency = m_checker.enumerator<BlockFrequency>(head.frequency);
    if (head.subject != Wire::none)
        block.subject = valueID(head.subject);
    bool hasHandler = m_checker.boolean(head.hasHandler);
    adoptCheckerFailure();

    decodeParameters(block, checkedCount(head.parameterCount, numberOfWordsIn<Wire::ParameterHead>));

    m_value = { };
    if (hasHandler)
        block.handler = decodeTarget();

    uint32_t valueCount = checkedCount(head.valueCount, numberOfWordsIn<Wire::ValueHead>);
    block.values.reserveInitialCapacity(valueCount);
    for (uint32_t index = 0; index < valueCount && !hasFailed(); ++index)
        decodeValue(block);

    m_value = { };
    decodeTerminator(block);
}

void BodyDecoder::decodeParameters(Block& block, uint32_t count)
{
    block.parameters.reserveInitialCapacity(count);
    for (uint32_t index = 0; index < count && !hasFailed(); ++index) {
        ValueID id = allocateValueID();
        if (!id)
            return;

        auto head = nextRecord<Wire::ParameterHead>();
        Value& parameter = m_function.at(id);
        parameter.become(Opcode::Parameter);
        parameter.type = m_checker.reader().promisedTypeOf(m_checker.optionalID<TypeID>(head.type));
        adoptCheckerFailure();
        block.parameters.append(id);
    }
}

void BodyDecoder::decodeValue(Block& block)
{
    ValueID id = allocateValueID();
    if (!id)
        return;

    auto head = nextRecord<Wire::ValueHead>();
    Opcode opcode = m_checker.enumerator<Opcode>(head.opcode);
    adoptCheckerFailure();
    if (opcode == Opcode::Parameter)
        fail(InternalErrorKind::EngineOnlyOnWire);
    if (hasFailed())
        return;

    Value& value = m_function.at(id);
    value.become(opcode);
    value.position = head.position;
    if (uint16_t wireFlags = wireFlagsOf(opcode))
        value.flags = nextFlags(wireFlags, declaredFlagsOf(opcode));
    decodeFields(*this, value);
    adoptCheckerFailure();
    block.values.append(id);
}

void BodyDecoder::decodeTerminator(Block& block)
{
    auto head = nextRecord<Wire::TerminatorHead>();
    TerminatorKind kind = m_checker.enumerator<TerminatorKind>(head.kind);
    adoptCheckerFailure();
    if (hasFailed())
        return;

    Terminator& terminator = block.terminator;
    terminator.become(kind);
    terminator.position = head.position;
    decodeFields(*this, terminator);
    adoptCheckerFailure();

    uint32_t targetCount = checkedCount(head.targetCount, numberOfWordsIn<Wire::TargetHead>);
    terminator.targets.reserveInitialCapacity(targetCount);
    for (uint32_t index = 0; index < targetCount && !hasFailed(); ++index)
        terminator.targets.append(decodeTarget());
}

Target BodyDecoder::decodeTarget()
{
    Target target;
    auto head = nextRecord<Wire::TargetHead>();
    if (head.block >= m_wire.blockCount) {
        fail(InternalErrorKind::IdOutOfRange);
        return target;
    }
    target.block = BlockID(head.block);

    uint32_t argumentCount = checkedCount(head.argumentCount);
    target.arguments.reserveInitialCapacity(argumentCount);
    for (uint32_t index = 0; index < argumentCount && !hasFailed(); ++index)
        target.arguments.append(valueID(next()));

    uint32_t proofCount = checkedCount(head.proofCount);
    target.proofs.reserveInitialCapacity(proofCount);
    for (uint32_t index = 0; index < proofCount && !hasFailed(); ++index)
        target.proofs.append(next());
    return target;
}

bool Reader::readFunctions()
{
    constexpr size_t functionsPerChunk = 32;
    RELEASE_ASSERT(m_program.functions.size() == m_sections.functions.size());
    auto failures = readInChunks(m_build.parallelLoops(), m_sections.functions.size(), functionsPerChunk, [&](uint32_t index) {
        return readFunction(index, m_program.functions[index]);
    });
    return reportFirst(failures.span());
}

Failure Reader::readFunction(uint32_t index, Function& function) const
{
    const Wire::Function& wire = m_sections.functions[index];
    FunctionID id(index);
    RecordChecker checker(*this, { .kind = InternalErrorKind::Unknown, .function = id, .count = index });
    read(checker, wire, static_cast<FunctionRecord&>(function));
    std::span<const uint32_t> code = checker.code(wire.body.code);
    if (Failure failure = checker.takeFailure())
        return failure;
    if (Failure failure = BodyDecoder(*this, id, wire.body, code, function).run())
        return failure;
    function.capabilities = initialCapabilitiesOf(function);

    FunctionVerifier verifier(m_program, id, function, m_form, m_build, m_verifierInputs);
    if (verifier.runStructure())
        return std::nullopt;
    return verifier.takeErrors().first();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
