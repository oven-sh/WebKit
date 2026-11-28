/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTBodyIndex.h"
#include "AOTBodyVerifier.h"
#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static_assert(BodyList<StringID>::wordsPerElement == 1 && BodyList<int64_t>::wordsPerElement == 2 && BodyList<TemplateString>::wordsPerElement == 2 && BodyList<TailCast>::wordsPerElement == 3);

static constexpr uint32_t bodyTestTableSize = 8;
static constexpr uint32_t bodyTestUnnamedOrdinal = bodyTestTableSize - 1;
static constexpr uint32_t bodyTestCopiedOperandShift = 1000;
static constexpr uint64_t bodyTestNegativeZeroBits = 0x8000000000000000ull;
static constexpr uint64_t bodyTestNaNBits = 0xfff8dead00000001ull;

static constexpr uint32_t bodyTestFirstID(RemappedIDKind kind)
{
    return 100 * (static_cast<uint32_t>(kind) + 1);
}

template<typename IDType>
static IDType bodyTestID(uint32_t ordinal)
{
    return IDType(bodyTestFirstID(remappedIDKindOf<IDType>) + ordinal);
}

enum class BodyTestNumbering : bool { Forward, Backward };

class BodyTestUnit {
    WTF_MAKE_NONCOPYABLE(BodyTestUnit);
public:
    explicit BodyTestUnit(BodyTestNumbering numbering)
        : m_numbering(numbering)
    {
        for (uint32_t kind = 0; kind < numberOfRemappedIDKinds; ++kind) {
            for (uint32_t number = 0; number < bodyTestTableSize; ++number) {
                uint32_t ordinal = numberOf(number);
                m_entries[kind][number] = ordinal == bodyTestUnnamedOrdinal ? 0 : bodyTestFirstID(static_cast<RemappedIDKind>(kind)) + ordinal + 1;
            }
            m_remap.tables[kind] = { m_entries[kind].data(), m_entries[kind].size() };
        }
    }

    const UnitIDRemap& remap() const { return m_remap; }
    uint32_t numberOf(uint32_t ordinal) const { return m_numbering == BodyTestNumbering::Backward ? bodyTestTableSize - 1 - ordinal : ordinal; }

private:
    std::array<std::array<uint32_t, bodyTestTableSize>, numberOfRemappedIDKinds> m_entries;
    UnitIDRemap m_remap;
    BodyTestNumbering m_numbering;
};

class BodyTestWords {
public:
    template<typename... Words>
    BodyTestWords& add(Words... words)
    {
        (m_words.append(static_cast<uint32_t>(words)), ...);
        return *this;
    }

    BodyTestWords& addPair(uint64_t bits) { return add(bits, bits >> 32); }

    BodyTestWords& addList(std::initializer_list<uint32_t> words)
    {
        add(words.size());
        for (uint32_t word : words)
            add(word);
        return *this;
    }

    BodyTestWords& block(std::initializer_list<uint32_t> parameterTypes, BlockRole role = BlockRole::Ordinary, uint32_t subject = Wire::none)
    {
        m_blockHead = m_words.size();
        ++m_numberOfBlocks;
        m_numberOfValues += static_cast<uint32_t>(parameterTypes.size());
        static_assert(BodyBlock::numberOfHeadWords == 6);
        add(parameterTypes.size(), 0, role, subject, 0, BlockFrequency::Normal);
        for (uint32_t type : parameterTypes)
            add(type);
        return *this;
    }

    BodyTestWords& handler(uint32_t block, std::initializer_list<uint32_t> arguments)
    {
        m_words[m_blockHead + offsetof(Wire::BlockHead, hasHandler) / sizeof(uint32_t)] = 1;
        return target(block, arguments);
    }

    BodyTestWords& value(Opcode opcode, uint32_t position, uint32_t flags = 0, uint32_t bitsBesideOpcode = 0)
    {
        ++m_words[m_blockHead + offsetof(Wire::BlockHead, valueCount) / sizeof(uint32_t)];
        ++m_numberOfValues;
        add(static_cast<uint32_t>(opcode) | bitsBesideOpcode, position);
        if (wireFlagsOf(opcode))
            return add(flags);
        AOT_TEST_CHECK(!flags);
        return *this;
    }

    BodyTestWords& terminator(TerminatorKind kind, uint32_t position, uint32_t numberOfTargets) { return add(kind, position, numberOfTargets); }

    BodyTestWords& target(uint32_t block, std::initializer_list<uint32_t> arguments = { }, std::initializer_list<uint32_t> proofs = { })
    {
        add(block, arguments.size(), proofs.size());
        for (uint32_t argument : arguments)
            add(argument);
        for (uint32_t proof : proofs)
            add(proof);
        return *this;
    }

