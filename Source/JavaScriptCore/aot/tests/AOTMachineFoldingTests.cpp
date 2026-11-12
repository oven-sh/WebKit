/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineFolding.h"
#include "AOTMachineTarget.h"
#include "AOTMachineTestSupport.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <type_traits>
#include <wtf/ASCIICType.h>
#include <wtf/DataLog.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Machine {

namespace {

enum class FoldingTestOperands : uint8_t { Integers, Numbers };

enum class FoldingTestForm : uint8_t { Folded, Unfolded };

enum class FoldingTestAnswers : uint8_t { Compared, NotCompared };

struct FoldingTestOperand {
    int64_t integer;
    double number;
};

} // anonymous namespace

static constexpr double foldingTestNaN = std::numeric_limits<double>::quiet_NaN();
static constexpr double foldingTestInfinity = std::numeric_limits<double>::infinity();
static constexpr int64_t foldingTestTag = static_cast<int64_t>(0xfffe000000000000ull);
static constexpr ASCIILiteral foldingTestTextOfPureNaN = "ConstDouble(NaN(0x7ff8000000000000))"_s;
static constexpr ASCIILiteral foldingTestTextOfNegativeNaN = "ConstDouble(NaN(0xfff8000000000000))"_s;
static constexpr ASCIILiteral foldingTestTextOfInvalidOperation = isX86_64() ? foldingTestTextOfNegativeNaN : foldingTestTextOfPureNaN;
static constexpr ASCIILiteral foldingTestTextOfPureNaNOfFloats = "ConstFloat(NaN(0x7fc00000))"_s;
static constexpr ASCIILiteral foldingTestTextOfNegativeNaNOfFloats = "ConstFloat(NaN(0xffc00000))"_s;
static constexpr ASCIILiteral foldingTestTextOfInvalidOperationOnFloats = isX86_64() ? foldingTestTextOfNegativeNaNOfFloats : foldingTestTextOfPureNaNOfFloats;
static constexpr int64_t foldingTestBitsOfNaNWithPayload = 0x7ff8000000abcdefll;
static constexpr int64_t foldingTestBitsOfNegativeNaNWithPayload = static_cast<int64_t>(0xfff8000000abcdefull);
static constexpr int64_t foldingTestBitsOfNegativeSignalingNaN = static_cast<int64_t>(0xfff0000000000001ull);
static constexpr ASCIILiteral foldingTestAnyText { };

static constexpr ASCIILiteral foldingTestTextOnARM64(ASCIILiteral text)
{
    return isARM64() ? text : ASCIILiteral();
}

static constexpr FoldingTestOperand foldingTestEdgeValues[] = {
    { 0, 0.0 },
    { 1, -0.0 },
    { -1, 1.0 },
    { 2, -1.0 },
    { 7, 0.5 },
    { -8, -0.5 },
    { 31, 1.5 },
    { 32, -2.5 },
    { 33, 2.0 },
    { 63, 1.0 / 3.0 },
    { 64, 16777217.0 },
    { 0x7f, 2147483647.0 },
    { 0x80, -2147483648.0 },
    { 0xff, 4294967296.0 },
    { 0x7fff, 9007199254740992.0 },
    { 0x8000, -9007199254740992.0 },
    { 0xffff, 1e300 },
    { INT32_MAX, -1e300 },
    { INT32_MIN, std::numeric_limits<double>::denorm_min() },
    { 0x80000000ll, std::numeric_limits<double>::min() },
    { 0xffffffffll, std::numeric_limits<double>::max() },
    { 0x100000000ll, foldingTestInfinity },
    { INT64_MAX, -foldingTestInfinity },
    { INT64_MIN, foldingTestNaN },
};

static uint64_t foldingTestBitsOf(double number)
{
    return std::bit_cast<uint64_t>(std::isnan(number) ? std::copysign(foldingTestNaN, number) : number);
}

static uint64_t foldingTestBitsOf(const Operation* constant)
{
    if (constant->hasInt32())
        return static_cast<uint32_t>(constant->asInt32());
    if (constant->hasInt64())
        return static_cast<uint64_t>(constant->asInt64());
    return foldingTestBitsOf(constant->hasDouble() ? constant->asDouble() : static_cast<double>(constant->asFloat()));
}

static uint32_t parseFoldingTestIndex(StringView text, unsigned& position)
{
    uint32_t index = 0;
    while (position < text.length() && isASCIIDigit(text[position]))
        index = index * 10 + (text[position++] - '0');
    return index;
}

namespace {

class FoldingTest {
    WTF_MAKE_NONCOPYABLE(FoldingTest);
public:
    FoldingTest(FoldingTestOperands operands, FoldingTestForm form)
        : m_test(traits())
        , m_operands(operands)
        , m_form(form)
    {
        if (operands == FoldingTestOperands::Integers) {
            a = named("a"_s, m_test.argumentGPR(0));
            b = named("b"_s, m_test.argumentGPR(1));
            i = named("i"_s, derived(out().castToInt32(a)));
            j = named("j"_s, derived(out().castToInt32(b)));
            cage = named("cage"_s, out().argumentRegister(cellCageBaseGPR));
            return;
        }
        x = named("x"_s, m_test.argumentFPR(0));
        y = named("y"_s, m_test.argumentFPR(1));
        f = named("f"_s, derived(out().doubleToFloat(x)));
        g = named("g"_s, derived(out().doubleToFloat(y)));
    }

    Output& out() { return m_test.out(); }
    Procedure& procedure() { return m_test.procedure(); }

    LValue a { nullptr };
    LValue b { nullptr };
    LValue i { nullptr };
    LValue j { nullptr };
    LValue cage { nullptr };
    LValue x { nullptr };
    LValue y { nullptr };
    LValue f { nullptr };
    LValue g { nullptr };

    LValue firstOperand(Type type) const { return operandOf(type, a, i, x, f); }
    LValue secondOperand(Type type) const { return operandOf(type, b, j, y, g); }

    LValue int32(int32_t value) { return out().constInt32(value); }
    LValue int64(int64_t value) { return out().constInt64(value); }
    LValue number(double value) { return out().constDouble(value); }
    LValue single(float value) { return out().constFloat(value); }

