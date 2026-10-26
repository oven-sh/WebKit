/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTCheckTypesAtEntry.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "AOTVerifierInternal.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Record>
static constexpr uint32_t numberOfWordsIn = sizeof(Record) / sizeof(uint32_t);

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return wireFlagsOf(kind); }), "A terminator has no flags word on the wire.");

BodyDecoder::BodyDecoder(const Reader& reader, FunctionID id, const Wire::Body& wire, std::span<const uint32_t> code, Function& function, const UnitIDRemap* remap)
    : m_id(id)
    , m_wire(wire)
    , m_code(code)
    , m_function(function)
    , m_checker(reader, InternalError { .kind = InternalErrorKind::Unknown, .function = id, .count = id.index() })
{
    m_checker.setIDRemap(remap);
}

Failure BodyDecoder::run()
{
    constexpr uint32_t minWordsPerBlock = numberOfWordsIn<Wire::BlockHead> + numberOfWordsIn<Wire::TerminatorHead>;
    if (!hasWords(static_cast<uint64_t>(m_wire.blockCount) * minWordsPerBlock) || !hasWords(m_wire.valueCount)) {
        fail(InternalErrorKind::ListOutOfBounds);
        return m_failure;
    }

    m_function.editAllBlocks().grow(m_wire.blockCount);
    m_function.editAllValues().reserveInitialCapacity(m_wire.valueCount + m_numberOfChecks);
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

Value& BodyDecoder::appendValue(ValueID id, Opcode opcode)
{
    Function::Values::Storage& values = m_function.editAllValues();
    RELEASE_ASSERT(id.index() == values.size());
    values.constructAndAppend(opcode);
    return values.last();
}

ValueID BodyDecoder::valueID(uint32_t word)
{
    if (word < m_wire.valueCount)
        return usedValue(word);
    fail(InternalErrorKind::IdOutOfRange);
    return { };
}

void BodyDecoder::setParametersCheckedAtEntry(std::span<const unsigned> checkedParameters)
{
    if (checkedParameters.empty())
        return;
    m_numberOfChecks = static_cast<uint32_t>(checkedParameters.size());
    m_checksOfParameters = Vector<ValueID, 8>(checkedParameters.back() + 1, [](size_t index) {
        return ValueID(static_cast<uint32_t>(index));
    });
    for (size_t position = 0; position < checkedParameters.size(); ++position)
        m_checksOfParameters[checkedParameters[position]] = ValueID(m_wire.valueCount + static_cast<uint32_t>(position));
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
    instruction.reserveVariadicOperands(count);
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
    Block& block = m_function.edit(id);

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
        Value& parameter = appendValue(id, Opcode::Parameter);
        parameter.type = m_checker.optionalID<TypeID>(head.type);
        adoptCheckerFailure();
        block.parameters.append(id);
    }
}

static bool constructsWhereWritten(const Value& value)
{
    switch (value.opcode) {
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return true;
    case Opcode::Call:
        return value.isReflectConstructSyntax();
    default:
        return false;
    }
}

void BodyDecoder::decodeValue(Block& block)
{
    ValueID id = allocateValueID();
    if (!id)
        return;

    auto head = nextRecord<Wire::ValueHead>();
    Opcode opcode = m_checker.enumerator<Opcode>(head.opcode & ~Wire::WrittenPositionFollows);
    adoptCheckerFailure();
    if (opcode == Opcode::Parameter)
        fail(InternalErrorKind::EngineOnlyOnWire);
    if (hasFailed())
        return;

    Value& value = appendValue(id, opcode);
    value.position = head.position;
    if (uint16_t wireFlags = wireFlagsOf(opcode))
        value.flags = nextFlags(wireFlags, declaredFlagsOf(opcode));
    decodeFields(*this, value);
    adoptCheckerFailure();
    if (head.opcode & Wire::WrittenPositionFollows) {
        uint32_t written = next();
        if (constructsWhereWritten(value))
            m_function.constructionStarts.append({ value.position, written, opcode == Opcode::Call });
        else
            m_function.writtenPositions.append({ id, written });
    }
    if (opcode == Opcode::Convert && !hasFailed())
        checkConvertedAllocation(value);
    if (opcode == Opcode::Builtin && !hasFailed())
        m_namesObjectCreate |= m_checker.reader().builtins().canonicalID(value.builtin()) == Builtins::ObjectCreate;
    if (opcode == Opcode::CallDirect && !hasFailed())
        m_directCallees.append(value.function());
    block.values.append(id);
}