    const Vector<uint32_t>& words() const { return m_words; }
    uint32_t numberOfBlocks() const { return m_numberOfBlocks; }
    uint32_t numberOfValues() const { return m_numberOfValues; }
    Body body(const UnitIDRemap& unit) const { return Body(m_words.span(), m_numberOfBlocks, m_numberOfValues, unit); }

private:
    Vector<uint32_t> m_words;
    size_t m_blockHead { 0 };
    uint32_t m_numberOfBlocks { 0 };
    uint32_t m_numberOfValues { 0 };
};

namespace BodyOfEveryShape {

enum : uint32_t { v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, numberOfValues };
enum : uint32_t { b0, b1, b2, b3, b4, numberOfBlocks };

} // namespace BodyOfEveryShape

static BodyTestWords writeBodyOfEveryShape(const BodyTestUnit& unit)
{
    using namespace BodyOfEveryShape;
    constexpr uint32_t none = Wire::none;
    auto id = [&](uint32_t ordinal) { return unit.numberOf(ordinal); };
    auto bitsOf = [](int64_t integer) { return static_cast<uint64_t>(integer); };

    BodyTestWords words;
    words.block({ id(0), none });
    words.value(Opcode::Integer, 102).addPair(bitsOf(-5));
    words.value(Opcode::Double, 103).addPair(bodyTestNegativeZeroBits);
    words.value(Opcode::Double, none).addPair(bodyTestNaNBits);
    words.value(Opcode::String, 105).add(id(1));
    words.value(Opcode::String, 106).add(id(bodyTestUnnamedOrdinal));
    words.value(Opcode::Call, 107, declaredPosition(FlagMask::isNeverInlined(Opcode::Call))).add(v0, none).addList({ v2, v3 }).add(id(2), none, id(3), none, 9);
    words.value(Opcode::NewObject, 108).add(id(1)).addList({ 0, 2 }).add(none).addList({ v2, v3 });
    words.value(Opcode::NewArray, 109).add(id(2)).addList({ 1 }).addList({ v2, v3, v4 });
    words.value(Opcode::AppendElement, 110).add(v9, v2);
    words.value(Opcode::NewTemplateObject, 111).add(2, id(2), id(3), none, id(4));
    words.value(Opcode::Convert, 112).add(v7, id(3), id(1), PositionFlags::IsExpected, IllTypedCause::Assertion);
    words.value(Opcode::CopyDataProperties, 113).add(v8, v7).addList({ id(0), id(bodyTestUnnamedOrdinal), id(4) }).addList({ v5 });
    words.value(Opcode::GetModuleVariable, 114).add(id(2));
    words.value(Opcode::ImportModule, 115).add(id(1), none, id(2));
    words.value(Opcode::NewClosure, 116).add(id(3)).addList({ });
    words.value(Opcode::Data, 117).add(id(4));
    words.value(Opcode::DirectEval, 118).add(id(5), v5, v0, none).addList({ });
    words.value(Opcode::TypeTest, 119).add(v0, id(1));
    words.terminator(TerminatorKind::Branch, 200, 2).add(v19).target(b1, { v2 }, { 0 }).target(b2);

    words.block({ none, none }).handler(b4, { v0, v2 });
    words.value(Opcode::ThrowModuleFailure, 122, declaredPosition(FlagMask::isForRequire(Opcode::ThrowModuleFailure))).add(id(6));
    words.terminator(TerminatorKind::Switch, 201, 4).add(v20, 3).addPair(bitsOf(1)).addPair(bitsOf(-1)).addPair(bitsOf(std::numeric_limits<int64_t>::min()));
    words.target(b2).target(b3, { v20 }).target(b3, { v21 }).target(b3, { v2 });

    words.block({ });
    words.terminator(TerminatorKind::SwitchString, 202, 3).add(v5).addList({ id(1), id(2) }).target(b3, { v2 }).target(b3, { v3 }).target(b3, { v4 });

    words.block({ id(1) });
    words.value(Opcode::Call, 124, 0, Wire::StandsInTailPosition).add(v0, v23).addList({ }).add(none, id(5), none, none, none);
    words.terminator(TerminatorKind::ReturnCall, 203, 0).add(v24, 2, id(1), id(0), 0, id(2), none, PositionFlags::IsExpected);

    words.block({ none, none }, BlockRole::CatchHandler);
    words.value(Opcode::CatchValue, 127);
    words.terminator(TerminatorKind::Throw, 204, 0).add(v27);

    AOT_TEST_CHECK(words.numberOfBlocks() == numberOfBlocks && words.numberOfValues() == numberOfValues);
    return words;
}

