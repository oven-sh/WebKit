/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineDescription.h"
#include "AOTMachineTarget.h"
#include "AOTMachineTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTSavedFrame.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRegisters.h"
#include "CPU.h"
#include "JSCJSValue.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <wtf/DataLog.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

template<typename... Context>
static void expectInMemoryTest(bool holds, const Context&... context)
{
    if (holds)
        return;
    dataLogLn("machine/memory: ", context...);
    AOT_TEST_CHECK(holds);
}

class MemoryTestBytes {
public:
    explicit MemoryTestBytes(size_t reach)
        : m_bytes(2 * reach)
        , m_reach(reach)
    {
        for (size_t index = 0; index < m_bytes.size(); ++index)
            m_bytes[index] = static_cast<uint8_t>(0x80 | (static_cast<uint32_t>(index) * 2654435761u >> 25));
    }

    uint8_t* middle() { return m_bytes.mutableSpan().data() + m_reach; }

    uint64_t read(ptrdiff_t offset, size_t size) const
    {
        uint64_t bits = 0;
        std::memcpy(&bits, m_bytes.span().data() + m_reach + offset, size);
        return bits;
    }
    void write(ptrdiff_t offset, uint64_t bits, size_t size) { std::memcpy(middle() + offset, &bits, size); }

    template<typename... Context>
    void expectSameAs(const MemoryTestBytes& expected, const Context&... context) const
    {
        for (size_t index = 0; index < m_bytes.size(); ++index)
            expectInMemoryTest(m_bytes[index] == expected.m_bytes[index], context..., ": the byte at ", static_cast<ptrdiff_t>(index) - static_cast<ptrdiff_t>(m_reach), " differs");
    }

private:
    Vector<uint8_t> m_bytes;
    size_t m_reach;
};

struct MemoryTestKind {
    size_t size() const { return bytesForWidth(accessWidthOf(opcode, type)); }

    Opcode opcode;
    Type type;
};

static constexpr MemoryTestKind memoryTestLoads[] = {
    { Load8Z, Int32 }, { Load8S, Int32 }, { Load16Z, Int32 }, { Load16S, Int32 },
    { Load, Int32 }, { Load, Int64 }, { Load, Float }, { Load, Double },
};
static constexpr MemoryTestKind memoryTestStores[] = {
    { Store8, Int32 }, { Store16, Int32 }, { Store, Int32 }, { Store, Int64 }, { Store, Float }, { Store, Double },
};
static constexpr uint64_t memoryTestStoredBits = 0xf1e2d3c4b5a69788ull;

static LValue memoryTestValueOfType(TestProcedure& test, unsigned argument, Type type)
{
    Output& out = test.out();
    LValue bits = test.argumentGPR(argument);
    switch (type.kind()) {
    case Int32:
        return out.castToInt32(bits);
    case Int64:
        return bits;
    case Float:
        return out.bitCast(out.castToInt32(bits), Float);
    case Double:
        return out.bitCast(bits, Double);
    case Vector128:
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LValue memoryTestBitsOf(Output& out, LValue value)
{
    if (value->type() == Float)
        value = out.bitCast(value, Int32);
    if (value->type() == Double)
        value = out.bitCast(value, Int64);
    return out.zeroExtPtr(value);
}

static uint64_t memoryTestBitsLoaded(const MemoryTestKind& kind, const MemoryTestBytes& bytes, ptrdiff_t offset)
{
    uint64_t bits = bytes.read(offset, kind.size());
    if (kind.opcode == Load8S)
        return static_cast<uint32_t>(static_cast<int8_t>(bits));
    if (kind.opcode == Load16S)
        return static_cast<uint32_t>(static_cast<int16_t>(bits));
    return bits;
}

static const Step* findExecuteStepInMemoryTest(TestProcedure& test, const Operation* operation)
{
    for (const Step& step : test.procedure().allocation().steps()) {
        if (step.kind == StepKind::Execute && step.operation == operation)
            return &step;
    }
    return nullptr;
}

static const Step& executeStepInMemoryTest(TestProcedure& test, const Operation* operation)
{
    const Step* step = findExecuteStepInMemoryTest(test, operation);
    AOT_TEST_CHECK(step);
    return *step;
}

static unsigned occurrencesInMemoryTestText(const String& text, ASCIILiteral word)
{
    unsigned count = 0;
    for (size_t place = text.find(word); place != notFound; place = text.find(word, static_cast<unsigned>(place + 1)))
        ++count;
    return count;
}

static void appendStoringPatchInMemoryTest(TestProcedure& test, LValue value, LValue base, int32_t offset, const HeapSet& writes)
{
    PatchOperation* patch = test.out().patchpoint(Void);
    patch->effects = Effects::none();
    patch->effects.writes = writes;
    patch->setRunsWithoutFrame(true);
    patch->appendSomeRegister(value);
    patch->appendSomeRegister(base);
    patch->setGenerator([offset](CCallHelpers& jit, const PatchParameters& parameters) {
        jit.store64(parameters[0].gpr(), CCallHelpers::Address(parameters[1].gpr(), offset));
    });
}

static void checkIndexedAccessInMemoryTest(const MemoryTestKind& kind, unsigned shift, IndexExtension extension, int32_t offset)
{
    int64_t indexFromMiddle = extension == IndexExtension::ZeroExtend32 ? 5 : -3;
    int64_t index = indexFromMiddle;
    if (extension == IndexExtension::ZeroExtend32)
        index += int64_t { 1 } << 31;
    uint64_t indexRegister = static_cast<uint64_t>(index);
    if (extension != IndexExtension::None)
        indexRegister = 0xdeadbeef00000000ull | static_cast<uint32_t>(index);
    ptrdiff_t distance = static_cast<ptrdiff_t>(indexFromMiddle * (int64_t { 1 } << shift) + offset);

    TestProcedure test;
    Output& out = test.out();
    Builder& builder = out;
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue indexValue = test.argumentGPR(1);
    switch (extension) {
    case IndexExtension::None:
        break;
    case IndexExtension::ZeroExtend32:
        indexValue = out.zeroExtPtr(out.castToInt32(indexValue));
        break;
    case IndexExtension::SignExtend32:
        indexValue = out.signExt32To64(out.castToInt32(indexValue));
        break;
    }
    LValue address = out.add(base, out.shl(indexValue, shift));

    MemoryTestBytes bytes(256);
    uintptr_t start = reinterpret_cast<uintptr_t>(bytes.middle()) - (static_cast<uintptr_t>(index - indexFromMiddle) << shift);
    if (isLoad(kind.opcode)) {
        out.ret(memoryTestBitsOf(out, builder.load(kind.opcode, kind.type, address, offset, heap)));
        uint64_t answer = compileAndRun<uint64_t>(test, start, indexRegister);
        expectInMemoryTest(answer == memoryTestBitsLoaded(kind, bytes, distance), kind.opcode, " ", kind.type, ", shift ", shift, ", extension ", static_cast<unsigned>(extension), ", offset ", offset);
        return;
    }

    builder.store(kind.opcode, memoryTestValueOfType(test, 2, kind.type), address, offset, heap);
    out.retVoid();
    MemoryTestBytes expected(256);
    expected.write(distance, memoryTestStoredBits, kind.size());
    compileAndRun<void>(test, start, indexRegister, memoryTestStoredBits);
    bytes.expectSameAs(expected, kind.opcode, " ", kind.type, ", shift ", shift, ", extension ", static_cast<unsigned>(extension), ", offset ", offset);
}

static void testMemoryIndexedAddresses()
{
    auto checkEachForm = [](const MemoryTestKind& kind) {
        for (unsigned shift = 0; shift <= 4; ++shift) {
            for (IndexExtension extension : { IndexExtension::None, IndexExtension::ZeroExtend32, IndexExtension::SignExtend32 }) {
                for (int32_t offset : { 0, 16 })
                    checkIndexedAccessInMemoryTest(kind, shift, extension, offset);
            }
        }
    };
    for (const MemoryTestKind& kind : memoryTestLoads)
        checkEachForm(kind);
    for (const MemoryTestKind& kind : memoryTestStores)
        checkEachForm(kind);
}

static void testMemoryWordsOfInstance()
{
    {
        TestInstance instance;
        instance.words[3] = 1000;
        instance.wordsBelow[63] = 1;
        TestProcedure test;
        Output& out = test.out();
        const AbstractHeap& heap = test.heaps().AOTInstance_mutableFields;
        LValue base = out.argumentRegister(instanceGPR);
        LValue above = out.load64(out.address(heap, base, 24));
        LValue below = out.load64(out.address(heap, out.add(base, -16), 8));
        LValue stored = out.store64(out.add(above, below), out.address(heap, base, 32));
        out.retVoid();
        compileOrFail(test, instance.pointer());

        AOT_TEST_CHECK(below->child(0) == base);
        AOT_TEST_CHECK(below->offset() == -8);
        AOT_TEST_CHECK(executeStepInMemoryTest(test, stored).sources[1] == Location::reg(instanceGPR));
        test.call<void>();
        AOT_TEST_CHECK(instance.words[4] == 1001);
    }

    TestProcedure test;
    Output& out = test.out();
    LValue stored = out.store64(test.argumentGPR(0), out.instanceWord(test.heaps().AOTInstance_mutableFields, 3));
    out.retVoid();
    prepareOrFail(test);
    AOT_TEST_CHECK(executeStepInMemoryTest(test, stored).sources[1].isImmediate());
}

static void testMemoryConstantData()
{
    uint64_t datum = 0x0123456789abcdefull;
    TestProcedure test;
    Output& out = test.out();
    uint32_t constant = out.addConstantData(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&datum), sizeof(datum)), ConstantKind::Unlabeled);
    out.ret(out.load64(out.address(test.heaps().AOTImage_tables, out.relocatable(SymbolSpace::Constant, constant))));
    AOT_TEST_CHECK(compileAndRun<uint64_t>(test) == datum);
}