    template<typename T> requires std::is_arithmetic_v<T>
    LValue constant(Type type, T value)
    {
        switch (type.kind()) {
        case Int32:
            return int32(static_cast<int32_t>(value));
        case Int64:
            return int64(static_cast<int64_t>(value));
        case Float:
            return single(static_cast<float>(value));
        case Double:
            return number(static_cast<double>(value));
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    LValue constant(Type type, const FoldingTestOperand& value) { return type.isInt() ? constant(type, value.integer) : constant(type, value.number); }

    LValue make(Opcode opcode, LValue value) { return out().unary(opcode, use(value)); }
    LValue make(Opcode opcode, LValue left, LValue right) { return out().binary(opcode, use(left), use(right)); }
    template<typename T> requires std::is_arithmetic_v<T>
    LValue make(Opcode opcode, LValue left, T right)
    {
        bool isShift = opcode == Shl || opcode == SShr || opcode == ZShr || opcode == RotR;
        return make(opcode, left, constant(isShift ? Type(Int32) : left->type(), right));
    }
    template<typename T> requires std::is_arithmetic_v<T>
    LValue make(Opcode opcode, T left, LValue right) { return make(opcode, constant(right->type(), left), right); }

    LValue chill(Opcode opcode, LValue left, LValue right) { return out().chill(opcode, use(left), use(right)); }
    LValue narrowReference(LValue word)
    {
        word->markAsNarrowReference();
        return word;
    }
    template<typename T> requires std::is_arithmetic_v<T>
    LValue chill(Opcode opcode, LValue left, T right) { return chill(opcode, left, constant(left->type(), right)); }

    LValue lowWord(LValue value) { return out().lowWord(use(value)); }
    LValue cast(LValue value, Type type) { return out().bitwiseCast(use(value), type); }
    LValue select(LValue predicate, LValue whenNotZero, LValue whenZero) { return out().select(use(predicate), use(whenNotZero), use(whenZero)); }

    void finish(LValue result)
    {
        m_result = result;
        out().ret(result);
    }
    LValue result() const { return m_result; }

    String treeOf(const Operation* root)
    {
        String printed = m_test.printedProcedure();
        WTF::Vector<StringView> definitions(procedure().numberOfOperations());
        for (StringView line : StringView(printed).split('\n')) {
            size_t start = line.find('@');
            if (start == notFound)
                continue;
            unsigned position = static_cast<unsigned>(start) + 1;
            uint32_t index = parseFoldingTestIndex(line, position);
            AOT_TEST_CHECK(index < definitions.size() && line.substring(position, 3) == " = "_s);
            definitions[index] = line.substring(position + 3);
        }

        StringBuilder tree;
        appendTree(tree, definitions.span(), root->index());
        return tree.toString();
    }

    void expectTree(const Operation* root, StringView expected)
    {
        String tree = treeOf(root);
        if (StringView(tree) == expected)
            return;
        dataLogLn("In row ", row, " of its table: expected ", expected, " and got ", tree, " in\n", m_test.printedProcedure());
        AOT_TEST_CHECK(StringView(tree) == expected);
    }

    void compile() { compileOrFail(m_test); }

    size_t row { 0 };

    uint64_t run(const FoldingTestOperand& first, const FoldingTestOperand& second)
    {
        if (m_operands == FoldingTestOperands::Integers)
            return answer(first.integer, second.integer);
        return answer(first.number, second.number);
    }

    void expectAnswer(uint64_t expected, const FoldingTestOperand& first, const FoldingTestOperand& second)
    {
        uint64_t actual = run(first, second);
        if (actual == expected)
            return;
        if (m_operands == FoldingTestOperands::Integers)
            dataLog("In row ", row, " of its table: for the operands ", first.integer, " and ", second.integer);
        else
            dataLog("In row ", row, " of its table: for the operands ", first.number, " and ", second.number);
        dataLogLn(", the unfolded form returns the bits ", expected, " but the folded form returns ", actual, ":\n", m_test.printedProcedure());
        AOT_TEST_CHECK(actual == expected);
    }

private:
    struct Name {
        const Operation* operand { nullptr };
        ASCIILiteral text;
    };

    static ProcedureTraits traits()
    {
        ProcedureTraits result;
        result.features = CPUFeatureSet::host();
        return result;
    }

    static LValue operandOf(Type type, LValue int64Operand, LValue int32Operand, LValue doubleOperand, LValue floatOperand)
    {
        switch (type.kind()) {
        case Int32:
            return int32Operand;
        case Int64:
            return int64Operand;
        case Float:
            return floatOperand;
        case Double:
            return doubleOperand;
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LValue named(ASCIILiteral text, LValue operand)
    {
        m_names[m_numberOfNames++] = { operand, text };
        return operand;
    }

    LValue derived(LValue value) { return m_form == FoldingTestForm::Folded ? out().opaque(value) : value; }
    LValue use(LValue value) { return m_form == FoldingTestForm::Unfolded ? out().opaque(value) : value; }

    void appendTree(StringBuilder& tree, std::span<const StringView> definitions, uint32_t index) const
    {
        for (const Name& name : std::span(m_names).first(m_numberOfNames)) {
            if (name.operand->index() == index) {
                tree.append(name.text);
                return;
            }
        }

        StringView definition = definitions[index];
        AOT_TEST_CHECK(!definition.isNull());
        for (unsigned position = 0; position < definition.length();) {
            if (definition[position] != '@') {
                tree.append(definition[position++]);
                continue;
            }
            ++position;
            uint32_t child = parseFoldingTestIndex(definition, position);
            appendTree(tree, definitions, child);
        }
    }

    template<typename Argument>
    uint64_t answer(Argument first, Argument second)
    {
        switch (m_result->type().kind()) {
        case Int32:
            return static_cast<uint32_t>(m_test.call<int32_t>(first, second));
        case Int64:
            return static_cast<uint64_t>(m_test.call<int64_t>(first, second));
        case Float:
            return foldingTestBitsOf(static_cast<double>(m_test.call<float>(first, second)));
        case Double:
            return foldingTestBitsOf(m_test.call<double>(first, second));
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    TestProcedure m_test;
    FoldingTestOperands m_operands;
    FoldingTestForm m_form;
    LValue m_result { nullptr };
    std::array<Name, 5> m_names;
    unsigned m_numberOfNames { 0 };
};

struct FoldingTestCase {
    ASCIILiteral expected;
    LValue (*request)(FoldingTest&);
    FoldingTestAnswers answers { FoldingTestAnswers::Compared };
};

} // anonymous namespace

static void checkFoldingTestCases(FoldingTestOperands operands, std::span<const FoldingTestCase> cases)
{
    for (const FoldingTestCase& testCase : cases) {
        FoldingTest folded(operands, FoldingTestForm::Folded);
        folded.row = static_cast<size_t>(&testCase - cases.data());
        folded.finish(testCase.request(folded));
        if (!testCase.expected.isNull())
            folded.expectTree(folded.result(), testCase.expected);
        if (testCase.answers == FoldingTestAnswers::NotCompared)
            continue;

        FoldingTest unfolded(operands, FoldingTestForm::Unfolded);
        unfolded.finish(testCase.request(unfolded));
        AOT_TEST_CHECK(folded.result()->type() == unfolded.result()->type());
        folded.compile();
        unfolded.compile();
        for (const FoldingTestOperand& first : foldingTestEdgeValues) {
            for (const FoldingTestOperand& second : foldingTestEdgeValues)
                folded.expectAnswer(unfolded.run(first, second), first, second);
        }
    }
}

static void testMachineFoldingOfDivisions()
{
    static constexpr FoldingTestCase cases[] = {
        { "Const64(3)"_s, [](FoldingTest& v) { return v.make(Div, v.int64(7), 2); } },
        { "Const64(-3)"_s, [](FoldingTest& v) { return v.make(Div, v.int64(-7), 2); } },
        { "Const64(-1)"_s, [](FoldingTest& v) { return v.make(Mod, v.int64(-7), 2); } },
        { "Const32(2147483647)"_s, [](FoldingTest& v) { return v.make(UDiv, v.int32(-1), 2); } },
        { "Const32(5)"_s, [](FoldingTest& v) { return v.make(UMod, v.int32(-1), 10); } },
        { "Const64(5)"_s, [](FoldingTest& v) { return v.make(UMod, v.int64(-1), 10); } },

        { "a"_s, [](FoldingTest& v) { return v.make(Div, v.a, 1); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(Mod, v.a, 1); } },
        { "a"_s, [](FoldingTest& v) { return v.make(UDiv, v.a, 1); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(UMod, v.a, 1); } },
        { "Neg(a)"_s, [](FoldingTest& v) { return v.make(Div, v.a, -1); }, FoldingTestAnswers::NotCompared },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(Mod, v.a, -1); }, FoldingTestAnswers::NotCompared },

        { "ZShr(a, Const32(3))"_s, [](FoldingTest& v) { return v.make(UDiv, v.a, 8); } },
        { "ZShr(i, Const32(31))"_s, [](FoldingTest& v) { return v.make(UDiv, v.i, INT32_MIN); } },
        { "BitAnd(a, Const64(7))"_s, [](FoldingTest& v) { return v.make(UMod, v.a, 8); } },
        { "BitAnd(i, Const32(7))"_s, [](FoldingTest& v) { return v.make(UMod, v.i, 8); } },
        { "ZShr(a, Const32(4))"_s, [](FoldingTest& v) { return v.make(Div, v.make(ZShr, v.a, 1), 8); } },
        { foldingTestTextOnARM64("UDiv(a, Const64(6))"_s), [](FoldingTest& v) { return v.make(UDiv, v.a, 6); } },
        { foldingTestTextOnARM64("Div(a, Const64(8))"_s), [](FoldingTest& v) { return v.make(Div, v.a, 8); } },
        { foldingTestTextOnARM64("Div(a, Const64(7))"_s), [](FoldingTest& v) { return v.make(Div, v.a, 7); } },
        { foldingTestTextOnARM64("Sub(a, Mul(Div(a, Const64(7)), Const64(7)))"_s), [](FoldingTest& v) { return v.make(Mod, v.a, 7); } },

        { "Const64(0)"_s, [](FoldingTest& v) { return v.chill(Div, v.int64(7), 0); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.chill(Mod, v.int64(7), 0); } },
        { "Const64(-9223372036854775808)"_s, [](FoldingTest& v) { return v.chill(Div, v.int64(INT64_MIN), -1); } },
        { "Const32(-2147483648)"_s, [](FoldingTest& v) { return v.chill(Div, v.int32(INT32_MIN), -1); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.chill(Mod, v.int64(INT64_MIN), -1); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.chill(Div, v.a, 0); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.chill(Mod, v.a, 0); } },
        { "Neg(a)"_s, [](FoldingTest& v) { return v.chill(Div, v.a, -1); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.chill(Mod, v.a, -1); } },
        { foldingTestTextOnARM64("Div(a, Const64(7))"_s), [](FoldingTest& v) { return v.chill(Div, v.a, 7); } },
        { foldingTestTextOnARM64("Div(a, b, IsChill)"_s), [](FoldingTest& v) { return v.chill(Div, v.a, v.b); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.chill(Mod, v.a, v.b); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.chill(Mod, v.i, v.j); } },

        { "Const64(1)"_s, [](FoldingTest& v) { return v.make(MulHigh, v.int64(INT64_MAX), 4); } },
        { "Const64(-1)"_s, [](FoldingTest& v) { return v.make(MulHigh, v.int64(-1), 1); } },
        { "Const64(1)"_s, [](FoldingTest& v) { return v.make(UMulHigh, v.int64(-1), 2); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(MulHigh, v.int32(0x40000000), 4); }, FoldingTestAnswers::NotCompared },
        { "Const32(-2)"_s, [](FoldingTest& v) { return v.make(UMulHigh, v.int32(-1), -1); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOnARM64("MulHigh(a, Const64(2))"_s), [](FoldingTest& v) { return v.make(MulHigh, v.a, 2); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Integers, cases);

    FoldingTest v(FoldingTestOperands::Integers, FoldingTestForm::Folded);
    AOT_TEST_CHECK(!v.make(Div, v.int64(7), 0)->isConstant());
    AOT_TEST_CHECK(!v.make(UDiv, v.int64(7), 0)->isConstant());
    AOT_TEST_CHECK(!v.make(Div, v.int64(INT64_MIN), -1)->isConstant());
    AOT_TEST_CHECK(!v.make(Div, v.int32(INT32_MIN), -1)->isConstant());
    AOT_TEST_CHECK(!v.make(Mod, v.int64(INT64_MIN), -1)->isConstant());
    AOT_TEST_CHECK(v.make(Div, v.int32(INT32_MIN + 1), -1)->isInt32(INT32_MAX));
    AOT_TEST_CHECK(v.make(Div, v.int64(INT32_MIN), -1)->isInt64(int64_t { 1 } << 31));

    Opcode opcodeOfDivision = isX86_64() ? Patch : Div;
    AOT_TEST_CHECK(v.make(Div, v.a, 7)->opcode() == opcodeOfDivision);
    AOT_TEST_CHECK(v.make(Div, v.a, 8)->opcode() == opcodeOfDivision);
    AOT_TEST_CHECK(v.make(Div, v.a, v.a)->opcode() == opcodeOfDivision);
}

static void testMachineFoldingOfBitOperations()
{
    static constexpr FoldingTestCase cases[] = {
        { "NotEqual(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(Equal, v.a, v.b), 1); } },
        { "Equal(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(NotEqual, v.a, v.b), 1); } },
        { "GreaterEqual(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(LessThan, v.a, v.b), 1); } },
        { "GreaterThan(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(LessEqual, v.a, v.b), 1); } },
        { "AboveEqual(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(Below, v.a, v.b), 1); } },
        { "Above(i, j)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BelowEqual, v.i, v.j), 1); } },
        { "LessThan(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.make(LessThan, v.a, v.b), 1), 1); } },
        { "i"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.i, 1), 1); } },
        { "BitXor(i, Const32(1))"_s, [](FoldingTest& v) { return v.make(BitXor, v.i, 1); } },
        { "BitXor(LessThan(a, b), Const32(3))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(LessThan, v.a, v.b), 3); } },

        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.a, 0); } },
        { "a"_s, [](FoldingTest& v) { return v.make(BitAnd, v.a, -1); } },
        { "i"_s, [](FoldingTest& v) { return v.make(BitAnd, v.i, -1); } },
        { "a"_s, [](FoldingTest& v) { return v.make(BitAnd, v.a, v.a); } },
        { "a"_s, [](FoldingTest& v) { return v.make(BitOr, v.a, 0); } },
        { "Const64(-1)"_s, [](FoldingTest& v) { return v.make(BitOr, v.a, -1); } },
        { "a"_s, [](FoldingTest& v) { return v.make(BitOr, v.a, v.a); } },
        { "a"_s, [](FoldingTest& v) { return v.make(BitXor, v.a, 0); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(BitXor, v.a, v.a); } },
        { "BitAnd(a, b)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.a, v.b); } },
        { "BitOr(a, b)"_s, [](FoldingTest& v) { return v.make(BitOr, v.a, v.b); } },
        { "BitXor(a, b)"_s, [](FoldingTest& v) { return v.make(BitXor, v.a, v.b); } },
        { "BitAnd(a, Const64(12))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.a, 12); } },
        { "BitAnd(a, Const64(4294967295))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.a, 0xffffffffll); } },
        { "Const64(8)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.int64(12), 10); } },
        { "Const64(14)"_s, [](FoldingTest& v) { return v.make(BitOr, v.int64(12), 10); } },
        { "Const64(6)"_s, [](FoldingTest& v) { return v.make(BitXor, v.int64(12), 10); } },

        { "BitAnd(a, Const64(240))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(BitAnd, v.a, 0xff0), 0x0ff); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(BitAnd, v.a, 0xf00), 0x0ff); } },
        { "BitOr(a, Const64(5))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(BitOr, v.a, 1), 4); } },
        { "BitXor(a, Const64(6))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.a, 5), 3); } },
        { "BitAnd(a, Const64(15))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(BitOr, v.a, 0xf0), 0x0f); } },
        { "BitAnd(BitOr(a, Const64(240)), Const64(255))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(BitOr, v.a, 0xf0), 0xff); } },
        { "BitOr(BitAnd(a, Const64(240)), Const64(15))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(BitAnd, v.a, 0xf0), 0x0f); } },

        { "ZShr(i, Const32(24))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(ZShr, v.i, 24), 0xff); } },
        { "BitAnd(ZShr(i, Const32(23)), Const32(255))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(ZShr, v.i, 23), 0xff); } },
        { "LessThan(a, b)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(LessThan, v.a, v.b), 1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(LessThan, v.a, v.b), 2); } },
        { "ZExt32(i)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(ZExt32, v.i), 0xffffffffll); } },
        { "ZExt32(i)"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(SExt32, v.i), 0xffffffffll); } },
        { "BitAnd(SExt32(i), Const64(2147483647))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(SExt32, v.i), 0x7fffffff); } },
        { "BitAnd(SExt32(i), Const64(8589934591))"_s, [](FoldingTest& v) { return v.make(BitAnd, v.make(SExt32, v.i), 0x1ffffffffll); } },

        { "Const32(32)"_s, [](FoldingTest& v) { return v.make(Clz, v.int32(0)); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Clz, v.int32(-1)); } },
        { "Const64(63)"_s, [](FoldingTest& v) { return v.make(Clz, v.int64(1)); }, FoldingTestAnswers::NotCompared },
        { "Const32(32)"_s, [](FoldingTest& v) { return v.make(PopCount, v.int32(-1)); } },
        { "Const64(1)"_s, [](FoldingTest& v) { return v.make(PopCount, v.int64(INT64_MIN)); }, FoldingTestAnswers::NotCompared },
        { "Clz(i)"_s, [](FoldingTest& v) { return v.make(Clz, v.i); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Integers, cases);
}

static LValue foldingTestRotation(FoldingTest& v, LValue value, int amount)
{
    return v.make(BitOr, v.make(ZShr, value, amount), v.make(Shl, value, (value->type() == Int32 ? 32 : 64) - amount));
}

static void testMachineFoldingOfShifts()
{
    static constexpr FoldingTestCase cases[] = {
        { "Shl(i, Const32(1))"_s, [](FoldingTest& v) { return v.make(Shl, v.i, 33); } },
        { "Shl(a, Const32(33))"_s, [](FoldingTest& v) { return v.make(Shl, v.a, 33); } },
        { "Shl(a, Const32(1))"_s, [](FoldingTest& v) { return v.make(Shl, v.a, 65); } },
        { "SShr(i, Const32(31))"_s, [](FoldingTest& v) { return v.make(SShr, v.i, -1); } },
        { "i"_s, [](FoldingTest& v) { return v.make(Shl, v.i, 32); } },
        { "i"_s, [](FoldingTest& v) { return v.make(SShr, v.i, 32); } },
        { "i"_s, [](FoldingTest& v) { return v.make(ZShr, v.i, 32); } },
        { "a"_s, [](FoldingTest& v) { return v.make(Shl, v.a, 64); } },
        { "a"_s, [](FoldingTest& v) { return v.make(ZShr, v.a, 64); } },
        { "ZShr(a, Const32(32))"_s, [](FoldingTest& v) { return v.make(ZShr, v.a, 32); } },
        { "a"_s, [](FoldingTest& v) { return v.make(Shl, v.a, 0); } },
        { "Const64(0)"_s, [](FoldingTest& v) { return v.make(Shl, v.int64(0), v.j); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(ZShr, v.int32(0), v.j); } },
        { "Const64(-1)"_s, [](FoldingTest& v) { return v.make(SShr, v.int64(-1), v.j); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(ZShr, v.int64(-1), v.j); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Shl, v.int32(1), v.j); } },

        { "Const32(-2147483648)"_s, [](FoldingTest& v) { return v.make(Shl, v.int32(1), 31); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Shl, v.int32(1), 32); } },
        { "Const32(-1)"_s, [](FoldingTest& v) { return v.make(SShr, v.int32(INT32_MIN), 31); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(ZShr, v.int32(INT32_MIN), 31); } },
        { "Const64(1)"_s, [](FoldingTest& v) { return v.make(ZShr, v.int64(-1), 63); } },
        { "Const64(-9223372036854775808)"_s, [](FoldingTest& v) { return v.make(Shl, v.int64(1), 63); } },

        { "Shl(a, Const32(7))"_s, [](FoldingTest& v) { return v.make(Shl, v.make(Shl, v.a, 3), 4); } },
        { "ZShr(a, Const32(7))"_s, [](FoldingTest& v) { return v.make(ZShr, v.make(ZShr, v.a, 3), 4); } },
        { "SShr(a, Const32(7))"_s, [](FoldingTest& v) { return v.make(SShr, v.make(SShr, v.a, 3), 4); } },
        { "Shl(a, Const32(32))"_s, [](FoldingTest& v) { return v.make(Shl, v.make(Shl, v.a, 20), 12); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Shl, v.make(Shl, v.i, 20), 12); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(ZShr, v.make(ZShr, v.i, 20), 12); } },
        { "SShr(i, Const32(31))"_s, [](FoldingTest& v) { return v.make(SShr, v.make(SShr, v.i, 20), 12); } },
        { "Shl(ZShr(a, Const32(3)), Const32(4))"_s, [](FoldingTest& v) { return v.make(Shl, v.make(ZShr, v.a, 3), 4); } },
        { "ZShr(SShr(a, Const32(3)), Const32(4))"_s, [](FoldingTest& v) { return v.make(ZShr, v.make(SShr, v.a, 3), 4); } },

        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SShr, v.make(Shl, v.i, 24), 24); } },
        { "SExt16(i)"_s, [](FoldingTest& v) { return v.make(SShr, v.make(Shl, v.i, 16), 16); } },
        { "SShr(Shl(i, Const32(8)), Const32(8))"_s, [](FoldingTest& v) { return v.make(SShr, v.make(Shl, v.i, 8), 8); } },
        { "SShr(Shl(a, Const32(24)), Const32(24))"_s, [](FoldingTest& v) { return v.make(SShr, v.make(Shl, v.a, 24), 24); } },
        { "SShr(Shl(i, Const32(24)), Const32(16))"_s, [](FoldingTest& v) { return v.make(SShr, v.make(Shl, v.i, 24), 16); } },
        { "ZExt32(Trunc(a))"_s, [](FoldingTest& v) { return v.make(ZShr, v.make(Shl, v.a, 32), 32); } },
        { "SExt32(Trunc(a))"_s, [](FoldingTest& v) { return v.make(SShr, v.make(Shl, v.a, 32), 32); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(ZShr, v.make(Shl, v.i, 24), 24); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(ZShr, v.make(Shl, v.a, 31), 31); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Shl, v.make(ZShr, v.a, 3), 3); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Shl, v.make(SShr, v.i, 3), 3); } },

        { "RotR(i, Const32(7))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(ZShr, v.i, 7), v.make(Shl, v.i, 25)); } },
        { "RotR(i, Const32(27))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.i, 5), v.make(ZShr, v.i, 27)); } },
        { "RotR(i, Const32(31))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(Shl, v.i, 1), v.make(ZShr, v.i, 31)); } },
        { "RotR(i, Const32(16))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(ZShr, v.i, 16), v.make(Shl, v.i, 16)); } },
        { "RotR(a, Const32(1))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.a, 63), v.make(ZShr, v.a, 1)); } },
        { "RotR(a, Const32(32))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(ZShr, v.a, 32), v.make(Shl, v.a, 32)); } },
        { "RotR(a, Const32(59))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.a, 5), v.make(ZShr, v.a, 59)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.i, 5), v.make(ZShr, v.i, 26)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.i, 5), v.make(SShr, v.i, 27)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.i, 5), v.make(ZShr, v.j, 27)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitOr, v.make(Shl, v.a, 5), v.make(ZShr, v.a, 27)); } },
        { "BitXor(ZShr(i, Const32(11)), RotR(i, Const32(6)))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.make(ZShr, v.i, 6), v.make(ZShr, v.i, 11)), v.make(Shl, v.i, 26)); } },
        { isX86_64() ? "RotR(BitXor(i, RotR(i, Const32(5))), Const32(6))"_s : "BitXor(RotR(i, Const32(6)), RotR(i, Const32(11)))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.make(BitXor, v.make(ZShr, v.i, 6), v.make(ZShr, v.i, 11)), v.make(Shl, v.i, 26)), v.make(Shl, v.i, 21)); } },
        { isX86_64() ? "RotR(BitXor(i, RotR(BitXor(i, RotR(i, Const32(14))), Const32(5))), Const32(6))"_s : foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, foldingTestRotation(v, v.i, 25), foldingTestRotation(v, v.i, 6)), foldingTestRotation(v, v.i, 11)); } },
        { isX86_64() ? "RotR(BitXor(a, RotR(a, Const32(7))), Const32(1))"_s : foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, foldingTestRotation(v, v.a, 8), foldingTestRotation(v, v.a, 1)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, foldingTestRotation(v, v.i, 2), v.j), foldingTestRotation(v, v.i, 13)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.i, foldingTestRotation(v, v.i, 9)), foldingTestRotation(v, v.i, 17)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, foldingTestRotation(v, v.make(BitXor, v.i, foldingTestRotation(v, v.i, 20)), 20), foldingTestRotation(v, v.i, 3)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, foldingTestRotation(v, v.make(BitXor, v.i, foldingTestRotation(v, v.i, 5)), 6), foldingTestRotation(v, v.i, 11)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, foldingTestRotation(v, v.make(BitXor, v.i, foldingTestRotation(v, v.i, 5)), 6), foldingTestRotation(v, v.make(BitXor, v.i, foldingTestRotation(v, v.i, 9)), 13)); } },
        { foldingTestAnyText, [](FoldingTest& v) { LValue x = v.make(BitXor, v.j, foldingTestRotation(v, v.j, 3)); return v.make(BitXor, x, foldingTestRotation(v, x, 7)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, foldingTestRotation(v, v.i, 2), foldingTestRotation(v, v.j, 13)); } },
        { "BitOr(j, RotR(i, Const32(6)))"_s, [](FoldingTest& v) { return v.make(BitOr, v.make(BitOr, v.make(ZShr, v.i, 6), v.j), v.make(Shl, v.i, 26)); } },
        { "BitXor(b, RotR(a, Const32(14)))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.make(ZShr, v.a, 14), v.b), v.make(Shl, v.a, 50)); } },
        { "BitXor(ZShr(i, Const32(11)), RotR(i, Const32(6)))"_s, [](FoldingTest& v) { Operation* older = v.make(Shl, v.i, 26); return v.make(BitXor, older, v.make(BitXor, v.make(ZShr, v.i, 6), v.make(ZShr, v.i, 11))); } },
        { "BitOr(j, RotR(i, Const32(26)))"_s, [](FoldingTest& v) { Operation* older = v.make(ZShr, v.i, 26); return v.make(BitOr, older, v.make(BitOr, v.make(Shl, v.i, 6), v.j)); } },
        { "BitXor(b, RotR(a, Const32(14)))"_s, [](FoldingTest& v) { Operation* older = v.make(Shl, v.a, 50); return v.make(BitXor, older, v.make(BitXor, v.make(ZShr, v.a, 14), v.b)); } },
        { foldingTestAnyText, [](FoldingTest& v) { Operation* older = v.make(Shl, v.i, 26); return v.make(BitXor, older, v.make(BitOr, v.make(ZShr, v.i, 6), v.j)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, v.make(BitOr, v.make(ZShr, v.i, 6), v.j), v.make(Shl, v.i, 26)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.make(SShr, v.i, 6), v.j), v.make(Shl, v.i, 26)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(BitXor, v.make(BitXor, v.make(ZShr, v.i, 6), v.j), v.make(Shl, v.i, 25)); } },
        { "i"_s, [](FoldingTest& v) { return v.make(RotR, v.i, 32); }, FoldingTestAnswers::NotCompared },
        { "RotR(i, Const32(1))"_s, [](FoldingTest& v) { return v.make(RotR, v.i, 33); }, FoldingTestAnswers::NotCompared },
        { "Const32(-2147483648)"_s, [](FoldingTest& v) { return v.make(RotR, v.int32(1), 1); }, FoldingTestAnswers::NotCompared },
        { "Const64(-9223372036854775808)"_s, [](FoldingTest& v) { return v.make(RotR, v.int64(1), 1); }, FoldingTestAnswers::NotCompared },

        { foldingTestTextOnARM64("Shl(a, j)"_s), [](FoldingTest& v) { return v.make(Shl, v.a, v.make(BitAnd, v.j, 63)); } },
        { foldingTestTextOnARM64("Shl(i, j)"_s), [](FoldingTest& v) { return v.make(Shl, v.i, v.make(BitAnd, v.j, 31)); } },
        { foldingTestTextOnARM64("Shl(a, BitAnd(j, Const32(31)))"_s), [](FoldingTest& v) { return v.make(Shl, v.a, v.make(BitAnd, v.j, 31)); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Integers, cases);
}

static void testMachineFoldingOfIntegerConversions()
{
    static constexpr FoldingTestCase cases[] = {
        { "Const32(-128)"_s, [](FoldingTest& v) { return v.make(SExt8, v.int32(0x80)); } },
        { "Const32(-32768)"_s, [](FoldingTest& v) { return v.make(SExt16, v.int32(0x8000)); } },
        { "Const64(-2147483648)"_s, [](FoldingTest& v) { return v.make(SExt32, v.int32(INT32_MIN)); } },
        { "Const64(4294967295)"_s, [](FoldingTest& v) { return v.make(ZExt32, v.int32(-1)); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Trunc, v.int64(0x100000001ll)); } },
        { "Const32(-2147483648)"_s, [](FoldingTest& v) { return v.make(Trunc, v.int64(0x80000000ll)); } },

        { "i"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(ZExt32, v.i)); } },
        { "i"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(SExt32, v.i)); } },
        { "Trunc(a)"_s, [](FoldingTest& v) { return v.make(Trunc, v.a); } },
        { "Trunc(a)"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(Add, v.a, 0x100000000ll)); } },
        { "Trunc(a)"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(BitAnd, v.a, 0xffffffffll)); } },
        { "Trunc(Add(a, Const64(1)))"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(Add, v.a, 1)); } },
        { "Trunc(BitAnd(a, Const64(2147483647)))"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(BitAnd, v.a, 0x7fffffff)); } },

        { "Trunc(Add(SExt32(i), SExt32(j)))"_s, [](FoldingTest& v) { LValue left = v.make(SExt32, v.i); return v.make(Trunc, v.make(Add, left, v.make(SExt32, v.j))); } },
        { "Trunc(Add(SExt32(i), Const64(1)))"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(Add, v.make(SExt32, v.i), 1)); } },
        { "Trunc(Sub(SExt32(i), SExt32(j)))"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(Sub, v.make(SExt32, v.i), v.make(SExt32, v.j))); } },

        { "i"_s, [](FoldingTest& v) { return v.lowWord(v.make(SExt32, v.i)); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.lowWord(v.int64(0x100000001ll)); } },
        { "Trunc(a)"_s, [](FoldingTest& v) { return v.lowWord(v.a); } },
        { "Trunc(a)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.a, 0x100000000ll)); } },
        { "Add(i, j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.make(SExt32, v.i), v.make(SExt32, v.j))); } },
        { "Sub(i, j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Sub, v.make(SExt32, v.i), v.make(ZExt32, v.j))); } },
        { "Mul(i, j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Mul, v.make(SExt32, v.i), v.make(SExt32, v.j))); } },
        { "BitAnd(i, j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(BitAnd, v.make(SExt32, v.i), v.make(ZExt32, v.j))); } },
        { "BitOr(i, j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(BitOr, v.make(ZExt32, v.i), v.make(SExt32, v.j))); } },
        { "BitXor(i, j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(BitXor, v.make(SExt32, v.i), v.make(ZExt32, v.j))); } },
        { "Neg(i)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Neg, v.make(SExt32, v.i))); } },
        { "Add(i, Const32(5))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.make(SExt32, v.i), 0x100000005ll)); } },
        { "Add(i, Const32(-5))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Sub, v.make(SExt32, v.i), 5)); } },
        { "Sub(Const32(5), i)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Sub, 5, v.make(SExt32, v.i))); } },
        { "Shl(i, Const32(1))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.make(SExt32, v.i), v.make(SExt32, v.i))); } },
        { "Shl(i, Const32(3))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Mul, v.make(SExt32, v.i), 8)); } },
        { "Shl(i, Const32(31))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Shl, v.make(SExt32, v.i), 31)); } },
        { "Trunc(Shl(SExt32(i), Const32(32)))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Shl, v.make(SExt32, v.i), 32)); } },
        { "Trunc(SShr(SExt32(i), Const32(1)))"_s, [](FoldingTest& v) { return v.lowWord(v.make(SShr, v.make(SExt32, v.i), 1)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.lowWord(v.make(Shl, v.make(SExt32, v.i), v.j)); } },

        { "Add(j, Sub(Shl(i, Const32(5)), i))"_s, [](FoldingTest& v) {
            LValue difference = v.make(Sub, v.make(SExt32, v.make(Shl, v.i, 5)), v.make(SExt32, v.i));
            return v.lowWord(v.make(Add, difference, v.make(SExt32, v.j)));
        } },
        { "Add(Trunc(a), j)"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.a, v.make(SExt32, v.j))); } },
        { "Add(i, Trunc(Add(a, b)))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.make(Add, v.a, v.b), v.make(SExt32, v.i))); } },
        { "Trunc(Add(a, Const64(1)))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.a, 1)); } },
        { "Trunc(Add(a, b))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.a, v.b)); } },
        { "Trunc(Mul(a, b))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Mul, v.a, v.b)); } },
        { "Trunc(Neg(a))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Neg, v.a)); } },
        { "Add(Trunc(b), Add(Trunc(a), i))"_s, [](FoldingTest& v) { return v.lowWord(v.make(Add, v.make(Add, v.a, v.make(SExt32, v.i)), v.b)); } },
        { foldingTestAnyText, [](FoldingTest& v) {
            LValue left = v.make(Add, v.a, v.make(SExt32, v.i));
            return v.lowWord(v.make(Mul, left, v.make(Sub, v.b, v.make(SExt32, v.j))));
        } },

        { "Sub(Sub(Sub(Sub(Sub(Sub(Sub(Sub(Sub(i, j), j), j), j), j), j), j), j), j)"_s, [](FoldingTest& v) {
            LValue difference = v.make(SExt32, v.i);
            for (unsigned level = 0; level < 9; ++level)
                difference = v.make(Sub, difference, v.make(SExt32, v.j));
            return v.lowWord(difference);
        } },
        { foldingTestAnyText, [](FoldingTest& v) {
            LValue difference = v.make(SExt32, v.i);
            for (unsigned level = 0; level < 3000; ++level)
                difference = v.make(Sub, difference, v.make(SExt32, v.j));
            return v.lowWord(difference);
        } },
        { foldingTestAnyText, [](FoldingTest& v) {
            LValue sum = v.make(SExt32, v.i);
            for (unsigned level = 0; level < 40; ++level) {
                LValue left = v.make(Add, sum, v.make(SExt32, v.i));
                sum = v.make(Add, left, v.make(Sub, sum, v.make(SExt32, v.j)));
            }
            return v.lowWord(sum);
        } },

        { "BitAnd(a, Const64(255))"_s, [](FoldingTest& v) { return v.make(ZExt32, v.make(Trunc, v.make(BitAnd, v.a, 0xff))); } },
        { "BitAnd(a, Const64(255))"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(Trunc, v.make(BitAnd, v.a, 0xff))); } },
        { "ZShr(a, Const32(32))"_s, [](FoldingTest& v) { return v.make(ZExt32, v.make(Trunc, v.make(ZShr, v.a, 32))); } },
        { "SExt32(Trunc(ZShr(a, Const32(32))))"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(Trunc, v.make(ZShr, v.a, 32))); } },
        { "SShr(a, Const32(32))"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(Trunc, v.make(SShr, v.a, 32))); } },
        { "ZExt32(Trunc(SShr(a, Const32(32))))"_s, [](FoldingTest& v) { return v.make(ZExt32, v.make(Trunc, v.make(SShr, v.a, 32))); } },
        { "ZExt32(Trunc(a))"_s, [](FoldingTest& v) { return v.make(ZExt32, v.make(Trunc, v.a)); } },
        { "SExt32(Trunc(a))"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(Trunc, v.a)); } },
        { "SExt32(i)"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(Trunc, v.make(ZExt32, v.i))); } },
        { "ZExt32(i)"_s, [](FoldingTest& v) { return v.make(ZExt32, v.make(Trunc, v.make(SExt32, v.i))); } },

        { "ZExt32(ZShr(i, Const32(1)))"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(ZShr, v.i, 1)); } },
        { "SExt32(i)"_s, [](FoldingTest& v) { return v.make(SExt32, v.i); } },
        { "SExt32(SShr(i, Const32(1)))"_s, [](FoldingTest& v) { return v.make(SExt32, v.make(SShr, v.i, 1)); } },

        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SExt8, v.make(SExt8, v.i)); } },
        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SExt16, v.make(SExt8, v.i)); } },
        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SExt8, v.make(SExt16, v.i)); } },
        { "SExt16(i)"_s, [](FoldingTest& v) { return v.make(SExt16, v.make(SExt16, v.i)); } },
        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SExt8, v.make(BitAnd, v.i, 0xff)); } },
        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SExt8, v.make(BitAnd, v.i, 0x1ff)); } },
        { "SExt8(BitAnd(i, Const32(240)))"_s, [](FoldingTest& v) { return v.make(SExt8, v.make(BitAnd, v.i, 0xf0)); } },
        { "SExt16(BitAnd(i, Const32(65280)))"_s, [](FoldingTest& v) { return v.make(SExt16, v.make(BitAnd, v.i, 0xff00)); } },
        { "BitAnd(i, Const32(127))"_s, [](FoldingTest& v) { return v.make(SExt8, v.make(BitAnd, v.i, 0x7f)); } },
        { "BitAnd(i, Const32(255))"_s, [](FoldingTest& v) { return v.make(SExt16, v.make(BitAnd, v.i, 0xff)); } },
        { "SExt8(i)"_s, [](FoldingTest& v) { return v.make(SExt8, v.i); } },

        { "a"_s, [](FoldingTest& v) { return v.cast(v.cast(v.a, Double), Int64); } },
        { "i"_s, [](FoldingTest& v) { return v.cast(v.cast(v.i, Float), Int32); } },
        { "BitwiseCast(a)"_s, [](FoldingTest& v) { return v.cast(v.a, Double); } },
        { "Const64(4607182418800017408)"_s, [](FoldingTest& v) { return v.cast(v.number(1.0), Int64); } },
        { "Const64(-9223372036854775808)"_s, [](FoldingTest& v) { return v.cast(v.number(-0.0), Int64); } },
        { "Const32(1065353216)"_s, [](FoldingTest& v) { return v.cast(v.single(1.0f), Int32); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.cast(v.int64(INT64_MIN), Double); } },
        { "ConstDouble(NaN(0x7ff8000000000001))"_s, [](FoldingTest& v) { return v.cast(v.int64(0x7ff8000000000001ll), Double); } },

        { "ConstDouble(3)"_s, [](FoldingTest& v) { return v.make(IToD, v.int32(3)); } },
        { "ConstDouble(-2147483648)"_s, [](FoldingTest& v) { return v.make(IToD, v.int32(INT32_MIN)); } },
        { "ConstDouble(9007199254740992)"_s, [](FoldingTest& v) { return v.make(IToD, v.int64(9007199254740993ll)); } },
        { "IToD(i)"_s, [](FoldingTest& v) { return v.make(IToD, v.i); } },
        { "IToD(i)"_s, [](FoldingTest& v) { return v.make(IToD, v.make(SExt32, v.i)); } },
        { "IToD(ZShr(i, Const32(1)))"_s, [](FoldingTest& v) { return v.make(IToD, v.make(ZExt32, v.make(ZShr, v.i, 1))); } },
        { "IToD(ZExt32(i))"_s, [](FoldingTest& v) { return v.make(IToD, v.make(ZExt32, v.i)); } },

        { "IToD(i)"_s, [](FoldingTest& v) { return v.make(Ceil, v.make(IToD, v.i)); } },
        { "IToD(i)"_s, [](FoldingTest& v) { return v.make(Floor, v.make(IToD, v.i)); } },
        { "IToD(i)"_s, [](FoldingTest& v) { return v.make(FTrunc, v.make(IToD, v.i)); } },
        { "Sqrt(IToD(i))"_s, [](FoldingTest& v) { return v.make(Sqrt, v.make(IToD, v.i)); } },

        { "IToD(Add(SExt32(i), SExt32(j)))"_s, [](FoldingTest& v) {
            LValue left = v.make(IToD, v.i);
            return v.make(Add, left, v.make(IToD, v.j));
        } },
        { "IToD(Sub(SExt32(i), SExt32(j)))"_s, [](FoldingTest& v) { return v.make(Sub, v.make(IToD, v.i), v.make(IToD, v.j)); } },
        { "IToD(Sub(SExt32(i), ZShr(a, Const32(12))))"_s, [](FoldingTest& v) { return v.make(Sub, v.make(IToD, v.i), v.make(IToD, v.make(ZShr, v.a, 12))); } },
        { "Sub(IToD(i), IToD(ZShr(a, Const32(11))))"_s, [](FoldingTest& v) { return v.make(Sub, v.make(IToD, v.i), v.make(IToD, v.make(ZShr, v.a, 11))); } },
        { "Sub(IToD(a), IToD(b))"_s, [](FoldingTest& v) { return v.make(Sub, v.make(IToD, v.a), v.make(IToD, v.b)); } },

        { "Trunc(ZShr(Add(cage, Shl(ZExt32(i), Const32(3))), Const32(3)))"_s, [](FoldingTest& v) { return v.make(Trunc, v.make(ZShr, v.make(Add, v.cage, v.make(Shl, v.make(ZExt32, v.i), 3)), 3)); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Integers, cases);
}