static BodyTestWords writeBodyWithSecondPositions(const BodyTestUnit& unit)
{
    constexpr uint32_t none = Wire::none;
    constexpr uint32_t follows = Wire::WrittenPositionFollows;
    uint32_t key = unit.numberOf(1);

    BodyTestWords words;
    words.block({ }).handler(1, { });
    words.value(Opcode::Undefined, 10);
    words.value(Opcode::PropGet, 11, 0, follows).add(0, key, 21);
    words.value(Opcode::Construct, 12, 0, follows).add(0, none).addList({ }).add(none, 22);
    words.value(Opcode::Call, 13, declaredPosition(FlagMask::isDoneInPlace(Opcode::Call)), follows).add(0, none).addList({ }).add(none, none, none, none, none, 23);
    words.value(Opcode::Call, 14, 0, follows | Wire::StandsInTailPosition).add(0, none).addList({ }).add(none, none, none, none, none, 24);
    words.value(Opcode::PropGet, 15, 0, follows | Wire::WrittenPositionIsForCallers).add(0, key, 25);
    words.value(Opcode::IteratorOpen, 16, declaredPosition(FlagMask::isDestructuring(Opcode::IteratorOpen)), follows).add(0, IteratorKind::Sync, none, 26);
    words.terminator(TerminatorKind::Return, 30, 0).add(none);

    words.block({ }, BlockRole::IterationCleanup, 6);
    words.terminator(TerminatorKind::Unreachable, 31, 0);
    return words;
}

template<typename Functor>
static void forEachNumberingOfBodyOfEveryShape(const Functor& functor)
{
    for (BodyTestNumbering numbering : { BodyTestNumbering::Forward, BodyTestNumbering::Backward }) {
        BodyTestUnit unit(numbering);
        BodyTestWords words = writeBodyOfEveryShape(unit);
        functor(words.body(unit.remap()));
    }
}

template<typename Range, typename T>
static bool bodyTestRangeEquals(const Range& range, std::initializer_list<T> expected)
{
    return std::ranges::equal(range, expected);
}

static bool bodyTestValuesEqual(std::span<const ValueID> values, std::initializer_list<uint32_t> expected)
{
    return std::ranges::equal(values, expected, { }, &ValueID::index);
}

struct BodyTestWalk {
    Vector<BodyBlock> blocks;
    Vector<BodyValue> values;
};

static BodyTestWalk walkBodyInStreamOrder(const Body& body)
{
    BodyTestWalk walk;
    body.forEachBlock([&](BodyBlock block) {
        AOT_TEST_CHECK(block.id().index() == walk.blocks.size() && block.firstParameter().index() == walk.values.size());
        walk.blocks.append(block);
        for (uint32_t index = 0; index < block.numberOfParameters(); ++index)
            walk.values.append(block.parameterValue(index));
        AOT_TEST_CHECK(block.firstValue().index() == walk.values.size() && block.values().size() == block.numberOfValues());
        for (BodyValue value : block.values())
            walk.values.append(value);
        AOT_TEST_CHECK(block.endOfValues().index() == walk.values.size());
    });
    AOT_TEST_CHECK(walk.blocks.size() == body.numberOfBlocks() && walk.values.size() == body.numberOfValues());
    AOT_TEST_CHECK(walk.blocks.last().terminator().end() == body.words().data() + body.words().size());
    for (uint32_t id = 0; id < walk.values.size(); ++id)
        AOT_TEST_CHECK(walk.values[id].id() == ValueID(id));
    return walk;
}

