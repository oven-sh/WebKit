/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTInt32Proof.h"
#include "AOTResultRuleMonotonicity.h"
#include "AOTResultRules.h"
#include "AOTSupportTestCommands.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <climits>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<FunctionID> parseFunctionWord(const Program& program, StringView word)
{
    if (word.length() < 2 || word[0] != 'f')
        return std::nullopt;
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(word.substring(1));
    if (!index || *index >= program.functions.size())
        return std::nullopt;
    return FunctionID(*index);
}

static std::optional<Vector<FunctionID>> parseFunctionWords(TestCommandContext& context, std::span<const String> words)
{
    const Program& program = *context.program;
    Vector<FunctionID> functions;
    if (words.empty()) {
        for (size_t index = 0; index < program.functions.size(); ++index)
            functions.append(FunctionID(index));
        return functions;
    }
    for (const String& word : words) {
        std::optional<FunctionID> function = parseFunctionWord(program, word);
        if (!function) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        functions.append(*function);
    }
    return functions;
}

static Vector<StringView> linesOf(StringView text)
{
    Vector<StringView> lines;
    for (StringView line : text.splitAllowingEmptyEntries('\n'))
        lines.append(line);
    return lines;
}

static StringView lineAt(const Vector<StringView>& lines, size_t index)
{
    return index < lines.size() ? lines[index] : StringView();
}

static void printRoundTripComparison(PrintStream& out, const String& firstText, const String& secondText)
{
    if (firstText == secondText) {
        out.println("round trip: same"_s);
        return;
    }
    Vector<StringView> first = linesOf(firstText);
    Vector<StringView> second = linesOf(secondText);
    size_t numberOfSharedLines = std::min(first.size(), second.size());
    size_t index = 0;
    while (index < numberOfSharedLines && first[index] == second[index])
        ++index;
    out.println("round trip: differs at line "_s, index + 1);
    out.println("first: "_s, lineAt(first, index));
    out.println("second: "_s, lineAt(second, index));
}

static void runRoundTrip(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    PrintedFields fields = PrintedFields::All;
    if (!words.empty() && words.front() == "wire"_s) {
        fields = PrintedFields::WireOnly;
        words = words.subspan(1);
    }
    if (!words.empty()) {
        printBadArgument(context.out, words.front());
        return;
    }

    StringPrintStream first;
    printProgram(first, *context.program, fields);
    String firstText = first.toString();
    context.out.print(firstText);

    std::unique_ptr<Program> again = parseProgram(firstText, context.form, context.build, VerifierInputs { .isOptimizing = context.isOptimizing });
    if (!again) {
        context.out.println("round trip: does not parse"_s);
        return;
    }
    StringPrintStream second;
    printProgram(second, *again, fields);
    printRoundTripComparison(context.out, firstText, second.toString());
}

static void runPrint(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    if (words.empty()) {
        printProgram(context.out, *context.program);
        return;
    }
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;
    for (FunctionID id : *functions)
        printFunction(context.out, *context.program, id, context.program->at(id));
}

static void typeFunction(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    typeValuesFromNever({ program, id, function, analyses, types, build });
}

static void typeProgramAsRead(Program& program, BuildContext& build)
{
    TypeArena types(program.types);
    for (size_t index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (function.hasBody())
            typeFunction(program, FunctionID(index), function, types, build);
    }
}