static void testMemoryConstantDataSaysWhatItIs()
{
    uint64_t datum = 0x0123456789abcdefull;
    double number = 0.1;
    AOT_TEST_CHECK(!findPinnedConstant(std::bit_cast<uint64_t>(number)));
    TestProcedure test;
    Output& out = test.out();
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(&datum), sizeof(datum));
    uint32_t constant = out.addConstantData(bytes, ConstantKind::ShapeSet);
    AOT_TEST_CHECK(out.addConstantData(bytes, ConstantKind::OffsetTable) == constant);
    out.ret(out.doubleAdd(test.argumentFPR(0), out.constDouble(number)));
    compileOrFail(test);
    AOT_TEST_CHECK(test.call<double>(2.5) == 2.5 + number);

    decltype(RareListsOfBody::constants) described;
    describeConstants(test.procedure(), described);
    AOT_TEST_CHECK(described.size() == 2 && !constant);
    AOT_TEST_CHECK(described[0].kind == ConstantKind::ShapeSet);
    AOT_TEST_CHECK(described[1].kind == ConstantKind::Number);
    AOT_TEST_CHECK(described[1].bytes.size() == sizeof(number) && !memcmp(described[1].bytes.span().data(), &number, sizeof(number)));
}

static void testMemoryConstantAddedToIndexMovesIntoOffset()
{
    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue index = test.argumentGPR(1);

    LValue scaled = out.load64(out.address(heap, out.add(base, out.shl(out.add(index, 3), 3)), 8));
    AOT_TEST_CHECK(scaled->offset() == 32);
    AOT_TEST_CHECK(scaled->child(0)->opcode() == Add && scaled->child(0)->child(0) == base);
    AOT_TEST_CHECK(scaled->child(0)->child(1)->opcode() == Shl && scaled->child(0)->child(1)->child(0) == index);
    AOT_TEST_CHECK(out.load64(out.address(heap, out.add(base, out.shl(out.add(index, 4), 3)))) == scaled);

    LValue byte = out.load8ZeroExt32(out.address(heap, out.add(base, out.add(index, -5))));
    AOT_TEST_CHECK(byte->offset() == -5);
    AOT_TEST_CHECK(byte->child(0)->opcode() == Add && byte->child(0)->child(0) == base && byte->child(0)->child(1) == index);

    LValue farAway = out.load8ZeroExt32(out.address(heap, out.add(base, out.shl(out.add(index, int64_t { 1 } << 30), 3))));
    AOT_TEST_CHECK(!farAway->offset());

    out.ret(out.add(scaled, out.zeroExtPtr(byte)));

    MemoryTestBytes bytes(128);
    uint64_t expected = bytes.read(-2 * 8 + 32, 8) + bytes.read(-2 - 5, 1);
    AOT_TEST_CHECK(compileAndRun<uint64_t>(test, bytes.middle(), int64_t { -2 }) == expected);
}

static void testMemoryTableOfImageIsBaseOfIndexedLoad()
{
    AOT_TEST_CHECK(isEncodableIndexedAddress(Width32, 2, IndexExtension::None, 0));

    static constexpr uint32_t words[] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
    TestProcedure test;
    Output& out = test.out();
    LValue table = out.relocatable(SymbolSpace::Constant, out.addConstantData(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(words), sizeof(words)), ConstantKind::ShapeBitmap));
    LValue element = out.lShr(out.castToInt32(test.argumentGPR(0)), 5);
    LValue extended = out.zeroExtPtr(element);
    LValue shifted = out.shl(extended, 2);
    LValue address = out.add(table, shifted);
    LValue loaded = out.load32(out.address(test.heaps().AOTImage_tables, address));
    LValue result = out.zeroExtPtr(loaded);
    out.ret(result);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    AOT_TEST_CHECK(table->opcode() == AddressOf && table->hasNoPosition() && address->child(0) == table);
    AOT_TEST_CHECK(liveness.isFused(address) && liveness.isFused(shifted));
    AOT_TEST_CHECK(!findExecuteStepInMemoryTest(test, address) && !findExecuteStepInMemoryTest(test, shifted));
    const Step& step = executeStepInMemoryTest(test, liveness.isFused(loaded) ? result : loaded);
    AOT_TEST_CHECK(step.sources[0].isRegister() && step.sources[1].isRegister() && step.sources[0] != step.sources[1]);
    for (uint64_t index = 0; index < 128; index += 31)
        AOT_TEST_CHECK(test.call<uint64_t>(index) == words[index >> 5]);
}

enum class MemoryTestSecondUse : uint8_t { OfSum, OfShiftedIndex, BySecondAccess };

static void checkAddressWithTwoUsesInMemoryTest(MemoryTestSecondUse secondUse)
{
    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue shifted = out.shl(test.argumentGPR(1), 3);
    LValue address = out.add(base, shifted);
    LValue loaded = out.load64(out.address(heap, address));

    MemoryTestBytes bytes(64);
    MemoryTestBytes expected(64);
    uint64_t answer = expected.read(16, 8);
    LValue usedTwice = address;
    switch (secondUse) {
    case MemoryTestSecondUse::OfSum:
        out.ret(out.add(loaded, address));
        answer += reinterpret_cast<uintptr_t>(bytes.middle()) + 16;
        break;
    case MemoryTestSecondUse::OfShiftedIndex:
        out.ret(out.add(loaded, shifted));
        answer += 16;
        usedTwice = shifted;
        break;
    case MemoryTestSecondUse::BySecondAccess:
        out.store64(loaded, out.address(heap, address, 8));
        out.ret(loaded);
        expected.write(24, answer, 8);
        break;
    }
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    if (isX86_64() && secondUse == MemoryTestSecondUse::BySecondAccess)
        AOT_TEST_CHECK(address->useCount() == 1 && liveness.isFused(address));
    else {
        AOT_TEST_CHECK(usedTwice->useCount() == 2);
        AOT_TEST_CHECK(!liveness.isFused(usedTwice));
        AOT_TEST_CHECK(liveness.isFused(shifted) == (usedTwice == address) && addsScaledIndex(liveness, address) == (usedTwice == address));
    }
    AOT_TEST_CHECK(test.call<uint64_t>(bytes.middle(), int64_t { 2 }) == answer);
    bytes.expectSameAs(expected, "an address with two uses");
}

static void testMemoryAddressWithTwoUsesIsNotFused()
{
    checkAddressWithTwoUsesInMemoryTest(MemoryTestSecondUse::OfSum);
    checkAddressWithTwoUsesInMemoryTest(MemoryTestSecondUse::OfShiftedIndex);
    checkAddressWithTwoUsesInMemoryTest(MemoryTestSecondUse::BySecondAccess);
}

static void testMemoryExtensionOfIndexIsFusedWhereRegisterHoldsIt()
{
    for (bool upperHalfIsClear : { true, false }) {
        TestProcedure test;
        Output& out = test.out();
        LValue base = test.argumentGPR(0);
        LValue index = out.castToInt32(test.argumentGPR(1));
        if (upperHalfIsClear)
            index = out.add(index, out.constInt32(1));
        LValue extended = out.zeroExtPtr(index);
        LValue address = out.add(base, out.shl(extended, 3));
        out.ret(out.load64(out.address(test.heaps().root, address)));
        compileOrFail(test);

        const Liveness& liveness = test.procedure().liveness();
        AOT_TEST_CHECK(extended->opcode() == ZExt32 && clearsUpperHalf(index->opcode()) == upperHalfIsClear);
        AOT_TEST_CHECK(liveness.isFused(address) && liveness.isFused(extended) == (upperHalfIsClear || isARM64()));

        uint64_t words[4] = { 10, 11, 12, 13 };
        AOT_TEST_CHECK(test.call<uint64_t>(&words[0], uint64_t { 0xdeadbeef00000001 }) == (upperHalfIsClear ? 12u : 11u));
    }
}

static void testMemoryAliasedAddresses()
{
    TestProcedure test;
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue other = test.argumentGPR(1);
    LValue index = out.bitAnd(out.castToInt32(test.argumentGPR(2)), out.constInt32(3));
    LValue extended = out.zeroExtPtr(index);
    LValue first = out.load64(out.address(test.heaps().root, out.add(base, out.shl(extended, 3))));
    LValue second = out.load32(out.address(test.heaps().root, out.add(other, out.shl(extended, 2))));
    out.ret(out.add(out.add(first, out.zeroExtPtr(second)), extended));
    compileOrFail(test);

    AOT_TEST_CHECK(extended->opcode() == ZExt32 && extended->useCount() > 1);
    const Liveness& liveness = test.procedure().liveness();
    AOT_TEST_CHECK(liveness.isFused(first->child(0)) && liveness.isFused(second->child(0)) && liveness.isFused(extended));

    uint64_t words[4] = { 10, 11, 12, 13 };
    uint32_t others[4] = { 100, 200, 300, 400 };
    AOT_TEST_CHECK(test.call<uint64_t>(&words[0], &others[0], uint64_t { 0xdeadbeefdeadbee2 }) == 12 + 300 + 2);
}