static void testMachineFoldingOfIntegerComparisons()
{
    static constexpr FoldingTestCase cases[] = {
        { "GreaterThan(a, Const64(5))"_s, [](FoldingTest& v) { return v.make(LessThan, 5, v.a); } },
        { "LessEqual(a, Const64(5))"_s, [](FoldingTest& v) { return v.make(GreaterEqual, 5, v.a); } },
        { "Above(a, Const64(5))"_s, [](FoldingTest& v) { return v.make(Below, 5, v.a); } },
        { "BelowEqual(i, Const32(5))"_s, [](FoldingTest& v) { return v.make(AboveEqual, 5, v.i); } },
        { "Equal(a, Const64(5))"_s, [](FoldingTest& v) { return v.make(Equal, 5, v.a); } },
        { "LessThan(a, Const64(5))"_s, [](FoldingTest& v) { return v.make(LessThan, v.a, 5); } },
        { "LessThan(b, a)"_s, [](FoldingTest& v) { return v.make(LessThan, v.b, v.a); } },
        { "LessThan(a, SExt32(i))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(IToD, v.a), v.make(IToD, v.i)); } },
        { "Equal(a, SExt32(i))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(IToD, v.i), v.make(IToD, v.a)); } },
        { "GreaterEqual(a, Const64(4503599627370496))"_s, [](FoldingTest& v) { return v.make(GreaterEqual, v.make(IToD, v.a), 0x1p52); } },
        { "LessThan(IToD(a), ConstDouble(9007199254740992))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(IToD, v.a), 0x1p53); } },
        { "Equal(IToD(a), IToD(b))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(IToD, v.a), v.make(IToD, v.b)); } },

        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(LessThan, v.int64(-1), 1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Below, v.int64(-1), 1); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Above, v.int32(-1), 1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(GreaterThan, v.int32(-1), 1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(GreaterEqual, v.int64(INT64_MIN), INT64_MAX); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.int64(INT64_MIN), INT64_MAX); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Equal, v.int64(3), 3); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.int64(3), 4); } },

        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Equal, v.a, v.a); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.a, v.a); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(LessThan, v.a, v.a); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(LessEqual, v.a, v.a); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Above, v.i, v.i); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.i, v.i); } },
        { "Equal(a, b)"_s, [](FoldingTest& v) { return v.make(Equal, v.a, v.b); } },

        { "LessThan(a, b)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(LessThan, v.a, v.b), 0); } },
        { "LessThan(a, b)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(LessThan, v.a, v.b), 1); } },
        { "GreaterEqual(a, b)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(LessThan, v.a, v.b), 0); } },
        { "GreaterEqual(a, b)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(LessThan, v.a, v.b), 1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(LessThan, v.a, v.b), 2); } },
        { "NotEqual(i, Const32(0))"_s, [](FoldingTest& v) { return v.make(NotEqual, v.i, 0); } },
        { "Equal(i, Const32(1))"_s, [](FoldingTest& v) { return v.make(Equal, v.i, 1); } },
        { "BitAnd(i, Const32(1))"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(BitAnd, v.i, 1), 0); } },
        { "Equal(BitAnd(i, Const32(1)), Const32(0))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(BitAnd, v.i, 1), 0); } },
        { "Equal(BitAnd(a, Const64(12)), Const64(0))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(BitAnd, v.a, 12), 0); } },
        { "NotEqual(BitAnd(a, Const64(12)), Const64(0))"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(BitAnd, v.a, 12), 0); } },
        { "Trunc(BitAnd(a, Const64(1)))"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(BitAnd, v.a, 1), 0); } },
        { "Trunc(BitAnd(a, Const64(1)))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(BitAnd, v.a, 1), 1); } },
        { "Equal(BitAnd(a, Const64(1)), Const64(0))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(BitAnd, v.a, 1), 0); } },
        { "Equal(a, b)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(Sub, v.a, v.b), 0); } },
        { "Equal(Sub(a, b), Const64(1))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(Sub, v.a, v.b), 1); } },
        { "LessThan(Sub(a, b), Const64(0))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(Sub, v.a, v.b), 0); } },

        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Below, v.make(BitAnd, v.a, 0xff), 256); } },
        { "Below(BitAnd(a, Const64(255)), Const64(255))"_s, [](FoldingTest& v) { return v.make(Below, v.make(BitAnd, v.a, 0xff), 255); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Above, v.a, -1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Above, v.i, -1); } },
        { "Above(a, Const64(-2))"_s, [](FoldingTest& v) { return v.make(Above, v.a, -2); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.a, 0); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Below, v.a, 0); } },
        { "GreaterEqual(a, Const64(0))"_s, [](FoldingTest& v) { return v.make(GreaterEqual, v.a, 0); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(GreaterThan, v.a, INT64_MAX); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(LessThan, v.i, INT32_MIN); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(BitAnd, v.a, 15), v.make(Add, v.make(BitAnd, v.b, 15), 16)); } },
        { "LessThan(BitAnd(a, Const64(15)), BitAnd(b, Const64(15)))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(BitAnd, v.a, 15), v.make(BitAnd, v.b, 15)); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Above, v.make(Add, v.make(BitAnd, v.a, 3), -4), 2); } },
        { "Above(Add(BitAnd(a, Const64(7)), Const64(-4)), Const64(2))"_s, [](FoldingTest& v) { return v.make(Above, v.make(Add, v.make(BitAnd, v.a, 7), -4), 2); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.make(Add, v.make(ZExt32, v.i), foldingTestTag), foldingTestTag); } },
        { "AboveEqual(Add(SExt32(i), Const64(-562949953421312)), Const64(-562949953421312))"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.make(Add, v.make(SExt32, v.i), foldingTestTag), foldingTestTag); } },

        { "Equal(a, Const64(0))"_s, [](FoldingTest& v) { return v.make(Below, v.a, 1); } },
        { "Equal(a, Const64(0))"_s, [](FoldingTest& v) { return v.make(BelowEqual, v.a, 0); } },
        { "NotEqual(a, Const64(0))"_s, [](FoldingTest& v) { return v.make(Above, v.a, 0); } },
        { "NotEqual(i, Const32(0))"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.i, 1); } },
        { "AboveEqual(i, Const32(16))"_s, [](FoldingTest& v) { return v.make(AboveEqual, v.i, 16); } },
        { "LessThan(a, Const64(1))"_s, [](FoldingTest& v) { return v.make(LessThan, v.a, 1); } },

        { "Equal(i, Const32(5))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(ZExt32, v.i), 5); } },
        { "NotEqual(i, Const32(5))"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(ZExt32, v.i), 5); } },
        { "Below(i, Const32(5))"_s, [](FoldingTest& v) { return v.make(Below, v.make(ZExt32, v.i), 5); } },
        { "Below(i, Const32(5))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(ZExt32, v.i), 5); } },
        { "Equal(i, Const32(-1))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(ZExt32, v.i), 0xffffffffll); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(ZExt32, v.i), 0x100000000ll); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(ZExt32, v.i), -1); } },
        { "LessThan(i, Const32(-5))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(SExt32, v.i), -5); } },
        { "Equal(i, Const32(-1))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(SExt32, v.i), -1); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(SExt32, v.i), 0x80000000ll); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(SExt32, v.i), 0xffffffffll); } },
        { "LessThan(i, j)"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(SExt32, v.i), v.make(SExt32, v.j)); } },
        { "Below(i, j)"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(ZExt32, v.i), v.make(ZExt32, v.j)); } },
        { "LessThan(SExt32(i), ZExt32(j))"_s, [](FoldingTest& v) { return v.make(LessThan, v.make(SExt32, v.i), v.make(ZExt32, v.j)); } },
        { "Equal(SExt32(i), ZExt32(j))"_s, [](FoldingTest& v) { LValue left = v.make(SExt32, v.i); return v.make(Equal, left, v.make(ZExt32, v.j)); } },
        { "Equal(a, SExt32(i))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(SExt32, v.i), v.a); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Integers, cases);
}