void BodyDecoder::checkConvertedAllocation(const Value& conversion)
{
    ValueID operand = conversion.valueOperand();
    DeclaredType declared = conversion.declared();
    bool isDecoded = operand.index() < m_value.index();
    if (!isDecoded || m_function.at(operand).opcode != Opcode::NewObject)
        return;
    if (conversion.illTypedCause() != IllTypedCause::None || declared.isExpected())
        return;

    const Reader& reader = m_checker.reader();
    LayoutID allocated = m_function.at(operand).layout();
    const Type& type = reader.program().at(declared.expected().predicted());
    if (type.anyObject || std::ranges::binary_search(membersOf(reader.program(), type), allocated))
        return;

    reader.build().statistics().count(StatisticID::ConversionsOutsideFamily);
    if (!reader.build().options().verifiesAfterEveryStage)
        return;
    fail(InternalErrorKind::FamilyNotClosed);
    m_failure->layout = allocated;
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

Vector<Failure> Reader::readFunctions()
{
    constexpr size_t functionsPerChunk = 32;
    RELEASE_ASSERT(m_program.functions.size() == m_sections.functions.size());
    RELEASE_ASSERT(!m_externalBodies || m_externalBodies->bodyCount == m_sections.functions.size());
    size_t numberOfFunctions = m_sections.functions.size();
    Vector<Failure> failures((numberOfFunctions + functionsPerChunk - 1) / functionsPerChunk);
    Vector<Program::BodiesAsRead> seenByWorkers(m_build.parallelLoops().width());
    forEachIndexInParallel(m_build.parallelLoops(), static_cast<unsigned>(failures.size()), [&](unsigned chunk, unsigned worker) {
        size_t end = std::min(numberOfFunctions, (static_cast<size_t>(chunk) + 1) * functionsPerChunk);
        for (size_t index = static_cast<size_t>(chunk) * functionsPerChunk; index < end && !failures[chunk]; ++index) {
            Function& function = m_program.functions[index];
            SeenInBody seen;
            failures[chunk] = readFunction(static_cast<uint32_t>(index), function, seen);
            if (failures[chunk])
                break;
            if (seen.namesObjectCreate)
                seenByWorkers[worker].functionsThatNameObjectCreate.append(FunctionID(static_cast<uint32_t>(index)));
            if (!seen.directCallees.isEmpty()) {
                seenByWorkers[worker].functionsCalledDirectly.ensureSize(numberOfFunctions);
                for (FunctionID callee : seen.directCallees)
                    seenByWorkers[worker].functionsCalledDirectly.quickSet(callee.index());
            }
            if (m_layoutUses)
                m_layoutUses->collect(function, worker);
            if (m_preparesFunctionsForTyping && m_longExpectations->hasAny())
                m_longExpectations->forgetInBodyOf(function);
        }
    });
    if (m_preparesFunctionsForTyping) {
        Program::BodiesAsRead bodies;
        bodies.functionsCalledDirectly.ensureSize(numberOfFunctions);
        for (Program::BodiesAsRead& seen : seenByWorkers) {
            bodies.functionsThatNameObjectCreate.appendVector(seen.functionsThatNameObjectCreate);
            bodies.functionsCalledDirectly.merge(seen.functionsCalledDirectly);
        }
        AOT::sort(bodies.functionsThatNameObjectCreate);
        m_program.setBodiesAsRead(WTF::move(bodies));
    }
    return failures;
}

static void readRecordWithIDsOfUnit(RecordChecker& checker, const Wire::Function& wire, const UnitIDRemap& remap, FunctionRecord& record)
{
    Wire::Function withoutFunctionTypes = wire;
    withoutFunctionTypes.functionTypes = { };
    checker.setIDRemap(&remap);
    read(checker, withoutFunctionTypes, record);
    checker.setIDRemap(nullptr);

    std::span<const uint32_t> functionTypes = checker.words(wire.functionTypes);
    record.functionTypes.reserveInitialCapacity(functionTypes.size());
    for (size_t index = 0; index < functionTypes.size(); ++index) {
        if (index)
            checker.requireAscending(functionTypes[index - 1], functionTypes[index]);
        record.functionTypes.append(checker.id<FunctionID>(functionTypes[index]));
    }
}

Failure Reader::readFunction(uint32_t index, Function& function, SeenInBody& seen) const
{
    const Wire::Function& wire = m_sections.functions[index];
    FunctionID id(index);
    RecordChecker checker(*this, { .kind = InternalErrorKind::Unknown, .function = id, .count = index });
    std::span<const uint32_t> code = checker.code(wire.body.code);
    const UnitIDRemap* remap = nullptr;
    if (m_externalBodies) {
        const ExternalFunctionBody& body = m_externalBodies->bodies[index];
        checker.require(code.empty() && body.unitIndex < m_externalBodies->unitCount, InternalErrorKind::ListOutOfBounds);
        if (Failure failure = checker.takeFailure())
            return failure;
        code = { body.code, body.wordCount };
        remap = &m_externalBodies->units[body.unitIndex];
        readRecordWithIDsOfUnit(checker, wire, *remap, function);
    } else
        read(checker, wire, static_cast<FunctionRecord&>(function));
    if (Failure failure = checker.takeFailure())
        return failure;

    Vector<unsigned, 8> checkedParameters;
    if (m_preparesFunctionsForTyping) {
        if (m_longExpectations->hasAny())
            m_longExpectations->forgetInRecordOf(function);
        if (!m_constructorsOfTaggedFamilies.quickGet(index))
            checkedParameters = findParametersCheckedAtEntry(m_program, m_build, id, function, wire.body.blockCount);
    }
    {
        BodyDecoder decoder(*this, id, wire.body, code, function, remap);
        decoder.setParametersCheckedAtEntry(checkedParameters.span());
        if (Failure failure = decoder.run())
            return failure;
        seen.namesObjectCreate = decoder.namesObjectCreate();
        seen.directCallees = decoder.takeDirectCallees();
    }
    if (seen.namesObjectCreate && !checkedParameters.isEmpty()) {
        checkedParameters.clear();
        function.editAllBlocks().clear();
        function.editAllValues().clear();
        function.writtenPositions.clear();
        function.constructionStarts.clear();
        if (Failure failure = BodyDecoder(*this, id, wire.body, code, function, remap).run())
            return failure;
    }
    if (!checkedParameters.isEmpty()) {
        if (function.at(entryBlock).parameters.size() != function.numberOfEntryParameters())
            return InternalError { .kind = InternalErrorKind::ListOutOfBounds, .function = id, .count = index };
        appendChecksAtEntry(m_program, m_build, function, checkedParameters.span());
        m_build.statistics().count(StatisticID::FunctionsWithTypesCheckedAtEntry);
    }
    function.capabilities = initialCapabilitiesOf(function);

    const auto& options = m_build.options();
    if (!options.verifiesProgram && !options.isVerifyOnly())
        return std::nullopt;
    FunctionVerifier verifier(m_program, id, function, m_form, m_build, m_verifierInputs);
    if (verifier.runStructure())
        return std::nullopt;
    return verifier.takeErrors().first();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