static void testMemoryNumberIsMadeOfLowHalfOfRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LValue word = test.argumentGPR(0);
    LValue lowHalf = out.castToInt32(word);
    LValue number = out.intToDouble(lowHalf);
    out.ret(out.add(out.bitCast(number, Int64), word));
    compileOrFail(test);

    AOT_TEST_CHECK(lowHalf->opcode() == Trunc && test.procedure().liveness().isFused(lowHalf) && !findExecuteStepInMemoryTest(test, lowHalf));
    for (uint64_t bits : { uint64_t { 0xfffe000000000007 }, uint64_t { 0xfffe0000fffffffb } })
        AOT_TEST_CHECK(test.call<uint64_t>(bits) == std::bit_cast<uint64_t>(static_cast<double>(static_cast<int32_t>(bits))) + bits);
}

struct MemoryTestTestedLoad {
    Opcode opcode;
    Type type;
    size_t numberOfBytes;
    bool extendsSign;
    std::array<int64_t, 4> constants;
};

static constexpr MemoryTestTestedLoad testedLoadsOfMemoryTest[] = {
    { Load8Z, Int32, 1, false, { 0, 2, 128, 255 } },
    { Load8S, Int32, 1, true, { -128, -1, 0, 127 } },
    { Load16Z, Int32, 2, false, { 0, 1, 32, 127 } },
    { Load16S, Int32, 2, true, { -128, -1, 0, 127 } },
    { Load, Int32, 4, true, { std::numeric_limits<int32_t>::min(), -1, 0, 70000 } },
    { Load, Int64, 8, true, { std::numeric_limits<int32_t>::min(), -1, 0, 70000 } },
};

static int64_t valueOfTestedLoadInMemoryTest(const MemoryTestTestedLoad& load, uint64_t bits)
{
    unsigned unusedBits = 64 - 8 * static_cast<unsigned>(load.numberOfBytes);
    if (load.extendsSign)
        return static_cast<int64_t>(bits << unusedBits) >> unusedBits;
    return static_cast<int64_t>(bits << unusedBits >> unusedBits);
}

static bool holdsInMemoryTest(Opcode relation, Type type, int64_t left, int64_t right)
{
    uint64_t unsignedLeft = type == Int32 ? static_cast<uint32_t>(left) : static_cast<uint64_t>(left);
    uint64_t unsignedRight = type == Int32 ? static_cast<uint32_t>(right) : static_cast<uint64_t>(right);
    if (relation == Equal)
        return left == right;
    if (relation == NotEqual)
        return left != right;
    if (relation == LessThan)
        return left < right;
    if (relation == LessEqual)
        return left <= right;
    if (relation == GreaterThan)
        return left > right;
    if (relation == GreaterEqual)
        return left >= right;
    if (relation == Below)
        return unsignedLeft < unsignedRight;
    if (relation == BelowEqual)
        return unsignedLeft <= unsignedRight;
    if (relation == Above)
        return unsignedLeft > unsignedRight;
    RELEASE_ASSERT(relation == AboveEqual);
    return unsignedLeft >= unsignedRight;
}

static LValue constantOfTypeInMemoryTest(Output& out, Type type, int64_t constant)
{
    return type == Int32 ? out.constInt32(static_cast<int32_t>(constant)) : out.constInt64(constant);
}

enum class MemoryTestUserOfTest : uint8_t { Branch, Select };
enum class MemoryTestAddress : uint8_t { BaseAndOffset, Indexed };

template<typename MakeTest, typename Expects, typename... Context>
static void checkTestOfLoadInMemoryTest(const MemoryTestTestedLoad& load, MemoryTestUserOfTest user, MemoryTestAddress address, const MakeTest& makeTest, const Expects& expects, const Context&... context)
{
    constexpr ptrdiff_t offset = 5;
    TestProcedure test;
    Output& out = test.out();
    Builder& builder = out;
    LValue base = test.argumentGPR(0);
    if (address == MemoryTestAddress::Indexed)
        base = out.add(base, out.shl(test.argumentGPR(1), 3));
    LValue loaded = builder.load(load.opcode, load.type, base, offset, test.heaps().root);
    LValue tested = makeTest(out, loaded);
    bool isDecided = tested->hasInt();
    if (user == MemoryTestUserOfTest::Select)
        out.ret(builder.select(tested, out.constInt64(10), out.constInt64(20)));
    else {
        LBasicBlock holds = out.newBlock();
        LBasicBlock fails = out.newBlock();
        out.branch(tested, normalEdge(holds), normalEdge(fails));
        out.appendTo(holds);
        out.ret(out.constInt64(10));
        out.appendTo(fails);
        out.ret(out.constInt64(20));
    }
    compileOrFail(test);
    expectInMemoryTest(isDecided || test.procedure().liveness().isFused(loaded) == isX86_64(), context..., ": the load is fused on x86-64 and nowhere else");

    MemoryTestBytes bytes(64);
    for (uint64_t bits : { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 2 }, uint64_t { 0x20 }, uint64_t { 0x7f }, uint64_t { 0x80 }, uint64_t { 0xff }, uint64_t { 0x100 }, uint64_t { 0x7fff }, uint64_t { 0x8000 }, uint64_t { 0xffff }, uint64_t { 70000 },
        uint64_t { 0x7fffffff }, uint64_t { 0x80000000 }, uint64_t { 0xe0000000 }, uint64_t { 0xffffffff }, uint64_t { 0x100000000 }, uint64_t { 0xffffffff80000000 }, UINT64_MAX }) {
        bytes.write(offset + 16, bits, load.numberOfBytes);
        int64_t value = valueOfTestedLoadInMemoryTest(load, bytes.read(offset + 16, load.numberOfBytes));
        int64_t answer = address == MemoryTestAddress::Indexed ? test.call<int64_t>(bytes.middle(), int64_t { 2 }) : test.call<int64_t>(bytes.middle() + 16, int64_t { 0 });
        expectInMemoryTest(answer == (expects(value) ? 10 : 20), context..., ": the word ", value, " is tested wrongly");
    }
}

static void testMemoryComparisonReadsLoadWhereItLies()
{
    for (const MemoryTestTestedLoad& load : testedLoadsOfMemoryTest) {
        for (Opcode relation : { Equal, NotEqual, LessThan, LessEqual, GreaterThan, GreaterEqual, Below, BelowEqual, Above, AboveEqual }) {
            for (int64_t constant : load.constants) {
                for (MemoryTestAddress address : { MemoryTestAddress::BaseAndOffset, MemoryTestAddress::Indexed }) {
                    checkTestOfLoadInMemoryTest(load, MemoryTestUserOfTest::Branch, address, [&](Output& out, LValue loaded) {
                        return out.binary(relation, loaded, constantOfTypeInMemoryTest(out, load.type, constant));
                    }, [&](int64_t value) {
                        return holdsInMemoryTest(relation, load.type, value, constant);
                    }, load.opcode, " ", load.type, " ", relation, " ", constant);
                }
            }
        }
        checkTestOfLoadInMemoryTest(load, MemoryTestUserOfTest::Select, MemoryTestAddress::BaseAndOffset, [&](Output& out, LValue loaded) {
            return out.equal(loaded, constantOfTypeInMemoryTest(out, load.type, load.constants[1]));
        }, [&](int64_t value) {
            return value == load.constants[1];
        }, "a Select on ", load.opcode, " ", load.type);
    }
}

static void testMemoryBitTestReadsLoadWhereItLies()
{
    for (const MemoryTestTestedLoad& load : testedLoadsOfMemoryTest) {
        uint64_t bitsOfWord = load.numberOfBytes == 8 ? UINT64_MAX : (uint64_t { 1 } << (8 * load.numberOfBytes)) - 1;
        for (uint64_t mask : { uint64_t { 1 }, uint64_t { 0x40 }, uint64_t { 0x80 }, uint64_t { 0xe0 }, uint64_t { 0x4000 }, uint64_t { 0x00ff0000 }, uint64_t { 0x60000000 }, uint64_t { 0x7fff0001 } }) {
            if (mask & ~bitsOfWord)
                continue;
            for (bool testsForZero : { false, true }) {
                for (MemoryTestAddress address : { MemoryTestAddress::BaseAndOffset, MemoryTestAddress::Indexed }) {
                    checkTestOfLoadInMemoryTest(load, MemoryTestUserOfTest::Branch, address, [&](Output& out, LValue loaded) {
                        LValue bits = constantOfTypeInMemoryTest(out, load.type, static_cast<int64_t>(mask));
                        if (load.type == Int32)
                            return testsForZero ? out.testIsZero32(loaded, bits) : out.testNonZero32(loaded, bits);
                        return testsForZero ? out.testIsZero64(loaded, bits) : out.testNonZero64(loaded, bits);
                    }, [&](int64_t value) {
                        return !(static_cast<uint64_t>(value) & mask) == testsForZero;
                    }, load.opcode, " ", load.type, " & ", mask, testsForZero ? " is zero" : " is not zero");
                }
            }
        }
        checkTestOfLoadInMemoryTest(load, MemoryTestUserOfTest::Branch, MemoryTestAddress::BaseAndOffset, [&](Output&, LValue loaded) {
            return loaded;
        }, [&](int64_t value) {
            return !!value;
        }, "a Branch on ", load.opcode, " ", load.type);
    }
}