static void checkBlocksAndEdgesOfEveryShape(const Body& body, uint32_t typeShift)
{
    using namespace BodyOfEveryShape;
    auto type = [&](uint32_t ordinal) { return TypeID(bodyTestID<TypeID>(ordinal).bits() + typeShift); };
    BodyTestWalk walk = walkBodyInStreamOrder(body);
    const Vector<BodyBlock>& blocks = walk.blocks;

    const BodyBlock& entry = blocks[b0];
    AOT_TEST_CHECK(body && !Body() && entry.id() == entryBlock && entry.words() == body.words().data());
    AOT_TEST_CHECK(entry.role() == BlockRole::Ordinary && !entry.isHandlerBlock() && !entry.subject() && !entry.hasHandler() && !entry.handler());
    AOT_TEST_CHECK(entry.numberOfParameters() == 2 && entry.parameter(1) == ValueID(v1) && entry.parameterType(0) == type(0) && !entry.parameterType(1));
    AOT_TEST_CHECK(entry.indexOfParameter(ValueID(v1)) == 1u && !entry.indexOfParameter(ValueID(v2)) && !blocks[b1].indexOfParameter(ValueID(v1)));
    AOT_TEST_CHECK(entry.firstValue() == ValueID(v2) && entry.endOfValues() == ValueID(v20) && entry.holds(ValueID(v19)) && !entry.holds(ValueID(v20)));

    BodyValue parameter = entry.parameterValue(0);
    AOT_TEST_CHECK(parameter.isParameter() && parameter.opcode() == Opcode::Parameter && parameter.sizeInWords() == 1 && !parameter.hasPosition() && !parameter.flags());
    AOT_TEST_CHECK(parameter.writtenPositionRole() == WrittenPositionRole::None && !parameter.standsInTailPosition() && !parameter.isPartOfConstruction());

    BodyTerminator branch = entry.terminator();
    AOT_TEST_CHECK(branch.kind() == TerminatorKind::Branch && branch.block() == BlockID(b0) && branch.position() == 200 && branch.hasPosition() && branch.origin().position == 200);
    AOT_TEST_CHECK(branch.isConditionalBranch() && !branch.hasCases() && !branch.numberOfCases() && branch.numberOfTargets() == 2);
    AOT_TEST_CHECK(branch.as<TerminatorKind::Branch>().conditionOperand() == ValueID(v19) && branch.end() == blocks[b1].words());
    AOT_TEST_CHECK(branch.taken().block() == BlockID(b1) && bodyTestValuesEqual(branch.taken().arguments(), { v2 }) && bodyTestRangeEquals(branch.taken().proofs(), { 0u }));
    AOT_TEST_CHECK(branch.taken().numberOfIncomingValues() == blocks[b1].numberOfParameters());
    AOT_TEST_CHECK(branch.notTaken().block() == BlockID(b2) && branch.notTaken().arguments().empty() && branch.notTaken().proofs().empty());

    const BodyBlock& guarded = blocks[b1];
    AOT_TEST_CHECK(guarded.hasHandler() && guarded.handler().block() == BlockID(b4) && bodyTestValuesEqual(guarded.handler().arguments(), { v0, v2 }) && guarded.handler().proofs().empty());
    AOT_TEST_CHECK(guarded.numberOfValues() == 1 && guarded.firstValue() == ValueID(v22) && !guarded.parameterType(0));

    BodyTerminator switchOnIntegers = guarded.terminator();
    BodyTargets targets = switchOnIntegers.targets();
    AOT_TEST_CHECK(switchOnIntegers.hasCases() && switchOnIntegers.numberOfCases() == 3 && targets.size() == 4 && !targets.isEmpty());
    AOT_TEST_CHECK(targets[0].block() == BlockID(b2) && bodyTestValuesEqual(targets[2].arguments(), { v21 }) && bodyTestValuesEqual(switchOnIntegers.otherwise().arguments(), { v2 }));

    Vector<uint32_t> edges;
    guarded.forEachOutgoingEdge([&](BodyTarget target, bool isHandlerEdge) {
        edges.append(target.block().index() * 2 + isHandlerEdge);
    });
    AOT_TEST_CHECK(bodyTestRangeEquals(edges, { b2 * 2u, b3 * 2u, b3 * 2u, b3 * 2u, b4 * 2u + 1 }));

    AOT_TEST_CHECK(!blocks[b2].numberOfParameters() && blocks[b2].values().isEmpty() && blocks[b2].firstValue() == blocks[b2].endOfValues());
    AOT_TEST_CHECK(blocks[b3].parameterType(0) == type(1) && blocks[b3].terminator().targets().isEmpty() && !blocks[b3].terminator().hasCases());
    AOT_TEST_CHECK(blocks[b4].role() == BlockRole::CatchHandler && blocks[b4].isHandlerBlock() && blocks[b4].terminator().as<TerminatorKind::Throw>().valueOperand() == ValueID(v27));
}

struct BodyTestHolder {
    using OperandID = ValueID;

    explicit BodyTestHolder(Opcode opcode)
        : BodyTestHolder(numberOfNamedOperandsOf(opcode), defaultPayloadOf(opcode))
    {
    }
    explicit BodyTestHolder(TerminatorKind kind)
        : BodyTestHolder(numberOfNamedOperandsOf(kind), defaultPayloadOf(kind))
    {
    }
    BodyTestHolder(uint32_t numberOfNamedOperands, std::span<const uint32_t> defaultPayload)
        : numberOfNamedOperands(numberOfNamedOperands)
    {
        operands.fill(ValueID(), numberOfNamedOperands);
        payload.append(defaultPayload);
    }

    void setOperand(uint32_t operandIndex, ValueID operand) { operands[operandIndex] = operand; }
    void reserveVariadicOperands(uint32_t count) { numberOfReservedOperands = count; }
    void appendVariadicOperand(ValueID operand)
    {
        AOT_TEST_CHECK(operands.size() - numberOfNamedOperands < numberOfReservedOperands);
        operands.append(operand);
    }
    uint32_t payloadWordAt(unsigned word) const { return payload[word]; }
    void setPayloadWordAt(unsigned word, uint32_t value) { payload[word] = value; }
    void setPayloadListFrom(unsigned firstWord, std::span<const uint32_t> words)
    {
        AOT_TEST_CHECK(payload.size() <= firstWord);
        while (payload.size() < firstWord)
            payload.append(absentPayloadWord);
        payload.append(words);
    }

    uint32_t word(std::optional<unsigned> position) const { return payload[declaredPosition(position)]; }
    template<typename T> T record(std::optional<unsigned> position) const { return decodePayloadRecord<T>(*this, declaredPosition(position)); }
    std::span<const uint32_t> list(std::optional<unsigned> position) const { return payload.span().subspan(std::min<size_t>(declaredPosition(position), payload.size())); }

