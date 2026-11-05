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

// Enough pairs for several shares, in lists of pairs of every length: none, a few, more than a share holds.
static void readerListsSecondsAlikeOnAnyNumberOfThreads()
{
    for (uint32_t numberOfLists : { 1u, 7u, 256u, 257u, 70000u }) {
        Vector<Vector<uint64_t>> pairs;
        Vector<Vector<uint32_t>> expected(numberOfLists);
        uint32_t second = 0;
        for (size_t length : { size_t { 0 }, size_t { 3 }, size_t { 100000 }, size_t { 0 }, size_t { 1 }, size_t { 32768 }, size_t { 40000 }, size_t { 5 } }) {
            Vector<uint64_t> list;
            for (size_t index = 0; index < length; ++index, ++second) {
                uint32_t first = static_cast<uint32_t>((static_cast<uint64_t>(second) * 2654435761u >> 7) % numberOfLists);
                list.append(pairOfWireIndexes(first, second));
                expected[first].append(second);
            }
            pairs.append(WTF::move(list));
        }
        for (unsigned width : { 1u, 3u, 8u }) {
            ParallelLoops loops(width);
            Vector<Vector<uint32_t>> lists(numberOfLists);
            listSecondsByFirst(loops, pairs.span(), lists);
            AOT_TEST_CHECK(lists == expected);
            destroyLists(loops, lists);
            AOT_TEST_CHECK(lists.isEmpty());
        }
    }
}

static bool areSameObjectRecords(const ObjectRecord& first, const ObjectRecord& second)
{
    return first.kind == second.kind && first.origin == second.origin && first.traits == second.traits && first.payload == second.payload
        && first.layout == second.layout && first.builtinClass == second.builtinClass && first.ordinal == second.ordinal && first.owner == second.owner
        && first.module == second.module && first.sharedForm == second.sharedForm && first.instance == second.instance && first.siteTraits == second.siteTraits;
}

static void checkFirstObjectRecordsAreSame(const ObjectTable& written, const ObjectTable& expected, uint32_t count)
{
    AOT_TEST_CHECK(written.size() == expected.size() && count <= expected.size());
    for (uint32_t index = 0; index < count; ++index)
        AOT_TEST_CHECK(areSameObjectRecords(written.at(ObjectID(index)), expected.at(ObjectID(index))));
}

static void readerWritesRecordsOfFunctionsAlikeOnAnyNumberOfThreads()
{
    using Group = FunctionInOrderOfObjects::Group;
    Vector<FunctionInOrderOfObjects> functions(20000, [](size_t index) {
        return FunctionInOrderOfObjects { static_cast<Group>(index * 7 % FunctionInOrderOfObjects::numberOfGroups), !(index % 3), !(index % 5) };
    });
    ObjectTable onCallingThread(BuiltinTable::singleton(), functions.span());
    for (unsigned width : { 1u, 3u, 8u }) {
        ParallelLoops loops(width);
        ObjectTable onEveryThread(BuiltinTable::singleton(), functions.span(), &loops);
        checkFirstObjectRecordsAreSame(onEveryThread, onCallingThread, onCallingThread.size());
    }
}