static void testMemoryComparisonWithRegisterReadsLoadWhereItLies()
{
    for (Type type : { Type(Int32), Type(Int64) }) {
        for (bool loadStandsOnLeft : { true, false }) {
            for (Opcode relation : { Equal, LessThan, GreaterEqual, Below, Above }) {
                TestProcedure test;
                Output& out = test.out();
                Builder& builder = out;
                LBasicBlock holds = out.newBlock();
                LBasicBlock fails = out.newBlock();
                LValue other = memoryTestValueOfType(test, 1, type);
                LValue loaded = builder.load(Load, type, test.argumentGPR(0), 8, test.heaps().root);
                out.branch(loadStandsOnLeft ? out.binary(relation, loaded, other) : out.binary(relation, other, loaded), normalEdge(holds), normalEdge(fails));
                out.appendTo(holds);
                out.ret(out.constInt64(10));
                out.appendTo(fails);
                out.ret(out.constInt64(20));
                compileOrFail(test);
                AOT_TEST_CHECK(test.procedure().liveness().isFused(loaded) == isX86_64());

                for (int64_t word : { int64_t { -5 }, int64_t { 0 }, int64_t { 7 } }) {
                    for (int64_t beside : { int64_t { -5 }, int64_t { 0 }, int64_t { 7 }, int64_t { 9 } }) {
                        int64_t words[2] = { -1, word };
                        bool expected = loadStandsOnLeft ? holdsInMemoryTest(relation, type, word, beside) : holdsInMemoryTest(relation, type, beside, word);
                        AOT_TEST_CHECK(test.call<int64_t>(&words[0], beside) == (expected ? 10 : 20));
                    }
                }
            }
        }
    }
}

static void testMemoryTestedLoadMovesBehindWhatOnlyKeepsValueAlive()
{
    for (bool declaresWrite : { false, true }) {
        TestProcedure test;
        Output& out = test.out();
        const AbstractHeap& heap = test.heaps().root;
        LBasicBlock holds = out.newBlock();
        LBasicBlock fails = out.newBlock();
        LValue base = test.argumentGPR(0);
        LValue owner = test.argumentGPR(1);
        LValue loaded = out.load8ZeroExt32(out.address(heap, base, 5));
        PatchOperation* keeper = out.patch(Void);
        keeper->append(owner, ValueRep::ColdAny);
        keeper->effects = Effects::none();
        keeper->effects.writesLocalState = true;
        keeper->effects.reads = HeapSet::top();
        if (declaresWrite)
            keeper->effects.writes = HeapSet::top();
        keeper->setRunsWithoutFrame(true);
        keeper->onlyKeepsChildrenAlive = true;
        keeper->setGenerator([](CCallHelpers&, const PatchParameters&) { });
        out.branch(out.equal(loaded, out.constInt32(32)), normalEdge(holds), normalEdge(fails));
        out.appendTo(holds);
        out.ret(out.constInt64(10));
        out.appendTo(fails);
        out.ret(out.constInt64(20));
        compileOrFail(test);
        AOT_TEST_CHECK(test.procedure().liveness().isFused(loaded) == (isX86_64() && !declaresWrite));

        uint8_t bytes[8] = { 0, 0, 0, 0, 0, 32, 0, 0 };
        AOT_TEST_CHECK(test.call<int64_t>(&bytes[0], &bytes[0]) == 10);
        bytes[5] = 33;
        AOT_TEST_CHECK(test.call<int64_t>(&bytes[0], &bytes[0]) == 20);
    }
}

static void testMemoryFenceEndsNoLoad()
{
    uint64_t words[2] = { 3, 4 };
    TestProcedure test;
    Output& out = test.out();
    const AOTHeaps& heaps = test.heaps();
    LValue base = test.argumentGPR(0);
    LValue length = out.load64(out.address(heaps.Butterfly_publicLength, base, 0));
    LValue capacity = out.load64(out.address(heaps.Butterfly_vectorLength, base, 8));
    out.fence();
    AOT_TEST_CHECK(out.load64(out.address(heaps.Butterfly_publicLength, base, 0)) == length);
    AOT_TEST_CHECK(out.load64(out.address(heaps.Butterfly_vectorLength, base, 8)) == capacity);
    AOT_TEST_CHECK(occurrencesInMemoryTestText(test.printedProcedure(), "= Load("_s) == 2);
    AOT_TEST_CHECK(occurrencesInMemoryTestText(test.printedProcedure(), "= Fence("_s) == 1);

    out.ret(out.add(length, capacity));
    AOT_TEST_CHECK(compileAndRun<uint64_t>(test, &words[0]) == 7);
}

static void checkRecordOfTrapInMemoryTest(TestProcedure& test, const Operation* trappingLoad, const Operation* faulting)
{
    std::span<const TrappingAccess> accesses = test.procedure().trappingAccesses();
    AOT_TEST_CHECK(accesses.size() == 1);
    const TrappingAccess& access = accesses[0];

    const Step& step = executeStepInMemoryTest(test, faulting);
    unsigned firstLeafOfAddress = isStore(faulting->opcode()) ? 1 : 0;
    bool goesThroughCageBase = step.sources[firstLeafOfAddress] == Location::reg(cellCageBaseGPR);
    Location reference = step.sources[firstLeafOfAddress + goesThroughCageBase];
    AOT_TEST_CHECK(reference.isRegister());
    AOT_TEST_CHECK(access.instruction.isSet());
    AOT_TEST_CHECK(access.origin == trappingLoad->origin());
    AOT_TEST_CHECK(access.reference == reference.gpr());
    AOT_TEST_CHECK(access.referenceIsNarrow == goesThroughCageBase);
}

static void testMemoryUnusedTrappingLoadStays()
{
    TestProcedure test;
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue answer = test.argumentGPR(1);
    out.setOrigin(Origin(7));
    LValue trappingLoad = out.load8ZeroExt32Trapping(out.address(test.heaps().JSCell_typeInfoType, base, 5));
    out.setOrigin(Origin(8));
    LValue plainLoad = out.load8ZeroExt32(out.address(test.heaps().JSCell_typeInfoFlags, base, 6));
    out.ret(answer);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    AOT_TEST_CHECK(trappingLoad->traps());
    AOT_TEST_CHECK(!trappingLoad->useCount());
    AOT_TEST_CHECK(!plainLoad->useCount());
    AOT_TEST_CHECK(!liveness.isDropped(trappingLoad));
    AOT_TEST_CHECK(liveness.isDropped(plainLoad));
    checkRecordOfTrapInMemoryTest(test, trappingLoad, trappingLoad);
    AOT_TEST_CHECK(test.procedure().trappingAccesses()[0].reference == test.procedure().allocation().registerAtDefinitionOf(base).gpr());

    MemoryTestBytes bytes(64);
    AOT_TEST_CHECK(test.call<uint64_t>(bytes.middle(), uint64_t { 3 }) == 3);
}

static void checkTrapThroughNarrowReferenceInMemoryTest(const MemoryTestKind& kind, unsigned shift, int32_t offset)
{
    TestProcedure test;
    Output& out = test.out();
    Builder& builder = out;
    LValue narrow = out.castToInt32(test.argumentGPR(0));
    LValue extended = out.zeroExtPtr(narrow);
    LValue scaled = out.shl(extended, shift);
    LValue pointer = out.add(out.argumentRegister(cellCageBaseGPR), scaled);
    out.setOrigin(Origin(9));
    LValue trappingLoad = builder.load(kind.opcode, kind.type, pointer, offset, test.heaps().JSCell_typeInfoType, Builder::Trapping::Yes);
    out.retVoid();
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    Width width = accessWidthOf(kind.opcode, kind.type);
    bool isFused = liveness.isFused(pointer);
    bool isEncodable = isEncodableIndexedAddress(width, shift, IndexExtension::ZeroExtend32, offset) || isEncodableIndexedAddress(width, shift, IndexExtension::None, offset);
    AOT_TEST_CHECK(!liveness.isDropped(trappingLoad));
    expectInMemoryTest(isFused || !shift || !isEncodable, "the address of a trapping ", kind.opcode, " ", kind.type, ", shift ", shift, ", offset ", offset, " is not fused");
    AOT_TEST_CHECK(!isFused || !shift || liveness.isFused(scaled));
    checkRecordOfTrapInMemoryTest(test, trappingLoad, trappingLoad);
    AOT_TEST_CHECK(test.procedure().trappingAccesses()[0].referenceIsNarrow == isFused);
}