    Vector<ValueID> operands;
    Vector<ValueID> operandsAskedFor;
    Vector<uint32_t> payload;
    uint32_t numberOfNamedOperands;
    uint32_t numberOfReservedOperands { 0 };
};
static_assert(OperationHolder<BodyTestHolder>);

template<typename Cursor, typename Entry>
static BodyTestHolder copyIntoBodyTestHolder(const Cursor& cursor, Entry entry)
{
    BodyTestHolder holder(entry);
    copyFields(cursor, holder, [&](ValueID operand) {
        holder.operandsAskedFor.append(operand);
        return ValueID(operand.index() + bodyTestCopiedOperandShift);
    });
    return holder;
}

static void checkCopiesOfEveryShape(const Body& body)
{
    using namespace BodyOfEveryShape;
    constexpr uint32_t none = Wire::none;
    constexpr uint32_t shift = bodyTestCopiedOperandShift;
    BodyTestWalk walk = walkBodyInStreamOrder(body);
    const Vector<BodyValue>& values = walk.values;

    AOT_TEST_CHECK(copyIntoBodyTestHolder(values[v2], Opcode::Integer).record<int64_t>(PayloadWord::integer(Opcode::Integer)) == -5);
    AOT_TEST_CHECK(std::bit_cast<uint64_t>(copyIntoBodyTestHolder(values[v3], Opcode::Double).record<double>(PayloadWord::number(Opcode::Double))) == bodyTestNegativeZeroBits);
    AOT_TEST_CHECK(std::bit_cast<uint64_t>(copyIntoBodyTestHolder(values[v4], Opcode::Double).record<double>(PayloadWord::number(Opcode::Double))) == bodyTestNaNBits);

    BodyTestHolder call = copyIntoBodyTestHolder(values[v7], Opcode::Call);
    AOT_TEST_CHECK(bodyTestValuesEqual(call.operandsAskedFor.span(), { v0, v2, v3 }) && bodyTestValuesEqual(call.operands.span(), { v0 + shift, none, v2 + shift, v3 + shift }));
    AOT_TEST_CHECK(call.word(PayloadWord::calleeKey(Opcode::Call)) == bodyTestID<PropertyKeyID>(2).bits() && call.word(PayloadWord::calleeName(Opcode::Call)) == absentPayloadWord);
    AOT_TEST_CHECK(call.word(PayloadWord::expectedResult(Opcode::Call)) == bodyTestID<ExpectedResultID>(3).bits() && call.word(PayloadWord::calledBuiltin(Opcode::Call)) == absentPayloadWord);
    AOT_TEST_CHECK(call.word(PayloadWord::callNumber(Opcode::Call)) == 9);

    BodyTestHolder newObject = copyIntoBodyTestHolder(values[v8], Opcode::NewObject);
    AOT_TEST_CHECK(bodyTestValuesEqual(newObject.operands.span(), { none, v2 + shift, v3 + shift }) && newObject.word(PayloadWord::layout(Opcode::NewObject)) == bodyTestID<LayoutID>(1).bits());
    AOT_TEST_CHECK(bodyTestRangeEquals(newObject.list(PayloadWord::fields(Opcode::NewObject)), { 0u, 2u }));

    BodyTestHolder newArray = copyIntoBodyTestHolder(values[v9], Opcode::NewArray);
    AOT_TEST_CHECK(bodyTestValuesEqual(newArray.operands.span(), { v2 + shift, v3 + shift, v4 + shift }) && newArray.word(PayloadWord::contents(Opcode::NewArray)) == bodyTestID<TypeID>(2).bits());
    AOT_TEST_CHECK(bodyTestRangeEquals(newArray.list(PayloadWord::holes(Opcode::NewArray)), { 1u }));

    BodyTestHolder templateObject = copyIntoBodyTestHolder(values[v11], Opcode::NewTemplateObject);
    AOT_TEST_CHECK(std::ranges::equal(PayloadListView<TemplateString>(templateObject.list(PayloadWord::templateStrings(Opcode::NewTemplateObject))), values[v11].as<Opcode::NewTemplateObject>().templateStrings()));

    BodyTestHolder convert = copyIntoBodyTestHolder(values[v12], Opcode::Convert);
    AOT_TEST_CHECK(convert.record<DeclaredType>(PayloadWord::declared(Opcode::Convert)) == values[v12].as<Opcode::Convert>().declared());
    AOT_TEST_CHECK(convert.word(PayloadWord::illTypedCause(Opcode::Convert)) == static_cast<uint32_t>(IllTypedCause::Assertion));

    BodyTestHolder copy = copyIntoBodyTestHolder(values[v13], Opcode::CopyDataProperties);
    AOT_TEST_CHECK(bodyTestValuesEqual(copy.operands.span(), { v8 + shift, v7 + shift, v5 + shift }));
    AOT_TEST_CHECK(bodyTestRangeEquals(copy.list(PayloadWord::excludedKeys(Opcode::CopyDataProperties)), { bodyTestID<PropertyKeyID>(0).bits(), none, bodyTestID<PropertyKeyID>(4).bits() }));

    BodyTestHolder switchOnIntegers = copyIntoBodyTestHolder(walk.blocks[b1].terminator(), TerminatorKind::Switch);
    AOT_TEST_CHECK(bodyTestValuesEqual(switchOnIntegers.operands.span(), { v20 + shift }));
    AOT_TEST_CHECK(std::ranges::equal(PayloadListView<int64_t>(switchOnIntegers.list(PayloadWord::cases(TerminatorKind::Switch))), walk.blocks[b1].terminator().as<TerminatorKind::Switch>().cases()));

    BodyTestHolder returnCall = copyIntoBodyTestHolder(walk.blocks[b3].terminator(), TerminatorKind::ReturnCall);
    AOT_TEST_CHECK(std::ranges::equal(PayloadListView<TailCast>(returnCall.list(PayloadWord::casts(TerminatorKind::ReturnCall))), walk.blocks[b3].terminator().as<TerminatorKind::ReturnCall>().casts()));
}