static void runVerify(TestCommandContext& context)
{
    PlacedTableSizes unboundedSizes { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
    VerifierInputs inputs { .placed = &unboundedSizes };
    for (const String& word : context.arguments) {
        if (word == "optimizing"_s)
            inputs.isOptimizing = true;
        else if (word == "casts-name-their-facts"_s)
            inputs.castsNameTheirFacts = true;
        else {
            printBadArgument(context.out, word);
            return;
        }
    }

    if (context.form == IRForm::AsRead)
        typeProgramAsRead(*context.program, context.build);
    if (verifyProgram(*context.program, context.form, context.build, inputs))
        context.out.println("valid"_s);
}

static void printTypeAndExpectedType(PrintStream& out, const TypingContext& typing, ValueID id)
{
    const Value& value = typing.function.at(id);
    out.print("v"_s, id.index(), ": "_s);
    printType(out, typing.program, value.type);
    if (TypeID expected = value.expected().predicted()) {
        out.print(" ~ "_s);
        printType(out, typing.program, expected);
    }
    out.print("\n"_s);
}

static void printResultTypes(PrintStream& out, const TypingContext& typing, ValueID id)
{
    const Value& value = typing.function.at(id);
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(typing.program, typing.function, value);
    if (!numberOfResults) {
        if (resultRuleOf(value.opcode) != ResultRule::None)
            printTypeAndExpectedType(out, typing, id);
        return;
    }
    for (uint32_t resultIndex = 0; resultIndex < *numberOfResults; ++resultIndex) {
        out.print("v"_s, id.index(), "."_s, resultIndex, ": "_s);
        printType(out, typing.program, computeResultType(typing, value, resultIndex));
        out.print("\n"_s);
    }
}

static void printResultTypesOfFunction(TestCommandContext& context, FunctionID id, TypeArena& types)
{
    const Program& program = *context.program;
    Function function = program.at(id);
    typeFunction(program, id, function, types, context.build);

    context.out.println("function f"_s, id.index());
    TypingContext typing { program, function, types };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            printTypeAndExpectedType(context.out, typing, parameter);
        for (ValueID value : block.values)
            printResultTypes(context.out, typing, value);
    }
}

static void runResultTypes(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    TypeArena types(context.program->types);
    for (FunctionID id : *functions) {
        if (context.program->at(id).hasBody())
            printResultTypesOfFunction(context, id, types);
    }
}

static void printBounds(PrintStream& out, const NumberBounds& bounds)
{
    switch (bounds.kind) {
    case BoundsKind::Unknown:
        out.print("unknown "_s, nameOf(bounds.failure));
        break;
    case BoundsKind::None:
        out.print("no number"_s);
        break;
    case BoundsKind::Integers:
        out.print("integers ["_s, bounds.range.minimum, ", "_s, bounds.range.maximum, "]"_s);
        break;
    case BoundsKind::Reals:
        out.print("reals ["_s, bounds.range.minimum, ", "_s, bounds.range.maximum, "]"_s);
        break;
    }
    out.println(bounds.mayBeNullish ? " or nullish"_s : ""_s);
}

static void printInt32BoundsOfFunction(TestCommandContext& context, FunctionID id, TypeArena& types)
{
    const Program& program = *context.program;
    Function function = program.at(id);
    FunctionAnalyses analyses(AnalysisContext { context.build, program, nullptr, nullptr, id }, function);
    typeValuesFromNever({ program, id, function, analyses, types, context.build });

    context.out.println("function f"_s, id.index());
    Int32Prover prover({ program, function, analyses, types, context.build });
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            const Value& request = function.at(value);
            if (request.opcode != Opcode::Convert || !request.declared().has(PositionFlags::IsInt32Promise))
                continue;
            context.out.print("v"_s, value.index(), ": "_s);
            printBounds(context.out, prover.boundsOf(request.valueOperand()));
        }
    }
}

static void runInt32Bounds(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    TypeArena types(context.program->types);
    for (FunctionID id : *functions) {
        if (context.program->at(id).hasBody())
            printInt32BoundsOfFunction(context, id, types);
    }
}

void BrokenResultRule::dump(PrintStream& out, const Program& program, const Function& function) const
{
    out.print("v", value.index(), " ", nameOf(function.at(value).opcode), " operand ", operandIndex, ": ");
    printType(out, program, narrowOperand);
    out.print(" gives ");
    printType(out, program, resultOfNarrowOperand);
    switch (law) {
    case Law::NarrowerOperandGivesNarrowerResult:
        out.print(", ");
        printType(out, program, wideOperand);
        out.print(" gives ");
        break;
    case Law::BothPathsAgree:
        out.print(" by type ids, by the rule ");
        break;
    }
    printType(out, program, resultOfWideOperand);
}