static void testMemoryTrappingLoadNamesItsReference()
{
    static constexpr MemoryTestKind trappingLoads[] = { { Load8Z, Int32 }, { Load, Int64 } };
    AOT_TEST_CHECK(isEncodableIndexedAddress(Width64, cell32Shift, IndexExtension::None, 0));
    for (const MemoryTestKind& kind : trappingLoads) {
        for (unsigned shift : { 0u, cell32Shift }) {
            for (int32_t offset : { 0, 5 })
                checkTrapThroughNarrowReferenceInMemoryTest(kind, shift, offset);
        }
    }

    TestProcedure test;
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue address = out.add(base, out.shl(test.argumentGPR(1), 3));
    out.setOrigin(Origin(10));
    LValue trappingLoad = out.load8ZeroExt32Trapping(out.address(test.heaps().JSCell_typeInfoType, address));
    out.ret(out.zeroExtPtr(trappingLoad));
    compileOrFail(test);

    checkRecordOfTrapInMemoryTest(test, trappingLoad, trappingLoad);
    if (test.procedure().liveness().isFused(address))
        AOT_TEST_CHECK(test.procedure().trappingAccesses()[0].reference == test.procedure().allocation().registerAtDefinitionOf(base).gpr());
    MemoryTestBytes bytes(64);
    AOT_TEST_CHECK(test.call<uint64_t>(bytes.middle(), int64_t { 2 }) == bytes.read(16, 1));
}

enum class TrapInMemoryTest : uint8_t { Stays, MayBeHandedOver, IsHandedOver };

template<typename Functor>
static void checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest expected, const Functor& appendRest, const ProcedureTraits& traits = { })
{
    TestProcedure test(traits);
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue otherBase = test.argumentGPR(1);
    out.setOrigin(Origin(11));
    LValue trappingLoad = out.load8ZeroExt32Trapping(out.address(test.heaps().JSCell_typeInfoType, base, 5));
    out.setOrigin(Origin(12));
    LValue access = appendRest(out, test.heaps().root, base, otherBase);
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    if (!liveness.isDropped(trappingLoad)) {
        AOT_TEST_CHECK(expected != TrapInMemoryTest::IsHandedOver);
        checkRecordOfTrapInMemoryTest(test, trappingLoad, trappingLoad);
        return;
    }
    AOT_TEST_CHECK(expected != TrapInMemoryTest::Stays);
    AOT_TEST_CHECK(liveness.standsInItsBlock(access));
    AOT_TEST_CHECK(liveness.of(access).has(ValueFlag::TakesOverTrap));
    AOT_TEST_CHECK(liveness.trapTakenOverBy(access) == trappingLoad);
    checkRecordOfTrapInMemoryTest(test, trappingLoad, access);
}

static void testMemoryTrapIsHandedOverOrStays()
{
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::MayBeHandedOver, [](Output& out, const AbstractHeap& heap, LValue base, LValue) {
        LValue loaded = out.load64(out.address(heap, base, 8));
        out.ret(loaded);
        return loaded;
    });
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::MayBeHandedOver, [](Output& out, const AbstractHeap& heap, LValue base, LValue otherBase) {
        LValue stored = out.store64(otherBase, out.address(heap, base, 8));
        out.retVoid();
        return stored;
    });
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::Stays, [](Output& out, const AbstractHeap& heap, LValue base, LValue) {
        LValue loaded = out.load64(out.address(heap, base, static_cast<ptrdiff_t>(trapReach()) - 4));
        out.ret(loaded);
        return loaded;
    });
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::Stays, [](Output& out, const AbstractHeap& heap, LValue, LValue otherBase) {
        LValue loaded = out.load64(out.address(heap, otherBase, 8));
        out.ret(loaded);
        return loaded;
    });
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::Stays, [](Output& out, const AbstractHeap& heap, LValue base, LValue otherBase) {
        out.store64(base, out.address(heap, otherBase));
        LValue loaded = out.load64(out.address(heap, base, 8));
        out.ret(loaded);
        return loaded;
    });
}

static LValue appendPatchAndLoadBehindTrapInMemoryTest(Output& out, const AbstractHeap& heap, LValue base, LValue operand, const Effects& effects)
{
    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(operand);
    patch->resultConstraints = { ValueRep::SomeEarlyRegister };
    patch->effects = effects;
    patch->setRunsWithoutFrame(true);
    patch->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
        if (params[1].isFPR())
            jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
        else
            jit.move(params[1].gpr(), params[0].gpr());
    });
    LValue loaded = out.load64(out.address(heap, base, 8));
    out.ret(out.add(out.add(loaded, loaded), patch));
    return loaded;
}

static void testMemoryTrapIsHandedOverPastPatchWithoutEffects()
{
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::IsHandedOver, [](Output& out, const AbstractHeap& heap, LValue base, LValue) {
        return appendPatchAndLoadBehindTrapInMemoryTest(out, heap, base, out.constDouble(2.5), Effects::none());
    });
}

enum class PatchBehindTrapInMemoryTest : uint8_t { Reads, Writes, TakesInteger, TakesBase, Suspends };