static void checkBodyValuePositions(const BodyValue& value, WrittenPositionRole role, uint32_t position, std::optional<uint32_t> writtenPosition, std::optional<ConstructionStart> constructionStart)
{
    AOT_TEST_CHECK(value.writtenPositionRole() == role && value.position() == position && value.writtenPosition() == writtenPosition);
    AOT_TEST_CHECK(value.constructionStart() == constructionStart && value.isPartOfConstruction() == constructionStart.has_value());
    AOT_TEST_CHECK(value.origin().position == position && value.origin().isPartOfConstruction == constructionStart.has_value() && !value.origin().inlineFrame);
}

static void checkBodyIndexAgreesWithStream(const Function& function)
{
    BodyIndexTable table(1, 1);
    const BodyIndex& index = table.build(FunctionID(0u), function, 0);
    BodyTestWalk walk = walkBodyInStreamOrder(function.body);
    AOT_TEST_CHECK(index.numberOfBlocks() == walk.blocks.size() && index.numberOfValues() == walk.values.size());

    for (const BodyBlock& block : walk.blocks) {
        BodyBlock indexed = index.block(block.id());
        AOT_TEST_CHECK(indexed.words() == block.words() && indexed.id() == block.id() && indexed.firstParameter() == block.firstParameter());
        AOT_TEST_CHECK(indexed.terminator().words() == block.terminator().words() && index.terminator(block.id()).end() == block.terminator().end());
        if (block.id().index() + 1 < walk.blocks.size())
            AOT_TEST_CHECK(indexed.next().words() == walk.blocks[block.id().index() + 1].words());
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            AOT_TEST_CHECK(index.blockOf(ValueID(value)) == block.id());
    }
    for (const BodyValue& value : walk.values) {
        BodyValue indexed = index.value(value.id());
        AOT_TEST_CHECK(indexed.words() == value.words() && indexed.id() == value.id() && indexed.isParameter() == value.isParameter());
        AOT_TEST_CHECK(index.isParameter(value.id()) == value.isParameter() && index.opcodeOf(value.id()) == value.opcode());
    }
}

static constexpr ASCIILiteral bodyTestProgramText = R"IR(module m0 path="/a.ts" displayPath="a.ts"
function f0 name="walked" module=m0 parameters=[{type=anything}, {type=boolean}] results=[{type=number}] {
  b0(v0: anything, v1: boolean):
    v2 = Integer integer=-5 @1
    v3 = String string="walked" @2
    v4 = Call v0, -, v2, v3 calleeKey="walked" @3
    Jump -> next b1(v2) @4
  b1(v5) handler=b4(v0):
    LoopHint @5
    v7 = TypeTest v5 testedType=number @6
    Branch v7 -> taken b2(| 0), notTaken b3() @7
  b2(v8):
    v9 = Add v8, v2 @8
    Jump -> next b1(v9) @9
  b3():
    Switch v5 cases=[1, 2] -> case b5(v2), case b5(v5), otherwise b5(v3) @10
  b4(v10) role=CatchHandler:
    v11 = CatchValue @11
    Throw v11 @12
  b5(v12):
    Return v12 @13
}
entry module=m0
)IR"_s;

static Wire::CompileOptions bodyTestCompileOptions()
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.threadCount = 1;
    return options;
}

struct BodyTestProgram {
    BodyTestProgram()
        : build(bodyTestCompileOptions())
        , program(parseProgram(bodyTestProgramText, build))
    {
        AOT_TEST_CHECK(program);
    }

    BuildContext build;
    std::unique_ptr<Program> program;
};