static void testMachineFoldingOfSelects()
{
    static constexpr FoldingTestCase cases[] = {
        { "a"_s, [](FoldingTest& v) { return v.select(v.int32(1), v.a, v.b); } },
        { "a"_s, [](FoldingTest& v) { return v.select(v.int64(INT64_MIN), v.a, v.b); } },
        { "b"_s, [](FoldingTest& v) { return v.select(v.int32(0), v.a, v.b); } },
        { "b"_s, [](FoldingTest& v) { return v.select(v.make(NotEqual, v.a, v.a), v.a, v.b); } },
        { "a"_s, [](FoldingTest& v) { return v.select(v.i, v.a, v.a); } },
        { "Select(i, a, b)"_s, [](FoldingTest& v) { return v.select(v.i, v.a, v.b); } },
        { "Select(LessThan(a, b), a, b)"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.a, v.b), v.a, v.b); } },

        { "LessThan(a, b)"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.a, v.b), v.int32(1), v.int32(0)); } },
        { "GreaterEqual(a, b)"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.a, v.b), v.int32(0), v.int32(1)); } },
        { "ZExt32(LessThan(a, b))"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.a, v.b), v.int64(1), v.int64(0)); } },
        { "NotEqual(i, Const32(0))"_s, [](FoldingTest& v) { return v.select(v.make(NotEqual, v.i, 0), v.int32(1), v.int32(0)); } },
        { "Select(i, Const32(1), Const32(0))"_s, [](FoldingTest& v) { return v.select(v.i, v.int32(1), v.int32(0)); } },
        { "Select(LessThan(a, b), Const32(2), Const32(0))"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.a, v.b), v.int32(2), v.int32(0)); } },
        { "Select(BitAnd(i, Const32(1)), Const32(0), Const32(1))"_s, [](FoldingTest& v) { return v.select(v.make(BitAnd, v.i, 1), v.int32(0), v.int32(1)); } },

        { "Select(i, a, b)"_s, [](FoldingTest& v) { return v.select(v.make(NotEqual, v.i, 0), v.a, v.b); } },
        { "Select(i, b, a)"_s, [](FoldingTest& v) { return v.select(v.make(Equal, v.i, 0), v.a, v.b); } },
        { "Select(i, a, b)"_s, [](FoldingTest& v) { return v.select(v.make(ZExt32, v.i), v.a, v.b); } },
        { "Select(i, a, b)"_s, [](FoldingTest& v) { return v.select(v.make(SExt32, v.i), v.a, v.b); } },
        { "Select(BitAnd(i, Const32(1)), b, a)"_s, [](FoldingTest& v) { return v.select(v.make(BitXor, v.make(BitAnd, v.i, 1), 1), v.a, v.b); } },
        { "Select(BitXor(i, Const32(1)), a, b)"_s, [](FoldingTest& v) { return v.select(v.make(BitXor, v.i, 1), v.a, v.b); } },
        { "Select(i, b, a)"_s, [](FoldingTest& v) { return v.select(v.make(ZExt32, v.make(Equal, v.i, 0)), v.a, v.b); } },
        { "Select(i, a, b)"_s, [](FoldingTest& v) { return v.select(v.make(BitXor, v.make(ZExt32, v.make(Equal, v.i, 0)), 1), v.a, v.b); } },
        { "Select(BitAnd(a, Const64(1)), i, j)"_s, [](FoldingTest& v) { return v.select(v.make(Trunc, v.make(BitAnd, v.a, 1)), v.i, v.j); } },
        { "Select(Trunc(a), i, j)"_s, [](FoldingTest& v) { return v.select(v.make(Trunc, v.a), v.i, v.j); } },
        { "Select(Equal(i, Const32(1)), a, b)"_s, [](FoldingTest& v) { return v.select(v.make(Equal, v.i, 1), v.a, v.b); } },
        { "Select(BitAnd(a, Const64(12)), j, i)"_s, [](FoldingTest& v) { return v.select(v.make(Equal, v.make(BitAnd, v.a, 12), 0), v.i, v.j); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Integers, cases);
}