template<PatchBehindTrapInMemoryTest kind>
static void testMemoryTrapStaysAheadOfPatch()
{
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = kind == PatchBehindTrapInMemoryTest::Suspends;
    checkTrapAheadOfAccessInMemoryTest(TrapInMemoryTest::Stays, [&](Output& out, const AbstractHeap& heap, LValue base, LValue otherBase) {
        if constexpr (kind == PatchBehindTrapInMemoryTest::Suspends) {
            constexpr GPRReg result = GPRInfo::returnValueGPR;
            RegisterFile file = registerFileFor(traits);
            RegisterMask lost = (file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~maskOf(Reg(result));
            if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
                lost &= ~maskOf(Reg(SavedFrame::preservedRegister));
            PatchOperation* suspension = out.patch(Int64);
            suspension->resultConstraints = { ValueRep::reg(result) };
            suspension->effects = Effects::none();
            suspension->clobberLate(lost);
            suspension->setIsSuspension(true);
            suspension->setGenerator([](CCallHelpers& jit, const PatchParameters&) {
                jit.move(CCallHelpers::TrustedImm32(0), result);
            });
            LValue loaded = out.load64(out.address(heap, base, 8));
            out.ret(out.add(out.add(loaded, loaded), suspension));
            return loaded;
        } else {
            Effects effects = Effects::none();
            if constexpr (kind == PatchBehindTrapInMemoryTest::Reads)
                effects.reads = HeapSet::top();
            if constexpr (kind == PatchBehindTrapInMemoryTest::Writes)
                effects.writes = HeapSet::top();
            LValue operand = kind == PatchBehindTrapInMemoryTest::TakesInteger ? otherBase : kind == PatchBehindTrapInMemoryTest::TakesBase ? base : out.constDouble(2.5);
            return appendPatchAndLoadBehindTrapInMemoryTest(out, heap, base, operand, effects);
        }
    }, traits);
}

struct MemoryTestStore {
    Type type;
    int32_t offset;
};

struct MemoryTestStorePair {
    MemoryTestStore first;
    MemoryTestStore second;
};

static ptrdiff_t runTwoStoresInMemoryTest(const MemoryTestStorePair& pair)
{
    constexpr uint64_t firstBits = 0x1112131415161718ull;
    constexpr uint64_t secondBits = 0x2122232425262728ull;

    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue firstValue = memoryTestValueOfType(test, 1, pair.first.type);
    LValue secondValue = memoryTestValueOfType(test, 2, pair.second.type);
    out.store(firstValue, out.address(heap, base, pair.first.offset));
    out.store(secondValue, out.address(heap, base, pair.second.offset));
    out.retVoid();

    MemoryTestBytes bytes(1024);
    MemoryTestBytes expected(1024);
    expected.write(pair.first.offset, firstBits, sizeOf(pair.first.type));
    expected.write(pair.second.offset, secondBits, sizeOf(pair.second.type));
    compileAndRun<void>(test, bytes.middle(), firstBits, secondBits);
    bytes.expectSameAs(expected, pair.first.type, " at ", pair.first.offset, ", then ", pair.second.type, " at ", pair.second.offset);

    const Procedure& procedure = test.procedure();
    AOT_TEST_CHECK(procedure.rareCodeStart().isSet());
    return MacroAssembler::differenceBetween(procedure.entrypointLabel(0), procedure.rareCodeStart());
}

static void testMemoryNeighboringStoresPair()
{
    ptrdiff_t bytesOfNeighbors = runTwoStoresInMemoryTest({ { Int64, 0 }, { Int64, 8 } });
    ptrdiff_t bytesOfStoresApart = runTwoStoresInMemoryTest({ { Int64, 0 }, { Int64, 24 } });
    AOT_TEST_CHECK(!isARM64() || bytesOfNeighbors + 4 == bytesOfStoresApart);

    static constexpr MemoryTestStorePair pairs[] = {
        { { Int64, 8 }, { Int64, 0 } }, { { Int64, -8 }, { Int64, 0 } },
        { { Int64, 504 }, { Int64, 512 } }, { { Int64, 512 }, { Int64, 520 } }, { { Int64, -512 }, { Int64, -504 } }, { { Int64, -520 }, { Int64, -512 } },
        { { Int64, 4 }, { Int64, 12 } }, { { Int64, 0 }, { Int64, 4 } }, { { Int64, 0 }, { Int64, 0 } },
        { { Int32, 0 }, { Int32, 4 } }, { { Int32, 4 }, { Int32, 0 } },
        { { Int32, 252 }, { Int32, 256 } }, { { Int32, 256 }, { Int32, 260 } }, { { Int32, -256 }, { Int32, -252 } }, { { Int32, -260 }, { Int32, -256 } },
        { { Int32, 2 }, { Int32, 6 } },
        { { Int32, 0 }, { Int64, 4 } }, { { Int32, 0 }, { Int64, 8 } }, { { Int64, 0 }, { Int32, 8 } },
        { { Double, 0 }, { Double, 8 } }, { { Float, 0 }, { Float, 4 } },
        { { Int64, 0 }, { Double, 8 } }, { { Double, 0 }, { Int64, 8 } }, { { Float, 0 }, { Int32, 4 } },
    };
    for (const MemoryTestStorePair& pair : pairs)
        runTwoStoresInMemoryTest(pair);

    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue otherBase = test.argumentGPR(1);
    LValue value = test.argumentGPR(2);
    out.store64(value, out.address(heap, base, 0));
    out.store64(value, out.address(heap, otherBase, 8));
    out.retVoid();

    MemoryTestBytes bytes(128);
    MemoryTestBytes expected(128);
    expected.write(0, memoryTestStoredBits, 8);
    expected.write(72, memoryTestStoredBits, 8);
    compileAndRun<void>(test, bytes.middle(), bytes.middle() + 64, memoryTestStoredBits);
    bytes.expectSameAs(expected, "neighboring offsets from two bases");
}

static constexpr ptrdiff_t memoryTestOffsetOfUpdatedWord = 8;
static constexpr ptrdiff_t memoryTestOffsetOfWordFound = 24;
static constexpr int64_t memoryTestInt32Words[] = { 0, 1, -1, 4, 5, 41, 97, 98, INT32_MAX, INT32_MAX - 1, INT32_MIN, INT32_MIN + 1, minInlineInt32, minInlineInt32 + 1 };
static constexpr int64_t memoryTestIntegersAddedTo[] = { 0, 5, -1, -2, 0xffffffffll, 0x100000000ll, 0x1ffffffffll, -0x100000000ll, maxIntegerAddedTo, maxIntegerAddedTo - 1, minIntegerAddedTo, minIntegerAddedTo + 1 };
static constexpr int64_t memoryTestOperandsOfUpdates[] = { 0, 1, 7, -1, INT32_MAX, INT32_MIN, 0x5eadbeef00000002ll, -0x5eadbeef00000002ll };

struct MemoryTestTestedUpdate {
    ASCIILiteral name;
    Type typeOfWord;
    MemoryUpdate::Kind kind;
    bool hasFormOnARM64;
    LValue (*makeSum)(Output&, LValue loaded, LValue operand);
    LValue (*makeTest)(Output&, LValue sum);
    std::optional<int64_t> (*wordBehind)(int64_t wordInFront, int64_t operand);
};

static LValue memoryTestInt32OperandOfUpdate(Output& out, LValue operand)
{
    return out.signExt32To64(out.castToInt32(operand));
}

static std::optional<int64_t> memoryTestSumIfWithin(int64_t sum, int64_t min, int64_t max)
{
    return sum >= min && sum <= max ? std::optional<int64_t>(sum) : std::nullopt;
}

static void checkTestedUpdateInMemoryTest(const MemoryTestTestedUpdate& update)
{
    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue operand = test.argumentGPR(1);
    LValue loaded = out.load(out.address(heap, base, memoryTestOffsetOfUpdatedWord), update.typeOfWord);
    LValue sum = update.makeSum(out, loaded, operand);
    LBasicBlock tests = out.m_block;
    LBasicBlock stores = out.newBlock();
    LBasicBlock leaves = out.newBlock();
    out.branch(update.makeTest(out, sum), normalEdge(stores), rareEdge(leaves));
    const Operation* branch = tests->last();

    out.appendTo(stores);
    LValue stored = out.store(update.typeOfWord == Int32 ? out.castToInt32(sum) : sum, out.address(heap, base, memoryTestOffsetOfUpdatedWord));
    out.ret(out.constInt64(1));

    out.appendTo(leaves);
    out.store(out.load(out.address(heap, base, memoryTestOffsetOfUpdatedWord), update.typeOfWord), out.address(heap, base, memoryTestOffsetOfWordFound));
    out.ret(out.constInt64(0));
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    bool hasForm = isX86_64() || update.hasFormOnARM64;
    expectInMemoryTest(liveness.worksInMemory(branch) == hasForm, update.name, ": the Branch");
    if (hasForm) {
        expectInMemoryTest(memoryUpdateOf(liveness, branch).kind == update.kind, update.name, ": the kind");
        expectInMemoryTest(liveness.isDropped(stored) == (update.kind != MemoryUpdate::ComparedAhead), update.name, ": the Store");
        expectInMemoryTest(liveness.isDropped(loaded) && liveness.isDropped(sum), update.name, ": the Load and the sum");
    }

    size_t size = bytesForWidth(accessWidthOf(Store, update.typeOfWord));
    std::span<const int64_t> words = update.typeOfWord == Int32 ? std::span<const int64_t>(memoryTestInt32Words) : std::span<const int64_t>(memoryTestIntegersAddedTo);
    for (int64_t wordInFront : words) {
        for (int64_t operandOfRun : memoryTestOperandsOfUpdates) {
            std::optional<int64_t> wordBehind = update.wordBehind(wordInFront, operandOfRun);
            MemoryTestBytes bytes(64);
            MemoryTestBytes expected(64);
            bytes.write(memoryTestOffsetOfUpdatedWord, static_cast<uint64_t>(wordInFront), size);
            expected.write(memoryTestOffsetOfUpdatedWord, static_cast<uint64_t>(wordBehind.value_or(wordInFront)), size);
            if (!wordBehind)
                expected.write(memoryTestOffsetOfWordFound, static_cast<uint64_t>(wordInFront), size);
            uint64_t answer = test.call<uint64_t>(bytes.middle(), operandOfRun);
            expectInMemoryTest(answer == (wordBehind ? 1u : 0u), update.name, ": the way taken with ", wordInFront, " and ", operandOfRun);
            bytes.expectSameAs(expected, update.name, " with ", wordInFront, " and ", operandOfRun);
        }
    }
}

static void testMemoryTestedSumIsStoredByBranch()
{
    static const MemoryTestTestedUpdate updates[] = {
        { "an int32 counted up"_s, Int32, MemoryUpdate::TestedBehind, true,
            [](Output& out, LValue loaded, LValue) { return out.add(out.signExt32To64(loaded), out.constInt64(1)); },
            [](Output& out, LValue sum) { return out.lessThanOrEqual(sum, out.constInt64(INT32_MAX)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word + 1, INT32_MIN, INT32_MAX); } },
        { "an int32 counted down"_s, Int32, MemoryUpdate::TestedBehind, true,
            [](Output& out, LValue loaded, LValue) { return out.add(out.signExt32To64(loaded), out.constInt64(-1)); },
            [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt64(INT32_MIN)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word - 1, INT32_MIN, INT32_MAX); } },
        { "an int32 added to an int32"_s, Int32, MemoryUpdate::TestedBehind, true,
            [](Output& out, LValue loaded, LValue operand) { return out.add(out.signExt32To64(loaded), memoryTestInt32OperandOfUpdate(out, operand)); },
            [](Output& out, LValue sum) { return out.equal(out.signExt32To64(out.castToInt32(sum)), sum); },
            [](int64_t word, int64_t operand) { return memoryTestSumIfWithin(word + static_cast<int32_t>(operand), INT32_MIN, INT32_MAX); } },
        { "an int32 taken off an int32"_s, Int32, MemoryUpdate::TestedBehind, true,
            [](Output& out, LValue loaded, LValue operand) { return out.sub(out.signExt32To64(loaded), memoryTestInt32OperandOfUpdate(out, operand)); },
            [](Output& out, LValue sum) { return out.equal(out.signExt32To64(out.castToInt32(sum)), sum); },
            [](int64_t word, int64_t operand) { return memoryTestSumIfWithin(word - static_cast<int32_t>(operand), INT32_MIN, INT32_MAX); } },
        { "an int32 beside markers"_s, Int32, MemoryUpdate::TestedBehind, true,
            [](Output& out, LValue loaded, LValue operand) { return out.add(out.signExt32To64(loaded), memoryTestInt32OperandOfUpdate(out, operand)); },
            [](Output& out, LValue sum) { return out.belowOrEqual(out.sub(sum, out.constInt64(minInlineInt32)), out.constInt64(static_cast<int64_t>(INT32_MAX) - minInlineInt32)); },
            [](int64_t word, int64_t operand) { return memoryTestSumIfWithin(word + static_cast<int32_t>(operand), minInlineInt32, INT32_MAX); } },
        { "an int32 beside markers counted down"_s, Int32, isARM64() ? MemoryUpdate::TestedBehind : MemoryUpdate::ComparedAhead, true,
            [](Output& out, LValue loaded, LValue) { return out.add(out.signExt32To64(loaded), out.constInt64(-1)); },
            [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt64(minInlineInt32)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word - 1, minInlineInt32, INT32_MAX); } },
        { "an int32 held below a bound"_s, Int32, MemoryUpdate::ComparedAhead, false,
            [](Output& out, LValue loaded, LValue) { return out.add(out.signExt32To64(loaded), out.constInt64(3)); },
            [](Output& out, LValue sum) { return out.lessThan(sum, out.constInt64(101)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word + 3, std::numeric_limits<int64_t>::min(), 100); } },
        { "a sum that wraps"_s, Int32, MemoryUpdate::TestedBehind, false,
            [](Output& out, LValue loaded, LValue operand) { return out.add(loaded, out.castToInt32(operand)); },
            [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt32(5)); },
            [](int64_t word, int64_t operand) { return memoryTestSumIfWithin(static_cast<int32_t>(static_cast<uint32_t>(word) + static_cast<uint32_t>(operand)), 5, INT32_MAX); } },
        { "an integer counted up"_s, Int64, MemoryUpdate::LowHalf, false,
            [](Output& out, LValue loaded, LValue) {
                out.noteIntegerAddedTo(loaded);
                return out.add(loaded, out.constInt64(1));
            },
            [](Output& out, LValue sum) { return out.lessThanOrEqual(sum, out.constInt64(maxIntegerAddedTo)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word + 1, minIntegerAddedTo, maxIntegerAddedTo); } },
        { "an integer counted down"_s, Int64, MemoryUpdate::LowHalf, false,
            [](Output& out, LValue loaded, LValue) {
                out.noteIntegerAddedTo(loaded);
                return out.add(loaded, out.constInt64(-1));
            },
            [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt64(minIntegerAddedTo)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word - 1, minIntegerAddedTo, maxIntegerAddedTo); } },
        { "an integer held below a bound"_s, Int64, MemoryUpdate::ComparedAhead, false,
            [](Output& out, LValue loaded, LValue) {
                out.noteIntegerAddedTo(loaded);
                return out.add(loaded, out.constInt64(3));
            },
            [](Output& out, LValue sum) { return out.lessThan(sum, out.constInt64(9)); },
            [](int64_t word, int64_t) { return memoryTestSumIfWithin(word + 3, std::numeric_limits<int64_t>::min(), 8); } },
        { "32 bits added to an integer"_s, Int64, MemoryUpdate::LowHalf, false,
            [](Output& out, LValue loaded, LValue operand) {
                out.noteIntegerAddedTo(loaded);
                return out.add(loaded, out.zeroExtPtr(out.castToInt32(operand)));
            },
            [](Output& out, LValue sum) { return out.belowOrEqual(out.sub(sum, out.constInt64(minIntegerAddedTo)), out.constInt64(maxIntegerAddedTo - minIntegerAddedTo)); },
            [](int64_t word, int64_t operand) { return memoryTestSumIfWithin(word + static_cast<uint32_t>(operand), minIntegerAddedTo, maxIntegerAddedTo); } },
        { "32 bits taken off an integer"_s, Int64, MemoryUpdate::LowHalf, false,
            [](Output& out, LValue loaded, LValue operand) {
                out.noteIntegerAddedTo(loaded);
                return out.sub(loaded, out.zeroExtPtr(out.castToInt32(operand)));
            },
            [](Output& out, LValue sum) { return out.belowOrEqual(out.sub(sum, out.constInt64(minIntegerAddedTo)), out.constInt64(maxIntegerAddedTo - minIntegerAddedTo)); },
            [](int64_t word, int64_t operand) { return memoryTestSumIfWithin(word - static_cast<uint32_t>(operand), minIntegerAddedTo, maxIntegerAddedTo); } },
    };
    for (const MemoryTestTestedUpdate& update : updates)
        checkTestedUpdateInMemoryTest(update);
}