// More functions and more layouts than one thread writes at a time, and more records than one thread carries over at a time: in a small program one thread does it all.
static void readerAddsObjectsOfProgramAlikeOnAnyNumberOfThreads()
{
    using Group = FunctionInOrderOfObjects::Group;
    constexpr uint32_t numberOfFunctions = 40000;
    constexpr uint32_t numberOfLayouts = 9000;

    auto program = makeUnique<Program>(BuiltinTable::singleton());
    program->functions.grow(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (!(index % 3))
            program->functions[index].flags.add(FunctionFlags::IsAsync);
        if (!(index % 5))
            program->functions[index].flags.add(FunctionFlags::IsGenerator);
    }
    program->layouts.grow(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        Layout& layout = program->layouts[index];
        layout.kind = index % 4 == 2 ? LayoutKind::Record : index % 4 == 3 ? LayoutKind::Environment : LayoutKind::Instance;
        if (layout.kind == LayoutKind::Instance)
            layout.constructor = FunctionID(index * 4);
        if (index && !(index % 7))
            layout.base = LayoutID(index - 1);
        if (!(index % 6))
            layout.flags.add(LayoutFlags::IsConstructorFunction);
        if (!(index % 11))
            layout.flags.add(LayoutFlags::IsNeverAllocated);
        if (!(index % 13))
            layout.flags.add(LayoutFlags::IsTypeParameter);
        if (index && !(index % 17)) {
            layout.base = LayoutID(index - 1);
            layout.flags.add(LayoutFlags::IsDerived);
        }
    }

    Vector<FunctionInOrderOfObjects> functions(numberOfFunctions, [&](size_t index) {
        return FunctionInOrderOfObjects { static_cast<Group>(index * 7 % FunctionInOrderOfObjects::numberOfGroups), program->functions[index].isCoroutine(), !(index % 4) };
    });
    Vector<uint32_t> numberOfSites(numberOfFunctions, [](size_t index) {
        return static_cast<uint32_t>(index % 3);
    });
    BitVector noModules;

    std::unique_ptr<ObjectTable> expected;
    for (unsigned width : { 1u, 3u, 8u }) {
        ParallelLoops loops(width);
        auto table = makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), functions.span(), &loops);
        table->addObjectsOfProgram(*program, noModules, loops);
        uint32_t numberOfRecordsWritten = table->size();
        // Whoever has scanned a function writes its sites. Nobody has here.
        table->reserveSites(numberOfSites.span(), loops);
        AOT_TEST_CHECK(table->size() > numberOfRecordsWritten);
        if (expected)
            checkFirstObjectRecordsAreSame(*table, *expected, numberOfRecordsWritten);
        else
            expected = WTF::move(table);
    }
}

static constexpr ASCIILiteral readerTestProgramText = R"IR(module m0 path="/a.ts" displayPath="a.ts"
function f0 name="same" module=m0 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Return v0 @1
}
entry module=m0
)IR"_s;

// No set here has more than one member, so the id of each says what it holds. Of every kind that internInOrder() tells apart: one that needs no record, one that the table has from its start, one that is new, one that repeats a near or a far one.
// `earlier`: types that the contents of a class may name.
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

// Enough for many tasks, twice: the second time the table holds records, and the contents of classes name types of the first time.
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

        // Whoever asks for one of them later finds it.
        TypeArena arena(*table, ArenaKeeps::InProgram);
        for (uint32_t index = 0; index < numberAtATime; ++index)
            AOT_TEST_CHECK(arena.intern(partsOfTypeInternedWithOthers(index, { }, arena)) == ids[index]);
        AOT_TEST_CHECK(table->numberOfTypes() == expectedTable->numberOfTypes());
    }
}

static void keysInternedTogetherAreNumberedAsByOneThread()
{
    constexpr uint32_t numberOfKeys = 20000;
    auto keyAt = [](uint32_t index) {
        uint32_t number = index % 3 == 1 ? index / 2 : index;
        if (number % 4 == 3)
            return PropertyKey { .kind = PropertyKeyKind::RegisteredSymbol, .text = StringID(number) };
        return PropertyKey { .kind = PropertyKeyKind::Text, .text = StringID(number) };
    };

    TypeTable expectedTable(BuiltinTable::singleton());
    Vector<PropertyKeyID> expected(numberOfKeys);
    {
        TypeArena arena(expectedTable, ArenaKeeps::InProgram);
        for (uint32_t index = 0; index < numberOfKeys; ++index)
            expected[index] = arena.intern(keyAt(index));
    }
    AOT_TEST_CHECK(expectedTable.numberOfPropertyKeys() < numberOfKeys);

    for (unsigned width : { 1u, 3u, 8u }) {
        ParallelLoops loops(width);
        TypeTable table(BuiltinTable::singleton());
        Vector<PropertyKeyID> ids(numberOfKeys);
        ArenasOfWorkers arenas(table, loops);
        // The first few by themselves, so that the rest meets keys in the table.
        constexpr uint32_t numberOfFirstKeys = 100;
        arenas.internInOrder(ids.mutableSpan().first(numberOfFirstKeys), keyAt);
        arenas.internInOrder(ids.mutableSpan().subspan(numberOfFirstKeys), [&](uint32_t index) {
            return keyAt(numberOfFirstKeys + index);
        });
        AOT_TEST_CHECK(ids == expected);
        AOT_TEST_CHECK(table.numberOfPropertyKeys() == expectedTable.numberOfPropertyKeys());
        for (uint32_t index = 0; index < numberOfKeys; ++index) {
            AOT_TEST_CHECK(table.at(ids[index]) == keyAt(index));
            AOT_TEST_CHECK(table.find(keyAt(index)) == ids[index]);
        }
    }
}