static void testMachineFoldingOfNumberComparisons()
{
    static constexpr FoldingTestCase cases[] = {
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(Equal, v.number(foldingTestNaN), foldingTestNaN); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(LessThan, v.number(foldingTestNaN), 1.0); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(GreaterThan, v.number(1.0), foldingTestNaN); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(LessEqual, v.number(foldingTestNaN), foldingTestNaN); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(GreaterEqual, v.number(foldingTestNaN), 1.0); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.number(foldingTestNaN), foldingTestNaN); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(EqualOrUnordered, v.number(foldingTestNaN), 1.0); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(EqualOrUnordered, v.number(1.0), 2.0); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(EqualOrUnordered, v.number(2.0), 2.0); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(Equal, v.number(0.0), -0.0); } },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(LessThan, v.number(-0.0), 0.0); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(LessThan, v.number(-foldingTestInfinity), foldingTestInfinity); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(LessThan, v.single(1.5f), 2.5f); }, FoldingTestAnswers::NotCompared },
        { "Const32(0)"_s, [](FoldingTest& v) { return v.make(LessThan, v.x, foldingTestNaN); } },
        { "Const32(1)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.x, foldingTestNaN); } },

        { "Equal(x, x)"_s, [](FoldingTest& v) { return v.make(Equal, v.x, v.x); } },
        { "NotEqual(x, x)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.x, v.x); } },
        { "LessEqual(x, x)"_s, [](FoldingTest& v) { return v.make(LessEqual, v.x, v.x); } },
        { "EqualOrUnordered(x, x)"_s, [](FoldingTest& v) { return v.make(EqualOrUnordered, v.x, v.x); } },
        { "LessThan(f, g)"_s, [](FoldingTest& v) { return v.make(LessThan, v.f, v.g); }, FoldingTestAnswers::NotCompared },
        { "GreaterThan(x, ConstDouble(1))"_s, [](FoldingTest& v) { return v.make(LessThan, 1.0, v.x); } },
        { "LessEqual(x, ConstDouble(1))"_s, [](FoldingTest& v) { return v.make(GreaterEqual, 1.0, v.x); } },
        { "Equal(x, ConstDouble(0))"_s, [](FoldingTest& v) { return v.make(Equal, v.x, 0.0); } },

        { "BitXor(LessThan(x, y), Const32(1))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(LessThan, v.x, v.y), 1); } },
        { "BitXor(GreaterEqual(x, y), Const32(1))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(GreaterEqual, v.x, v.y), 1); } },
        { "BitXor(EqualOrUnordered(x, y), Const32(1))"_s, [](FoldingTest& v) { return v.make(BitXor, v.make(EqualOrUnordered, v.x, v.y), 1); } },
        { "Equal(LessThan(x, y), Const32(0))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(LessThan, v.x, v.y), 0); } },
        { "NotEqual(LessThan(x, y), Const32(1))"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(LessThan, v.x, v.y), 1); } },
        { "LessThan(x, y)"_s, [](FoldingTest& v) { return v.make(NotEqual, v.make(LessThan, v.x, v.y), 0); } },
        { "LessThan(x, y)"_s, [](FoldingTest& v) { return v.make(Equal, v.make(BitXor, v.make(LessThan, v.x, v.y), 1), 0); } },
        { "Equal(Sub(x, y), ConstDouble(0))"_s, [](FoldingTest& v) { return v.make(Equal, v.make(Sub, v.x, v.y), 0.0); } },

        { "LessThan(x, y)"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.x, v.y), v.int32(1), v.int32(0)); } },
        { "Select(LessThan(x, y), Const32(0), Const32(1))"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.x, v.y), v.int32(0), v.int32(1)); } },
        { "Select(LessThan(x, y), y, x)"_s, [](FoldingTest& v) { return v.select(v.make(BitXor, v.make(LessThan, v.x, v.y), 1), v.x, v.y); } },
        { "Select(LessThan(x, y), y, x)"_s, [](FoldingTest& v) { return v.select(v.make(Equal, v.make(LessThan, v.x, v.y), 0), v.x, v.y); } },
        { "Select(Equal(x, ConstDouble(0)), x, y)"_s, [](FoldingTest& v) { return v.select(v.make(Equal, v.x, 0.0), v.x, v.y); } },
        { "x"_s, [](FoldingTest& v) { return v.select(v.make(LessThan, v.x, v.y), v.x, v.x); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Numbers, cases);
}