static void runLawsOfResultRules(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    const Program& program = *context.program;
    TypeArena types(context.program->types);
    ShapesWithMonotoneResult checkedShapes;
    for (FunctionID id : *functions) {
        if (!program.at(id).hasBody())
            continue;
        Function function = program.at(id);
        typeFunction(program, id, function, types, context.build);
        std::optional<BrokenResultRule> found = findBrokenResultRule(program, function, types, checkedShapes);
        context.out.print("function f"_s, id.index(), ": "_s);
        if (found)
            found->dump(context.out, program, function);
        else
            context.out.print("every rule keeps both laws"_s);
        context.out.println();
    }
}

#define FOR_EACH_LAW_OF_TYPES(v) \
    v(SubsetHoldsItself) \
    v(MeetLiesInFirst) \
    v(MeetLiesInSecond) \
    v(MeetIsTheSameBothWays) \
    v(MeetIsWrittenTheSameBothWays) \
    v(MeetWithItselfIsItself) \
    v(MeetHoldsWhatLiesInBoth) \
    v(CastLiesInFirst) \
    v(CastLiesInSecond) \
    v(CastWithItselfIsItself) \
    v(CastHoldsWhatLiesInBoth) \
    v(JoinHoldsFirst) \
    v(JoinHoldsSecond) \
    v(JoinIsTheSameBothWays) \
    v(JoinIsWrittenTheSameBothWays) \
    v(JoinWithItselfIsItself) \
    v(JoinLiesInWhatHoldsBoth) \
    v(DifferenceLiesInFirst) \
    v(DifferenceMeetsSecondNowhere) \
    v(DifferenceWithItselfIsNever) \
    v(DifferenceHoldsWhatSecondLacks) \
    v(DifferenceAndSecondHoldFirst) \
    v(OverlapIsTheSameBothWays) \
    v(WhatLiesInsideOverlaps) \
    v(WhatDoesNotOverlapMeetsNowhere) \
    v(WhatOverlapsMeetsSomewhere)

enum class LawOfTypes : uint8_t {
#define DECLARE_LAW(name) name,
    FOR_EACH_LAW_OF_TYPES(DECLARE_LAW)
#undef DECLARE_LAW
};

static ASCIILiteral nameOf(LawOfTypes law)
{
    switch (law) {
#define NAME_LAW(name) case LawOfTypes::name: return #name ## _s;
    FOR_EACH_LAW_OF_TYPES(NAME_LAW)
#undef NAME_LAW
    }
    return "?"_s;
}

class LawsOfTypes {
public:
    LawsOfTypes(PrintStream& out, Program& program, unsigned mostTypes)
        : m_out(out)
        , m_program(program)
        , m_types(program.types)
    {
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> shapes;
        m_numberOfTypes = program.types.size();
        for (bool takesShapesSeen : { false, true }) {
            for (unsigned index = 0; index < m_numberOfTypes && m_corpus.size() < mostTypes; ++index) {
                const Type& type = program.types.at(TypeID(index));
                if (!type.isJavaScriptValue())
                    continue;
                if (takesShapesSeen ? m_corpus.contains(type) : !shapes.add(shapeOf(type)).isNewEntry)
                    continue;
                m_corpus.append(type);
            }
        }
    }