// First thousands of types that name no type, then as many that name those, then types that each name the one in front of them: one long stretch, a second one, and stretches of one.
static void readerNumbersTypesAlikeOnAnyNumberOfThreads()
{
    constexpr uint32_t numberOfRanges = 6000;
    constexpr uint32_t typesPerFunction = 100;
    StringBuilder text;
    text.append("module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"_s);
    uint32_t numberOfFunctions = 0;
    auto appendFunction = [&](NOESCAPE const Invocable<void(uint32_t)> auto& appendType) {
        text.append("function f"_s, numberOfFunctions, " name=\"types\" role=Signature parameters=["_s);
        for (uint32_t index = 0; index < typesPerFunction; ++index) {
            text.append(index ? ", {type="_s : "{type="_s);
            appendType(index);
            text.append('}');
        }
        text.append("] results=[{type=undefined}]\n"_s);
        ++numberOfFunctions;
    };
    for (uint32_t first = 0; first < numberOfRanges; first += typesPerFunction) {
        appendFunction([&](uint32_t index) {
            text.append("int[0, "_s, 100000 + first + index, ']');
        });
    }
    for (uint32_t first = 0; first < numberOfRanges; first += typesPerFunction) {
        appendFunction([&](uint32_t index) {
            text.append(index % 2 ? "undefined|null|"_s : ""_s, "Array<int[0, "_s, 100000 + first + index / 2, "]>"_s);
        });
    }
    // The text of a type nests 32 deep at most.
    constexpr uint32_t deepestArray = 24;
    appendFunction([&](uint32_t index) {
        for (uint32_t depth = 0; depth <= index % deepestArray; ++depth)
            text.append("Array<"_s);
        text.append("string"_s);
        for (uint32_t depth = 0; depth <= index % deepestArray; ++depth)
            text.append('>');
    });
    text.append("function f"_s, numberOfFunctions, " name=\"same\" module=m0 parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Return v0 @1\n}\nentry module=m0\n"_s);
    String source = text.toString();

    Vector<TypeID> expected;
    uint32_t expectedNumberOfTypes = 0;
    for (unsigned width : { 1u, 3u, 8u }) {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.threadCount = width;
        BuildContext build(options);
        std::unique_ptr<Program> program = parseProgram(source, build);
        AOT_TEST_CHECK(program);
        std::span<const TypeID> types = program->types.typesOfWireIndexes();
        AOT_TEST_CHECK(types.size() > 2 * numberOfRanges + deepestArray);
        if (expected.isEmpty()) {
            expected.append(types);
            expectedNumberOfTypes = program->types.numberOfTypes();
            continue;
        }
        AOT_TEST_CHECK(std::ranges::equal(types, expected));
        AOT_TEST_CHECK(program->types.numberOfTypes() == expectedNumberOfTypes);
    }
}

// A row of function types, each of which converts to the next two, longer than the list that a walk up such a row begins with, and the last converts back to the one in front of it.
// One function enters the row at its start and one far up.
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
        // So every walk comes by what it has visited.
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

// A row of modules, each of which asks for the next and names the one behind that: more names than one task of the verifier looks at. One module in a hundred also names the one in front of it, to which nothing leads.
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
        { "reader/keys-interned-together-are-numbered-as-by-one-thread"_s, keysInternedTogetherAreNumberedAsByOneThread },
        { "reader/types-are-numbered-the-same-on-any-number-of-threads"_s, readerNumbersTypesAlikeOnAnyNumberOfThreads },
        { "reader/lists-of-seconds-are-the-same-on-any-number-of-threads"_s, readerListsSecondsAlikeOnAnyNumberOfThreads },
        { "reader/records-of-functions-are-the-same-on-any-number-of-threads"_s, readerWritesRecordsOfFunctionsAlikeOnAnyNumberOfThreads },
        { "reader/objects-of-the-program-are-the-same-on-any-number-of-threads"_s, readerAddsObjectsOfProgramAlikeOnAnyNumberOfThreads },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