static void checkLowHalfBesideWholeReadInMemoryTest(bool readsWholeWord)
{
    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue loaded = out.load64(out.address(heap, base, memoryTestOffsetOfUpdatedWord));
    out.noteIntegerAddedTo(loaded);
    LValue sum = out.add(loaded, out.constInt64(1));
    LBasicBlock tests = out.m_block;
    LBasicBlock stores = out.newBlock();
    LBasicBlock leaves = out.newBlock();
    out.branch(out.lessThanOrEqual(sum, out.constInt64(maxIntegerAddedTo)), normalEdge(stores), rareEdge(leaves));
    const Operation* branch = tests->last();

    out.appendTo(stores);
    out.store64(sum, out.address(heap, base, memoryTestOffsetOfUpdatedWord));
    out.ret(out.load64(out.address(heap, base, readsWholeWord ? memoryTestOffsetOfUpdatedWord : memoryTestOffsetOfWordFound)));
    out.appendTo(leaves);
    out.ret(out.constInt64(-1));
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    AOT_TEST_CHECK(liveness.worksInMemory(branch) == isX86_64());
    AOT_TEST_CHECK(liveness.of(branch).has(ValueFlag::AddsToWholeWord) == (isX86_64() && readsWholeWord));
    for (int64_t wordInFront : memoryTestIntegersAddedTo) {
        int64_t words[4] = { 0, wordInFront, 0, 77 };
        int64_t answer = test.call<int64_t>(&words[0]);
        bool isStored = wordInFront < maxIntegerAddedTo;
        AOT_TEST_CHECK(words[1] == (isStored ? wordInFront + 1 : wordInFront));
        AOT_TEST_CHECK(answer == (!isStored ? -1 : readsWholeWord ? wordInFront + 1 : 77));
    }
}

static void checkLowHalfInLoopInMemoryTest(bool readsWholeWord)
{
    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LBasicBlock head = out.newBlock();
    LBasicBlock stores = out.newBlock();
    LBasicBlock done = out.newBlock();
    LBasicBlock leaves = out.newBlock();
    ValueFromBlock firstRoundsLeft = out.anchor(test.argumentGPR(1));
    ValueFromBlock firstTotal = out.anchor(out.int64Zero);
    out.jump(normalEdge(head));

    out.appendTo(head);
    LValue roundsLeft = out.phi(Int64, firstRoundsLeft);
    LValue total = out.phi(Int64, firstTotal);
    LValue loaded = out.load64(out.address(heap, base, memoryTestOffsetOfUpdatedWord));
    out.noteIntegerAddedTo(loaded);
    LValue sum = out.add(loaded, out.constInt64(1));
    LValue nextTotal = out.add(total, readsWholeWord ? loaded : roundsLeft);
    out.branch(out.lessThanOrEqual(sum, out.constInt64(maxIntegerAddedTo)), normalEdge(stores), rareEdge(leaves));
    const Operation* branch = head->last();

    out.appendTo(stores);
    out.store64(sum, out.address(heap, base, memoryTestOffsetOfUpdatedWord));
    LValue nextRoundsLeft = out.sub(roundsLeft, out.constInt64(1));
    out.addIncomingToPhi(roundsLeft, out.anchor(nextRoundsLeft));
    out.addIncomingToPhi(total, out.anchor(nextTotal));
    out.branch(out.notZero64(nextRoundsLeft), normalEdge(head), normalEdge(done));

    out.appendTo(done);
    out.ret(nextTotal);
    out.appendTo(leaves);
    out.ret(out.constInt64(-1));
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    AOT_TEST_CHECK(liveness.worksInMemory(branch) == isX86_64());
    AOT_TEST_CHECK(liveness.of(branch).has(ValueFlag::AddsToWholeWord) == (isX86_64() && readsWholeWord));

    int64_t words[2] = { 0, 0xfffffffell };
    AOT_TEST_CHECK(test.call<int64_t>(&words[0], int64_t { 3 }) == (readsWholeWord ? 3 * 0xffffffffll : 6));
    AOT_TEST_CHECK(words[1] == 0x100000001ll);
    words[1] = maxIntegerAddedTo - 1;
    AOT_TEST_CHECK(test.call<int64_t>(&words[0], int64_t { 3 }) == -1);
    AOT_TEST_CHECK(words[1] == maxIntegerAddedTo);
}

static void testMemoryLowHalfIsLeftAloneWhereWholeWordIsRead()
{
    checkLowHalfBesideWholeReadInMemoryTest(false);
    checkLowHalfBesideWholeReadInMemoryTest(true);
    checkLowHalfInLoopInMemoryTest(false);
    checkLowHalfInLoopInMemoryTest(true);
}