    unsigned run()
    {
        size_t size = m_corpus.size();
        Vector<BitVector> inside(size);
        Vector<BitVector> overlaps(size);
        for (size_t a = 0; a < size; ++a) {
            for (size_t b = 0; b < size; ++b) {
                if (liesIn(m_corpus[a], m_corpus[b]))
                    inside[a].set(b);
                if (m_corpus[a].overlaps(m_corpus[b], m_program))
                    overlaps[a].set(b);
            }
        }
        for (size_t a = 0; a < size; ++a) {
            const Type& first = m_corpus[a];
            expect(LawOfTypes::SubsetHoldsItself, inside[a].get(a), first, first, nullptr, first);
            expect(LawOfTypes::MeetWithItselfIsItself, first.intersectWith(first, m_types) == first, first, first, nullptr, first.intersectWith(first, m_types));
            expect(LawOfTypes::CastWithItselfIsItself, first.castTo(first, m_types) == first, first, first, nullptr, first.castTo(first, m_types));
            expect(LawOfTypes::JoinWithItselfIsItself, first.unionWith(first, m_types) == first, first, first, nullptr, first.unionWith(first, m_types));
            expect(LawOfTypes::DifferenceWithItselfIsNever, first.without(first, m_types).isNever(), first, first, nullptr, first.without(first, m_types));
            for (size_t b = 0; b < size; ++b) {
                const Type& second = m_corpus[b];
                Type meet = first.intersectWith(second, m_types);
                Type cast = first.castTo(second, m_types);
                Type join = first.unionWith(second, m_types);
                Type difference = first.without(second, m_types);

                expect(LawOfTypes::MeetLiesInFirst, liesIn(meet, first), first, second, nullptr, meet);
                expect(LawOfTypes::MeetLiesInSecond, liesIn(meet, second), first, second, nullptr, meet);
                expect(LawOfTypes::CastLiesInFirst, liesIn(cast, first), first, second, nullptr, cast);
                expect(LawOfTypes::CastLiesInSecond, liesIn(cast, second), first, second, nullptr, cast);
                expect(LawOfTypes::JoinHoldsFirst, liesIn(first, join), first, second, nullptr, join);
                expect(LawOfTypes::JoinHoldsSecond, liesIn(second, join), first, second, nullptr, join);
                expect(LawOfTypes::DifferenceLiesInFirst, liesIn(difference, first), first, second, nullptr, difference);
                expect(LawOfTypes::DifferenceMeetsSecondNowhere, difference.intersectWith(second, m_types).isNever(), first, second, nullptr, difference);
                expect(LawOfTypes::DifferenceAndSecondHoldFirst, liesIn(first, difference.unionWith(second, m_types)), first, second, nullptr, difference);
                expect(LawOfTypes::OverlapIsTheSameBothWays, overlaps[a].get(b) == overlaps[b].get(a), first, second, nullptr, meet);
                expect(LawOfTypes::WhatLiesInsideOverlaps, !inside[a].get(b) || first.isNever() || overlaps[a].get(b), first, second, nullptr, meet);
                expect(LawOfTypes::WhatDoesNotOverlapMeetsNowhere, overlaps[a].get(b) || meet.isNever(), first, second, nullptr, meet);
                expect(LawOfTypes::WhatOverlapsMeetsSomewhere, !overlaps[a].get(b) || !meet.isNever(), first, second, nullptr, meet);
                if (inside[a].get(b))
                    expect(LawOfTypes::JoinLiesInWhatHoldsBoth, liesIn(join, second), first, second, nullptr, join);
                if (a < b) {
                    Type meetTurned = second.intersectWith(first, m_types);
                    bool isTheSame = liesIn(meet, meetTurned) && liesIn(meetTurned, meet);
                    expect(LawOfTypes::MeetIsTheSameBothWays, isTheSame, first, second, &meetTurned, meet);
                    expect(LawOfTypes::MeetIsWrittenTheSameBothWays, !isTheSame || meet == meetTurned, first, second, &meetTurned, meet);
                    Type joinTurned = second.unionWith(first, m_types);
                    isTheSame = liesIn(join, joinTurned) && liesIn(joinTurned, join);
                    expect(LawOfTypes::JoinIsTheSameBothWays, isTheSame, first, second, &joinTurned, join);
                    expect(LawOfTypes::JoinIsWrittenTheSameBothWays, !isTheSame || join == joinTurned, first, second, &joinTurned, join);
                }
                for (size_t c = 0; c < size; ++c) {
                    if (!inside[c].get(a))
                        continue;
                    const Type& third = m_corpus[c];
                    if (inside[c].get(b)) {
                        expect(LawOfTypes::MeetHoldsWhatLiesInBoth, liesIn(third, meet), first, second, &third, meet);
                        expect(LawOfTypes::CastHoldsWhatLiesInBoth, liesIn(third, cast), first, second, &third, cast);
                    }
                    if (!overlaps[c].get(b) && !overlaps[b].get(c))
                        expect(LawOfTypes::DifferenceHoldsWhatSecondLacks, liesIn(third, difference), first, second, &third, difference);
                }
            }
        }

        unsigned shapes = 0;
        unsigned numberThatHold = 0;
        m_out.println("laws of types: "_s, size, " of "_s, m_numberOfTypes, " types"_s);
        for (size_t law = 0; law < std::size(m_broken); ++law) {
            if (m_broken[law])
                m_out.println("sum "_s, nameOf(static_cast<LawOfTypes>(law)), ": "_s, m_broken[law], " times in "_s, m_shapes[law], " shapes"_s);
            else
                ++numberThatHold;
            shapes += m_shapes[law];
        }
        m_out.println(numberThatHold, " of "_s, std::size(m_broken), " laws hold"_s);
        return shapes;
    }

private:
    static constexpr size_t numberOfLaws = static_cast<size_t>(LawOfTypes::WhatOverlapsMeetsSomewhere) + 1;

