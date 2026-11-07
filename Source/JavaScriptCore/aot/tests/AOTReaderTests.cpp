/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTIRProgram.h"
#include "AOTNativeTest.h"
#include "AOTObjects.h"
#include "AOTProgramVerifier.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "AOTWireTypesInternal.h"
#include <memory>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ASCIILiteral readerTestProgramText = R"IR(module m0 path="/a.ts" displayPath="a.ts"
function f0 name="same" module=m0 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Return v0 @1
}
entry module=m0
)IR"_s;

static TypeParts partsOfTypeInternedWithOthers(uint32_t index, std::span<const TypeID> earlier, TypeArena& arena)
{
    uint32_t number = index % 5 == 2 ? index / 3 : index % 11 == 4 ? index - 1 : index;
    TypeParts parts;
    switch (number % 6) {
    case 0:
        parts.primitives = Primitive::Integer;
        parts.integers = Range::one(number % 50);
        break;
    case 1:
        parts.primitives = Primitive::Integer;
        parts.integers = { 0, 100000 + static_cast<int64_t>(number) };
        break;
    case 2:
        parts.primitives = Primitive::Undefined;
        parts.unnamedLayouts = LayoutSets::of(LayoutID(number));
        break;
    case 3: {
        ClassPart part;
        part.builtinClass = BuiltinClassID(0u);
        if (!earlier.empty())
            part.writtenContents[0] = earlier[number % earlier.size()];
        parts.unnamedClasses = arena.internClassParts(singleElementSpan(part));
        break;
    }
    case 4:
        parts.objects = ObjectSets::of(ObjectID(number % arena.objects().size()));
        break;
    case 5:
        parts.primitives = Primitive::All;
        parts.integers = Range::full();
        parts.anyObject = true;
        break;
    }
    return parts;
}

static bool areSameTypesButForIDsOfClassParts(const TypeTable& firstTable, TypeID first, const TypeTable& secondTable, TypeID second)
{
    Type a = firstTable.at(first);
    Type b = secondTable.at(second);
    auto haveSameClasses = [&](ClassPartsID classesOfFirst, ClassPartsID classesOfSecond) {
        return std::ranges::equal(firstTable.at(classesOfFirst), secondTable.at(classesOfSecond));
    };
    return a.primitives == b.primitives && a.integers == b.integers && a.anyObject == b.anyObject && a.anyFunction == b.anyFunction && a.objects == b.objects
        && a.traitsOfNamed == b.traitsOfNamed && a.unnamedLayouts == b.unnamedLayouts && a.layouts == b.layouts
        && haveSameClasses(a.unnamedClasses, b.unnamedClasses) && haveSameClasses(a.classes, b.classes);
}

static void typesInternedTogetherAreNumberedAsByOneThread()
{
    constexpr uint32_t numberAtATime = 30000;
    auto makeTable = [] {
        auto table = makeUniqueWithoutFastMallocCheck<TypeTable>(BuiltinTable::singleton());
        table->setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
        return table;
    };

    auto expectedTable = makeTable();
    Vector<TypeID> expected(2 * numberAtATime);
    {
        TypeArena arena(*expectedTable, ArenaKeeps::InProgram);
        for (uint32_t index = 0; index < numberAtATime; ++index)
            expected[index] = arena.intern(partsOfTypeInternedWithOthers(index, { }, arena));
        for (uint32_t index = numberAtATime; index < 2 * numberAtATime; ++index)
            expected[index] = arena.intern(partsOfTypeInternedWithOthers(index - numberAtATime / 2, expected.span().first(numberAtATime), arena));
    }
    AOT_TEST_CHECK(expectedTable->numberOfTypes() > numberAtATime / 4);

    for (unsigned width : { 1u, 3u, 8u }) {
        ParallelLoops loops(width);
        auto table = makeTable();
        Vector<TypeID> ids(2 * numberAtATime);
        {
            ArenasOfWorkers arenas(*table, loops);
            arenas.internInOrder(ids.mutableSpan().first(numberAtATime), [&](uint32_t index, TypeArena& arena) {
                return partsOfTypeInternedWithOthers(index, { }, arena);
            });
            arenas.internInOrder(ids.mutableSpan().subspan(numberAtATime), [&](uint32_t index, TypeArena& arena) {
                return partsOfTypeInternedWithOthers(index + numberAtATime - numberAtATime / 2, ids.span().first(numberAtATime), arena);
            });
        }
        AOT_TEST_CHECK(ids == expected);
        AOT_TEST_CHECK(table->numberOfTypes() == expectedTable->numberOfTypes());
        for (uint32_t index = 0; index < ids.size(); ++index)
            AOT_TEST_CHECK(areSameTypesButForIDsOfClassParts(*table, ids[index], *expectedTable, expected[index]));

        TypeArena arena(*table, ArenaKeeps::InProgram);
        for (uint32_t index = 0; index < numberAtATime; ++index)
            AOT_TEST_CHECK(arena.intern(partsOfTypeInternedWithOthers(index, { }, arena)) == ids[index]);
        AOT_TEST_CHECK(table->numberOfTypes() == expectedTable->numberOfTypes());
    }
}