static void bodyReadsBlocksAndEdges()
{
    forEachNumberingOfBodyOfEveryShape([](const Body& body) {
        checkBlocksAndEdgesOfEveryShape(body, 0);
    });
}

static void bodyCopiesFieldsIntoHolder()
{
    forEachNumberingOfBodyOfEveryShape(checkCopiesOfEveryShape);
}

static void bodyReadsSecondPositionRoles()
{
    BodyTestUnit unit(BodyTestNumbering::Backward);
    BodyTestWords words = writeBodyWithSecondPositions(unit);
    BodyTestWalk walk = walkBodyInStreamOrder(words.body(unit.remap()));
    const Vector<BodyValue>& values = walk.values;
    AOT_TEST_CHECK(walk.blocks.size() == 2 && values.size() == 7);

    checkBodyValuePositions(values[0], WrittenPositionRole::None, 10, std::nullopt, std::nullopt);
    checkBodyValuePositions(values[1], WrittenPositionRole::WhereWritten, 11, 21, std::nullopt);
    checkBodyValuePositions(values[2], WrittenPositionRole::StartOfConstruction, 12, std::nullopt, ConstructionStart { .reported = 12, .start = 22 });
    checkBodyValuePositions(values[3], WrittenPositionRole::StartOfConstruction, 13, std::nullopt, ConstructionStart { .reported = 13, .start = 23, .isWrittenAsCall = true });
    checkBodyValuePositions(values[4], WrittenPositionRole::WhereWritten, 14, 24, std::nullopt);
    checkBodyValuePositions(values[5], WrittenPositionRole::StartForCallers, 15, std::nullopt, ConstructionStart { .reported = 15, .start = 25, .isForCallers = true });
    checkBodyValuePositions(values[6], WrittenPositionRole::WhereListIsWritten, 26, std::nullopt, ConstructionStart { .reported = 26, .start = 16, .isShownAtStart = true });

    AOT_TEST_CHECK(values[0].sizeInWords() == 2 && values[1].sizeInWords() == 6 && values[5].sizeInWords() == 6);
    AOT_TEST_CHECK(values[4].opcode() == Opcode::Call && values[4].standsInTailPosition() && values[4].origin().isTailCall && !values[3].standsInTailPosition() && !values[3].origin().isTailCall);
    AOT_TEST_CHECK(values[3].as<Opcode::Call>().isDoneInPlace() && !values[4].as<Opcode::Call>().isDoneInPlace() && !values[2].as<Opcode::Construct>().newTargetOperand());
    AOT_TEST_CHECK(values[5].opcode() == Opcode::PropGet && values[5].as<Opcode::PropGet>().key() == bodyTestID<PropertyKeyID>(1) && !values[5].as<Opcode::PropGet>().isFreeName());
    AOT_TEST_CHECK(values[6].as<Opcode::IteratorOpen>().isDestructuring() && values[6].as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync && !values[6].as<Opcode::IteratorOpen>().elementCount());

    AOT_TEST_CHECK(walk.blocks[0].terminator().kind() == TerminatorKind::Return && !walk.blocks[0].terminator().as<TerminatorKind::Return>().valueOperand() && walk.blocks[0].terminator().position() == 30);
    AOT_TEST_CHECK(walk.blocks[1].role() == BlockRole::IterationCleanup && walk.blocks[1].isHandlerBlock() && walk.blocks[1].subject() == ValueID(6u));
}

static void bodyWalksAlikeWithAndWithoutIndex()
{
    auto checkBody = [](const Body& body) {
        Function function;
        AOT_TEST_CHECK(!function.hasBody());
        function.body = body;
        checkBodyIndexAgreesWithStream(function);
    };
    forEachNumberingOfBodyOfEveryShape(checkBody);

    BodyTestUnit unit(BodyTestNumbering::Forward);
    BodyTestWords words = writeBodyWithSecondPositions(unit);
    checkBody(words.body(unit.remap()));

    BodyTestProgram parsed;
    for (const Function& function : parsed.program->functions) {
        if (function.hasBody())
            checkBodyIndexAgreesWithStream(function);
    }
}