    bool liesIn(const Type& inner, const Type& outer) const { return inner.isSubsetOf(outer, m_program); }

    static uint64_t mix(uint64_t hash, uint64_t word) { return (hash ^ word) * 1099511628211ull; }

    static uint64_t shapeOf(const Type& type)
    {
        uint64_t hash = mix(14695981039346656037ull, type.primitives);
        hash = mix(hash, type.anyObject | type.anyFunction << 1 | !!type.layouts << 2 | !type.stringLiterals.isEmpty() << 3 | !type.symbolLiterals.isEmpty() << 4 | !type.functions.isEmpty() << 5 | !type.builtins.isEmpty() << 6 | !type.regExpLiteralSet.isEmpty() << 7 | type.integers.isSubsetOf(Range::int32()) << 8);
        for (const ClassPart& part : type.classes) {
            hash = mix(hash, part.builtinClass.index());
            hash = mix(hash, part.contentsAreProven | static_cast<uint64_t>(part.arrayStorage) << 1 | static_cast<uint64_t>(part.provenPristine.toRaw()) << 9 | static_cast<uint64_t>(part.writtenContents.size()) << 32);
        }
        return hash;
    }

    void expect(LawOfTypes law, bool holds, const Type& first, const Type& second, const Type* third, const Type& result)
    {
        if (holds)
            return;
        size_t index = static_cast<size_t>(law);
        ++m_broken[index];
        uint64_t shape = mix(mix(mix(mix(index, shapeOf(first)), shapeOf(second)), third ? shapeOf(*third) : 0), shapeOf(result));
        if (!m_seen.add(shape).isNewEntry)
            return;
        ++m_shapes[index];
        m_out.print("broken "_s, nameOf(law), " | a: "_s);
        printType(m_out, m_program, first);
        m_out.print(" | b: "_s);
        printType(m_out, m_program, second);
        if (third) {
            m_out.print(" | c: "_s);
            printType(m_out, m_program, *third);
        }
        m_out.print(" | gives: "_s);
        printType(m_out, m_program, result);
        m_out.println();
    }

    PrintStream& m_out;
    const Program& m_program;
    TypeArena m_types;
    Vector<Type> m_corpus;
    unsigned m_numberOfTypes { 0 };
    uint64_t m_broken[numberOfLaws] { };
    unsigned m_shapes[numberOfLaws] { };
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_seen;
};

unsigned listBrokenLawsOfTypes(PrintStream& out, Program& program, unsigned mostTypes)
{
    return LawsOfTypes(out, program, mostTypes).run();
}

static void runLawsOfTypes(TestCommandContext& context)
{
    listBrokenLawsOfTypes(context.out, *context.program, std::numeric_limits<unsigned>::max());
}

std::span<const TestCommand> irTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "round-trip"_s, IRForm::AsRead, runRoundTrip },
        { "print"_s, IRForm::AsRead, runPrint },
        { "verify"_s, IRForm::AsRead, runVerify },
        { "result-types"_s, IRForm::AsRead, runResultTypes },
        { "laws-of-result-rules"_s, IRForm::AsRead, runLawsOfResultRules },
        { "laws-of-types"_s, IRForm::AsRead, runLawsOfTypes },
        { "int32-bounds"_s, IRForm::AsRead, runInt32Bounds },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