static void readerFollowsLongRowsOfFunctionTypes()
{
    constexpr uint32_t firstFunctionType = 2;
    constexpr uint32_t numberOfFunctionTypes = 40;
    constexpr uint32_t whereSecondFunctionEnters = 31;
    constexpr uint32_t endOfFunctionTypes = firstFunctionType + numberOfFunctionTypes;
    StringBuilder text;
    text.append("module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"_s);
    for (uint32_t function : { 0u, 1u }) {
        text.append("function f"_s, function, " name=\"same\" module=m0 parameters=[{type=anything}] results=[{type=anything}] functionTypes=[f"_s, function ? whereSecondFunctionEnters : firstFunctionType);
        text.append("] {\n  b0(v0: anything):\n    Return v0 @1\n}\n"_s);
    }
    for (uint32_t function = firstFunctionType; function < endOfFunctionTypes; ++function) {
        text.append("function f"_s, function, " module=m0 role=Signature thisType={type=anything} results=[{type=anything}]"_s);
        if (function + 2 < endOfFunctionTypes)
            text.append(" functionTypes=[f"_s, function + 1, ", f"_s, function + 2, ']');
        else
            text.append(" functionTypes=[f"_s, function + 1 < endOfFunctionTypes ? function + 1 : function - 1, ']');
        text.append('\n');
    }
    text.append("function f"_s, endOfFunctionTypes, " name=\"types\" role=Signature parameters=["_s);
    for (uint32_t function = firstFunctionType; function < endOfFunctionTypes; ++function)
        text.append(function == firstFunctionType ? "{type=f"_s : ", {type=f"_s, function, '}');
    text.append("] results=[{type=undefined}]\nentry module=m0\n"_s);
    String source = text.toString();

    for (unsigned width : { 1u, 3u }) {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.threadCount = width;
        BuildContext build(options);
        std::unique_ptr<Program> program = parseProgram(source, build);
        AOT_TEST_CHECK(program && program->functions.size() > endOfFunctionTypes);
        const Function& types = program->functions[endOfFunctionTypes];
        AOT_TEST_CHECK(types.parameters.size() == numberOfFunctionTypes);
        for (uint32_t index = 0; index < numberOfFunctionTypes; ++index) {
            Type type = program->at(types.parameters[index].written());
            AOT_TEST_CHECK(namesFunction(*program, type, FunctionID(0u)));
            AOT_TEST_CHECK(namesFunction(*program, type, FunctionID(1u)) == (firstFunctionType + index >= whereSecondFunctionEnters));
        }
    }
}

static void verifierFindsUnreachedNamesAmongManyOnAnyNumberOfThreads()
{
    constexpr uint32_t numberOfModules = 1500;
    auto namesModuleInFront = [](uint32_t module) {
        return module % 100 == 50;
    };
    StringBuilder text;
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        text.append("module m"_s, module, " path=\"m"_s, module, "\" displayPath=\"m"_s, module, '"');
        if (module + 1 < numberOfModules)
            text.append(" requests=[{target=m"_s, module + 1, " completes=true}]"_s);
        if (module + 2 < numberOfModules) {
            text.append(" requiredNames=[{module=m"_s, module + 2, " name=\"a\"}"_s);
            if (namesModuleInFront(module))
                text.append(", {module=m"_s, module - 1, " name=\"a\"}"_s);
            text.append(']');
        }
        text.append('\n');
    }
    text.append("entry module=m0\n"_s);
    String source = text.toString();

    for (unsigned width : { 1u, 3u, 8u }) {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.threadCount = width;
        BuildContext build(options);
        std::unique_ptr<Program> program = parseProgram(source, build);
        AOT_TEST_CHECK(program && program->modules.size() == numberOfModules);
        AOT_TEST_CHECK(!verifyRecords(*program, build));
        build.diagnostics().finish(program.get());
        Vector<uint32_t> refused;
        for (const InternalError& error : build.diagnostics().internalErrors()) {
            AOT_TEST_CHECK(error.kind == InternalErrorKind::InvalidModule);
            refused.append(error.count);
        }
        std::ranges::sort(refused);
        Vector<uint32_t> expected;
        for (uint32_t module = 0; module + 2 < numberOfModules; ++module) {
            if (namesModuleInFront(module))
                expected.append(module);
        }
        AOT_TEST_CHECK(refused == expected);
    }
}

std::span<const NativeTest> readerTests()
{
    static constexpr NativeTest tests[] = {
        { "reader/verifier-finds-unreached-names-among-many-on-any-number-of-threads"_s, verifierFindsUnreachedNamesAmongManyOnAnyNumberOfThreads },
        { "reader/long-rows-of-function-types-are-followed"_s, readerFollowsLongRowsOfFunctionTypes },
        { "reader/types-interned-together-are-numbered-as-by-one-thread"_s, typesInternedTogetherAreNumberedAsByOneThread },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