static void testMemoryWordIsComputedInPlace()
{
    static constexpr Opcode opcodes[] = { Add, Sub, BitAnd, BitOr, BitXor };
    constexpr uint64_t wordInFront = 0x8123456789abcdefull;
    constexpr uint64_t operandOfRun = 0xf0f0f0f00ff00ff1ull;
    constexpr int32_t constant = -0x1235;
    auto compute = [](Opcode opcode, uint64_t word, uint64_t operand) -> uint64_t {
        if (opcode == Add)
            return word + operand;
        if (opcode == Sub)
            return word - operand;
        if (opcode == BitAnd)
            return word & operand;
        return opcode == BitOr ? word | operand : word ^ operand;
    };

    for (Opcode opcode : opcodes) {
        for (Type type : { Type(Int32), Type(Int64) }) {
            for (bool takesConstant : { false, true }) {
                TestProcedure test;
                Output& out = test.out();
                Builder& builder = out;
                const AbstractHeap& heap = test.heaps().root;
                LValue base = test.argumentGPR(0);
                LValue operand = memoryTestValueOfType(test, 1, type);
                if (takesConstant)
                    operand = out.constantOfType(type, constant);
                LValue loaded = out.load(out.address(heap, base, memoryTestOffsetOfUpdatedWord), type);
                LValue computed = builder.binary(opcode, loaded, operand);
                LValue stored = out.store(computed, out.address(heap, base, memoryTestOffsetOfUpdatedWord));
                out.retVoid();
                compileOrFail(test);

                const Liveness& liveness = test.procedure().liveness();
                expectInMemoryTest(liveness.worksInMemory(stored) == isX86_64(), opcode, " ", type, " in place");
                expectInMemoryTest(liveness.isDropped(loaded) == isX86_64() && liveness.isDropped(computed) == isX86_64(), opcode, " ", type, ": what the Store has taken over");

                MemoryTestBytes bytes(64);
                MemoryTestBytes expected(64);
                bytes.write(memoryTestOffsetOfUpdatedWord, wordInFront, 8);
                expected.write(memoryTestOffsetOfUpdatedWord, wordInFront, 8);
                expected.write(memoryTestOffsetOfUpdatedWord, compute(opcode, wordInFront, takesConstant ? static_cast<uint64_t>(static_cast<int64_t>(constant)) : operandOfRun), bytesForWidth(accessWidthOf(Store, type)));
                test.call<void>(bytes.middle(), operandOfRun);
                bytes.expectSameAs(expected, opcode, " ", type, takesConstant ? " of a constant" : " of a register");
            }
        }
    }
}

enum class MemoryTestBesideUpdate : uint8_t { OldWordIsRead, SumIsRead, StoreStandsBetween, PatchStandsBetween, ShiftStandsBetween, OtherOffset };

static void checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate beside)
{
    TestProcedure test;
    Output& out = test.out();
    const AbstractHeap& heap = test.heaps().root;
    LValue base = test.argumentGPR(0);
    LValue other = test.argumentGPR(1);
    LValue loaded = out.load64(out.address(heap, base, 8));
    if (beside == MemoryTestBesideUpdate::StoreStandsBetween)
        out.store64(out.constInt64(100), out.address(heap, other, 8));
    if (beside == MemoryTestBesideUpdate::PatchStandsBetween)
        appendStoringPatchInMemoryTest(test, other, other, 8, HeapSet::top());
    LValue addend = out.constInt64(2);
    if (beside == MemoryTestBesideUpdate::ShiftStandsBetween)
        addend = out.shl(addend, out.castToInt32(other));
    LValue sum = out.add(loaded, addend);
    LValue stored = out.store64(sum, out.address(heap, base, beside == MemoryTestBesideUpdate::OtherOffset ? 16 : 8));
    out.ret(beside == MemoryTestBesideUpdate::OldWordIsRead ? loaded : beside == MemoryTestBesideUpdate::SumIsRead ? sum : out.constInt64(0));
    compileOrFail(test);

    const Liveness& liveness = test.procedure().liveness();
    bool mayWorkInMemory = beside == MemoryTestBesideUpdate::OldWordIsRead || beside == MemoryTestBesideUpdate::ShiftStandsBetween;
    AOT_TEST_CHECK(liveness.worksInMemory(stored) == (mayWorkInMemory && isX86_64()));
    AOT_TEST_CHECK(beside != MemoryTestBesideUpdate::OldWordIsRead || !liveness.isDropped(loaded));

    uint64_t words[3] = { 0, 40, 7 };
    uint64_t addendOfRun = beside == MemoryTestBesideUpdate::ShiftStandsBetween ? uint64_t { 2 } << (reinterpret_cast<uintptr_t>(&words[0]) & 63) : 2;
    uint64_t answer = test.call<uint64_t>(&words[0], &words[0]);
    AOT_TEST_CHECK(words[beside == MemoryTestBesideUpdate::OtherOffset ? 2 : 1] == 40 + addendOfRun);
    AOT_TEST_CHECK(beside != MemoryTestBesideUpdate::OtherOffset || words[1] == 40);
    AOT_TEST_CHECK(answer == (beside == MemoryTestBesideUpdate::OldWordIsRead ? 40u : beside == MemoryTestBesideUpdate::SumIsRead ? 42u : 0u));
}

static void testMemoryUpdateLeavesOtherReadersAndWritersAlone()
{
    checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate::OldWordIsRead);
    checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate::SumIsRead);
    checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate::StoreStandsBetween);
    checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate::PatchStandsBetween);
    checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate::ShiftStandsBetween);
    checkUpdateBesideInMemoryTest(MemoryTestBesideUpdate::OtherOffset);
}

} // namespace Machine

std::span<const NativeTest> machineMemoryTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/memory/indexed-addresses"_s, Machine::testMemoryIndexedAddresses },
        { "machine/memory/words-of-the-instance"_s, Machine::testMemoryWordsOfInstance },
        { "machine/memory/constant-data"_s, Machine::testMemoryConstantData },
        { "machine/memory/constant-data-says-what-it-is"_s, Machine::testMemoryConstantDataSaysWhatItIs },
        { "machine/memory/constant-added-to-an-index-moves-into-the-offset"_s, Machine::testMemoryConstantAddedToIndexMovesIntoOffset },
        { "machine/memory/table-of-the-image-is-the-base-of-an-indexed-load"_s, Machine::testMemoryTableOfImageIsBaseOfIndexedLoad },
        { "machine/memory/address-with-two-uses-is-not-fused"_s, Machine::testMemoryAddressWithTwoUsesIsNotFused },
        { "machine/memory/extension-of-an-index-is-fused-where-the-register-holds-it"_s, Machine::testMemoryExtensionOfIndexIsFusedWhereRegisterHoldsIt },
        { "machine/memory/several-addresses-name-one-word"_s, Machine::testMemoryAliasedAddresses },
        { "machine/memory/number-is-made-of-the-low-half-of-a-register"_s, Machine::testMemoryNumberIsMadeOfLowHalfOfRegister },
        { "machine/memory/comparison-with-a-constant-reads-the-load-where-it-lies"_s, Machine::testMemoryComparisonReadsLoadWhereItLies },
        { "machine/memory/bit-test-reads-the-load-where-it-lies"_s, Machine::testMemoryBitTestReadsLoadWhereItLies },
        { "machine/memory/comparison-with-a-register-reads-the-load-where-it-lies"_s, Machine::testMemoryComparisonWithRegisterReadsLoadWhereItLies },
        { "machine/memory/tested-load-moves-behind-what-only-keeps-a-value-alive"_s, Machine::testMemoryTestedLoadMovesBehindWhatOnlyKeepsValueAlive },
        { "machine/memory/fence-ends-no-load"_s, Machine::testMemoryFenceEndsNoLoad },
        { "machine/memory/unused-trapping-load-stays"_s, Machine::testMemoryUnusedTrappingLoadStays },
        { "machine/memory/trapping-load-names-its-reference"_s, Machine::testMemoryTrappingLoadNamesItsReference },
        { "machine/memory/trap-is-handed-over-or-stays"_s, Machine::testMemoryTrapIsHandedOverOrStays },
        { "machine/memory/trap-is-handed-over-past-a-patch-without-effects"_s, Machine::testMemoryTrapIsHandedOverPastPatchWithoutEffects },
        { "machine/memory/trap-stays-ahead-of-a-patch-that-reads"_s, Machine::testMemoryTrapStaysAheadOfPatch<Machine::PatchBehindTrapInMemoryTest::Reads> },
        { "machine/memory/trap-stays-ahead-of-a-patch-that-writes"_s, Machine::testMemoryTrapStaysAheadOfPatch<Machine::PatchBehindTrapInMemoryTest::Writes> },
        { "machine/memory/trap-stays-ahead-of-a-patch-that-takes-an-integer"_s, Machine::testMemoryTrapStaysAheadOfPatch<Machine::PatchBehindTrapInMemoryTest::TakesInteger> },
        { "machine/memory/trap-stays-ahead-of-a-patch-that-takes-its-base"_s, Machine::testMemoryTrapStaysAheadOfPatch<Machine::PatchBehindTrapInMemoryTest::TakesBase> },
        { "machine/memory/trap-stays-ahead-of-a-patch-that-suspends"_s, Machine::testMemoryTrapStaysAheadOfPatch<Machine::PatchBehindTrapInMemoryTest::Suspends> },
        { "machine/memory/neighboring-stores-pair"_s, Machine::testMemoryNeighboringStoresPair },
        { "machine/memory/tested-sum-is-stored-by-the-branch"_s, Machine::testMemoryTestedSumIsStoredByBranch },
        { "machine/memory/low-half-is-left-alone-where-the-whole-word-is-read"_s, Machine::testMemoryLowHalfIsLeftAloneWhereWholeWordIsRead },
        { "machine/memory/word-is-computed-in-place"_s, Machine::testMemoryWordIsComputedInPlace },
        { "machine/memory/update-leaves-other-readers-and-writers-alone"_s, Machine::testMemoryUpdateLeavesOtherReadersAndWritersAlone },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