static void bodyOfParsedTextReadsProgramIDs()
{
    BodyTestProgram parsed;
    const Program& program = *parsed.program;
    const Function& function = program.functions[0];
    AOT_TEST_CHECK(function.hasBody() && program.unitsOfBodies.numberOfUnits() == 1 && &function.body.unit() == &program.unitsOfBodies.unit(0));

    BodyTestWalk walk = walkBodyInStreamOrder(function.body);
    const Vector<BodyValue>& values = walk.values;
    AOT_TEST_CHECK(walk.blocks.size() == 6 && values.size() == 13);
    AOT_TEST_CHECK(walk.blocks[0].parameterType(0) == function.parameters[0].written() && walk.blocks[0].parameterType(1) == function.parameters[1].written());
    AOT_TEST_CHECK(walk.blocks[0].parameterType(0) != walk.blocks[0].parameterType(1) && !walk.blocks[1].parameterType(0));
    AOT_TEST_CHECK(values[7].as<Opcode::TypeTest>().testedType() == function.results[0].written());

    AOT_TEST_CHECK(values[2].as<Opcode::Integer>().integer() == -5 && values[2].position() == 1 && values[6].opcode() == Opcode::LoopHint);
    AOT_TEST_CHECK(function.name && values[3].as<Opcode::String>().string() == function.name);
    auto call = values[4].as<Opcode::Call>();
    AOT_TEST_CHECK(call.calleeOperand() == ValueID(0u) && !call.thisValueOperand() && bodyTestValuesEqual(call.argumentsOperands(), { 2, 3 }) && call.calleeKey() && !call.calleeName());
    const PropertyKey& key = program.at(call.calleeKey());
    AOT_TEST_CHECK(key.kind == PropertyKeyKind::Text && key.text == function.name);

    AOT_TEST_CHECK(walk.blocks[0].terminator().next().block() == BlockID(1u) && bodyTestValuesEqual(walk.blocks[0].terminator().next().arguments(), { 2 }));
    AOT_TEST_CHECK(walk.blocks[1].handler().block() == BlockID(4u) && bodyTestValuesEqual(walk.blocks[1].handler().arguments(), { 0 }));
    AOT_TEST_CHECK(walk.blocks[1].terminator().taken().arguments().empty() && bodyTestRangeEquals(walk.blocks[1].terminator().taken().proofs(), { 0u }));
    AOT_TEST_CHECK(bodyTestRangeEquals(walk.blocks[3].terminator().as<TerminatorKind::Switch>().cases(), { int64_t { 1 }, int64_t { 2 } }));
    AOT_TEST_CHECK(walk.blocks[4].role() == BlockRole::CatchHandler && walk.blocks[5].terminator().as<TerminatorKind::Return>().valueOperand() == ValueID(12u));
}

static void bodyIndexThatVerifierLeavesIsTheOneThatScanWouldBuild()
{
    BodyTestProgram fixture;
    const Program& program = *fixture.program;
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());

    BodyIndexTable leftByVerifier(numberOfFunctions, fixture.build.parallelLoops().width());
    verifyBodies(program, fixture.build, &leftByVerifier);
    BodyIndexTable builtAlone(numberOfFunctions, 1);

    for (uint32_t number = 0; number < numberOfFunctions; ++number) {
        FunctionID function(number);
        const Function& record = program.at(function);
        AOT_TEST_CHECK(leftByVerifier.has(function) == record.hasBody());
        if (!record.hasBody())
            continue;
        const BodyIndex& left = leftByVerifier.at(function);
        const BodyIndex& built = builtAlone.build(function, record, 0);
        AOT_TEST_CHECK(left.numberOfBlocks() == built.numberOfBlocks() && left.numberOfValues() == built.numberOfValues() && left.opcodes() == built.opcodes());
        AOT_TEST_CHECK(left.hasLoops() == built.hasLoops() && left.hasIrreducibleFlow() == built.hasIrreducibleFlow());
        for (uint32_t block = 0; block < built.numberOfBlocks(); ++block) {
            BlockID id(block);
            AOT_TEST_CHECK(left.isReachable(id) == built.isReachable(id) && left.immediateDominator(id) == built.immediateDominator(id) && left.loopOf(id) == built.loopOf(id));
            AOT_TEST_CHECK(left.isLoopHead(id) == built.isLoopHead(id) && left.numberOfIncomingEdges(id) == built.numberOfIncomingEdges(id) && left.isEnteredByHandlerEdge(id) == built.isEnteredByHandlerEdge(id));
        }
        for (uint32_t value = 0; value < built.numberOfValues(); ++value) {
            ValueID id(value);
            AOT_TEST_CHECK(left.value(id).words() == built.value(id).words() && left.useCount(id) == built.useCount(id) && left.onlyUsingValue(id) == built.onlyUsingValue(id));
        }
    }
    AOT_TEST_CHECK(leftByVerifier.valuesGoneOver() == builtAlone.valuesGoneOver());
}

std::span<const NativeTest> bodyTests()
{
    static constexpr NativeTest tests[] = {
        { "body/blocks-and-edges"_s, bodyReadsBlocksAndEdges },
        { "body/copies-fields-into-holder"_s, bodyCopiesFieldsIntoHolder },
        { "body/second-position-roles"_s, bodyReadsSecondPositionRoles },
        { "body/walks-alike-with-and-without-index"_s, bodyWalksAlikeWithAndWithoutIndex },
        { "body/parsed-text-reads-program-ids"_s, bodyOfParsedTextReadsProgramIDs },
        { "body/index-that-the-verifier-leaves-is-the-one-that-the-scan-would-build"_s, bodyIndexThatVerifierLeavesIsTheOneThatScanWouldBuild },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