static void testMachineFoldingOfNumbers()
{
    static constexpr FoldingTestCase cases[] = {
        { "ConstDouble(3.75)"_s, [](FoldingTest& v) { return v.make(Add, v.number(1.5), 2.25); } },
        { "ConstFloat(3.75)"_s, [](FoldingTest& v) { return v.make(Add, v.single(1.5f), 2.25f); }, FoldingTestAnswers::NotCompared },
        { "ConstFloat(16777216)"_s, [](FoldingTest& v) { return v.make(Add, v.single(16777216.0f), 1.0f); }, FoldingTestAnswers::NotCompared },
        { "ConstDouble(16777217)"_s, [](FoldingTest& v) { return v.make(Add, v.number(16777216.0), 1.0); } },
        { "ConstDouble(-0.75)"_s, [](FoldingTest& v) { return v.make(Sub, v.number(1.5), 2.25); } },
        { "ConstDouble(3.375)"_s, [](FoldingTest& v) { return v.make(Mul, v.number(1.5), 2.25); } },
        { "ConstDouble(0.75)"_s, [](FoldingTest& v) { return v.make(Div, v.number(1.5), 2.0); } },
        { "ConstDouble(Infinity)"_s, [](FoldingTest& v) { return v.make(Div, v.number(1.0), 0.0); } },
        { "ConstDouble(-Infinity)"_s, [](FoldingTest& v) { return v.make(Div, v.number(1.0), -0.0); } },
        { "ConstDouble(Infinity)"_s, [](FoldingTest& v) { return v.make(Mul, v.number(1e300), 1e300); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Div, v.number(0.0), 0.0); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Sub, v.number(foldingTestInfinity), foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Mul, v.number(foldingTestInfinity), 0.0); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Sqrt, v.number(-1.0)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Add, v.cast(v.int64(-1), Double), 1.0); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Add, v.number(foldingTestInfinity), -foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Add, v.number(-foldingTestInfinity), foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Sub, v.number(-foldingTestInfinity), -foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Mul, v.number(0.0), foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Mul, v.number(-0.0), foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Mul, v.number(-foldingTestInfinity), 0.0); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Div, v.number(-0.0), 0.0); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Div, v.number(foldingTestInfinity), foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Div, v.number(-foldingTestInfinity), foldingTestInfinity); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Sqrt, v.number(-foldingTestInfinity)); } },
        { foldingTestTextOfInvalidOperation, [](FoldingTest& v) { return v.make(Sqrt, v.number(-std::numeric_limits<double>::denorm_min())); } },
        { foldingTestTextOfInvalidOperationOnFloats, [](FoldingTest& v) { return v.make(Div, v.single(0.0f), 0.0f); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfInvalidOperationOnFloats, [](FoldingTest& v) { return v.make(Sub, v.single(std::numeric_limits<float>::infinity()), std::numeric_limits<float>::infinity()); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfInvalidOperationOnFloats, [](FoldingTest& v) { return v.make(Mul, v.single(std::numeric_limits<float>::infinity()), 0.0f); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfInvalidOperationOnFloats, [](FoldingTest& v) { return v.make(Sqrt, v.single(-1.0f)); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfPureNaNOfFloats, [](FoldingTest& v) { return v.make(Add, v.single(std::numeric_limits<float>::quiet_NaN()), 1.0f); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfNegativeNaNOfFloats, [](FoldingTest& v) { return v.make(Neg, v.single(std::numeric_limits<float>::quiet_NaN())); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Add, v.cast(v.int64(foldingTestBitsOfNaNWithPayload), Double), 1.0); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Sub, v.number(1.0), v.cast(v.int64(foldingTestBitsOfNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Sub, v.number(1.0), v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Mul, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double), 0.0); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Mul, v.number(foldingTestInfinity), v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Div, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double), 0.0); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Div, v.number(0.0), v.cast(v.int64(foldingTestBitsOfNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Add, v.cast(v.int64(foldingTestBitsOfNegativeSignalingNaN), Double), 1.0); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Add, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double), v.cast(v.int64(foldingTestBitsOfNaNWithPayload), Double)); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Add, v.cast(v.int64(foldingTestBitsOfNaNWithPayload), Double), v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); }, FoldingTestAnswers::NotCompared },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Sqrt, v.cast(v.int64(foldingTestBitsOfNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Sqrt, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Ceil, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Floor, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(FTrunc, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Ceil, v.number(foldingTestNaN)); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Floor, v.number(foldingTestNaN)); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(FTrunc, v.number(foldingTestNaN)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(Neg, v.number(foldingTestNaN)); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Neg, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Abs, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(Abs, v.number(foldingTestNaN)); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(Mul, v.number(-1.0), 0.0); } },
        { "ConstDouble(0)"_s, [](FoldingTest& v) { return v.make(Add, v.number(-0.0), 0.0); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(Add, v.number(-0.0), -0.0); } },
        { "ConstDouble(0)"_s, [](FoldingTest& v) { return v.make(Sub, v.number(1.5), 1.5); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(Neg, v.number(0.0)); } },
        { "ConstDouble(0)"_s, [](FoldingTest& v) { return v.make(Abs, v.number(-0.0)); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(Ceil, v.number(-0.5)); } },
        { "ConstDouble(-1)"_s, [](FoldingTest& v) { return v.make(Floor, v.number(-0.5)); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(FTrunc, v.number(-0.5)); } },
        { "ConstDouble(2)"_s, [](FoldingTest& v) { return v.make(Ceil, v.number(1.5)); } },
        { "ConstDouble(2)"_s, [](FoldingTest& v) { return v.make(Sqrt, v.number(4.0)); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(Sqrt, v.number(-0.0)); } },

        { "Add(x, ConstDouble(0))"_s, [](FoldingTest& v) { return v.make(Add, v.x, 0.0); } },
        { "Add(x, ConstDouble(0))"_s, [](FoldingTest& v) { return v.make(Add, 0.0, v.x); } },
        { "x"_s, [](FoldingTest& v) { return v.make(Sub, v.x, 0.0); } },
        { "x"_s, [](FoldingTest& v) { return v.make(Mul, v.x, 1.0); } },
        { "x"_s, [](FoldingTest& v) { return v.make(Mul, 1.0, v.x); } },
        { "x"_s, [](FoldingTest& v) { return v.make(Div, v.x, 1.0); } },
        { "Mul(x, ConstDouble(0))"_s, [](FoldingTest& v) { return v.make(Mul, v.x, 0.0); } },
        { "Sub(x, x)"_s, [](FoldingTest& v) { return v.make(Sub, v.x, v.x); } },
        { "Mul(x, ConstDouble(2))"_s, [](FoldingTest& v) { return v.make(Mul, v.x, 2.0); } },
        { "Add(x, x)"_s, [](FoldingTest& v) { return v.make(Add, v.x, v.x); } },
        { "Sub(ConstDouble(0), x)"_s, [](FoldingTest& v) { return v.make(Sub, 0.0, v.x); } },
        { "Div(ConstDouble(1), x)"_s, [](FoldingTest& v) { return v.make(Div, 1.0, v.x); } },
        { "Add(Add(x, ConstDouble(1)), ConstDouble(2))"_s, [](FoldingTest& v) { return v.make(Add, v.make(Add, v.x, 1.0), 2.0); } },
        { "Sub(Add(x, y), y)"_s, [](FoldingTest& v) { return v.make(Sub, v.make(Add, v.x, v.y), v.y); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Add, v.x, -0.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Sub, v.x, -0.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Mul, v.x, -1.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Div, v.x, -1.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Div, v.x, 2.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Div, v.x, 3.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Div, v.x, v.x); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Neg, v.make(Sub, v.x, v.y)); } },

        { "x"_s, [](FoldingTest& v) { return v.make(Neg, v.make(Neg, v.x)); } },
        { "Neg(x)"_s, [](FoldingTest& v) { return v.make(Neg, v.x); } },
        { "Abs(x)"_s, [](FoldingTest& v) { return v.make(Abs, v.make(Abs, v.x)); } },
        { "Abs(x)"_s, [](FoldingTest& v) { return v.make(Abs, v.make(Neg, v.x)); } },
        { "Neg(Abs(x))"_s, [](FoldingTest& v) { return v.make(Neg, v.make(Abs, v.x)); } },
        { "Ceil(x)"_s, [](FoldingTest& v) { return v.make(Ceil, v.make(Ceil, v.x)); } },
        { "Floor(x)"_s, [](FoldingTest& v) { return v.make(Floor, v.make(Floor, v.x)); } },
        { "FTrunc(x)"_s, [](FoldingTest& v) { return v.make(FTrunc, v.make(FTrunc, v.x)); } },
        { "Ceil(x)"_s, [](FoldingTest& v) { return v.make(Ceil, v.x); } },
        { "Floor(Sqrt(x))"_s, [](FoldingTest& v) { return v.make(Floor, v.make(Sqrt, v.x)); } },
        { "Sqrt(Sqrt(x))"_s, [](FoldingTest& v) { return v.make(Sqrt, v.make(Sqrt, v.x)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Ceil, v.make(Floor, v.x)); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(Floor, v.make(FTrunc, v.x)); } },

        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(FMin, v.number(foldingTestNaN), 1.0); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(FMin, v.number(1.0), foldingTestNaN); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(FMax, v.number(foldingTestNaN), 1.0); } },
        { foldingTestTextOfPureNaN, [](FoldingTest& v) { return v.make(FMax, v.number(1.0), foldingTestNaN); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(FMin, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double), 1.0); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(FMin, v.number(1.0), v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(FMax, v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double), 1.0); } },
        { foldingTestTextOfNegativeNaN, [](FoldingTest& v) { return v.make(FMax, v.number(1.0), v.cast(v.int64(foldingTestBitsOfNegativeNaNWithPayload), Double)); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(FMin, v.number(0.0), -0.0); } },
        { "ConstDouble(-0)"_s, [](FoldingTest& v) { return v.make(FMin, v.number(-0.0), 0.0); } },
        { "ConstDouble(0)"_s, [](FoldingTest& v) { return v.make(FMax, v.number(0.0), -0.0); } },
        { "ConstDouble(0)"_s, [](FoldingTest& v) { return v.make(FMax, v.number(-0.0), 0.0); } },
        { "ConstDouble(1)"_s, [](FoldingTest& v) { return v.make(FMin, v.number(2.0), 1.0); } },
        { "ConstDouble(2)"_s, [](FoldingTest& v) { return v.make(FMax, v.number(2.0), 1.0); } },
        { "ConstDouble(-Infinity)"_s, [](FoldingTest& v) { return v.make(FMin, v.number(-foldingTestInfinity), 1.0); } },
        { "FMin(x, y)"_s, [](FoldingTest& v) { return v.make(FMin, v.x, v.y); } },
        { "FMax(x, y)"_s, [](FoldingTest& v) { return v.make(FMax, v.x, v.y); } },
        { "FMin(x, ConstDouble(0))"_s, [](FoldingTest& v) { return v.make(FMin, v.x, 0.0); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(FMin, v.x, v.x); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(FMax, v.x, foldingTestNaN); } },
        { foldingTestAnyText, [](FoldingTest& v) { return v.make(FMax, v.x, -foldingTestInfinity); } },

        { "ConstDouble(1.5)"_s, [](FoldingTest& v) { return v.make(FloatToDouble, v.single(1.5f)); } },
        { "ConstFloat(1.5)"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.number(1.5)); } },
        { "ConstFloat(16777216)"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.number(16777217.0)); } },
        { "ConstFloat(Infinity)"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.number(1e300)); } },
        { "ConstFloat(-0)"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.number(-0.0)); } },
        { "f"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(FloatToDouble, v.f)); } },
        { "FloatToDouble(DoubleToFloat(x))"_s, [](FoldingTest& v) { return v.make(FloatToDouble, v.make(DoubleToFloat, v.x)); } },
        { "Add(f, DoubleToFloat(y))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Add, v.make(FloatToDouble, v.f), v.make(FloatToDouble, v.make(DoubleToFloat, v.y)))); } },
        { "Sub(f, DoubleToFloat(y))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Sub, v.make(FloatToDouble, v.f), v.make(FloatToDouble, v.make(DoubleToFloat, v.y)))); } },
        { "Mul(f, DoubleToFloat(y))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Mul, v.make(FloatToDouble, v.f), v.make(FloatToDouble, v.make(DoubleToFloat, v.y)))); } },
        { "Div(f, DoubleToFloat(y))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Div, v.make(FloatToDouble, v.f), v.make(FloatToDouble, v.make(DoubleToFloat, v.y)))); } },
        { "Sqrt(f)"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Sqrt, v.make(FloatToDouble, v.f))); } },
        { "Mul(f, ConstFloat(0.5))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Mul, v.make(FloatToDouble, v.f), 0.5)); } },
        { "DoubleToFloat(Mul(FloatToDouble(f), ConstDouble(0.1)))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Mul, v.make(FloatToDouble, v.f), 0.1)); } },
        { "DoubleToFloat(Add(y, FloatToDouble(f)))"_s, [](FoldingTest& v) { return v.make(DoubleToFloat, v.make(Add, v.make(FloatToDouble, v.f), v.y)); } },
        { "x"_s, [](FoldingTest& v) { return v.cast(v.cast(v.x, Int64), Double); } },
        { "BitwiseCast(x)"_s, [](FoldingTest& v) { return v.cast(v.x, Int64); } },
    };
    checkFoldingTestCases(FoldingTestOperands::Numbers, cases);
}

static void testMachineFoldingOfBranchesAndChecks()
{
    FoldingTest v(FoldingTestOperands::Integers, FoldingTestForm::Folded);
    Output& out = v.out();
    Block* entry = out.m_block;
    Block* first = out.newBlock();
    Block* second = out.newBlock();
    Block* third = out.newBlock();
    Block* fourth = out.newBlock();
    Block* fifth = out.newBlock();

    out.branch(out.isZero32(v.i), first, second);
    v.expectTree(entry->last(), makeString("Branch(i) -> #"_s, second->index(), ", #"_s, first->index()));

    out.appendTo(first);
    out.branch(out.notZero64(out.zeroExtPtr(v.i)), third, rareEdge(fourth));
    v.expectTree(first->last(), makeString("Branch(i) -> #"_s, third->index(), ", rare #"_s, fourth->index()));

    out.appendTo(second);
    out.branch(out.equal(v.a, v.a), rareEdge(third), fourth);
    v.expectTree(second->last(), makeString("Jump() -> #"_s, third->index()));

    out.appendTo(third);
    out.branch(out.logicalNot(out.below(v.a, 0)), fourth, third);
    v.expectTree(third->last(), makeString("Jump() -> #"_s, fourth->index()));

    out.appendTo(fourth);
    AOT_TEST_CHECK(!out.check(out.notEqual(v.a, v.a))->owner());
    AOT_TEST_CHECK(fourth->isEmpty());
    PatchOperation* check = out.check(out.notEqual(v.a, v.b));
    AOT_TEST_CHECK(check->owner() == fourth && check->child(0)->opcode() == NotEqual);
    AOT_TEST_CHECK(out.check(out.equal(v.a, v.a))->owner() == fourth);
    AOT_TEST_CHECK(out.check(out.notZero32(v.i))->child(0) == v.i);
    AOT_TEST_CHECK(out.check(out.isZero32(v.i))->child(0)->opcode() == Equal);

    out.appendTo(fifth);
    LValue hasLowBit = out.testNonZero64(v.a, 1);
    out.branch(hasLowBit, first, second);
    v.expectTree(fifth->last(), makeString("Branch(BitAnd(a, Const64(1))) -> #"_s, first->index(), ", #"_s, second->index()));
    const Operation* bitTest = fifth->last()->child(0);
    AOT_TEST_CHECK(bitTest->opcode() == BitAnd && bitTest->useCount() == 1);
    AOT_TEST_CHECK(hasLowBit->opcode() == Trunc && hasLowBit->child(0) == bitTest && !hasLowBit->useCount());
}

} // namespace Machine

std::span<const NativeTest> machineFoldingTests()
{
    static constexpr NativeTest tests[] = {
        { "machine-folding/divisions"_s, Machine::testMachineFoldingOfDivisions },
        { "machine-folding/bit-operations"_s, Machine::testMachineFoldingOfBitOperations },
        { "machine-folding/shifts"_s, Machine::testMachineFoldingOfShifts },
        { "machine-folding/integer-conversions"_s, Machine::testMachineFoldingOfIntegerConversions },
        { "machine-folding/integer-comparisons"_s, Machine::testMachineFoldingOfIntegerComparisons },
        { "machine-folding/selects"_s, Machine::testMachineFoldingOfSelects },
        { "machine-folding/number-comparisons"_s, Machine::testMachineFoldingOfNumberComparisons },
        { "machine-folding/numbers"_s, Machine::testMachineFoldingOfNumbers },
        { "machine-folding/branches-and-checks"_s, Machine::testMachineFoldingOfBranchesAndChecks },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
